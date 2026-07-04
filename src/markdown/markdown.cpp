#include "markdown/markdown.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

#include "pkchat/version.hpp"

namespace pkchat::markdown {
namespace {

enum class RenderMode {
    Html,
    Plaintext,
};

struct ListMarker {
    bool matched = false;
    bool ordered = false;
    size_t indent = 0;
    size_t text_start = 0;
};

struct ListState {
    size_t indent = 0;
    bool ordered = false;
    bool li_open = false;
};

std::string lower_ascii(std::string text) {
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return text;
}

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

std::vector<std::string> split_lines(const std::string& input) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= input.size()) {
        const size_t end = input.find('\n', start);
        std::string line = end == std::string::npos ? input.substr(start) : input.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return lines;
}

std::string escape_html_text(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (char ch : input) {
        switch (ch) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            default:
                out.push_back(ch);
                break;
        }
    }
    return out;
}

std::string escape_html_attr(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (char ch : input) {
        switch (ch) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            default:
                out.push_back(ch);
                break;
        }
    }
    return out;
}

std::string decode_basic_entities(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input.compare(i, 5, "&amp;") == 0) {
            out.push_back('&');
            i += 4;
        } else if (input.compare(i, 4, "&lt;") == 0) {
            out.push_back('<');
            i += 3;
        } else if (input.compare(i, 4, "&gt;") == 0) {
            out.push_back('>');
            i += 3;
        } else if (input.compare(i, 6, "&quot;") == 0) {
            out.push_back('"');
            i += 5;
        } else if (input.compare(i, 6, "&apos;") == 0) {
            out.push_back('\'');
            i += 5;
        } else {
            out.push_back(input[i]);
        }
    }
    return out;
}

