#include "editor/autosave.hpp"
#include "editor/terminal_ui.hpp"

#include "common.hpp"
#include "editor/ai_continue.hpp"
#include "editor/detail/editor_common.hpp"
#include "editor/detail/unicode.hpp"
#include "editor/detail/wrap.hpp"
#include "editor/clipboard.hpp"
#include "editor/editor_assist.hpp"
#include "editor/selection.hpp"
#include "editor/terminal_input.hpp"
#include "tui/activity.hpp"
#include "ui/confirmation.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <sys/ioctl.h>
#include <unistd.h>

namespace pkchat::editor {
namespace {

bool minibuffer_supports_path_completion(MinibufferAction action) {
    return action == MinibufferAction::SaveFile || action == MinibufferAction::SaveAsFile ||
           action == MinibufferAction::LoadFile;
}

}  // namespace

using detail::pad_or_clip_ascii;

TerminalSession::~TerminalSession() {
    restore();
}

TerminalSize terminal_size() {
    TerminalSize size;
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        size.rows = ws.ws_row;
        size.cols = ws.ws_col;
    }
    return size;
}

Error TerminalSession::enter() {
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
            return {ErrorCode::BadArgs, "--editor requires an interactive terminal"};
        }
        if (tcgetattr(STDIN_FILENO, &original_) != 0) {
            return {ErrorCode::Internal, std::string("could not read terminal mode: ") + std::strerror(errno)};
        }

        termios raw = original_;
        raw.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON | IEXTEN | ISIG));
        raw.c_iflag &= static_cast<tcflag_t>(~(IXON | IXOFF | ICRNL | BRKINT));
        raw.c_oflag &= static_cast<tcflag_t>(~OPOST);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
            return {ErrorCode::Internal, std::string("could not set terminal mode: ") + std::strerror(errno)};
        }

        active_ = true;
        clear_terminal_input_queue();
        std::cout << "\x1b[?1049h\x1b[?25h\x1b[2J\x1b[H" << bracketed_paste_enable_sequence()
                  << keyboard_modifier_enable_sequence();
        std::cout.flush();
        return ok_error();
}

void TerminalSession::restore() {
        if (!active_) {
            return;
        }
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_);
        std::cout << keyboard_modifier_disable_sequence() << bracketed_paste_disable_sequence()
                  << "\x1b[0m\x1b[?25h\x1b[2J\x1b[H\x1b[?1049l";
        std::cout.flush();
        clear_terminal_input_queue();
        active_ = false;
}

std::string editor_status_line(const EditorState& state, bool help_view) {
    std::ostringstream out;
    if (help_view) {
        out << "Help (read-only)";
    } else {
        out << (state.path.empty() ? "[scratch]" : state.path);
        if (state.dirty) {
            out << " *";
        }
        out << "  Mode: Editor";
    }
    const size_t line = state.text.line_for_offset(state.cursor) + 1;
    const size_t column = state.text.display_column_for_offset(state.cursor) + 1;
    out << "  Ln " << line << ", Col " << column;
    if (help_view) {
        out << "  Esc /help or Ctrl+Q to return";
    } else {
        out << "  Ctrl+Q quit  Esc /help for help";
    }
    return out.str();
}

std::string minibuffer_text(const MinibufferState& minibuffer) {
    if (minibuffer.active) {
        return minibuffer.prompt + minibuffer.input;
    }
    return minibuffer.message;
}

