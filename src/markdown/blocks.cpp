#include "markdown/blocks.hpp"

#include "common.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

namespace ainiux::markdown {
namespace {

bool is_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

std::string ltrim(std::string text) {
    while (!text.empty() && is_space(text.front())) {
        text.erase(text.begin());
    }
    return text;
}

std::string rtrim(std::string text) {
    while (!text.empty() && is_space(text.back())) {
        text.pop_back();
    }
    return text;
}

std::string trim(std::string text) {
    return rtrim(ltrim(std::move(text)));
}

bool starts_with(const std::string& text, const std::string& prefix) {
    return text.rfind(prefix, 0) == 0;
}

size_t leading_spaces(const std::string& line) {
    size_t count = 0;
    for (char ch : line) {
        if (ch == ' ') {
            ++count;
        } else if (ch == '\t') {
            count += 4;
        } else {
            break;
        }
    }
    return count;
}

size_t find_matching_square_bracket(const std::string& input, size_t open) {
    if (open >= input.size() || input[open] != '[') {
        return std::string::npos;
    }
    size_t depth = 0;
    for (size_t i = open; i < input.size(); ++i) {
        if (input[i] == '\\') {
            ++i;
            continue;
        }
        if (input[i] == '[') {
            ++depth;
        } else if (input[i] == ']' && --depth == 0) {
            return i;
        }
    }
    return std::string::npos;
}

void append_run(std::vector<Run>& runs, std::string text, unsigned style, const std::string& url) {
    if (text.empty()) {
        return;
    }
    if (!runs.empty() && runs.back().style == style && runs.back().url == url &&
        !runs.back().hard_break_after) {
        runs.back().text += text;
        return;
    }
    Run run;
    run.text = std::move(text);
    run.style = style;
    run.url = url;
    runs.push_back(std::move(run));
}

void parse_inlines(const std::string& input, unsigned style, const std::string& url,
                   std::vector<Run>& out, int depth) {
    if (depth > 32) {
        append_run(out, input, style, url);
        return;
    }
    for (size_t i = 0; i < input.size();) {
        if (input[i] == '`') {
            const size_t close = input.find('`', i + 1);
            if (close != std::string::npos) {
                parse_inlines(input.substr(i + 1, close - i - 1),
                              style | static_cast<unsigned>(RunStyle::Code), url, out, depth + 1);
                i = close + 1;
                continue;
            }
        }
        if (input.compare(i, 3, "***") == 0 || input.compare(i, 3, "___") == 0) {
            const std::string marker = input.substr(i, 3);
            const size_t close = input.find(marker, i + 3);
            if (close != std::string::npos) {
                parse_inlines(input.substr(i + 3, close - i - 3),
                              style | static_cast<unsigned>(RunStyle::Bold) |
                                  static_cast<unsigned>(RunStyle::Italic),
                              url, out, depth + 1);
                i = close + 3;
                continue;
            }
        }
        if (input.compare(i, 2, "~~") == 0) {
            const size_t close = input.find("~~", i + 2);
            if (close != std::string::npos) {
                parse_inlines(input.substr(i + 2, close - i - 2),
                              style | static_cast<unsigned>(RunStyle::Strike), url, out, depth + 1);
                i = close + 2;
                continue;
            }
        }
        if (input.compare(i, 2, "![") == 0) {
            const size_t label_end = find_matching_square_bracket(input, i + 1);
            if (label_end != std::string::npos && input.compare(label_end, 2, "](") == 0) {
                const size_t url_end = input.find(')', label_end + 2);
                if (url_end != std::string::npos) {
                    append_run(out, input.substr(i + 2, label_end - i - 2), style, url);
                    i = url_end + 1;
                    continue;
                }
            }
        }
        if (input[i] == '[') {
            const size_t label_end = find_matching_square_bracket(input, i);
            if (label_end != std::string::npos && input.compare(label_end, 2, "](") == 0) {
                const size_t url_end = input.find(')', label_end + 2);
                if (url_end != std::string::npos) {
                    const std::string href = input.substr(label_end + 2, url_end - label_end - 2);
                    parse_inlines(input.substr(i + 1, label_end - i - 1), style, href, out, depth + 1);
                    if (!href.empty()) {
                        append_run(out, " (" + href + ")", style, "");
                    }
                    i = url_end + 1;
                    continue;
                }
            }
        }
        if (input.compare(i, 2, "**") == 0 || input.compare(i, 2, "__") == 0) {
            const std::string marker = input.substr(i, 2);
            const size_t close = input.find(marker, i + 2);
            if (close != std::string::npos) {
                parse_inlines(input.substr(i + 2, close - i - 2),
                              style | static_cast<unsigned>(RunStyle::Bold), url, out, depth + 1);
                i = close + 2;
                continue;
            }
        }
        if (input.compare(i, 2, "++") == 0) {
            const size_t close = input.find("++", i + 2);
            if (close != std::string::npos) {
                parse_inlines(input.substr(i + 2, close - i - 2), style, url, out, depth + 1);
                i = close + 2;
                continue;
            }
        }
        if (input[i] == '*' || input[i] == '_') {
            const char marker = input[i];
            const size_t close = input.find(marker, i + 1);
            if (close != std::string::npos) {
                parse_inlines(input.substr(i + 1, close - i - 1),
                              style | static_cast<unsigned>(RunStyle::Italic), url, out, depth + 1);
                i = close + 1;
                continue;
            }
        }
        size_t j = i + 1;
        while (j < input.size()) {
            const char ch = input[j];
            if (ch == '`' || ch == '*' || ch == '_' || ch == '~' || ch == '[' || ch == '!' ||
                ch == '+') {
                break;
            }
            ++j;
        }
        append_run(out, input.substr(i, j - i), style, url);
        i = j;
    }
}

std::vector<Run> parse_inline_line(const std::string& text) {
    std::vector<Run> runs;
    parse_inlines(text, 0, "", runs, 0);
    return runs;
}

struct ListMarker {
    bool matched = false;
    bool ordered = false;
    size_t indent = 0;
    size_t text_start = 0;
};

bool parse_header(const std::string& line, int& level, std::string& text) {
    const std::string left = ltrim(line);
    if (left.empty() || left[0] != '#') {
        return false;
    }
    size_t count = 0;
    while (count < left.size() && left[count] == '#') {
        ++count;
    }
    if (count == 0 || count > 6 || count >= left.size() || !is_space(left[count])) {
        return false;
    }
    level = static_cast<int>(count);
    text = trim(left.substr(count));
    while (!text.empty() && text.back() == '#') {
        text.pop_back();
    }
    text = rtrim(text);
    return true;
}

bool parse_list_marker(const std::string& line, ListMarker& marker) {
    marker = {};
    size_t pos = 0;
    size_t indent = 0;
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
        indent += line[pos] == '\t' ? 4 : 1;
        ++pos;
    }
    if (pos >= line.size()) {
        return false;
    }
    if ((line[pos] == '-' || line[pos] == '*' || line[pos] == '+') &&
        pos + 1 < line.size() && is_space(line[pos + 1])) {
        marker.matched = true;
        marker.ordered = false;
        marker.indent = indent;
        marker.text_start = pos + 2;
        return true;
    }
    if (std::isdigit(static_cast<unsigned char>(line[pos]))) {
        size_t end = pos;
        while (end < line.size() && std::isdigit(static_cast<unsigned char>(line[end]))) {
            ++end;
        }
        if (end < line.size() && (line[end] == '.' || line[end] == ')') &&
            end + 1 < line.size() && is_space(line[end + 1])) {
            marker.matched = true;
            marker.ordered = true;
            marker.indent = indent;
            marker.text_start = end + 2;
            return true;
        }
    }
    return false;
}

bool parse_fence_open(const std::string& line, std::string& fence, std::string& info) {
    const std::string left = ltrim(line);
    if (!starts_with(left, "```") && !starts_with(left, "~~~")) {
        return false;
    }
    const char marker = left[0];
    size_t count = 0;
    while (count < left.size() && left[count] == marker) {
        ++count;
    }
    if (count < 3) {
        return false;
    }
    fence.assign(count, marker);
    info = trim(left.substr(count));
    return true;
}

bool is_fence_close(const std::string& line, const std::string& fence) {
    const std::string left = ltrim(line);
    if (!starts_with(left, fence)) {
        return false;
    }
    return trim(left.substr(fence.size())).empty();
}

bool is_indented_code_line(const std::string& line) {
    if (leading_spaces(line) < 4) {
        return false;
    }
    ListMarker marker;
    if (parse_list_marker(line, marker)) {
        return false;
    }
    return true;
}

std::string remove_code_indent(const std::string& line) {
    size_t pos = 0;
    size_t count = 0;
    while (pos < line.size() && count < 4) {
        if (line[pos] == ' ') {
            ++count;
            ++pos;
        } else if (line[pos] == '\t') {
            count = 4;
            ++pos;
        } else {
            break;
        }
    }
    return line.substr(pos);
}

bool is_horizontal_rule(const std::string& line) {
    const std::string stripped = trim(line);
    if (stripped.size() < 3) {
        return false;
    }
    const char marker = stripped[0];
    if (marker != '-' && marker != '*' && marker != '_') {
        return false;
    }
    return std::all_of(stripped.begin(), stripped.end(), [&](char ch) { return ch == marker; });
}

bool parse_blockquote_marker(const std::string& line, std::string& text) {
    std::string left = ltrim(line);
    if (left.empty() || left[0] != '>') {
        return false;
    }
    left.erase(left.begin());
    left = ltrim(left);
    text = left;
    return true;
}

bool line_has_hard_break(const std::string& line) {
    std::string raw = line;
    if (!raw.empty() && raw.back() == '\r') {
        raw.pop_back();
    }
    return raw.size() >= 2 && raw.back() == ' ' && raw[raw.size() - 2] == ' ';
}

std::string strip_hard_break_suffix(std::string line) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    while (line.size() >= 2 && line.back() == ' ' && line[line.size() - 2] == ' ') {
        line.pop_back();
    }
    return rtrim(std::move(line));
}

