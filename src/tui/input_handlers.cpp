#include "tui/input_handlers.hpp"

#include "tui/detail/render.hpp"

#include "editor/selection.hpp"
#include "provider/provider.hpp"
#include "editor/terminal_input.hpp"

#include <sstream>

namespace pkchat::tui {

namespace {

bool is_escape_final(unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') || ch == '~';
}

}  // namespace

std::string thread_picker_text(const std::vector<chat::ThreadSummary>& threads, size_t selected) {
    std::ostringstream out;
    out << "Newest first · Enter opens · Esc cancels\n";
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

EscapeResult handle_escape(editor::EditorState& input, const Layout& layout, int& history_scroll, std::string& status) {
    unsigned char ch = 0;
    if (!editor::read_terminal_byte(ch, 25)) {
        return EscapeResult::Unhandled;
    }
    if (ch == '\r' || ch == '\n') {
        detail::insert_input(input, "\n", status);
        status = "Inserted newline. Enter sends; Ctrl+S also sends.";
        return EscapeResult::Handled;
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
        if (ch == 'r' || ch == 'R') {
            return EscapeResult::Regenerate;
        }
        if (ch >= 32 || ch == '\t') {
            detail::insert_input(input, std::string(1, static_cast<char>(ch)), status);
        }
        return EscapeResult::Handled;
    }

    editor::MovementKeyEvent movement;
    if (editor::parse_movement_sequence(sequence, movement)) {
        if (!movement.shift && (movement.key == editor::MovementKey::Home ||
                                movement.key == editor::MovementKey::End)) {
            if (movement.key == editor::MovementKey::Home) {
                history_scroll = history_scroll_for_thread_beginning();
            } else {
                history_scroll = history_scroll_for_thread_end();
            }
        } else if (!movement.shift && (movement.key == editor::MovementKey::PageUp ||
                                       movement.key == editor::MovementKey::PageDown)) {
            const int step = std::max(1, layout.history_rows / 2);
            if (movement.key == editor::MovementKey::PageUp) {
                history_scroll += step;
            } else {
                history_scroll -= step;
            }
        } else {
            input.apply_movement(movement.key, layout.input_rect, movement.shift);
        }
        return EscapeResult::Handled;
    }
    if (sequence == "[3~") {
        detail::set_status_from_error(input.erase_at_cursor(), status);
        return EscapeResult::Handled;
    }
    return EscapeResult::Handled;
}

bool handle_thread_picker_escape(std::vector<chat::ThreadSummary>& threads,
                                 size_t& selected,
                                 TuiMode& mode,
                                 std::string& status) {
    unsigned char ch = 0;
    if (!editor::read_terminal_byte(ch, 25)) {
        mode = TuiMode::Chat;
        status = "Thread list cancelled";
        return true;
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
        mode = TuiMode::Chat;
        status = "Thread list cancelled";
        return true;
    }

    editor::MovementKeyEvent movement;
    if (!editor::parse_movement_sequence(sequence, movement) || threads.empty()) {
        return true;
    }
    switch (movement.key) {
        case editor::MovementKey::Up:
            if (selected > 0) {
                --selected;
            }
            break;
        case editor::MovementKey::Down:
            if (selected + 1 < threads.size()) {
                ++selected;
            }
            break;
        case editor::MovementKey::PageUp:
            selected = selected > 10 ? selected - 10 : 0;
            break;
        case editor::MovementKey::PageDown:
            selected = std::min(threads.size() - 1, selected + 10);
            break;
        case editor::MovementKey::Home:
            selected = 0;
            break;
        case editor::MovementKey::End:
            selected = threads.size() - 1;
            break;
        case editor::MovementKey::Left:
        case editor::MovementKey::Right:
            break;
    }
    status = "Selected thread " + std::to_string(selected + 1) + "/" + std::to_string(threads.size());
    return true;
}

}  // namespace pkchat::tui