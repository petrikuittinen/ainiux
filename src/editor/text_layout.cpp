#include "editor/text_layout.hpp"

#include "editor/detail/unicode.hpp"
#include "editor/detail/wrap.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>

namespace ainiux::editor {
namespace {

bool is_ascii_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v';
}

bool is_blank_line(const std::string& line) {
    for (char ch : line) {
        if (!is_ascii_space(ch)) {
            return false;
        }
    }
    return true;
}

size_t display_width(const std::string& text) {
    return detail::display_width_for_range(text, 0, text.size(), 4);
}

std::vector<std::string> split_physical_lines(const std::string& text, bool& had_trailing_newline) {
    had_trailing_newline = !text.empty() && text.back() == '\n';
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
        if (start == text.size()) {
            // Trailing newline does not create an extra empty line for processing
            // unless the content is only "\n".
            break;
        }
    }
    if (text.empty()) {
        lines.push_back("");
    }
    return lines;
}

std::string join_lines(const std::vector<std::string>& lines, bool trailing_newline) {
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) {
            out.push_back('\n');
        }
        out += lines[i];
    }
    if (trailing_newline && (out.empty() || out.back() != '\n')) {
        out.push_back('\n');
    }
    return out;
}

std::vector<std::string> words_in_paragraph(const std::vector<std::string>& paragraph_lines) {
    std::vector<std::string> words;
    for (const std::string& line : paragraph_lines) {
        size_t i = 0;
        while (i < line.size()) {
            while (i < line.size() && is_ascii_space(line[i])) {
                ++i;
            }
            if (i >= line.size()) {
                break;
            }
            const size_t begin = i;
            while (i < line.size() && !is_ascii_space(line[i])) {
                ++i;
            }
            words.push_back(line.substr(begin, i - begin));
        }
    }
    return words;
}

std::string pad_left(const std::string& line, size_t width) {
    const size_t w = display_width(line);
    if (w >= width) {
        return line;
    }
    return std::string(width - w, ' ') + line;
}

std::string pad_center(const std::string& line, size_t width) {
    const size_t w = display_width(line);
    if (w >= width) {
        return line;
    }
    const size_t pad = width - w;
    const size_t left = pad / 2;
    const size_t right = pad - left;
    return std::string(left, ' ') + line + std::string(right, ' ');
}

std::string join_words_single_space(const std::vector<std::string>& words,
                                    size_t begin,
                                    size_t end) {
    std::string line;
    for (size_t i = begin; i < end; ++i) {
        if (i != begin) {
            line.push_back(' ');
        }
        line += words[i];
    }
    return line;
}

std::string justify_words(const std::vector<std::string>& words,
                          size_t begin,
                          size_t end,
                          size_t width) {
    if (end <= begin) {
        return {};
    }
    if (end - begin == 1) {
        return words[begin];
    }
    size_t words_width = 0;
    for (size_t i = begin; i < end; ++i) {
        words_width += display_width(words[i]);
    }
    const size_t gaps = end - begin - 1;
    if (words_width + gaps >= width) {
        return join_words_single_space(words, begin, end);
    }
    const size_t extra = width - words_width - gaps;
    const size_t each = extra / gaps;
    const size_t rem = extra % gaps;
    std::string line = words[begin];
    for (size_t g = 0; g < gaps; ++g) {
        size_t spaces = 1 + each;
        if (g < rem) {
            ++spaces;
        }
        line.append(spaces, ' ');
        line += words[begin + g + 1];
    }
    return line;
}

