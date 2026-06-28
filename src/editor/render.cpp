#include "editor/editor.hpp"
#include "editor/detail/editor_common.hpp"
#include "editor/detail/wrap.hpp"

#include <optional>
#include <vector>

namespace pkchat::editor {

using detail::WrapSegment;
using detail::WrappedCursor;
using detail::cursor_in_wrapped_line;
using detail::display_range;
using detail::display_range_highlighted;
using detail::selection_end_exclusive_for;
using detail::wrap_line_segments;
using detail::wrapped_row_count;

RenderedPanel render_panel(const PieceTable& text,
                           const Rect& rect,
                           size_t cursor,
                           size_t scroll_line,
                           size_t scroll_column,
                           const std::optional<Selection>& selection) {
    (void)scroll_column;
    RenderedPanel rendered;
    const size_t height = static_cast<size_t>(std::max(0, rect.height));
    const size_t width = static_cast<size_t>(std::max(0, rect.width));
    rendered.lines.reserve(height);
    if (width == 0) {
        rendered.lines.resize(height);
        return rendered;
    }

    const bool highlight_selection = selection.has_value() && selection->has_range();
    const size_t sel_start = highlight_selection ? selection->start() : 0;
    const size_t sel_end = highlight_selection
                               ? selection_end_exclusive_for(*selection, text, cursor)
                               : 0;

    const size_t line_count = text.line_count();
    size_t line = 0;
    size_t segment_index = 0;
    size_t rows_to_skip = scroll_line;
    while (line < line_count) {
        const std::string line_text_value = text.line_text(line);
        const size_t rows = wrapped_row_count(line_text_value, width);
        if (rows_to_skip < rows) {
            segment_index = rows_to_skip;
            break;
        }
        rows_to_skip -= rows;
        ++line;
    }

    for (size_t row = 0; row < height; ++row) {
        if (line >= line_count) {
            rendered.lines.push_back(std::string(width, ' '));
            continue;
        }

        const std::string line_text_value = text.line_text(line);
        const std::vector<WrapSegment> segments = wrap_line_segments(line_text_value, width);
        const WrapSegment segment = segments[std::min(segment_index, segments.size() - 1)];
        const size_t global_line_start = text.line_start(line);
        if (highlight_selection) {
            rendered.lines.push_back(display_range_highlighted(line_text_value,
                                                              segment.start,
                                                              segment.end,
                                                              width,
                                                              global_line_start,
                                                              sel_start,
                                                              sel_end,
                                                              true));
        } else {
            rendered.lines.push_back(display_range(line_text_value, segment.start, segment.end, width));
        }

        ++segment_index;
        if (segment_index >= segments.size()) {
            segment_index = 0;
            ++line;
        }
    }

    const size_t cursor_line = text.line_for_offset(cursor);
    size_t cursor_row = 0;
    for (size_t i = 0; i < cursor_line; ++i) {
        cursor_row += wrapped_row_count(text.line_text(i), width);
    }
    const size_t cursor_line_start = text.line_start(cursor_line);
    const std::string cursor_line_text = text.line_text(cursor_line);
    const WrappedCursor wrapped_cursor = cursor_in_wrapped_line(cursor_line_text, cursor - cursor_line_start, width);
    cursor_row += wrapped_cursor.row;
    if (cursor_row >= scroll_line && cursor_row < scroll_line + height) {
        rendered.cursor.row = static_cast<int>(cursor_row - scroll_line);
        rendered.cursor.col = static_cast<int>(wrapped_cursor.col);
        rendered.cursor.visible = true;
    }
    return rendered;
}


}  // namespace pkchat::editor
