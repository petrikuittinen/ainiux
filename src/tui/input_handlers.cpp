#include "tui/input_handlers.hpp"

#include "tui/detail/render.hpp"

#include "editor/selection.hpp"
#include "provider/provider.hpp"
#include "editor/terminal_input.hpp"

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

bool apply_chat_history_scroll(const editor::MovementKeyEvent& movement,
                               const Layout& layout,
                               int& history_scroll) {
    if (movement.shift || movement.ctrl || !movement.alt) {
        return false;
    }
    switch (movement.key) {
        case editor::MovementKey::PageUp: {
            const int step = std::max(1, layout.history_rows / 2);
            history_scroll += step;
            return true;
        }
        case editor::MovementKey::PageDown: {
            const int step = std::max(1, layout.history_rows / 2);
            history_scroll -= step;
            return true;
        }
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
        if (profile.offline || profile.name == "custom_openai_chat") {
            continue;
        }
        providers.push_back(profile.name);
    }
    return providers;
}

std::string list_picker_text(const std::string& hint,
                             const std::vector<std::string>& items,
                             size_t selected) {
    std::ostringstream out;
    out << hint << "\n";
    for (size_t i = 0; i < items.size(); ++i) {
        out << (i == selected ? u8"› " : "  ");
        out << items[i];
        if (i + 1 != items.size()) {
            out << "\n";
        }
    }
    return out.str();
}

std::string provider_picker_text(const std::vector<std::string>& provider_ids, size_t selected) {
    std::vector<std::string> labels;
    labels.reserve(provider_ids.size());
    for (const std::string& provider_id : provider_ids) {
        labels.push_back(provider::display_name_for_profile(provider_id));
    }
    return list_picker_text("↑↓ move · Enter select · Esc cancel", labels, selected);
}

std::string model_picker_text(const std::vector<std::string>& models, size_t selected) {
    return list_picker_text("↑↓ move · Enter select · Esc cancel", models, selected);
}

std::string thread_picker_text(const std::vector<chat::ThreadSummary>& threads, size_t selected) {
    std::ostringstream out;
    out << "Newest first · Enter opens · N new · Esc cancels\n";
    for (size_t i = 0; i < threads.size(); ++i) {
        const chat::ThreadSummary& thread = threads[i];
        out << (i == selected ? u8"› " : "  ");
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
        if (i + 1 != threads.size()) {
            out << "\n";
        }
    }
    return out.str();
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
        if (ch == 'r' || ch == 'R') {
            return EscapeResult::Regenerate;
        }
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
    switch (movement.key) {
        case editor::MovementKey::Up:
            if (selected > 0) {
                --selected;
            }
            break;
        case editor::MovementKey::Down:
            if (selected + 1 < item_count) {
                ++selected;
            }
            break;
        case editor::MovementKey::PageUp:
            selected = selected > 10 ? selected - 10 : 0;
            break;
        case editor::MovementKey::PageDown:
            selected = std::min(item_count - 1, selected + 10);
            break;
        case editor::MovementKey::Home:
            selected = 0;
            break;
        case editor::MovementKey::End:
            selected = item_count - 1;
            break;
        case editor::MovementKey::Left:
        case editor::MovementKey::Right:
            break;
    }
    status = selection_label + " " + std::to_string(selected + 1) + "/" + std::to_string(item_count);
    return PickerEscapeResult::Navigated;
}

bool handle_thread_picker_escape(std::vector<chat::ThreadSummary>& threads,
                                 size_t& selected,
                                 TuiMode& mode,
                                 std::string& status) {
    const PickerEscapeResult result =
        handle_list_picker_escape(threads.size(), selected, status, "Selected thread");
    if (result == PickerEscapeResult::Cancelled) {
        mode = TuiMode::Chat;
        status = "Thread list cancelled";
    }
    return true;
}

}  // namespace pkchat::tui