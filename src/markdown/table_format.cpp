#include "markdown/table_format.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
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

bool locale_looks_utf8() {
    const char* keys[] = {"LC_ALL", "LC_CTYPE", "LANG"};
    for (const char* key : keys) {
        const char* value = std::getenv(key);
        if (value == nullptr || value[0] == '\0') {
            continue;
        }
        std::string lower(value);
        for (char& ch : lower) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        if (lower == "c" || lower == "posix") {
            return false;
        }
        if (lower.find("utf-8") != std::string::npos || lower.find("utf8") != std::string::npos) {
            return true;
        }
        // Non-empty locale that is not explicitly C/POSIX: prefer UTF-8 tables.
        return true;
    }
    // No locale env: modern terminals are UTF-8.
    return true;
}

// Decode one UTF-8 sequence; returns length and codepoint (replacement on error).
void decode_utf8(const std::string& text, size_t pos, size_t& length, uint32_t& codepoint) {
    length = 1;
    codepoint = 0xFFFD;
    if (pos >= text.size()) {
        return;
    }
    const unsigned char c0 = static_cast<unsigned char>(text[pos]);
    if (c0 < 0x80U) {
        codepoint = c0;
        length = 1;
        return;
    }
    if ((c0 & 0xE0U) == 0xC0U && pos + 1 < text.size()) {
        const unsigned char c1 = static_cast<unsigned char>(text[pos + 1]);
        if ((c1 & 0xC0U) == 0x80U) {
            codepoint = (static_cast<uint32_t>(c0 & 0x1FU) << 6) | (c1 & 0x3FU);
            length = 2;
            return;
        }
    }
    if ((c0 & 0xF0U) == 0xE0U && pos + 2 < text.size()) {
        const unsigned char c1 = static_cast<unsigned char>(text[pos + 1]);
        const unsigned char c2 = static_cast<unsigned char>(text[pos + 2]);
        if ((c1 & 0xC0U) == 0x80U && (c2 & 0xC0U) == 0x80U) {
            codepoint = (static_cast<uint32_t>(c0 & 0x0FU) << 12) |
                        (static_cast<uint32_t>(c1 & 0x3FU) << 6) | (c2 & 0x3FU);
            length = 3;
            return;
        }
    }
    if ((c0 & 0xF8U) == 0xF0U && pos + 3 < text.size()) {
        const unsigned char c1 = static_cast<unsigned char>(text[pos + 1]);
        const unsigned char c2 = static_cast<unsigned char>(text[pos + 2]);
        const unsigned char c3 = static_cast<unsigned char>(text[pos + 3]);
        if ((c1 & 0xC0U) == 0x80U && (c2 & 0xC0U) == 0x80U && (c3 & 0xC0U) == 0x80U) {
            codepoint = (static_cast<uint32_t>(c0 & 0x07U) << 18) |
                        (static_cast<uint32_t>(c1 & 0x3FU) << 12) |
                        (static_cast<uint32_t>(c2 & 0x3FU) << 6) | (c3 & 0x3FU);
            length = 4;
            return;
        }
    }
    codepoint = c0;
    length = 1;
}

bool is_wide_codepoint(uint32_t cp) {
    // Simplified East-Asian-wide / fullwidth ranges (not full UAX #11).
    if (cp >= 0x1100 && cp <= 0x115F) return true;
    if (cp >= 0x2329 && cp <= 0x232A) return true;
    if (cp >= 0x2E80 && cp <= 0xA4CF) return true;
    if (cp >= 0xAC00 && cp <= 0xD7A3) return true;
    if (cp >= 0xF900 && cp <= 0xFAFF) return true;
    if (cp >= 0xFE10 && cp <= 0xFE19) return true;
    if (cp >= 0xFE30 && cp <= 0xFE6F) return true;
    if (cp >= 0xFF00 && cp <= 0xFF60) return true;
    if (cp >= 0xFFE0 && cp <= 0xFFE6) return true;
    if (cp >= 0x1F300 && cp <= 0x1F9FF) return true;
    if (cp >= 0x20000 && cp <= 0x3FFFD) return true;
    return false;
}