TextLayoutResult reflow_paragraph(const std::vector<std::string>& paragraph_lines,
                                  TextAlignMode mode,
                                  size_t width,
                                  bool fail_on_overlong) {
    TextLayoutResult result;
    const std::vector<std::string> words = words_in_paragraph(paragraph_lines);
    if (words.empty()) {
        result.replacement = "";
        result.changed = false;
        result.line_count = 0;
        return result;
    }
    if (fail_on_overlong) {
        for (const std::string& word : words) {
            if (display_width(word) > width) {
                result.error = {ErrorCode::BadArgs,
                                "word wider than " + std::to_string(width) +
                                    " columns: " +
                                    word.substr(0, std::min<size_t>(word.size(), 40))};
                return result;
            }
        }
    }

    // Pack words into lines.
    std::vector<std::pair<size_t, size_t>> ranges;  // [begin, end)
    size_t begin = 0;
    while (begin < words.size()) {
        size_t end = begin;
        size_t line_w = 0;
        while (end < words.size()) {
            const size_t word_w = display_width(words[end]);
            const size_t next_w = end == begin ? word_w : line_w + 1 + word_w;
            if (next_w > width) {
                break;
            }
            line_w = next_w;
            ++end;
        }
        if (end == begin) {
            // Overlong word (display path) or single word: force one word.
            ++end;
        }
        ranges.push_back({begin, end});
        begin = end;
    }

    std::vector<std::string> out_lines;
    out_lines.reserve(ranges.size());
    for (size_t r = 0; r < ranges.size(); ++r) {
        const size_t b = ranges[r].first;
        const size_t e = ranges[r].second;
        const bool last = r + 1 == ranges.size();
        std::string line;
        if (mode == TextAlignMode::Justify && !last && e - b >= 2) {
            line = justify_words(words, b, e, width);
        } else {
            line = join_words_single_space(words, b, e);
            if (mode == TextAlignMode::Right) {
                line = pad_left(line, width);
            } else if (mode == TextAlignMode::Center) {
                line = pad_center(line, width);
            }
        }
        out_lines.push_back(std::move(line));
    }

    result.replacement = join_lines(out_lines, false);
    result.line_count = out_lines.size();
    result.changed = true;
    return result;
}

}  // namespace

const char* text_align_mode_name(TextAlignMode mode) {
    switch (mode) {
        case TextAlignMode::Left:
            return "left-align";
        case TextAlignMode::Right:
            return "right-align";
        case TextAlignMode::Center:
            return "center-align";
        case TextAlignMode::Justify:
            return "justify";
    }
    return "left-align";
}

bool parse_text_align_mode(const std::string& name, TextAlignMode& out) {
    if (name == "left-align" || name == "leftalign") {
        out = TextAlignMode::Left;
        return true;
    }
    if (name == "right-align" || name == "rightalign") {
        out = TextAlignMode::Right;
        return true;
    }
    if (name == "center-align" || name == "centeralign" || name == "centre-align") {
        out = TextAlignMode::Center;
        return true;
    }
    if (name == "justify" || name == "justify-align" || name == "justifyalign") {
        out = TextAlignMode::Justify;
        return true;
    }
    return false;
}

bool valid_text_align_width(size_t width) {
    return width > kMinTextAlignWidthExclusive && width <= kMaxTextAlignWidth;
}

bool valid_chat_align_width(long long width) {
    if (width == -1) {
        return true;
    }
    return width > static_cast<long long>(kMinTextAlignWidthExclusive) &&
           width <= static_cast<long long>(kMaxTextAlignWidth);
}

bool looks_like_fence_open(const std::string& line, std::string& fence) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    if (i >= line.size()) {
        return false;
    }
    const char marker = line[i];
    if (marker != '`' && marker != '~') {
        return false;
    }
    size_t count = 0;
    while (i + count < line.size() && line[i + count] == marker) {
        ++count;
    }
    if (count < 3) {
        return false;
    }
    fence.assign(count, marker);
    return true;
}

bool looks_like_fence_close(const std::string& line, const std::string& fence) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    if (i + fence.size() > line.size()) {
        return false;
    }
    if (line.compare(i, fence.size(), fence) != 0) {
        return false;
    }
    i += fence.size();
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    return i == line.size();
}

