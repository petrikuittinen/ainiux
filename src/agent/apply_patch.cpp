#include "agent/apply_patch.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "agent/text_match.hpp"

namespace ainiux::agent {
namespace {

std::string trim_right(std::string text) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.pop_back();
    return text;
}

std::string trim_left(std::string text) {
    std::size_t i = 0;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
    return text.substr(i);
}

std::string strip_cr(std::string line) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

std::vector<std::string> split_patch_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            lines.push_back(strip_cr(text.substr(start, i - start)));
            start = i + 1;
        }
    }
    if (start < text.size() || text.empty()) lines.push_back(strip_cr(text.substr(start)));
    // Drop a single trailing empty line caused by a final newline after End Patch.
    if (lines.size() > 1 && lines.back().empty()) lines.pop_back();
    return lines;
}

bool starts_with(const std::string& text, const char* prefix) {
    const std::size_t n = std::char_traits<char>::length(prefix);
    return text.size() >= n && text.compare(0, n, prefix) == 0;
}

std::string path_after_prefix(const std::string& line, const char* prefix) {
    std::string rest = trim_left(line.substr(std::char_traits<char>::length(prefix)));
    return trim_right(rest);
}

bool is_file_header(const std::string& line) {
    return starts_with(line, "*** Add File:") || starts_with(line, "*** Update File:") ||
           starts_with(line, "*** Delete File:") || starts_with(line, "*** End Patch");
}

Error parse_add_body(const std::vector<std::string>& lines,
                     std::size_t& index,
                     std::string& content) {
    std::ostringstream out;
    bool any = false;
    while (index < lines.size() && !is_file_header(lines[index]) &&
           !starts_with(lines[index], "*** Begin Patch")) {
        const std::string& line = lines[index];
        if (line.empty()) {
            // Blank line inside an add body is treated as an empty content line.
            if (any) out << '\n';
            any = true;
            ++index;
            continue;
        }
        if (line[0] == '+') {
            if (any) out << '\n';
            out << line.substr(1);
            any = true;
            ++index;
            continue;
        }
        // Some models omit the leading '+' on Add File bodies; accept non-header lines.
        if (line[0] == '-' || line[0] == '@')
            return {ErrorCode::BadArgs,
                    "apply_patch Add File body must use +lines (found '" + line.substr(0, 1) +
                        "' prefix)"};
        if (any) out << '\n';
        out << line;
        any = true;
        ++index;
    }
    if (any) out << '\n';  // new files usually end with a newline
    content = out.str();
    return ok_error();
}

Error parse_update_hunks(const std::vector<std::string>& lines,
                         std::size_t& index,
                         std::vector<PatchHunk>& hunks) {
    hunks.clear();
    PatchHunk current;
    bool in_hunk = false;
    auto flush = [&] {
        if (in_hunk) {
            if (current.lines.empty() && current.header.empty()) {
                // empty hunk — ignore
            } else {
                hunks.push_back(current);
            }
            current = PatchHunk{};
            in_hunk = false;
        }
    };

    while (index < lines.size() && !is_file_header(lines[index]) &&
           !starts_with(lines[index], "*** Begin Patch")) {
        const std::string& line = lines[index];
        if (starts_with(line, "@@")) {
            flush();
            in_hunk = true;
            current.header = trim_left(line.substr(2));
            ++index;
            continue;
        }
        if (line.empty()) {
            // Empty line inside a hunk is context with empty content — rare; treat as " ".
            if (!in_hunk) {
                in_hunk = true;
            }
            current.lines.push_back(" ");
            ++index;
            continue;
        }
        const char prefix = line[0];
        if (prefix == ' ' || prefix == '-' || prefix == '+') {
            if (!in_hunk) in_hunk = true;
            current.lines.push_back(line);
            ++index;
            continue;
        }
        // Bare context line without leading space (weak models).
        if (!starts_with(line, "***")) {
            if (!in_hunk) in_hunk = true;
            current.lines.push_back(std::string(" ") + line);
            ++index;
            continue;
        }
        return {ErrorCode::BadArgs, "unexpected line in Update File section: " + line};
    }
    flush();
    if (hunks.empty())
        return {ErrorCode::BadArgs, "Update File section has no hunks"};
    return ok_error();
}

}  // namespace