bool is_zero_width(uint32_t cp) {
    if (cp == 0x200D || cp == 0x200B || cp == 0x200C || cp == 0xFEFF) return true;
    if (cp >= 0x0300 && cp <= 0x036F) return true;
    if (cp >= 0x1AB0 && cp <= 0x1AFF) return true;
    if (cp >= 0x1DC0 && cp <= 0x1DFF) return true;
    if (cp >= 0x20D0 && cp <= 0x20FF) return true;
    if (cp >= 0xFE00 && cp <= 0xFE0F) return true;
    if (cp >= 0xFE20 && cp <= 0xFE2F) return true;
    if (cp >= 0xE0100 && cp <= 0xE01EF) return true;
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
    if (cell.empty()) {
        return false;
    }
    bool left = false;
    bool right = false;
    if (!cell.empty() && cell.front() == ':') {
        left = true;
        cell.erase(cell.begin());
    }
    if (!cell.empty() && cell.back() == ':') {
        right = true;
        cell.pop_back();
    }
    (void)left;
    (void)right;
    if (cell.size() < 3) {
        return false;
    }
    return std::all_of(cell.begin(), cell.end(), [](char ch) { return ch == '-'; });
}

TableAlign align_from_separator_cell(std::string cell) {
    cell = trim(std::move(cell));
    bool left = false;
    bool right = false;
    if (!cell.empty() && cell.front() == ':') {
        left = true;
        cell.erase(cell.begin());
    }
    if (!cell.empty() && cell.back() == ':') {
        right = true;
    }
    if (left && right) {
        return TableAlign::Center;
    }
    if (right) {
        return TableAlign::Right;
    }
    return TableAlign::Left;
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

bool is_table_body_line(const std::string& line) {
    const std::string trimmed = trim(line);
    if (trimmed.empty()) {
        return false;
    }
    if (trimmed.find('|') == std::string::npos) {
        return false;
    }
    // Do not treat a following table's separator as body of this table.
    if (is_table_separator_line(trimmed)) {
        return false;
    }
    return true;
}

bool parse_fence_open(const std::string& line, std::string& fence) {
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
    return true;
}

bool is_fence_close(const std::string& line, const std::string& fence) {
    const std::string left = ltrim(line);
    if (!starts_with(left, fence)) {
        return false;
    }
    return trim(left.substr(fence.size())).empty();
}

std::vector<std::string> split_lines_keep_last_empty(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t nl = text.find('\n', start);
        if (nl == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, nl - start));
        start = nl + 1;
        if (start == text.size()) {
            lines.push_back("");
            break;
        }
    }
    if (lines.empty()) {
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

std::string pad_cell(const std::string& cell, size_t width, TableAlign align) {
    const size_t cell_width = table_display_width(cell);
    if (cell_width >= width) {
        return cell;
    }
    const size_t pad = width - cell_width;
    size_t left = 0;
    size_t right = pad;
    if (align == TableAlign::Right) {
        left = pad;
        right = 0;
    } else if (align == TableAlign::Center) {
        left = pad / 2;
        right = pad - left;
    }
    return std::string(left, ' ') + cell + std::string(right, ' ');
}

std::string gfm_separator_cell(size_t width, TableAlign align) {
    const size_t w = std::max<size_t>(3, width);
    if (align == TableAlign::Center) {
        return ":" + std::string(w >= 2 ? w - 2 : 1, '-') + ":";
    }
    if (align == TableAlign::Right) {
        return std::string(w >= 1 ? w - 1 : 2, '-') + ":";
    }
    return std::string(w, '-');
}

std::string format_gfm_row(const std::vector<std::string>& cells,
                           const std::vector<size_t>& widths,
                           const std::vector<TableAlign>& aligns) {
    std::string line = "|";
    for (size_t i = 0; i < widths.size(); ++i) {
        const std::string cell = i < cells.size() ? cells[i] : "";
        const TableAlign align = i < aligns.size() ? aligns[i] : TableAlign::Left;
        line += " " + pad_cell(cell, widths[i], align) + " |";
    }
    return line;
}

std::string format_gfm_separator(const std::vector<size_t>& widths,
                                 const std::vector<TableAlign>& aligns) {
    std::string line = "|";
    for (size_t i = 0; i < widths.size(); ++i) {
        const TableAlign align = i < aligns.size() ? aligns[i] : TableAlign::Left;
        line += " " + gfm_separator_cell(widths[i], align) + " |";
    }
    return line;
}

std::string repeat_utf8(const char* unit, size_t count) {
    std::string out;
    out.reserve(count * 3);
    for (size_t i = 0; i < count; ++i) {
        out += unit;
    }
    return out;
}

std::string horizontal_rule(size_t width, const char* fill) {
    return repeat_utf8(fill, width);
}

std::string format_unicode_row(const std::vector<std::string>& cells,
                               const std::vector<size_t>& widths,
                               const std::vector<TableAlign>& aligns,
                               const char* left,
                               const char* mid,
                               const char* right) {
    std::string line = left;
    for (size_t i = 0; i < widths.size(); ++i) {
        if (i != 0) {
            line += mid;
        }
        const std::string cell = i < cells.size() ? cells[i] : "";
        const TableAlign align = i < aligns.size() ? aligns[i] : TableAlign::Left;
        line += " " + pad_cell(cell, widths[i], align) + " ";
    }
    line += right;
    return line;
}

std::string format_unicode_rule(const std::vector<size_t>& widths,
                                const char* left,
                                const char* mid,
                                const char* right,
                                const char* fill) {
    std::string line = left;
    for (size_t i = 0; i < widths.size(); ++i) {
        if (i != 0) {
            line += mid;
        }
        // two padding spaces around cell content
        line += horizontal_rule(widths[i] + 2, fill);
    }
    line += right;
    return line;
}

void compute_widths(const std::vector<std::string>& headers,
                    const std::vector<std::vector<std::string>>& body_rows,
                    size_t columns,
                    std::vector<size_t>& widths) {
    widths.assign(columns, 3);
    for (size_t i = 0; i < columns; ++i) {
        if (i < headers.size()) {
            widths[i] = std::max(widths[i], table_display_width(headers[i]));
        }
    }
    for (const auto& row : body_rows) {
        for (size_t i = 0; i < columns; ++i) {
            if (i < row.size()) {
                widths[i] = std::max(widths[i], table_display_width(row[i]));
            }
        }
    }
}

std::vector<TableAlign> normalize_aligns(const std::vector<TableAlign>& aligns, size_t columns) {
    std::vector<TableAlign> out(columns, TableAlign::Left);
    for (size_t i = 0; i < columns && i < aligns.size(); ++i) {
        out[i] = aligns[i];
    }
    return out;
}

std::vector<std::string> normalize_row(const std::vector<std::string>& row, size_t columns) {
    std::vector<std::string> out(columns);
    for (size_t i = 0; i < columns; ++i) {
        out[i] = i < row.size() ? row[i] : "";
    }
    return out;
}

}  // namespace