namespace {

std::string activity_color_sequence(tui::ActivityKind kind) {
    switch (kind) {
        case tui::ActivityKind::Thinking:
            return "\x1b[38;2;147;197;253m";
        case tui::ActivityKind::Streaming:
            return "\x1b[38;2;74;222;128m";
        case tui::ActivityKind::None:
            break;
    }
    return "";
}

int append_utf8_cells(std::string& out, const std::string& text, size_t& pos, int max_cells) {
    int used = 0;
    while (pos < text.size() && used < max_cells) {
        const size_t width = detail::display_width_at(text, pos, 0);
        if (width == 0) {
            break;
        }
        size_t len = 1;
        if ((static_cast<unsigned char>(text[pos]) & 0xF8U) == 0xF0U && pos + 3 < text.size()) {
            len = 4;
        } else if ((static_cast<unsigned char>(text[pos]) & 0xF0U) == 0xE0U && pos + 2 < text.size()) {
            len = 3;
        } else if ((static_cast<unsigned char>(text[pos]) & 0xE0U) == 0xC0U && pos + 1 < text.size()) {
            len = 2;
        }
        out.append(text, pos, len);
        pos += len;
        used += static_cast<int>(width);
    }
    return used;
}

std::string editor_assist_minibuffer_line(const EditorAssistDisplay& assist, int width) {
    if (width <= 0) {
        return "";
    }
    const std::string label = continue_status_label(assist.provider_name, assist.model_name);
    const std::string indicator = tui::activity_indicator_text(assist.kind, assist.frame);
    const std::string color = activity_color_sequence(assist.kind);
    const std::string reset = "\x1b[0m";

    std::string out;
    out.reserve(static_cast<size_t>(width) * 4);
    int cells = 0;

    auto append_plain = [&](const std::string& text) {
        size_t pos = 0;
        cells += append_utf8_cells(out, text, pos, width - cells);
    };
    auto append_indicator = [&]() {
        if (cells >= width || indicator.empty()) {
            return;
        }
        size_t pos = 0;
        const int indicator_cells = static_cast<int>(tui::activity_indicator_width(assist.kind));
        if (cells + indicator_cells > width) {
            return;
        }
        if (!color.empty()) {
            out += color;
        }
        cells += append_utf8_cells(out, indicator, pos, width - cells);
        if (!color.empty()) {
            out += reset;
        }
    };

    append_plain(label);
    if (!label.empty() && cells < width) {
        append_plain(" ");
    }
    append_indicator();
    if (!assist.suffix.empty() && cells < width) {
        append_plain(" ");
        append_plain(assist.suffix);
    }
    while (cells < width) {
        out.push_back(' ');
        ++cells;
    }
    return out;
}

}  // namespace

void minibuffer_message(MinibufferState& minibuffer, std::string message) {
    minibuffer.active = false;
    minibuffer.action = MinibufferAction::None;
    minibuffer.prompt.clear();
    minibuffer.input.clear();
    minibuffer.message = std::move(message);
}

void start_minibuffer(MinibufferState& minibuffer,
                      MinibufferAction action,
                      std::string prompt,
                      std::string initial,
                      PathCompleter* path_completer) {
    if (path_completer != nullptr && minibuffer_supports_path_completion(action)) {
        path_completer->reset();
    }
    minibuffer.active = true;
    minibuffer.action = action;
    minibuffer.prompt = std::move(prompt);
    minibuffer.input = std::move(initial);
}

bool is_assist_minibuffer_action(MinibufferAction action) {
    return action == MinibufferAction::AssistCommand || action == MinibufferAction::AssistScopeChoice ||
           action == MinibufferAction::AssistPromptMode;
}

void exit_assist_command_mode(MinibufferState& minibuffer, AssistCompleterState& completer) {
    completer = AssistCompleterState{};
    minibuffer_message(minibuffer, "Ready");
}

void start_assist_command_mode(MinibufferState& minibuffer, AssistCompleterState& completer) {
    completer = AssistCompleterState{};
    start_minibuffer(minibuffer, MinibufferAction::AssistCommand, "Command: ", "");
}

void reset_editor_buffer(EditorState& state, PieceTable text, std::string path) {
    state.text = std::move(text);
    state.cursor = 0;
    state.preferred_column = 0;
    state.scroll_line = 0;
    state.scroll_column = 0;
    state.path = std::move(path);
    state.dirty = false;
    state.reset_autosave_pending();
    state.clear_undo_history();
    state.clear_selection();
}

bool editor_target_exists(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::error_code filesystem_error;
    return std::filesystem::exists(path, filesystem_error) &&
           std::filesystem::is_regular_file(path, filesystem_error);
}

bool needs_overwrite_confirm(const std::string& target, const std::string& current_path) {
    if (!editor_target_exists(target)) {
        return false;
    }
    if (!current_path.empty() && target == current_path) {
        return false;
    }
    return true;
}

