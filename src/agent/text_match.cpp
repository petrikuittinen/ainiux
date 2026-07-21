#include "agent/text_match.hpp"

#include <algorithm>
#include <cctype>
#include <limits>

namespace ainiux::agent {
namespace {

bool is_ws(unsigned char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

std::size_t count_lines_through(const std::string& text, std::size_t exclusive_end) {
    if (text.empty()) return 1;
    std::size_t line = 1;
    const std::size_t end = std::min(exclusive_end, text.size());
    for (std::size_t i = 0; i < end; ++i) {
        if (text[i] == '\n') ++line;
    }
    return line;
}

void annotate_lines(const std::string& haystack, TextSpan& span) {
    span.start_line = line_number_at(haystack, span.offset);
    if (span.length == 0) {
        span.end_line = span.start_line;
        return;
    }
    // End line is the line of the last included byte.
    span.end_line = line_number_at(haystack, span.offset + span.length - 1);
}

// Collapse each run of whitespace to a single space. Does not trim ends.
std::string normalize_whitespace(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool last_ws = false;
    for (unsigned char ch : text) {
        if (is_ws(ch)) {
            if (!last_ws) {
                out.push_back(' ');
                last_ws = true;
            }
        } else {
            out.push_back(static_cast<char>(ch));
            last_ws = false;
        }
    }
    return out;
}

// Flexible whitespace match (PLANS.md §13 normalized_whitespace): non-whitespace
// characters must match in order; whitespace runs in the needle require at least one
// whitespace character in the haystack; extra whitespace in the haystack is allowed
// between needle tokens (so "foo(a,b)" matches "foo(  a,\n  b )").
bool match_normalized_at(const std::string& haystack,
                         std::size_t pos,
                         const std::string& needle,
                         std::size_t& match_end) {
    if (needle.empty() || pos > haystack.size()) return false;
    std::size_t ni = 0;
    std::size_t hi = pos;
    while (ni < needle.size()) {
        if (is_ws(static_cast<unsigned char>(needle[ni]))) {
            while (ni < needle.size() && is_ws(static_cast<unsigned char>(needle[ni]))) ++ni;
            if (hi >= haystack.size() || !is_ws(static_cast<unsigned char>(haystack[hi])))
                return false;
            while (hi < haystack.size() && is_ws(static_cast<unsigned char>(haystack[hi]))) ++hi;
            continue;
        }
        // Optional whitespace in the haystack before the next non-ws needle char,
        // but not before the first character of the match (keeps the start anchored).
        if (hi != pos) {
            while (hi < haystack.size() && is_ws(static_cast<unsigned char>(haystack[hi]))) ++hi;
        }
        if (hi >= haystack.size() || haystack[hi] != needle[ni]) return false;
        ++hi;
        ++ni;
    }
    match_end = hi;
    return true;
}

std::vector<TextSpan> find_exact(const std::string& haystack,
                                 const std::string& needle,
                                 std::size_t region_start,
                                 std::size_t region_end) {
    std::vector<TextSpan> matches;
    if (needle.empty() || region_start >= region_end) return matches;
    std::size_t pos = region_start;
    while (pos < region_end) {
        const std::size_t found = haystack.find(needle, pos);
        if (found == std::string::npos || found >= region_end) break;
        if (found + needle.size() > region_end) break;
        TextSpan span;
        span.offset = found;
        span.length = needle.size();
        annotate_lines(haystack, span);
        matches.push_back(span);
        pos = found + (needle.empty() ? 1 : needle.size());
    }
    return matches;
}

std::vector<TextSpan> find_normalized(const std::string& haystack,
                                      const std::string& needle,
                                      std::size_t region_start,
                                      std::size_t region_end) {
    std::vector<TextSpan> matches;
    if (needle.empty() || region_start >= region_end) return matches;
    // Collapse needle whitespace runs so matching is stable regardless of how the
    // model spaced the snippet; haystack whitespace is handled flexibly in the matcher.
    const std::string norm_needle = normalize_whitespace(needle);
    if (norm_needle.empty()) return matches;

    // Only start at positions that could match the first non-ws needle character
    // (or region_start when the needle begins with whitespace).
    std::size_t pos = region_start;
    while (pos < region_end) {
        std::size_t match_end = 0;
        if (match_normalized_at(haystack, pos, norm_needle, match_end) && match_end <= region_end &&
            match_end > pos) {
            TextSpan span;
            span.offset = pos;
            span.length = match_end - pos;
            annotate_lines(haystack, span);
            matches.push_back(span);
            pos = match_end;
        } else {
            ++pos;
        }
    }
    return matches;
}

// Leading indentation: spaces and tabs only (not newlines).
std::size_t leading_indent_bytes(const std::string& line) {
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    return i;
}

// Split into lines preserving terminators on each piece (same convention as tools.cpp).
std::vector<std::string> split_preserving(const std::string& source) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    for (std::size_t i = 0; i < source.size(); ++i) {
        if (source[i] == '\n') {
            lines.push_back(source.substr(start, i - start + 1));
            start = i + 1;
        }
    }
    if (start < source.size() || source.empty()) lines.push_back(source.substr(start));
    return lines;
}

std::string strip_common_indent(const std::string& text) {
    const std::vector<std::string> lines = split_preserving(text);
    if (lines.empty()) return text;
    std::size_t common = std::numeric_limits<std::size_t>::max();
    for (const std::string& line : lines) {
        // Blank / newline-only lines do not constrain common indent.
        bool only_ws = true;
        for (char ch : line) {
            if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
                only_ws = false;
                break;
            }
        }
        if (only_ws) continue;
        common = std::min(common, leading_indent_bytes(line));
    }
    if (common == std::numeric_limits<std::size_t>::max() || common == 0) return text;