Error hunk_old_new_text(const PatchHunk& hunk, std::string& old_text, std::string& new_text) {
    old_text.clear();
    new_text.clear();
    bool old_any = false;
    bool new_any = false;
    for (const std::string& line : hunk.lines) {
        if (line.empty()) continue;
        const char prefix = line[0];
        const std::string body = line.substr(1);
        if (prefix == ' ' || prefix == '-') {
            if (old_any) old_text.push_back('\n');
            old_text += body;
            old_any = true;
        }
        if (prefix == ' ' || prefix == '+') {
            if (new_any) new_text.push_back('\n');
            new_text += body;
            new_any = true;
        }
        if (prefix != ' ' && prefix != '-' && prefix != '+')
            return {ErrorCode::BadArgs, "invalid hunk line prefix"};
    }
    // Hunks that only add lines may have empty old_text (pure insertion at EOF or
    // after unique context). Pure deletion may have empty new_text.
    return ok_error();
}

// Strip optional Markdown fences weak models wrap around the patch body.
std::string strip_optional_fence(std::string text) {
    // Trim overall whitespace first.
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\n' ||
                             text.front() == '\r'))
        text.erase(text.begin());
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' ||
                             text.back() == '\r'))
        text.pop_back();
    if (text.size() >= 3 && text[0] == '`' && text[1] == '`' && text[2] == '`') {
        // Drop opening fence line.
        const std::size_t nl = text.find('\n');
        if (nl == std::string::npos) return text;
        text = text.substr(nl + 1);
        // Drop closing ``` if present at end.
        std::size_t end = text.rfind("```");
        if (end != std::string::npos) {
            // Only strip if it's a trailing fence (optional final newline after).
            std::string after = text.substr(end + 3);
            bool only_ws = true;
            for (char ch : after) {
                if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
                    only_ws = false;
                    break;
                }
            }
            if (only_ws) text = text.substr(0, end);
        }
    }
    return text;
}

bool is_file_op_header(const std::string& line) {
    return starts_with(line, "*** Add File:") || starts_with(line, "*** Update File:") ||
           starts_with(line, "*** Delete File:");
}

Error parse_apply_patch(const std::string& patch_text, ParsedPatch& out) {
    out = ParsedPatch{};
    if (patch_text.find('\0') != std::string::npos)
        return {ErrorCode::BadArgs, "apply_patch text must not contain NUL bytes"};

    std::vector<std::string> lines = split_patch_lines(strip_optional_fence(patch_text));
    // Trim leading empty lines.
    while (!lines.empty() && trim_right(lines.front()).empty()) lines.erase(lines.begin());
    if (lines.empty()) return {ErrorCode::BadArgs, "apply_patch text is empty"};

    std::size_t index = 0;
    // Local models (e.g. Qwen) often omit the Begin/End envelope and start directly
    // at *** Update/Add/Delete File. Accept that form; require End only when Begin
    // was present (otherwise EOF ends the patch).
    bool require_end = false;
    if (starts_with(lines[index], "*** Begin Patch")) {
        require_end = true;
        ++index;
    } else if (!is_file_op_header(lines[index])) {
        return {ErrorCode::BadArgs,
                "apply_patch must start with '*** Begin Patch' or a file operation "
                "('*** Update File:' / '*** Add File:' / '*** Delete File:'); got: " +
                    lines[index].substr(0, std::min<std::size_t>(60, lines[index].size()))};
    }

    bool saw_end = false;
    while (index < lines.size()) {
        const std::string& line = lines[index];
        if (starts_with(line, "*** End Patch")) {
            saw_end = true;
            ++index;
            break;
        }
        // Ignore a redundant Begin if the model duplicated it.
        if (starts_with(line, "*** Begin Patch")) {
            ++index;
            require_end = true;
            continue;
        }
        if (starts_with(line, "*** Add File:")) {
            PatchFileOp op;
            op.kind = PatchOpKind::AddFile;
            op.path = path_after_prefix(line, "*** Add File:");
            if (op.path.empty()) return {ErrorCode::BadArgs, "Add File requires a path"};
            ++index;
            Error error = parse_add_body(lines, index, op.add_content);
            if (!error.ok()) return error;
            out.ops.push_back(std::move(op));
            continue;
        }
        if (starts_with(line, "*** Update File:")) {
            PatchFileOp op;
            op.kind = PatchOpKind::UpdateFile;
            op.path = path_after_prefix(line, "*** Update File:");
            if (op.path.empty()) return {ErrorCode::BadArgs, "Update File requires a path"};
            ++index;
            // Optional: *** Move to: newpath — not implemented yet; refuse clearly.
            if (index < lines.size() && starts_with(lines[index], "*** Move to:"))
                return {ErrorCode::UnsupportedFeature,
                        "apply_patch Move to is not implemented yet; update in place or use "
                        "write_file/remove"};
            Error error = parse_update_hunks(lines, index, op.hunks);
            if (!error.ok()) return error;
            out.ops.push_back(std::move(op));
            continue;
        }
        if (starts_with(line, "*** Delete File:")) {
            PatchFileOp op;
            op.kind = PatchOpKind::DeleteFile;
            op.path = path_after_prefix(line, "*** Delete File:");
            if (op.path.empty()) return {ErrorCode::BadArgs, "Delete File requires a path"};
            ++index;
            out.ops.push_back(std::move(op));
            continue;
        }
        if (trim_right(line).empty()) {
            ++index;
            continue;
        }
        return {ErrorCode::BadArgs, "unexpected patch line: " + line};
    }

    if (require_end && !saw_end)
        return {ErrorCode::BadArgs,
                "apply_patch has '*** Begin Patch' but is missing '*** End Patch'"};
    // Allow trailing blank lines only (after End, or after last op when envelope omitted).
    while (index < lines.size()) {
        if (!trim_right(lines[index]).empty() && !starts_with(lines[index], "*** End Patch"))
            return {ErrorCode::BadArgs, "unexpected content after patch body: " + lines[index]};
        ++index;
    }
    if (out.ops.empty()) return {ErrorCode::BadArgs, "apply_patch contains no file operations"};
    return ok_error();
}