std::string overwrite_prompt_message(const std::string& path) {
    return path + " exists. Press y to overwrite or any other key to cancel: ";
}

void save_editor_to_path(EditorState& state,
                         const std::string& path,
                         MinibufferState& minibuffer,
                         bool update_path,
                         const EditorSettings& settings) {
    Error save_error = save_file(path, state.text);
    if (save_error.ok()) {
        state.dirty = false;
        state.reset_autosave_pending();
        remove_autosave_file(path, settings);
        if (update_path) {
            state.path = path;
        }
        minibuffer_message(minibuffer, "Saved " + path);
    } else {
        minibuffer_message(minibuffer, save_error.message);
    }
}

void request_save_editor_to_path(EditorState& state,
                                 const std::string& path,
                                 MinibufferState& minibuffer,
                                 bool update_path,
                                 bool quit_after_save,
                                 bool& quit,
                                 PendingSaveRequest& pending_save,
                                 const EditorSettings& settings) {
    if (needs_overwrite_confirm(path, state.path)) {
        pending_save.path = path;
        pending_save.update_path = update_path;
        pending_save.quit_after_save = quit_after_save;
        start_minibuffer(minibuffer,
                         MinibufferAction::ConfirmOverwrite,
                         overwrite_prompt_message(path));
        return;
    }
    save_editor_to_path(state, path, minibuffer, update_path, settings);
    if (quit_after_save && !state.dirty) {
        quit = true;
    }
}

std::string file_size_warning_message(const std::string& path, std::uintmax_t size) {
    return "Warning: " + path + " is " + std::to_string(size) + " bytes";
}

void load_editor_from_path(EditorState& state,
                           const std::string& path,
                           const EditorSettings& settings,
                           MinibufferState& minibuffer) {
    PieceTable loaded;
    Error load_error = load_file(path, settings, loaded);
    if (load_error.ok()) {
        reset_editor_buffer(state, std::move(loaded), path);
        minibuffer_message(minibuffer, "Loaded " + path);
    } else {
        minibuffer_message(minibuffer, load_error.message);
    }
}

void recover_editor_from_autosave(EditorState& state,
                                  const std::string& path,
                                  const std::string& autosave_path,
                                  const EditorSettings& settings,
                                  MinibufferState& minibuffer) {
    PieceTable loaded;
    Error load_error = load_file(autosave_path, settings, loaded);
    if (load_error.ok()) {
        reset_editor_buffer(state, std::move(loaded), path);
        state.dirty = true;
        minibuffer_message(minibuffer, "Recovered auto-save from " + autosave_path);
    } else {
        minibuffer_message(minibuffer, load_error.message);
    }
}

bool offer_autosave_recovery_before_load(const std::string& path,
                                         const EditorSettings& settings,
                                         MinibufferState& minibuffer,
                                         PendingAutosaveRecovery& pending) {
    const AutosaveRecoveryOffer offer = check_autosave_recovery_offer(path, settings);
    if (!offer.should_offer) {
        return false;
    }
    pending.path = path;
    pending.autosave_path = offer.autosave_path;
    start_minibuffer(minibuffer,
                     MinibufferAction::ConfirmAutosaveRecovery,
                     autosave_recovery_prompt_message(path, offer.autosave_path) + " (y/n) ");
    return true;
}

void request_load_editor_from_path(EditorState& state,
                                   const std::string& path,
                                   const EditorSettings& settings,
                                   MinibufferState& minibuffer,
                                   std::string& pending_load_path,
                                   PendingAutosaveRecovery& pending_autosave_recovery) {
    FileLoadCheck check;
    Error err = check_load_file_size(path, settings, check);
    if (!err.ok()) {
        minibuffer_message(minibuffer, err.message);
        return;
    }
    if (check.should_warn) {
        pending_load_path = path;
        start_minibuffer(minibuffer,
                         MinibufferAction::ConfirmLoad,
                         file_size_warning_message(path, check.size) + "; load anyway? (y/n) ");
        return;
    }
    if (offer_autosave_recovery_before_load(path, settings, minibuffer, pending_autosave_recovery)) {
        return;
    }
    load_editor_from_path(state, path, settings, minibuffer);
}

