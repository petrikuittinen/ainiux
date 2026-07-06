#include "editor/editor.hpp"
#include "editor/ai_continue.hpp"
#include "editor/clipboard.hpp"
#include "editor/detail/editor_common.hpp"
#include "editor/editor_assist.hpp"
#include "editor/editor_help.hpp"
#include "editor/terminal_input.hpp"
#include "editor/terminal_ui.hpp"
#include "runtime/runtime.hpp"
#include "search/search.hpp"
#include "tui/activity.hpp"

#include <algorithm>
#include <functional>
#include <iostream>
#include <optional>
#include <vector>
#include <unistd.h>

namespace pkchat::editor {
namespace {

Error ensure_empty_file(const std::string& path) {
    if (path.empty()) {
        return ok_error();
    }
    if (access(path.c_str(), F_OK) == 0) {
        return ok_error();
    }
    return save_file(path, PieceTable::from_string(""));
}

struct HelpViewSession {
    bool active = false;
    EditorSnapshot saved;
    std::string saved_path;
    bool saved_dirty = false;
};

struct AssistSession {
    runtime::JobHandle job;
    runtime::EventQueue<ContinueEvent> events;
    bool active = false;
    bool streaming = false;
    bool saw_visible = false;
    AssistEditKind edit_kind = AssistEditKind::StreamInsert;
    EditorSnapshot undo_before;
    std::string provider_name;
    std::string model_name;
    std::string status_suffix;
    tui::ActivityKind activity_kind = tui::ActivityKind::None;
    size_t replace_start = 0;
    size_t replace_count = 0;
};

struct StoredAssistCommand {
    bool valid = false;
    AssistCommandKind kind = AssistCommandKind::Unknown;
    size_t command_index = 0;
    std::optional<AssistScope> scope;
    std::string custom_prompt;
    std::optional<AssistPromptMode> prompt_mode;
};

void clear_assist_session(AssistSession& session) {
    session.job.join();
    ContinueEvent event;
    while (session.events.try_pop(event)) {
    }
    session.active = false;
    session.streaming = false;
    session.saw_visible = false;
    session.edit_kind = AssistEditKind::StreamInsert;
    session.undo_before = EditorSnapshot{};
    session.provider_name.clear();
    session.model_name.clear();
    session.status_suffix.clear();
    session.activity_kind = tui::ActivityKind::None;
    session.replace_start = 0;
    session.replace_count = 0;
}

}  // namespace

int run_editor(const std::string& path,
               const std::string& save_as,
               const EditorSettings& settings,
               const AiContinueContext* ai_continue) {
    EditorState state;
    state.set_undo_limit(settings.undo_limit);
    state.path = path.empty() ? save_as : path;
    std::string status = "Ready";
    if (!path.empty() && access(path.c_str(), F_OK) == 0) {
        FileLoadCheck check;
        Error err = check_load_file_size(path, settings, check);
        if (!err.ok()) {
            std::cerr << error_code_name(err.code) << ": " << err.message << "\n";
            return 5;
        }
        if (check.should_warn && !confirm_huge_load_before_terminal(path, check)) {
            std::cerr << "Editor load cancelled: " << path << "\n";
            return 5;
        }
        err = load_file(path, settings, state.text);
        if (!err.ok()) {
            std::cerr << error_code_name(err.code) << ": " << err.message << "\n";
            return 5;
        }
        status = "Loaded";
    } else if (!path.empty()) {
        Error create_err = ensure_empty_file(path);
        if (!create_err.ok()) {
            std::cerr << error_code_name(create_err.code) << ": " << create_err.message << "\n";
            return 5;
        }
        status = "New file";
    }
    std::vector<EditorState> buffers;
    buffers.push_back(state);
    size_t active_buffer = 0;

    TerminalSession terminal;
    Error err = terminal.enter();
    if (!err.ok()) {
        std::cerr << error_code_name(err.code) << ": " << err.message << "\n";
        return err.code == ErrorCode::BadArgs ? 2 : 6;
    }

    bool quit = false;
    bool pending_quit_after_save = false;
    PendingSaveRequest pending_save;
    MinibufferState minibuffer;
    minibuffer.message = status;
    std::string last_search;
    ReplaceSession replace;
    std::string pending_load_path;
    AssistSession assist_session;
    StoredAssistCommand last_assist_command;
    AssistCompleterState assist_completer;
    PendingAssist pending_assist;
    HelpViewSession help_view;
    bool buffer_list_active = false;
    size_t buffer_list_selected = 0;
    EditorState buffer_list_view;
    bool pending_close_confirm = false;
    TerminalSize last_size = terminal_size();
    size_t activity_frame = 0;
    EditorAssistDisplay assist_display;
    auto refresh_assist_display = [&]() -> const EditorAssistDisplay* {
        assist_display.active = assist_session.active;
        assist_display.provider_name = assist_session.provider_name;
        assist_display.model_name = assist_session.model_name;
        assist_display.suffix = assist_session.status_suffix;
        assist_display.kind = assist_session.activity_kind;
        assist_display.frame = activity_frame;
        return assist_display.active && assist_display.kind != tui::ActivityKind::None ? &assist_display
                                                                                       : nullptr;
    };

    auto sync_active_buffer = [&]() {
        if (!help_view.active && active_buffer < buffers.size()) {
            buffers[active_buffer] = state;
        }
    };

    auto selected_buffer_status = [&]() {
        return "Selected buffer " + std::to_string(buffer_list_selected + 1) + "/" +
               std::to_string(buffers.size());
    };

    auto refresh_buffer_list_view = [&]() {
        sync_active_buffer();
        if (buffers.empty()) {
            buffers.push_back(EditorState{});
            buffers.back().set_undo_limit(settings.undo_limit);
            active_buffer = 0;
        }
        buffer_list_selected = std::min(buffer_list_selected, buffers.size() - 1);
        buffer_list_view = EditorState::from_text(editor_buffer_list_text(buffers, buffer_list_selected));
        buffer_list_view.path = "[buffers]";
        const size_t selected_line = std::min(buffer_list_selected + 1, buffer_list_view.text.line_count() - 1);
        buffer_list_view.cursor = buffer_list_view.text.line_start(selected_line);
        buffer_list_view.dirty = false;
        buffer_list_view.clear_undo_history();
    };

    auto render_editor = [&]() {
        if (buffer_list_active) {
            refresh_buffer_list_view();
            render_terminal(buffer_list_view, minibuffer, false, nullptr);
            return;
        }
        render_terminal(state, minibuffer, help_view.active, refresh_assist_display());
    };
    render_editor();

    auto assist_panel_rect = [&]() {
        const TerminalSize size = terminal_size();
        return Rect{1, 1, std::max(1, size.rows - 2), std::max(1, size.cols - 1)};
    };

    auto exit_help_view = [&]() {
        if (!help_view.active) {
            return;
        }
        state.restore_captured_state(help_view.saved);
        state.path = help_view.saved_path;
        state.dirty = help_view.saved_dirty;
        help_view.active = false;
        minibuffer_message(minibuffer, "Returned to editing");
    };

    auto enter_help_view = [&]() {
        if (help_view.active) {
            exit_help_view();
            return;
        }
        if (assist_session.active) {
            minibuffer_message(minibuffer, "Finish or cancel AI assist before opening help");
            return;
        }
        std::string help_text;
        Error help_error = load_editor_help_markdown(help_text);
        if (!help_error.ok()) {
            minibuffer_message(minibuffer, help_error.message);
            return;
        }
        help_view.saved = state.capture_state();
        help_view.saved_path = state.path;
        help_view.saved_dirty = state.dirty;
        help_view.active = true;
        state.text = PieceTable::from_string(std::move(help_text));
        state.cursor = 0;
        state.preferred_column = 0;
        state.scroll_line = 0;
        state.scroll_column = 0;
        state.dirty = false;
        state.clear_selection();
        state.clear_undo_history();
        minibuffer_message(minibuffer, "Help (read-only) — Esc /help or Ctrl+Q to return");
    };

    auto activate_buffer = [&](size_t index) {
        if (index >= buffers.size()) {
            return;
        }
        sync_active_buffer();
        active_buffer = index;
        state = buffers[active_buffer];
        buffer_list_active = false;
        buffer_list_selected = active_buffer;
        minibuffer_message(minibuffer,
                           "Opened buffer " + std::to_string(active_buffer + 1) + "/" +
                               std::to_string(buffers.size()) + ": " +
                               editor_buffer_display_name(state, active_buffer));
    };

    auto find_open_buffer = [&](const std::string& path) -> std::optional<size_t> {
        if (path.empty()) {
            return std::nullopt;
        }
        sync_active_buffer();
        for (size_t i = 0; i < buffers.size(); ++i) {
            if (buffers[i].path == path) {
                return i;
            }
        }
        return std::nullopt;
    };

    auto open_buffer_from_path = [&](const std::string& open_path) {
        if (const std::optional<size_t> existing = find_open_buffer(open_path)) {
            activate_buffer(*existing);
            return;
        }
        PieceTable loaded;
        Error load_error = load_file(open_path, settings, loaded);
        if (!load_error.ok()) {
            minibuffer_message(minibuffer, load_error.message);
            return;
        }
        sync_active_buffer();
        EditorState next;
        next.set_undo_limit(settings.undo_limit);
        next.text = std::move(loaded);
        next.path = open_path;
        next.cursor = 0;
        next.preferred_column = 0;
        next.scroll_line = 0;
        next.scroll_column = 0;
        next.dirty = false;
        next.clear_selection();
        next.clear_undo_history();
        buffers.push_back(next);
        active_buffer = buffers.size() - 1;
        state = next;
        buffer_list_selected = active_buffer;
        minibuffer_message(minibuffer, "Opened " + open_path);
    };

    auto request_open_buffer_from_path = [&](const std::string& open_path) {
        if (const std::optional<size_t> existing = find_open_buffer(open_path)) {
            activate_buffer(*existing);
            return;
        }
        FileLoadCheck check;
        Error check_error = check_load_file_size(open_path, settings, check);
        if (!check_error.ok()) {
            minibuffer_message(minibuffer, check_error.message);
            return;
        }
        if (check.should_warn) {
            pending_load_path = open_path;
            start_minibuffer(minibuffer,
                             MinibufferAction::ConfirmLoad,
                             "Warning: " + open_path + " is " + std::to_string(check.size) +
                                 " bytes; open anyway? (y/n) ");
            return;
        }
        open_buffer_from_path(open_path);
    };

    auto new_empty_buffer = [&]() {
        if (assist_session.active) {
            minibuffer_message(minibuffer, "Finish or cancel AI assist before creating buffers");
            return;
        }
        if (help_view.active) {
            exit_help_view();
        }
        sync_active_buffer();
        EditorState next;
        next.set_undo_limit(settings.undo_limit);
        next.text = PieceTable::from_string("");
        next.path.clear();
        next.dirty = false;
        next.clear_selection();
        next.clear_undo_history();
        buffers.push_back(next);
        active_buffer = buffers.size() - 1;
        state = next;
        buffer_list_active = false;
        buffer_list_selected = active_buffer;
        pending_close_confirm = false;
        minibuffer_message(minibuffer,
                           "New buffer " + std::to_string(active_buffer + 1) + "/" +
                               std::to_string(buffers.size()) + ": " +
                               editor_buffer_display_name(state, active_buffer));
    };

    auto enter_buffer_list = [&]() {
        if (assist_session.active) {
            minibuffer_message(minibuffer, "Finish or cancel AI assist before listing buffers");
            return;
        }
        if (help_view.active) {
            exit_help_view();
        }
        sync_active_buffer();
        buffer_list_selected = std::min(active_buffer, buffers.empty() ? size_t{0} : buffers.size() - 1);
        buffer_list_active = true;
        pending_close_confirm = false;
        minibuffer_message(minibuffer, selected_buffer_status());
    };

    auto cancel_buffer_list = [&]() {
        buffer_list_active = false;
        buffer_list_selected = active_buffer;
        minibuffer_message(minibuffer, "Buffer list cancelled");
    };

    auto close_active_buffer = [&](bool force) {
        if (assist_session.active) {
            minibuffer_message(minibuffer, "Finish or cancel AI assist before closing buffers");
            return;
        }
        if (help_view.active) {
            exit_help_view();
        }
        if (!force && state.dirty) {
            pending_close_confirm = true;
            minibuffer_message(minibuffer, "Buffer modified; close anyway? (y/n) ");
            return;
        }
        pending_close_confirm = false;
        if (buffers.size() <= 1) {
            buffers.clear();
            state = EditorState{};
            state.set_undo_limit(settings.undo_limit);
            buffers.push_back(state);
            active_buffer = 0;
            buffer_list_selected = 0;
            minibuffer_message(minibuffer, "Closed buffer; opened scratch buffer");
            return;
        }
        buffers.erase(buffers.begin() + static_cast<std::ptrdiff_t>(active_buffer));
        active_buffer = std::min(active_buffer, buffers.size() - 1);
        state = buffers[active_buffer];
        buffer_list_selected = active_buffer;
        minibuffer_message(minibuffer,
                           "Closed buffer; active " + std::to_string(active_buffer + 1) + "/" +
                               std::to_string(buffers.size()) + ": " +
                               editor_buffer_display_name(state, active_buffer));
    };

    auto handle_buffer_list_escape = [&]() {
        const std::string sequence = read_escape_suffix();
        if (sequence.empty()) {
            cancel_buffer_list();
            return;
        }
        MovementKeyEvent movement;
        if (parse_movement_sequence(sequence, movement)) {
            buffer_list_selected =
                move_editor_buffer_selection(buffer_list_selected, buffers.size(), movement.key);
            minibuffer_message(minibuffer, selected_buffer_status());
            return;
        }
    };

    auto handle_open_minibuffer_key = [&](unsigned char ch) -> bool {
        if (!minibuffer.active ||
            (minibuffer.action != MinibufferAction::LoadFile &&
             minibuffer.action != MinibufferAction::ConfirmLoad)) {
            return false;
        }
        if (ch == 27 || ch == 7) {
            pending_load_path.clear();
            minibuffer_message(minibuffer, "Open cancelled");
            return true;
        }
        if (ch == 19 || ch == editor_key_save_as()) {
            return false;
        }
        if (minibuffer.action == MinibufferAction::ConfirmLoad) {
            if (ch == 'y' || ch == 'Y') {
                const std::string path_to_open = pending_load_path;
                pending_load_path.clear();
                open_buffer_from_path(path_to_open);
            } else if (ch == 'n' || ch == 'N') {
                pending_load_path.clear();
                minibuffer_message(minibuffer, "Open cancelled");
            } else {
                minibuffer.prompt = "Type y or n: ";
                minibuffer.input.clear();
            }
            return true;
        }
        if (ch == '\r' || ch == '\n') {
            const std::string open_path = trim_ascii_copy(minibuffer.input);
            if (open_path.empty()) {
                minibuffer.prompt = "Open file (path required): ";
                return true;
            }
            request_open_buffer_from_path(open_path);
            return true;
        }
        if (ch == 127 || ch == 8) {
            if (!minibuffer.input.empty()) {
                minibuffer.input.pop_back();
            }
            return true;
        }
        if (ch == '\t') {
            minibuffer.prompt = "Tab completion disabled; enter path: ";
            return true;
        }
        if (ch >= 0x20U) {
            minibuffer.input.push_back(static_cast<char>(ch));
            return true;
        }
        return true;
    };

    auto set_assist_activity = [&](tui::ActivityKind kind, const std::string& suffix) {
        assist_session.activity_kind = kind;
        assist_session.status_suffix = suffix;
    };

    auto set_assist_minibuffer = [&](const std::string& suffix) {
        minibuffer_message(minibuffer,
                           continue_status_message(assist_session.provider_name,
                                                   assist_session.model_name,
                                                   suffix));
    };

    auto finish_assist_session = [&](const std::string& suffix,
                                     bool commit_stream_undo,
                                     const std::optional<std::string>& inplace_content) {
        assist_session.job.join();
        if (inplace_content.has_value()) {
            Error replace_error =
                state.replace(assist_session.replace_start, assist_session.replace_count, *inplace_content);
            if (!replace_error.ok()) {
                minibuffer_message(minibuffer, replace_error.message);
            } else {
                state.clear_selection();
                set_assist_minibuffer(suffix);
            }
        } else {
            if (assist_session.edit_kind == AssistEditKind::StreamInsert) {
                strip_trailing_assist_close_tag_without_undo(state);
            }
            if (commit_stream_undo && assist_session.saw_visible) {
                state.finalize_stream_edit(assist_session.undo_before);
            }
            set_assist_minibuffer(suffix);
        }
        clear_assist_session(assist_session);
    };

    auto process_assist_events = [&]() -> bool {
        bool updated = false;
        ContinueEvent event;
        while (assist_session.events.try_pop(event)) {
            updated = true;
            switch (event.type) {
                case ContinueEventType::Thinking:
                    set_assist_activity(tui::ActivityKind::Thinking, "thinking... ESC to abort");
                    break;
                case ContinueEventType::Writing:
                    set_assist_activity(tui::ActivityKind::Streaming, "writing. Press ESC to stop.");
                    break;
                case ContinueEventType::Delta:
                    assist_session.saw_visible = true;
                    set_assist_activity(tui::ActivityKind::Streaming, "writing. Press ESC to stop.");
                    if (Error insert_error = state.insert_without_undo(event.text); !insert_error.ok()) {
                        assist_session.job.cancel();
                        assist_session.job.join();
                        minibuffer_message(minibuffer, insert_error.message);
                        clear_assist_session(assist_session);
                        return true;
                    }
                    state.ensure_cursor_visible(assist_panel_rect());
                    break;
                case ContinueEventType::Done:
                    if (!event.chat.model.empty()) {
                        assist_session.model_name = event.chat.model;
                    }
                    if (assist_session.edit_kind == AssistEditKind::ReplaceInPlace) {
                        finish_assist_session(
                            continue_completion_status_suffix(event.chat, assist_session.streaming, "ready"),
                            false,
                            trim_assist_inplace_response(event.chat.content));
                    } else {
                        finish_assist_session(
                            continue_completion_status_suffix(event.chat,
                                                              assist_session.streaming,
                                                              "stopped and ready"),
                            true,
                            std::nullopt);
                    }
                    return true;
                case ContinueEventType::Error:
                    if (event.error.code == ErrorCode::Cancelled) {
                        if (assist_session.edit_kind == AssistEditKind::ReplaceInPlace) {
                            assist_session.job.join();
                            set_assist_minibuffer("stopped and ready");
                            clear_assist_session(assist_session);
                        } else {
                            finish_assist_session("stopped and ready", true, std::nullopt);
                        }
                    } else {
                        assist_session.job.join();
                        minibuffer_message(minibuffer, event.error.message);
                        clear_assist_session(assist_session);
                    }
                    return true;
            }
        }
        return updated;
    };

    auto start_assist = [&](AssistCommandKind kind,
                            size_t command_index,
                            std::optional<AssistScope> scope,
                            const std::string& custom_prompt,
                            std::optional<AssistPromptMode> prompt_mode) {
        if (assist_session.active) {
            return;
        }
        if (minibuffer.active && !is_assist_minibuffer_action(minibuffer.action)) {
            return;
        }
        if (ai_continue == nullptr) {
            minibuffer_message(minibuffer,
                               "AI assist requires a provider; use --provider lmstudio -m MODEL");
            return;
        }
        Error validation = validate_continue_request(*ai_continue);
        if (!validation.ok()) {
            minibuffer_message(minibuffer, validation.message);
            return;
        }

        AssistExecution execution = build_assist_execution(
            state, *ai_continue, kind, command_index, scope, custom_prompt, prompt_mode);
        if (!execution.ok) {
            minibuffer_message(minibuffer, execution.error_message);
            return;
        }

        if (kind == AssistCommandKind::Configured || kind == AssistCommandKind::Prompt) {
            last_assist_command.valid = true;
            last_assist_command.kind = kind;
            last_assist_command.command_index = command_index;
            last_assist_command.scope = scope;
            last_assist_command.custom_prompt = custom_prompt;
            last_assist_command.prompt_mode = prompt_mode;
        }

        pending_assist = PendingAssist{};
        exit_assist_command_mode(minibuffer, assist_completer);
        clear_assist_session(assist_session);
        assist_session.active = true;
        assist_session.streaming = execution.stream;
        assist_session.edit_kind = execution.edit_kind;
        assist_session.provider_name = ai_continue->request.profile.name;
        assist_session.model_name = ai_continue->request.options.model;
        assist_session.replace_start = execution.replace_start;
        assist_session.replace_count = execution.replace_count;
        if (execution.edit_kind == AssistEditKind::StreamInsert) {
            assist_session.undo_before = state.capture_state();
        }
        state.clear_selection();
        start_assist_job(*ai_continue, execution.messages, execution.stream, assist_session.events, assist_session.job);
        set_assist_activity(tui::ActivityKind::Thinking, "thinking... ESC to abort");
    };

    auto trigger_save = [&]() {
        if (state.path.empty()) {
            start_minibuffer(minibuffer, MinibufferAction::SaveFile, "Save file: ");
        } else {
            request_save_editor_to_path(state, state.path, minibuffer, true, false, quit, pending_save);
        }
    };

    auto submit_assist_command = [&]() {
        if (is_editor_help_command(minibuffer.input)) {
            exit_assist_command_mode(minibuffer, assist_completer);
            enter_help_view();
            return;
        }
        const ParsedEditorSlashCommand slash = parse_editor_slash_command(minibuffer.input);
        switch (slash.command) {
            case EditorSlashCommand::Save:
                pending_assist = PendingAssist{};
                exit_assist_command_mode(minibuffer, assist_completer);
                trigger_save();
                return;
            case EditorSlashCommand::SaveAs:
                pending_assist = PendingAssist{};
                exit_assist_command_mode(minibuffer, assist_completer);
                if (slash.path.empty()) {
                    start_minibuffer(minibuffer, MinibufferAction::SaveAsFile, "Save as: ", state.path);
                } else {
                    request_save_editor_to_path(state, slash.path, minibuffer, true, false, quit, pending_save);
                }
                return;
            case EditorSlashCommand::Find:
                pending_assist = PendingAssist{};
                exit_assist_command_mode(minibuffer, assist_completer);
                start_minibuffer(minibuffer, MinibufferAction::Search, "Search: ", last_search);
                return;
            case EditorSlashCommand::Replace:
                pending_assist = PendingAssist{};
                exit_assist_command_mode(minibuffer, assist_completer);
                start_minibuffer(minibuffer, MinibufferAction::ReplaceSearch, "Replace search: ", last_search);
                return;
            case EditorSlashCommand::Open:
                pending_assist = PendingAssist{};
                exit_assist_command_mode(minibuffer, assist_completer);
                if (slash.path.empty()) {
                    start_minibuffer(minibuffer, MinibufferAction::LoadFile, "Open file: ");
                } else {
                    request_open_buffer_from_path(slash.path);
                }
                return;
            case EditorSlashCommand::New:
                pending_assist = PendingAssist{};
                exit_assist_command_mode(minibuffer, assist_completer);
                new_empty_buffer();
                return;
            case EditorSlashCommand::List:
                pending_assist = PendingAssist{};
                exit_assist_command_mode(minibuffer, assist_completer);
                enter_buffer_list();
                return;
            case EditorSlashCommand::Close:
                pending_assist = PendingAssist{};
                exit_assist_command_mode(minibuffer, assist_completer);
                close_active_buffer(false);
                return;
            case EditorSlashCommand::None:
                break;
        }
        const ParsedAssistCommand parsed =
            parse_assist_command(minibuffer.input, ai_continue == nullptr ? default_editor_assist_config()
                                                                          : ai_continue->assist_config);
        if (!parsed.ok) {
            minibuffer_message(minibuffer, parsed.error_message);
            return;
        }
        if (parsed.kind == AssistCommandKind::Configured) {
            const EditorAssistConfig& assist_config =
                ai_continue == nullptr ? default_editor_assist_config() : ai_continue->assist_config;
            if (parsed.command_index >= assist_config.commands.size()) {
                minibuffer_message(minibuffer, "Configured assist command index is out of range");
                return;
            }
            const EditorAssistCommand& command = assist_config.commands[parsed.command_index];
            if (assist_command_requires_scope(command) && !parsed.scope.has_value()) {
                pending_assist.kind = AssistCommandKind::Configured;
                pending_assist.command_index = parsed.command_index;
                start_minibuffer(minibuffer,
                                 MinibufferAction::AssistScopeChoice,
                                 assist_scope_prompt(command));
                return;
            }
            start_assist(AssistCommandKind::Configured, parsed.command_index, parsed.scope, "", std::nullopt);
            return;
        }
        if (parsed.kind == AssistCommandKind::Prompt) {
            pending_assist.kind = parsed.kind;
            pending_assist.custom_prompt = parsed.custom_prompt;
            start_minibuffer(minibuffer, MinibufferAction::AssistPromptMode, assist_prompt_mode_message());
            return;
        }
        if (parsed.kind == AssistCommandKind::Regenerate) {
            if (!last_assist_command.valid) {
                minibuffer_message(minibuffer, "No previous AI command to regenerate");
                return;
            }
            start_assist(last_assist_command.kind,
                         last_assist_command.command_index,
                         last_assist_command.scope,
                         last_assist_command.custom_prompt,
                         last_assist_command.prompt_mode);
            return;
        }
        if (parsed.kind == AssistCommandKind::Quit) {
            pending_assist = PendingAssist{};
            exit_assist_command_mode(minibuffer, assist_completer);
            quit = true;
            return;
        }
        if (parsed.kind == AssistCommandKind::WebSearch) {
            search::Options search_options =
                ai_continue == nullptr ? search::default_options()
                                       : search::options_for(ai_continue->request.options);
            search::SearchResponse response;
            Error search_error = search::search(parsed.custom_prompt, search_options, response);
            if (!search_error.ok()) {
                minibuffer_message(minibuffer, search_error.message);
                return;
            }
            const std::string text = search::format_context_message(parsed.custom_prompt, response);
            Error insert_error = state.insert(text + "\n");
            if (!insert_error.ok()) {
                minibuffer_message(minibuffer, insert_error.message);
                return;
            }
            pending_assist = PendingAssist{};
            exit_assist_command_mode(minibuffer, assist_completer);
            minibuffer_message(minibuffer,
                               "Inserted web search results from " + response.provider_used);
            return;
        }
        start_assist(parsed.kind, parsed.command_index, parsed.scope, parsed.custom_prompt, std::nullopt);
    };

    auto start_continue = [&]() {
        if (ai_continue == nullptr) {
            return;
        }
        const std::optional<size_t> command_index =
            assist_command_index(ai_continue->assist_config, "/continue");
        if (!command_index.has_value()) {
            minibuffer_message(minibuffer, "Configured editor assist commands are missing /continue");
            return;
        }
        start_assist(AssistCommandKind::Configured,
                     *command_index,
                     AssistScope::Continue,
                     "",
                     std::nullopt);
    };

    std::function<void(unsigned char)> handle_key;
    handle_key = [&](unsigned char ch) {
        if (help_view.active) {
            if (handle_minibuffer_key(state,
                                      minibuffer,
                                      ch,
                                      quit,
                                      last_search,
                                      replace,
                                      settings,
                                      pending_load_path,
                                      pending_quit_after_save,
                                      pending_save)) {
                return;
            }
            if (minibuffer.active && is_assist_minibuffer_action(minibuffer.action)) {
                if (ch == 27 || ch == 7) {
                    pending_assist = PendingAssist{};
                    exit_assist_command_mode(minibuffer, assist_completer);
                    return;
                }
                if (minibuffer.action == MinibufferAction::AssistCommand) {
                    if (ch == '\t') {
                        const AssistCompletionResult result = complete_assist_command(
                            minibuffer.input,
                            assist_completer,
                            ai_continue == nullptr ? default_editor_assist_config() : ai_continue->assist_config);
                        minibuffer.message = assist_completion_status(result);
                        return;
                    }
                    if (ch == '\r' || ch == '\n') {
                        submit_assist_command();
                        return;
                    }
                    if (ch == 127 || ch == 8) {
                        if (!minibuffer.input.empty()) {
                            minibuffer.input.pop_back();
                            assist_completer = AssistCompleterState{};
                        }
                        return;
                    }
                    if (ch >= 0x20U) {
                        minibuffer.input.push_back(static_cast<char>(ch));
                        assist_completer = AssistCompleterState{};
                        return;
                    }
                    return;
                }
                return;
            }
            if (ch == 17) {
                exit_help_view();
                return;
            }
            if (ch == 27) {
                const std::string sequence = read_escape_suffix();
                if (!sequence.empty()) {
                    unsigned char decoded = 0;
                    if (decode_control_key_sequence(sequence, decoded)) {
                        handle_key(decoded);
                        return;
                    }
                    std::string escape_status;
                    dispatch_escape_sequence(state, sequence, escape_status, last_search);
                    if (!escape_status.empty()) {
                        minibuffer_message(minibuffer, escape_status);
                    }
                    return;
                }
                start_assist_command_mode(minibuffer, assist_completer);
                return;
            }
            if (ch == 6 || ch == 8) {
                start_minibuffer(minibuffer, MinibufferAction::Search, "Search: ", last_search);
                return;
            }
            return;
        }
        if (assist_session.active) {
            if (ch == 27) {
                assist_session.job.cancel();
                return;
            }
            return;
        }
        if (buffer_list_active) {
            if (ch == 17) {
                quit = true;
                return;
            }
            if (ch == 27) {
                handle_buffer_list_escape();
                return;
            }
            if (ch == '\r' || ch == '\n') {
                activate_buffer(buffer_list_selected);
                return;
            }
            return;
        }
        if (pending_close_confirm) {
            if (ch == 'y' || ch == 'Y') {
                close_active_buffer(true);
            } else if (ch == 27 || ch == 'n' || ch == 'N') {
                if (ch == 27) {
                    read_escape_suffix();
                }
                pending_close_confirm = false;
                minibuffer_message(minibuffer, "Close cancelled");
            } else {
                minibuffer_message(minibuffer, "Type y or n: ");
            }
            return;
        }
        if (minibuffer.active && is_assist_minibuffer_action(minibuffer.action)) {
            if (ch == 27 || ch == 7) {
                pending_assist = PendingAssist{};
                exit_assist_command_mode(minibuffer, assist_completer);
                return;
            }
            if (minibuffer.action == MinibufferAction::AssistCommand) {
                if (ch == '\t') {
                    const AssistCompletionResult result = complete_assist_command(
                        minibuffer.input,
                        assist_completer,
                        ai_continue == nullptr ? default_editor_assist_config() : ai_continue->assist_config);
                    minibuffer.message = assist_completion_status(result);
                    return;
                }
                if (ch == '\r' || ch == '\n') {
                    submit_assist_command();
                    return;
                }
                if (ch == 127 || ch == 8) {
                    if (!minibuffer.input.empty()) {
                        minibuffer.input.pop_back();
                        assist_completer = AssistCompleterState{};
                    }
                    return;
                }
                if (ch >= 0x20U) {
                    minibuffer.input.push_back(static_cast<char>(ch));
                    assist_completer = AssistCompleterState{};
                    return;
                }
                return;
            }
            if (minibuffer.action == MinibufferAction::AssistScopeChoice) {
                if (ch == 's' || ch == 'S') {
                    start_assist(pending_assist.kind,
                                 pending_assist.command_index,
                                 AssistScope::Selection,
                                 "",
                                 std::nullopt);
                } else if (ch == 'a' || ch == 'A') {
                    start_assist(pending_assist.kind,
                                 pending_assist.command_index,
                                 AssistScope::All,
                                 "",
                                 std::nullopt);
                } else if (ch == 'c' || ch == 'C') {
                    start_assist(pending_assist.kind,
                                 pending_assist.command_index,
                                 AssistScope::Continue,
                                 "",
                                 std::nullopt);
                } else if (ch == 'i' || ch == 'I' || ch == 'l' || ch == 'L') {
                    start_assist(pending_assist.kind,
                                 pending_assist.command_index,
                                 AssistScope::Insert,
                                 "",
                                 std::nullopt);
                }
                return;
            }
            if (minibuffer.action == MinibufferAction::AssistPromptMode) {
                if (ch == 'c' || ch == 'C') {
                    start_assist(pending_assist.kind,
                                 pending_assist.command_index,
                                 std::nullopt,
                                 pending_assist.custom_prompt,
                                 AssistPromptMode::Continue);
                } else if (ch == 's' || ch == 'S') {
                    start_assist(pending_assist.kind,
                                 pending_assist.command_index,
                                 std::nullopt,
                                 pending_assist.custom_prompt,
                                 AssistPromptMode::Selection);
                } else if (ch == 'a' || ch == 'A') {
                    start_assist(pending_assist.kind,
                                 pending_assist.command_index,
                                 std::nullopt,
                                 pending_assist.custom_prompt,
                                 AssistPromptMode::All);
                } else if (ch == 'i' || ch == 'I' || ch == 'l' || ch == 'L') {
                    start_assist(pending_assist.kind,
                                 pending_assist.command_index,
                                 std::nullopt,
                                 pending_assist.custom_prompt,
                                 AssistPromptMode::Insert);
                }
                return;
            }
            return;
        }
        if (handle_open_minibuffer_key(ch) ||
            handle_minibuffer_key(state,
                                  minibuffer,
                                  ch,
                                  quit,
                                  last_search,
                                  replace,
                                  settings,
                                  pending_load_path,
                                  pending_quit_after_save,
                                  pending_save)) {
            return;
        }
        if (handle_replace_key(state, minibuffer, replace, ch)) {
            return;
        }

        if (ch == 17) {
            if (help_view.active) {
                exit_help_view();
                return;
            }
            sync_active_buffer();
            const bool modified_buffers = std::any_of(buffers.begin(), buffers.end(), [](const EditorState& buffer) {
                return buffer.dirty;
            });
            if (buffers.size() > 1 && modified_buffers) {
                start_minibuffer(minibuffer,
                                 MinibufferAction::ConfirmQuit,
                                 "Modified buffers exist; quit anyway? (y/n) ");
            } else if (state.path.empty()) {
                if (state.dirty || !state.text.empty()) {
                    start_minibuffer(minibuffer,
                                     MinibufferAction::ConfirmSaveOnQuit,
                                     "Modified buffer exists; save before quit? (y/n) ");
                } else {
                    quit = true;
                }
            } else if (state.dirty) {
                start_minibuffer(minibuffer, MinibufferAction::ConfirmQuit,
                                 "Buffer modified; quit anyway? (y/n) ");
            } else {
                quit = true;
            }
        } else if (ch == 3) {
            Error copy_error = state.copy_selection(shared_clipboard());
            minibuffer_message(minibuffer, copy_error.ok() ? "Copied selection" : copy_error.message);
        } else if (ch == 24) {
            Error cut_error = state.cut_selection(shared_clipboard());
            minibuffer_message(minibuffer, cut_error.ok() ? "Cut selection" : cut_error.message);
        } else if (ch == 22) {
            Error paste_error = paste_with_clipboard_preference(state, shared_clipboard(), "");
            minibuffer_message(minibuffer, paste_error.ok() ? "Pasted" : paste_error.message);
        } else if (ch == 6) {
            start_minibuffer(minibuffer, MinibufferAction::Search, "Search: ", last_search);
        } else if (ch == 8) {
            start_minibuffer(minibuffer, MinibufferAction::ReplaceSearch, "Replace search: ", last_search);
        } else if (ch == 21) {
            minibuffer_message(minibuffer, state.undo() ? "Undone" : "Nothing to undo");
        } else if (ch == 18) {
            minibuffer_message(minibuffer, state.redo() ? "Redone" : "Nothing to redo");
        } else if (ch == 1) {
            state.select_all();
        } else if (ch == 11) {
            Error kill_error = state.kill_to_line_end();
            if (!kill_error.ok()) {
                minibuffer_message(minibuffer, kill_error.message);
            }
        } else if (ch == 14) {
            new_empty_buffer();
        } else if (ch == 15) {
            start_minibuffer(minibuffer, MinibufferAction::LoadFile, "Open file: ");
        } else if (ch == 0) {
            start_continue();
        } else if (ch == 19) {
            trigger_save();
        } else if (ch == 12) {
            enter_buffer_list();
        } else if (ch == 23) {
            close_active_buffer(false);
        } else if (ch == editor_key_save_as()) {
            start_minibuffer(minibuffer, MinibufferAction::SaveAsFile, "Save as: ", state.path);
        } else if (ch == '\t') {
            minibuffer_message(minibuffer, "Tab completion is disabled in editor mode");
        } else if (ch == 27) {
            if (!minibuffer.active && !replace.active && !assist_session.active) {
                const std::string sequence = read_escape_suffix();
                if (!sequence.empty()) {
                    unsigned char decoded = 0;
                    if (decode_control_key_sequence(sequence, decoded)) {
                        handle_key(decoded);
                        return;
                    }
                    std::string escape_status;
                    dispatch_escape_sequence(state, sequence, escape_status, last_search);
                    if (!escape_status.empty()) {
                        minibuffer_message(minibuffer, escape_status);
                    }
                    return;
                }
                start_assist_command_mode(minibuffer, assist_completer);
                return;
            }
            std::string escape_status;
            handle_escape(state, escape_status, last_search);
            if (!escape_status.empty()) {
                minibuffer_message(minibuffer, escape_status);
            }
        } else if (ch == 127) {
            Error erase_error = state.erase_before_cursor();
            if (!erase_error.ok()) {
                minibuffer_message(minibuffer, erase_error.message);
            }
        } else if (ch == '\r' || ch == '\n') {
            Error insert_error = state.insert("\n");
            if (!insert_error.ok()) {
                minibuffer_message(minibuffer, insert_error.message);
            }
        } else if (ch >= 0x20U) {
            const std::string text(1, static_cast<char>(ch));
            Error insert_error = state.insert(text);
            if (!insert_error.ok()) {
                minibuffer_message(minibuffer, insert_error.message);
            }
        }
    };

    auto handle_paste = [&](const std::string& terminal_text) {
        if (buffer_list_active) {
            minibuffer_message(minibuffer, "Choose a buffer or press Esc to cancel");
            return;
        }
        if (pending_close_confirm) {
            minibuffer_message(minibuffer, "Type y or n: ");
            return;
        }
        Error paste_error = paste_with_clipboard_preference(state, shared_clipboard(), terminal_text);
        minibuffer_message(minibuffer, paste_error.ok() ? "Pasted" : paste_error.message);
    };

    while (!quit) {
        const bool assist_updated = process_assist_events();
        const bool assist_animating =
            assist_session.active && assist_session.activity_kind != tui::ActivityKind::None;
        if (assist_animating) {
            ++activity_frame;
        }

        TerminalInputEvent event;
        if (!read_terminal_input(event, 100)) {
            const TerminalSize current_size = terminal_size();
            if (current_size.rows != last_size.rows || current_size.cols != last_size.cols ||
                assist_session.job.running() || assist_updated || assist_animating) {
                last_size = current_size;
                render_editor();
            }
            continue;
        }

        if (event.type == TerminalInputType::BracketedPaste) {
            handle_paste(event.text);
        } else if (event.type == TerminalInputType::Byte) {
            handle_key(event.byte);
            while (!quit) {
                if (!read_terminal_input(event, 0)) {
                    break;
                }
                if (event.type == TerminalInputType::BracketedPaste) {
                    handle_paste(event.text);
                } else if (event.type == TerminalInputType::Byte) {
                    handle_key(event.byte);
                } else {
                    break;
                }
            }
        }

        last_size = terminal_size();
        render_editor();
    }
    sync_active_buffer();
    return 0;
}

}  // namespace pkchat::editor