    std::string out;
    out.reserve(text.size());
    for (const std::string& line : lines) {
        bool only_ws = true;
        for (char ch : line) {
            if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
                only_ws = false;
                break;
            }
        }
        if (only_ws) {
            out += line;
            continue;
        }
        const std::size_t strip = std::min(common, leading_indent_bytes(line));
        out.append(line, strip, std::string::npos);
    }
    return out;
}

// Compare two multi-line blocks after stripping each side's common leading indent.
bool indent_cores_equal(const std::string& a, const std::string& b) {
    return strip_common_indent(a) == strip_common_indent(b);
}

std::vector<TextSpan> find_indent_stripped(const std::string& haystack,
                                           const std::string& needle,
                                           std::size_t region_start,
                                           std::size_t region_end) {
    std::vector<TextSpan> matches;
    if (needle.find('\n') == std::string::npos) {
        // Single-line indent strip: strip leading indent from needle and from each line
        // start candidate in the region.
        const std::string core = strip_common_indent(needle);
        if (core.empty()) return matches;
        // Walk line starts within region.
        std::size_t line_start = region_start;
        // If region_start is mid-line, still try from region_start as a candidate.
        while (line_start < region_end) {
            std::size_t line_end = haystack.find('\n', line_start);
            if (line_end == std::string::npos || line_end >= region_end)
                line_end = region_end;
            else
                ++line_end;  // include newline when present inside region
            if (line_end > region_end) line_end = region_end;
            const std::string line = haystack.substr(line_start, line_end - line_start);
            // Try match at line_start after stripping indent from a window starting there.
            // Also try: core equals strip_common_indent of some prefix of the line.
            if (indent_cores_equal(line, needle) ||
                (line.size() >= core.size() &&
                 strip_common_indent(line.substr(0, std::min(line.size(), needle.size() + 32))) ==
                     core)) {
                // Prefer full-line match when cores equal.
                if (indent_cores_equal(line, needle)) {
                    TextSpan span;
                    span.offset = line_start;
                    // Match the non-trailing-only content used by the needle when possible.
                    // Use the whole line excluding a trailing newline if needle lacked one.
                    span.length = line.size();
                    if (!needle.empty() && needle.back() != '\n' && !line.empty() &&
                        line.back() == '\n' &&
                        indent_cores_equal(line.substr(0, line.size() - 1), needle)) {
                        span.length = line.size() - 1;
                    }
                    annotate_lines(haystack, span);
                    matches.push_back(span);
                    line_start = line_end;
                    continue;
                }
            }
            // Sliding exact core after strip on line content.
            const std::size_t indent = leading_indent_bytes(line);
            if (indent < line.size()) {
                const std::string stripped = line.substr(indent);
                // Compare stripped content without its newline against core without newline.
                std::string stripped_core = stripped;
                std::string needle_core = core;
                if (!stripped_core.empty() && stripped_core.back() == '\n')
                    stripped_core.pop_back();
                if (!needle_core.empty() && needle_core.back() == '\n') needle_core.pop_back();
                if (stripped_core == needle_core) {
                    TextSpan span;
                    span.offset = line_start + indent;
                    span.length = stripped_core.size();
                    annotate_lines(haystack, span);
                    matches.push_back(span);
                }
            }
            if (line_end <= line_start) break;
            line_start = line_end;
        }
        return matches;
    }

    // Multi-line: window of consecutive lines with the same line count as needle.
    const std::vector<std::string> needle_lines = split_preserving(needle);
    const std::size_t n_lines = needle_lines.size();
    if (n_lines == 0) return matches;

    // Build line start offsets for the whole haystack, then restrict to region.
    std::vector<std::size_t> line_starts;
    line_starts.push_back(0);
    for (std::size_t i = 0; i < haystack.size(); ++i) {
        if (haystack[i] == '\n' && i + 1 < haystack.size()) line_starts.push_back(i + 1);
    }

    for (std::size_t i = 0; i < line_starts.size(); ++i) {
        const std::size_t start = line_starts[i];
        if (start < region_start) continue;
        if (start >= region_end) break;
        if (i + n_lines - 1 >= line_starts.size()) break;
        // End of window: start of line after last, or EOF.
        std::size_t end = 0;
        if (i + n_lines < line_starts.size())
            end = line_starts[i + n_lines];
        else
            end = haystack.size();
        if (end > region_end) continue;
        if (end <= start) continue;
        const std::string window = haystack.substr(start, end - start);
        if (!indent_cores_equal(window, needle)) continue;
        // If needle does not end with newline but window does (because next line start
        // ate through trailing newline of last line), trim.
        TextSpan span;
        span.offset = start;
        span.length = end - start;
        if (!needle.empty() && needle.back() != '\n' && !window.empty() && window.back() == '\n' &&
            indent_cores_equal(window.substr(0, window.size() - 1), needle)) {
            span.length = window.size() - 1;
        }
        annotate_lines(haystack, span);
        matches.push_back(span);
    }
    return matches;
}

}  // namespace