bool confirm_huge_load_before_terminal(const std::string& path, const FileLoadCheck& check) {
    std::cerr << file_size_warning_message(path, check.size)
              << ". Load anyway? [y/N] ";
    std::cerr.flush();
    std::string response;
    if (!std::getline(std::cin, response)) {
        return false;
    }
    return ui::yes_answer(trim_ascii_copy(response));
}

std::string search_found_message(const std::string& needle) {
    return "Found: " + needle;
}

std::string search_not_found_message(const std::string& needle) {
    return "Search not found: " + needle;
}

std::string replacement_count_message(size_t replacements) {
    std::ostringstream out;
    out << "Replaced " << replacements << " occurrence";
    if (replacements != 1) {
        out << "s";
    }
    return out.str();
}

std::string replace_prompt_message(const std::string& needle) {
    return "Replace '" + needle + "': Space replace | s skip | a all | Esc done";
}

void finish_replace_session(ReplaceSession& replace, MinibufferState& minibuffer, std::string message) {
    replace = ReplaceSession{};
    minibuffer_message(minibuffer, std::move(message));
}

bool find_replace_match(EditorState& state, ReplaceSession& replace, size_t start) {
    if (replace.needle.empty()) {
        return false;
    }
    const std::string haystack = state.text.str();
    if (haystack.empty()) {
        return false;
    }
    const size_t found = haystack.find(replace.needle, std::min(start, haystack.size()));
    if (found == std::string::npos) {
        replace.match_valid = false;
        return false;
    }
    replace.active = true;
    replace.match_valid = true;
    replace.match_start = found;
    state.cursor = found;
    update_preferred_column(state);
    return true;
}

void begin_replace_choices(EditorState& state, MinibufferState& minibuffer, ReplaceSession& replace) {
    minibuffer.active = false;
    minibuffer.action = MinibufferAction::None;
    minibuffer.prompt.clear();
    minibuffer.input.clear();
    if (find_replace_match(state, replace, state.cursor)) {
        minibuffer.message = replace_prompt_message(replace.needle);
    } else {
        finish_replace_session(replace, minibuffer, search_not_found_message(replace.needle));
    }
}

