#include "editor/editor.hpp"
#include "editor/detail/editor_common.hpp"
#include "editor/detail/wrap.hpp"

#include <algorithm>
#include <optional>
#include <vector>

namespace pkchat::editor {

using detail::WrapSegment;
using detail::WrappedCursor;
using detail::cursor_in_wrapped_line;
using detail::display_range;
using detail::selection_end_exclusive_for;
using detail::wrap_line_segments;
using detail::wrapped_row_count;

RenderedPanel render_panel(const PieceTable& text,
                           const Rect& rect,
                           size_t cursor,
                           size_t scroll_line,
                           size_t scroll_column,
                           const std::optional<Selection>& selection,
                           highlight::Language language,
                           bool highlight_enabled,
                           highlight::DocumentCache* highlight_cache) {
    (void)scroll_column;
    RenderedPanel rendered;
    const size_t height = static_cast<size_t>(std::max(0, rect.height));
    const size_t width = static_cast<size_t>(std::max(0, rect.width));
    rendered.lines.reserve(height);
    rendered.line_spans.reserve(height);
    if (width == 0) {
        rendered.lines.resize(height);
        rendered.line_spans.resize(height);
        return rendered;
    }

    const bool highlight_selection = selection.has_value() && selection->has_range();
    const size_t sel_start = highlight_selection ? selection->start() : 0;
    const size_t sel_end = highlight_selection
                               ? selection_end_exclusive_for(*selection, text, cursor)
                               : 0;

    const size_t line_count = text.line_count();
    std::vector<std::string> source_lines;
    if (highlight_enabled && highlight_cache != nullptr && language != highlight::Language::Text) {
        source_lines.reserve(line_count);
        for (size_t source_line = 0; source_line < line_count; ++source_line) {
            source_lines.push_back(text.line_text(source_line));
        }
        highlight_cache->update(source_lines, language);
    }
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
            rendered.line_spans.push_back({});
            continue;
        }

        const std::string line_text_value = text.line_text(line);
        const std::vector<WrapSegment> segments = wrap_line_segments(line_text_value, width);
        const WrapSegment segment = segments[std::min(segment_index, segments.size() - 1)];
        const size_t global_line_start = text.line_start(line);
        rendered.lines.push_back(display_range(line_text_value, segment.start, segment.end, width));

        const highlight::HighlightedLine* highlighted = nullptr;
        if (highlight_enabled && highlight_cache != nullptr && language != highlight::Language::Text) {
            size_t budget = highlight::kDefaultFrameBudgetBytes;
            if (highlight_cache->highlight_through(line, budget)) {
                highlighted = highlight_cache->line(line);
            }
        }

        std::vector<size_t> boundaries{segment.start, segment.end};
        if (highlighted != nullptr) {
            for (const highlight::Span& span : highlighted->spans) {
                const size_t start = std::max(segment.start, span.start);
                const size_t end = std::min(segment.end, span.end);
                if (start < end) {
                    boundaries.push_back(start);
                    boundaries.push_back(end);
                }
            }
        }
        if (highlight_selection) {
            const size_t local_selection_start =
                sel_start > global_line_start ? sel_start - global_line_start : 0;
            const size_t local_selection_end =
                sel_end > global_line_start ? sel_end - global_line_start : 0;
            const size_t start = std::max(segment.start, local_selection_start);
            const size_t end = std::min(segment.end, local_selection_end);
            if (start < end) {
                boundaries.push_back(start);
                boundaries.push_back(end);
            }
        }
        std::sort(boundaries.begin(), boundaries.end());
        boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

        std::vector<RenderedPanel::Span> rendered_spans;
        auto displayed_byte_offset = [&](size_t source_offset) {
            const size_t cells = detail::display_width_for_range(
                line_text_value, segment.start, std::min(source_offset, segment.end));
            return display_range(line_text_value,
                                 segment.start,
                                 std::min(source_offset, segment.end),
                                 cells)
                .size();
        };
        for (size_t boundary = 0; boundary + 1 < boundaries.size(); ++boundary) {
            const size_t source_start = boundaries[boundary];
            const size_t source_end = boundaries[boundary + 1];
            if (source_start >= source_end) {
                continue;
            }
            bool has_syntax = false;
            highlight::TokenRole role = highlight::TokenRole::Operator;
            if (highlighted != nullptr) {
                for (const highlight::Span& span : highlighted->spans) {
                    if (span.start <= source_start && source_start < span.end) {
                        has_syntax = true;
                        role = span.role;
                        break;
                    }
                }
            }
            const size_t global_offset = global_line_start + source_start;
            const bool selected = highlight_selection && global_offset >= sel_start && global_offset < sel_end;
            if (!has_syntax && !selected) {
                continue;
            }
            RenderedPanel::Span output_span;
            output_span.start = displayed_byte_offset(source_start);
            output_span.end = displayed_byte_offset(source_end);
            output_span.role = role;
            output_span.syntax = has_syntax;
            output_span.selected = selected;
            if (output_span.start < output_span.end) {
                rendered_spans.push_back(output_span);
            }
        }
        rendered.line_spans.push_back(std::move(rendered_spans));

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
