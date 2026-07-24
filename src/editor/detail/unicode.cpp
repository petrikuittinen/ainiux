#include "editor/detail/unicode.hpp"
#include "editor/detail/wrap.hpp"
#include "editor/editor.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace ainiux::editor::detail {

struct DecodedChar {
    uint32_t codepoint = 0xFFFDU;
    size_t length = 1;
    bool valid = false;
};

bool is_continuation_byte(unsigned char ch) {
    return (ch & 0xC0U) == 0x80U;
}

DecodedChar decode_utf8_at(const std::string& text, size_t pos) {
    if (pos >= text.size()) {
        return {};
    }

    const unsigned char b0 = static_cast<unsigned char>(text[pos]);
    if (b0 < 0x80U) {
        return {b0, 1, true};
    }

    size_t length = 0;
    uint32_t codepoint = 0;
    uint32_t minimum = 0;
    if ((b0 & 0xE0U) == 0xC0U) {
        length = 2;
        codepoint = b0 & 0x1FU;
        minimum = 0x80U;
    } else if ((b0 & 0xF0U) == 0xE0U) {
        length = 3;
        codepoint = b0 & 0x0FU;
        minimum = 0x800U;
    } else if ((b0 & 0xF8U) == 0xF0U) {
        length = 4;
        codepoint = b0 & 0x07U;
        minimum = 0x10000U;
    } else {
        return {0xFFFDU, 1, false};
    }

    if (pos + length > text.size()) {
        return {0xFFFDU, 1, false};
    }
    for (size_t i = 1; i < length; ++i) {
        const unsigned char byte = static_cast<unsigned char>(text[pos + i]);
        if (!is_continuation_byte(byte)) {
            return {0xFFFDU, 1, false};
        }
        codepoint = (codepoint << 6U) | static_cast<uint32_t>(byte & 0x3FU);
    }

    if (codepoint < minimum || codepoint > 0x10FFFFU ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
        return {0xFFFDU, 1, false};
    }
    return {codepoint, length, true};
}

bool is_combining_mark(uint32_t codepoint) {
    return (codepoint >= 0x0300U && codepoint <= 0x036FU) ||
           (codepoint >= 0x0591U && codepoint <= 0x05BDU) ||
           codepoint == 0x05BFU ||
           (codepoint >= 0x05C1U && codepoint <= 0x05C2U) ||
           (codepoint >= 0x05C4U && codepoint <= 0x05C5U) ||
           codepoint == 0x05C7U ||
           (codepoint >= 0x0610U && codepoint <= 0x061AU) ||
           (codepoint >= 0x064BU && codepoint <= 0x065FU) ||
           codepoint == 0x0670U ||
           (codepoint >= 0x06D6U && codepoint <= 0x06DCU) ||
           (codepoint >= 0x06DFU && codepoint <= 0x06E4U) ||
           (codepoint >= 0x06E7U && codepoint <= 0x06E8U) ||
           (codepoint >= 0x06EAU && codepoint <= 0x06EDU) ||
           (codepoint >= 0x1AB0U && codepoint <= 0x1AFFU) ||
           (codepoint >= 0x1DC0U && codepoint <= 0x1DFFU) ||
           (codepoint >= 0x20D0U && codepoint <= 0x20FFU) ||
           (codepoint >= 0xFE20U && codepoint <= 0xFE2FU);
}

bool is_variation_selector(uint32_t codepoint) {
    return (codepoint >= 0xFE00U && codepoint <= 0xFE0FU) ||
           (codepoint >= 0xE0100U && codepoint <= 0xE01EFU);
}

bool is_emoji_modifier(uint32_t codepoint) {
    return codepoint >= 0x1F3FBU && codepoint <= 0x1F3FFU;
}

bool is_wide_codepoint(uint32_t codepoint) {
    return (codepoint >= 0x1100U && codepoint <= 0x115FU) ||
           (codepoint >= 0x2329U && codepoint <= 0x232AU) ||
           (codepoint >= 0x2E80U && codepoint <= 0xA4CFU) ||
           (codepoint >= 0xAC00U && codepoint <= 0xD7A3U) ||
           (codepoint >= 0xF900U && codepoint <= 0xFAFFU) ||
           (codepoint >= 0xFE10U && codepoint <= 0xFE19U) ||
           (codepoint >= 0xFE30U && codepoint <= 0xFE6FU) ||
           (codepoint >= 0xFF00U && codepoint <= 0xFF60U) ||
           (codepoint >= 0xFFE0U && codepoint <= 0xFFE6U) ||
           (codepoint >= 0x1F000U && codepoint <= 0x1FAFFU);
}