void submit_minibuffer(EditorState& state,
                       MinibufferState& minibuffer,
                       bool& quit,
                       std::string& last_search,
                       ReplaceSession& replace,
                       const EditorSettings& settings,
                       std::string& pending_load_path,
                       bool& pending_quit_after_save,
                       PendingSaveRequest& pending_save,
                       PendingAutosaveRecovery& pending_autosave_recovery,
                       PathCompleter& path_completer) {
    const MinibufferAction action = minibuffer.action;
    const std::string value = trim_ascii_copy(minibuffer.input);
    const std::string raw_value = minibuffer.input;
    if (action == MinibufferAction::SaveFile) {
        if (value.empty()) {
            minibuffer.prompt = pending_quit_after_save ? "Save file before quit (path required): "
                                                        : "Save file (path required): ";
            return;
        }
        request_save_editor_to_path(state,
                                    expand_user_path(value),
                                    minibuffer,
                                    true,
                                    pending_quit_after_save,
                                    quit,
                                    pending_save,
                                    settings);
        if (!pending_save.path.empty()) {
            return;
        }
        pending_quit_after_save = false;
        return;
    }
    if (action == MinibufferAction::SaveAsFile) {
        if (value.empty()) {
            minibuffer.prompt = "Save as (path required): ";
            return;
        }
        request_save_editor_to_path(state,
                                    expand_user_path(value),
                                    minibuffer,
                                    true,
                                    false,
                                    quit,
                                    pending_save,
                                    settings);
        return;
    }
    if (action == MinibufferAction::LoadFile) {
        if (value.empty()) {
            minibuffer.prompt = "Load file (path required): ";
            return;
        }
        request_load_editor_from_path(state,
                                      expand_user_path(value),
                                      settings,
                                      minibuffer,
                                      pending_load_path,
                                      pending_autosave_recovery);
        return;
    }
    if (action == MinibufferAction::Search) {
        if (value.empty()) {
            minibuffer.prompt = "Search (substring required): ";
            return;
        }
        last_search = value;
        minibuffer_message(minibuffer,
                           state.search(value) ? search_found_message(value)
                                               : search_not_found_message(value));
        return;
    }
    if (action == MinibufferAction::ReplaceSearch) {
        if (raw_value.empty()) {
            minibuffer.prompt = "Replace search (substring required): ";
            return;
        }
        replace = ReplaceSession{};
        replace.active = true;
        replace.needle = raw_value;
        last_search = raw_value;
        start_minibuffer(minibuffer, MinibufferAction::ReplaceWith, "Replace with: ");
        return;
    }
    if (action == MinibufferAction::ReplaceWith) {
        replace.replacement = raw_value;
        begin_replace_choices(state, minibuffer, replace);
        return;
    }
    if (action == MinibufferAction::ConfirmLoad) {
        if (ui::yes_answer(value)) {
            const std::string path = pending_load_path;
            pending_load_path.clear();
            if (offer_autosave_recovery_before_load(path, settings, minibuffer, pending_autosave_recovery)) {
                return;
            }
            load_editor_from_path(state, path, settings, minibuffer);
        } else if (ui::no_answer(value) || value.empty()) {
            pending_load_path.clear();
            minibuffer_message(minibuffer, "Load cancelled");
        } else {
            minibuffer.prompt = ui::kConfirmationRetryPrompt;
            minibuffer.input.clear();
        }
        return;
    }
    if (action == MinibufferAction::ConfirmAutosaveRecovery) {
        const PendingAutosaveRecovery recovery = pending_autosave_recovery;
        pending_autosave_recovery = PendingAutosaveRecovery{};
        if (ui::yes_answer(value)) {
            recover_editor_from_autosave(state,
                                         recovery.path,
                                         recovery.autosave_path,
                                         settings,
                                         minibuffer);
        } else if (ui::no_answer(value) || value.empty()) {
            load_editor_from_path(state, recovery.path, settings, minibuffer);
        } else {
            pending_autosave_recovery = recovery;
            minibuffer.prompt = ui::kConfirmationRetryPrompt;
            minibuffer.input.clear();
        }
        return;
    }
    if (action == MinibufferAction::ConfirmQuit) {
        if (ui::yes_answer(value)) {
            quit = true;
        } else if (ui::no_answer(value) || value.empty()) {
            minibuffer_message(minibuffer, "Quit cancelled");
        } else {
            minibuffer.prompt = ui::kConfirmationRetryPrompt;
            minibuffer.input.clear();
        }
        return;
    }
    if (action == MinibufferAction::ConfirmSaveOnQuit) {
        if (ui::yes_answer(value)) {
            pending_quit_after_save = true;
            start_minibuffer(minibuffer,
                             MinibufferAction::SaveFile,
                             "Save file: ",
                             "",
                             &path_completer);
        } else if (ui::no_answer(value) || value.empty()) {
            quit = true;
        } else {
            minibuffer.prompt = ui::kConfirmationRetryPrompt;
            minibuffer.input.clear();
        }
        return;
    }
    if (action == MinibufferAction::ConfirmOverwrite) {
        if (ui::yes_answer(value)) {
            const PendingSaveRequest request = pending_save;
            pending_save = PendingSaveRequest{};
            save_editor_to_path(state, request.path, minibuffer, request.update_path, settings);
            if (request.quit_after_save && !state.dirty) {
                quit = true;
            }
            pending_quit_after_save = false;
        } else {
            pending_save = PendingSaveRequest{};
            pending_quit_after_save = false;
            minibuffer_message(minibuffer, "Save cancelled");
        }
        return;
    }
    minibuffer_message(minibuffer, "");
}

