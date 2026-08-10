#include "app/interactive_mode.hpp"
#include "app/user_shell.hpp"
#include "chat/settings.hpp"
#include "chat/generation_settings.hpp"
#include "chat/sqlite_store.hpp"
#include "common.hpp"
#include "security/redact.hpp"
#include "editor/autosave.hpp"
#include "editor/editor.hpp"
#include "editor/ai_continue.hpp"
#include "editor/editor_ai_setup.hpp"
#include "editor/clipboard.hpp"
#include "editor/detail/editor_common.hpp"
#include "editor/assist_runtime.hpp"
#include "editor/dired.hpp"
#include "editor/line_diff.hpp"
#include "editor/editor_assist.hpp"
#include "editor/editor_help.hpp"
#include "editor/editor_picker.hpp"
#include "editor/model_list_runtime.hpp"
#include "editor/path_completion.hpp"
#include "editor/reformat.hpp"
#include "editor/split.hpp"
#include "editor/text_layout.hpp"
#include "editor/terminal_input.hpp"
#include "editor/terminal_ui.hpp"
#include "input/input.hpp"
#include "runtime/runtime.hpp"
#include "provider/model_selection.hpp"
#include "search/search.hpp"
#include "tui/activity.hpp"
#include "tui/detail/frame_buffer.hpp"
#include "tui/events.hpp"
#include "tui/theme_registry.hpp"
#include "tui/tui.hpp"
#include "ui/confirmation.hpp"
#include "ui/provider_model_display.hpp"
#include "ui/provider_model_selector.hpp"
#include "ui/scrollbar.hpp"
#include "ui/text_selector.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <vector>

