#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "editor/editor.hpp"

namespace ainiux::editor::detail {

struct WrapSegment {
    size_t start = 0;
    size_t end = 0;
};

struct WrappedCursor {
    size_t row = 0;
    size_t col = 0;
};

struct WrappedLocation {
    size_t line = 0;
    size_t segment = 0;
};

std::vector<WrapSegment> wrap_line_segments(const std::string& text,
                                            size_t width,
                                            size_t tab_width = 4);
size_t wrapped_row_count(const std::string& text, size_t width, size_t tab_width = 4);
WrappedCursor cursor_in_wrapped_line(const std::string& text,
                                     size_t byte_offset,
                                     size_t width,
                                     size_t tab_width = 4);
std::string display_range(const std::string& text,
                          size_t start,
                          size_t end,
                          size_t width,
                          size_t tab_width = 4);
std::string display_range_highlighted(const std::string& text,
                                      size_t start,
                                      size_t end,
                                      size_t width,
                                      size_t global_line_start,
                                      size_t sel_start,
                                      size_t sel_end,
                                      bool highlight_selection);
size_t byte_offset_for_range_column(const std::string& text,
                                    size_t start,
                                    size_t end,
                                    size_t target_column,
                                    size_t tab_width = 4);
size_t display_width_for_range(const std::string& text,
                               size_t start,
                               size_t end,
                               size_t tab_width = 4);
WrappedLocation wrapped_location_for_offset(const PieceTable& text,
                                            size_t offset,
                                            size_t width,
                                            size_t tab_width = 4);
size_t offset_for_wrapped_location(const PieceTable& text,
                                   size_t line,
                                   size_t segment_index,
                                   size_t column,
                                   size_t width,
                                   size_t tab_width = 4);
std::string pad_or_clip_ascii(const std::string& text, int width);

}  // namespace ainiux::editor::detail