bool handle_minibuffer_key(EditorState& state,
                           MinibufferState& minibuffer,
                           unsigned char ch,
                           bool& quit,
                           std::string& last_search,
                           ReplaceSession& replace,
                           const EditorSettings& settings,
                           std::string& pending_load_path,
                           bool& pending_quit_after_save,
                           PendingSaveRequest& pending_save,
                           PendingAutosaveRecovery& pending_autosave_recovery,
                           PathCompleter& path_completer) {
    if (!minibuffer.active) {
        return false;
    }
    if (is_assist_minibuffer_action(minibuffer.action)) {
        return false;
    }
    if (ch == 27 || ch == 7) {
        replace = ReplaceSession{};
        pending_load_path.clear();
        pending_autosave_recovery = PendingAutosaveRecovery{};
        pending_quit_after_save = false;
        pending_save = PendingSaveRequest{};
        path_completer.reset();
        minibuffer_message(minibuffer, "Minibuffer cancelled");
        return true;
    }
    if (ch == '\r' || ch == '\n') {
        submit_minibuffer(state,
                          minibuffer,
                          quit,
                          last_search,
                          replace,
                          settings,
                          pending_load_path,
                          pending_quit_after_save,
                          pending_save,
                          pending_autosave_recovery,
                          path_completer);
        return true;
    }
    if (ch == 127 || ch == 8) {
        if (!minibuffer.input.empty()) {
            minibuffer.input.pop_back();
            if (minibuffer_supports_path_completion(minibuffer.action)) {
                path_completer.reset();
            }
        }
        return true;
    }
    if (ch == 19 || ch == editor_key_save_as()) {
        return false;
    }
    if (minibuffer.action == MinibufferAction::ConfirmOverwrite) {
        if (ch == 'y' || ch == 'Y' || ch == 19) {
            const PendingSaveRequest request = pending_save;
            pending_save = PendingSaveRequest{};
            save_editor_to_path(state, request.path, minibuffer, request.update_path, settings);
            if (request.quit_after_save && !state.dirty) {
                quit = true;
            }
            pending_quit_after_save = false;
        } else {
            pending_save = PendingSaveRequest{};
            pending_quit_after_save = false;
            minibuffer_message(minibuffer, "Save cancelled");
        }
        return true;
    }
    if (minibuffer.action == MinibufferAction::ConfirmAutosaveRecovery) {
        const PendingAutosaveRecovery recovery = pending_autosave_recovery;
        pending_autosave_recovery = PendingAutosaveRecovery{};
        if (ch == 'y' || ch == 'Y') {
            recover_editor_from_autosave(state,
                                         recovery.path,
                                         recovery.autosave_path,
                                         settings,
                                         minibuffer);
        } else if (ch == 'n' || ch == 'N') {
            load_editor_from_path(state, recovery.path, settings, minibuffer);
        } else {
            pending_autosave_recovery = recovery;
            minibuffer.prompt = ui::kConfirmationRetryPrompt;
        }
        return true;
    }
    if (minibuffer.action == MinibufferAction::ConfirmQuit ||
        minibuffer.action == MinibufferAction::ConfirmLoad ||
        minibuffer.action == MinibufferAction::ConfirmSaveOnQuit) {
        if (ch == 'y' || ch == 'Y') {
            if (minibuffer.action == MinibufferAction::ConfirmQuit) {
                quit = true;
            } else if (minibuffer.action == MinibufferAction::ConfirmLoad) {
                const std::string path = pending_load_path;
                pending_load_path.clear();
                if (offer_autosave_recovery_before_load(path, settings, minibuffer, pending_autosave_recovery)) {
                    return true;
                }
                load_editor_from_path(state, path, settings, minibuffer);
            } else {
                pending_quit_after_save = true;
                start_minibuffer(minibuffer,
                                 MinibufferAction::SaveFile,
                                 "Save file: ",
                                 "",
                                 &path_completer);
            }
        } else if (ch == 'n' || ch == 'N') {
            if (minibuffer.action == MinibufferAction::ConfirmLoad) {
                pending_load_path.clear();
                minibuffer_message(minibuffer, "Load cancelled");
            } else if (minibuffer.action == MinibufferAction::ConfirmSaveOnQuit) {
                quit = true;
            } else {
                minibuffer_message(minibuffer, "Quit cancelled");
            }
        } else {
            minibuffer.prompt = ui::kConfirmationRetryPrompt;
        }
        return true;
    }
    if (ch == '\t') {
        if (minibuffer_supports_path_completion(minibuffer.action)) {
            const PathCompletionResult result = complete_path_input(minibuffer.input, path_completer);
            minibuffer.message = path_completion_status(result);
        } else {
            minibuffer.message = "Tab completion is not active here";
        }
        return true;
    }
    if (ch >= 0x20U) {
        minibuffer.input.push_back(static_cast<char>(ch));
        if (minibuffer_supports_path_completion(minibuffer.action)) {
            EditorState temp = EditorState::from_text(minibuffer.input);
            temp.cursor = minibuffer.input.size();
            if (!path_completer.can_cycle(temp)) {
                path_completer.reset();
            }
        }
        return true;
    }
    return true;
}