bool is_control_codepoint(uint32_t codepoint) {
    return codepoint < 0x20U || (codepoint >= 0x7FU && codepoint < 0xA0U);
}

size_t next_grapheme_offset(const std::string& text, size_t pos) {
    if (pos >= text.size()) {
        return text.size();
    }

    DecodedChar current = decode_utf8_at(text, pos);
    size_t next = pos + current.length;
    if (!current.valid) {
        return next;
    }

    while (next < text.size()) {
        const DecodedChar decoded = decode_utf8_at(text, next);
        if (!decoded.valid) {
            break;
        }
        if (is_combining_mark(decoded.codepoint) || is_variation_selector(decoded.codepoint) ||
            is_emoji_modifier(decoded.codepoint)) {
            next += decoded.length;
            current = decoded;
            continue;
        }
        if (decoded.codepoint == 0x200DU) {
            next += decoded.length;
            if (next < text.size()) {
                const DecodedChar joined = decode_utf8_at(text, next);
                if (joined.valid) {
                    next += joined.length;
                    current = joined;
                    continue;
                }
            }
            break;
        }
        break;
    }
    return next;
}

size_t previous_grapheme_offset(const std::string& text, size_t pos) {
    const size_t target = std::min(pos, text.size());
    size_t previous = 0;
    size_t current = 0;
    while (current < target) {
        previous = current;
        const size_t next = next_grapheme_offset(text, current);
        if (next <= current || next >= target) {
            return previous;
        }
        current = next;
    }
    return previous;
}

size_t display_width_at(const std::string& text, size_t pos, size_t column, size_t tab_width) {
    if (pos >= text.size()) {
        return 0;
    }
    const DecodedChar first = decode_utf8_at(text, pos);
    if (first.codepoint == '\t') {
        tab_width = std::max<size_t>(1, tab_width);
        return tab_width - (column % tab_width);
    }
    if (!first.valid || is_control_codepoint(first.codepoint)) {
        return 1;
    }

    const size_t end = next_grapheme_offset(text, pos);
    size_t width = 0;
    size_t scan = pos;
    while (scan < end) {
        const DecodedChar decoded = decode_utf8_at(text, scan);
        if (decoded.codepoint == '\t') {
            tab_width = std::max<size_t>(1, tab_width);
            return tab_width - (column % tab_width);
        }
        if (!decoded.valid || is_control_codepoint(decoded.codepoint)) {
            return 1;
        }
        if (is_wide_codepoint(decoded.codepoint)) {
            width = std::max<size_t>(width, 2);
        } else if (!is_combining_mark(decoded.codepoint) &&
                   !is_variation_selector(decoded.codepoint) &&
                   !is_emoji_modifier(decoded.codepoint) &&
                   decoded.codepoint != 0x200DU) {
            width = std::max<size_t>(width, 1);
        }
        scan += decoded.length;
    }
    return width;
}

size_t display_column_for_text(const std::string& text, size_t byte_offset, size_t tab_width) {
    size_t column = 0;
    size_t pos = 0;
    const size_t limit = std::min(byte_offset, text.size());
    while (pos < limit) {
        const size_t next = next_grapheme_offset(text, pos);
        if (next > limit) {
            break;
        }
        column += display_width_at(text, pos, column, tab_width);
        pos = next;
    }
    return column;
}

size_t byte_offset_for_display_column(const std::string& text,
                                      size_t target_column,
                                      size_t tab_width) {
    size_t column = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t width = display_width_at(text, pos, column, tab_width);
        if (column + width > target_column) {
            break;
        }
        column += width;
        pos = next_grapheme_offset(text, pos);
    }
    return pos;
}

size_t display_width_for_range(const std::string& text,
                               size_t start,
                               size_t end,
                               size_t tab_width) {
    size_t column = 0;
    size_t pos = start;
    const size_t limit = std::min(end, text.size());
    while (pos < limit) {
        const size_t next = next_grapheme_offset(text, pos);
        if (next > limit) {
            break;
        }
        column += display_width_at(text, pos, column, tab_width);
        pos = next;
    }
    return column;
}

