#include "editor/editor_picker.hpp"

#include "provider/provider.hpp"
#include "ui/provider_model_selector.hpp"
#include "ui/text_selector.hpp"

namespace ainiux::editor {

std::string EditorProviderModelPicker::selection_label() const {
    return for_provider ? "Selected provider" : for_reasoning ? "Selected reasoning" : "Selected model";
}

std::string EditorProviderModelPicker::cancel_message() const {
    return for_provider ? "Provider selection cancelled"
                        : for_reasoning ? "Reasoning selection cancelled" : "Model selection cancelled";
}

std::string EditorProviderModelPicker::empty_message() const {
    return for_provider ? "No providers available" : for_reasoning ? "No reasoning options" : "No models returned";
}

std::string EditorProviderModelPicker::status_message() const {
    return items.empty() ? empty_message()
                         : ui::text_selector_status(selection_label(), selected, items.size());
}

void EditorProviderModelPicker::refresh_view() {
    std::string text;
    if (for_provider) {
        text = ui::provider_selector_text(items, selected);
    } else if (for_reasoning) {
        ui::TextSelectorConfig config;
        config.header = ui::kTextSelectorStandardHint;
        config.selected_prefix = ui::kTextSelectorCaretPrefix;
        text = ui::render_text_selector(config, selected, display_labels);
    } else {
        text = ui::model_selector_text(items, selected);
    }
    view = EditorState::from_text(text);
    view.path = for_provider ? "[providers]" : for_reasoning ? "[reasoning]" : "[models]";
    view.highlight_enabled = false;
    const size_t selected_line = std::min(selected + 1, view.text.line_count() - 1);
    view.cursor = view.text.line_start(selected_line);
    view.dirty = false;
    view.clear_undo_history();
}

void EditorProviderModelPicker::open_providers() {
    items = ui::selectable_provider_ids();
    display_labels.clear();
    selected = 0;
    scroll = 0;
    for_provider = true;
    for_reasoning = false;
    active = true;
}

void EditorProviderModelPicker::open_models(std::vector<std::string> models) {
    items = std::move(models);
    display_labels.clear();
    selected = 0;
    scroll = 0;
    for_provider = false;
    for_reasoning = false;
    active = true;
}

void EditorProviderModelPicker::open_reasoning(std::vector<std::string> values,
                                               std::vector<std::string> labels,
                                               size_t current) {
    items = std::move(values);
    display_labels = std::move(labels);
    selected = items.empty() ? 0 : std::min(current, items.size() - 1);
    scroll = 0;
    for_provider = false;
    for_reasoning = true;
    active = true;
}

void EditorProviderModelPicker::clear() {
    active = false;
    items.clear();
    display_labels.clear();
    selected = 0;
    scroll = 0;
    for_reasoning = false;
}

bool EditorProviderModelPicker::handle_escape(const std::string& sequence, std::string& status_out) {
    switch (ui::handle_selector_escape_sequence(sequence, items.size(), selected, status_out, selection_label())) {
        case ui::SelectorMovementResult::Navigated:
            return true;
        case ui::SelectorMovementResult::Cancelled: {
            const std::string message = cancel_message();
            clear();
            status_out = message;
            return true;
        }
    }
    return false;
}

bool EditorProviderModelPicker::handle_jump_char(unsigned char ch, std::string& status_out) {
    if (!active || items.empty()) {
        return false;
    }
    const bool jumped = ui::jump_text_selector_by_char(
        selected,
        items.size(),
        [&](size_t index) -> std::string {
            if (for_provider) {
                return provider::display_name_for_profile(items[index]);
            }
            if (for_reasoning && index < display_labels.size()) {
                return display_labels[index];
            }
            return items[index];
        },
        ch);
    if (jumped) {
        status_out = status_message();
    }
    return jumped;
}

}  // namespace ainiux::editor