namespace {

// Byte offset of the first byte of 1-based line `line` (clamped to size).
std::size_t offset_of_line(const std::string& text, std::size_t line) {
    if (line <= 1) return 0;
    std::size_t current = 1;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            ++current;
            if (current == line) return i + 1;
        }
    }
    return text.size();
}

// Parse unified-diff style anchors from a hunk header: "-120,5 +120,8" or "-120".
// Returns true when an old-file start line was found.
bool parse_hunk_line_anchor(const std::string& header,
                            std::size_t& old_start,
                            std::size_t& old_count) {
    old_start = 0;
    old_count = 0;
    // Skip leading spaces; accept optional function label after the ranges.
    std::size_t i = 0;
    while (i < header.size() && (header[i] == ' ' || header[i] == '\t')) ++i;
    if (i >= header.size() || header[i] != '-') return false;
    ++i;
    if (i >= header.size() || header[i] < '0' || header[i] > '9') return false;
    std::size_t start = 0;
    while (i < header.size() && header[i] >= '0' && header[i] <= '9') {
        start = start * 10U + static_cast<std::size_t>(header[i] - '0');
        ++i;
    }
    std::size_t count = 1;
    if (i < header.size() && header[i] == ',') {
        ++i;
        count = 0;
        if (i >= header.size() || header[i] < '0' || header[i] > '9') return false;
        while (i < header.size() && header[i] >= '0' && header[i] <= '9') {
            count = count * 10U + static_cast<std::size_t>(header[i] - '0');
            ++i;
        }
    }
    if (start == 0) return false;
    old_start = start;
    old_count = count == 0 ? 1 : count;
    return true;
}

std::string preview_snippet(const std::string& text, std::size_t max_bytes) {
    std::string preview = text;
    if (preview.size() > max_bytes) {
        preview.resize(max_bytes);
        preview += "…";
    }
    for (char& ch : preview) {
        if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
    }
    return preview;
}

std::string format_match_lines(const std::vector<TextSpan>& matches, std::size_t limit) {
    std::ostringstream out;
    const std::size_t n = std::min(limit, matches.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (i) out << ',';
        out << matches[i].start_line;
    }
    if (matches.size() > limit) out << ",…";
    return out.str();
}

