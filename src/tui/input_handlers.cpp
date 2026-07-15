#include "tui/input_handlers.hpp"

#include "tui/detail/render.hpp"

#include "editor/selection.hpp"
#include "editor/terminal_input.hpp"
#include "provider/provider.hpp"
#include "ui/text_selector.hpp"

#include <sstream>

namespace pkchat::tui {

namespace {

bool is_escape_final(unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') || ch == '~' || (ch >= 'a' && ch <= 'z');
}

bool consume_alt_meta_prefix(unsigned char& ch) {
    if (ch != 27) {
        return false;
    }
    unsigned char next = 0;
    if (!editor::read_terminal_byte(next, 25)) {
        return false;
    }
    ch = next;
    return true;
}

bool read_csi_sequence(unsigned char first_byte, std::string& sequence) {
    if (first_byte != '[' && first_byte != 'O') {
        return false;
    }
    sequence.clear();
    sequence.push_back(static_cast<char>(first_byte));
    unsigned char ch = 0;
    while (sequence.size() < 16 && editor::read_terminal_byte(ch, 25)) {
        sequence.push_back(static_cast<char>(ch));
        if (is_escape_final(ch)) {
            break;
        }
    }
    return true;
}

void apply_alt_meta_prefix(editor::MovementKeyEvent& movement, bool alt_meta_prefix) {
    if (alt_meta_prefix) {
        movement.alt = true;
    }
}

}  // namespace

void scroll_chat_history_page_up(const Layout& layout, int& history_scroll) {
    const int step = std::max(1, layout.history_rows / 2);
    history_scroll += step;
}

void scroll_chat_history_page_down(const Layout& layout, int& history_scroll) {
    const int step = std::max(1, layout.history_rows / 2);
    history_scroll -= step;
}

bool apply_chat_history_scroll(const editor::MovementKeyEvent& movement,
                               const Layout& layout,
                               int& history_scroll) {
    if (movement.shift || movement.ctrl || !movement.alt) {
        return false;
    }
    switch (movement.key) {
        case editor::MovementKey::PageUp:
            scroll_chat_history_page_up(layout, history_scroll);
            return true;
        case editor::MovementKey::PageDown:
            scroll_chat_history_page_down(layout, history_scroll);
            return true;
        case editor::MovementKey::Home:
            history_scroll = history_scroll_for_thread_beginning();
            return true;
        case editor::MovementKey::End:
            history_scroll = history_scroll_for_thread_end();
            return true;
        default:
            return false;
    }
}

std::vector<std::string> selectable_provider_ids() {
    std::vector<std::string> providers;
    for (const provider::Profile& profile : provider::built_in_profiles()) {
        if (!provider::is_selectable_provider(profile)) {
            continue;
        }
        providers.push_back(profile.name);
    }
    return providers;
}

namespace {

std::string thread_summary_label(const chat::ThreadSummary& thread) {
    std::ostringstream out;
    out << thread.name;
    if (!thread.last_provider.empty() || !thread.last_model.empty()) {
        out << " [";
        if (!thread.last_provider.empty()) {
            out << provider::display_name_for_profile(thread.last_provider);
        }
        if (!thread.last_model.empty()) {
            if (!thread.last_provider.empty()) {
                out << " / ";
            }
            out << thread.last_model;
        }
        out << "]";
    }
    out << " · " << thread.modified_at;
    out << " · " << thread.message_count << " msgs";
    return out.str();
}

ui::TextSelectorConfig standard_picker_config(const char* header) {
    ui::TextSelectorConfig config;
    config.header = header;
    return config;
}

}  // namespace

std::string provider_picker_text(const std::vector<std::string>& provider_ids, size_t selected) {
    std::vector<std::string> labels;
    labels.reserve(provider_ids.size());
    for (const std::string& provider_id : provider_ids) {
        labels.push_back(provider::display_name_for_profile(provider_id));
    }
    return ui::render_text_selector(standard_picker_config(ui::kTextSelectorStandardHint), selected, labels);
}

std::string model_picker_text(const std::vector<std::string>& models, size_t selected) {
    return ui::render_text_selector(standard_picker_config(ui::kTextSelectorStandardHint), selected, models);
}

std::string thread_picker_text(const std::vector<chat::ThreadSummary>& threads, size_t selected) {
    return ui::render_text_selector(standard_picker_config(ui::kTextSelectorThreadHint), selected, threads.size(),
                                    [&](size_t index) { return thread_summary_label(threads[index]); });
}

std::string attachment_picker_text(const std::vector<ChatAttachment>& attachments, size_t selected) {
    ui::TextSelectorConfig config;
    config.header = ui::kTextSelectorAttachmentHint;
    return ui::render_text_selector(config, selected, attachments.size(), [&](size_t index) {
        return attachments[index].source;
    });
}

std::string remove_confirm_text(const chat::Session& session) {
    std::ostringstream out;
    out << (session.name.empty() ? "New chat" : session.name) << "\n";
    out << "Press y to remove · n or Esc to cancel";
    return out.str();
}

std::string system_edit_text() {
    return "Enter saves · Esc cancels";
}

std::string history_edit_text() {
    return "Enter saves · Esc cancels";
}

std::string join_models_preview(const std::vector<std::string>& models) {
    if (models.empty()) {
        return "No models returned";
    }
    std::string out = "Models:";
    const size_t limit = std::min<size_t>(models.size(), 5);
    for (size_t i = 0; i < limit; ++i) {
        out += (i == 0 ? " " : ", ");
        out += models[i];
    }
    if (models.size() > limit) {
        out += ", ...";
    }
    return out;
}

EscapeResult handle_escape(editor::EditorState& input,
                           const Layout& layout,
                           int& history_scroll,
                           std::string& status,
                           bool input_only_movement) {
    unsigned char ch = 0;
    if (!editor::read_terminal_byte(ch, 25)) {
        return EscapeResult::Unhandled;
    }
    bool alt_meta_prefix = consume_alt_meta_prefix(ch);
    if (!alt_meta_prefix) {
        alt_meta_prefix = editor::consume_pending_escape_alt_meta();
    }
    if (ch == '\r' || ch == '\n') {
        detail::insert_input(input, "\n", status);
        status = "Inserted newline. Enter sends; Ctrl+S also sends.";
        return EscapeResult::Handled;
    }

    std::string sequence;
    if (read_csi_sequence(ch, sequence)) {
        editor::MovementKeyEvent movement;
        if (editor::parse_movement_sequence(sequence, movement)) {
            apply_alt_meta_prefix(movement, alt_meta_prefix);
            if (!input_only_movement && apply_chat_history_scroll(movement, layout, history_scroll)) {
                // Chat history scroll handled; leave input cursor unchanged.
            } else {
                input.apply_movement(movement.key,
                                     layout.input_rect,
                                     movement.shift,
                                     movement.alt,
                                     movement.ctrl);
            }
            return EscapeResult::Handled;
        }
    } else {
        if (ch >= 32 || ch == '\t') {
            detail::insert_input(input, std::string(1, static_cast<char>(ch)), status);
        }
        return EscapeResult::Handled;
    }
    if (sequence == "[3~") {
        detail::set_status_from_error(input.erase_at_cursor(), status);
        return EscapeResult::Handled;
    }
    return EscapeResult::Handled;
}

PickerEscapeResult handle_list_picker_escape(size_t item_count,
                                             size_t& selected,
                                             std::string& status,
                                             const std::string& selection_label) {
    unsigned char ch = 0;
    if (!editor::read_terminal_byte(ch, 25)) {
        return PickerEscapeResult::Cancelled;
    }
    std::string sequence;
    if (ch == '[' || ch == 'O') {
        sequence.push_back(static_cast<char>(ch));
        while (sequence.size() < 16 && editor::read_terminal_byte(ch, 25)) {
            sequence.push_back(static_cast<char>(ch));
            if (is_escape_final(ch)) {
                break;
            }
        }
    } else {
        return PickerEscapeResult::Cancelled;
    }

    editor::MovementKeyEvent movement;
    if (!editor::parse_movement_sequence(sequence, movement) || item_count == 0) {
        return PickerEscapeResult::Cancelled;
    }
    selected = ui::move_text_selector_selection(selected, item_count, movement.key);
    status = ui::text_selector_status(selection_label, selected, item_count);
    return PickerEscapeResult::Navigated;
}

PickerEscapeResult handle_thread_list_escape(std::vector<chat::ThreadSummary>& threads,
                                                size_t& selected,
                                                std::string& status,
                                                size_t& pending_delete,
                                                TuiMode& mode) {
    unsigned char ch = 0;
    if (!editor::read_terminal_byte(ch, 25)) {
        mode = TuiMode::Chat;
        return PickerEscapeResult::Cancelled;
    }
    if (ch == '[') {
        std::string seq;
        seq.push_back('[');
        unsigned char next = 0;
        while (seq.size() < 8 && editor::read_terminal_byte(next, 25)) {
            seq.push_back(static_cast<char>(next));
            if ((next >= 'A' && next <= 'Z') || next == '~') {
                break;
            }
        }
        if (seq == "[3~") {
            // Forward delete key (DEL)
            if (selected < threads.size()) {
                pending_delete = selected;
                mode = TuiMode::ThreadDeleteConfirm;
                status = "Delete thread? y/n (Esc cancels)";
            }
            return PickerEscapeResult::Navigated;
        }
        // Treat as movement
        editor::MovementKeyEvent movement;
        if (editor::parse_movement_sequence(seq, movement) && threads.size() > 0) {
            selected = ui::move_text_selector_selection(selected, threads.size(), movement.key);
            status = ui::text_selector_status("Selected thread", selected, threads.size());
            return PickerEscapeResult::Navigated;
        }
    }
    // Unknown or plain Esc: cancel list
    mode = TuiMode::Chat;
    return PickerEscapeResult::Cancelled;
}

PickerEscapeResult handle_attachment_list_escape(size_t item_count,
                                                 size_t& selected,
                                                 std::string& status,
                                                 size_t& pending_delete,
                                                 TuiMode& mode) {
    unsigned char ch = 0;
    if (!editor::read_terminal_byte(ch, 25)) {
        mode = TuiMode::Chat;
        return PickerEscapeResult::Cancelled;
    }
    if (ch == '[') {
        std::string seq;
        seq.push_back('[');
        unsigned char next = 0;
        while (seq.size() < 8 && editor::read_terminal_byte(next, 25)) {
            seq.push_back(static_cast<char>(next));
            if ((next >= 'A' && next <= 'Z') || next == '~') {
                break;
            }
        }
        if (seq == "[3~") {
            // Forward delete key
            if (selected < item_count) {
                pending_delete = selected;
                mode = TuiMode::AttachmentDeleteConfirm;
                status = "Delete attachment? y/n (Esc cancels)";
            }
            return PickerEscapeResult::Navigated;
        }
        // Treat as movement
        editor::MovementKeyEvent movement;
        if (editor::parse_movement_sequence(seq, movement) && item_count > 0) {
            selected = ui::move_text_selector_selection(selected, item_count, movement.key);
            status = ui::text_selector_status("Selected attachment", selected, item_count);
            return PickerEscapeResult::Navigated;
        }
    }
    // Unknown escape: cancel list
    mode = TuiMode::Chat;
    return PickerEscapeResult::Cancelled;
}

}  // namespace pkchat::tui