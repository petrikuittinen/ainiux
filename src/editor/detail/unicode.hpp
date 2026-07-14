#pragma once

#include <cstddef>
#include <string>

namespace pkchat::editor::detail {

size_t next_grapheme_offset(const std::string& text, size_t pos);
size_t previous_grapheme_offset(const std::string& text, size_t pos);
size_t display_width_at(const std::string& text, size_t pos, size_t column, size_t tab_width = 4);
size_t display_column_for_text(const std::string& text, size_t byte_offset, size_t tab_width = 4);
size_t byte_offset_for_display_column(const std::string& text,
                                      size_t target_column,
                                      size_t tab_width = 4);

}  // namespace pkchat::editor::detail
