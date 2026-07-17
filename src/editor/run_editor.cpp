#include "app/interactive_mode.hpp"
#include "common.hpp"
#include "editor/autosave.hpp"
#include "editor/editor.hpp"
#include "editor/ai_continue.hpp"
#include "editor/editor_ai_setup.hpp"
#include "editor/clipboard.hpp"
#include "editor/detail/editor_common.hpp"
#include "editor/assist_runtime.hpp"
#include "editor/editor_assist.hpp"
#include "editor/editor_help.hpp"
#include "editor/editor_picker.hpp"
#include "editor/model_list_runtime.hpp"
#include "editor/path_completion.hpp"
#include "editor/reformat.hpp"
#include "editor/split.hpp"
#include "editor/terminal_input.hpp"
#include "editor/terminal_ui.hpp"
#include "input/input.hpp"
#include "runtime/runtime.hpp"
#include "search/search.hpp"
#include "tui/activity.hpp"
#include "tui/events.hpp"
#include "tui/theme_registry.hpp"
#include "tui/tui.hpp"
#include "ui/confirmation.hpp"
#include "ui/text_selector.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <functional>
#include <iostream>
#include <optional>
#include <vector>
#include <unistd.h>

namespace ainiux::editor {
namespace {

struct HelpViewSession {
    bool active = false;
    EditorSnapshot saved;
    std::string saved_path;
    bool saved_dirty = false;
    highlight::Language saved_language = highlight::Language::Text;
    bool saved_language_automatic = true;
};

struct InsertEvent {
    Error error;
    input::InsertSource inserted;
};

struct InsertSession {
    bool active = false;
    bool cancel_requested = false;
    std::uint64_t buffer_id = 0;
    std::uint64_t revision = 0;
    size_t position = 0;
    std::string source;
    runtime::EventQueue<InsertEvent> events;
    // Join the worker before destroying the event queue it references.
    runtime::JobHandle job;
};

std::string canonical_editor_command_line(const std::string& line) {
    std::string normalized = trim_ascii_copy(line);
    if (normalized.empty()) {
        return normalized;
    }
    if (normalized.front() != '/') {
        normalized.insert(normalized.begin(), '/');
    }
    size_t command_end = 1;
    while (command_end < normalized.size() && normalized[command_end] != ' ' &&
           normalized[command_end] != '\t' && normalized[command_end] != '\r' &&
           normalized[command_end] != '\n') {
        normalized[command_end] = static_cast<char>(
            std::tolower(static_cast<unsigned char>(normalized[command_end])));
        ++command_end;
    }
    return normalized;
}

}  // namespace

app::EditorRunResult run_editor(const std::string& path,
                                const std::string& save_as,
                                const EditorSettings& settings,
                                std::optional<AiContinueContext> ai_continue,
                                const EditorAssistConfig& assist_config,
                                app::InteractiveSession* interactive) {
    EditorState state;
    state.set_undo_limit(settings.undo_limit);
    state.tab_width = settings.tab_width;
    state.tab_style = settings.tab_style;
    state.linebreak = settings.linebreak;
    bool highlight_enabled = interactive != nullptr ? interactive->highlight_enabled
                                                    : settings.highlight_enabled;
    state.highlight_enabled = highlight_enabled;
    const std::string initial_path = expand_user_path(path.empty() ? save_as : path);
    state.set_path(initial_path);
    std::string status = "Ready";
    std::string initial_linebreak_warning;
    std::string initial_lock_warning;
    bool switch_to_chat = false;
    std::vector<EditorState> buffers;
    size_t active_buffer = 0;
    SplitLayout split_layout;
    bool window_prefix_active = false;
    const bool restore_editor_buffers =
        interactive != nullptr && interactive->editor_buffers_initialized &&
        !interactive->editor_buffers.empty();
    if (restore_editor_buffers) {
        buffers = std::move(interactive->editor_buffers);
        active_buffer = std::min(interactive->editor_active_buffer, buffers.size() - 1);
        interactive->editor_buffers_initialized = false;
        interactive->editor_buffers.clear();
        interactive->editor_active_buffer = 0;
        state = buffers[active_buffer];
        state.highlight_enabled = highlight_enabled;
        split_layout.reset(active_buffer);
        status = "Editor";
    } else {
        const bool initial_target_exists =
            !initial_path.empty() && access(initial_path.c_str(), F_OK) == 0;
        if (!initial_path.empty()) {
            Error lock_error = state.begin_file_session(initial_path, initial_target_exists);
            if (!lock_error.ok()) {
                if (!initial_target_exists) {
                    std::cerr << error_code_name(lock_error.code) << ": "
                              << lock_error.message << "\n";
                    return {5, app::InteractiveUiTarget::Quit};
                }
                initial_lock_warning = lock_error.message;
            }
        }
        if (initial_target_exists) {
            FileLoadCheck check;
            Error err = check_load_file_size(initial_path, settings, check);
            if (!err.ok()) {
                std::cerr << error_code_name(err.code) << ": " << err.message << "\n";
                return {5, app::InteractiveUiTarget::Quit};
            }
            if (check.should_warn && !confirm_huge_load_before_terminal(initial_path, check)) {
                std::cerr << "Editor load cancelled: " << initial_path << "\n";
                return {5, app::InteractiveUiTarget::Quit};
            }
            const AutosaveRecoveryOffer recovery_offer =
                check_autosave_recovery_offer(initial_path, settings);
            const bool recover_autosave = recovery_offer.should_offer &&
                                          confirm_autosave_recovery_before_terminal(
                                              initial_path, recovery_offer.autosave_path);
            const std::string& load_path =
                recover_autosave ? recovery_offer.autosave_path : initial_path;
            LoadedFile loaded;
            err = load_file(load_path, settings, loaded);
            if (!err.ok()) {
                std::cerr << error_code_name(err.code) << ": " << err.message << "\n";
                return {5, app::InteractiveUiTarget::Quit};
            }
            state.text = std::move(loaded.text);
            state.invalidate_word_index();
            state.linebreak = loaded.linebreak;
            state.tab_width = loaded.tab_width;
            state.tab_style = loaded.tab_style;
            if (loaded.mixed_linebreaks) {
                initial_linebreak_warning =
                    "Warning: mixed line endings in " + load_path +
                    "; normalized and using " + linebreak_name(state.linebreak) + " for saves";
            }
            if (recover_autosave) {
                state.dirty = true;
                status = "Recovered auto-save";
            } else {
                status = "Loaded";
            }
            buffers.push_back(state);
        } else {
            if (!initial_path.empty()) {
                Error create_err = ensure_empty_file(initial_path);
                if (!create_err.ok()) {
                    std::cerr << error_code_name(create_err.code) << ": " << create_err.message
                              << "\n";
                    return {5, app::InteractiveUiTarget::Quit};
                }
                // Detection is path-based and must not depend on the file having
                // existed before editor startup.
                if (state.language_automatic) {
                    state.redetect_language();
                }
                status = "New file";
                (void)state.refresh_disk_fingerprint();
            }
            buffers.push_back(state);
        }
    }

    TerminalSession terminal;
    Error err = terminal.enter();
    if (!err.ok()) {
        std::cerr << error_code_name(err.code) << ": " << err.message << "\n";
        const int exit_code = err.code == ErrorCode::BadArgs ? 2 : 6;
        return {exit_code, app::InteractiveUiTarget::Quit};
    }

    bool quit = false;
    bool regenerate_after_cancel = false;
    bool pending_regenerate_restart = false;
    bool pending_quit_after_save = false;
    PendingSaveRequest pending_save;
    MinibufferState minibuffer;
    minibuffer.message = !initial_lock_warning.empty()
                             ? initial_lock_warning
                             : initial_linebreak_warning.empty() ? editor_startup_status(ai_continue)
                                                                 : initial_linebreak_warning;
    std::string last_search;
    ReplaceSession replace;
    std::string pending_load_path;
    PendingAutosaveRecovery pending_autosave_recovery;
    AssistSession assist_session;
    ReformatSession reformat_session;
    InsertSession insert_session;
    StoredAssistCommand last_assist_command;
    AssistCompleterState assist_completer;
    PathCompleter minibuffer_path_completer;
    WordCompleter word_completer;
    PendingAssist pending_assist;
    HelpViewSession help_view;
    bool buffer_list_active = false;
    size_t buffer_list_selected = 0;
    int buffer_list_scroll = 0;
    EditorState buffer_list_view;
    EditorProviderModelPicker picker;
    EditorModelListRuntime model_list;
    bool pending_close_confirm = false;
    size_t pending_close_index = static_cast<size_t>(-1);
    TerminalSize last_size = terminal_size();
    size_t activity_frame = 0;
    std::string theme_name = settings.theme_name;
    input::InsertSourceOptions insert_options;
    const cli::Options* runtime_options = nullptr;
    if (interactive != nullptr) {
        runtime_options = &interactive->context.options;
    } else if (ai_continue.has_value()) {
        runtime_options = &ai_continue->request.options;
    }
    if (runtime_options != nullptr) {
        insert_options.max_file_bytes = runtime_options->max_input_bytes > 0
                                            ? static_cast<size_t>(runtime_options->max_input_bytes)
                                            : 0;
        insert_options.fetch.connect_timeout_seconds = runtime_options->connect_timeout_seconds;
        insert_options.fetch.timeout_seconds = runtime_options->timeout_seconds > 0
                                                   ? runtime_options->timeout_seconds
                                                   : 30;
        insert_options.fetch.max_bytes = runtime_options->max_fetch_bytes;
        insert_options.fetch.proxy = runtime_options->proxy;
        insert_options.fetch.insecure_tls = runtime_options->insecure_tls;
        insert_options.fetch.trace_http = runtime_options->trace_http;
        insert_options.fetch.allow_private = runtime_options->allow_private_url_fetch;
        insert_options.auto_convert_html_to_markdown =
            runtime_options->auto_convert_html_to_markdown;
    }
    if (settings.themes != nullptr) {
        settings.themes->normalize_name(theme_name, theme_name);
    }
    auto terminal_theme_style = [&]() {
        return TerminalThemeStyle{settings.themes, theme_name, settings.use_colors};
    };
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

    auto focus_buffer_from_split = [&]() {
        if (buffers.empty()) {
            return;
        }
        const size_t next = std::min(split_layout.focused_buffer(), buffers.size() - 1);
        if (next == active_buffer) {
            split_layout.set_focused_buffer(active_buffer);
            return;
        }
        sync_active_buffer();
        active_buffer = next;
        state = buffers[active_buffer];
        state.highlight_enabled = highlight_enabled;
        split_layout.set_focused_buffer(active_buffer);
    };

    auto can_switch_to_chat = [&]() {
        if (interactive == nullptr) {
            return false;
        }
        return !help_view.active && !picker.active && !buffer_list_active && !pending_close_confirm &&
               !minibuffer.active && !replace.active && !assist_session.active &&
               !reformat_session.active && !insert_session.active && !window_prefix_active;
    };

    auto request_switch_to_chat = [&]() {
        if (!can_switch_to_chat()) {
            if (interactive != nullptr) {
                minibuffer_message(minibuffer, "Cannot switch to chat right now");
            }
            return;
        }
        clear_assist_session(assist_session);
        model_list.job.cancel();
        model_list.job.join();
        sync_active_buffer();
        interactive->editor_buffers = std::move(buffers);
        interactive->editor_active_buffer = active_buffer;
        interactive->editor_buffers_initialized = true;
        interactive->editor_path = path;
        interactive->editor_save_as = save_as;
        interactive->editor_settings = settings;
        interactive->assist_config = assist_config;
        interactive->highlight_enabled = highlight_enabled;
        interactive->ai_continue = ai_continue;
        app::sync_editor_provider_to_shared(*interactive, ai_continue);
        switch_to_chat = true;
        quit = true;
    };

    auto selected_buffer_status = [&]() {
        return ui::text_selector_status("Selected buffer", buffer_list_selected, buffers.size());
    };

    auto refresh_buffer_list_view = [&]() {
        sync_active_buffer();
        if (buffers.empty()) {
            buffers.push_back(EditorState{});
            buffers.back().set_undo_limit(settings.undo_limit);
            buffers.back().tab_width = settings.tab_width;
            buffers.back().tab_style = settings.tab_style;
            buffers.back().linebreak = settings.linebreak;
            active_buffer = 0;
        }
        buffer_list_selected = std::min(buffer_list_selected, buffers.size() - 1);
        buffer_list_view = EditorState::from_text(editor_buffer_list_text(buffers, buffer_list_selected));
        buffer_list_view.set_path("[buffers]");
        buffer_list_view.highlight_enabled = false;
        const size_t selected_line = std::min(buffer_list_selected + 1, buffer_list_view.text.line_count() - 1);
        buffer_list_view.cursor = buffer_list_view.text.line_start(selected_line);
        buffer_list_view.dirty = false;
        buffer_list_view.clear_undo_history();
    };

    auto render_editor = [&]() {
        const TerminalThemeStyle theme_style = terminal_theme_style();
        if (picker.active) {
            picker.refresh_view();
            render_terminal(picker.view, minibuffer, theme_style);
            return;
        }
        if (buffer_list_active) {
            refresh_buffer_list_view();
            render_terminal_panel(buffer_list_view,
                                  minibuffer,
                                  theme_style,
                                  // The editor buffer selector uses the same panel widget as chat /list.
                                  tui::TuiMode::ThreadList,
                                  buffer_list_scroll,
                                  "Buffers");
            return;
        }
        state.highlight_enabled = highlight_enabled;
        if (help_view.active || !split_layout.has_split()) {
            render_terminal(state, minibuffer, theme_style, help_view.active, refresh_assist_display());
            return;
        }
        const std::vector<SplitPaneRect> panes = split_layout.layout_panes(editor_main_area());
        render_terminal_splits(
            panes,
            [&](size_t buffer_index) -> const EditorState& {
                if (buffer_index == active_buffer) {
                    return state;
                }
                if (buffer_index < buffers.size()) {
                    return buffers[buffer_index];
                }
                return state;
            },
            state,
            minibuffer,
            theme_style,
            help_view.active,
            refresh_assist_display(),
            split_layout.leaf_count());
    };
    render_editor();

    auto assist_panel_rect = [&]() {
        if (split_layout.has_split()) {
            return split_layout.focused_rect(editor_main_area());
        }
        return editor_main_area();
    };

    auto exit_help_view = [&]() {
        if (!help_view.active) {
            return;
        }
        state.restore_captured_state(help_view.saved);
        state.path = help_view.saved_path;
        state.dirty = help_view.saved_dirty;
        state.set_language(help_view.saved_language, help_view.saved_language_automatic);
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
        if (reformat_session.active) {
            minibuffer_message(minibuffer, "Wait for reformatting or press Esc to cancel it");
            return;
        }
        if (insert_session.active) {
            minibuffer_message(minibuffer, "Wait for insertion or press Esc to cancel it");
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
        help_view.saved_language = state.language;
        help_view.saved_language_automatic = state.language_automatic;
        help_view.active = true;
        state.text = PieceTable::from_string(std::move(help_text));
        state.invalidate_word_index();
        state.cursor = 0;
        state.preferred_column = 0;
        state.scroll_line = 0;
        state.scroll_column = 0;
        state.dirty = false;
        state.set_language(highlight::Language::Markdown, false);
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
        state.highlight_enabled = highlight_enabled;
        split_layout.set_focused_buffer(active_buffer);
        buffer_list_active = false;
        buffer_list_selected = active_buffer;
        pending_close_confirm = false;
        pending_close_index = static_cast<size_t>(-1);
        minibuffer_message(minibuffer,
                           "Opened buffer " + std::to_string(active_buffer + 1) + "/" +
                               std::to_string(buffers.size()) + ": " +
                               editor_buffer_display_name(state, active_buffer));
    };

    auto find_open_buffer = [&](const std::string& path) -> std::optional<size_t> {
        if (path.empty()) {
            return std::nullopt;
        }
        std::string canonical;
        if (!canonicalize_editor_target(path, canonical).ok()) {
            canonical.clear();
        }
        sync_active_buffer();
        for (size_t i = 0; i < buffers.size(); ++i) {
            if ((!canonical.empty() && buffers[i].canonical_path == canonical) ||
                buffers[i].path == path) {
                return i;
            }
        }
        return std::nullopt;
    };

    auto open_buffer_from_path = [&](const std::string& open_path,
                                     const std::string& load_path,
                                     bool recovered_from_autosave) {
        if (const std::optional<size_t> existing = find_open_buffer(open_path)) {
            activate_buffer(*existing);
            return;
        }
        EditorState next;
        next.set_undo_limit(settings.undo_limit);
        next.set_path(open_path);
        const Error lock_error = next.begin_file_session(open_path, true);
        LoadedFile loaded;
        Error load_error = load_file(load_path, settings, loaded);
        if (!load_error.ok()) {
            minibuffer_message(minibuffer, load_error.message);
            return;
        }
        sync_active_buffer();
        const bool mixed_linebreaks = loaded.mixed_linebreaks;
        next.text = std::move(loaded.text);
        next.invalidate_word_index();
        next.tab_width = loaded.tab_width;
        next.tab_style = loaded.tab_style;
        next.linebreak = loaded.linebreak;
        next.highlight_enabled = highlight_enabled;
        next.cursor = 0;
        next.preferred_column = 0;
        next.scroll_line = 0;
        next.scroll_column = 0;
        next.dirty = recovered_from_autosave;
        next.clear_selection();
        next.clear_undo_history();
        buffers.push_back(next);
        active_buffer = buffers.size() - 1;
        state = next;
        split_layout.set_focused_buffer(active_buffer);
        buffer_list_selected = active_buffer;
        if (!lock_error.ok()) {
            minibuffer_message(minibuffer, lock_error.message);
        } else if (mixed_linebreaks) {
            minibuffer_message(minibuffer,
                               "Warning: mixed line endings in " + load_path +
                                   "; normalized and using " +
                                   linebreak_name(next.linebreak) + " for saves");
        } else if (recovered_from_autosave) {
            minibuffer_message(minibuffer, "Recovered auto-save for " + open_path);
        } else {
            minibuffer_message(minibuffer, "Opened " + open_path);
        }
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
        if (offer_autosave_recovery_before_load(open_path, settings, minibuffer, pending_autosave_recovery)) {
            return;
        }
        open_buffer_from_path(open_path, open_path, false);
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
        next.tab_width = settings.tab_width;
        next.tab_style = settings.tab_style;
        next.linebreak = settings.linebreak;
        next.text = PieceTable::from_string("");
        next.invalidate_word_index();
        next.path.clear();
        next.redetect_language();
        next.highlight_enabled = highlight_enabled;
        next.dirty = false;
        next.clear_selection();
        next.clear_undo_history();
        buffers.push_back(next);
        active_buffer = buffers.size() - 1;
        state = next;
        split_layout.set_focused_buffer(active_buffer);
        buffer_list_active = false;
        buffer_list_selected = active_buffer;
        pending_close_confirm = false;
        pending_close_index = static_cast<size_t>(-1);
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
        buffer_list_scroll = 0;
        buffer_list_active = true;
        pending_close_confirm = false;
        pending_close_index = static_cast<size_t>(-1);
        minibuffer_message(minibuffer, selected_buffer_status());
    };

    auto cancel_buffer_list = [&]() {
        buffer_list_active = false;
        buffer_list_selected = active_buffer;
        pending_close_confirm = false;
        pending_close_index = static_cast<size_t>(-1);
        minibuffer_message(minibuffer, "Buffer list cancelled");
    };

    auto perform_buffer_close = [&](size_t index, bool force) {
        if (assist_session.active) {
            minibuffer_message(minibuffer, "Finish or cancel AI assist before closing buffers");
            return;
        }
        if (help_view.active) {
            exit_help_view();
        }
        if (index >= buffers.size()) {
            return;
        }
        if (!force && buffers[index].dirty) {
            pending_close_confirm = true;
            pending_close_index = index;
            const std::string nm = editor_buffer_display_name(buffers[index], index);
            minibuffer_message(minibuffer, "Close " + nm + " (modified)? (y/n) ");
            return;
        }
        pending_close_confirm = false;
        pending_close_index = static_cast<size_t>(-1);
        const bool was_only = (buffers.size() <= 1);
        if (was_only) {
            buffers.clear();
            state = EditorState{};
            state.set_undo_limit(settings.undo_limit);
            state.tab_width = settings.tab_width;
            state.tab_style = settings.tab_style;
            state.linebreak = settings.linebreak;
            state.highlight_enabled = highlight_enabled;
            buffers.push_back(state);
            active_buffer = 0;
            split_layout.reset(0);
            if (buffer_list_active) {
                buffer_list_selected = 0;
            }
            minibuffer_message(minibuffer, "Closed buffer; opened scratch buffer");
            return;
        }
        const size_t fallback = index > 0 ? index - 1 : 0;
        split_layout.on_buffer_removed(index, fallback);
        buffers.erase(buffers.begin() + static_cast<std::ptrdiff_t>(index));
        if (active_buffer > index) {
            --active_buffer;
        } else if (active_buffer == index) {
            active_buffer = std::min(active_buffer, buffers.size() - 1);
            state = buffers[active_buffer];
        }
        split_layout.clamp_buffers(buffers.size());
        focus_buffer_from_split();
        if (buffer_list_active) {
            if (buffer_list_selected > index) {
                --buffer_list_selected;
            }
            buffer_list_selected = std::min(buffer_list_selected, buffers.size() - 1);
            minibuffer_message(minibuffer, selected_buffer_status());
        } else {
            minibuffer_message(minibuffer,
                               "Closed buffer; active " + std::to_string(active_buffer + 1) + "/" +
                                   std::to_string(buffers.size()) + ": " +
                                   editor_buffer_display_name(state, active_buffer));
        }
    };

    auto close_active_buffer = [&](bool force) {
        perform_buffer_close(active_buffer, force);
    };

    auto handle_buffer_list_escape = [&]() {
        const std::string sequence = read_escape_suffix();
        if (sequence.empty()) {
            cancel_buffer_list();
            return;
        }
        if (sequence == "[3~") {
            if (!buffers.empty() && buffer_list_selected < buffers.size()) {
                perform_buffer_close(buffer_list_selected, false);
            }
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

    auto refresh_ai_status = [&]() {
        minibuffer_message(minibuffer, editor_startup_status(ai_continue));
    };

    auto open_provider_picker = [&]() {
        if (assist_session.active) {
            minibuffer_message(minibuffer, "Finish or cancel AI assist before changing provider");
            return;
        }
        if (help_view.active) {
            exit_help_view();
        }
        picker.open_providers();
        buffer_list_active = false;
        pending_close_confirm = false;
        pending_close_index = static_cast<size_t>(-1);
        minibuffer_message(minibuffer, picker.status_message());
    };

    auto start_model_list = [&]() {
        if (!editor_ai_has_provider(ai_continue)) {
            minibuffer_message(minibuffer, editor_no_provider_message());
            return;
        }
        if (model_list.job.running()) {
            minibuffer_message(minibuffer, "Model list is already loading");
            return;
        }
        model_list.start(ai_continue->request);
        minibuffer_message(minibuffer, "Loading models...");
    };

    auto open_model_picker = [&](std::vector<std::string> models) {
        if (assist_session.active) {
            minibuffer_message(minibuffer, "Finish or cancel AI assist before changing model");
            return;
        }
        if (help_view.active) {
            exit_help_view();
        }
        picker.open_models(std::move(models));
        buffer_list_active = false;
        pending_close_confirm = false;
        pending_close_index = static_cast<size_t>(-1);
        minibuffer_message(minibuffer, picker.status_message());
    };

    auto handle_provider_command = [&](const std::string& target) {
        if (target.empty()) {
            open_provider_picker();
            return;
        }
        Error apply_error = apply_editor_provider_target(ai_continue, assist_config, target);
        if (!apply_error.ok()) {
            minibuffer_message(minibuffer, apply_error.message);
            return;
        }
        refresh_ai_status();
    };

    auto handle_model_command = [&](const std::string& model_name) {
        if (model_name.empty()) {
            if (!editor_ai_has_provider(ai_continue)) {
                minibuffer_message(minibuffer, editor_no_provider_message());
                return;
            }
            start_model_list();
            return;
        }
        Error apply_error = apply_editor_model(ai_continue, model_name);
        if (!apply_error.ok()) {
            minibuffer_message(minibuffer, apply_error.message);
            return;
        }
        refresh_ai_status();
    };

    auto handle_picker_list_escape = [&]() {
        const std::string sequence = read_escape_suffix();
        std::string picker_status;
        picker.handle_escape(sequence, picker_status);
        minibuffer_message(minibuffer, picker_status);
    };

    auto confirm_picker_selection = [&]() {
        if (picker.selected >= picker.items.size()) {
            return;
        }
        if (picker.for_provider) {
            const std::string provider_name = picker.items[picker.selected];
            const bool was_provider_picker = picker.for_provider;
            picker.clear();
            Error apply_error = apply_editor_provider_target(ai_continue, assist_config, provider_name);
            if (!apply_error.ok()) {
                minibuffer_message(minibuffer, apply_error.message);
                return;
            }
            refresh_ai_status();
            if (was_provider_picker && editor_ai_has_provider(ai_continue) &&
                ai_continue->request.options.model.empty()) {
                start_model_list();
            }
            return;
        }
        const std::string model_name = picker.items[picker.selected];
        picker.clear();
        Error apply_error = apply_editor_model(ai_continue, model_name);
        if (!apply_error.ok()) {
            minibuffer_message(minibuffer, apply_error.message);
            return;
        }
        refresh_ai_status();
    };

    auto require_ai_provider = [&]() -> bool {
        if (!editor_ai_has_provider(ai_continue)) {
            minibuffer_message(minibuffer, editor_no_provider_message());
            return false;
        }
        return true;
    };

    auto process_model_events = [&]() -> bool {
        return model_list.process(
            [&](std::vector<std::string> models) { open_model_picker(std::move(models)); },
            [&](const std::string& message) { minibuffer_message(minibuffer, message); });
    };

    auto handle_open_minibuffer_key = [&](unsigned char ch) -> bool {
        if (!minibuffer.active ||
            (minibuffer.action != MinibufferAction::LoadFile &&
             minibuffer.action != MinibufferAction::ConfirmLoad &&
             minibuffer.action != MinibufferAction::ConfirmAutosaveRecovery)) {
            return false;
        }
        if (ch == 27) {
            pending_load_path.clear();
            pending_autosave_recovery = PendingAutosaveRecovery{};
            minibuffer_path_completer.reset();
            minibuffer_message(minibuffer, "Open cancelled");
            return true;
        }
        if (ch == 19 || ch == editor_key_save_as()) {
            return false;
        }
        if (minibuffer.action == MinibufferAction::ConfirmAutosaveRecovery) {
            const PendingAutosaveRecovery recovery = pending_autosave_recovery;
            pending_autosave_recovery = PendingAutosaveRecovery{};
            if (ch == 'y' || ch == 'Y') {
                open_buffer_from_path(recovery.path, recovery.autosave_path, true);
            } else if (ch == 'n' || ch == 'N') {
                open_buffer_from_path(recovery.path, recovery.path, false);
            } else {
                pending_autosave_recovery = recovery;
                minibuffer.prompt = ui::kConfirmationRetryPrompt;
                minibuffer.input.clear();
            }
            return true;
        }
        if (minibuffer.action == MinibufferAction::ConfirmLoad) {
            if (ch == 'y' || ch == 'Y') {
                const std::string path_to_open = pending_load_path;
                pending_load_path.clear();
                if (offer_autosave_recovery_before_load(path_to_open,
                                                        settings,
                                                        minibuffer,
                                                        pending_autosave_recovery)) {
                    return true;
                }
                open_buffer_from_path(path_to_open, path_to_open, false);
            } else if (ch == 'n' || ch == 'N') {
                pending_load_path.clear();
                minibuffer_message(minibuffer, "Open cancelled");
            } else {
                minibuffer.prompt = ui::kConfirmationRetryPrompt;
                minibuffer.input.clear();
            }
            return true;
        }
        if (ch == '\r' || ch == '\n') {
            const std::string open_path = expand_user_path(trim_ascii_copy(minibuffer.input));
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
                if (minibuffer.action == MinibufferAction::LoadFile) {
                    minibuffer_path_completer.reset();
                }
            }
            return true;
        }
        if (ch == '\t' && minibuffer.action == MinibufferAction::LoadFile) {
            const PathCompletionResult result =
                complete_path_input(minibuffer.input, minibuffer_path_completer);
            minibuffer.message = path_completion_status(result);
            return true;
        }
        if (ch >= 0x20U) {
            minibuffer.input.push_back(static_cast<char>(ch));
            if (minibuffer.action == MinibufferAction::LoadFile) {
                EditorState temp = EditorState::from_text(minibuffer.input);
                temp.cursor = minibuffer.input.size();
                if (!minibuffer_path_completer.can_cycle(temp)) {
                    minibuffer_path_completer.reset();
                }
            }
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

    auto mark_last_assist_output = [&]() {
        if (!last_assist_command.valid) {
            return;
        }
        last_assist_command.has_revertable_output = true;
        last_assist_command.revert_snapshot = assist_session.undo_before;
    };

    auto revert_last_assist_output = [&]() {
        if (!last_assist_command.has_revertable_output) {
            return;
        }
        state.revert_to_snapshot(last_assist_command.revert_snapshot);
    };

    auto restore_after_new_buffer_assist_cancel = [&]() {
        if (!assist_session.new_buffer_assist) {
            return;
        }
        const size_t removed = active_buffer;
        size_t source = assist_session.source_buffer_index;
        if (assist_session.opened_split_for_assist && split_layout.has_split()) {
            // Drop the assist pane first so focus returns to the prior pane.
            split_layout.close_focused();
        }
        if (removed < buffers.size()) {
            size_t fallback = source < buffers.size() ? source : 0;
            if (fallback == removed) {
                fallback = removed > 0 ? removed - 1 : 0;
            }
            split_layout.on_buffer_removed(removed, fallback);
            buffers.erase(buffers.begin() + static_cast<std::ptrdiff_t>(removed));
            if (source > removed) {
                --source;
            } else if (source == removed && !buffers.empty()) {
                source = std::min(source, buffers.size() - 1);
            }
        }
        if (buffers.empty()) {
            EditorState scratch;
            scratch.set_undo_limit(settings.undo_limit);
            scratch.highlight_enabled = highlight_enabled;
            buffers.push_back(scratch);
            active_buffer = 0;
            split_layout.reset(0);
            state = buffers[0];
        } else {
            active_buffer = std::min(source, buffers.size() - 1);
            state = buffers[active_buffer];
            state.highlight_enabled = highlight_enabled;
            split_layout.clamp_buffers(buffers.size());
            split_layout.set_focused_buffer(active_buffer);
        }
        assist_session.new_buffer_assist = false;
        assist_session.opened_split_for_assist = false;
    };

    auto finish_assist_session = [&](const std::string& message,
                                     bool commit_stream_undo,
                                     const std::optional<std::string>& inplace_content,
                                     bool message_includes_label = false) {
        assist_session.new_buffer_assist = false;
        assist_session.opened_split_for_assist = false;
        assist_session.job.join();
        const auto show_finish_message = [&]() {
            if (message_includes_label) {
                minibuffer_message(minibuffer, message);
            } else {
                set_assist_minibuffer(message);
            }
        };
        if (inplace_content.has_value()) {
            Error replace_error =
                state.replace(assist_session.replace_start, assist_session.replace_count, *inplace_content);
            if (!replace_error.ok()) {
                minibuffer_message(minibuffer, replace_error.message);
            } else {
                state.clear_selection();
                mark_last_assist_output();
                show_finish_message();
            }
        } else {
            if (!assist_session.code_completion && !assist_session.prose_completion &&
                (assist_session.edit_kind == AssistEditKind::StreamInsert ||
                 assist_session.edit_kind == AssistEditKind::NewBuffer)) {
                strip_trailing_assist_close_tag_without_undo(state);
            }
            if (commit_stream_undo && assist_session.saw_visible) {
                state.finalize_stream_edit(assist_session.undo_before);
                mark_last_assist_output();
            }
            show_finish_message();
        }
        clear_assist_session(assist_session);
    };

    AssistEventHandlers assist_handlers;
    assist_handlers.on_thinking = [&]() {
        set_assist_activity(tui::ActivityKind::Thinking, "thinking... ESC to abort");
    };
    assist_handlers.on_writing = [&]() {
        set_assist_activity(tui::ActivityKind::Streaming, "writing. Press ESC to stop.");
    };
    assist_handlers.on_delta = [&](const std::string& text) -> Error {
        assist_session.saw_visible = true;
        set_assist_activity(tui::ActivityKind::Streaming, "writing. Press ESC to stop.");
        const Error insert_error = state.insert_without_undo(text);
        if (insert_error.ok()) {
            state.ensure_cursor_visible(assist_panel_rect());
        }
        return insert_error;
    };
    assist_handlers.on_done = [&](const ContinueEvent& event, AssistSession& session_state) {
        if (!event.chat.model.empty()) {
            session_state.model_name = event.chat.model;
        }
        long long context_tokens = 0;
        if (ai_continue.has_value()) {
            provider::resolve_context_window(ai_continue->request, session_state.model_name);
            context_tokens = ai_continue->request.options.context_tokens;
        }
        const std::vector<provider::Message>& usage_messages =
            session_state.usage_messages.empty() ? session_state.messages : session_state.usage_messages;
        const std::string completion_status = continue_completion_status_message(
            session_state.provider_name,
            session_state.model_name,
            event.chat,
            session_state.streaming,
            usage_messages,
            context_tokens);
        if (session_state.edit_kind == AssistEditKind::ReplaceInPlace) {
            finish_assist_session(completion_status,
                                  false,
                                  trim_assist_inplace_response(event.chat.content),
                                  true);
        } else {
            finish_assist_session(completion_status, true, std::nullopt, true);
        }
    };
    assist_handlers.on_error = [&](const Error& error, bool cancelled, AssistSession& session_state) {
        (void)session_state;
        if (cancelled) {
            if (regenerate_after_cancel) {
                if (assist_session.saw_visible) {
                    state.revert_to_snapshot(assist_session.undo_before);
                }
                restore_after_new_buffer_assist_cancel();
                return;
            }
            if (assist_session.edit_kind == AssistEditKind::ReplaceInPlace) {
                set_assist_minibuffer("stopped and ready");
                return;
            }
            restore_after_new_buffer_assist_cancel();
            finish_assist_session("stopped and ready", true, std::nullopt);
            return;
        }
        restore_after_new_buffer_assist_cancel();
        minibuffer_message(minibuffer, error.message);
    };

    auto process_assist_events = [&]() -> bool {
        return editor::process_assist_events(assist_session,
                                             assist_handlers,
                                             regenerate_after_cancel,
                                             pending_regenerate_restart);
    };

    auto start_assist = [&](AssistCommandKind kind,
                            size_t command_index,
                            std::optional<AssistScope> scope,
                            const std::string& custom_prompt,
                            std::optional<AssistPromptMode> prompt_mode,
                            const EditorState* execution_state = nullptr) {
        if (assist_session.active) {
            return;
        }
        if (reformat_session.active) {
            minibuffer_message(minibuffer, "Wait for reformatting or press Esc to cancel it");
            return;
        }
        if (minibuffer.active && !is_assist_minibuffer_action(minibuffer.action)) {
            return;
        }
        if (!require_ai_provider()) {
            return;
        }
        Error validation = validate_continue_request(*ai_continue);
        if (!validation.ok()) {
            minibuffer_message(minibuffer, validation.message);
            return;
        }

        const EditorState& source_state = execution_state != nullptr ? *execution_state : state;
        AssistExecution execution = build_assist_execution(
            source_state, *ai_continue, kind, command_index, scope, custom_prompt, prompt_mode);
        if (!execution.ok) {
            minibuffer_message(minibuffer, execution.error_message);
            return;
        }
        if (execution.edit_kind != AssistEditKind::NewBuffer && execution_state == nullptr) {
            Error writable = state.mutation_allowed();
            if (!writable.ok()) {
                minibuffer_message(minibuffer, writable.message);
                return;
            }
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
        if (execution.edit_kind == AssistEditKind::NewBuffer) {
            const size_t source_buffer_index = active_buffer;
            sync_active_buffer();
            // Keep the focused split leaf on the source buffer while we create the target.
            split_layout.set_focused_buffer(source_buffer_index);
            EditorState next;
            next.set_undo_limit(settings.undo_limit);
            next.text = PieceTable::from_string("");
            next.invalidate_word_index();
            next.path.clear();
            next.redetect_language();
            next.highlight_enabled = highlight_enabled;
            next.dirty = false;
            next.clear_selection();
            next.clear_undo_history();
            buffers.push_back(next);
            const size_t new_buffer_index = buffers.size() - 1;
            active_buffer = new_buffer_index;
            state = buffers[active_buffer];
            assist_session.new_buffer_assist = true;
            assist_session.opened_split_for_assist = false;
            assist_session.source_buffer_index = source_buffer_index;
            if (execution.new_buffer_layout == AssistNewBufferLayout::VSplit ||
                execution.new_buffer_layout == AssistNewBufferLayout::HSplit) {
                const SplitKind kind = execution.new_buffer_layout == AssistNewBufferLayout::VSplit
                                           ? SplitKind::Vertical
                                           : SplitKind::Horizontal;
                if (split_layout.split_and_open_buffer(kind, editor_main_area(), new_buffer_index)) {
                    assist_session.opened_split_for_assist = true;
                } else {
                    // Fall back to a plain new buffer when the window is too small.
                    split_layout.set_focused_buffer(new_buffer_index);
                    minibuffer_message(
                        minibuffer,
                        "Window too small to split; opened a new buffer instead");
                }
            } else {
                split_layout.set_focused_buffer(new_buffer_index);
            }
            buffer_list_active = false;
            buffer_list_selected = active_buffer;
            pending_close_confirm = false;
            pending_close_index = static_cast<size_t>(-1);
        }
        assist_session.active = true;
        assist_session.streaming = execution.stream;
        assist_session.code_completion = execution.code_completion;
        assist_session.prose_completion = execution.prose_completion;
        assist_session.edit_kind = execution.edit_kind;
        assist_session.provider_name = ai_continue->request.profile.name;
        assist_session.model_name = ai_continue->request.options.model;
        assist_session.messages = execution.messages;
        assist_session.usage_messages = execution.usage_messages;
        assist_session.replace_start = execution.replace_start;
        assist_session.replace_count = execution.replace_count;
        assist_session.undo_before = state.capture_state();
        state.clear_selection();
        start_assist_job(*ai_continue,
                         execution.messages,
                         execution.stream,
                         execution.code_completion,
                         execution.prose_completion,
                         execution.completion_language,
                         assist_session.events,
                         assist_session.job);
        set_assist_activity(tui::ActivityKind::Thinking, "thinking... ESC to abort");
    };

    auto regenerate_last_assist = [&]() {
        if (!last_assist_command.valid) {
            minibuffer_message(minibuffer, "No previous AI command to regenerate");
            return;
        }
        if (assist_session.active) {
            regenerate_after_cancel = true;
            assist_session.job.cancel();
            minibuffer_message(minibuffer, "Cancelling before regenerate...");
            return;
        }
        revert_last_assist_output();
        start_assist(last_assist_command.kind,
                     last_assist_command.command_index,
                     last_assist_command.scope,
                     last_assist_command.custom_prompt,
                     last_assist_command.prompt_mode);
        minibuffer_message(minibuffer, "Regenerating...");
    };

    auto trigger_save = [&]() {
        if (state.path.empty()) {
            start_minibuffer(minibuffer,
                             MinibufferAction::SaveFile,
                             "Save file: ",
                             "",
                             &minibuffer_path_completer);
        } else {
            request_save_editor_to_path(state, state.path, minibuffer, true, false, quit, pending_save, settings);
        }
    };

    auto start_reformat = [&](bool all) {
        if (reformat_session.active) {
            minibuffer_message(minibuffer, "A reformat job is already running; press Esc to cancel it");
            return;
        }
        if (assist_session.active) {
            minibuffer_message(minibuffer, "Finish or cancel AI assist before reformatting");
            return;
        }
        Error writable = state.mutation_allowed();
        if (!writable.ok()) {
            minibuffer_message(minibuffer, writable.message);
            return;
        }
        ReformatRequest request;
        Error request_error = build_reformat_request(state, all, request);
        if (!request_error.ok()) {
            minibuffer_message(minibuffer, request_error.message);
            return;
        }
        if (state.language == highlight::Language::Text) {
            minibuffer_message(minibuffer,
                               "Cannot reformat text mode. Choose a programming language with /mode first");
            return;
        }
        reformat_session.active = true;
        reformat_session.all = all;
        reformat_session.cancel_requested = false;
        reformat_session.buffer_id = state.buffer_id();
        reformat_session.revision = state.revision();
        reformat_session.language = state.language;
        reformat_session.tab_width = state.tab_width;
        reformat_session.tab_style = state.tab_style;
        start_reformat_job(std::move(request), reformat_session.events, reformat_session.job);
        minibuffer_message(minibuffer,
                           std::string(all ? "Reformatting entire " : "Reformatting selected ") +
                               highlight::language_name(state.language) +
                               " buffer... Esc to cancel");
    };

    auto process_reformat_events = [&]() -> bool {
        ReformatEvent event;
        if (!reformat_session.events.try_pop(event)) {
            return false;
        }
        reformat_session.job.join();
        const bool was_all = reformat_session.all;
        const bool was_cancelled = reformat_session.cancel_requested;
        EditorState* target = nullptr;
        if (state.buffer_id() == reformat_session.buffer_id) {
            target = &state;
        } else {
            for (size_t index = 0; index < buffers.size(); ++index) {
                if (index != active_buffer &&
                    buffers[index].buffer_id() == reformat_session.buffer_id) {
                    target = &buffers[index];
                    break;
                }
            }
        }
        reformat_session.active = false;
        if (was_cancelled) {
            minibuffer_message(minibuffer, "Reformat cancelled");
            return true;
        }
        if (!event.result.error.ok()) {
            minibuffer_message(minibuffer, event.result.error.message);
            return true;
        }
        if (target == nullptr) {
            minibuffer_message(minibuffer,
                               "Reformat result discarded because its buffer was closed");
            return true;
        }
        if (target->revision() != reformat_session.revision ||
            target->language != reformat_session.language ||
            target->tab_width != reformat_session.tab_width ||
            target->tab_style != reformat_session.tab_style) {
            minibuffer_message(minibuffer,
                               "Reformat result discarded because the buffer or its indentation settings changed");
            return true;
        }
        Error apply_error = apply_reformat_result(*target, event.result, was_all);
        if (!apply_error.ok()) {
            minibuffer_message(minibuffer, apply_error.message);
            return true;
        }
        if (target == &state) {
            sync_active_buffer();
        }
        const size_t count = event.result.last_line - event.result.first_line + 1;
        std::string message = event.result.changed
                                  ? "Reformatted " + std::to_string(count) + " line(s)"
                                  : "Indentation already matches the active language mode";
        if (!event.result.warning.empty()) {
            message += ". Warning: " + event.result.warning;
        }
        minibuffer_message(minibuffer, std::move(message));
        return true;
    };

    auto start_insert_source = [&](const std::string& source) {
        if (insert_session.active) {
            minibuffer_message(minibuffer, "An insertion is already running");
            return;
        }
        if (source.empty()) {
            minibuffer_message(minibuffer, "Usage: /insert FILE_OR_URL");
            return;
        }
        if (source == "stdin") {
            minibuffer_message(minibuffer,
                               "stdin input is only supported by non-interactive --input and --attach");
            return;
        }
        Error writable = state.mutation_allowed();
        if (!writable.ok()) {
            minibuffer_message(minibuffer, writable.message);
            return;
        }
        insert_session.active = true;
        insert_session.cancel_requested = false;
        insert_session.buffer_id = state.buffer_id();
        insert_session.revision = state.revision();
        insert_session.position = state.cursor;
        insert_session.source = source;
        const input::InsertSourceOptions options = insert_options;
        runtime::EventQueue<InsertEvent>& event_queue = insert_session.events;
        insert_session.job.start([source, options, &event_queue](runtime::CancellationToken token) mutable {
            InsertEvent event;
            event.error = input::load_insert_source(source, options, event.inserted, token);
            event_queue.push(std::move(event));
        });
        minibuffer_message(minibuffer,
                           std::string(input::is_http_url(source) ? "Fetching " : "Reading ") + source +
                               " for insertion... Esc to cancel");
    };

    auto process_insert_events = [&]() -> bool {
        InsertEvent event;
        if (!insert_session.events.try_pop(event)) {
            return false;
        }
        insert_session.job.join();
        const bool was_cancelled = insert_session.cancel_requested;
        EditorState* target = nullptr;
        if (state.buffer_id() == insert_session.buffer_id) {
            target = &state;
        } else {
            for (size_t index = 0; index < buffers.size(); ++index) {
                if (index != active_buffer &&
                    buffers[index].buffer_id() == insert_session.buffer_id) {
                    target = &buffers[index];
                    break;
                }
            }
        }
        insert_session.active = false;
        if (was_cancelled || event.error.code == ErrorCode::Cancelled) {
            minibuffer_message(minibuffer, "Insertion cancelled");
            return true;
        }
        if (!event.error.ok()) {
            minibuffer_message(minibuffer, event.error.message);
            return true;
        }
        if (target == nullptr) {
            minibuffer_message(minibuffer,
                               "Insertion discarded because its target buffer was closed");
            return true;
        }
        if (target->revision() != insert_session.revision) {
            minibuffer_message(minibuffer,
                               "Insertion discarded because its target buffer changed while loading");
            return true;
        }
        target->cursor = std::min(insert_session.position, target->text.size());
        target->clear_selection();
        Error insert_error = target->insert(event.inserted.content);
        if (!insert_error.ok()) {
            minibuffer_message(minibuffer, insert_error.message);
            return true;
        }
        if (target == &state) {
            sync_active_buffer();
        }
        minibuffer_message(minibuffer,
                           "Inserted " + insert_session.source + " at cursor" +
                               (event.inserted.converted_html ? " as Markdown" : ""));
        return true;
    };

    auto apply_vsplit = [&]() {
        const Rect area = editor_main_area();
        if (split_layout.split_focused(SplitKind::Vertical, area)) {
            minibuffer_message(minibuffer, "Vertical split (Ctrl+G o other pane)");
        } else {
            minibuffer_message(minibuffer, "Window too small for vertical split");
        }
    };

    auto apply_hsplit = [&]() {
        const Rect area = editor_main_area();
        if (split_layout.split_focused(SplitKind::Horizontal, area)) {
            minibuffer_message(minibuffer, "Horizontal split (Ctrl+G o other pane)");
        } else {
            minibuffer_message(minibuffer, "Window too small for horizontal split");
        }
    };

    auto apply_closesplit = [&]() {
        if (!split_layout.close_focused()) {
            minibuffer_message(minibuffer, "Only one pane");
        } else {
            focus_buffer_from_split();
            minibuffer_message(minibuffer, "Closed pane");
        }
    };

    auto apply_maximize_split = [&]() {
        split_layout.maximize_focused();
        focus_buffer_from_split();
        minibuffer_message(minibuffer, "Maximized pane");
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
                    start_minibuffer(minibuffer,
                                     MinibufferAction::SaveAsFile,
                                     "Save as: ",
                                     state.path,
                                     &minibuffer_path_completer);
                } else {
                    request_save_editor_to_path(state, slash.path, minibuffer, true, false, quit, pending_save,
                                                settings);
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
                    start_minibuffer(minibuffer,
                                     MinibufferAction::LoadFile,
                                     "Open file: ",
                                     "",
                                     &minibuffer_path_completer);
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
            case EditorSlashCommand::Chat:
                pending_assist = PendingAssist{};
                exit_assist_command_mode(minibuffer, assist_completer);
                request_switch_to_chat();
                return;
            case EditorSlashCommand::VSplit:
                pending_assist = PendingAssist{};
                exit_assist_command_mode(minibuffer, assist_completer);
                apply_vsplit();
                return;
            case EditorSlashCommand::HSplit:
                pending_assist = PendingAssist{};
                exit_assist_command_mode(minibuffer, assist_completer);
                apply_hsplit();
                return;
            case EditorSlashCommand::CloseSplit:
                pending_assist = PendingAssist{};
                exit_assist_command_mode(minibuffer, assist_completer);
                apply_closesplit();
                return;
            case EditorSlashCommand::Maximize:
                pending_assist = PendingAssist{};
                exit_assist_command_mode(minibuffer, assist_completer);
                apply_maximize_split();
                return;
            case EditorSlashCommand::None:
                break;
        }
        const std::string command_line = canonical_editor_command_line(minibuffer.input);
        if (command_line == "/insert" || command_line.rfind("/insert ", 0) == 0) {
            pending_assist = PendingAssist{};
            exit_assist_command_mode(minibuffer, assist_completer);
            start_insert_source(command_line.size() <= 7
                                    ? ""
                                    : trim_ascii_copy(command_line.substr(7)));
            return;
        }
        if (command_line == "/auto-convert-html-to-md" ||
            command_line.rfind("/auto-convert-html-to-md ", 0) == 0) {
            pending_assist = PendingAssist{};
            exit_assist_command_mode(minibuffer, assist_completer);
            const std::string requested = command_line.size() <= 24
                                              ? ""
                                              : ascii_lower(trim_ascii_copy(command_line.substr(24)));
            if (requested.empty()) {
                minibuffer_message(minibuffer,
                                   std::string("Auto-convert HTML to Markdown: ") +
                                       (insert_options.auto_convert_html_to_markdown ? "yes" : "no"));
                return;
            }
            if (requested != "yes" && requested != "no" && requested != "on" &&
                requested != "off" && requested != "true" && requested != "false") {
                minibuffer_message(minibuffer,
                                   "Usage: /auto-convert-html-to-md yes|no");
                return;
            }
            insert_options.auto_convert_html_to_markdown =
                requested == "yes" || requested == "on" || requested == "true";
            if (interactive != nullptr) {
                interactive->context.options.auto_convert_html_to_markdown =
                    insert_options.auto_convert_html_to_markdown;
            }
            if (ai_continue.has_value()) {
                ai_continue->request.options.auto_convert_html_to_markdown =
                    insert_options.auto_convert_html_to_markdown;
            }
            minibuffer_message(minibuffer,
                               std::string("Auto-convert HTML to Markdown: ") +
                                   (insert_options.auto_convert_html_to_markdown ? "yes" : "no"));
            return;
        }
        if (command_line == "/reformat" || command_line == "/reformat-all") {
            pending_assist = PendingAssist{};
            exit_assist_command_mode(minibuffer, assist_completer);
            start_reformat(command_line == "/reformat-all");
            return;
        }
        if (command_line.rfind("/reformat ", 0) == 0 ||
            command_line.rfind("/reformat-all ", 0) == 0) {
            pending_assist = PendingAssist{};
            exit_assist_command_mode(minibuffer, assist_completer);
            minibuffer_message(minibuffer, "Usage: /reformat or /reformat-all");
            return;
        }
        if (command_line == "/chat") {
            pending_assist = PendingAssist{};
            exit_assist_command_mode(minibuffer, assist_completer);
            request_switch_to_chat();
            return;
        }
        if (command_line == "/highlight" || command_line.rfind("/highlight ", 0) == 0) {
            pending_assist = PendingAssist{};
            exit_assist_command_mode(minibuffer, assist_completer);
            const std::string requested = command_line.size() <= 10
                                              ? ""
                                              : ascii_lower(trim_ascii_copy(command_line.substr(10)));
            if (requested.empty()) {
                minibuffer_message(minibuffer,
                                   std::string("Syntax highlighting: ") +
                                       (highlight_enabled ? "on" : "off"));
                return;
            }
            if (requested != "on" && requested != "off") {
                minibuffer_message(minibuffer, "Usage: /highlight on|off");
                return;
            }
            highlight_enabled = requested == "on";
            state.highlight_enabled = highlight_enabled;
            if (interactive != nullptr) {
                interactive->highlight_enabled = highlight_enabled;
            }
            minibuffer_message(minibuffer,
                               std::string("Syntax highlighting ") +
                                   (highlight_enabled ? "enabled" : "disabled"));
            return;
        }
        if (command_line == "/tab-width" || command_line.rfind("/tab-width ", 0) == 0) {
            pending_assist = PendingAssist{};
            exit_assist_command_mode(minibuffer, assist_completer);
            const std::string requested =
                command_line.size() <= 10 ? "" : trim_ascii_copy(command_line.substr(10));
            if (requested.empty()) {
                minibuffer_message(minibuffer, "Tab width: " + std::to_string(state.tab_width));
                return;
            }
            size_t width = 0;
            const char* begin = requested.data();
            const char* end = begin + requested.size();
            const std::from_chars_result parsed = std::from_chars(begin, end, width);
            if (parsed.ec != std::errc{} || parsed.ptr != end || width < 1 ||
                width > kMaxTabWidth) {
                minibuffer_message(minibuffer, "Usage: /tab-width WIDTH (1 through 32)");
                return;
            }
            state.tab_width = width;
            update_preferred_column(state);
            minibuffer_message(minibuffer, "Tab width: " + std::to_string(state.tab_width));
            return;
        }
        if (command_line == "/tab-style" || command_line.rfind("/tab-style ", 0) == 0) {
            pending_assist = PendingAssist{};
            exit_assist_command_mode(minibuffer, assist_completer);
            const std::string requested = command_line.size() <= 10
                                              ? ""
                                              : ascii_lower(trim_ascii_copy(command_line.substr(10)));
            if (requested.empty()) {
                minibuffer_message(minibuffer,
                                   std::string("Tab style: ") + tab_style_name(state.tab_style));
                return;
            }
            TabStyle style;
            if (!parse_tab_style(requested, style)) {
                minibuffer_message(minibuffer, "Usage: /tab-style spaces|tab");
                return;
            }
            state.tab_style = style;
            minibuffer_message(minibuffer,
                               std::string("Tab style: ") + tab_style_name(state.tab_style));
            return;
        }
        if (command_line == "/linebreak" || command_line.rfind("/linebreak ", 0) == 0) {
            pending_assist = PendingAssist{};
            exit_assist_command_mode(minibuffer, assist_completer);
            const std::string requested = command_line.size() <= 10
                                              ? ""
                                              : ascii_lower(trim_ascii_copy(command_line.substr(10)));
            if (requested.empty()) {
                minibuffer_message(minibuffer,
                                   std::string("Line break: ") + linebreak_name(state.linebreak));
                return;
            }
            LineBreak linebreak;
            if (!parse_linebreak(requested, linebreak)) {
                minibuffer_message(minibuffer, "Usage: /linebreak lf|cr|crlf");
                return;
            }
            if (state.linebreak != linebreak) {
                state.linebreak = linebreak;
                state.dirty = true;
                state.record_autosave_change(1);
            }
            minibuffer_message(minibuffer,
                               std::string("Line break: ") + linebreak_name(state.linebreak));
            return;
        }
        if (command_line == "/mode" || command_line.rfind("/mode ", 0) == 0) {
            pending_assist = PendingAssist{};
            exit_assist_command_mode(minibuffer, assist_completer);
            const std::string requested = command_line.size() <= 5
                                              ? ""
                                              : ascii_lower(trim_ascii_copy(command_line.substr(5)));
            if (requested.empty()) {
                minibuffer_message(minibuffer,
                                   std::string("Mode: ") + highlight::language_name(state.language) +
                                       (state.language_automatic ? " (automatic)" : " (manual)"));
                return;
            }
            if (requested == "auto") {
                state.language_automatic = true;
                state.redetect_language();
                minibuffer_message(minibuffer,
                                   std::string("Mode: ") + highlight::language_name(state.language) +
                                       " (automatic)");
                return;
            }
            highlight::Language language = highlight::Language::Text;
            if (!highlight::parse_language(requested, language)) {
                minibuffer_message(minibuffer,
                                   "Unknown mode: " + requested +
                                       ". Use auto, text, markdown, python, c, cpp, csharp, java, "
                                       "javascript, typescript, html, htmlonly, css, xml, json, bash, "
                                       "php, perl, ruby, rust, go, powershell, assembly, sql, toml, "
                                       "yaml, or ini");
                return;
            }
            state.set_language(language, false);
            minibuffer_message(minibuffer,
                               std::string("Mode: ") + highlight::language_name(state.language) +
                                   " (manual)");
            return;
        }
        if (command_line == "/theme" || command_line.rfind("/theme ", 0) == 0) {
            pending_assist = PendingAssist{};
            exit_assist_command_mode(minibuffer, assist_completer);
            if (settings.themes == nullptr) {
                minibuffer_message(minibuffer, "Themes are unavailable");
                return;
            }
            const std::string requested =
                command_line.size() <= 6 ? "" : trim_ascii_copy(command_line.substr(6));
            const tui::ThemeCommandResult theme_result =
                tui::handle_theme_command(*settings.themes, theme_name, requested, settings.use_colors);
            minibuffer_message(minibuffer, theme_result.message);
            if (theme_result.ok && !theme_result.selected_theme.empty()) {
                theme_name = theme_result.selected_theme;
            }
            return;
        }
        if (command_line == "/provider" || command_line.rfind("/provider ", 0) == 0) {
            pending_assist = PendingAssist{};
            exit_assist_command_mode(minibuffer, assist_completer);
            handle_provider_command(command_line.size() <= 9 ? "" : trim_ascii_copy(command_line.substr(9)));
            return;
        }
        if (command_line == "/model" || command_line.rfind("/model ", 0) == 0) {
            pending_assist = PendingAssist{};
            exit_assist_command_mode(minibuffer, assist_completer);
            handle_model_command(command_line.size() <= 6 ? "" : trim_ascii_copy(command_line.substr(6)));
            return;
        }
        const ParsedAssistCommand parsed = parse_assist_command(
            minibuffer.input, ai_continue.has_value() ? ai_continue->assist_config : assist_config);
        if (!parsed.ok) {
            minibuffer_message(minibuffer, parsed.error_message);
            return;
        }
        if (parsed.kind == AssistCommandKind::Configured) {
            const EditorAssistConfig& active_assist_config =
                ai_continue.has_value() ? ai_continue->assist_config : assist_config;
            if (parsed.command_index >= active_assist_config.commands.size()) {
                minibuffer_message(minibuffer, "Configured assist command index is out of range");
                return;
            }
            const EditorAssistCommand& command = active_assist_config.commands[parsed.command_index];
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
            pending_assist = PendingAssist{};
            exit_assist_command_mode(minibuffer, assist_completer);
            regenerate_last_assist();
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
                ai_continue.has_value() ? search::options_for(ai_continue->request.options)
                                        : search::default_options();
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
        if (!require_ai_provider()) {
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
        if (ch != '\t') {
            word_completer.reset();
        }
        if (minibuffer.active && ch == 22) {
            if (shared_clipboard().empty()) {
                minibuffer.message = "Clipboard is empty";
                return;
            }
            const Error paste_error = paste_into_minibuffer(minibuffer, shared_clipboard().text());
            if (paste_error.ok()) {
                assist_completer = AssistCompleterState{};
                minibuffer_path_completer.reset();
                minibuffer.message = "Pasted into minibuffer";
            } else {
                minibuffer.message = paste_error.message;
            }
            return;
        }
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
                                      pending_save,
                                      pending_autosave_recovery,
                                      minibuffer_path_completer)) {
                return;
            }
            if (minibuffer.active && is_assist_minibuffer_action(minibuffer.action)) {
                if (ch == 27) {
                    pending_assist = PendingAssist{};
                    exit_assist_command_mode(minibuffer, assist_completer);
                    return;
                }
                if (minibuffer.action == MinibufferAction::AssistCommand) {
                    if (ch == '\t') {
                        const AssistCompletionResult result = complete_assist_command(
                            minibuffer.input,
                            assist_completer,
                            ai_continue.has_value() ? ai_continue->assist_config : assist_config);
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
                    const Rect focus_rect = assist_panel_rect();
                    dispatch_escape_sequence(state, sequence, escape_status, last_search, &focus_rect);
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
        if (reformat_session.active && ch == 27) {
            reformat_session.cancel_requested = true;
            reformat_session.job.cancel();
            minibuffer_message(minibuffer, "Cancelling reformat...");
            return;
        }
        if (insert_session.active && ch == 27) {
            insert_session.cancel_requested = true;
            insert_session.job.cancel();
            minibuffer_message(minibuffer, "Cancelling insertion...");
            return;
        }
        if (picker.active) {
            if (ch == 17) {
                quit = true;
                return;
            }
            if (ch == 27) {
                handle_picker_list_escape();
                return;
            }
            if (ch == '\r' || ch == '\n') {
                confirm_picker_selection();
                return;
            }
            return;
        }
        if (buffer_list_active) {
            if (pending_close_confirm) {
                switch (ui::parse_confirmation_key(ch)) {
                    case ui::ConfirmationKeyResult::Accepted: {
                        size_t idx = pending_close_index != static_cast<size_t>(-1)
                                         ? pending_close_index
                                         : buffer_list_selected;
                        perform_buffer_close(idx, true);
                        return;
                    }
                    case ui::ConfirmationKeyResult::Rejected:
                        pending_close_confirm = false;
                        pending_close_index = static_cast<size_t>(-1);
                        minibuffer_message(minibuffer, "Close cancelled");
                        return;
                    case ui::ConfirmationKeyResult::Pending:
                        minibuffer_message(minibuffer, ui::kConfirmationRetryPrompt);
                        return;
                }
            }
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
            if (ch == 'n' || ch == 'N') {
                new_empty_buffer();
                return;
            }
            if (ch == 127 || ch == 8) {
                if (!buffers.empty() && buffer_list_selected < buffers.size()) {
                    perform_buffer_close(buffer_list_selected, false);
                }
                return;
            }
            return;
        }
        if (pending_close_confirm) {
            switch (ui::parse_confirmation_key(ch)) {
                case ui::ConfirmationKeyResult::Accepted: {
                    size_t idx = pending_close_index != static_cast<size_t>(-1) ? pending_close_index
                                                                                : active_buffer;
                    perform_buffer_close(idx, true);
                    return;
                }
                case ui::ConfirmationKeyResult::Rejected:
                    pending_close_confirm = false;
                    pending_close_index = static_cast<size_t>(-1);
                    minibuffer_message(minibuffer, "Close cancelled");
                    return;
                case ui::ConfirmationKeyResult::Pending:
                    minibuffer_message(minibuffer, ui::kConfirmationRetryPrompt);
                    return;
            }
        }
        if (minibuffer.active && is_assist_minibuffer_action(minibuffer.action)) {
            if (ch == 27) {
                pending_assist = PendingAssist{};
                exit_assist_command_mode(minibuffer, assist_completer);
                return;
            }
            if (minibuffer.action == MinibufferAction::AssistCommand) {
                if (ch == '\t') {
                    const AssistCompletionResult result = complete_assist_command(
                        minibuffer.input,
                        assist_completer,
                        ai_continue.has_value() ? ai_continue->assist_config : assist_config);
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
                const EditorAssistConfig& active_assist_config =
                    ai_continue.has_value() ? ai_continue->assist_config : assist_config;
                const EditorAssistCommand* scope_command = nullptr;
                if (pending_assist.kind == AssistCommandKind::Configured &&
                    pending_assist.command_index < active_assist_config.commands.size()) {
                    scope_command = &active_assist_config.commands[pending_assist.command_index];
                }
                auto command_has_mode = [&](AssistCommandMode mode) {
                    return scope_command != nullptr &&
                           std::find(scope_command->modes.begin(), scope_command->modes.end(), mode) !=
                               scope_command->modes.end();
                };
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
                } else if ((ch == 'c' || ch == 'C') && command_has_mode(AssistCommandMode::Continue)) {
                    start_assist(pending_assist.kind,
                                 pending_assist.command_index,
                                 AssistScope::Continue,
                                 "",
                                 std::nullopt);
                } else if ((ch == 'i' || ch == 'I' || ch == 'l' || ch == 'L') &&
                           command_has_mode(AssistCommandMode::Insert)) {
                    start_assist(pending_assist.kind,
                                 pending_assist.command_index,
                                 AssistScope::Insert,
                                 "",
                                 std::nullopt);
                } else if ((ch == 'n' || ch == 'N') && command_has_mode(AssistCommandMode::NewBuffer)) {
                    start_assist(pending_assist.kind,
                                 pending_assist.command_index,
                                 AssistScope::NewBuffer,
                                 "",
                                 std::nullopt);
                } else if ((ch == 'v' || ch == 'V') && command_has_mode(AssistCommandMode::NewBuffer)) {
                    start_assist(pending_assist.kind,
                                 pending_assist.command_index,
                                 AssistScope::NewBufferVSplit,
                                 "",
                                 std::nullopt);
                } else if ((ch == 'h' || ch == 'H') && command_has_mode(AssistCommandMode::NewBuffer)) {
                    start_assist(pending_assist.kind,
                                 pending_assist.command_index,
                                 AssistScope::NewBufferHSplit,
                                 "",
                                 std::nullopt);
                }
                return;
            }
            if (minibuffer.action == MinibufferAction::AssistPromptMode) {
                const std::optional<AssistPromptMode> prompt_mode =
                    assist_prompt_mode_for_key(ch);
                if (prompt_mode.has_value()) {
                    start_assist(pending_assist.kind,
                                 pending_assist.command_index,
                                 std::nullopt,
                                 pending_assist.custom_prompt,
                                 *prompt_mode);
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
                                  pending_save,
                                  pending_autosave_recovery,
                                  minibuffer_path_completer)) {
            return;
        }
        if (handle_replace_key(state, minibuffer, replace, ch)) {
            return;
        }

        // Ctrl+G window-command prefix (Emacs-style): v/h/2/3/o/0/1.
        if (window_prefix_active) {
            const std::string action = window_prefix_action(ch);
            window_prefix_active = false;
            if (action == "cancel" || action.empty()) {
                minibuffer_message(minibuffer,
                                   action == "cancel" ? "Window command cancelled"
                                                      : "Unknown window command (v/h/o/0/1)");
                return;
            }
            if (action == "split-v") {
                apply_vsplit();
            } else if (action == "split-h") {
                apply_hsplit();
            } else if (action == "other") {
                if (!split_layout.has_split()) {
                    minibuffer_message(minibuffer, "No other pane");
                } else {
                    sync_active_buffer();
                    split_layout.focus_next();
                    focus_buffer_from_split();
                    minibuffer_message(minibuffer,
                                       "Pane " + std::to_string(split_layout.focused_leaf() + 1) +
                                           "/" + std::to_string(split_layout.leaf_count()) + ": " +
                                           editor_buffer_display_name(state, active_buffer));
                }
            } else if (action == "close") {
                apply_closesplit();
            } else if (action == "maximize") {
                apply_maximize_split();
            }
            return;
        }
        if (ch == 7) {
            if (help_view.active || picker.active || buffer_list_active || pending_close_confirm ||
                assist_session.active || reformat_session.active || insert_session.active) {
                minibuffer_message(minibuffer, "Finish the current action before window commands");
                return;
            }
            window_prefix_active = true;
            minibuffer_message(minibuffer, "Window: v/3 vertical  h/2 horizontal  o other  0 close  1 max");
            return;
        }

        // Ctrl+B / Ctrl+D: PageUp / PageDown in the previously focused other pane.
        if ((ch == 2 || ch == 4) && split_layout.has_split() && !help_view.active &&
            !picker.active && !buffer_list_active) {
            const std::optional<size_t> other_leaf = split_layout.other_scroll_leaf();
            if (!other_leaf.has_value()) {
                minibuffer_message(minibuffer, "No other pane to scroll");
                return;
            }
            const Rect area = editor_main_area();
            const std::vector<SplitPaneRect> panes = split_layout.layout_panes(area);
            const SplitPaneRect* target_pane = nullptr;
            for (const SplitPaneRect& pane : panes) {
                if (pane.leaf_index == *other_leaf) {
                    target_pane = &pane;
                    break;
                }
            }
            if (target_pane == nullptr) {
                minibuffer_message(minibuffer, "No other pane to scroll");
                return;
            }
            EditorState* target_state = nullptr;
            if (target_pane->buffer_index == active_buffer) {
                target_state = &state;
            } else if (target_pane->buffer_index < buffers.size()) {
                target_state = &buffers[target_pane->buffer_index];
            }
            if (target_state == nullptr) {
                minibuffer_message(minibuffer, "No other pane to scroll");
                return;
            }
            const MovementKey key =
                ch == 2 ? MovementKey::PageUp : MovementKey::PageDown;
            target_state->apply_movement(key, target_pane->rect, false, false, false);
            minibuffer_message(minibuffer,
                               std::string(ch == 2 ? "Scrolled other pane up" : "Scrolled other pane down") +
                                   " (pane " + std::to_string(*other_leaf + 1) + "/" +
                                   std::to_string(split_layout.leaf_count()) + ")");
            return;
        }

        if (ch == 16) {
            request_switch_to_chat();
            return;
        }
        if (ch == 18) {
            regenerate_last_assist();
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
        } else if (is_editor_undo_key(ch)) {
            minibuffer_message(minibuffer, state.undo() ? "Undone" : "Nothing to undo");
        } else if (is_editor_redo_key(ch)) {
            minibuffer_message(minibuffer, state.redo() ? "Redone" : "Nothing to redo");
        } else if (ch == 1) {
            state.select_all();
        } else if (ch == 11) {
            Error kill_error = state.kill_to_line_end(shared_clipboard());
            if (!kill_error.ok()) {
                minibuffer_message(minibuffer, kill_error.message);
            }
        } else if (ch == 14) {
            new_empty_buffer();
        } else if (ch == 15) {
            start_minibuffer(minibuffer,
                             MinibufferAction::LoadFile,
                             "Open file: ",
                             "",
                             &minibuffer_path_completer);
        } else if (ch == 0) {
            start_continue();
        } else if (ch == 19) {
            trigger_save();
        } else if (ch == 12) {
            enter_buffer_list();
        } else if (ch == 23) {
            close_active_buffer(false);
        } else if (ch == editor_key_save_as()) {
            start_minibuffer(minibuffer,
                             MinibufferAction::SaveAsFile,
                             "Save as: ",
                             state.path,
                             &minibuffer_path_completer);
        } else if (ch == '\t') {
            if (state.selection.has_range()) {
                word_completer.reset();
                Error indent_error = state.indent();
                if (!indent_error.ok()) {
                    minibuffer_message(minibuffer, indent_error.message);
                }
            } else {
                const WordCompletionResult completion =
                    word_completer.complete(state, buffers, active_buffer);
                if (!completion.error.ok()) {
                    minibuffer_message(minibuffer, completion.error.message);
                } else if (completion.completed) {
                    minibuffer_message(minibuffer, word_completion_status(completion));
                } else {
                    Error indent_error = state.indent();
                    if (!indent_error.ok()) {
                        minibuffer_message(minibuffer, indent_error.message);
                    }
                }
            }
        } else if (ch == editor_key_backtab()) {
            Error outdent_error = state.outdent();
            if (!outdent_error.ok()) {
                minibuffer_message(minibuffer, outdent_error.message);
            }
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
                    const Rect focus_rect = assist_panel_rect();
                    dispatch_escape_sequence(state, sequence, escape_status, last_search, &focus_rect);
                    if (!escape_status.empty()) {
                        minibuffer_message(minibuffer, escape_status);
                    }
                    return;
                }
                if (window_prefix_active) {
                    window_prefix_active = false;
                    minibuffer_message(minibuffer, "Window command cancelled");
                    return;
                }
                start_assist_command_mode(minibuffer, assist_completer);
                return;
            }
            std::string escape_status;
            const Rect focus_rect = assist_panel_rect();
            handle_escape(state, escape_status, last_search, &focus_rect);
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
        if (picker.active) {
            minibuffer_message(minibuffer, "Choose an item or press Esc to cancel");
            return;
        }
        if (buffer_list_active) {
            minibuffer_message(minibuffer, "Choose a buffer or press Esc to cancel");
            return;
        }
        if (pending_close_confirm) {
            minibuffer_message(minibuffer, ui::kConfirmationRetryPrompt);
            return;
        }
        if (minibuffer.active) {
            const Error paste_error = paste_into_minibuffer(minibuffer, terminal_text);
            if (paste_error.ok()) {
                assist_completer = AssistCompleterState{};
                minibuffer_path_completer.reset();
                minibuffer.message = "Pasted into minibuffer";
            } else {
                minibuffer.message = paste_error.message;
            }
            return;
        }
        Error paste_error = paste_with_clipboard_preference(state, shared_clipboard(), terminal_text);
        minibuffer_message(minibuffer, paste_error.ok() ? "Pasted" : paste_error.message);
    };

    auto prompt_for_changed_read_only_file = [&]() {
        if (state.reload_required &&
            (!minibuffer.active || minibuffer.action != MinibufferAction::ConfirmReloadAfterLock)) {
            start_minibuffer(minibuffer,
                             MinibufferAction::ConfirmReloadAfterLock,
                             state.path +
                                 " changed while locked by another editor. Reload it and make this "
                                 "buffer writable? (y/n) ");
        }
    };

    using SteadyClock = std::chrono::steady_clock;
    SteadyClock::time_point last_activity = SteadyClock::now();

    auto autosave_context_ready = [&]() {
        return !help_view.active && !picker.active && !buffer_list_active && !pending_close_confirm &&
               !minibuffer.active;
    };

    auto try_autosave = [&](SteadyClock::duration idle_time) {
        if (!autosave_context_ready()) {
            return;
        }
        sync_active_buffer();
        const AutosaveEvaluation evaluation = evaluate_autosave(state, settings, idle_time);
        if (!evaluation.should_save) {
            return;
        }
        std::string message;
        const Error err = perform_autosave(state, settings, message);
        if (err.ok()) {
            minibuffer_message(minibuffer, message);
            sync_active_buffer();
        } else if (!message.empty()) {
            minibuffer_message(minibuffer, message);
        }
    };

    if (interactive != nullptr && interactive->pending_editor_assist.active) {
        const app::PendingEditorAssistFromChat pending = interactive->pending_editor_assist;
        interactive->pending_editor_assist = {};
        const EditorAssistConfig& active_assist_config =
            ai_continue.has_value() ? ai_continue->assist_config : assist_config;
        if (pending.selection_text.empty()) {
            minibuffer_message(minibuffer, "AI new-buffer command requires selected text");
        } else if (pending.command_index >= active_assist_config.commands.size()) {
            minibuffer_message(minibuffer, "Configured assist command index is out of range");
        } else {
            EditorState assist_state = EditorState::from_text(pending.selection_text);
            assist_state.selection.anchor = 0;
            assist_state.selection.active = assist_state.text.size();
            start_assist(AssistCommandKind::Configured,
                         pending.command_index,
                         AssistScope::NewBuffer,
                         "",
                         std::nullopt,
                         &assist_state);
        }
    }

    while (!quit) {
        const bool assist_updated = process_assist_events();
        const bool reformat_updated = process_reformat_events();
        const bool insert_updated = process_insert_events();
        if (pending_regenerate_restart) {
            pending_regenerate_restart = false;
            if (last_assist_command.valid) {
                start_assist(last_assist_command.kind,
                             last_assist_command.command_index,
                             last_assist_command.scope,
                             last_assist_command.custom_prompt,
                             last_assist_command.prompt_mode);
                minibuffer_message(minibuffer, "Regenerating...");
            }
        }
        const bool model_updated = process_model_events();
        const bool assist_animating =
            assist_session.active && assist_session.activity_kind != tui::ActivityKind::None;
        if (assist_animating) {
            ++activity_frame;
        }
        if (assist_updated) {
            try_autosave(SteadyClock::now() - last_activity);
        }

        TerminalInputEvent event;
        if (!read_terminal_input(event, 100)) {
            const SteadyClock::time_point now = SteadyClock::now();
            try_autosave(now - last_activity);
            const TerminalSize current_size = terminal_size();
            if (current_size.rows != last_size.rows || current_size.cols != last_size.cols ||
                assist_session.job.running() || model_list.job.running() || assist_updated ||
                reformat_session.job.running() || reformat_updated || insert_session.job.running() ||
                insert_updated || model_updated ||
                assist_animating) {
                last_size = current_size;
                render_editor();
            }
            continue;
        }

        last_activity = SteadyClock::now();
        if (event.type == TerminalInputType::BracketedPaste) {
            handle_paste(event.text);
            prompt_for_changed_read_only_file();
        } else if (event.type == TerminalInputType::Byte) {
            handle_key(event.byte);
            prompt_for_changed_read_only_file();
            while (!quit) {
                if (!read_terminal_input(event, 0)) {
                    break;
                }
                last_activity = SteadyClock::now();
                if (event.type == TerminalInputType::BracketedPaste) {
                    handle_paste(event.text);
                    prompt_for_changed_read_only_file();
                } else if (event.type == TerminalInputType::Byte) {
                    handle_key(event.byte);
                    prompt_for_changed_read_only_file();
                } else {
                    break;
                }
            }
        }
        try_autosave(SteadyClock::duration::zero());

        last_size = terminal_size();
        render_editor();
    }
    sync_active_buffer();
    if (switch_to_chat) {
        return {0, app::InteractiveUiTarget::Chat};
    }
    return {0, app::InteractiveUiTarget::Quit};
}

}  // namespace ainiux::editor