void discard_escape_sequence_tail() {
    unsigned char ch = 0;
    std::string sequence;
    while (sequence.size() < 16 && read_terminal_byte(ch, 1)) {
        sequence.push_back(static_cast<char>(ch));
        if (sequence.size() == 1 && ch == 'O') {
            continue;
        }
        if ((ch >= 'A' && ch <= 'Z') || ch == '~') {
            break;
        }
    }
}

void replace_current_match(EditorState& state, MinibufferState& minibuffer, ReplaceSession& replace) {
    if (!replace.match_valid && !find_replace_match(state, replace, state.cursor)) {
        finish_replace_session(replace, minibuffer, "Replace complete");
        return;
    }

    const size_t replaced_at = replace.match_start;
    Error err = state.replace(replaced_at, replace.needle.size(), replace.replacement);
    if (!err.ok()) {
        finish_replace_session(replace, minibuffer, err.message);
        return;
    }

    const size_t next_start = replaced_at + replace.replacement.size();
    if (find_replace_match(state, replace, next_start)) {
        minibuffer.message = replacement_count_message(1) + ". " + replace_prompt_message(replace.needle);
    } else {
        finish_replace_session(replace, minibuffer, replacement_count_message(1) + "; no more matches");
    }
}

void skip_current_match(EditorState& state, MinibufferState& minibuffer, ReplaceSession& replace) {
    if (!replace.match_valid && !find_replace_match(state, replace, state.cursor)) {
        finish_replace_session(replace, minibuffer, "Replace complete");
        return;
    }

    const size_t next_start = replace.match_start + replace.needle.size();
    if (find_replace_match(state, replace, next_start)) {
        minibuffer.message = replace_prompt_message(replace.needle);
    } else {
        finish_replace_session(replace, minibuffer, "Replace complete");
    }
}

void replace_all_remaining(EditorState& state, MinibufferState& minibuffer, ReplaceSession& replace) {
    const size_t start = replace.match_valid ? replace.match_start : state.cursor;
    size_t replacements = 0;
    Error err = state.replace_all_from(start, replace.needle, replace.replacement, replacements);
    if (!err.ok()) {
        finish_replace_session(replace, minibuffer, err.message);
        return;
    }
    finish_replace_session(replace, minibuffer, replacement_count_message(replacements));
}

bool handle_replace_key(EditorState& state,
                        MinibufferState& minibuffer,
                        ReplaceSession& replace,
                        unsigned char ch) {
    if (!replace.active || minibuffer.active) {
        return false;
    }
    if (ch == 27 || ch == 7) {
        discard_escape_sequence_tail();
        finish_replace_session(replace, minibuffer, "Replace ended");
        return true;
    }
    if (ch == ' ') {
        replace_current_match(state, minibuffer, replace);
        return true;
    }
    if (ch == 's' || ch == 'S') {
        skip_current_match(state, minibuffer, replace);
        return true;
    }
    if (ch == 'a' || ch == 'A') {
        replace_all_remaining(state, minibuffer, replace);
        return true;
    }
    minibuffer.message = replace_prompt_message(replace.needle);
    return true;
}