std::vector<std::string> split_table_row(std::string line) {
    line = trim(std::move(line));
    if (!line.empty() && line.front() == '|') {
        line.erase(line.begin());
    }
    if (!line.empty() && line.back() == '|') {
        line.pop_back();
    }
    std::vector<std::string> cells;
    size_t start = 0;
    while (start <= line.size()) {
        const size_t bar = line.find('|', start);
        const size_t end = bar == std::string::npos ? line.size() : bar;
        cells.push_back(trim(line.substr(start, end - start)));
        if (bar == std::string::npos) {
            break;
        }
        start = bar + 1;
    }
    return cells;
}

bool is_table_separator_cell(std::string cell) {
    cell = trim(std::move(cell));
    if (!cell.empty() && cell.front() == ':') {
        cell.erase(cell.begin());
    }
    if (!cell.empty() && cell.back() == ':') {
        cell.pop_back();
    }
    if (cell.size() < 3) {
        return false;
    }
    return std::all_of(cell.begin(), cell.end(), [](char ch) { return ch == '-'; });
}

bool is_table_separator_line(const std::string& line) {
    const std::vector<std::string> cells = split_table_row(line);
    if (cells.size() < 2) {
        return false;
    }
    for (const std::string& cell : cells) {
        if (!is_table_separator_cell(cell)) {
            return false;
        }
    }
    return true;
}