// Collect matches for old_text, trying with and without a trailing newline.
// Returns the needle that was used (possibly with trailing \n) via matched_needle.
TextMatchResult find_hunk_matches(const std::string& haystack,
                                  const std::string& old_text,
                                  bool allow_fuzzy,
                                  std::size_t region_start,
                                  std::size_t region_end,
                                  std::string& matched_needle) {
    matched_needle = old_text;
    TextMatchResult found =
        find_text_matches(haystack, old_text, allow_fuzzy, region_start, region_end);
    if (!found.matches.empty()) return found;
    if (!old_text.empty() && old_text.back() != '\n') {
        matched_needle = old_text + "\n";
        found = find_text_matches(haystack, matched_needle, allow_fuzzy, region_start, region_end);
    }
    return found;
}

std::vector<TextSpan> filter_matches_from(const std::vector<TextSpan>& matches,
                                          std::size_t min_offset) {
    std::vector<TextSpan> out;
    for (const TextSpan& span : matches) {
        if (span.offset >= min_offset) out.push_back(span);
    }
    return out;
}

Error ambiguous_hunk_error(std::size_t hunk_index,
                           const std::string& old_text,
                           const std::vector<TextSpan>& matches) {
    return {ErrorCode::FileWrite,
            "apply_patch hunk " + std::to_string(hunk_index) +
                " context matches " + std::to_string(matches.size()) +
                " places (lines " + format_match_lines(matches, 10) +
                "); old_text=\"" + preview_snippet(old_text, 80) +
                "\". Make the context unique, add a @@ -line,count header, "
                "or use str_replace/edit_file with line_range_hint."};
}

Error not_found_hunk_error(std::size_t hunk_index, const std::string& old_text) {
    return {ErrorCode::FileWrite,
            "apply_patch hunk " + std::to_string(hunk_index) +
                " context not found in file (old text did not match); old_text=\"" +
                preview_snippet(old_text, 80) +
                "\". Re-read the file and refresh the hunk context."};
}

// Count how many remaining hunks (from index inclusive) share the same old_text.
// Used to apply identical multi-site one-line fixes left-to-right.
std::size_t count_same_old_text_hunks(const std::vector<PatchHunk>& hunks,
                                      std::size_t from_index,
                                      const std::string& old_text) {
    std::size_t count = 0;
    for (std::size_t i = from_index; i < hunks.size(); ++i) {
        std::string other_old;
        std::string other_new;
        if (!hunk_old_new_text(hunks[i], other_old, other_new).ok()) break;
        if (other_old != old_text) break;
        ++count;
    }
    return count;
}

}  // namespace

