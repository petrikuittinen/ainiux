#pragma once

#include <string>
#include <vector>

#include "editor/editor.hpp"

namespace ainiux::editor {

void update_preferred_column(EditorState& state);
std::string trim_ascii_copy(std::string text);

namespace detail {

size_t selection_end_exclusive_for(const Selection& selection, const PieceTable& text, size_t cursor);
std::string longest_common_prefix(const std::vector<std::string>& values,
                                  bool case_sensitive = true);

}  // namespace detail

}  // namespace ainiux::editor