namespace ainiux::editor {
namespace {

struct HelpViewSession {
    bool active = false;
    bool settings = false;
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

struct ShellEvent {
    Error error;
    std::string status_line;
    std::string buffer_text;
    long long duration_ms = 0;
    std::int64_t exit_status = -1;
    bool failed = false;
};

struct ShellSession {
    bool active = false;
    bool cancel_requested = false;
    std::string command;
    runtime::EventQueue<ShellEvent> events;
    runtime::JobHandle job;
};

struct EditorSelectionSaveEvent {
    Error error;
};

struct PendingClipboardPaste {
    bool active = false;
    bool minibuffer = false;
    bool terminal_query = false;
    std::uint64_t generation = 0;
    std::uint64_t buffer_id = 0;
    std::uint64_t revision = 0;
    size_t cursor = 0;
    Selection selection;
    MinibufferAction minibuffer_action = MinibufferAction::None;
    std::string minibuffer_input;
    std::chrono::steady_clock::time_point query_deadline;
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
    size_t text_align_width_default = settings.text_align_width;
    if (!valid_text_align_width(text_align_width_default)) {
        text_align_width_default = kDefaultTextAlignWidth;
    }
    bool highlight_enabled = interactive != nullptr ? interactive->highlight_enabled
                                                    : settings.highlight_enabled;
    state.highlight_enabled = highlight_enabled;
    const std::string initial_path = expand_user_path(path.empty() ? save_as : path);
    state.set_path(initial_path);
    std::string status = "Ready";
    std::string initial_linebreak_warning;
    std::string initial_lock_warning;
    bool switch_to_chat = false;
    bool switch_to_agent = false;
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
        std::error_code initial_path_error;
        const bool initial_target_exists =
            !initial_path.empty() &&
            std::filesystem::exists(std::filesystem::u8path(initial_path), initial_path_error) &&
            !initial_path_error;
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
    std::string pending_reasoning;
    PendingAutosaveRecovery pending_autosave_recovery;
    AssistSession assist_session;
    // Shown after AI assist; merged into the next auto-save minibuffer line when both fire.
    std::string last_generated_tokens_message;
    ReformatSession reformat_session;
    InsertSession insert_session;
    ShellSession shell_session;
    StoredAssistCommand last_assist_command;
    AssistCompleterState assist_completer;
    PathCompleter minibuffer_path_completer;
    WordCompleter word_completer;
    PendingAssist pending_assist;
    HelpViewSession help_view;
    DiredState dired;
    bool dired_overwrite_is_rename = false;
    bool buffer_list_active = false;
    size_t buffer_list_selected = 0;
    int buffer_list_scroll = 0;
    EditorState buffer_list_view;
    ui::TextSelectorNavState buffer_list_nav;
    EditorProviderModelPicker picker;
    EditorModelListRuntime model_list;
    provider::ModelsResult cached_editor_models;
    bool have_cached_editor_models = false;
    bool model_list_context_only = false;
    runtime::EventQueue<EditorSelectionSaveEvent> selection_save_events;
    runtime::JobHandle selection_save_job;
    std::string pending_selection_save;
    bool pending_close_confirm = false;
    size_t pending_close_index = static_cast<size_t>(-1);
    TerminalSize last_size = terminal_size();
    size_t activity_frame = 0;
    const auto activity_animation_started = std::chrono::steady_clock::now();
    std::string theme_name =
        interactive != nullptr && !interactive->theme_name.empty() ? interactive->theme_name
                                                                   : settings.theme_name;
    bool use_colors = interactive != nullptr ? interactive->use_colors : settings.use_colors;
    bool show_scrollbars = interactive != nullptr ? interactive->show_scrollbars : true;
    bool mouse_view_detached = false;
    EscapeRepeatGuard assist_escape_repeat_guard;
    bool current_escape_was_decoded = false;
    input::InsertSourceOptions insert_options;
    const cli::Options* runtime_options = nullptr;
    if (interactive != nullptr) {
        runtime_options = &interactive->context.options;
    } else if (ai_continue.has_value()) {
        runtime_options = &ai_continue->request.options;
    }
    // Theme paint must never see a null registry. Options owns the loaded
    // themes.conf / defaults; EditorSettings.themes is only a non-owning hint.
    // resolve_theme_registry falls back to process-lifetime builtins if needed.
    const tui::ThemeRegistry* preferred_themes = settings.themes;
    const tui::ThemeRegistry* options_themes =
        runtime_options != nullptr ? &runtime_options->tui_themes : nullptr;
    const tui::ThemeRegistry& themes_ref =
        tui::resolve_theme_registry(preferred_themes, options_themes);
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
    themes_ref.normalize_name(theme_name, theme_name);
    auto terminal_theme_style = [&]() {
        const tui::ColorModePreference preference =
            runtime_options != nullptr ? runtime_options->color_mode
                                       : tui::ColorModePreference::Auto;
        return TerminalThemeStyle{&themes_ref,
                                  theme_name,
                                  use_colors,
                                  tui::resolve_color_mode(use_colors, preference)};
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
    auto current_status_chrome = [&]() -> EditorStatusChrome {
        EditorStatusChrome chrome;
        if (ai_continue.has_value() && !ai_continue->request.options.model.empty()) {
            chrome.model = ai_continue->request.options.model;
            chrome.reasoning =
                config::reasoning_selection_value(ai_continue->request.options.reasoning);
        }
        return chrome;
    };

    auto sync_active_buffer = [&]() {
        if (!help_view.active && active_buffer < buffers.size()) {
            buffers[active_buffer] = state;
        }
    };

    auto save_focused_pane_view = [&]() {
        if (!help_view.active) {
            split_layout.set_focused_view(pane_view_from_state(state));
        }
    };

    auto focus_buffer_from_split = [&]() {
        if (buffers.empty()) {
            return;
        }
        const size_t next = std::min(split_layout.focused_buffer(), buffers.size() - 1);
        if (next != active_buffer) {
            sync_active_buffer();
            active_buffer = next;
            state = buffers[active_buffer];
            state.highlight_enabled = highlight_enabled;
            split_layout.set_focused_buffer(active_buffer);
        } else {
            split_layout.set_focused_buffer(active_buffer);
        }
        // Always apply the leaf's own cursor/scroll, even when the buffer is unchanged.
        apply_pane_view_to_state(state, split_layout.focused_view());
    };

    bool pending_agent_guard = false;
    agent::GuardApprovalRequest agent_guard_request;
    bool pending_agent_quit_confirm = false;
    std::string last_agent_status_badge;
    std::chrono::steady_clock::time_point last_dired_sync{};

    auto agent_controller = [&]() -> agent::AgentController* {
        if (interactive == nullptr || !interactive->agent_controller) return nullptr;
        return interactive->agent_controller.get();
    };

    auto refresh_agent_background_status = [&]() {
        agent::AgentController* ctl = agent_controller();
        if (!ctl) return;
        if (ctl->waiting_guard() || (ctl->approval_gate() && ctl->approval_gate()->has_pending())) {
            agent::GuardApprovalRequest pending;
            if (ctl->approval_gate() && ctl->approval_gate()->try_get_pending(pending)) {
                if (!pending_agent_guard ||
                    pending.command_preview != agent_guard_request.command_preview) {
                    agent_guard_request = std::move(pending);
                    pending_agent_guard = true;
                    std::string prompt = "Agent Guard: ";
                    if (!agent_guard_request.tool_name.empty()) {
                        prompt += agent_guard_request.tool_name;
                        prompt += " · ";
                    }
                    if (!agent_guard_request.command_preview.empty()) {
                        prompt += agent_guard_request.command_preview;
                    } else if (!agent_guard_request.message.empty()) {
                        prompt += agent_guard_request.message;
                    } else {
                        prompt += "approval required";
                    }
                    prompt += " (y/n) ";
                    minibuffer_message(minibuffer, prompt);
                }
            }
            return;
        }
        if (pending_agent_guard) {
            // Resolved elsewhere (e.g. cancel); clear local modal state.
            pending_agent_guard = false;
            agent_guard_request = {};
        }
        if (ctl->turn_running()) {
            const std::string badge = ctl->status_label().empty()
                                          ? std::string("Agent running · Ctrl+G to return")
                                          : ctl->status_label() + " · Ctrl+G to return";
            if (badge != last_agent_status_badge && !minibuffer.active && !pending_agent_guard &&
                !pending_close_confirm && !pending_agent_quit_confirm) {
                last_agent_status_badge = badge;
                minibuffer_message(minibuffer, badge);
            }
        } else if (!last_agent_status_badge.empty() &&
                   last_agent_status_badge.find("Agent") != std::string::npos) {
            const std::string done = ctl->status_label().empty()
                                         ? std::string("Agent done · Ctrl+G to return")
                                         : ctl->status_label() + " · Ctrl+G to return";
            if (done != last_agent_status_badge && !minibuffer.active && !pending_agent_guard) {
                last_agent_status_badge = done;
                minibuffer_message(minibuffer, done);
            }
        }
    };

    auto can_leave_editor_for_mode_switch = [&]() {
        if (interactive == nullptr) {
            return false;
        }
        // Dired may still be active: leave_editor_for closes it first so Ctrl+G
        // from dired can return to agent without requiring q first.
        // pending_agent_guard is allowed: Ctrl+G leaves Guard Ask pending for
        // the agent TUI (must not auto-deny).
        return !help_view.active && !picker.active && !buffer_list_active &&
               !pending_close_confirm && !pending_agent_quit_confirm &&
               !minibuffer.active && !replace.active &&
               !assist_session.active && !reformat_session.active && !insert_session.active &&
               !shell_session.active && !window_prefix_active;
    };

    auto leave_editor_for = [&](app::InteractiveUiTarget target) {
        if (!can_leave_editor_for_mode_switch()) {
            if (interactive != nullptr) {
                minibuffer_message(minibuffer, "Cannot switch mode right now");
            }
            return;
        }
        if (dired.active) {
            dired_close(dired);
            dired_overwrite_is_rename = false;
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
        // start_dired is a one-shot entry flag (agent F4 / CLI); never persist it.
        interactive->editor_settings.start_dired = false;
        interactive->editor_settings.start_dired_path.clear();
        interactive->assist_config = assist_config;
        interactive->highlight_enabled = highlight_enabled;
        interactive->theme_name = theme_name;
        interactive->use_colors = use_colors;
        interactive->show_scrollbars = show_scrollbars;
        interactive->ai_continue = ai_continue;
        app::sync_editor_provider_to_shared(*interactive, ai_continue);
        if (target == app::InteractiveUiTarget::Agent) {
            switch_to_agent = true;
        } else {
            switch_to_chat = true;
        }
        quit = true;
    };

    auto request_switch_to_chat = [&]() { leave_editor_for(app::InteractiveUiTarget::Chat); };
    auto request_switch_to_agent = [&]() { leave_editor_for(app::InteractiveUiTarget::Agent); };
    auto request_editor_toggle = [&]() {
        if (interactive != nullptr &&
            app::editor_toggle_target(*interactive) == app::InteractiveUiTarget::Agent) {
            request_switch_to_agent();
        } else {
            request_switch_to_chat();
        }
    };

    auto buffer_list_underlying = [&](size_t display_selected) -> size_t {
        return ui::text_selector_underlying_index(buffer_list_nav.display_order, display_selected,
                                                  buffers.size());
    };

    auto buffer_list_label_at = [&](size_t display_index) -> std::string {
        const size_t underlying = buffer_list_underlying(display_index);
        if (underlying >= buffers.size()) {
            return {};
        }
        return editor_buffer_display_name(buffers[underlying], underlying);
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
        // Buffer add/remove invalidates any display permutation; restore identity order.
        if (buffer_list_nav.sorted &&
            buffer_list_nav.display_order.size() != buffers.size()) {
            buffer_list_nav.sorted = false;
            buffer_list_nav.display_order.clear();
        }
        buffer_list_selected = std::min(buffer_list_selected, buffers.size() - 1);
        buffer_list_view = EditorState::from_text(
            editor_buffer_list_text(buffers, buffer_list_selected, buffer_list_nav.display_order));
        buffer_list_view.set_path("[buffers]");
        buffer_list_view.highlight_enabled = false;
        const size_t selected_line = std::min(buffer_list_selected + 1, buffer_list_view.text.line_count() - 1);
        buffer_list_view.cursor = buffer_list_view.text.line_start(selected_line);
        buffer_list_view.dirty = false;
        buffer_list_view.clear_undo_history();
    };

    tui::detail::TerminalFrameRenderer terminal_frame_renderer;
    auto refresh_dired_list_view = [&]() {
        buffer_list_view = EditorState::from_text(dired_list_text(dired));
        buffer_list_view.set_path("[dired]");
        buffer_list_view.highlight_enabled = false;
        buffer_list_view.dirty = false;
        buffer_list_view.clear_undo_history();
        if (!dired.entries.empty() && dired.selected < dired.entries.size()) {
            // Title rule + blank + entry lines: keep selection near cursor via panel highlight.
            const size_t line = std::min(dired.selected, buffer_list_view.text.line_count() - 1);
            buffer_list_view.cursor = buffer_list_view.text.line_start(line);
        }
    };

    auto render_editor = [&]() {
        // Keep the focused leaf's view in sync so unfocused panes stay independent.
        if (!help_view.active && !dired.active && !picker.active && !buffer_list_active) {
            split_layout.set_focused_view(pane_view_from_state(state));
        }
        const TerminalThemeStyle theme_style = terminal_theme_style();
        if (picker.active) {
            picker.refresh_view();
            render_terminal_panel(picker.view,
                                  minibuffer,
                                  terminal_frame_renderer,
                                  theme_style,
                                  picker.for_provider ? tui::TuiMode::ProviderList
                                      : picker.for_reasoning ? tui::TuiMode::ReasoningList
                                                             : tui::TuiMode::ModelList,
                                  picker.scroll);
            return;
        }
        if (dired.active && dired.focus == DiredFocus::List) {
            refresh_dired_list_view();
            const std::string title = dired_header_line(dired);
            const std::string dired_status = dired_status_line(dired);
            const std::vector<tui::StyledLine> body = dired_list_body_lines(dired);
            render_terminal_panel(buffer_list_view,
                                  minibuffer,
                                  terminal_frame_renderer,
                                  theme_style,
                                  tui::TuiMode::ThreadList,
                                  dired.list_scroll,
                                  title.c_str(),
                                  dired_status.c_str(),
                                  &body);
            return;
        }
        if (dired.active && dired.focus == DiredFocus::View) {
            dired.view.highlight_enabled = highlight_enabled;
            dired.view.read_only = true;
            const std::string dired_status = dired_status_line(dired);
            const std::vector<bool>* changed =
                dired.view_changed_lines.empty() ? nullptr : &dired.view_changed_lines;
            render_terminal(dired.view,
                            minibuffer,
                            terminal_frame_renderer,
                            theme_style,
                            false,
                            nullptr,
                            show_scrollbars,
                            true,
                            {},
                            dired_status.c_str(),
                            changed);
            return;
        }
        if (buffer_list_active) {
            refresh_buffer_list_view();
            render_terminal_panel(buffer_list_view,
                                  minibuffer,
                                  terminal_frame_renderer,
                                  theme_style,
                                  // The editor buffer selector uses the same panel widget as chat /list.
                                  tui::TuiMode::ThreadList,
                                  buffer_list_scroll,
                                  "Buffers");
            return;
        }
        state.highlight_enabled = highlight_enabled;
        const EditorStatusChrome status_chrome = current_status_chrome();
        if (help_view.active || !split_layout.has_split()) {
            render_terminal(state, minibuffer, terminal_frame_renderer, theme_style,
                            help_view.active, refresh_assist_display(), show_scrollbars,
                            !mouse_view_detached, status_chrome);
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
            terminal_frame_renderer,
            theme_style,
            help_view.active,
            refresh_assist_display(),
            split_layout.leaf_count(),
            show_scrollbars,
            !mouse_view_detached,
            status_chrome);
    };
    render_editor();

    auto assist_panel_rect = [&]() {
        if (split_layout.has_split()) {
            return editor_content_rect(split_layout.focused_rect(editor_main_area()));
        }
        return editor_content_rect(editor_main_area());
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
        help_view.settings = false;
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
        help_view.settings = false;
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
        minibuffer_message(minibuffer, "Help (read-only) — Ctrl+H, Esc /help, or Ctrl+Q to return");
    };

    auto enter_settings_view = [&](const std::string& text) {
        if (help_view.active) {
            if (help_view.settings) {
                exit_help_view();
                return;
            }
            exit_help_view();
        }
        if (assist_session.active || reformat_session.active || insert_session.active) {
            minibuffer_message(minibuffer, "Finish or cancel the active job before opening settings");
            return;
        }
        help_view.saved = state.capture_state();
        help_view.saved_path = state.path;
        help_view.saved_dirty = state.dirty;
        help_view.saved_language = state.language;
        help_view.saved_language_automatic = state.language_automatic;
        help_view.active = true;
        help_view.settings = true;
        state.text = PieceTable::from_string(text);
        state.invalidate_word_index();
        state.cursor = 0;
        state.preferred_column = 0;
        state.scroll_line = 0;
        state.scroll_column = 0;
        state.dirty = false;
        state.set_language(highlight::Language::Text, false);
        state.clear_selection();
        state.clear_undo_history();
        minibuffer_message(minibuffer, "Settings (read-only) — /setting, Esc, or Ctrl+Q to return");
    };

    auto exit_dired = [&]() {
        dired_close(dired);
        dired_overwrite_is_rename = false;
        minibuffer_message(minibuffer, "Left dired — editor mode");
    };

    auto enter_dired = [&](const std::string& path_or_glob) {
        if (help_view.active) {
            exit_help_view();
        }
        if (buffer_list_active) {
            buffer_list_active = false;
        }
        if (picker.active) {
            picker.active = false;
        }
        if (assist_session.active || reformat_session.active || insert_session.active ||
            shell_session.active) {
            minibuffer_message(minibuffer, "Finish or cancel the active job before opening dired");
            return;
        }
        if (minibuffer.active) {
            minibuffer_message(minibuffer, "Cancel the minibuffer before opening dired");
            return;
        }
        std::string target = path_or_glob;
        if (target.empty()) {
            target = state.path.empty() ? std::string(".") : state.path;
            // If current buffer is a file, open its directory.
            if (!state.path.empty()) {
                std::error_code ec;
                const std::filesystem::path p = std::filesystem::u8path(state.path);
                if (std::filesystem::is_regular_file(p, ec) || !std::filesystem::is_directory(p, ec)) {
                    target = p.has_parent_path() ? p.parent_path().u8string() : std::string(".");
                }
            }
        }
        Error err = dired_open(dired, target);
        if (!err.ok()) {
            dired_close(dired);
            minibuffer_message(minibuffer, err.message);
            return;
        }
        minibuffer_message(minibuffer, dired_status_line(dired));
    };

    auto activate_buffer = [&](size_t index) {
        if (index >= buffers.size()) {
            return;
        }
        save_focused_pane_view();
        sync_active_buffer();
        active_buffer = index;
        state = buffers[active_buffer];
        state.highlight_enabled = highlight_enabled;
        split_layout.set_focused_buffer(active_buffer);
        // Buffer switch on this pane: adopt the buffer's stored point as the pane view.
        split_layout.set_focused_view(pane_view_from_state(state));
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
        split_layout.set_focused_view(pane_view_from_state(state));
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

    auto dired_open_for_edit = [&]() {
        const DiredEntry* entry = dired_selected_entry(dired);
        if (entry == nullptr || entry->is_parent) {
            minibuffer_message(minibuffer, "Nothing to open");
            return;
        }
        if (entry->is_directory) {
            minibuffer_message(minibuffer, "Use Enter to open a directory in dired");
            return;
        }
        const std::string path = entry->path;
        dired_close(dired);
        dired_overwrite_is_rename = false;
        request_open_buffer_from_path(path);
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
        split_layout.set_focused_view(pane_view_from_state(state));
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
        buffer_list_nav.reset_for_open();
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
        buffer_list_nav.reset_for_open();
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
                buffer_list_nav.reset_for_open();
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
            // Rebuilding the list after close is simpler and safer than patching a sort order.
            buffer_list_nav.reset_for_open();
            buffer_list_selected = std::min(active_buffer, buffers.size() - 1);
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
        const std::string sequence = current_escape_was_decoded ? std::string{} : read_escape_suffix();
        if (sequence.empty()) {
            cancel_buffer_list();
            return;
        }
        if (sequence == "[2~") {
            // Insert creates a new empty buffer in the list picker.
            new_empty_buffer();
            return;
        }
        if (sequence == "[3~") {
            if (!buffers.empty() && buffer_list_selected < buffers.size()) {
                perform_buffer_close(buffer_list_underlying(buffer_list_selected), false);
            }
            return;
        }
        MovementKeyEvent movement;
        if (parse_movement_sequence(sequence, movement)) {
            if (buffer_list_nav.search_active) {
                buffer_list_nav.search_active = false;
                buffer_list_nav.search_draft.clear();
            }
            buffer_list_selected =
                move_editor_buffer_selection(buffer_list_selected, buffers.size(), movement.key);
            minibuffer_message(minibuffer, selected_buffer_status());
            return;
        }
    };

    auto active_model_options = [&]() -> const cli::Options* {
        if (ai_continue.has_value()) return &ai_continue->request.options;
        if (interactive != nullptr) return &interactive->context.options;
        return nullptr;
    };

    auto mutable_model_options = [&]() -> cli::Options* {
        if (ai_continue.has_value()) return &ai_continue->request.options;
        if (interactive != nullptr) return &interactive->context.options;
        return nullptr;
    };

    auto refresh_settings_view = [&]() {
        if (!help_view.active || !help_view.settings) return;
        const cli::Options* options = active_model_options();
        if (options == nullptr) return;
        const size_t retained_mouse_scroll = state.scroll_line;
        state.text = PieceTable::from_string(chat::format_settings_panel(
            *options,
            ai_continue.has_value()
                ? provider::reasoning_temperature_advisory(ai_continue->request)
                : std::string{}));
        state.invalidate_word_index();
        state.cursor = 0;
        state.scroll_line = mouse_view_detached ? retained_mouse_scroll : 0;
        state.scroll_column = 0;
        state.dirty = false;
        state.clear_selection();
        state.clear_undo_history();
    };

    auto start_pending_selection_save = [&]() {
        if (selection_save_job.running() || pending_selection_save.empty()) return;
        std::string value = std::move(pending_selection_save);
        pending_selection_save.clear();
        selection_save_job.start([value = std::move(value), &selection_save_events](runtime::CancellationToken) {
            EditorSelectionSaveEvent event;
            chat::SqliteStore store;
            event.error = store.open_default();
            if (event.error.ok()) {
                event.error = store.set_app_state("editor_model_selection", value);
            }
            selection_save_events.push(std::move(event));
        });
    };

    auto schedule_selection_save = [&]() {
        const cli::Options* options = active_model_options();
        if (options == nullptr) return;
        pending_selection_save = provider::serialize_model_selection(
            provider::model_selection_from_options(*options));
        start_pending_selection_save();
    };

    auto process_selection_save_events = [&]() {
        EditorSelectionSaveEvent event;
        if (!selection_save_events.try_pop(event)) return false;
        selection_save_job.join();
        if (!event.error.ok()) {
            minibuffer_message(minibuffer, "Could not remember editor model selection: " + event.error.message);
        }
        start_pending_selection_save();
        return true;
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

    auto start_model_list = [&](bool context_only = false) {
        if (!editor_ai_has_provider(ai_continue)) {
            minibuffer_message(minibuffer, editor_no_provider_message());
            return;
        }
        if (model_list.job.running()) {
            minibuffer_message(minibuffer, "Model list is already loading");
            return;
        }
        model_list_context_only = context_only;
        model_list.start(ai_continue->request);
        minibuffer_message(minibuffer,
                           context_only ? "Loading model context..." : "Loading models...");
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

    auto open_reasoning_picker = [&]() {
        if (!editor_ai_ready(ai_continue)) {
            minibuffer_message(minibuffer,
                               editor_ai_has_provider(ai_continue)
                                   ? editor_no_model_message()
                                   : editor_no_provider_message());
            return;
        }
        const provider::RequestContext& request = ai_continue->request;
        config::ReasoningSelectorData data = config::reasoning_selector_data(
            request.options.model_catalog,
            request.profile.name,
            request.api_kind == provider::ApiKind::Responses ? "responses" : "chat",
            request.options.model);
        if (!data.guidance.empty()) {
            minibuffer_message(minibuffer, data.guidance);
            return;
        }
        std::vector<std::string> values;
        size_t current = 0;
        for (size_t i = 0; i < data.values.size(); ++i) {
            values.push_back(config::reasoning_selection_value(data.values[i]));
            if (data.values[i] == request.options.reasoning) current = i;
        }
        if (help_view.active) exit_help_view();
        picker.open_reasoning(std::move(values), std::move(data.labels), current);
        buffer_list_active = false;
        pending_close_confirm = false;
        pending_close_index = static_cast<size_t>(-1);
        minibuffer_message(minibuffer, picker.status_message());
    };

    auto commit_reasoning_selection = [&](const std::string& value) {
        if (!editor_ai_ready(ai_continue)) {
            minibuffer_message(minibuffer,
                               editor_ai_has_provider(ai_continue)
                                   ? editor_no_model_message()
                                   : editor_no_provider_message());
            return;
        }
        Error err = chat::apply_chat_setting(ai_continue->request.options, "reasoning", value);
        if (!err.ok()) {
            minibuffer_message(minibuffer, err.message);
            return;
        }
        std::string message = "Reasoning set to " +
            config::reasoning_selection_value(ai_continue->request.options.reasoning);
        const std::string advisory =
            provider::reasoning_temperature_advisory(ai_continue->request);
        if (!advisory.empty()) message += ". Warning: " + advisory;
        minibuffer_message(minibuffer, message);
        schedule_selection_save();
        refresh_settings_view();
    };

    auto cycle_reasoning = [&]() {
        if (assist_session.active) {
            minibuffer_message(minibuffer, "Cannot change reasoning while AI assist is running");
            return;
        }
        if (!editor_ai_ready(ai_continue)) {
            minibuffer_message(minibuffer,
                               editor_ai_has_provider(ai_continue)
                                   ? editor_no_model_message()
                                   : editor_no_provider_message());
            return;
        }
        provider::RequestContext& request = ai_continue->request;
        ReasoningSelection next;
        if (!config::next_reasoning_selection(
                request.options.model_catalog,
                request.profile.name,
                request.api_kind == provider::ApiKind::Responses ? "responses" : "chat",
                request.options.model,
                request.options.reasoning,
                next)) {
            minibuffer_message(
                minibuffer,
                "No reasoning options for this model (add models.conf entry or use /reasoning)");
            return;
        }
        commit_reasoning_selection(config::reasoning_selection_value(next));
    };

    auto toggle_thinking_traces = [&]() {
        if (!ai_continue.has_value()) return;
        cli::Options& options = ai_continue->request.options;
        options.show_thinking_traces = !options.show_thinking_traces;
        options.has_show_thinking_traces = true;
        minibuffer_message(minibuffer,
                           options.show_thinking_traces
                               ? "Thinking traces shown"
                               : "Thinking traces hidden");
    };

    auto apply_reasoning_selection = [&](const std::string& value) {
        if (!editor_ai_ready(ai_continue)) {
            minibuffer_message(minibuffer,
                               editor_ai_has_provider(ai_continue)
                                   ? editor_no_model_message()
                                   : editor_no_provider_message());
            return;
        }
        ReasoningSelection selection;
        Error parse_error = config::parse_reasoning_selection(value, selection);
        if (!parse_error.ok()) {
            minibuffer_message(minibuffer, parse_error.message);
            return;
        }
        const provider::RequestContext& request = ai_continue->request;
        parse_error = config::resolve_reasoning_off(
            request.options.model_catalog,
            request.profile.name,
            request.api_kind == provider::ApiKind::Responses ? "responses" : "chat",
            request.options.model,
            selection);
        if (!parse_error.ok()) {
            minibuffer_message(minibuffer, parse_error.message);
            return;
        }
        const std::string warning = config::reasoning_catalog_warning(
            request.options.model_catalog,
            request.profile.name,
            request.api_kind == provider::ApiKind::Responses ? "responses" : "chat",
            request.options.model,
            selection);
        if (warning.empty()) {
            commit_reasoning_selection(value);
            return;
        }
        pending_reasoning = value;
        start_minibuffer(minibuffer,
                         MinibufferAction::ConfirmReasoning,
                         "Warning: '" + value +
                             "' is not listed in models.conf. Proceed? y/n: ");
    };

    auto editor_options_seed = [&]() -> const cli::Options* {
        // Session options already include the models.conf catalog from startup.
        // Seeding prevents a deferred /provider enable from rebuilding AI context
        // with an empty catalog (Ctrl+T: "No reasoning options for this model").
        if (interactive != nullptr) return &interactive->context.options;
        if (ai_continue.has_value()) return &ai_continue->request.options;
        return nullptr;
    };

    auto apply_provider_selection = [&](const std::string& target) {
        Error apply_error =
            apply_editor_provider_target(ai_continue, assist_config, target, editor_options_seed());
        if (!apply_error.ok()) {
            minibuffer_message(minibuffer, apply_error.message);
            return;
        }
        schedule_selection_save();
        if (editor_ai_has_provider(ai_continue)) {
            start_model_list();
        } else {
            refresh_ai_status();
        }
    };

    auto handle_provider_command = [&](const std::string& target) {
        if (target.empty()) {
            open_provider_picker();
            return;
        }
        apply_provider_selection(target);
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
        schedule_selection_save();
        if (!ai_continue->request.options.has_context_tokens) {
            start_model_list(true);
        }
    };

    auto handle_context_command = [&](const std::string& requested) {
        if (requested.empty()) {
            if (ai_continue.has_value() &&
                ai_continue->request.options.context_tokens > 0) {
                const cli::Options& options = ai_continue->request.options;
                minibuffer_message(
                    minibuffer,
                    "Context window: " + std::to_string(options.context_tokens) +
                        " tokens (" +
                        (options.has_context_tokens ? "override" : "from /v1/models") +
                        ")");
            } else {
                minibuffer_message(
                    minibuffer,
                    "Context window: unknown; usage will show tokens without a percentage");
            }
            return;
        }
        Error ensure_error =
            ensure_editor_ai_context(ai_continue, assist_config, editor_options_seed());
        if (!ensure_error.ok()) {
            minibuffer_message(minibuffer, ensure_error.message);
            return;
        }
        if (ascii_lower(requested) == "auto") {
            ai_continue->request.options.has_context_tokens = false;
            ai_continue->request.options.context_tokens = 0;
            schedule_selection_save();
            if (editor_ai_ready(ai_continue)) {
                start_model_list(true);
            } else {
                minibuffer_message(minibuffer, "Context window set to automatic");
            }
            return;
        }
        long long tokens = 0;
        const Error parse_error = cli::parse_context_tokens(requested, tokens);
        if (!parse_error.ok()) {
            minibuffer_message(
                minibuffer,
                "Usage: /context [auto|TOKENS] (examples: 64k, 131072, 1M)");
            return;
        }
        ai_continue->request.options.context_tokens = tokens;
        ai_continue->request.options.has_context_tokens = true;
        schedule_selection_save();
        minibuffer_message(
            minibuffer, "Context window set to " + std::to_string(tokens) + " tokens");
    };

    auto handle_picker_list_escape = [&]() {
        const std::string sequence = current_escape_was_decoded ? std::string{} : read_escape_suffix();
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
            picker.clear();
            apply_provider_selection(provider_name);
            return;
        }
        if (picker.for_reasoning) {
            const std::string value = picker.items[picker.selected];
            picker.clear();
            apply_reasoning_selection(value);
            return;
        }
        const std::string model_name = picker.items[picker.selected];
        picker.clear();
        Error apply_error = apply_editor_model(ai_continue, model_name);
        if (!apply_error.ok()) {
            minibuffer_message(minibuffer, apply_error.message);
            return;
        }
        schedule_selection_save();
        if (have_cached_editor_models &&
            !ai_continue->request.options.has_context_tokens) {
            provider::apply_context_window_from_models(
                ai_continue->request, cached_editor_models, model_name);
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
            [&](provider::ModelsResult models_result) {
                cached_editor_models = models_result;
                have_cached_editor_models = true;
                if (model_list_context_only) {
                    model_list_context_only = false;
                    provider::apply_context_window_from_models(
                        ai_continue->request, cached_editor_models,
                        ai_continue->request.options.model);
                    minibuffer_message(
                        minibuffer,
                        ai_continue->request.options.context_tokens > 0
                            ? "Model context: " +
                                  std::to_string(
                                      ai_continue->request.options.context_tokens) +
                                  " tokens"
                            : "Context window unavailable; showing tokens only");
                    return;
                }
                std::vector<std::string> models = models_result.model_ids;
                if (!ui::should_auto_select_only_model(models)) {
                    open_model_picker(std::move(models));
                    return;
                }
                const Error apply_error = apply_editor_model(ai_continue, models.front());
                if (!apply_error.ok()) {
                    minibuffer_message(minibuffer, apply_error.message);
                    return;
                }
                if (!ai_continue->request.options.has_context_tokens) {
                    provider::apply_context_window_from_models(
                        ai_continue->request, cached_editor_models, models.front());
                }
                schedule_selection_save();
                minibuffer_message(
                    minibuffer,
                    ui::model_status_message(ai_continue->request.options.model,
                                             "auto-selected"));
            },
            [&](const std::string& message) {
                model_list_context_only = false;
                minibuffer_message(minibuffer, message);
            });
    };

    auto handle_open_minibuffer_key = [&](unsigned char ch) -> bool {
        if (!minibuffer.active ||
            (minibuffer.action != MinibufferAction::LoadFile &&
             minibuffer.action != MinibufferAction::ConfirmLoad &&
             minibuffer.action != MinibufferAction::ConfirmAutosaveRecovery &&
             minibuffer.action != MinibufferAction::ConfirmReasoning)) {
            return false;
        }
        if (minibuffer.action == MinibufferAction::ConfirmReasoning) {
            if (ch == 'y' || ch == 'Y') {
                const std::string reasoning = pending_reasoning;
                pending_reasoning.clear();
                commit_reasoning_selection(reasoning);
            } else if (ch == 'n' || ch == 'N' || ch == 27) {
                pending_reasoning.clear();
                minibuffer_message(minibuffer, "Reasoning change cancelled");
            } else {
                minibuffer.prompt = ui::kConfirmationRetryPrompt;
                minibuffer.input.clear();
            }
            return true;
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
        const std::string completion_status = format_generated_tokens_message(event.chat);
        last_generated_tokens_message = completion_status;
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
            save_focused_pane_view();
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
                    // New sibling inherits source view; replace with empty-buffer point.
                    split_layout.set_focused_view(pane_view_from_state(state));
                } else {
                    // Fall back to a plain new buffer when the window is too small.
                    split_layout.set_focused_buffer(new_buffer_index);
                    split_layout.set_focused_view(pane_view_from_state(state));
                    minibuffer_message(
                        minibuffer,
                        "Window too small to split; opened a new buffer instead");
                }
            } else {
                split_layout.set_focused_buffer(new_buffer_index);
                split_layout.set_focused_view(pane_view_from_state(state));
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

    auto collect_shell_secrets = [&]() {
        std::vector<std::string> secrets;
        auto take_context = [&](const provider::RequestContext* ctx) {
            if (ctx == nullptr) return;
            if (!ctx->api_key.empty()) secrets.push_back(ctx->api_key);
            if (!ctx->options.key.empty()) secrets.push_back(ctx->options.key);
            for (const std::string& header : ctx->headers) {
                const std::size_t colon = header.find(':');
                if (colon == std::string::npos) continue;
                if (is_sensitive_header_name(ascii_trim(header.substr(0, colon)))) {
                    const std::string value = ascii_trim(header.substr(colon + 1));
                    if (!value.empty()) secrets.push_back(value);
                }
            }
        };
        if (interactive != nullptr) take_context(&interactive->context);
        if (ai_continue.has_value()) take_context(&ai_continue->request);
        std::sort(secrets.begin(), secrets.end());
        secrets.erase(std::unique(secrets.begin(), secrets.end()), secrets.end());
        return secrets;
    };

    auto shell_timeout_ms = [&]() -> long {
        long seconds = 0;
        if (interactive != nullptr && interactive->context.options.timeout_seconds > 0) {
            seconds = interactive->context.options.timeout_seconds;
        } else if (ai_continue.has_value() &&
                   ai_continue->request.options.timeout_seconds > 0) {
            seconds = ai_continue->request.options.timeout_seconds;
        }
        return seconds > 0 ? seconds * 1000L : 60000L;
    };

    auto open_shell_output_buffer = [&](const std::string& content) {
        if (assist_session.active) {
            return false;
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
        next.text = PieceTable::from_string(content);
        next.invalidate_word_index();
        next.path.clear();
        next.redetect_language();
        next.highlight_enabled = highlight_enabled;
        next.dirty = !content.empty();
        next.clear_selection();
        next.clear_undo_history();
        next.cursor = 0;
        buffers.push_back(next);
        active_buffer = buffers.size() - 1;
        state = next;
        split_layout.set_focused_buffer(active_buffer);
        split_layout.set_focused_view(pane_view_from_state(state));
        buffer_list_active = false;
        buffer_list_selected = active_buffer;
        pending_close_confirm = false;
        pending_close_index = static_cast<size_t>(-1);
        return true;
    };

    auto start_editor_shell = [&](const std::string& command) {
        if (shell_session.active) {
            minibuffer_message(minibuffer, "A shell command is already running");
            return;
        }
        if (insert_session.active) {
            minibuffer_message(minibuffer, "Wait for insertion or press Esc to cancel it");
            return;
        }
        if (reformat_session.active) {
            minibuffer_message(minibuffer, "Wait for reformatting or press Esc to cancel it");
            return;
        }
        if (assist_session.active) {
            minibuffer_message(minibuffer, "Finish or cancel AI assist before running shell");
            return;
        }
        if (command.empty()) {
            minibuffer_message(minibuffer,
                               "Usage: shell COMMAND  or  !COMMAND  "
                               "(optional / prefix; shell-stdout / !! also ok)");
            return;
        }
        shell_session.active = true;
        shell_session.cancel_requested = false;
        shell_session.command = command;
        app::UserShellOptions options;
        options.timeout_ms = shell_timeout_ms();
        const std::vector<std::string> secrets = collect_shell_secrets();
        runtime::EventQueue<ShellEvent>& event_queue = shell_session.events;
        shell_session.job.start([command, options, secrets, &event_queue](
                                    runtime::CancellationToken token) mutable {
            ShellEvent event;
            options.cancellation = token;
            app::UserShellResult result;
            event.error = app::run_user_shell(command, options, result);
            event.duration_ms = result.duration_ms;
            event.exit_status = result.exit_status;
            event.failed = app::user_shell_failed(event.error, result);
            event.buffer_text = app::format_user_shell_draft_stdout(result, secrets);
            if (event.failed) {
                event.status_line = app::format_user_shell_draft_status(event.error, result, secrets);
                if (result.duration_ms > 0 &&
                    event.status_line.find("ms") == std::string::npos) {
                    event.status_line += " · " + std::to_string(result.duration_ms) + "ms";
                }
            } else {
                event.status_line = "Shell ok · exit 0 · " + std::to_string(result.duration_ms) +
                                    "ms · " + std::to_string(event.buffer_text.size()) + " bytes";
                if (result.stdout_truncated) event.status_line += " · truncated";
                if (event.buffer_text.empty()) event.status_line += " · empty stdout";
            }
            event_queue.push(std::move(event));
        });
        minibuffer_message(minibuffer, "Running shell: " + command + " · Esc to cancel");
    };

    auto process_shell_events = [&]() -> bool {
        ShellEvent event;
        if (!shell_session.events.try_pop(event)) {
            return false;
        }
        shell_session.job.join();
        const bool was_cancelled = shell_session.cancel_requested;
        shell_session.active = false;
        if (was_cancelled || event.error.code == ErrorCode::Cancelled) {
            if (!event.buffer_text.empty()) {
                (void)open_shell_output_buffer(event.buffer_text);
            }
            minibuffer_message(minibuffer,
                               event.status_line.empty() ? "Shell cancelled" : event.status_line);
            return true;
        }
        if (!open_shell_output_buffer(event.buffer_text)) {
            minibuffer_message(minibuffer,
                               event.status_line.empty() ? "Shell finished" : event.status_line);
            return true;
        }
        minibuffer_message(minibuffer,
                           event.status_line.empty()
                               ? (event.failed ? "Shell failed" : "Shell ok")
                               : event.status_line);
        return true;
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
        save_focused_pane_view();
        const Rect area = editor_main_area();
        if (split_layout.split_focused(SplitKind::Vertical, area)) {
            // Both new leaves inherited the view; keep live state aligned with focus.
            apply_pane_view_to_state(state, split_layout.focused_view());
            minibuffer_message(minibuffer, "Vertical split (Ctrl+X o other pane)");
        } else {
            minibuffer_message(minibuffer, "Window too small for vertical split");
        }
    };

    auto apply_hsplit = [&]() {
        save_focused_pane_view();
        const Rect area = editor_main_area();
        if (split_layout.split_focused(SplitKind::Horizontal, area)) {
            apply_pane_view_to_state(state, split_layout.focused_view());
            minibuffer_message(minibuffer, "Horizontal split (Ctrl+X o other pane)");
        } else {
            minibuffer_message(minibuffer, "Window too small for horizontal split");
        }
    };

    auto apply_closesplit = [&]() {
        save_focused_pane_view();
        if (!split_layout.close_focused()) {
            minibuffer_message(minibuffer, "Only one pane");
        } else {
            focus_buffer_from_split();
            minibuffer_message(minibuffer, "Closed pane");
        }
    };

    auto apply_maximize_split = [&]() {
        save_focused_pane_view();
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
        // Shell: parse raw input so !cmd is not rewritten to /!cmd.
        // Editor also accepts slashless shell / shell-stdout (like other commands).
        {
            std::string shell_line = trim_ascii_copy(minibuffer.input);
            if (!shell_line.empty() && shell_line.front() != '/' && shell_line.front() != '!') {
                // Longer token first so "shell-stdout" is not treated as "shell".
                if (shell_line == "shell-stdout" || shell_line.rfind("shell-stdout ", 0) == 0 ||
                    shell_line == "shell" || shell_line.rfind("shell ", 0) == 0) {
                    shell_line.insert(shell_line.begin(), '/');
                }
            }
            std::string shell_command;
            std::string shell_error;
            app::UserShellDestination shell_dest = app::UserShellDestination::Notice;
            if (app::parse_user_shell_invocation(shell_line, shell_command, shell_error,
                                                 shell_dest)) {
                pending_assist = PendingAssist{};
                exit_assist_command_mode(minibuffer, assist_completer);
                if (!shell_error.empty()) {
                    minibuffer_message(minibuffer,
                                       "Usage: shell COMMAND  or  !COMMAND  "
                                       "(optional / prefix; shell-stdout / !! also ok)");
                    return;
                }
                // Notice and Draft are identical in the editor: new buffer + minibuffer status.
                (void)shell_dest;
                start_editor_shell(shell_command);
                return;
            }
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
            case EditorSlashCommand::Agent:
                pending_assist = PendingAssist{};
                exit_assist_command_mode(minibuffer, assist_completer);
                request_switch_to_agent();
                return;
            case EditorSlashCommand::Editor:
                pending_assist = PendingAssist{};
                exit_assist_command_mode(minibuffer, assist_completer);
                minibuffer_message(minibuffer, "Already in editor mode");
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
            case EditorSlashCommand::GotoLine: {
                pending_assist = PendingAssist{};
                exit_assist_command_mode(minibuffer, assist_completer);
                if (slash.path.empty()) {
                    start_minibuffer(minibuffer, MinibufferAction::GotoLine, "Goto line: ");
                    return;
                }
                std::string message;
                (void)state.goto_line(slash.path, message);
                minibuffer_message(minibuffer, message);
                return;
            }
            case EditorSlashCommand::Dired:
                pending_assist = PendingAssist{};
                exit_assist_command_mode(minibuffer, assist_completer);
                enter_dired(slash.path);
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
                                              : trim_ascii_copy(command_line.substr(24));
            if (requested.empty()) {
                minibuffer_message(minibuffer,
                                   std::string("Auto-convert HTML to Markdown: ") +
                                       (insert_options.auto_convert_html_to_markdown ? "on" : "off"));
                return;
            }
            if (requested != "on" && requested != "off") {
                minibuffer_message(minibuffer,
                                   "Usage: /auto-convert-html-to-md on|off");
                return;
            }
            insert_options.auto_convert_html_to_markdown =
                requested == "on";
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
                                   (insert_options.auto_convert_html_to_markdown ? "on" : "off"));
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
        {
            auto try_text_align_command = [&](const char* name, TextAlignMode mode) -> bool {
                const std::string prefix = std::string("/") + name;
                if (command_line != prefix && command_line.rfind(prefix + " ", 0) != 0) {
                    return false;
                }
                pending_assist = PendingAssist{};
                exit_assist_command_mode(minibuffer, assist_completer);
                const std::string arg =
                    command_line.size() <= prefix.size()
                        ? std::string()
                        : trim_ascii_copy(command_line.substr(prefix.size()));
                auto apply_width = [&](size_t width) {
                    Error writable = state.mutation_allowed();
                    if (!writable.ok()) {
                        minibuffer_message(minibuffer, writable.message);
                        return;
                    }
                    size_t start = 0;
                    size_t end = 0;
                    bool had_selection = false;
                    text_layout_scope(state, start, end, had_selection);
                    const std::string scope = state.text.range_text(start, end - start);
                    TextLayoutResult layout = reflow_align(scope, mode, width);
                    if (!layout.error.ok()) {
                        minibuffer_message(minibuffer, layout.error.message);
                        return;
                    }
                    Error apply_error =
                        apply_text_layout_result(state, start, end, had_selection, layout);
                    if (!apply_error.ok()) {
                        minibuffer_message(minibuffer, apply_error.message);
                        return;
                    }
                    if (!layout.changed) {
                        minibuffer_message(minibuffer, "No changes");
                        return;
                    }
                    minibuffer_message(minibuffer,
                                       std::string(text_align_mode_name(mode)) + " to " +
                                           std::to_string(width) + " columns");
                };
                if (arg.empty()) {
                    start_minibuffer(
                        minibuffer,
                        MinibufferAction::TextAlignWidth,
                        "Enter width for the text-alignment (" +
                            std::to_string(text_align_width_default) + " default): ",
                        std::to_string(text_align_width_default));
                    minibuffer.text_align_mode = static_cast<int>(mode);
                    minibuffer.text_align_default_width = text_align_width_default;
                    return true;
                }
                size_t width = 0;
                const char* begin = arg.data();
                const char* endp = begin + arg.size();
                const std::from_chars_result parsed = std::from_chars(begin, endp, width);
                if (parsed.ec != std::errc{} || parsed.ptr != endp ||
                    !valid_text_align_width(width)) {
                    minibuffer_message(
                        minibuffer,
                        std::string("Usage: /") + name +
                            " [WIDTH]  (WIDTH > 20 and ≤ 1000; omit for default)");
                    return true;
                }
                apply_width(width);
                return true;
            };
            if (try_text_align_command("left-align", TextAlignMode::Left) ||
                try_text_align_command("right-align", TextAlignMode::Right) ||
                try_text_align_command("center-align", TextAlignMode::Center) ||
                try_text_align_command("justify", TextAlignMode::Justify)) {
                return;
            }
        }
        auto run_line_cleanup = [&](const char* command_name,
                                    TextLayoutResult (*fn)(const std::string&)) -> bool {
            if (command_line != std::string("/") + command_name &&
                command_line.rfind(std::string("/") + command_name + " ", 0) != 0) {
                return false;
            }
            pending_assist = PendingAssist{};
            exit_assist_command_mode(minibuffer, assist_completer);
            if (command_line.rfind(std::string("/") + command_name + " ", 0) == 0) {
                minibuffer_message(minibuffer, std::string("Usage: /") + command_name);
                return true;
            }
            Error writable = state.mutation_allowed();
            if (!writable.ok()) {
                minibuffer_message(minibuffer, writable.message);
                return true;
            }
            size_t start = 0;
            size_t end = 0;
            bool had_selection = false;
            text_layout_scope(state, start, end, had_selection);
            const std::string scope = state.text.range_text(start, end - start);
            TextLayoutResult layout = fn(scope);
            if (!layout.error.ok()) {
                minibuffer_message(minibuffer, layout.error.message);
                return true;
            }
            Error apply_error =
                apply_text_layout_result(state, start, end, had_selection, layout);
            if (!apply_error.ok()) {
                minibuffer_message(minibuffer, apply_error.message);
                return true;
            }
            if (!layout.changed) {
                minibuffer_message(minibuffer, "No changes");
                return true;
            }
            minibuffer_message(minibuffer, std::string(command_name) + " applied");
            return true;
        };
        if (run_line_cleanup("remove-blank-lines", remove_blank_lines) ||
            run_line_cleanup("remove-duplicate-blank-lines", remove_duplicate_blank_lines) ||
            run_line_cleanup("remove-duplicate-lines", remove_duplicate_lines)) {
            return;
        }
        if (command_line == "/alignment-width" ||
            command_line.rfind("/alignment-width ", 0) == 0) {
            pending_assist = PendingAssist{};
            exit_assist_command_mode(minibuffer, assist_completer);
            const std::string requested =
                command_line.size() <= 16 ? "" : trim_ascii_copy(command_line.substr(16));
            if (requested.empty()) {
                minibuffer_message(minibuffer,
                                   "Alignment width default: " +
                                       std::to_string(text_align_width_default));
                return;
            }
            size_t width = 0;
            const char* begin = requested.data();
            const char* endp = begin + requested.size();
            const std::from_chars_result parsed = std::from_chars(begin, endp, width);
            if (parsed.ec != std::errc{} || parsed.ptr != endp ||
                !valid_text_align_width(width)) {
                minibuffer_message(minibuffer,
                                   "Usage: /alignment-width [WIDTH]  (WIDTH > 20 and ≤ 1000)");
                return;
            }
            text_align_width_default = width;
            minibuffer_message(minibuffer,
                               "Alignment width default: " +
                                   std::to_string(text_align_width_default));
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
                                              : trim_ascii_copy(command_line.substr(10));
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
                tui::handle_theme_command(*settings.themes, theme_name, requested, use_colors);
            minibuffer_message(minibuffer, theme_result.message);
            if (theme_result.ok && !theme_result.selected_theme.empty()) {
                theme_name = theme_result.selected_theme;
            }
            if (theme_result.ok) use_colors = theme_result.colors_enabled;
            return;
        }
        if (command_line == "/scrollbar" ||
            command_line.rfind("/scrollbar ", 0) == 0) {
            pending_assist = PendingAssist{};
            exit_assist_command_mode(minibuffer, assist_completer);
            const std::string requested = command_line.size() <= 10
                                              ? ""
                                              : trim_ascii_copy(command_line.substr(10));
            const ui::ScrollbarVisibilityResult result =
                ui::handle_scrollbar_visibility(requested, show_scrollbars);
            if (result.ok) {
                show_scrollbars = result.visible;
            }
            if (result.ok && interactive != nullptr) {
                interactive->show_scrollbars = show_scrollbars;
            }
            minibuffer_message(minibuffer, result.message);
            return;
        }
        if (command_line == "/thinking" || command_line.rfind("/thinking ", 0) == 0) {
            pending_assist = PendingAssist{};
            exit_assist_command_mode(minibuffer, assist_completer);
            cli::Options* options = mutable_model_options();
            if (options == nullptr) {
                minibuffer_message(minibuffer, "Thinking trace settings are unavailable");
                return;
            }
            const std::string requested = command_line.size() <= 9
                                              ? ""
                                              : trim_ascii_copy(command_line.substr(9));
            if (requested.empty()) {
                minibuffer_message(minibuffer,
                                   std::string("Thinking traces: ") +
                                       (options->show_thinking_traces ? "show" : "hide"));
                return;
            }
            if (requested != "show" && requested != "hide") {
                minibuffer_message(minibuffer, "Usage: /thinking show|hide");
                return;
            }
            options->show_thinking_traces = requested == "show";
            options->has_show_thinking_traces = true;
            refresh_settings_view();
            minibuffer_message(minibuffer,
                               options->show_thinking_traces
                                   ? "Thinking traces shown"
                                   : "Thinking traces hidden");
            return;
        }
        if (command_line == "/setting" || command_line.rfind("/setting ", 0) == 0) {
            pending_assist = PendingAssist{};
            exit_assist_command_mode(minibuffer, assist_completer);
            cli::Options* options = mutable_model_options();
            if (options == nullptr) {
                minibuffer_message(minibuffer, "Settings are unavailable");
                return;
            }
            const std::string requested = command_line.size() <= 8
                                              ? ""
                                              : trim_ascii_copy(command_line.substr(8));
            if (requested.empty()) {
                enter_settings_view(chat::format_settings_panel(
                    *options,
                    ai_continue.has_value()
                        ? provider::reasoning_temperature_advisory(ai_continue->request)
                        : std::string{}));
                return;
            }
            if (chat::generation::is_chat_purpose(requested)) {
                if (options->model.empty()) {
                    minibuffer_message(minibuffer,
                                       "Set a model with /model before applying a purpose preset");
                    return;
                }
                const ModelCapability* capability = config::resolve_model_capability(
                    options->model_catalog, options->provider, options->api, options->model);
                const ModelSetting* preset = capability == nullptr
                    ? nullptr
                    : config::find_model_preset(options->model_catalog, *capability, requested);
                if (preset == nullptr) {
                    minibuffer_message(minibuffer,
                                       "No [preset] in models.conf for model " + options->model +
                                           " purpose " + requested);
                    return;
                }
                Error err = chat::apply_model_setting_preset(*options, *preset, capability);
                if (!err.ok()) {
                    minibuffer_message(minibuffer, err.message);
                    return;
                }
                minibuffer_message(minibuffer,
                                   "Applied " + requested + " settings for " + options->model);
                schedule_selection_save();
                refresh_settings_view();
                return;
            }
            const size_t equals = requested.find('=');
            if (equals == std::string::npos) {
                minibuffer_message(minibuffer,
                                   "Usage: /setting NAME=VALUE or /setting general|coding|instruct|creative");
                return;
            }
            const std::string name = trim_ascii_copy(requested.substr(0, equals));
            const std::string value = trim_ascii_copy(requested.substr(equals + 1));
            const std::string lower_name = ascii_lower(name);
            if (lower_name == "thinking_preview_max_chars" || lower_name == "cmd-out") {
                minibuffer_message(minibuffer,
                                   name + " is available only in interactive agent mode");
                return;
            }
            if (lower_name == "reasoning") {
                apply_reasoning_selection(value);
                return;
            }
            Error err = chat::apply_chat_setting(*options, name, value);
            if (!err.ok()) {
                minibuffer_message(minibuffer, err.message);
                return;
            }
            insert_options.auto_convert_html_to_markdown =
                options->auto_convert_html_to_markdown;
            highlight_enabled = options->tui_highlight;
            state.highlight_enabled = highlight_enabled;
            if (interactive != nullptr) interactive->highlight_enabled = highlight_enabled;
            refresh_settings_view();
            minibuffer_message(minibuffer, "Updated " + name);
            return;
        }
        if (command_line == "/provider" || command_line.rfind("/provider ", 0) == 0) {
            pending_assist = PendingAssist{};
            exit_assist_command_mode(minibuffer, assist_completer);
            handle_provider_command(command_line.size() <= 9 ? "" : trim_ascii_copy(command_line.substr(9)));
            return;
        }
        if (command_line == "/reasoning" || command_line.rfind("/reasoning ", 0) == 0) {
            pending_assist = PendingAssist{};
            exit_assist_command_mode(minibuffer, assist_completer);
            const std::string requested = command_line.size() <= 10
                                              ? ""
                                              : trim_ascii_copy(command_line.substr(10));
            if (requested.empty()) open_reasoning_picker();
            else apply_reasoning_selection(requested);
            return;
        }
        if (command_line == "/model" || command_line.rfind("/model ", 0) == 0) {
            pending_assist = PendingAssist{};
            exit_assist_command_mode(minibuffer, assist_completer);
            handle_model_command(command_line.size() <= 6 ? "" : trim_ascii_copy(command_line.substr(6)));
            return;
        }
        if (command_line == "/context" || command_line.rfind("/context ", 0) == 0) {
            pending_assist = PendingAssist{};
            exit_assist_command_mode(minibuffer, assist_completer);
            handle_context_command(
                command_line.size() <= 8 ? "" : trim_ascii_copy(command_line.substr(8)));
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

    ClipboardRuntime clipboard_runtime;
    const ClipboardEnvironment clipboard_environment = current_clipboard_environment();
    PendingClipboardPaste pending_clipboard;
    auto cancel_pending_clipboard = [&]() {
        clipboard_runtime.cancel_read();
        cancel_terminal_clipboard_request();
        pending_clipboard = PendingClipboardPaste{};
    };
    auto publish_internal_clipboard = [&]() {
        const std::string text = shared_clipboard().text();
        if (text.empty()) return;
        publish_terminal_clipboard(text);
        (void)clipboard_runtime.start_write(clipboard_environment, text);
    };
    auto clipboard_target_unchanged = [&]() {
        if (!pending_clipboard.active) return false;
        if (pending_clipboard.minibuffer) {
            return minibuffer.active &&
                   minibuffer.action == pending_clipboard.minibuffer_action &&
                   minibuffer.input == pending_clipboard.minibuffer_input;
        }
        return !minibuffer.active && !help_view.active && !picker.active &&
               !buffer_list_active && state.buffer_id() == pending_clipboard.buffer_id &&
               state.revision() == pending_clipboard.revision &&
               state.cursor == pending_clipboard.cursor &&
               state.selection.anchor == pending_clipboard.selection.anchor &&
               state.selection.active == pending_clipboard.selection.active;
    };
    auto apply_external_clipboard = [&](const std::string& text, const std::string& backend) {
        if (!clipboard_target_unchanged()) {
            if (pending_clipboard.minibuffer && minibuffer.active)
                minibuffer.message =
                    "Clipboard target changed; press Ctrl+V to paste again";
            else
                minibuffer_message(
                    minibuffer,
                    "Clipboard target changed; press Ctrl+V to paste again");
            pending_clipboard = PendingClipboardPaste{};
            return;
        }
        Error paste_error;
        if (pending_clipboard.minibuffer) {
            paste_error = paste_into_minibuffer(minibuffer, text);
        } else {
            Clipboard external;
            external.set(text);
            paste_error = state.paste(external);
        }
        const std::string message =
            paste_error.ok() ? "Pasted from " + backend : paste_error.message;
        if (pending_clipboard.minibuffer && minibuffer.active)
            minibuffer.message = message;
        else
            minibuffer_message(minibuffer, message);
        pending_clipboard = PendingClipboardPaste{};
    };
    auto begin_external_clipboard_paste = [&]() {
        cancel_pending_clipboard();
        if (minibuffer.active) {
            const Error accepted = paste_into_minibuffer(minibuffer, "");
            if (!accepted.ok()) {
                minibuffer.message = accepted.message;
                return;
            }
        }
        pending_clipboard.active = true;
        pending_clipboard.minibuffer = minibuffer.active;
        pending_clipboard.buffer_id = state.buffer_id();
        pending_clipboard.revision = state.revision();
        pending_clipboard.cursor = state.cursor;
        pending_clipboard.selection = state.selection;
        pending_clipboard.minibuffer_action = minibuffer.action;
        pending_clipboard.minibuffer_input = minibuffer.input;
        ClipboardCommand native_command;
        const bool native_available =
            resolve_clipboard_command(clipboard_environment, false, native_command);
        if (prefer_terminal_clipboard_query(clipboard_environment) || !native_available) {
            pending_clipboard.terminal_query = true;
            pending_clipboard.query_deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(2);
            request_terminal_clipboard();
        } else {
            pending_clipboard.generation =
                clipboard_runtime.start_read(clipboard_environment);
        }
        if (pending_clipboard.minibuffer)
            minibuffer.message = "Reading system clipboard...";
        else
            minibuffer_message(minibuffer, "Reading system clipboard...");
    };
    auto process_clipboard_events = [&]() {
        bool updated = false;
        ClipboardRuntimeEvent clipboard_event;
        while (clipboard_runtime.try_pop(clipboard_event)) {
            updated = true;
            if (clipboard_event.type == ClipboardRuntimeEventType::WriteFinished) {
                continue;
            }
            if (!pending_clipboard.active ||
                clipboard_event.generation != pending_clipboard.generation) {
                continue;
            }
            if (clipboard_event.result.ok()) {
                apply_external_clipboard(clipboard_event.result.text,
                                         clipboard_event.result.backend);
            } else {
                const std::string message =
                    clipboard_failure_help(clipboard_environment,
                                           clipboard_event.result,
                                           true);
                if (pending_clipboard.minibuffer && minibuffer.active)
                    minibuffer.message = message;
                else
                    minibuffer_message(minibuffer, message);
                pending_clipboard = PendingClipboardPaste{};
            }
        }
        if (pending_clipboard.active && pending_clipboard.terminal_query &&
            std::chrono::steady_clock::now() >= pending_clipboard.query_deadline) {
            cancel_terminal_clipboard_request();
            ClipboardCommand native_command;
            if (resolve_clipboard_command(clipboard_environment, false, native_command)) {
                pending_clipboard.terminal_query = false;
                pending_clipboard.generation =
                    clipboard_runtime.start_read(clipboard_environment);
            } else {
                if (pending_clipboard.minibuffer && minibuffer.active)
                    minibuffer.message =
                        "Terminal did not provide clipboard text; use the terminal paste shortcut";
                else
                    minibuffer_message(
                        minibuffer,
                        "Terminal did not provide clipboard text; use the terminal paste shortcut");
                pending_clipboard = PendingClipboardPaste{};
            }
            updated = true;
        }
        return updated;
    };

    std::function<void(unsigned char, bool)> handle_key;
    auto open_assist_command_minibuffer = [&]() {
        start_assist_command_mode(minibuffer, assist_completer);
        assist_escape_repeat_guard.arm(EscapeRepeatGuard::Clock::now());
    };
    handle_key = [&](unsigned char ch, bool decoded_escape) {
        current_escape_was_decoded = decoded_escape;
        mouse_view_detached = false;
        if (ch != 27) assist_escape_repeat_guard.reset();
        if (ch != '\t') {
            word_completer.reset();
        }
        // Background agent Guard Ask (worker blocked); answer without leaving editor.
        // Ctrl+G must NOT deny the Guard — leave the Ask pending for agent TUI.
        if (pending_agent_guard) {
            if (ch == 7) {
                // Cycle back to agent with Guard still pending (do not resolve).
                pending_agent_guard = false;
                agent_guard_request = {};
                if (dired.active) {
                    dired_close(dired);
                    dired_overwrite_is_rename = false;
                }
                request_editor_toggle();
                return;
            }
            switch (ui::parse_confirmation_key(ch)) {
                case ui::ConfirmationKeyResult::Accepted:
                    if (agent_controller() && agent_controller()->approval_gate()) {
                        agent_controller()->approval_gate()->resolve(
                            agent::GuardApprovalDecision::Allow);
                    }
                    pending_agent_guard = false;
                    agent_guard_request = {};
                    minibuffer_message(minibuffer, "Guard: allowed · agent continues");
                    return;
                case ui::ConfirmationKeyResult::Rejected:
                    if (agent_controller() && agent_controller()->approval_gate()) {
                        agent_controller()->approval_gate()->resolve(
                            agent::GuardApprovalDecision::Deny);
                    }
                    pending_agent_guard = false;
                    agent_guard_request = {};
                    minibuffer_message(minibuffer, "Guard: denied");
                    return;
                case ui::ConfirmationKeyResult::Pending:
                    minibuffer_message(minibuffer, ui::kConfirmationRetryPrompt);
                    return;
            }
        }
        if (pending_agent_quit_confirm) {
            switch (ui::parse_confirmation_key(ch)) {
                case ui::ConfirmationKeyResult::Accepted:
                    if (agent_controller()) {
                        agent_controller()->cancel_turn();
                        agent_controller()->set_status_label("Agent cancelled");
                    }
                    pending_agent_quit_confirm = false;
                    quit = true;
                    return;
                case ui::ConfirmationKeyResult::Rejected:
                    pending_agent_quit_confirm = false;
                    minibuffer_message(minibuffer, "Quit cancelled · agent still running");
                    return;
                case ui::ConfirmationKeyResult::Pending:
                    minibuffer_message(minibuffer, ui::kConfirmationRetryPrompt);
                    return;
            }
        }
        if (ch == editor_key_toggle_thinking_traces()) {
            toggle_thinking_traces();
            return;
        }
        if (ch == 20) {
            cycle_reasoning();
            return;
        }
        if (is_editor_command_minibuffer_key(ch)) {
            const bool command_unavailable =
                minibuffer.active || replace.active || assist_session.active ||
                reformat_session.active || insert_session.active || shell_session.active ||
                picker.active || buffer_list_active || dired.active || pending_close_confirm ||
                window_prefix_active;
            if (!command_unavailable) open_assist_command_minibuffer();
            return;
        }
        if (minibuffer.active && ch == 22) {
            if (shared_clipboard().empty()) {
                begin_external_clipboard_paste();
            } else {
                cancel_pending_clipboard();
                const Error paste_error =
                    paste_into_minibuffer(minibuffer, shared_clipboard().text());
                if (paste_error.ok()) {
                    assist_completer = AssistCompleterState{};
                    minibuffer_path_completer.reset();
                    minibuffer.message = "Pasted into minibuffer";
                } else {
                    minibuffer.message = paste_error.message;
                }
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
                    if (assist_escape_repeat_guard.suppress(EscapeRepeatGuard::Clock::now())) return;
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
                const std::string sequence = decoded_escape ? std::string{} : read_escape_suffix();
                if (!sequence.empty()) {
                    unsigned char decoded = 0;
                    if (decode_control_key_sequence(sequence, decoded)) {
                        handle_key(decoded, decoded == 27);
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
                open_assist_command_minibuffer();
                return;
            }
            if (ch == 8) {
                // Ctrl+H toggles help closed while the help view is active.
                exit_help_view();
                return;
            }
            if (ch == 6) {
                start_minibuffer(minibuffer, MinibufferAction::Search, "Search: ", last_search);
                return;
            }
            return;
        }
        if (dired.active) {
            auto dired_page_rows = [&]() {
                const TerminalSize size = terminal_size();
                return std::max(1, size.rows - 4);
            };
            auto finish_dired_prompt = [&](const std::string& message) {
                minibuffer_path_completer.reset();
                minibuffer_message(minibuffer, message);
            };
            if (minibuffer.active) {
                if (ch == 27) {
                    dired_overwrite_is_rename = false;
                    finish_dired_prompt("Cancelled");
                    return;
                }
                if (minibuffer.action == MinibufferAction::DiredConfirmDelete) {
                    if (ch == 'y' || ch == 'Y') {
                        Error err = dired_delete_selected(dired, true);
                        finish_dired_prompt(err.ok() ? "Deleted" : err.message);
                    } else if (ch == 'n' || ch == 'N') {
                        finish_dired_prompt("Delete cancelled");
                    } else {
                        minibuffer.prompt = "Delete? (y/n) ";
                        minibuffer.input.clear();
                    }
                    return;
                }
                if (minibuffer.action == MinibufferAction::DiredConfirmOverwrite) {
                    if (ch == 'y' || ch == 'Y') {
                        const bool was_rename = dired_overwrite_is_rename;
                        Error err = was_rename
                                        ? dired_rename_selected(dired, pending_load_path, true)
                                        : dired_copy_selected(dired, pending_load_path, true);
                        dired_overwrite_is_rename = false;
                        pending_load_path.clear();
                        finish_dired_prompt(err.ok() ? (was_rename ? "Renamed" : "Copied")
                                                     : err.message);
                    } else if (ch == 'n' || ch == 'N') {
                        dired_overwrite_is_rename = false;
                        pending_load_path.clear();
                        finish_dired_prompt("Overwrite cancelled");
                    } else {
                        minibuffer.prompt = "Overwrite? (y/n) ";
                        minibuffer.input.clear();
                    }
                    return;
                }
                if (minibuffer.action == MinibufferAction::Search) {
                    if (ch == '\r' || ch == '\n') {
                        last_search = minibuffer.input;
                        dired.last_search = last_search;
                        if (dired.focus == DiredFocus::View) {
                            const bool found = dired.view.search_next(last_search);
                            finish_dired_prompt(found ? ("Found: " + last_search)
                                                      : ("Search not found: " + last_search));
                        } else {
                            finish_dired_prompt("Open a file with Enter before searching");
                        }
                        return;
                    }
                    if (ch == 127 || ch == 8) {
                        if (!minibuffer.input.empty()) minibuffer.input.pop_back();
                        return;
                    }
                    if (ch >= 0x20U) {
                        minibuffer.input.push_back(static_cast<char>(ch));
                        return;
                    }
                    return;
                }
                const bool path_prompt =
                    minibuffer.action == MinibufferAction::DiredPath ||
                    minibuffer.action == MinibufferAction::DiredRename ||
                    minibuffer.action == MinibufferAction::DiredCopy ||
                    minibuffer.action == MinibufferAction::DiredNewFile ||
                    minibuffer.action == MinibufferAction::DiredNewDir;
                if (path_prompt) {
                    if (ch == '\t') {
                        const PathCompletionResult result =
                            complete_path_input(minibuffer.input, minibuffer_path_completer);
                        minibuffer.message = path_completion_status(result);
                        return;
                    }
                    if (ch == 127 || ch == 8) {
                        if (!minibuffer.input.empty()) {
                            minibuffer.input.pop_back();
                            minibuffer_path_completer.reset();
                        }
                        return;
                    }
                    if (ch == '\r' || ch == '\n') {
                        const std::string value = trim_ascii_copy(minibuffer.input);
                        const MinibufferAction action = minibuffer.action;
                        if (action == MinibufferAction::DiredPath) {
                            Error err = dired_open(dired, value.empty() ? "." : value);
                            finish_dired_prompt(err.ok() ? dired_status_line(dired) : err.message);
                            return;
                        }
                        if (action == MinibufferAction::DiredRename) {
                            Error err = dired_rename_selected(dired, value, false);
                            if (!err.ok() && err.message.find("confirm overwrite") != std::string::npos) {
                                pending_load_path = value;
                                dired_overwrite_is_rename = true;
                                start_minibuffer(minibuffer,
                                                 MinibufferAction::DiredConfirmOverwrite,
                                                 "Overwrite existing path? (y/n) ");
                                return;
                            }
                            finish_dired_prompt(err.ok() ? "Renamed" : err.message);
                            return;
                        }
                        if (action == MinibufferAction::DiredCopy) {
                            Error err = dired_copy_selected(dired, value, false);
                            if (!err.ok() && err.message.find("confirm overwrite") != std::string::npos) {
                                pending_load_path = value;
                                dired_overwrite_is_rename = false;
                                start_minibuffer(minibuffer,
                                                 MinibufferAction::DiredConfirmOverwrite,
                                                 "Overwrite existing path? (y/n) ");
                                return;
                            }
                            finish_dired_prompt(err.ok() ? "Copied" : err.message);
                            return;
                        }
                        if (action == MinibufferAction::DiredNewFile) {
                            std::string created;
                            Error err = dired_create_file(dired, value, created);
                            if (!err.ok()) {
                                finish_dired_prompt(err.message);
                                return;
                            }
                            dired_close(dired);
                            dired_overwrite_is_rename = false;
                            request_open_buffer_from_path(created);
                            return;
                        }
                        if (action == MinibufferAction::DiredNewDir) {
                            Error err = dired_create_directory(dired, value);
                            finish_dired_prompt(err.ok() ? "Directory created" : err.message);
                            return;
                        }
                        return;
                    }
                    if (ch >= 0x20U) {
                        minibuffer.input.push_back(static_cast<char>(ch));
                        minibuffer_path_completer.reset();
                        return;
                    }
                    return;
                }
                // Unknown minibuffer while dired: cancel on Esc already handled.
                return;
            }

            if (ch == 17) {
                // Ctrl+Q is global quit: leave dired, then run the normal editor quit path.
                dired_close(dired);
                dired_overwrite_is_rename = false;
                handle_key(17, false);
                return;
            }
            if (ch == 7) {
                // Ctrl+G: cycle back to agent/chat even while dired is open
                // (q only demotes to the editor buffer list).
                dired_close(dired);
                dired_overwrite_is_rename = false;
                request_editor_toggle();
                return;
            }
            if (ch == 'q' || ch == 'Q') {
                exit_dired();
                return;
            }

            // Viewing a file: navigation applies to the RO buffer.
            if (dired.focus == DiredFocus::View) {
                if (ch == '\r' || ch == '\n') {
                    dired_close_view(dired);
                    minibuffer_message(minibuffer, "");
                    return;
                }
                if (ch == 'o' || ch == 'O') {
                    dired_open_for_edit();
                    return;
                }
                // n / p: next / previous changed-line block (history diff only).
                // No-op when the file has no tracked changes. List mode keeps
                // n=new-file and p=pass; those keys never reach list while in view.
                if (ch == 'n' || ch == 'N') {
                    if (dired_goto_next_changed_block(dired)) {
                        const size_t line =
                            dired.view.text.line_for_offset(dired.view.cursor) + 1;
                        minibuffer_message(minibuffer,
                                           "Next change · Ln " + std::to_string(line));
                    } else {
                        minibuffer_message(minibuffer, "No changed lines in this view");
                    }
                    return;
                }
                if (ch == 'p' || ch == 'P') {
                    if (dired_goto_prev_changed_block(dired)) {
                        const size_t line =
                            dired.view.text.line_for_offset(dired.view.cursor) + 1;
                        minibuffer_message(minibuffer,
                                           "Previous change · Ln " + std::to_string(line));
                    } else {
                        minibuffer_message(minibuffer, "No changed lines in this view");
                    }
                    return;
                }
                // f, /, Ctrl+F: find in the viewed file (less-style / also accepted).
                if (ch == 'f' || ch == 'F' || ch == '/' || ch == 6) {
                    start_minibuffer(minibuffer, MinibufferAction::Search, "Find: ",
                                     dired.last_search.empty() ? last_search : dired.last_search);
                    return;
                }
                // Space = PageDown, b = PageUp (less-style).
                if (ch == ' ' || ch == 'b' || ch == 'B') {
                    const Rect focus_rect = editor_content_rect(editor_main_area());
                    dired.view.apply_movement(ch == ' ' ? MovementKey::PageDown
                                                        : MovementKey::PageUp,
                                              focus_rect, false, false, false);
                    return;
                }
                if (ch == 27) {
                    const std::string sequence =
                        decoded_escape ? std::string{} : read_escape_suffix();
                    if (sequence.empty()) {
                        // Bare Esc: stay in view; status bar has path/position.
                        return;
                    }
                    if (is_dired_f4_sequence(sequence)) {
                        // Already in dired.
                        return;
                    }
                    if (sequence == "OR" || sequence == "[13~" || sequence == "[[C") {
                        const std::string needle =
                            dired.last_search.empty() ? last_search : dired.last_search;
                        if (needle.empty()) {
                            minibuffer_message(minibuffer,
                                               "No search string; press f or / to find");
                        } else {
                            const bool found = dired.view.search_next(needle);
                            minibuffer_message(minibuffer,
                                               found ? ("Found: " + needle)
                                                     : ("Search not found: " + needle));
                        }
                        return;
                    }
                    if (sequence == "[1;2R" || sequence == "O1;2R" || sequence == "[13;2~" ||
                        sequence == "[25~") {
                        const std::string needle =
                            dired.last_search.empty() ? last_search : dired.last_search;
                        if (needle.empty()) {
                            minibuffer_message(minibuffer,
                                               "No search string; press f or / to find");
                        } else {
                            const bool found = dired.view.search_previous(needle);
                            minibuffer_message(minibuffer,
                                               found ? ("Found: " + needle)
                                                     : ("Search not found: " + needle));
                        }
                        return;
                    }
                    std::string escape_status;
                    const Rect focus_rect = editor_content_rect(editor_main_area());
                    dispatch_escape_sequence(dired.view, sequence, escape_status,
                                             dired.last_search.empty() ? last_search
                                                                       : dired.last_search,
                                             &focus_rect);
                    // Keep minibuffer clear during RO browsing; status bar tracks position.
                    if (!escape_status.empty() &&
                        escape_status.find("not found") != std::string::npos) {
                        minibuffer_message(minibuffer, escape_status);
                    }
                    return;
                }
                return;
            }

            // List mode.
            if (dired.sort_pending) {
                if (ch == 'n') {
                    dired_set_sort(dired, DiredSortKey::Name, true);
                } else if (ch == 'N') {
                    dired_set_sort(dired, DiredSortKey::Name, false);
                } else if (ch == 's') {
                    dired_set_sort(dired, DiredSortKey::Size, true);
                } else if (ch == 'S') {
                    dired_set_sort(dired, DiredSortKey::Size, false);
                } else if (ch == 'd') {
                    dired_set_sort(dired, DiredSortKey::Date, true);
                } else if (ch == 'D') {
                    dired_set_sort(dired, DiredSortKey::Date, false);
                } else if (ch == 27) {
                    dired.sort_pending = false;
                    minibuffer_message(minibuffer, "Sort cancelled");
                    return;
                } else {
                    dired.sort_pending = false;
                    minibuffer_message(
                        minibuffer,
                        "Sort cancelled (use (n)ame (s)ize (d)ate or (N)ame (S)ize (D)ate)");
                    return;
                }
                minibuffer_message(minibuffer, "Sorted by " + dired_sort_label(dired.sort_key,
                                                                               dired.sort_ascending));
                return;
            }

            if (ch == 27) {
                const std::string sequence =
                    decoded_escape ? std::string{} : read_escape_suffix();
                if (sequence.empty()) {
                    minibuffer_message(minibuffer, "Dired — press q to leave");
                    return;
                }
                if (is_dired_f4_sequence(sequence)) {
                    return;
                }
                MovementKeyEvent movement;
                if (parse_movement_sequence(sequence, movement)) {
                    if (movement.key == MovementKey::Left) {
                        Error err = dired_go_parent(dired);
                        minibuffer_message(minibuffer,
                                           err.ok() ? dired_status_line(dired) : err.message);
                        return;
                    }
                    if (movement.key == MovementKey::Right) {
                        Error err = dired_go_deeper(dired);
                        minibuffer_message(minibuffer,
                                           err.ok() ? dired_status_line(dired) : err.message);
                        return;
                    }
                    dired_move_selection(dired, movement.key, dired_page_rows());
                    minibuffer_message(minibuffer, dired_status_line(dired));
                    return;
                }
                return;
            }
            if (ch == '\r' || ch == '\n') {
                Error err = dired_activate_selection(dired, settings);
                if (!err.ok()) {
                    minibuffer_message(minibuffer, err.message);
                } else if (dired.focus == DiredFocus::View) {
                    // Surface history-diff status so users see when marks exist
                    // (agent F4 path previously painted them without a cue).
                    if (dired.view_has_history_baseline) {
                        const size_t n =
                            ainiux::editor::count_changed_lines(dired.view_changed_lines);
                        minibuffer_message(minibuffer,
                                           n == 0 ? "History baseline · no line changes"
                                                  : ("History diff · " + std::to_string(n) +
                                                     " changed line(s) · gray bg"));
                    } else {
                        minibuffer_message(minibuffer, "");
                    }
                } else {
                    minibuffer_message(minibuffer, dired_status_line(dired));
                }
                return;
            }
            if (ch == 'o' || ch == 'O') {
                dired_open_for_edit();
                return;
            }
            if (ch == 'g' || ch == 'G') {
                Error err = dired_refresh(dired);
                minibuffer_message(minibuffer, err.ok() ? "Refreshed" : err.message);
                return;
            }
            if (ch == 'r' || ch == 'R') {
                const DiredEntry* entry = dired_selected_entry(dired);
                const std::string initial =
                    entry != nullptr && !entry->is_parent ? entry->name : std::string();
                start_minibuffer(minibuffer, MinibufferAction::DiredRename, "Rename to: ", initial,
                                 &minibuffer_path_completer);
                return;
            }
            if (ch == 'c' || ch == 'C') {
                const DiredEntry* entry = dired_selected_entry(dired);
                const std::string initial =
                    entry != nullptr && !entry->is_parent ? entry->name : std::string();
                start_minibuffer(minibuffer, MinibufferAction::DiredCopy, "Copy to: ", initial,
                                 &minibuffer_path_completer);
                return;
            }
            if (ch == 'd' || ch == 'D') {
                const DiredEntry* entry = dired_selected_entry(dired);
                if (entry == nullptr || entry->is_parent) {
                    minibuffer_message(minibuffer, "Nothing to delete");
                    return;
                }
                std::error_code ec;
                const bool nonempty_dir =
                    entry->is_directory &&
                    !std::filesystem::is_empty(entry->path, ec) && !ec;
                start_minibuffer(
                    minibuffer,
                    MinibufferAction::DiredConfirmDelete,
                    nonempty_dir ? ("Delete non-empty " + entry->name + "/ recursively? (y/n) ")
                                 : ("Delete " + entry->name + "? (y/n) "));
                return;
            }
            if (ch == 'n' || ch == 'N') {
                start_minibuffer(minibuffer, MinibufferAction::DiredNewFile, "New file: ", "",
                                 &minibuffer_path_completer);
                return;
            }
            if (ch == 'm' || ch == 'M') {
                start_minibuffer(minibuffer, MinibufferAction::DiredNewDir,
                                 "New directory (mkdir -p): ", "", &minibuffer_path_completer);
                return;
            }
            if (ch == 't' || ch == 'T') {
                Error err = dired_touch_selected(dired);
                minibuffer_message(minibuffer, err.ok() ? "Touched" : err.message);
                return;
            }
            if (ch == 'f' || ch == 'F' || ch == '/') {
                minibuffer_message(minibuffer,
                                   "Press Enter to view a file, then f or / to find");
                return;
            }
            // Space = PageDown, b = PageUp (less-style).
            if (ch == ' ' || ch == 'b' || ch == 'B') {
                dired_move_selection(dired, ch == ' ' ? MovementKey::PageDown : MovementKey::PageUp,
                                     dired_page_rows());
                minibuffer_message(minibuffer, dired_status_line(dired));
                return;
            }
            if (ch == 's' || ch == 'S') {
                dired.sort_pending = true;
                minibuffer_message(
                    minibuffer,
                    "Sort by: (n)ame (s)ize (d)ate asc, (N)ame (S)ize (D)ate desc");
                return;
            }
            // p: toggle selected file dirty ↔ reviewed (per-file pass). Prefer p
            // over * because * is awkward on many non-US keyboard layouts.
            if (ch == 'p' || ch == 'P') {
                const DiredEntry* before = dired_selected_entry(dired);
                const std::string name =
                    before != nullptr ? before->name : std::string();
                Error err = dired_toggle_pass_selected(dired);
                if (!err.ok()) {
                    minibuffer_message(minibuffer, err.message);
                    return;
                }
                const DiredEntry* after = dired_selected_entry(dired);
                const bool now_dirty = after != nullptr && after->dirty;
                minibuffer_message(minibuffer,
                                   (now_dirty ? "Marked dirty: " : "Marked reviewed: ") + name);
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
        if (shell_session.active && ch == 27) {
            shell_session.cancel_requested = true;
            shell_session.job.cancel();
            minibuffer_message(minibuffer, "Cancelling shell...");
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
            std::string search_status;
            if (picker.handle_search_sort_char(ch, search_status)) {
                picker.refresh_view();
                minibuffer_message(minibuffer, search_status);
                return;
            }
            if (ch == '\r' || ch == '\n') {
                confirm_picker_selection();
                return;
            }
            std::string jump_status;
            if (picker.handle_jump_char(ch, jump_status)) {
                picker.refresh_view();
                minibuffer_message(minibuffer, jump_status);
            }
            return;
        }
        if (buffer_list_active) {
            if (pending_close_confirm) {
                switch (ui::parse_confirmation_key(ch)) {
                    case ui::ConfirmationKeyResult::Accepted: {
                        size_t idx = pending_close_index != static_cast<size_t>(-1)
                                         ? pending_close_index
                                         : buffer_list_underlying(buffer_list_selected);
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
                if (buffer_list_nav.search_active) {
                    const std::string sequence =
                        current_escape_was_decoded ? std::string{} : read_escape_suffix();
                    buffer_list_nav.search_active = false;
                    buffer_list_nav.search_draft.clear();
                    if (!sequence.empty()) {
                        MovementKeyEvent movement;
                        if (parse_movement_sequence(sequence, movement)) {
                            buffer_list_selected = move_editor_buffer_selection(
                                buffer_list_selected, buffers.size(), movement.key);
                        }
                    }
                    minibuffer_message(minibuffer, selected_buffer_status());
                    return;
                }
                handle_buffer_list_escape();
                return;
            }
            if (buffer_list_nav.search_active) {
                if (ch == '\r' || ch == '\n') {
                    const std::string needle = buffer_list_nav.search_draft.empty()
                                                   ? buffer_list_nav.last_search
                                                   : buffer_list_nav.search_draft;
                    buffer_list_nav.search_active = false;
                    buffer_list_nav.search_draft.clear();
                    if (needle.empty()) {
                        minibuffer_message(minibuffer, ui::text_selector_no_previous_search_status());
                        return;
                    }
                    buffer_list_nav.last_search = needle;
                    if (ui::find_next_text_selector_match(buffer_list_selected, buffers.size(),
                                                         buffer_list_label_at, needle)) {
                        minibuffer_message(minibuffer, selected_buffer_status());
                    } else {
                        minibuffer_message(minibuffer, ui::text_selector_no_match_status(needle));
                    }
                    return;
                }
                if (ch == 127 || ch == 8) {
                    if (!buffer_list_nav.search_draft.empty()) {
                        buffer_list_nav.search_draft.pop_back();
                    }
                    minibuffer_message(minibuffer, buffer_list_nav.draft_status());
                    return;
                }
                if (ch >= 0x20U && ch != 0x7fU) {
                    buffer_list_nav.search_draft.push_back(static_cast<char>(ch));
                    minibuffer_message(minibuffer, buffer_list_nav.draft_status());
                    return;
                }
                return;
            }
            if (ch == '/') {
                buffer_list_nav.search_active = true;
                buffer_list_nav.search_draft.clear();
                minibuffer_message(minibuffer, buffer_list_nav.draft_status());
                return;
            }
            if (ch == '.') {
                ui::toggle_text_selector_alpha_sort_order(
                    buffer_list_nav.display_order, buffer_list_selected, buffer_list_nav.sorted,
                    buffers.size(),
                    [&](size_t underlying) {
                        if (underlying >= buffers.size()) {
                            return std::string{};
                        }
                        return editor_buffer_display_name(buffers[underlying], underlying);
                    });
                minibuffer_message(minibuffer, selected_buffer_status());
                return;
            }
            if (ch == '\r' || ch == '\n') {
                activate_buffer(buffer_list_underlying(buffer_list_selected));
                return;
            }
            if (ch == '\t') {
                new_empty_buffer();
                return;
            }
            if (ch == 127 || ch == 8) {
                if (!buffers.empty() && buffer_list_selected < buffers.size()) {
                    perform_buffer_close(buffer_list_underlying(buffer_list_selected), false);
                }
                return;
            }
            if (ui::jump_text_selector_by_char(buffer_list_selected, buffers.size(),
                                              buffer_list_label_at, ch)) {
                minibuffer_message(minibuffer, selected_buffer_status());
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
                if (assist_escape_repeat_guard.suppress(EscapeRepeatGuard::Clock::now())) return;
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

        // Ctrl+X window-command prefix (Emacs-style): v/h/2/3/o/0/1.
        if (window_prefix_active) {
            const std::string action = window_prefix_action(ch);
            window_prefix_active = false;
            if (action == "cancel" || action.empty()) {
                minibuffer_message(minibuffer,
                                   action == "cancel" ? "Window command cancelled"
                                                      : "Unknown window command (v/h/o/0/1/d)");
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
                    save_focused_pane_view();
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
            } else if (action == "dired") {
                enter_dired("");
            }
            return;
        }
        if (ch == 24) {
            if (help_view.active || dired.active || picker.active || buffer_list_active ||
                pending_close_confirm || assist_session.active || reformat_session.active ||
                insert_session.active || shell_session.active) {
                minibuffer_message(minibuffer, "Finish the current action before window commands");
                return;
            }
            window_prefix_active = true;
            minibuffer_message(minibuffer, "Window: v/3 vertical  h/2 horizontal  o other  0 close  1 max");
            return;
        }

        // Ctrl+B / Ctrl+D: PageUp / PageDown in the previously focused other pane.
        // Mutate only that leaf's view so the focused pane (and same-buffer siblings)
        // keep their independent cursors/scrolls.
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
            save_focused_pane_view();
            const PaneViewState focused_view = split_layout.focused_view();
            const bool shares_live_state = target_state == &state;
            apply_pane_view_to_state(*target_state, split_layout.leaf_view(*other_leaf));
            const MovementKey key =
                ch == 2 ? MovementKey::PageUp : MovementKey::PageDown;
            target_state->apply_movement(
                key, editor_content_rect(target_pane->rect), false, false, false);
            split_layout.set_leaf_view(*other_leaf, pane_view_from_state(*target_state));
            if (shares_live_state) {
                apply_pane_view_to_state(state, focused_view);
            }
            minibuffer_message(minibuffer,
                               std::string(ch == 2 ? "Scrolled other pane up" : "Scrolled other pane down") +
                                   " (pane " + std::to_string(*other_leaf + 1) + "/" +
                                   std::to_string(split_layout.leaf_count()) + ")");
            return;
        }

        if (ch == 7) {
            // Ctrl+G: return to the chat/agent mode that opened the editor.
            request_editor_toggle();
            return;
        }
        if (ch == 16) {
            // Ctrl+P: open provider picker (same as bare /provider).
            open_provider_picker();
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
            // Background agent turn: confirm cancel before process exit.
            if (agent_controller() && agent_controller()->turn_running()) {
                pending_agent_quit_confirm = true;
                minibuffer_message(minibuffer,
                                   "Agent is still running — cancel and quit? (y/n) ");
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
            if (copy_error.ok()) publish_internal_clipboard();
            minibuffer_message(minibuffer, copy_error.ok() ? "Copied selection" : copy_error.message);
        } else if (ch == 22) {
            if (shared_clipboard().empty()) {
                begin_external_clipboard_paste();
            } else {
                cancel_pending_clipboard();
                Error paste_error =
                    paste_with_clipboard_preference(state, shared_clipboard(), "");
                minibuffer_message(minibuffer,
                                   paste_error.ok() ? "Pasted" : paste_error.message);
            }
        } else if (ch == 6) {
            start_minibuffer(minibuffer, MinibufferAction::Search, "Search: ", last_search);
        } else if (ch == 8) {
            // Ctrl+H: mode-dependent help (standalone editor help view).
            enter_help_view();
        } else if (is_editor_undo_key(ch)) {
            minibuffer_message(minibuffer, state.undo() ? "Undone" : "Nothing to undo");
        } else if (is_editor_redo_key(ch)) {
            minibuffer_message(minibuffer, state.redo() ? "Redone" : "Nothing to redo");
        } else if (ch == 1) {
            state.select_all();
        } else if (ch == 11) {
            const std::uint64_t before = state.revision();
            Error kill_error = state.kill_to_line_end(shared_clipboard());
            if (kill_error.ok() && state.revision() != before)
                publish_internal_clipboard();
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
                const std::string sequence = decoded_escape ? std::string{} : read_escape_suffix();
                if (!sequence.empty()) {
                    unsigned char decoded = 0;
                    if (decode_control_key_sequence(sequence, decoded)) {
                        handle_key(decoded, decoded == 27);
                        return;
                    }
                    if (is_dired_f4_sequence(sequence)) {
                        enter_dired("");
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
                open_assist_command_minibuffer();
                return;
            }
            std::string escape_status;
            const Rect focus_rect = assist_panel_rect();
            handle_escape(state, escape_status, last_search, &focus_rect);
            if (!escape_status.empty()) {
                minibuffer_message(minibuffer, escape_status);
            }
        } else if (ch == 127) {
            const bool had_selection = state.selection.has_range();
            Error erase_error = state.erase_before_cursor();
            if (!erase_error.ok()) {
                minibuffer_message(minibuffer, erase_error.message);
            } else if (had_selection) {
                publish_internal_clipboard();
                minibuffer_message(minibuffer, "Cut selection");
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
        mouse_view_detached = false;
        cancel_pending_clipboard();
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
        Clipboard external;
        external.set(terminal_text);
        Error paste_error =
            terminal_text.empty()
                ? Error{ErrorCode::BadArgs, "terminal paste is empty"}
                : state.paste(external);
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
            if (!last_generated_tokens_message.empty() &&
                (minibuffer.message == last_generated_tokens_message ||
                 minibuffer.message.rfind("Generated ", 0) == 0)) {
                message = last_generated_tokens_message + ". " + message;
                last_generated_tokens_message.clear();
            }
            minibuffer_message(minibuffer, message);
            sync_active_buffer();
        } else if (!message.empty()) {
            minibuffer_message(minibuffer, message);
        }
    };

    if (ai_continue.has_value()) {
        if (provider::needs_interactive_model_selection(ai_continue->request)) {
            start_model_list();
            render_editor();
        } else if (editor_ai_ready(ai_continue) &&
                   !ai_continue->request.options.has_context_tokens) {
            start_model_list(true);
            render_editor();
        }
    }
    schedule_selection_save();

    auto handle_mouse_input = [&](const MouseInputEvent& mouse) {
        if (picker.active || buffer_list_active || pending_close_confirm) return;
        // RO dired view: move the caret with the wheel so Ln/Col status stays accurate.
        if (dired.active && dired.focus == DiredFocus::View) {
            const Rect content_rect = editor_content_rect(editor_main_area());
            if (mouse.row < content_rect.row ||
                mouse.row >= content_rect.row + content_rect.height ||
                mouse.col < content_rect.col ||
                mouse.col >= content_rect.col + content_rect.width) {
                return;
            }
            MovementKey key = MovementKey::Down;
            if (mouse.button == MouseButton::WheelUp) {
                key = MovementKey::Up;
            } else if (mouse.button == MouseButton::WheelDown) {
                key = MovementKey::Down;
            } else {
                return;
            }
            for (int step = 0; step < 3; ++step) {
                dired.view.apply_movement(key, content_rect, false, false, false);
            }
            return;
        }
        if (dired.active) return;
        const Rect pane_rect = help_view.active || !split_layout.has_split()
                                   ? editor_main_area()
                                   : split_layout.focused_rect(editor_main_area());
        const Rect content_rect = editor_content_rect(pane_rect);
        if (!apply_editor_mouse_scroll(state, content_rect, mouse)) return;
        mouse_view_detached = true;
        if (!help_view.active) {
            split_layout.set_focused_view(pane_view_from_state(state));
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

    if (settings.start_dired) {
        enter_dired(settings.start_dired_path.empty() ? "." : settings.start_dired_path);
        // One-shot request from agent F4 / CLI --dired; clear so a later mode
        // cycle does not reopen dired unexpectedly.
        if (interactive != nullptr) {
            interactive->editor_settings.start_dired = false;
            interactive->editor_settings.start_dired_path.clear();
        }
    }

    while (!quit) {
        const bool assist_updated = process_assist_events();
        const bool reformat_updated = process_reformat_events();
        const bool insert_updated = process_insert_events();
        const bool shell_updated = process_shell_events();
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
        const bool selection_updated = process_selection_save_events();
        const bool clipboard_updated = process_clipboard_events();
        const bool assist_animating =
            assist_session.active && assist_session.activity_kind != tui::ActivityKind::None;
        bool assist_animation_updated = false;
        if (assist_animating) {
            const size_t next_frame = static_cast<size_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - activity_animation_started)
                    .count() /
                200);
            assist_animation_updated = next_frame != activity_frame;
            activity_frame = next_frame;
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
                insert_updated || shell_session.job.running() || shell_updated || model_updated ||
                selection_updated || clipboard_updated || assist_animation_updated ||
                clipboard_runtime.read_running()) {
                last_size = current_size;
                render_editor();
            }
            continue;
        }

        last_activity = SteadyClock::now();
        if (event.type == TerminalInputType::Mouse) {
            handle_mouse_input(event.mouse);
        } else if (event.type == TerminalInputType::BracketedPaste) {
            handle_paste(event.text);
            prompt_for_changed_read_only_file();
        } else if (event.type == TerminalInputType::Osc52ClipboardResponse) {
            if (pending_clipboard.active && pending_clipboard.terminal_query) {
                if (event.text.empty()) {
                    ClipboardCommand native_command;
                    if (resolve_clipboard_command(clipboard_environment, false, native_command)) {
                        pending_clipboard.terminal_query = false;
                        pending_clipboard.generation =
                            clipboard_runtime.start_read(clipboard_environment);
                    } else {
                        const std::string message =
                            event.message.empty()
                                ? "Terminal clipboard query failed; use the terminal paste shortcut"
                                : event.message +
                                      "; use the terminal paste shortcut";
                        if (pending_clipboard.minibuffer && minibuffer.active)
                            minibuffer.message = message;
                        else
                            minibuffer_message(minibuffer, message);
                        pending_clipboard = PendingClipboardPaste{};
                    }
                } else {
                    apply_external_clipboard(event.text, "terminal clipboard");
                }
            }
        } else if (event.type == TerminalInputType::Byte) {
            handle_key(event.byte, event.decoded_escape);
            prompt_for_changed_read_only_file();
            while (!quit) {
                if (!read_terminal_input(event, 0)) {
                    break;
                }
                last_activity = SteadyClock::now();
                if (event.type == TerminalInputType::Mouse) {
                    handle_mouse_input(event.mouse);
                } else if (event.type == TerminalInputType::BracketedPaste) {
                    handle_paste(event.text);
                    prompt_for_changed_read_only_file();
                } else if (event.type == TerminalInputType::Osc52ClipboardResponse) {
                    if (pending_clipboard.active && pending_clipboard.terminal_query &&
                        !event.text.empty()) {
                        apply_external_clipboard(event.text, "terminal clipboard");
                    }
                } else if (event.type == TerminalInputType::Byte) {
                    handle_key(event.byte, event.decoded_escape);
                    prompt_for_changed_read_only_file();
                } else {
                    break;
                }
            }
        }
        try_autosave(SteadyClock::duration::zero());
        refresh_agent_background_status();
        // While the agent writes files under an open dired, re-hash and remake
        // RO history-diff marks so dirty files and changed-line tints stay live.
        if (dired.active) {
            const auto now = SteadyClock::now();
            const bool agent_writing =
                agent_controller() != nullptr && agent_controller()->turn_running();
            const auto interval = agent_writing ? std::chrono::milliseconds(400)
                                                : std::chrono::milliseconds(1500);
            if (last_dired_sync.time_since_epoch().count() == 0 ||
                now - last_dired_sync >= interval) {
                last_dired_sync = now;
                (void)dired_sync_live(dired, settings);
            }
        }

        last_size = terminal_size();
        render_editor();
    }
    if (shell_session.job.joinable()) {
        shell_session.job.cancel();
        shell_session.job.join();
    }
    clipboard_runtime.cancel_all();
    selection_save_job.join();
    EditorSelectionSaveEvent selection_event;
    while (selection_save_events.try_pop(selection_event)) {
        if (!selection_event.error.ok()) {
            std::cerr << "Warning: could not remember editor model selection: "
                      << selection_event.error.message << "\n";
        }
    }
    start_pending_selection_save();
    selection_save_job.join();
    while (selection_save_events.try_pop(selection_event)) {
        if (!selection_event.error.ok()) {
            std::cerr << "Warning: could not remember editor model selection: "
                      << selection_event.error.message << "\n";
        }
    }
    sync_active_buffer();
    if (switch_to_agent) {
        return {0, app::InteractiveUiTarget::Agent};
    }
    if (switch_to_chat) {
        return {0, app::InteractiveUiTarget::Chat};
    }
    return {0, app::InteractiveUiTarget::Quit};
}

}  // namespace ainiux::editor