bool looks_like_table_line(const std::string& line) {
    if (line.empty()) {
        return false;
    }
    // Unicode box-drawing table rows from pretty_format_tables.
    const unsigned char c0 = static_cast<unsigned char>(line[0]);
    if (c0 == 0xE2U && line.size() >= 3) {
        // UTF-8 box drawing mostly starts with e2 94 xx or e2 95 xx
        const unsigned char c1 = static_cast<unsigned char>(line[1]);
        if (c1 == 0x94U || c1 == 0x95U) {
            return true;
        }
    }
    // GFM pipe tables and separators.
    if (line.find('|') != std::string::npos) {
        size_t i = 0;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
            ++i;
        }
        if (i < line.size() && line[i] == '|') {
            return true;
        }
        // Loose GFM row without leading pipe still has multiple bars.
        size_t bars = 0;
        for (char ch : line) {
            if (ch == '|') {
                ++bars;
            }
        }
        return bars >= 2;
    }
    return false;
}

void append_reflowed_paragraph(std::vector<std::string>& out_lines,
                               const std::vector<std::string>& paragraph,
                               TextAlignMode mode,
                               size_t width,
                               bool fail_on_overlong,
                               TextLayoutResult& result,
                               size_t& total_content_lines) {
    TextLayoutResult part = reflow_paragraph(paragraph, mode, width, fail_on_overlong);
    if (!part.error.ok()) {
        result = part;
        return;
    }
    bool part_trailing = false;
    const std::vector<std::string> part_lines =
        split_physical_lines(part.replacement, part_trailing);
    for (const std::string& pl : part_lines) {
        out_lines.push_back(pl);
    }
    total_content_lines += part.line_count;
}

TextLayoutResult reflow_align(const std::string& text, TextAlignMode mode, size_t width) {
    TextLayoutResult result;
    if (!valid_text_align_width(width)) {
        result.error = {ErrorCode::BadArgs,
                        "width must be greater than 20 and at most " +
                            std::to_string(kMaxTextAlignWidth)};
        return result;
    }

    bool trailing = false;
    const std::vector<std::string> lines = split_physical_lines(text, trailing);
    std::vector<std::string> out_lines;
    size_t index = 0;
    size_t total_content_lines = 0;
    while (index < lines.size()) {
        if (is_blank_line(lines[index])) {
            out_lines.push_back("");
            ++index;
            continue;
        }
        std::vector<std::string> paragraph;
        while (index < lines.size() && !is_blank_line(lines[index])) {
            paragraph.push_back(lines[index]);
            ++index;
        }
        append_reflowed_paragraph(out_lines, paragraph, mode, width, true, result,
                                  total_content_lines);
        if (!result.error.ok()) {
            return result;
        }
    }

    result.replacement = join_lines(out_lines, trailing);
    result.line_count = total_content_lines;
    result.changed = result.replacement != text;
    return result;
}

TextLayoutResult reflow_align_display(const std::string& text, TextAlignMode mode, size_t width) {
    TextLayoutResult result;
    if (width == 0) {
        result.replacement = text;
        result.changed = false;
        return result;
    }

    bool trailing = false;
    const std::vector<std::string> lines = split_physical_lines(text, trailing);
    std::vector<std::string> out_lines;
    size_t index = 0;
    size_t total_content_lines = 0;
    std::string open_fence;
    while (index < lines.size()) {
        if (!open_fence.empty()) {
            out_lines.push_back(lines[index]);
            if (looks_like_fence_close(lines[index], open_fence)) {
                open_fence.clear();
            }
            ++index;
            continue;
        }

        std::string fence;
        if (looks_like_fence_open(lines[index], fence)) {
            open_fence = fence;
            out_lines.push_back(lines[index]);
            ++index;
            continue;
        }

        if (looks_like_table_line(lines[index])) {
            while (index < lines.size() && looks_like_table_line(lines[index])) {
                out_lines.push_back(lines[index]);
                ++index;
            }
            continue;
        }

        if (is_blank_line(lines[index])) {
            out_lines.push_back("");
            ++index;
            continue;
        }

        std::vector<std::string> paragraph;
        while (index < lines.size() && !is_blank_line(lines[index]) &&
               !looks_like_table_line(lines[index])) {
            std::string next_fence;
            if (looks_like_fence_open(lines[index], next_fence)) {
                break;
            }
            paragraph.push_back(lines[index]);
            ++index;
        }
        append_reflowed_paragraph(out_lines, paragraph, mode, width, false, result,
                                  total_content_lines);
        if (!result.error.ok()) {
            return result;
        }
    }

    result.replacement = join_lines(out_lines, trailing);
    result.line_count = total_content_lines;
    result.changed = result.replacement != text;
    result.error = ok_error();
    return result;
}