TableStyle default_table_style() {
    return locale_looks_utf8() ? TableStyle::UnicodeBox : TableStyle::PaddedGfm;
}

std::size_t table_display_width(const std::string& text) {
    size_t width = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        if (text[pos] == '\n' || text[pos] == '\r') {
            break;
        }
        size_t len = 1;
        uint32_t cp = 0;
        decode_utf8(text, pos, len, cp);
        if (is_zero_width(cp)) {
            // no width
        } else if (is_wide_codepoint(cp)) {
            width += 2;
        } else {
            width += 1;
        }
        pos += len;
    }
    return width;
}

std::string format_table(const std::vector<std::string>& headers,
                         const std::vector<TableAlign>& aligns,
                         const std::vector<std::vector<std::string>>& body_rows,
                         TableStyle style) {
    size_t columns = headers.size();
    for (const auto& row : body_rows) {
        columns = std::max(columns, row.size());
    }
    if (columns == 0) {
        return {};
    }

    const std::vector<std::string> head = normalize_row(headers, columns);
    const std::vector<TableAlign> use_aligns = normalize_aligns(aligns, columns);
    std::vector<size_t> widths;
    compute_widths(head, body_rows, columns, widths);

    std::vector<std::string> lines;
    if (style == TableStyle::PaddedGfm) {
        lines.push_back(format_gfm_row(head, widths, use_aligns));
        lines.push_back(format_gfm_separator(widths, use_aligns));
        for (const auto& row : body_rows) {
            lines.push_back(format_gfm_row(normalize_row(row, columns), widths, use_aligns));
        }
    } else {
        // Unicode box
        lines.push_back(format_unicode_rule(widths, u8"┌", u8"┬", u8"┐", u8"─"));
        lines.push_back(format_unicode_row(head, widths, use_aligns, u8"│", u8"│", u8"│"));
        lines.push_back(format_unicode_rule(widths, u8"├", u8"┼", u8"┤", u8"─"));
        for (const auto& row : body_rows) {
            lines.push_back(
                format_unicode_row(normalize_row(row, columns), widths, use_aligns, u8"│", u8"│",
                                   u8"│"));
        }
        lines.push_back(format_unicode_rule(widths, u8"└", u8"┴", u8"┘", u8"─"));
    }

    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) {
            out.push_back('\n');
        }
        out += lines[i];
    }
    out.push_back('\n');
    return out;
}