bool is_table_start(const std::vector<std::string>& lines, size_t index) {
    if (index + 1 >= lines.size() || lines[index].find('|') == std::string::npos) {
        return false;
    }
    const std::vector<std::string> header = split_table_row(lines[index]);
    return header.size() >= 2 && is_table_separator_line(lines[index + 1]);
}

bool is_raw_html_block(const std::string& line) {
    const std::string stripped = trim(line);
    return !stripped.empty() && stripped.front() == '<';
}

bool is_block_start(const std::vector<std::string>& lines, size_t index) {
    const std::string stripped = trim(lines[index]);
    if (stripped.empty()) {
        return true;
    }
    std::string fence;
    std::string info;
    int header_level = 0;
    std::string header_text;
    ListMarker list;
    std::string quote;
    return parse_fence_open(lines[index], fence, info) || is_indented_code_line(lines[index]) ||
           parse_header(lines[index], header_level, header_text) || is_horizontal_rule(lines[index]) ||
           parse_blockquote_marker(lines[index], quote) || is_table_start(lines, index) ||
           parse_list_marker(lines[index], list) || is_raw_html_block(lines[index]);
}

}  // namespace

std::vector<Block> parse_blocks(const std::string& markdown) {
    const std::vector<std::string> lines = split_lines_crlf(markdown);
    std::vector<Block> blocks;
    int ordered_index = 0;
    int last_list_indent = -1;
    bool last_ordered = false;

    for (size_t i = 0; i < lines.size();) {
        const std::string stripped = trim(lines[i]);
        if (stripped.empty()) {
            ++i;
            continue;
        }

        std::string fence;
        std::string info;
        if (parse_fence_open(lines[i], fence, info)) {
            Block block;
            block.kind = BlockKind::Code;
            ++i;
            while (i < lines.size() && !is_fence_close(lines[i], fence)) {
                block.text += lines[i];
                block.text.push_back('\n');
                ++i;
            }
            if (i < lines.size()) {
                ++i;
            }
            blocks.push_back(std::move(block));
            continue;
        }

        if (is_indented_code_line(lines[i])) {
            Block block;
            block.kind = BlockKind::Code;
            while (i < lines.size() && (is_indented_code_line(lines[i]) || trim(lines[i]).empty())) {
                if (trim(lines[i]).empty()) {
                    block.text.push_back('\n');
                } else {
                    block.text += remove_code_indent(lines[i]);
                    block.text.push_back('\n');
                }
                ++i;
            }
            blocks.push_back(std::move(block));
            continue;
        }

        int header_level = 0;
        std::string header_text;
        if (parse_header(lines[i], header_level, header_text)) {
            Block block;
            block.kind = BlockKind::Heading;
            block.heading_level = header_level;
            block.runs = parse_inline_line(header_text);
            blocks.push_back(std::move(block));
            ++i;
            continue;
        }

        if (is_horizontal_rule(lines[i])) {
            Block block;
            block.kind = BlockKind::Rule;
            blocks.push_back(std::move(block));
            ++i;
            continue;
        }

        std::string quote_text;
        if (parse_blockquote_marker(lines[i], quote_text)) {
            std::string inner;
            while (i < lines.size() && parse_blockquote_marker(lines[i], quote_text)) {
                inner += quote_text;
                inner.push_back('\n');
                ++i;
            }
            Block block;
            block.kind = BlockKind::Quote;
            block.children = parse_blocks(inner);
            blocks.push_back(std::move(block));
            continue;
        }

        if (is_table_start(lines, i)) {
            Block block;
            block.kind = BlockKind::Table;
            auto add_row = [&](const std::vector<std::string>& cells) {
                std::vector<std::vector<Run>> row;
                row.reserve(cells.size());
                for (const std::string& cell : cells) {
                    row.push_back(parse_inline_line(cell));
                }
                block.table_cells.push_back(std::move(row));
            };
            add_row(split_table_row(lines[i]));
            i += 2;
            while (i < lines.size() && lines[i].find('|') != std::string::npos &&
                   !trim(lines[i]).empty()) {
                add_row(split_table_row(lines[i]));
                ++i;
            }
            blocks.push_back(std::move(block));
            continue;
        }

        ListMarker list;
        if (parse_list_marker(lines[i], list)) {
            const int indent = static_cast<int>(list.indent);
            if (last_list_indent != indent || last_ordered != list.ordered) {
                ordered_index = 0;
            }
            last_list_indent = indent;
            last_ordered = list.ordered;
            ++ordered_index;
            Block block;
            block.kind = BlockKind::ListItem;
            block.ordered = list.ordered;
            block.indent = indent;
            block.list_index = ordered_index;
            block.runs = parse_inline_line(trim(lines[i].substr(list.text_start)));
            blocks.push_back(std::move(block));
            ++i;
            continue;
        }

        if (is_raw_html_block(lines[i])) {
            Block block;
            block.kind = BlockKind::Html;
            block.runs = parse_inline_line(trim(lines[i]));
            blocks.push_back(std::move(block));
            ++i;
            continue;
        }

        last_list_indent = -1;
        Block block;
        block.kind = BlockKind::Paragraph;
        while (i < lines.size() && !is_block_start(lines, i)) {
            std::vector<Run> line_runs = parse_inline_line(strip_hard_break_suffix(lines[i]));
            const bool hard = line_has_hard_break(lines[i]);
            if (!block.runs.empty() && !hard) {
                append_run(block.runs, " ", 0, "");
            } else if (!block.runs.empty() && hard && !block.runs.empty()) {
                block.runs.back().hard_break_after = true;
            }
            for (Run& run : line_runs) {
                block.runs.push_back(std::move(run));
            }
            ++i;
        }
        if (!block.runs.empty()) {
            blocks.push_back(std::move(block));
        }
    }
    return blocks;
}

}  // namespace ainiux::markdown