bool plausible_html_tag_at(const std::string& input, size_t pos, size_t& end) {
    if (pos >= input.size() || input[pos] != '<') {
        return false;
    }
    end = input.find('>', pos + 1);
    if (end == std::string::npos) {
        return false;
    }
    if (input.compare(pos, 4, "<!--") == 0) {
        return true;
    }
    size_t i = pos + 1;
    if (i < input.size() && input[i] == '/') {
        ++i;
    }
    if (i >= input.size() || !std::isalpha(static_cast<unsigned char>(input[i]))) {
        return false;
    }
    return true;
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

std::string render_inline(const std::string& input, RenderMode mode, int depth = 0) {
    if (depth > 32) {
        return mode == RenderMode::Html ? escape_html_text(input) : decode_basic_entities(input);
    }

    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size();) {
        if (input[i] == '`') {
            const size_t close = input.find('`', i + 1);
            if (close != std::string::npos) {
                const std::string code = input.substr(i + 1, close - i - 1);
                if (mode == RenderMode::Html) {
                    out += "<code>" + escape_html_text(code) + "</code>";
                } else {
                    out += code;
                }
                i = close + 1;
                continue;
            }
        }

        if (input.compare(i, 3, "***") == 0 || input.compare(i, 3, "___") == 0) {
            const std::string marker = input.substr(i, 3);
            const size_t close = input.find(marker, i + 3);
            if (close != std::string::npos) {
                const std::string body = render_inline(input.substr(i + 3, close - i - 3), mode, depth + 1);
                if (mode == RenderMode::Html) {
                    out += "<strong><em>" + body + "</em></strong>";
                } else {
                    out += body;
                }
                i = close + 3;
                continue;
            }
        }

        if (input.compare(i, 2, "~~") == 0) {
            const size_t close = input.find("~~", i + 2);
            if (close != std::string::npos) {
                const std::string body = render_inline(input.substr(i + 2, close - i - 2), mode, depth + 1);
                if (mode == RenderMode::Html) {
                    out += "<del>" + body + "</del>";
                } else {
                    out += body;
                }
                i = close + 2;
                continue;
            }
        }

        if (input.compare(i, 2, "![") == 0) {
            const size_t label_end = find_matching_square_bracket(input, i + 1);
            if (label_end != std::string::npos && input.compare(label_end, 2, "](") == 0) {
                const size_t url_end = input.find(')', label_end + 2);
                if (url_end != std::string::npos) {
                    const std::string label = input.substr(i + 2, label_end - i - 2);
                    const std::string url = input.substr(label_end + 2, url_end - label_end - 2);
                    if (mode == RenderMode::Html) {
                        out += "<img src=\"" + escape_html_attr(url) + "\" alt=\"" +
                               escape_html_attr(label) + "\">";
                    } else {
                        out += label;
                        if (!url.empty()) {
                            out += " (" + url + ")";
                        }
                    }
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
                    const std::string label = input.substr(i + 1, label_end - i - 1);
                    const std::string url = input.substr(label_end + 2, url_end - label_end - 2);
                    if (mode == RenderMode::Html) {
                        out += "<a href=\"" + escape_html_attr(url) + "\">" +
                               render_inline(label, mode, depth + 1) + "</a>";
                    } else {
                        out += render_inline(label, mode, depth + 1);
                        if (!url.empty()) {
                            out += " (" + url + ")";
                        }
                    }
                    i = url_end + 1;
                    continue;
                }
            }
        }

        if (input.compare(i, 2, "**") == 0 || input.compare(i, 2, "__") == 0 ||
            input.compare(i, 2, "++") == 0) {
            const std::string marker = input.substr(i, 2);
            const size_t close = input.find(marker, i + 2);
            if (close != std::string::npos) {
                const std::string body = render_inline(input.substr(i + 2, close - i - 2), mode, depth + 1);
                if (mode == RenderMode::Html) {
                    if (marker == "++") {
                        out += "<u>" + body + "</u>";
                    } else {
                        out += "<strong>" + body + "</strong>";
                    }
                } else {
                    out += body;
                }
                i = close + 2;
                continue;
            }
        }

        if (input[i] == '*' || input[i] == '_') {
            const char marker = input[i];
            const size_t close = input.find(marker, i + 1);
            if (close != std::string::npos) {
                const std::string body = render_inline(input.substr(i + 1, close - i - 1), mode, depth + 1);
                if (mode == RenderMode::Html) {
                    out += "<em>" + body + "</em>";
                } else {
                    out += body;
                }
                i = close + 1;
                continue;
            }
        }

        size_t tag_end = std::string::npos;
        if (input[i] == '<' && plausible_html_tag_at(input, i, tag_end)) {
            if (mode == RenderMode::Html) {
                out += input.substr(i, tag_end - i + 1);
            }
            i = tag_end + 1;
            continue;
        }

        if (mode == RenderMode::Html) {
            switch (input[i]) {
                case '&':
                    out += "&amp;";
                    break;
                case '<':
                    out += "&lt;";
                    break;
                case '>':
                    out += "&gt;";
                    break;
                default:
                    out.push_back(input[i]);
                    break;
            }
        } else {
            out.push_back(input[i]);
        }
        ++i;
    }
    return mode == RenderMode::Plaintext ? decode_basic_entities(out) : out;
}

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

