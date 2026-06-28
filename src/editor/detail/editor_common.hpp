#pragma once

#include <string>

#include "editor/editor.hpp"

namespace pkchat::editor {

void update_preferred_column(EditorState& state);
std::string trim_ascii_copy(std::string text);

namespace detail {

size_t selection_end_exclusive_for(const Selection& selection, const PieceTable& text, size_t cursor);

}  // namespace detail

}  // namespace pkchat::editor