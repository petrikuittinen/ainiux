#include "editor/editor_picker.hpp"

#include "tui/input_handlers.hpp"
#include "ui/text_selector.hpp"

namespace pkchat::editor {

std::string EditorProviderModelPicker::selection_label() const {
    return for_provider ? "Selected provider" : "Selected model";
}

std::string EditorProviderModelPicker::cancel_message() const {
    return for_provider ? "Provider selection cancelled" : "Model selection cancelled";
}

std::string EditorProviderModelPicker::empty_message() const {
    return for_provider ? "No providers available" : "No models returned";
}

std::string EditorProviderModelPicker::status_message() const {
    return items.empty() ? empty_message()
                         : ui::text_selector_status(selection_label(), selected, items.size());
}

void EditorProviderModelPicker::refresh_view() {
    const std::string text = for_provider ? tui::provider_picker_text(items, selected)
                                          : tui::model_picker_text(items, selected);
    view = EditorState::from_text(text);
    view.path = for_provider ? "[providers]" : "[models]";
    const size_t selected_line = std::min(selected + 1, view.text.line_count() - 1);
    view.cursor = view.text.line_start(selected_line);
    view.dirty = false;
    view.clear_undo_history();
}

void EditorProviderModelPicker::open_providers() {
    items = tui::selectable_provider_ids();
    selected = 0;
    for_provider = true;
    active = true;
}

void EditorProviderModelPicker::open_models(std::vector<std::string> models) {
    items = std::move(models);
    selected = 0;
    for_provider = false;
    active = true;
}

void EditorProviderModelPicker::clear() {
    active = false;
    items.clear();
    selected = 0;
}

bool EditorProviderModelPicker::handle_escape(const std::string& sequence, std::string& status_out) {
    switch (ui::handle_selector_escape_sequence(sequence, items.size(), selected, status_out, selection_label())) {
        case ui::SelectorMovementResult::Navigated:
            return true;
        case ui::SelectorMovementResult::Cancelled:
            clear();
            status_out = cancel_message();
            return true;
    }
    return false;
}

}  // namespace pkchat::editor