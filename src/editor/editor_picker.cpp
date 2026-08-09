#include "editor/editor_picker.hpp"

#include "provider/provider.hpp"
#include "ui/provider_model_selector.hpp"
#include "ui/text_selector.hpp"

namespace ainiux::editor {
namespace {

bool is_printable_search_char(unsigned char ch) {
    return ch >= 0x20U && ch != 0x7fU;
}

}  // namespace

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

std::string EditorProviderModelPicker::label_at(size_t index) const {
    if (index >= items.size()) {
        return {};
    }
    if (for_provider) {
        return provider::display_name_for_profile(items[index]);
    }
    if (for_reasoning && index < display_labels.size()) {
        return display_labels[index];
    }
    return items[index];
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
    nav.reset_for_open();
    active = true;
}

void EditorProviderModelPicker::open_models(std::vector<std::string> models) {
    items = std::move(models);
    display_labels.clear();
    selected = 0;
    scroll = 0;
    for_provider = false;
    for_reasoning = false;
    nav.reset_for_open();
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
    nav.reset_for_open();
    active = true;
}

void EditorProviderModelPicker::clear() {
    active = false;
    items.clear();
    display_labels.clear();
    selected = 0;
    scroll = 0;
    for_reasoning = false;
    nav.reset_for_open();
}

bool EditorProviderModelPicker::handle_escape(const std::string& sequence, std::string& status_out) {
    if (nav.search_active) {
        // Bare Esc (empty sequence) cancels draft only. Movement cancels draft and navigates.
        nav.search_active = false;
        nav.search_draft.clear();
        if (sequence.empty()) {
            status_out = status_message();
            return true;
        }
        switch (ui::handle_selector_escape_sequence(sequence, items.size(), selected, status_out,
                                                    selection_label())) {
            case ui::SelectorMovementResult::Navigated:
                return true;
            case ui::SelectorMovementResult::Cancelled:
                status_out = status_message();
                return true;
        }
        return true;
    }
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
    if (!active || items.empty() || nav.search_active) {
        return false;
    }
    if (ch == '/' || ch == '.') {
        return false;
    }
    const bool jumped = ui::jump_text_selector_by_char(
        selected, items.size(), [&](size_t index) { return label_at(index); }, ch);
    if (jumped) {
        status_out = status_message();
    }
    return jumped;
}

bool EditorProviderModelPicker::handle_search_sort_char(unsigned char ch, std::string& status_out) {
    if (!active) {
        return false;
    }

    if (nav.search_active) {
        if (ch == '\r' || ch == '\n') {
            const std::string needle = nav.search_draft.empty() ? nav.last_search : nav.search_draft;
            nav.search_active = false;
            nav.search_draft.clear();
            if (needle.empty()) {
                status_out = ui::text_selector_no_previous_search_status();
                return true;
            }
            nav.last_search = needle;
            if (ui::find_next_text_selector_match(
                    selected, items.size(), [&](size_t index) { return label_at(index); }, needle)) {
                status_out = status_message();
            } else {
                status_out = ui::text_selector_no_match_status(needle);
            }
            return true;
        }
        if (ch == 127 || ch == 8) {
            if (!nav.search_draft.empty()) {
                nav.search_draft.pop_back();
            }
            status_out = nav.draft_status();
            return true;
        }
        if (is_printable_search_char(ch)) {
            nav.search_draft.push_back(static_cast<char>(ch));
            status_out = nav.draft_status();
            return true;
        }
        // Consume other control keys while drafting so they do not select/jump.
        return true;
    }

    if (ch == '/') {
        nav.search_active = true;
        nav.search_draft.clear();
        status_out = nav.draft_status();
        return true;
    }

    if (ch == '.') {
        if (items.empty()) {
            status_out = empty_message();
            return true;
        }
        if (for_reasoning) {
            ui::toggle_text_selector_alpha_sort_paired(items, display_labels, selected, nav.sorted,
                                                       nav.original_items, nav.original_labels);
        } else if (for_provider) {
            ui::toggle_text_selector_alpha_sort_by_label(
                items, selected, nav.sorted, nav.original_items,
                [&](size_t index) { return label_at(index); });
        } else {
            ui::toggle_text_selector_alpha_sort(items, selected, nav.sorted, nav.original_items);
        }
        status_out = status_message();
        return true;
    }

    return false;
}

}  // namespace ainiux::editor