std::string first_word(const std::string& text) {
    const std::string stripped = trim(text);
    size_t end = 0;
    while (end < stripped.size() && !is_space(stripped[end])) {
        ++end;
    }
    return stripped.substr(0, end);
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
    return raw.size() >= 2 && raw[raw.size() - 1] == ' ' && raw[raw.size() - 2] == ' ';
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

bool is_raw_html_block(const std::string& line) {
    const std::string stripped = trim(line);
    if (stripped.empty() || stripped.front() != '<') {
        return false;
    }
    size_t end = std::string::npos;
    return plausible_html_tag_at(stripped, 0, end);
}

void append_plain_line(std::string& out, const std::string& text) {
    out += rtrim(text);
    out.push_back('\n');
}

class BlockRenderer {
   public:
    explicit BlockRenderer(RenderMode mode) : mode_(mode) {}

    std::string render(const std::string& markdown) {
        const std::vector<std::string> lines = split_lines(markdown);
        for (size_t i = 0; i < lines.size();) {
            const std::string stripped = trim(lines[i]);
            if (stripped.empty()) {
                close_lists();
                if (mode_ == RenderMode::Plaintext && !out_.empty() && out_.back() != '\n') {
                    out_.push_back('\n');
                }
                ++i;
                continue;
            }

            std::string fence;
            std::string info;
            if (parse_fence_open(lines[i], fence, info)) {
                close_lists();
                emit_fenced_code(lines, i, fence, info);
                continue;
            }

            if (is_indented_code_line(lines[i])) {
                close_lists();
                emit_indented_code(lines, i);
                continue;
            }

            int header_level = 0;
            std::string header_text;
            if (parse_header(lines[i], header_level, header_text)) {
                close_lists();
                emit_header(header_level, header_text);
                ++i;
                continue;
            }

            if (is_horizontal_rule(lines[i])) {
                close_lists();
                emit_horizontal_rule();
                ++i;
                continue;
            }

            std::string blockquote_text;
            if (parse_blockquote_marker(lines[i], blockquote_text)) {
                close_lists();
                emit_blockquote(lines, i);
                continue;
            }

            if (is_table_start(lines, i)) {
                close_lists();
                emit_table(lines, i);
                continue;
            }

            ListMarker list;
            if (parse_list_marker(lines[i], list)) {
                emit_list_item(list, lines[i].substr(list.text_start));
                ++i;
                continue;
            }

            if (is_raw_html_block(lines[i])) {
                close_lists();
                emit_raw_html(lines[i]);
                ++i;
                continue;
            }

            close_lists();
            emit_paragraph(lines, i);
        }
        close_lists();
        trim_trailing_blank_lines();
        if (!out_.empty()) {
            out_.push_back('\n');
        }
        return out_;
    }

   private:
    RenderMode mode_;
    std::string out_;
    std::vector<ListState> lists_;

    void emit_header(int level, const std::string& text) {
        if (mode_ == RenderMode::Html) {
            out_ += "<h" + std::to_string(level) + ">" + render_inline(text, mode_) +
                    "</h" + std::to_string(level) + ">\n";
        } else {
            append_plain_line(out_, render_inline(text, mode_));
        }
    }

    void emit_horizontal_rule() {
        if (mode_ == RenderMode::Html) {
            out_ += "<hr>\n";
        } else {
            append_plain_line(out_, "---");
        }
    }

    void emit_blockquote(const std::vector<std::string>& lines, size_t& index) {
        if (mode_ == RenderMode::Html) {
            out_ += "<blockquote>\n";
        }
        while (index < lines.size()) {
            std::string text;
            if (!parse_blockquote_marker(lines[index], text)) {
                break;
            }
            if (mode_ == RenderMode::Html) {
                out_ += "<p>" + render_inline(text, mode_) + "</p>\n";
            } else {
                append_plain_line(out_, "> " + render_inline(text, mode_));
            }
            ++index;
        }
        if (mode_ == RenderMode::Html) {
            out_ += "</blockquote>\n";
        }
    }

    void emit_fenced_code(const std::vector<std::string>& lines,
                          size_t& index,
                          const std::string& fence,
                          const std::string& info) {
        std::string code;
        ++index;
        while (index < lines.size() && !is_fence_close(lines[index], fence)) {
            code += lines[index];
            code.push_back('\n');
            ++index;
        }
        if (index < lines.size()) {
            ++index;
        }
        if (mode_ == RenderMode::Html) {
            const std::string lang = first_word(info);
            out_ += "<pre><code";
            if (!lang.empty()) {
                out_ += " class=\"language-" + escape_html_attr(lang) + "\"";
            }
            out_ += ">";
            out_ += escape_html_text(code);
            out_ += "</code></pre>\n";
        } else {
            out_ += code;
            if (!code.empty() && code.back() != '\n') {
                out_.push_back('\n');
            }
        }
    }

    void emit_indented_code(const std::vector<std::string>& lines, size_t& index) {
        std::string code;
        while (index < lines.size() && (is_indented_code_line(lines[index]) || trim(lines[index]).empty())) {
            code += trim(lines[index]).empty() ? std::string() : remove_code_indent(lines[index]);
            code.push_back('\n');
            ++index;
        }
        if (mode_ == RenderMode::Html) {
            out_ += "<pre><code>" + escape_html_text(code) + "</code></pre>\n";
        } else {
            out_ += code;
        }
    }

    void emit_table(const std::vector<std::string>& lines, size_t& index) {
        const std::vector<std::string> headers = split_table_row(lines[index]);
        index += 2;
        if (mode_ == RenderMode::Html) {
            out_ += "<table>\n<thead><tr>";
            for (const std::string& cell : headers) {
                out_ += "<th>" + render_inline(cell, mode_) + "</th>";
            }
            out_ += "</tr></thead>\n<tbody>\n";
            while (index < lines.size() && lines[index].find('|') != std::string::npos &&
                   !trim(lines[index]).empty()) {
                const std::vector<std::string> cells = split_table_row(lines[index]);
                out_ += "<tr>";
                for (const std::string& cell : cells) {
                    out_ += "<td>" + render_inline(cell, mode_) + "</td>";
                }
                out_ += "</tr>\n";
                ++index;
            }
            out_ += "</tbody>\n</table>\n";
        } else {
            append_plain_line(out_, plain_cells(headers));
            while (index < lines.size() && lines[index].find('|') != std::string::npos &&
                   !trim(lines[index]).empty()) {
                append_plain_line(out_, plain_cells(split_table_row(lines[index])));
                ++index;
            }
        }
    }

    std::string plain_cells(const std::vector<std::string>& cells) {
        std::string out;
        for (size_t i = 0; i < cells.size(); ++i) {
            if (i != 0) {
                out.push_back('\t');
            }
            out += render_inline(cells[i], mode_);
        }
        return out;
    }

    void emit_list_item(const ListMarker& marker, const std::string& text) {
        if (mode_ == RenderMode::Plaintext) {
            const size_t level = marker.indent / 2;
            out_ += std::string(level * 2, ' ');
            append_plain_line(out_, render_inline(trim(text), mode_));
            return;
        }

        while (!lists_.empty() && marker.indent < lists_.back().indent) {
            close_one_list();
        }
        if (!lists_.empty() && marker.indent == lists_.back().indent &&
            marker.ordered != lists_.back().ordered) {
            close_one_list();
        }
        if (lists_.empty() || marker.indent > lists_.back().indent) {
            out_ += marker.ordered ? "<ol>\n" : "<ul>\n";
            lists_.push_back({marker.indent, marker.ordered, false});
        }
        if (lists_.back().li_open) {
            out_ += "</li>\n";
            lists_.back().li_open = false;
        }
        out_ += "<li>" + render_inline(trim(text), mode_);
        lists_.back().li_open = true;
    }

    void emit_raw_html(const std::string& line) {
        if (mode_ == RenderMode::Html) {
            out_ += trim(line) + "\n";
        } else {
            const std::string text = render_inline(line, mode_);
            if (!trim(text).empty()) {
                append_plain_line(out_, text);
            }
        }
    }

    struct ParagraphPart {
        std::string text;
        bool hard_break_after = false;
    };

    void emit_paragraph(const std::vector<std::string>& lines, size_t& index) {
        std::vector<ParagraphPart> parts;
        while (index < lines.size()) {
            const std::string stripped = trim(lines[index]);
            if (stripped.empty()) {
                break;
            }
            std::string fence;
            std::string info;
            int header_level = 0;
            std::string header_text;
            ListMarker list;
            std::string blockquote_text;
            if (parse_fence_open(lines[index], fence, info) || is_indented_code_line(lines[index]) ||
                parse_header(lines[index], header_level, header_text) || is_horizontal_rule(lines[index]) ||
                parse_blockquote_marker(lines[index], blockquote_text) || is_table_start(lines, index) ||
                parse_list_marker(lines[index], list) || is_raw_html_block(lines[index])) {
                break;
            }
            ParagraphPart part;
            part.text = strip_hard_break_suffix(lines[index]);
            part.hard_break_after = line_has_hard_break(lines[index]);
            parts.push_back(std::move(part));
            ++index;
        }
        if (parts.empty()) {
            return;
        }
        if (mode_ == RenderMode::Html) {
            std::string paragraph;
            for (size_t part_index = 0; part_index < parts.size(); ++part_index) {
                if (part_index != 0) {
                    if (parts[part_index - 1].hard_break_after) {
                        paragraph += "<br>";
                    } else {
                        paragraph.push_back(' ');
                    }
                }
                paragraph += render_inline(parts[part_index].text, mode_);
            }
            out_ += "<p>" + paragraph + "</p>\n";
        } else {
            for (const ParagraphPart& part : parts) {
                append_plain_line(out_, render_inline(part.text, mode_));
            }
        }
    }

    void close_one_list() {
        if (lists_.empty()) {
            return;
        }
        if (lists_.back().li_open) {
            out_ += "</li>\n";
        }
        out_ += lists_.back().ordered ? "</ol>\n" : "</ul>\n";
        lists_.pop_back();
    }

    void close_lists() {
        while (!lists_.empty()) {
            close_one_list();
        }
    }

    void trim_trailing_blank_lines() {
        while (!out_.empty() && (out_.back() == '\n' || out_.back() == ' ' || out_.back() == '\t')) {
            out_.pop_back();
        }
    }
};

std::string render_html_document(const std::string& markdown) {
    std::string body = to_html_fragment(markdown);
    if (!body.empty() && body.back() != '\n') {
        body.push_back('\n');
    }
    return std::string("<!doctype html>\n"
                       "<html lang=\"en\">\n"
                       "<head>\n"
                       "  <meta charset=\"utf-8\">\n"
                       "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
                       "  <title>pkchat ") +
           kVersion +
           " output</title>\n"
                       "</head>\n"
                       "<body>\n" +
           body +
           "</body>\n"
           "</html>\n";
}

}  // namespace

