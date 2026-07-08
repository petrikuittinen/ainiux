#include "editor/editor.hpp"

#include <algorithm>
#include <sstream>

namespace pkchat::editor {

std::string editor_buffer_display_name(const EditorState& state, size_t index) {
    if (!state.path.empty()) {
        return state.path;
    }
    return "[scratch " + std::to_string(index + 1) + "]";
}

std::string editor_buffer_list_text(const std::vector<EditorState>& buffers, size_t selected) {
    std::ostringstream out;
    out << "Buffers - Enter opens - N new - Esc cancels\n";
    for (size_t i = 0; i < buffers.size(); ++i) {
        const EditorState& buffer = buffers[i];
        out << (i == selected ? "> " : "  ");
        out << editor_buffer_display_name(buffer, i);
        if (buffer.dirty) {
            out << " *";
        }
        const size_t line = buffer.text.line_for_offset(buffer.cursor) + 1;
        const size_t column = buffer.text.display_column_for_offset(buffer.cursor) + 1;
        out << " - Ln " << line << ", Col " << column;
        if (i + 1 != buffers.size()) {
            out << "\n";
        }
    }
    return out.str();
}

size_t move_editor_buffer_selection(size_t selected, size_t count, MovementKey key) {
    if (count == 0) {
        return 0;
    }
    selected = std::min(selected, count - 1);
    switch (key) {
        case MovementKey::Up:
            return selected == 0 ? 0 : selected - 1;
        case MovementKey::Down:
            return std::min(count - 1, selected + 1);
        case MovementKey::PageUp:
            return selected > 10 ? selected - 10 : 0;
        case MovementKey::PageDown:
            return std::min(count - 1, selected + 10);
        case MovementKey::Home:
            return 0;
        case MovementKey::End:
            return count - 1;
        case MovementKey::Left:
        case MovementKey::Right:
            return selected;
    }
    return selected;
}

}  // namespace pkchat::editor