void render_terminal(EditorState& state,
                     const MinibufferState& minibuffer,
                     bool help_view,
                     const EditorAssistDisplay* assist_display) {
    const TerminalSize size = terminal_size();
    const int rows = std::max(3, size.rows);
    const int cols = std::max(20, size.cols);
    const int width = std::max(1, cols - 1);
    Rect panel_rect{1, 1, std::max(1, rows - 2), width};
    state.ensure_cursor_visible(panel_rect);
    const RenderedPanel panel = state.render(panel_rect);

    std::cout << "\x1b[?25l";
    for (int row = 0; row < panel_rect.height; ++row) {
        std::cout << "\x1b[" << (panel_rect.row + row) << ";" << panel_rect.col << "H\x1b[K";
        if (row < static_cast<int>(panel.lines.size())) {
            std::cout << panel.lines[static_cast<size_t>(row)];
        }
    }

    const int status_row = rows - 1;
    const int minibuffer_row = rows;
    std::cout << "\x1b[" << status_row << ";1H\x1b[7m"
              << pad_or_clip_ascii(editor_status_line(state, help_view), width) << "\x1b[0m\x1b[K";
    const bool show_assist_activity = assist_display != nullptr && assist_display->active &&
                                        assist_display->kind != tui::ActivityKind::None &&
                                        !minibuffer.active;
    const std::string minibuffer_line =
        show_assist_activity ? editor_assist_minibuffer_line(*assist_display, width)
                             : pad_or_clip_ascii(minibuffer_text(minibuffer), width);
    std::cout << "\x1b[" << minibuffer_row << ";1H" << minibuffer_line << "\x1b[K";

    int cursor_row = panel.cursor.visible ? panel_rect.row + panel.cursor.row : panel_rect.row;
    int cursor_col = panel.cursor.visible ? panel_rect.col + panel.cursor.col : panel_rect.col;
    if (minibuffer.active) {
        cursor_row = minibuffer_row;
        const size_t prompt_width = minibuffer.prompt.size();
        const size_t input_width = minibuffer.input.size();
        cursor_col = 1 + static_cast<int>(std::min<size_t>(
            static_cast<size_t>(std::max(0, width - 1)), prompt_width + input_width));
    }
    std::cout << "\x1b[" << cursor_row << ";" << cursor_col << "H\x1b[?25h";
    std::cout.flush();
}

std::string read_escape_suffix() {
    std::string sequence;
    unsigned char ch = 0;
    while (sequence.size() < 16 && read_terminal_byte(ch, 25)) {
        sequence.push_back(static_cast<char>(ch));
        if (sequence.size() == 1 && ch == 'O') {
            continue;
        }
        if ((ch >= 'A' && ch <= 'Z') || ch == '~') {
            break;
        }
    }
    return sequence;
}

void dispatch_escape_sequence(EditorState& state,
                            const std::string& sequence,
                            std::string& status,
                            const std::string& last_search) {
    if (sequence.empty()) {
        return;
    }

    MovementKeyEvent movement;
    if (parse_movement_sequence(sequence, movement)) {
        const TerminalSize size = terminal_size();
        const Rect panel_rect{1, 1, std::max(1, size.rows - 2), std::max(1, size.cols - 1)};
        state.apply_movement(movement.key, panel_rect, movement.shift, movement.alt, movement.ctrl);
        return;
    }

    if (sequence == "[3~") {
        Error err = state.erase_at_cursor();
        if (!err.ok()) {
            status = err.message;
        }
    } else if (sequence == "OR" || sequence == "[13~" || sequence == "[[C") {
        if (last_search.empty()) {
            status = "No search string; use Ctrl+F";
        } else {
            status = state.search_next(last_search) ? search_found_message(last_search)
                                                    : search_not_found_message(last_search);
        }
    } else if (sequence == "[1;2R" || sequence == "O1;2R" ||
               sequence == "[13;2~" || sequence == "[25~") {
        if (last_search.empty()) {
            status = "No search string; use Ctrl+F";
        } else {
            status = state.search_previous(last_search) ? search_found_message(last_search)
                                                        : search_not_found_message(last_search);
        }
    }
}

void handle_escape(EditorState& state, std::string& status, const std::string& last_search) {
    dispatch_escape_sequence(state, read_escape_suffix(), status, last_search);
}


}  // namespace pkchat::editor
