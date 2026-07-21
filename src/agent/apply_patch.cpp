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

Error apply_patch_hunks(const std::string& original,
                        const std::vector<PatchHunk>& hunks,
                        bool allow_fuzzy,
                        std::string& result,
                        std::vector<std::string>& match_modes) {
    result = original;
    match_modes.clear();
    if (hunks.empty()) return {ErrorCode::BadArgs, "no hunks to apply"};

    for (const PatchHunk& hunk : hunks) {
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
            result += new_text;
            if (!new_text.empty() && new_text.back() != '\n') result.push_back('\n');
            match_modes.push_back("eof_insert");
            continue;
        }

        // Prefer matching old_text as a contiguous region. When the original uses
        // trailing newlines between lines, rebuild with \n between logical lines.
        // old_text from hunk_old_new_text uses \n separators without a final \n
        // unless the last body line was empty — also try with trailing \n.
        const TextMatchResult found =
            find_text_matches(result, old_text, allow_fuzzy, 0, std::string::npos);
        TextMatchResult found_nl;
        bool use_nl = false;
        if (found.matches.empty() && !old_text.empty() && old_text.back() != '\n') {
            found_nl = find_text_matches(result, old_text + "\n", allow_fuzzy, 0, std::string::npos);
            use_nl = !found_nl.matches.empty();
        }
        const TextMatchResult& chosen = use_nl ? found_nl : found;
        const std::string matched_old = use_nl ? old_text + "\n" : old_text;
        std::string replacement = new_text;
        if (use_nl && !replacement.empty() && replacement.back() != '\n') replacement.push_back('\n');
        if (use_nl && replacement.empty()) replacement = "\n";  // pure delete of a line

        if (chosen.matches.empty())
            return {ErrorCode::FileWrite,
                    "apply_patch hunk context not found in file (old text did not match)"};
        if (chosen.matches.size() > 1)
            return {ErrorCode::FileWrite,
                    "apply_patch hunk context matches " + std::to_string(chosen.matches.size()) +
                        " places; make the context unique or split the patch"};

        std::size_t replacements = 0;
        result = apply_text_replacements(result, chosen.matches, replacement, false, replacements);
        if (replacements != 1)
            return {ErrorCode::Internal, "apply_patch expected exactly one hunk replacement"};
        match_modes.push_back(chosen.mode.empty() ? "exact" : chosen.mode);
    }
    return ok_error();
}

}  // namespace ainiux::agent