std::vector<WrapSegment> wrap_line_segments(const std::string& text,
                                            size_t width,
                                            size_t tab_width) {
    width = std::max<size_t>(1, width);
    if (text.empty()) {
        return {{0, 0}};
    }

    std::vector<WrapSegment> segments;
    size_t start = 0;
    while (start < text.size()) {
        size_t pos = start;
        size_t column = 0;
        size_t last_break = std::string::npos;
        size_t hard_break = start;

        while (pos < text.size()) {
            const size_t grapheme_start = pos;
            const size_t grapheme_end = next_grapheme_offset(text, pos);
            const size_t char_width = display_width_at(text, pos, column, tab_width);
            if (column + char_width > width) {
                break;
            }
            column += char_width;
            pos = grapheme_end;
            hard_break = pos;
            const DecodedChar decoded = decode_utf8_at(text, grapheme_start);
            if (decoded.valid && (decoded.codepoint == ' ' || decoded.codepoint == '\t')) {
                last_break = pos;
            }
            if (column >= width) {
                break;
            }
        }

        size_t end = hard_break;
        if (pos < text.size() && last_break != std::string::npos && last_break > start) {
            end = last_break;
        }
        if (end <= start) {
            end = text.size();
            if (start < text.size()) {
                end = next_grapheme_offset(text, start);
            }
        }

        segments.push_back({start, end});
        start = end;
    }
    return segments;
}

size_t wrapped_row_count(const std::string& text, size_t width, size_t tab_width) {
    return wrap_line_segments(text, width, tab_width).size();
}

size_t wrapped_row_count_bounded(const std::string& text,
                                 size_t width,
                                 size_t limit,
                                 size_t tab_width) {
    if (limit == 0) return 0;
    width = std::max<size_t>(1, width);
    if (text.empty()) return 1;

    size_t rows = 0;
    size_t start = 0;
    while (start < text.size() && rows < limit) {
        size_t pos = start;
        size_t column = 0;
        size_t last_break = std::string::npos;
        size_t hard_break = start;
        while (pos < text.size()) {
            const size_t grapheme_start = pos;
            const size_t grapheme_end = next_grapheme_offset(text, pos);
            const size_t char_width = display_width_at(text, pos, column, tab_width);
            if (column + char_width > width) break;
            column += char_width;
            pos = grapheme_end;
            hard_break = pos;
            const DecodedChar decoded = decode_utf8_at(text, grapheme_start);
            if (decoded.valid && (decoded.codepoint == ' ' || decoded.codepoint == '\t')) {
                last_break = pos;
            }
            if (column >= width) break;
        }
        size_t end = hard_break;
        if (pos < text.size() && last_break != std::string::npos && last_break > start) {
            end = last_break;
        }
        if (end <= start) end = next_grapheme_offset(text, start);
        start = end;
        ++rows;
    }
    return rows;
}

WrappedCursor cursor_in_wrapped_line(const std::string& text,
                                     size_t byte_offset,
                                     size_t width,
                                     size_t tab_width) {
    width = std::max<size_t>(1, width);
    const std::vector<WrapSegment> segments = wrap_line_segments(text, width, tab_width);
    const size_t clamped = std::min(byte_offset, text.size());
    for (size_t i = 0; i < segments.size(); ++i) {
        const WrapSegment& segment = segments[i];
        if (clamped == segment.end && i + 1 < segments.size()) {
            continue;
        }
        if (clamped <= segment.end || i + 1 == segments.size()) {
            const size_t col = display_width_for_range(text, segment.start, clamped, tab_width);
            if (clamped == segment.end && col >= width) {
                return {i + 1, 0};
            }
            return {i, std::min(col, width - 1)};
        }
    }
    return {};
}

std::string display_range(const std::string& text,
                          size_t start,
                          size_t end,
                          size_t width,
                          size_t tab_width) {
    std::string out;
    size_t column = 0;
    size_t visible = 0;
    size_t pos = start;
    const size_t limit = std::min(end, text.size());
    while (pos < limit && visible < width) {
        const DecodedChar decoded = decode_utf8_at(text, pos);
        const size_t next = std::min(next_grapheme_offset(text, pos), limit);
        const size_t char_width = display_width_at(text, pos, column, tab_width);
        const size_t next_column = column + char_width;

        if (decoded.valid && decoded.codepoint == '\t') {
            for (size_t tab_col = column; tab_col < next_column && visible < width; ++tab_col) {
                out.push_back(' ');
                ++visible;
            }
        } else if (!decoded.valid || is_control_codepoint(decoded.codepoint)) {
            out.push_back('?');
            ++visible;
        } else {
            out.append(text, pos, next - pos);
            visible += char_width;
        }

        column = next_column;
        pos = next;
    }
    while (visible < width) {
        out.push_back(' ');
        ++visible;
    }
    return out;
}