Error apply_patch_hunks(const std::string& original,
                        const std::vector<PatchHunk>& hunks,
                        bool allow_fuzzy,
                        std::string& result,
                        std::vector<std::string>& match_modes) {
    result = original;
    match_modes.clear();
    if (hunks.empty()) return {ErrorCode::BadArgs, "no hunks to apply"};

    // Sequential cursor: after each successful hunk, later hunks prefer the first
    // match at or after this offset (Codex-style left-to-right application). This
    // fixes multi-site identical one-line fixes and pure insertions after a prior
    // unique edit in the same file. Bare first-hunk weak context still errors.
    std::size_t search_from = 0;

    for (std::size_t hunk_index = 0; hunk_index < hunks.size(); ++hunk_index) {
        const PatchHunk& hunk = hunks[hunk_index];
        const std::size_t display_index = hunk_index + 1;
        std::string old_text;
        std::string new_text;
        Error error = hunk_old_new_text(hunk, old_text, new_text);
        if (!error.ok()) return error;

        if (old_text.empty()) {
            // Pure insertion: append if no context. Prefer end-of-file when the hunk
            // has only '+' lines.
            bool only_inserts = true;
            for (const std::string& line : hunk.lines) {
                if (!line.empty() && line[0] != '+') {
                    only_inserts = false;
                    break;
                }
            }
            if (!only_inserts)
                return {ErrorCode::FileWrite,
                        "hunk has empty old text but is not a pure insertion"};
            if (!result.empty() && result.back() != '\n') result.push_back('\n');
            const std::size_t insert_at = result.size();
            result += new_text;
            if (!new_text.empty() && new_text.back() != '\n') result.push_back('\n');
            search_from = result.size();
            (void)insert_at;
            match_modes.push_back("eof_insert");
            continue;
        }

        std::size_t anchor_start = 0;
        std::size_t anchor_count = 0;
        const bool has_line_anchor =
            parse_hunk_line_anchor(hunk.header, anchor_start, anchor_count);

        std::string matched_needle;
        TextMatchResult chosen;
        std::string mode_suffix;
        bool selected = false;

        // 1) Unique match inside @@ line window (with slack for nearby drift).
        if (has_line_anchor) {
            const std::size_t window_line =
                anchor_start > 2 ? anchor_start - 2 : 1;
            const std::size_t window_end_line = anchor_start + anchor_count + 8;
            const std::size_t region_start = offset_of_line(result, window_line);
            const std::size_t region_end = offset_of_line(result, window_end_line + 1);
            chosen = find_hunk_matches(result, old_text, allow_fuzzy, region_start,
                                       region_end, matched_needle);
            if (chosen.matches.size() == 1) {
                selected = true;
                mode_suffix = "_line_anchor";
            } else if (chosen.matches.size() > 1) {
                // Prefer the first match still at/after the sequential cursor.
                std::vector<TextSpan> after = filter_matches_from(chosen.matches, search_from);
                if (after.size() == 1) {
                    chosen.matches = std::move(after);
                    selected = true;
                    mode_suffix = "_line_anchor_sequential";
                } else if (!after.empty()) {
                    chosen.matches.assign(1, after.front());
                    selected = true;
                    mode_suffix = "_line_anchor_first";
                } else {
                    // All window matches are before the cursor; take first in window.
                    chosen.matches.resize(1);
                    selected = true;
                    mode_suffix = "_line_anchor_first";
                }
            }
        }

        // 2) Full-file search with sequential disambiguation.
        if (!selected) {
            chosen = find_hunk_matches(result, old_text, allow_fuzzy, 0, std::string::npos,
                                       matched_needle);
            if (chosen.matches.empty()) return not_found_hunk_error(display_index, old_text);

            if (chosen.matches.size() == 1) {
                selected = true;
            } else {
                std::vector<TextSpan> after =
                    filter_matches_from(chosen.matches, search_from);
                if (after.empty()) {
                    return ambiguous_hunk_error(display_index, old_text, chosen.matches);
                }
                if (after.size() == 1) {
                    chosen.matches = std::move(after);
                    selected = true;
                    mode_suffix = "_sequential";
                } else if (search_from > 0) {
                    // Prior hunk advanced the cursor: take the next site (identical
                    // multi-site edits; pure insert after a nearby unique edit).
                    chosen.matches.assign(1, after.front());
                    selected = true;
                    mode_suffix = "_sequential_first";
                } else {
                    // First ambiguous site: only auto-pick when a run of remaining
                    // hunks share this old_text and there are enough match sites
                    // (e.g. two identical body_len=23 fixes).
                    const std::size_t same_run =
                        count_same_old_text_hunks(hunks, hunk_index, old_text);
                    if (same_run >= 2 && after.size() >= same_run) {
                        chosen.matches.assign(1, after.front());
                        selected = true;
                        mode_suffix = "_multisite_first";
                    } else {
                        return ambiguous_hunk_error(display_index, old_text, after);
                    }
                }
            }
        }

        if (!selected || chosen.matches.empty())
            return not_found_hunk_error(display_index, old_text);

        std::string replacement = new_text;
        const bool use_nl =
            matched_needle.size() == old_text.size() + 1 && !matched_needle.empty() &&
            matched_needle.back() == '\n';
        if (use_nl && !replacement.empty() && replacement.back() != '\n') replacement.push_back('\n');
        if (use_nl && replacement.empty()) replacement = "\n";  // pure delete of a line

        const TextSpan span = chosen.matches.front();
        std::size_t replacements = 0;
        std::vector<TextSpan> one = {span};
        result = apply_text_replacements(result, one, replacement, false, replacements);
        if (replacements != 1)
            return {ErrorCode::Internal, "apply_patch expected exactly one hunk replacement"};

        // Advance cursor to just after the replacement.
        search_from = span.offset + replacement.size();
        std::string mode = chosen.mode.empty() ? "exact" : chosen.mode;
        mode += mode_suffix;
        match_modes.push_back(std::move(mode));
    }
    return ok_error();
}

}  // namespace ainiux::agent
