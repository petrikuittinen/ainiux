#include "editor/editor.hpp"

#include "ui/text_selector.hpp"

#include <sstream>

namespace ainiux::editor {

std::string editor_buffer_display_name(const EditorState& state, size_t index) {
    if (!state.path.empty()) {
        return state.path;
    }
    return "[scratch " + std::to_string(index + 1) + "]";
}

std::string editor_buffer_list_text(const std::vector<EditorState>& buffers, size_t selected) {
    ui::TextSelectorConfig config;
    config.header = ui::kTextSelectorBufferHint;
    config.selected_prefix = ui::kTextSelectorCaretPrefix;
    return ui::render_text_selector(config, selected, buffers.size(), [&](size_t index) {
        const EditorState& buffer = buffers[index];
        std::ostringstream out;
        out << editor_buffer_display_name(buffer, index);
        if (buffer.read_only) {
            out << " [RO]";
        }
        if (buffer.dirty) {
            out << " *";
        }
        const size_t line = buffer.text.line_for_offset(buffer.cursor) + 1;
        const size_t column =
            buffer.text.display_column_for_offset(buffer.cursor, buffer.tab_width) + 1;
        out << " - Ln " << line << ", Col " << column;
        return out.str();
    });
}

size_t move_editor_buffer_selection(size_t selected, size_t count, MovementKey key) {
    return ui::move_text_selector_selection(selected, count, key);
}

}  // namespace ainiux::editor