TextLayoutResult remove_blank_lines(const std::string& text) {
    TextLayoutResult result;
    bool trailing = false;
    const std::vector<std::string> lines = split_physical_lines(text, trailing);
    std::vector<std::string> out;
    out.reserve(lines.size());
    for (const std::string& line : lines) {
        if (!is_blank_line(line)) {
            out.push_back(line);
        }
    }
    result.replacement = join_lines(out, trailing);
    result.line_count = out.size();
    result.changed = result.replacement != text;
    return result;
}

TextLayoutResult remove_duplicate_blank_lines(const std::string& text) {
    TextLayoutResult result;
    bool trailing = false;
    const std::vector<std::string> lines = split_physical_lines(text, trailing);
    std::vector<std::string> out;
    out.reserve(lines.size());
    bool prev_blank = false;
    for (const std::string& line : lines) {
        const bool blank = is_blank_line(line);
        if (blank && prev_blank) {
            continue;
        }
        out.push_back(blank ? std::string() : line);
        prev_blank = blank;
    }
    result.replacement = join_lines(out, trailing);
    result.line_count = out.size();
    result.changed = result.replacement != text;
    return result;
}

TextLayoutResult remove_duplicate_lines(const std::string& text) {
    TextLayoutResult result;
    bool trailing = false;
    const std::vector<std::string> lines = split_physical_lines(text, trailing);
    std::vector<std::string> out;
    out.reserve(lines.size());
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0 && lines[i] == lines[i - 1]) {
            continue;
        }
        out.push_back(lines[i]);
    }
    result.replacement = join_lines(out, trailing);
    result.line_count = out.size();
    result.changed = result.replacement != text;
    return result;
}

void text_layout_scope(const EditorState& state, size_t& start, size_t& end, bool& had_selection) {
    had_selection = state.selection.has_range();
    if (!had_selection) {
        start = 0;
        end = state.text.size();
        return;
    }
    size_t sel_start = state.selection.start();
    size_t sel_end = state.selection_end_exclusive();
    const size_t first_line = state.text.line_for_offset(sel_start);
    size_t last_line = state.text.line_for_offset(sel_end > sel_start ? sel_end - 1 : sel_end);
    // If selection ends exactly at a line start (and spans more than one line),
    // do not include that following line (same spirit as /reformat).
    if (sel_end > sel_start && sel_end == state.text.line_start(last_line) &&
        last_line > first_line) {
        --last_line;
    }
    start = state.text.line_start(first_line);
    end = last_line + 1 < state.text.line_count() ? state.text.line_start(last_line + 1)
                                                  : state.text.size();
}

Error apply_text_layout_result(EditorState& state,
                               size_t start,
                               size_t end,
                               bool had_selection,
                               const TextLayoutResult& result) {
    if (!result.error.ok()) {
        return result.error;
    }
    if (!result.changed) {
        return ok_error();
    }
    const bool reverse =
        had_selection && state.selection.has_range() && state.selection.anchor > state.selection.active;
    Error error = state.replace(start, end - start, result.replacement);
    if (!error.ok()) {
        return error;
    }
    if (had_selection) {
        const size_t new_end = start + result.replacement.size();
        state.selection.anchor = reverse ? new_end : start;
        state.selection.active = reverse ? start : new_end;
        state.cursor = state.selection.active;
    } else {
        state.cursor = std::min(state.cursor, state.text.size());
        state.selection.clear(state.cursor);
    }
    state.preferred_column = state.text.display_column_for_offset(state.cursor, state.tab_width);
    return ok_error();
}

}  // namespace ainiux::editor