std::size_t line_number_at(const std::string& text, std::size_t offset) {
    return count_lines_through(text, offset);
}

TextMatchResult find_text_matches(const std::string& haystack,
                                  const std::string& needle,
                                  bool allow_fuzzy,
                                  std::size_t region_start,
                                  std::size_t region_end) {
    TextMatchResult result;
    if (needle.empty()) return result;
    if (region_end == std::string::npos || region_end > haystack.size())
        region_end = haystack.size();
    if (region_start > region_end) region_start = region_end;

    std::vector<TextSpan> exact = find_exact(haystack, needle, region_start, region_end);
    if (!exact.empty()) {
        result.mode = "exact";
        result.matches = std::move(exact);
        return result;
    }
    if (!allow_fuzzy) return result;

    std::vector<TextSpan> normalized =
        find_normalized(haystack, needle, region_start, region_end);
    if (!normalized.empty()) {
        result.mode = "normalized_whitespace";
        result.matches = std::move(normalized);
        return result;
    }

    std::vector<TextSpan> indent =
        find_indent_stripped(haystack, needle, region_start, region_end);
    if (!indent.empty()) {
        result.mode = "indent_stripped";
        result.matches = std::move(indent);
        return result;
    }
    return result;
}

std::string apply_text_replacements(const std::string& haystack,
                                    const std::vector<TextSpan>& matches,
                                    const std::string& replacement,
                                    bool replace_all,
                                    std::size_t& replacements_made) {
    replacements_made = 0;
    if (matches.empty()) return haystack;
    if (!replace_all) {
        const TextSpan& span = matches.front();
        std::string out = haystack;
        out.replace(span.offset, span.length, replacement);
        replacements_made = 1;
        return out;
    }
    // Apply from the end so earlier offsets stay valid.
    std::vector<TextSpan> ordered = matches;
    std::sort(ordered.begin(), ordered.end(),
              [](const TextSpan& a, const TextSpan& b) { return a.offset > b.offset; });
    // Drop overlapping spans (keep later-starting when sorted descending = higher offset first).
    std::vector<TextSpan> non_overlap;
    std::size_t last_start = std::numeric_limits<std::size_t>::max();
    for (const TextSpan& span : ordered) {
        if (span.offset + span.length > last_start) continue;  // overlaps a kept higher span
        non_overlap.push_back(span);
        last_start = span.offset;
    }
    std::string out = haystack;
    for (const TextSpan& span : non_overlap) {
        out.replace(span.offset, span.length, replacement);
        ++replacements_made;
    }
    return out;
}

}  // namespace ainiux::agent
