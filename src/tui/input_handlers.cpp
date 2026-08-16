#include "tui/input_handlers.hpp"

#include "tui/detail/render.hpp"
#include "tui/session_load.hpp"

#include "editor/dired.hpp"
#include "editor/selection.hpp"
#include "editor/terminal_input.hpp"
#include "provider/provider.hpp"
#include "ui/provider_model_display.hpp"
#include "ui/text_selector.hpp"

#include <sstream>
#include <limits>

namespace ainiux::tui {

namespace {

bool is_escape_final(unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') || ch == '~' || (ch >= 'a' && ch <= 'z');
}

bool consume_alt_meta_prefix(unsigned char& ch) {
    if (ch != 27) {
        return false;
    }
    unsigned char next = 0;
    if (!editor::read_terminal_byte(
            next, editor::terminal_escape_inter_byte_timeout_ms())) {
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
    while (sequence.size() < 16 &&
           editor::read_terminal_byte(
               ch, editor::terminal_escape_inter_byte_timeout_ms())) {
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

bool apply_top_aligned_panel_scroll(editor::MovementKey key, int& history_scroll) {
    switch (key) {
        case editor::MovementKey::Up:
            history_scroll = std::max(0, history_scroll - 1);
            return true;
        case editor::MovementKey::Down:
            if (history_scroll < std::numeric_limits<int>::max()) ++history_scroll;
            return true;
        case editor::MovementKey::PageUp:
            history_scroll = std::max(0, history_scroll - 8);
            return true;
        case editor::MovementKey::PageDown:
            if (history_scroll < std::numeric_limits<int>::max() - 8)
                history_scroll += 8;
            else
                history_scroll = std::numeric_limits<int>::max();
            return true;
        case editor::MovementKey::Home:
            history_scroll = 0;
            return true;
        case editor::MovementKey::End:
            history_scroll = std::numeric_limits<int>::max();
            return true;
        default:
            return false;
    }
}

bool apply_chat_mouse_scroll(const editor::MouseInputEvent& mouse,
                             const Layout& layout,
                             TuiMode mode,
                             int& history_scroll) {
    if (mouse.col < 1 || mouse.col > layout.cols ||
        mouse.row < layout.history_row ||
        mouse.row >= layout.history_row + layout.history_rows) {
        return false;
    }
    if (mode == TuiMode::GuardApprovalConfirm) {
        if (mouse.button == editor::MouseButton::WheelUp) {
            history_scroll = std::max(0, history_scroll - 1);
            return true;
        }
        if (mouse.button == editor::MouseButton::WheelDown) {
            if (history_scroll < std::numeric_limits<int>::max()) ++history_scroll;
            return true;
        }
        return false;
    }
    if (mode != TuiMode::Chat) return false;
    if (mouse.button == editor::MouseButton::WheelUp) {
        if (history_scroll < std::numeric_limits<int>::max()) ++history_scroll;
        return true;
    }
    if (mouse.button == editor::MouseButton::WheelDown) {
        history_scroll = std::max(0, history_scroll - 1);
        return true;
    }
    return false;
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

namespace {

std::string thread_summary_label(const chat::ThreadSummary& thread) {
    std::ostringstream out;
    if (thread.read_only) {
        out << "[RO] ";
    }
    const std::string missing =
        saved_provider_model_missing(thread.last_provider, thread.last_model);
    if (!missing.empty()) {
        out << "[SETUP: " << missing << "] ";
    }
    out << thread.name;
    if (!thread.last_provider.empty() || !thread.last_model.empty()) {
        out << " "
            << ui::provider_model_display_label(thread.last_provider, thread.last_model);
    }
    out << " · " << thread.modified_at;
    out << " · " << thread.message_count << " msgs";
    return out.str();
}

}  // namespace

std::string thread_picker_label(const chat::ThreadSummary& thread) {
    return thread_summary_label(thread);
}

namespace {

ui::TextSelectorConfig picker_config(const char* header) {
    ui::TextSelectorConfig config;
    config.header = header;
    return config;
}

}  // namespace

std::string thread_picker_text(const std::vector<chat::ThreadSummary>& threads, size_t selected) {
    return ui::render_text_selector(picker_config(ui::kTextSelectorThreadHint), selected, threads.size(),
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

void apply_recalled_prompt(editor::EditorState& input,
                           const std::string& text,
                           size_t undo_limit) {
    input = editor::EditorState::from_text(text);
    input.set_undo_limit(undo_limit);
    input.mode = editor::EditorMode::Chat;
    input.vertical_movement = editor::VerticalMovementMode::VisualRow;
    input.cursor = input.text.size();
}

bool try_prompt_recall(PromptRecall& recall,
                       editor::EditorState& input,
                       const Layout& layout,
                       editor::MovementKey key,
                       size_t undo_limit) {
    if (key != editor::MovementKey::Up && key != editor::MovementKey::Down) {
        return false;
    }
    const bool first = input.cursor_on_first_visual_row(layout.input_rect);
    const bool last = input.cursor_on_last_visual_row(layout.input_rect);
    if (key == editor::MovementKey::Up && !first) return false;
    if (key == editor::MovementKey::Down && !last) return false;
    std::string current = input.text.str();
    const bool moved = key == editor::MovementKey::Up
                           ? recall.recall_previous(current)
                           : recall.recall_next(current);
    if (!moved) return false;
    apply_recalled_prompt(input, current, undo_limit);
    return true;
}

EscapeResult handle_escape(editor::EditorState& input,
                           const Layout& layout,
                           int& history_scroll,
                           std::string& status,
                           bool input_only_movement,
                           PromptRecall* prompt_recall,
                           size_t input_undo_limit) {
    unsigned char ch = 0;
    if (!editor::read_terminal_byte(
            ch, editor::terminal_escape_inter_byte_timeout_ms())) {
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
        // F4 opens dired (editor and agent share the same physical key sequences).
        if (editor::is_dired_f4_sequence(sequence)) {
            return EscapeResult::OpenDired;
        }
        editor::MovementKeyEvent movement;
        if (editor::parse_movement_sequence(sequence, movement)) {
            apply_alt_meta_prefix(movement, alt_meta_prefix);
            if (!input_only_movement && apply_chat_history_scroll(movement, layout, history_scroll)) {
                // Chat history scroll handled; leave input cursor unchanged.
            } else if (prompt_recall != nullptr && !movement.shift && !movement.alt &&
                       !movement.ctrl &&
                       try_prompt_recall(*prompt_recall, input, layout, movement.key,
                                         input_undo_limit)) {
                // Replaced the draft with a recalled prompt.
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
        const bool had_selection = input.selection.has_range();
        const Error erase_error = input.erase_at_cursor();
        if (!erase_error.ok()) {
            detail::set_status_from_error(erase_error, status);
        } else if (had_selection) {
            // Selection delete acts as cut into the process clipboard.
            editor::publish_terminal_clipboard(editor::shared_clipboard().text());
            status = "Cut selection";
        }
        return EscapeResult::Handled;
    }
    return EscapeResult::Handled;
}

PickerEscapeResult handle_list_picker_escape(size_t item_count,
                                             size_t& selected,
                                             std::string& status,
                                             const std::string& selection_label) {
    unsigned char ch = 0;
    if (!editor::read_terminal_byte(
            ch, editor::terminal_escape_inter_byte_timeout_ms())) {
        return PickerEscapeResult::Cancelled;
    }
    std::string sequence;
    if (ch == '[' || ch == 'O') {
        sequence.push_back(static_cast<char>(ch));
        while (sequence.size() < 16 &&
               editor::read_terminal_byte(
                   ch, editor::terminal_escape_inter_byte_timeout_ms())) {
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

PickerEscapeResult handle_guard_approval_escape(int* history_scroll) {
    unsigned char ch = 0;
    if (!editor::read_terminal_byte(
            ch, editor::terminal_escape_inter_byte_timeout_ms())) {
        return PickerEscapeResult::Cancelled;
    }
    std::string sequence;
    if (!read_csi_sequence(ch, sequence)) return PickerEscapeResult::Cancelled;
    if (editor::is_dired_f4_sequence(sequence)) return PickerEscapeResult::OpenDired;
    editor::MovementKeyEvent movement;
    if (!editor::parse_movement_sequence(sequence, movement))
        return PickerEscapeResult::Cancelled;
    if (history_scroll != nullptr &&
        apply_top_aligned_panel_scroll(movement.key, *history_scroll))
        return PickerEscapeResult::Navigated;
    return PickerEscapeResult::Cancelled;
}

PickerEscapeResult handle_thread_list_escape(std::vector<chat::ThreadSummary>& threads,
                                                size_t& selected,
                                                std::string& status,
                                                size_t& pending_delete,
                                                TuiMode& mode,
                                                bool allow_create_new) {
    unsigned char ch = 0;
    if (!editor::read_terminal_byte(
            ch, editor::terminal_escape_inter_byte_timeout_ms())) {
        mode = TuiMode::Chat;
        return PickerEscapeResult::Cancelled;
    }
    if (ch == '[') {
        std::string seq;
        seq.push_back('[');
        unsigned char next = 0;
        while (seq.size() < 8 &&
               editor::read_terminal_byte(
                   next, editor::terminal_escape_inter_byte_timeout_ms())) {
            seq.push_back(static_cast<char>(next));
            if ((next >= 'A' && next <= 'Z') || next == '~') {
                break;
            }
        }
        if (seq == "[2~") {
            // Insert key creates a new thread in chat; agent uses explicit /new.
            if (allow_create_new) {
                return PickerEscapeResult::CreateNew;
            }
            return PickerEscapeResult::Navigated;
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
    if (!editor::read_terminal_byte(
            ch, editor::terminal_escape_inter_byte_timeout_ms())) {
        mode = TuiMode::Chat;
        return PickerEscapeResult::Cancelled;
    }
    if (ch == '[') {
        std::string seq;
        seq.push_back('[');
        unsigned char next = 0;
        while (seq.size() < 8 &&
               editor::read_terminal_byte(
                   next, editor::terminal_escape_inter_byte_timeout_ms())) {
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

}  // namespace ainiux::tui