bool parse_output_format(const std::string& text, OutputFormat& out) {
    const std::string normalized = lower_ascii(text);
    if (normalized == "plaintext" || normalized == "plain" || normalized == "text") {
        out = OutputFormat::Plaintext;
        return true;
    }
    if (normalized == "md" || normalized == "markdown") {
        out = OutputFormat::Markdown;
        return true;
    }
    if (normalized == "html" || normalized == "htm") {
        out = OutputFormat::Html;
        return true;
    }
    return false;
}

const char* output_format_name(OutputFormat format) {
    switch (format) {
        case OutputFormat::Plaintext:
            return "plaintext";
        case OutputFormat::Markdown:
            return "md";
        case OutputFormat::Html:
            return "html";
    }
    return "md";
}

std::string to_html_fragment(const std::string& markdown) {
    return BlockRenderer(RenderMode::Html).render(markdown);
}

std::string to_html_document(const std::string& markdown) {
    return render_html_document(markdown);
}

std::string to_plaintext(const std::string& markdown) {
    return BlockRenderer(RenderMode::Plaintext).render(markdown);
}

std::string render(const std::string& markdown, OutputFormat format, bool complete_html_document) {
    switch (format) {
        case OutputFormat::Plaintext:
            return to_plaintext(markdown);
        case OutputFormat::Markdown:
            return markdown;
        case OutputFormat::Html:
            return complete_html_document ? to_html_document(markdown) : to_html_fragment(markdown);
    }
    return markdown;
}

}  // namespace pkchat::markdown