std::string pretty_format_tables(const std::string& text, const TableFormatOptions& options) {
    if (text.empty()) {
        return text;
    }
    const bool had_trailing_newline = !text.empty() && text.back() == '\n';
    std::vector<std::string> lines = split_lines_keep_last_empty(text);
    // Drop artificial trailing empty from a final newline so join can restore it.
    if (had_trailing_newline && !lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }

    std::vector<std::string> out_lines;
    out_lines.reserve(lines.size());
    std::string open_fence;
    size_t i = 0;
    while (i < lines.size()) {
        if (!open_fence.empty()) {
            out_lines.push_back(lines[i]);
            if (is_fence_close(lines[i], open_fence)) {
                open_fence.clear();
            }
            ++i;
            continue;
        }

        std::string fence;
        if (parse_fence_open(lines[i], fence)) {
            open_fence = fence;
            out_lines.push_back(lines[i]);
            ++i;
            continue;
        }

        if (is_table_start(lines, i)) {
            const std::vector<std::string> headers = split_table_row(lines[i]);
            const std::vector<std::string> sep_cells = split_table_row(lines[i + 1]);
            std::vector<TableAlign> aligns;
            aligns.reserve(sep_cells.size());
            for (const std::string& cell : sep_cells) {
                aligns.push_back(align_from_separator_cell(cell));
            }
            size_t j = i + 2;
            std::vector<std::vector<std::string>> body;
            while (j < lines.size() && is_table_body_line(lines[j])) {
                body.push_back(split_table_row(lines[j]));
                ++j;
            }
            // Incomplete: only header so far (should not happen with is_table_start).
            // Open table at end of text: reformat if allowed.
            const bool closed = j < lines.size();
            if (!closed && !options.reformat_open_tables) {
                for (size_t k = i; k < j; ++k) {
                    out_lines.push_back(lines[k]);
                }
            } else {
                const std::string formatted =
                    format_table(headers, aligns, body, options.style);
                std::vector<std::string> formatted_lines = split_lines_keep_last_empty(formatted);
                if (!formatted_lines.empty() && formatted_lines.back().empty()) {
                    formatted_lines.pop_back();
                }
                for (std::string& fl : formatted_lines) {
                    out_lines.push_back(std::move(fl));
                }
            }
            i = j;
            continue;
        }

        out_lines.push_back(lines[i]);
        ++i;
    }

    return join_lines(out_lines, had_trailing_newline);
}

std::string pretty_format_tables(const std::string& text) {
    TableFormatOptions options;
    options.style = default_table_style();
    options.reformat_open_tables = true;
    return pretty_format_tables(text, options);
}

}  // namespace ainiux::markdown
