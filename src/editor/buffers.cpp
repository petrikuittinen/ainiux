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

namespace {

std::string buffer_row_label(const std::vector<EditorState>& buffers, size_t underlying) {
    if (underlying >= buffers.size()) {
        return {};
    }
    const EditorState& buffer = buffers[underlying];
    std::ostringstream out;
    out << editor_buffer_display_name(buffer, underlying);
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
}

}  // namespace

std::string editor_buffer_list_text(const std::vector<EditorState>& buffers, size_t selected) {
    return editor_buffer_list_text(buffers, selected, {});
}

std::string editor_buffer_list_text(const std::vector<EditorState>& buffers,
                                    size_t selected,
                                    const std::vector<size_t>& display_order) {
    ui::TextSelectorConfig config;
    config.header = ui::kTextSelectorBufferHint;
    config.selected_prefix = ui::kTextSelectorCaretPrefix;
    return ui::render_text_selector(config, selected, buffers.size(), [&](size_t display_index) {
        const size_t underlying =
            ui::text_selector_underlying_index(display_order, display_index, buffers.size());
        return buffer_row_label(buffers, underlying);
    });
}

size_t move_editor_buffer_selection(size_t selected, size_t count, MovementKey key) {
    return ui::move_text_selector_selection(selected, count, key);
}

}  // namespace ainiux::editor