std::string display_range_highlighted(const std::string& text,
                                      size_t start,
                                      size_t end,
                                      size_t width,
                                      size_t global_line_start,
                                      size_t sel_start,
                                      size_t sel_end,
                                      bool highlight_selection) {
    std::string out;
    size_t column = 0;
    size_t visible = 0;
    size_t pos = start;
    const size_t limit = std::min(end, text.size());
    bool highlight_on = false;
    while (pos < limit && visible < width) {
        const size_t global_offset = global_line_start + pos;
        const bool selected = highlight_selection && sel_start < sel_end &&
                              global_offset >= sel_start && global_offset < sel_end;
        if (selected != highlight_on) {
            out += selected ? "\x1b[7m" : "\x1b[0m";
            highlight_on = selected;
        }

        const DecodedChar decoded = decode_utf8_at(text, pos);
        const size_t next = std::min(next_grapheme_offset(text, pos), limit);
        const size_t char_width = display_width_at(text, pos, column);
        const size_t next_column = column + char_width;

        if (decoded.valid && decoded.codepoint == '\t') {
            for (size_t tab_col = column; tab_col < next_column && visible < width; ++tab_col) {
                out.push_back(' ');
                ++visible;
            }
        } else if (!decoded.valid || is_control_codepoint(decoded.codepoint)) {
            out.push_back('?');
            ++visible;
        } else {
            out.append(text, pos, next - pos);
            visible += char_width;
        }

        column = next_column;
        pos = next;
    }
    if (highlight_on) {
        out += "\x1b[0m";
    }
    while (visible < width) {
        out.push_back(' ');
        ++visible;
    }
    return out;
}

size_t byte_offset_for_range_column(const std::string& text,
                                    size_t start,
                                    size_t end,
                                    size_t target_column,
                                    size_t tab_width) {
    size_t column = 0;
    size_t pos = start;
    const size_t limit = std::min(end, text.size());
    while (pos < limit) {
        const size_t width = display_width_at(text, pos, column, tab_width);
        if (column + width > target_column) {
            break;
        }
        column += width;
        pos = next_grapheme_offset(text, pos);
    }
    return pos;
}

WrappedLocation wrapped_location_for_offset(const PieceTable& text,
                                            size_t offset,
                                            size_t width,
                                            size_t tab_width) {
    width = std::max<size_t>(1, width);
    const size_t line = text.line_for_offset(offset);
    const std::string line_text_value = text.line_text(line);
    const size_t line_start = text.line_start(line);
    const size_t local_offset = std::min(offset - line_start, line_text_value.size());
    const std::vector<WrapSegment> segments = wrap_line_segments(line_text_value, width, tab_width);

    for (size_t i = 0; i < segments.size(); ++i) {
        const WrapSegment& segment = segments[i];
        if (local_offset < segment.end || i + 1 == segments.size()) {
            return {line, i};
        }
        if (local_offset == segment.end && i + 1 < segments.size()) {
            continue;
        }
    }
    return {line, 0};
}

size_t offset_for_wrapped_location(const PieceTable& text,
                                   size_t line,
                                   size_t segment_index,
                                   size_t column,
                                   size_t width,
                                   size_t tab_width) {
    width = std::max<size_t>(1, width);
    const std::string line_text_value = text.line_text(line);
    const std::vector<WrapSegment> segments = wrap_line_segments(line_text_value, width, tab_width);
    const WrapSegment& segment = segments[std::min(segment_index, segments.size() - 1)];
    return text.line_start(line) +
           byte_offset_for_range_column(line_text_value,
                                        segment.start,
                                        segment.end,
                                        column,
                                        tab_width);
}

std::string pad_or_clip_ascii(const std::string& text, int width) {
    if (width <= 0) {
        return "";
    }
    std::string out = text.substr(0, static_cast<size_t>(width));
    while (out.size() < static_cast<size_t>(width)) {
        out.push_back(' ');
    }
    return out;
}

}  // namespace ainiux::editor::detail
