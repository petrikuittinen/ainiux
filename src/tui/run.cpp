#include "tui/activity.hpp"
#include "tui/agent_progress.hpp"
#include "tui/agent_widgets.hpp"
#include "tui/tui.hpp"
#include "tui/events.hpp"
#include "tui/chat_assist.hpp"
#include "tui/background_metadata.hpp"
#include "tui/commands.hpp"
#include "editor/ai_continue.hpp"

#include "tui/file_jobs.hpp"
#include "tui/input_handlers.hpp"
#include "tui/picker_input.hpp"
#include "tui/provider_actions.hpp"
#include "tui/session_load.hpp"
#include "tui/terminal.hpp"
#include "tui/detail/render.hpp"

#include "provider/model_list_job.hpp"
#include "provider/credit_balance_job.hpp"

#include "app/app.hpp"
#include "app/interactive_mode.hpp"
#include "app/index_progress.hpp"
#include "app/detail.hpp"
#include "app/user_shell.hpp"
#include "agent/approval.hpp"
#include "agent/project_root.hpp"
#include "agent/session_runtime.hpp"
#include "agent/tool_display.hpp"
#include "chat/settings.hpp"
#include "chat/media_store.hpp"
#include "ainiux/model_setting.hpp"
#include "chat/sqlite_store.hpp"
#include "cli/args.hpp"
#include "config/config.hpp"
#include "context/context.hpp"
#include "editor/clipboard.hpp"
#include "editor/path_completion.hpp"
#include "editor/terminal_input.hpp"

#include "fetch/fetch.hpp"
#include "search/search.hpp"
#include "input/input.hpp"
#include "runtime/runtime.hpp"
#include "ui/confirmation.hpp"
#include "ui/text_selector.hpp"
#include "ui/provider_model_selector.hpp"
#include "ui/provider_model_display.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <sys/select.h>
#include <unistd.h>

namespace ainiux::tui {

using detail::RenderStyle;

struct PendingTuiClipboardPaste {
    bool active = false;
    bool terminal_query = false;
    std::uint64_t generation = 0;
    std::uint64_t buffer_id = 0;
    std::uint64_t revision = 0;
    size_t cursor = 0;
    editor::Selection selection;
    TuiMode mode = TuiMode::Chat;
    std::chrono::steady_clock::time_point query_deadline;
};

app::TuiRunResult run(provider::RequestContext context,
                      chat::Session session,
                      app::InteractiveSession* interactive) {
    const provider::RequestContext cli_context = context;
    std::string* shared_routing_session_id = nullptr;
    if (interactive != nullptr) {
        shared_routing_session_id =
            context.options.agent
                ? &interactive->agent_routing_session_id
                : &interactive->chat_routing_session_id;
    }
    if (shared_routing_session_id != nullptr &&
        !shared_routing_session_id->empty()) {
        context.routing_session_id = *shared_routing_session_id;
    } else {
        context.routing_session_id = provider::new_routing_session_id();
        if (shared_routing_session_id != nullptr)
            *shared_routing_session_id = context.routing_session_id;
    }
    auto rotate_routing_session_id = [&]() {
        context.routing_session_id = provider::new_routing_session_id();
        if (shared_routing_session_id != nullptr)
            *shared_routing_session_id = context.routing_session_id;
    };
    TerminalSession terminal;
    Error err = terminal.enter();
    if (!err.ok()) {
        std::cerr << error_code_name(err.code) << ": " << err.message << "\n";
        return {app::exit_code_for(err.code), app::InteractiveUiTarget::Quit};
    }

    runtime::EventQueue<TuiEvent> events;
    runtime::JobHandle model_job;
    runtime::BackgroundJobs credit_jobs;
    runtime::JobHandle file_job;
    runtime::JobHandle completion_job;
    ActiveJob active_job = ActiveJob::None;
    std::string credit_balance_label;
    std::optional<chat::Session> deferred_store_save;
    std::optional<std::string> deferred_agent_prompt;
    // Assigned once start_agent_prepare is defined. Starts local project prepare
    // even when the temporary offline "none" profile is active for the picker.
    std::function<void()> ensure_agent_prepare;
    const size_t input_undo_limit = static_cast<size_t>(std::max(0, context.options.editor_undo_limit));
    bool syntax_highlight = interactive != nullptr ? interactive->highlight_enabled
                                                   : context.options.tui_highlight;
    auto new_input_editor = [&]() {
        editor::EditorState editor = detail::empty_input_editor(input_undo_limit);
        editor.set_language(highlight::Language::Markdown, false);
        editor.highlight_enabled = syntax_highlight;
        return editor;
    };
    editor::EditorState input = new_input_editor();
    editor::ContextualCompleter path_completer;
    editor::AiContinueContext ai_continue;
    ai_continue.request = context;
    ai_continue.settings = editor::ai_continue_settings(context.options);
    ai_continue.assist_config = context.options.editor_assist_config;
    if (ai_continue.assist_config.commands.empty()) {
        ai_continue.assist_config = editor::default_editor_assist_config();
    }
    if (interactive != nullptr) {
        if (interactive->ai_continue.has_value()) {
            ai_continue = *interactive->ai_continue;
            ai_continue.request = context;
        } else {
            ai_continue.assist_config = interactive->assist_config;
        }
    }
    path_completer.set_assist_config(context.options.agent
                                         ? nullptr
                                         : &ai_continue.assist_config);
    path_completer.set_agent_mode(context.options.agent);
    ChatAssistCallbacks chat_assist_callbacks;
    size_t completion_generation = 0;
    bool completion_pending = false;
    std::string status = context.options.agent ? "Preparing agent..." : ready_status();
    AgentActivityState agent_activity_state =
        context.options.agent ? AgentActivityState::Preparing
                              : AgentActivityState::Ready;
    std::chrono::steady_clock::time_point agent_activity_started =
        std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point agent_task_started;
    bool agent_task_active = false;
    long long agent_completed_task_ms = -1;
    bool agent_compaction_active = false;
    CompactionStrategy agent_compaction_strategy = CompactionStrategy::Smart;
    std::string theme = "dark";
    context.options.tui_themes.normalize_name(context.options.tui_theme, theme);
    bool use_colors = !context.options.no_colors;
    if (interactive != nullptr) {
        context.options.tui_themes.normalize_name(interactive->theme_name, theme);
        use_colors = interactive->use_colors;
    }
    bool quit = false;
    app::InteractiveUiTarget leave_target = app::InteractiveUiTarget::Quit;
    // Warm multi-turn agent session (project .ainiux-pr/agent.sqlite).
    // Prepared on agent-mode entry (index refresh + history load), not deferred
    // until the first user turn.
    std::shared_ptr<agent::AgentSessionRuntime> agent_runtime =
        context.options.agent ? std::make_shared<agent::AgentSessionRuntime>()
                              : std::shared_ptr<agent::AgentSessionRuntime>{};
    auto agent_ready_with_index_controls = [&]() {
        std::string value = agent_ready_status();
        if (agent_runtime && agent_runtime->prepared() &&
            !agent_runtime->indexing_enabled())
            value += " · indexing off · /index-code to enable";
        return value;
    };
    std::string initial_agent_workspace = ".";
    if (context.options.agent) {
        std::string resolved_workspace;
        if (agent::resolve_agent_project_root(".", resolved_workspace).ok()) {
            initial_agent_workspace = std::move(resolved_workspace);
        }
    }
    // Shared with the agent worker: blocks tool execution until the user answers y/n.
    std::shared_ptr<agent::ApprovalGate> agent_approval_gate =
        context.options.agent ? std::make_shared<agent::ApprovalGate>()
                              : std::shared_ptr<agent::ApprovalGate>{};
    agent::GuardApprovalRequest pending_guard_request;
    bool have_pending_guard_request = false;
    if (agent_approval_gate) {
        agent_approval_gate->set_notify([&events](const agent::GuardApprovalRequest& request) {
            TuiEvent event;
            event.type = TuiEventType::GuardApproval;
            event.guard_tool_name = request.tool_name;
            event.guard_command_preview = request.command_preview;
            event.guard_rule_id = request.rule_id;
            event.guard_message = request.message;
            events.push(std::move(event));
        });
    }
    auto make_agent_runtime_options = [&](const std::string& workspace = std::string()) {
        agent::SessionRuntimeOptions options;
        options.workspace = !workspace.empty()
                                ? workspace
                                : (agent_runtime && agent_runtime->prepared()
                                       ? agent_runtime->workspace()
                                       : std::string("."));
        options.task_mode = agent::AgentTaskMode::Act;
        options.allow_network = true;
        options.interactive = true;
        options.enable_session_db = true;
        options.enable_agent_log = context.options.agent_log_enabled;
        options.security_review_log_keep_runs = context.options.security_review_log_keep_runs;
        options.trusted_prompt_dir = context.options.trusted_prompt_dir;
        options.max_source_code_file_size = context.options.max_source_code_file_size;
        options.history_backup.enabled = context.options.agent_history_backup_enabled;
        options.history_backup.max_bytes = context.options.agent_history_backup_max_bytes;
        options.history_backup.ttl_days = context.options.agent_history_backup_ttl_days;
        options.auto_compact = context.options.agent_auto_compact;
        options.compact_strategy = context.options.agent_compact_strategy;
        options.compact_limit = context.options.agent_compact_limit;
        options.index_mode =
            context.options.disable_indexing
                ? agent::SessionRuntimeOptions::IndexMode::Disabled
                : agent::SessionRuntimeOptions::IndexMode::UseExistingLazy;
        options.show_command_output = context.options.agent_show_command_output;
        options.fetch_options.connect_timeout_seconds = context.options.connect_timeout_seconds;
        options.fetch_options.timeout_seconds =
            context.options.timeout_seconds > 0 ? context.options.timeout_seconds : 30;
        options.fetch_options.max_bytes = context.options.max_fetch_bytes;
        options.fetch_options.proxy = context.options.proxy;
        options.fetch_options.insecure_tls = context.options.insecure_tls;
        options.fetch_options.trace_http = context.options.trace_http;
        options.fetch_options.allow_private = context.options.allow_private_url_fetch;
        options.search_options = search::options_for(context.options);
        options.on_phase = [&events](agent::AgentActivityPhase phase) {
            TuiEvent event;
            event.type = TuiEventType::AgentPhase;
            event.agent_phase = phase;
            events.push(std::move(event));
        };
        options.on_prepare_progress =
            [&events](const agent::PreparationProgress& progress) {
                TuiEvent event;
                event.type = TuiEventType::AgentPrepareProgress;
                event.agent_prepare_progress = progress;
                events.push(std::move(event));
            };
        if (agent_approval_gate) {
            std::shared_ptr<agent::ApprovalGate> gate = agent_approval_gate;
            options.on_guard_ask =
                [gate](const agent::GuardApprovalRequest& request,
                       runtime::CancellationToken cancellation) -> agent::GuardApprovalDecision {
                return gate->request(request, cancellation);
            };
        }
        return options;
    };
    bool show_thinking_traces = context.options.show_thinking_traces;
    size_t pending_user = static_cast<size_t>(-1);
    size_t pending_assistant = static_cast<size_t>(-1);
    bool pending_user_added_for_job = false;
    std::vector<AgentLiveRow> live_agent_rows;
    int history_scroll = 0;

    auto build_agent_chrome = [&]() -> AgentChrome {
        AgentChrome chrome;
        if (!context.options.agent) {
            return chrome;
        }
        chrome.enabled = true;
        chrome.provider = context.profile.name;
        chrome.model = context.options.model;
        chrome.reasoning = config::reasoning_selection_value(context.options.reasoning);
        // Prefer the worker-published atomic estimate (never walks conversation_ here —
        // that raced with run_user_turn and segfaulted during streaming). Fall back to
        // the display transcript on the UI thread.
        long long used = 0;
        if (agent_runtime && agent_runtime->prepared()) {
            used = agent_runtime->estimated_request_tokens();
        }
        const long long display_used = context::estimated_text_tokens(session.messages);
        if (display_used > used) {
            used = display_used;
        }
        chrome.used_tokens = used;
        chrome.window_tokens = context.options.context_tokens;
        chrome.workspace =
            agent_runtime && agent_runtime->prepared()
                ? agent_runtime->workspace()
                : initial_agent_workspace;
        // Active /goal overrides the task-mode tag (act/plan) until the goal
        // completes, is cleared/paused, or otherwise becomes inactive.
        if (agent_runtime && agent_runtime->prepared() &&
            agent::goal_is_active(agent_runtime->goal())) {
            chrome.mode_label = "goal";
        } else if (agent_runtime && agent_runtime->prepared()) {
            chrome.mode_label = agent::agent_task_mode_name(agent_runtime->task_mode());
        } else {
            chrome.mode_label = "act";
        }
        chrome.permission_label =
            agent_runtime && agent_runtime->prepared()
                ? agent::permission_mode_name(agent_runtime->permission_mode())
                : "smart";
        chrome.index_enabled =
            agent_runtime && agent_runtime->prepared() &&
            agent_runtime->indexing_enabled();
        chrome.credit_label = credit_balance_label;
        chrome.input_max_height_percent =
            context.options.agent_input_max_height_percent;
        chrome.cancellable = active_job != ActiveJob::None || file_job.joinable() ||
                             completion_job.joinable();
        chrome.activity_state = agent_activity_state;
        chrome.completed_task_ms = agent_completed_task_ms;
        if (agent_task_active) {
            chrome.task_elapsed_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                              std::chrono::steady_clock::now() -
                                              agent_task_started)
                                              .count();
        } else if (agent_activity_state == AgentActivityState::Preparing) {
            chrome.task_elapsed_seconds =
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() -
                    agent_activity_started)
                    .count();
        }
        return chrome;
    };

    auto current_layout = [&](int rows, int cols) {
        if (!context.options.agent) return layout_for_terminal(rows, cols);
        const int percentage_cap =
            std::max(3, (std::max(6, rows) *
                         context.options.agent_input_max_height_percent) /
                            100);
        const size_t measured = input.visual_row_count_bounded(
            static_cast<size_t>(std::max(1, cols - 2)),
            static_cast<size_t>(std::max(1, percentage_cap - 2)));
        const AgentInputGeometry geometry =
            agent_input_geometry(rows, cols, measured,
                                 context.options.agent_input_max_height_percent);
        return layout_for_agent_terminal(rows, cols, geometry.box_height);
    };

    auto append_agent_history_notice = [&](const std::string& text) {
        if (!context.options.agent || text.empty()) {
            return;
        }
        if (!session.messages.empty() && session.messages.back().role == "notice" &&
            session.messages.back().content == text) {
            return;
        }
        provider::Message notice{"notice", text};
        notice.created_at_ms = agent::now_unix_ms();
        session.messages.push_back(std::move(notice));
        history_scroll = history_scroll_for_thread_end();
        if (agent_runtime && agent_runtime->prepared()) {
            (void)agent_runtime->append_display_notice(text);
        }
    };

    // Agent mode: durable errors go to transcript history; status returns to idle chrome.
    auto report_agent_error = [&](const std::string& text) {
        if (!context.options.agent) {
            status = text;
            return;
        }
        append_agent_history_notice(text);
        if (agent_runtime && agent_runtime->prepared() && !agent_task_active)
            status = agent_ready_with_index_controls();
        else if (agent_activity_state == AgentActivityState::Unavailable)
            status = "Agent unavailable";
        else
            status = "Preparing agent...";
    };

    auto set_status_maybe_agent_error = [&](const std::string& text, bool as_error) {
        if (as_error && context.options.agent) {
            report_agent_error(text);
        } else {
            status = text;
        }
    };

    bool regenerate_after_cancel = false;
    std::string queued_regeneration_prompt;
    std::vector<provider::ImageInput> pending_images;
    size_t inflight_image_count = 0;
    std::string help_text;
    std::string settings_text;
    std::string pending_reasoning;
    std::string pending_reasoning_warning;
    agent::NewProjectTarget pending_new_project;
    bool have_pending_new_project = false;
    std::vector<provider::Message> project_switch_previous_history;
    TuiMode mode = TuiMode::Chat;
    editor::ClipboardRuntime clipboard_runtime;
    const editor::ClipboardEnvironment clipboard_environment =
        editor::current_clipboard_environment();
    PendingTuiClipboardPaste pending_clipboard;
    auto clipboard_mode_editable = [](TuiMode value) {
        return value == TuiMode::Chat || value == TuiMode::SystemEdit ||
               value == TuiMode::HistoryEdit;
    };
    auto cancel_pending_clipboard = [&]() {
        clipboard_runtime.cancel_read();
        editor::cancel_terminal_clipboard_request();
        pending_clipboard = PendingTuiClipboardPaste{};
    };
    auto publish_internal_clipboard = [&]() {
        const std::string text = editor::shared_clipboard().text();
        if (text.empty()) return;
        editor::publish_terminal_clipboard(text);
        (void)clipboard_runtime.start_write(clipboard_environment, text);
    };
    auto clipboard_target_unchanged = [&]() {
        return pending_clipboard.active && mode == pending_clipboard.mode &&
               input.buffer_id() == pending_clipboard.buffer_id &&
               input.revision() == pending_clipboard.revision &&
               input.cursor == pending_clipboard.cursor &&
               input.selection.anchor == pending_clipboard.selection.anchor &&
               input.selection.active == pending_clipboard.selection.active;
    };
    auto apply_external_clipboard = [&](const std::string& text,
                                        const std::string& backend) {
        if (!clipboard_target_unchanged()) {
            status = "Clipboard target changed; press Ctrl+V to paste again";
            pending_clipboard = PendingTuiClipboardPaste{};
            return;
        }
        editor::Clipboard external;
        external.set(text);
        const Error paste_error = input.paste(external);
        status = paste_error.ok() ? "Pasted from " + backend : paste_error.message;
        pending_clipboard = PendingTuiClipboardPaste{};
    };
    auto begin_external_clipboard_paste = [&]() {
        cancel_pending_clipboard();
        if (!clipboard_mode_editable(mode)) {
            status = "Paste is not accepted in this confirmation or picker";
            return;
        }
        pending_clipboard.active = true;
        pending_clipboard.buffer_id = input.buffer_id();
        pending_clipboard.revision = input.revision();
        pending_clipboard.cursor = input.cursor;
        pending_clipboard.selection = input.selection;
        pending_clipboard.mode = mode;
        editor::ClipboardCommand native_command;
        const bool native_available =
            editor::resolve_clipboard_command(clipboard_environment, false, native_command);
        if (editor::prefer_terminal_clipboard_query(clipboard_environment) ||
            !native_available) {
            pending_clipboard.terminal_query = true;
            pending_clipboard.query_deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(2);
            editor::request_terminal_clipboard();
        } else {
            pending_clipboard.generation =
                clipboard_runtime.start_read(clipboard_environment);
        }
        status = "Reading system clipboard...";
    };
    auto process_clipboard_events = [&]() {
        editor::ClipboardRuntimeEvent clipboard_event;
        while (clipboard_runtime.try_pop(clipboard_event)) {
            if (clipboard_event.type ==
                editor::ClipboardRuntimeEventType::WriteFinished) {
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
                status = editor::clipboard_failure_help(clipboard_environment,
                                                        clipboard_event.result,
                                                        true);
                pending_clipboard = PendingTuiClipboardPaste{};
            }
        }
        if (pending_clipboard.active && pending_clipboard.terminal_query &&
            std::chrono::steady_clock::now() >= pending_clipboard.query_deadline) {
            editor::cancel_terminal_clipboard_request();
            editor::ClipboardCommand native_command;
            if (editor::resolve_clipboard_command(clipboard_environment, false,
                                                  native_command)) {
                pending_clipboard.terminal_query = false;
                pending_clipboard.generation =
                    clipboard_runtime.start_read(clipboard_environment);
            } else {
                status =
                    "Terminal did not provide clipboard text; use the terminal paste shortcut";
                pending_clipboard = PendingTuiClipboardPaste{};
            }
        }
    };
    size_t history_edit_index = static_cast<size_t>(-1);
    std::vector<chat::ThreadSummary> thread_picker_threads;
    size_t thread_picker_selected = 0;
    std::vector<std::string> picker_items;
    size_t picker_selected = 0;
    bool picker_cancel_quits = false;
    bool loaded_thread_requires_provider_selection = false;
    std::vector<ChatAttachment> chat_attachments;
    size_t attachment_picker_selected = 0;
    size_t pending_attachment_delete = static_cast<size_t>(-1);
    size_t pending_thread_delete = static_cast<size_t>(-1);
    size_t attachments_committed_for_turn = 0;
    std::vector<provider::ImageInput> queued_regen_images;
    std::vector<provider::TextAttachment> queued_regen_text_attachments;
    ModelsRequestPurpose models_request_purpose = ModelsRequestPurpose::Preview;
    provider::ModelsResult cached_models;
    bool have_cached_models = false;
    chat::SqliteStore sqlite_store;
    bool sqlite_available = false;
    std::string sqlite_path;
    std::string sqlite_unavailable_reason;

    auto apply_loaded_session_context = [&](const chat::Session& loaded) {
        Error context_error = apply_loaded_session_to_context(context, loaded);
        show_thinking_traces = context.options.show_thinking_traces;
        return context_error;
    };

    auto finish_loaded_session = [&](const std::string& loaded_label) {
        pending_images.clear();
        inflight_image_count = 0;
        chat_attachments.clear();
        attachment_picker_selected = 0;
        attachments_committed_for_turn = 0;
        queued_regen_images.clear();
        queued_regen_text_attachments.clear();
        history_scroll = 0;
        const std::string status_label =
            session.read_only
                ? loaded_label + " [read-only: " + session.read_only_reason + "]"
                : loaded_label;
        if (!session_has_complete_provider_model(session)) {
            loaded_thread_requires_provider_selection = true;
            const Error context_error = apply_loaded_session_context(session);
            picker_items = ui::selectable_provider_ids();
            picker_selected = 0;
            picker_cancel_quits = false;
            mode = TuiMode::ProviderList;
            help_text.clear();
            settings_text.clear();
            const std::string missing =
                saved_provider_model_missing(session.provider, session.model);
            status = "Thread setup: " + missing + " · select provider; model follows";
            if (!context_error.ok()) {
                status += " · saved context error: " + context_error.message;
            }
            return;
        }
        loaded_thread_requires_provider_selection = false;
        if (!chat_provider_model_ready(context)) {
            const Error context_error = apply_loaded_session_context(session);
            status = context_error.ok() ? status_label : detail::error_line(context_error);
            return;
        }
        if (loaded_session_differs_from_context(context, session)) {
            mode = TuiMode::ModelConfirm;
            status = status_label;
            return;
        }
        if (active_context_has_provider_selection(context)) {
            app::refresh_session_metadata(session, context);
            status = status_label;
            return;
        }
        Error context_error = apply_loaded_session_context(session);
        status = context_error.ok() ? status_label : detail::error_line(context_error);
    };

    auto start_new_thread_from_cli = [&]() {
        loaded_thread_requires_provider_selection = false;
        restore_cli_context(context, cli_context);
        rotate_routing_session_id();
        show_thinking_traces = context.options.show_thinking_traces;
        session = chat::new_session(context);
        pending_images.clear();
        inflight_image_count = 0;
        chat_attachments.clear();
        attachment_picker_selected = 0;
        attachments_committed_for_turn = 0;
        queued_regen_images.clear();
        queued_regen_text_attachments.clear();
        app::apply_system_prompt(session, context.options.system);
        history_scroll = 0;
    };

    auto panel_text = [&]() {
        if (mode == TuiMode::ThreadList) {
            return thread_picker_text(thread_picker_threads, thread_picker_selected);
        }
        if (mode == TuiMode::ProviderList) {
            return ui::provider_selector_text(picker_items, picker_selected);
        }
        if (mode == TuiMode::ModelList) {
            return ui::model_selector_text(picker_items, picker_selected);
        }
        if (mode == TuiMode::ReasoningList) {
            return config::reasoning_selector_text(context.options.model_catalog,
                                                   context.profile.name,
                                                   context.api_kind == provider::ApiKind::Responses
                                                       ? "responses"
                                                       : "chat",
                                                   context.options.model,
                                                   picker_selected);
        }
        if (mode == TuiMode::ReasoningConfirm) {
            const std::string detail =
                pending_reasoning_warning + "\n\nUse '" + pending_reasoning + "' anyway?";
            return context.options.agent
                       ? detail
                       : detail + " Press y to proceed · n or Esc to cancel";
        }
        if (mode == TuiMode::AttachmentList) {
            return attachment_picker_text(chat_attachments, attachment_picker_selected);
        }
        if (mode == TuiMode::AttachmentDeleteConfirm) {
            if (pending_attachment_delete < chat_attachments.size()) {
                return "Delete attachment:\n  " + chat_attachments[pending_attachment_delete].source +
                       "\nPress y to delete · n or Esc to cancel";
            }
            return std::string("No attachment selected to delete");
        }
        if (mode == TuiMode::ThreadDeleteConfirm) {
            if (pending_thread_delete < thread_picker_threads.size()) {
                const auto& th = thread_picker_threads[pending_thread_delete];
                std::string label = th.name.empty() ? ("thread " + std::to_string(th.id)) : th.name;
                if (!th.last_provider.empty() || !th.last_model.empty()) {
                    label += " " + ui::provider_model_display_label(
                                       th.last_provider, th.last_model);
                }
                return "Delete thread:\n  " + label + "\nPress y to delete · n or Esc to cancel";
            }
            return std::string("No thread selected to delete");
        }
        if (mode == TuiMode::RemoveConfirm) {
            return remove_confirm_text(session);
        }
        if (mode == TuiMode::ModelConfirm) {
            return model_confirm_text(context, session);
        }
        if (mode == TuiMode::GuardApprovalConfirm) {
            std::string text = "Guard approval required";
            if (have_pending_guard_request) {
                text = "Allow this agent action?\n\n";
                if (!pending_guard_request.tool_name.empty())
                    text += "Tool: " + pending_guard_request.tool_name + "\n";
                if (!pending_guard_request.command_preview.empty())
                    text += "Command: " + pending_guard_request.command_preview + "\n";
                if (!pending_guard_request.rule_id.empty())
                    text += "Rule: " + pending_guard_request.rule_id + "\n";
                if (!pending_guard_request.message.empty())
                    text += "\n" + pending_guard_request.message + "\n";
            }
            if (!context.options.agent)
                text += "\nPress y to allow · n or Esc to deny";
            return text;
        }
        if (mode == TuiMode::AgentPermissionSelect) {
            return std::string(
                "Select project permission mode.\n\n"
                "Confirm asks for writes and external access.\n"
                "Smart allows project/system-temp native access and asks elsewhere.\n"
                "Yolo allows validated actions; hard safety and Plan denials remain.");
        }
        if (mode == TuiMode::AgentContinueConfirm) {
            return std::string("The agent reached its 50-round safety cap.\n\n"
                               "Continue the current task with a fresh round budget?");
        }
        if (mode == TuiMode::AgentNewConfirm) {
            if (!have_pending_new_project) return std::string("No agent project selected");
            return "Permanently remove agent project state:\n  " +
                   pending_new_project.state_dir +
                   "\n\nThis removes project history, index, approvals, and logs. "
                   "Other files in the project directory are preserved.";
        }
        if (mode == TuiMode::AgentIndexBuildConfirm) {
            return std::string(
                "No code index found. Code indexing can speed up some "
                "queries. Do you want to build code index?");
        }
        if (mode == TuiMode::SystemEdit) {
            return system_edit_text();
        }
        if (mode == TuiMode::HistoryEdit) {
            return history_edit_text();
        }
        if (!help_text.empty()) {
            return help_text;
        }
        return settings_text;
    };

    auto panel_title = [&]() -> const char* {
        if (mode != TuiMode::Chat) {
            return nullptr;
        }
        if (!help_text.empty()) {
            return "Help";
        }
        if (!settings_text.empty()) {
            return "Settings";
        }
        return nullptr;
    };

    auto refresh_settings_panel_if_visible = [&]() {
        if (!settings_text.empty()) {
            settings_text = chat::format_settings_panel(
                context.options,
                provider::reasoning_temperature_advisory(context));
        }
    };

    auto sqlite_unavailable_message = [&]() {
        return sqlite_unavailable_status(sqlite_unavailable_reason);
    };

    TuiFileJobs file_jobs{file_job,
                         events,
                         context,
                         session,
                         sqlite_store,
                         sqlite_path,
                         sqlite_available,
                         sqlite_unavailable_message,
                         status};

    const chat::DatabasePathResult sqlite_db_path = chat::default_sqlite_database_path();
    if (!sqlite_db_path.error.ok()) {
        sqlite_unavailable_reason = sqlite_db_path.error.message;
        status = sqlite_unavailable_message();
    } else {
        sqlite_path = sqlite_db_path.path;
        const Error sqlite_open_error = sqlite_store.open(sqlite_path);
        if (sqlite_open_error.ok()) {
            sqlite_available = true;
        } else {
            sqlite_unavailable_reason = sqlite_open_error.message;
            status = sqlite_unavailable_message();
        }
    }

    auto pending_assistant_is_hidden_thinking = [&]() {
        if (show_thinking_traces || pending_assistant == static_cast<size_t>(-1) ||
            pending_assistant >= session.messages.size()) {
            return false;
        }
        const ThinkingDisplay display = thinking_display_text(session.messages[pending_assistant].content, false);
        return display.saw_thinking_tag && app::detail::trim_ascii(display.text).empty();
    };

    auto start_path_completion = [&]() {
        if (!path_completer.can_complete(input)) {
            return;
        }
        if (path_completer.can_cycle(input)) {
            const editor::PathCompletionResult completion = path_completer.complete(input);
            if (completion.handled) {
                status = editor::path_completion_status(completion);
            }
            return;
        }
        if (editor::is_chat_slash_command_tab_completion(input)) {
            const editor::PathCompletionResult completion = path_completer.complete(input);
            if (completion.handled) {
                status = editor::path_completion_status(completion);
            }
            return;
        }
        if (completion_pending) {
            status = "Tab completion is still running";
            return;
        }

        editor::EditorState completion_input = input;
        editor::ContextualCompleter completion_completer;
        completion_completer.set_assist_config(
            context.options.agent ? nullptr : &ai_continue.assist_config);
        completion_completer.set_agent_mode(context.options.agent);
        const size_t generation = completion_generation;
        completion_job.start(
            [completion_input = std::move(completion_input),
             completion_completer = std::move(completion_completer),
             generation,
             &events](runtime::CancellationToken token) mutable {
                TuiEvent event;
                event.type = TuiEventType::CompletionDone;
                event.completion_generation = generation;
                event.completion = completion_completer.complete(
                    completion_input, [&token]() { return token.cancelled(); });
                event.completed_input = std::move(completion_input);
                event.path_completer = std::move(completion_completer);
                events.push(std::move(event));
            });
        completion_pending = true;
        status = "Completing...";
    };

    auto rollback_pending_turn = [&]() {
        if (pending_assistant != static_cast<size_t>(-1) && pending_assistant < session.messages.size()) {
            session.messages.erase(session.messages.begin() + static_cast<long>(pending_assistant));
        }
        if (pending_user_added_for_job && pending_user != static_cast<size_t>(-1) &&
            pending_user < session.messages.size()) {
            session.messages.erase(session.messages.begin() + static_cast<long>(pending_user));
        }
        pending_user = static_cast<size_t>(-1);
        pending_assistant = static_cast<size_t>(-1);
        pending_user_added_for_job = false;
    };

    auto keep_cancelled_turn = [&]() {
        if (pending_assistant != static_cast<size_t>(-1) && pending_assistant < session.messages.size() &&
            session.messages[pending_assistant].content.empty()) {
            session.messages.erase(session.messages.begin() + static_cast<long>(pending_assistant));
        }
        pending_user = static_cast<size_t>(-1);
        pending_assistant = static_cast<size_t>(-1);
        pending_user_added_for_job = false;
    };

    auto strip_non_chat_roles = [](chat::Session& snapshot) {
        // Chat SQLite / JSON chat files only persist system/user/assistant.
        // Notices (e.g. /shell) are display-only in the open TUI session.
        std::vector<provider::Message> kept;
        kept.reserve(snapshot.messages.size());
        for (provider::Message& message : snapshot.messages) {
            if (app::is_provider_chat_role(message.role)) {
                kept.push_back(std::move(message));
            }
        }
        snapshot.messages = std::move(kept);
    };

    auto start_save = [&](const std::string& path, chat::Session snapshot, bool quiet_success = false) {
        strip_non_chat_roles(snapshot);
        file_jobs.start_save(path, std::move(snapshot), quiet_success);
    };

    auto start_store_save = [&]() {
        // Agent mode persists to .ainiux-pr/agent.sqlite only. The chat library
        // rejects tool/notice/summary roles, which caused
        // AINIUX_ERR_PROVIDER_SCHEMA after agent turns.
        if (context.options.agent) {
            return;
        }
        if (!sqlite_available || session.read_only) {
            return;
        }
        chat::Session snapshot = session;
        strip_non_chat_roles(snapshot);
        app::refresh_session_metadata(snapshot, context);
        if (file_job.joinable()) {
            deferred_store_save = std::move(snapshot);
            return;
        }
        deferred_store_save.reset();
        file_jobs.start_store_save(std::move(snapshot));
    };

    auto resume_deferred_store_save = [&]() {
        if (!deferred_store_save.has_value() || file_job.joinable()) {
            return;
        }
        chat::Session snapshot = std::move(*deferred_store_save);
        deferred_store_save.reset();
        file_jobs.start_store_save(std::move(snapshot));
    };

    auto start_store_load = [&](long long thread_id) { file_jobs.start_store_load(thread_id); };

    auto apply_store_save_result = [&](const chat::Session& saved) {
        if (session.thread_id != 0 && session.thread_id != saved.thread_id) {
            return;
        }
        session.thread_id = saved.thread_id;
        session.name = saved.name;
        session.created_at = saved.created_at;
        session.updated_at = saved.updated_at;
        if (deferred_store_save.has_value() && deferred_store_save->thread_id == 0) {
            deferred_store_save->thread_id = saved.thread_id;
            deferred_store_save->created_at = saved.created_at;
        }
    };

    auto persist_settings_change = [&](const std::string& message) {
        app::refresh_session_metadata(session, context);
        status = message;
        start_store_save();
        if (context.options.agent && agent_runtime && agent_runtime->prepared()) {
            const Error error = agent_runtime->update_project_settings(context);
            if (!error.ok()) status = "Agent settings save failed: " + error.message;
        }
    };

    auto set_thinking_trace_mode = [&](bool show_traces) {
        show_thinking_traces = show_traces;
        context.options.show_thinking_traces = show_traces;
        context.options.has_show_thinking_traces = true;
        if (!show_thinking_traces && pending_assistant_is_hidden_thinking()) {
            status = "Thinking...";
        } else {
            status = show_thinking_traces ? "Thinking traces shown" : "Thinking traces hidden";
        }
        app::refresh_session_metadata(session, context);
        start_store_save();
        if (context.options.agent && agent_runtime && agent_runtime->prepared()) {
            const Error error = agent_runtime->update_project_settings(context);
            if (!error.ok()) status = "Agent settings save failed: " + error.message;
        }
        refresh_settings_panel_if_visible();
    };

    auto start_models = [&](ModelsRequestPurpose purpose = ModelsRequestPurpose::Preview) {
        if (active_job != ActiveJob::None) {
            status = "A model job is already running";
            return;
        }
        models_request_purpose = purpose;
        active_job = ActiveJob::Models;
        provider::RequestContext job_context = context;
        provider::start_list_models_job(model_job, job_context, [&events](Error error, provider::ModelsResult models) {
            TuiEvent event;
            event.type = TuiEventType::ModelsDone;
            event.error = std::move(error);
            event.models = std::move(models.model_ids);
            event.models_result = std::move(models);
            events.push(std::move(event));
        });
        status = purpose == ModelsRequestPurpose::Picker ? "Loading models..." : "Listing models...";
    };

    auto refresh_model_context = [&]() {
        if (context.options.has_context_tokens) return;
        // Automatic context discovery is deliberately catalog-only. Network
        // /models requests happen only for explicit model lists/pickers.
        apply_automatic_context_catalog(context);
    };

    auto refresh_credit_balance = [&]() {
        credit_balance_label.clear();
        if (!context.options.agent ||
            !provider::credit_balance_available(context)) {
            credit_jobs.cancel_all();
            return;
        }
        provider::RequestContext job_context =
            automatic_metadata_request(context);
        const std::string requested_provider =
            provider::normalize_provider_key(context.profile.name);
        const std::uint64_t generation = credit_jobs.generation() + 1;
        credit_jobs.start(
            [job_context = std::move(job_context), requested_provider,
             generation, &events](runtime::CancellationToken token) mutable {
                provider::CreditBalanceResult result;
                Error error =
                    provider::get_credit_balance(job_context, result, token);
                TuiEvent event;
                event.type = TuiEventType::CreditBalanceDone;
                event.requested_provider = requested_provider;
                event.background_generation = generation;
                event.error = std::move(error);
                event.credit_balance = std::move(result);
                events.push(std::move(event));
            });
    };

    auto open_provider_picker = [&](bool cancel_quits) {
        if (active_job != ActiveJob::None) {
            status = "Cannot change provider while a model job is running";
            return;
        }
        picker_items = ui::selectable_provider_ids();
        picker_selected = 0;
        picker_cancel_quits = cancel_quits;
        mode = TuiMode::ProviderList;
        history_scroll = 0;
        help_text.clear();
        settings_text.clear();
        status = picker_items.empty()
                     ? "No providers available"
                     : ui::text_selector_status("Selected provider", picker_selected, picker_items.size());
    };

    auto open_reasoning_picker = [&]() {
        if (active_job != ActiveJob::None) {
            status = "Cannot change reasoning while a model job is running";
            return;
        }
        const ModelCapability* capability = provider::matched_model_capability(context);
        if (capability == nullptr) {
            status = "No reasoning catalog entry matches " + context.options.model +
                     ". Use /reasoning VALUE directly or add an entry to models.conf";
            return;
        }
        std::vector<ReasoningSelection> selections;
        (void)config::reasoning_selector_text(context.options.model_catalog,
                                              context.profile.name,
                                              context.api_kind == provider::ApiKind::Responses
                                                  ? "responses"
                                                  : "chat",
                                              context.options.model,
                                              0,
                                              &selections);
        picker_items.clear();
        picker_selected = 0;
        for (size_t i = 0; i < selections.size(); ++i) {
            picker_items.push_back(config::reasoning_selection_value(selections[i]));
            if (selections[i] == context.options.reasoning) picker_selected = i;
        }
        picker_cancel_quits = false;
        mode = TuiMode::ReasoningList;
        history_scroll = 0;
        help_text.clear();
        settings_text.clear();
        status = ui::text_selector_status("Selected reasoning", picker_selected, picker_items.size());
    };

    auto refresh_startup_status = [&]() {
        if (!sqlite_available && !context.options.agent) {
            status = sqlite_unavailable_message();
            return;
        }
        if (context.options.agent) {
            if (agent_runtime && agent_runtime->prepared() &&
                !agent_task_active) {
                agent_activity_state = AgentActivityState::Ready;
                status = agent_ready_with_index_controls();
            } else if (agent_activity_state == AgentActivityState::Unavailable) {
                status = "Agent unavailable";
            } else {
                agent_activity_state = AgentActivityState::Preparing;
                status = "Preparing agent...";
            }
            return;
        }
        status = chat_startup_status(context);
    };

    auto remove_empty_thread_on_exit = [&]() {
        if (!sqlite_available || chat::session_has_chat_messages(session)) {
            return;
        }
        if (file_job.running()) {
            file_job.join();
        }
        TuiEvent event;
        while (events.try_pop(event)) {
            if (event.type == TuiEventType::StoreSaveDone && event.error.ok() &&
                (session.thread_id == 0 || session.thread_id == event.session.thread_id)) {
                session.thread_id = event.session.thread_id;
            }
        }
        if (session.thread_id <= 0) {
            return;
        }
        Error remove_error = sqlite_store.soft_delete_thread(session.thread_id);
        if (!remove_error.ok()) {
            return;
        }
        long long last_id = 0;
        bool found_last = false;
        if (sqlite_store.last_thread_id(last_id, found_last).ok() && found_last &&
            last_id == session.thread_id) {
            sqlite_store.set_last_thread_id(0);
        }
    };

    auto require_provider_model_for_send = [&]() {
        if (!loaded_thread_requires_provider_selection && chat_provider_model_ready(context)) {
            return true;
        }
        status = chat_provider_model_required_status(
            context, loaded_thread_requires_provider_selection);
        return false;
    };

    auto start_assistant_response = [&]() {
        if (session.read_only) {
            status = "Thread is read-only: " + session.read_only_reason;
            return;
        }
        if (!require_provider_model_for_send()) {
            return;
        }
        active_job = ActiveJob::Chat;
        history_scroll = 0;
        pending_assistant = session.messages.size();
        session.messages.push_back({"assistant", ""});
        live_agent_rows.clear();

        // Exclude display-only notice/tool/summary lines from the provider payload.
        std::vector<provider::Message> request_messages =
            app::provider_chat_messages(session.messages);
        if (!request_messages.empty() && request_messages.back().role == "assistant" &&
            request_messages.back().content.empty()) {
            request_messages.pop_back();
        }

        provider::RequestContext job_context = context;
        const std::string media_database_path = sqlite_available ? sqlite_path : std::string();
        const size_t max_image_bytes = context.options.max_image_bytes > 0
                                           ? static_cast<size_t>(context.options.max_image_bytes)
                                           : 0U;
        const size_t max_attachment_bytes = context.options.max_input_bytes > 0
                                                ? static_cast<size_t>(context.options.max_input_bytes)
                                                : 0U;
        // Agent is a separate mode from Chat (options.agent), but both use this
        // TUI shell. Chat sends ordinary completions; Agent runs the tool loop.
        const bool agent_mode = context.options.agent;
        agent::AgentSessionRuntime::UserTurnPayload agent_payload;
        if (agent_mode) {
            for (auto it = request_messages.rbegin(); it != request_messages.rend(); ++it) {
                if (it->role == "user") {
                    agent_payload.text = it->content;
                    agent_payload.images = it->images;
                    // Keep text attachment metadata; expand Markdown in the worker.
                    // (Hydration must not block the TUI thread.)
                    break;
                }
            }
            // Carry text_attachments via a one-message carrier for the worker.
            // agent_payload only holds images + text; attachments live on the
            // last user message inside request_messages until the worker runs.
        }
        // Fallback options if startup prepare failed; normally the runtime is already prepared.
        agent::SessionRuntimeOptions agent_prep_options = make_agent_runtime_options();
        if (agent_mode) {
            agent_activity_state = AgentActivityState::Thinking;
            agent_task_started = std::chrono::steady_clock::now();
            agent_task_active = true;
            agent_completed_task_ms = -1;
            status = "Waiting for provider";
        }
        model_job.start([job_context, request_messages = std::move(request_messages),
                         media_database_path, max_image_bytes, max_attachment_bytes, agent_mode,
                         agent_payload = std::move(agent_payload), agent_runtime,
                         agent_prep_options = std::move(agent_prep_options),
                         &events](runtime::CancellationToken token) mutable {
            provider::ChatResult chat_result;
            Error send_error = ok_error();
            ainiux::context::PreparedMessages prepared;
            agent::SessionTurnResult agent_turn;
            bool have_agent_turn = false;
            if (agent_mode) {
                // Interactive agent turn: multi-turn session runtime (shared tools/DB).
                const std::string model_name = job_context.options.model;
                job_context.options.quiet = true;
                std::shared_ptr<agent::AgentSessionRuntime> runtime = agent_runtime;
                if (!runtime) {
                    send_error = {ErrorCode::Internal, "agent session runtime is missing"};
                } else {
                    // prepare() normally runs at agent-mode entry; re-try only if
                    // that bootstrap failed or the runtime was reset.
                    if (!runtime->prepared()) {
                        send_error =
                            runtime->prepare(job_context, token, {}, agent_prep_options);
                    }
                    // Expand text attachments into the goal so the model sees bodies,
                    // not only the compact "Attached files" list kept for display.
                    if (send_error.ok()) {
                        for (auto it = request_messages.rbegin();
                             it != request_messages.rend(); ++it) {
                            if (it->role != "user" || it->text_attachments.empty()) continue;
                            std::vector<provider::Message> attach_carrier = {*it};
                            send_error = chat::hydrate_message_text_attachments(
                                media_database_path, attach_carrier, max_attachment_bytes,
                                token);
                            if (!send_error.ok()) {
                                // Prefer inline Markdown already on the message.
                                std::string expanded = agent_payload.text;
                                bool any_inline = false;
                                for (const provider::TextAttachment& attachment :
                                     it->text_attachments) {
                                    if (attachment.markdown_content.empty()) continue;
                                    any_inline = true;
                                    if (!expanded.empty()) expanded += "\n\n";
                                    expanded += "---" + attachment.display_name + "---\n";
                                    expanded += attachment.markdown_content;
                                }
                                if (any_inline) {
                                    agent_payload.text = std::move(expanded);
                                    send_error = ok_error();
                                }
                            } else if (!attach_carrier.empty()) {
                                agent_payload.text = std::move(attach_carrier.front().content);
                            }
                            break;
                        }
                    }
                    // Agent attaches are request-local base64. Hydrate unresolved
                    // managed-media refs only as a best-effort bridge from chat.
                    if (send_error.ok()) {
                        for (provider::ImageInput& image : agent_payload.images) {
                            if (!image.base64_data.empty()) continue;
                            if (!image.storage_ref.empty() && !media_database_path.empty()) {
                                std::vector<provider::Message> carrier = {
                                    {"user", "", {image}}};
                                send_error = chat::hydrate_message_images(
                                    media_database_path, carrier, max_image_bytes, token);
                                if (send_error.ok() && !carrier.empty() &&
                                    !carrier.front().images.empty()) {
                                    image = std::move(carrier.front().images.front());
                                }
                            } else {
                                send_error = {
                                    ErrorCode::FileRead,
                                    "image attachment data is unavailable" +
                                        (image.display_name.empty()
                                             ? std::string("; re-attach the file in agent mode")
                                             : ": " + image.display_name +
                                                   "; re-attach the file in agent mode")};
                            }
                            if (!send_error.ok()) break;
                        }
                    }
                    if (send_error.ok()) {
                        // Stream tool activity into the chat panel as each call runs.
                        auto agent_progress = [&events](const std::string& line) {
                            if (line.empty()) return;
                            if (line.rfind("Agent turn ", 0) == 0) return;
                            TuiEvent delta;
                            delta.type = TuiEventType::Delta;
                            delta.text = line;
                            if (delta.text.back() != '\n') delta.text.push_back('\n');
                            events.push(std::move(delta));
                        };
                        auto structured_progress =
                            [&events](const agent::AgentProgressUpdate& update) {
                                TuiEvent event;
                                event.type = TuiEventType::AgentProgress;
                                event.agent_progress = update;
                                events.push(std::move(event));
                            };
                        agent_turn = runtime->run_user_turn(job_context, std::move(agent_payload),
                                                            token, {}, agent_progress,
                                                            structured_progress);
                        have_agent_turn = true;
                        send_error = agent_turn.error;
                        // Fallback single-blob content (Done prefers structured fields).
                        std::string display;
                        for (std::size_t i = 0; i < agent_turn.compact_tool_lines.size(); ++i) {
                            if (!display.empty()) display.push_back('\n');
                            display += agent_turn.compact_tool_lines[i];
                        }
                        if (!agent_turn.final_text.empty()) {
                            if (!display.empty()) display.push_back('\n');
                            display += agent_turn.final_text;
                        } else if (!send_error.ok() && !send_error.message.empty()) {
                            // Surface abort/policy reasons instead of a blank failure.
                            if (!display.empty()) display.push_back('\n');
                            display += send_error.message;
                        }
                        chat_result.content = display;
                        chat_result.model = model_name;
                    }
                }
            } else {
                send_error = chat::hydrate_message_text_attachments(
                    media_database_path, request_messages, max_attachment_bytes, token);
                if (send_error.ok()) {
                    prepared = ainiux::context::prepare(
                        request_messages,
                        job_context.options.context_policy,
                        job_context.options.max_context_bytes > 0
                            ? static_cast<size_t>(job_context.options.max_context_bytes)
                            : 0U);
                    send_error = prepared.error;
                }
                if (send_error.ok()) {
                    send_error = chat::hydrate_message_images(
                        media_database_path, prepared.messages, max_image_bytes, token);
                }
                if (send_error.ok()) {
                    send_error = provider::send_chat_messages(
                        job_context,
                        prepared.messages,
                    [&](const std::string& delta) -> Error {
                        TuiEvent event;
                        event.type = TuiEventType::Delta;
                        event.text = delta;
                        events.push(std::move(event));
                        if (token.cancelled()) {
                            return {ErrorCode::Cancelled, "chat request cancelled while streaming"};
                        }
                        return ok_error();
                    },
                    chat_result,
                    token);
                }
            }
            TuiEvent event;
            if (send_error.ok()) {
                event.type = TuiEventType::Done;
                event.chat = std::move(chat_result);
                event.compaction = std::move(prepared.event);
                event.compacted = prepared.compacted;
                if (have_agent_turn) {
                    event.agent_turn = true;
                    event.agent_tool_lines = std::move(agent_turn.compact_tool_lines);
                    event.agent_tool_line_ms = std::move(agent_turn.compact_tool_line_ms);
                    event.agent_final_text = std::move(agent_turn.final_text);
                    event.agent_needs_user_continue = agent_turn.needs_user_continue;
                    event.agent_turn_started_ms = agent_turn.turn_started_ms;
                    event.agent_finished_at_ms = agent_turn.finished_at_ms;
                }
            } else {
                event.type = TuiEventType::Error;
                event.error = send_error;
                event.chat = std::move(chat_result);
                // Keep agent tool lines / failure text for the transcript even on error.
                if (have_agent_turn) {
                    event.agent_turn = true;
                    event.agent_tool_lines = std::move(agent_turn.compact_tool_lines);
                    event.agent_tool_line_ms = std::move(agent_turn.compact_tool_line_ms);
                    event.agent_final_text = !agent_turn.final_text.empty()
                                                ? std::move(agent_turn.final_text)
                                                : send_error.message;
                    if (event.agent_final_text.empty() && !agent_turn.notice.empty())
                        event.agent_final_text = agent_turn.notice;
                    event.agent_turn_started_ms = agent_turn.turn_started_ms;
                    event.agent_finished_at_ms = agent_turn.finished_at_ms > 0
                                                    ? agent_turn.finished_at_ms
                                                    : agent::now_unix_ms();
                }
            }
            events.push(std::move(event));
        });
        status = agent_mode ? "Waiting for provider"
                            : "Waiting for response...";
    };

    auto start_turn_with_payload = [&](const std::string& history_content,
                                       const std::vector<provider::ImageInput>& images,
                                       const std::vector<provider::TextAttachment>& text_attachments,
                                       size_t pending_image_count) -> bool {
        if (context.options.agent &&
            (!agent_runtime || !agent_runtime->prepared())) {
            status = file_job.running() ? "Agent is still preparing..."
                                        : "Agent is not ready";
            return false;
        }
        if (session.read_only) {
            status = "Thread is read-only: " + session.read_only_reason;
            return false;
        }
        if (!require_provider_model_for_send()) {
            return false;
        }
        if (active_job != ActiveJob::None) {
            status = "A model job is already running";
            return false;
        }
        pending_user = session.messages.size();
        pending_user_added_for_job = true;
        inflight_image_count = pending_image_count;
        provider::Message user_msg{"user", history_content, images, text_attachments};
        user_msg.created_at_ms = agent::now_unix_ms();
        session.messages.push_back(std::move(user_msg));
        start_assistant_response();
        return true;
    };

    auto start_turn_with_pending_attachments = [&](const std::string& history_content) {
        std::vector<provider::TextAttachment> text_attachments;
        text_attachments.reserve(chat_attachments.size());
        for (const ChatAttachment& attachment : chat_attachments) {
            text_attachments.push_back(attachment.attachment);
        }
        return start_turn_with_payload(history_content, pending_images, text_attachments,
                                       pending_images.size());
    };

    // One-arg version for call sites and callbacks that don't involve attachment bodies.
    auto start_turn = [&](const std::string& history_content) {
        return start_turn_with_payload(history_content, {}, {}, 0);
    };

    auto start_response_to_unanswered_user = [&]() {
        if (active_job != ActiveJob::None) {
            status = "A model job is already running";
            return;
        }
        size_t user_index = 0;
        if (!last_unanswered_user_message(session, user_index)) {
            status = "No unanswered user message to respond to";
            return;
        }
        pending_user = user_index;
        pending_user_added_for_job = false;
        inflight_image_count = 0;
        attachments_committed_for_turn = 0;
        start_assistant_response();
    };

    auto clear_queued_regeneration = [&]() {
        regenerate_after_cancel = false;
        queued_regeneration_prompt.clear();
        queued_regen_images.clear();
        queued_regen_text_attachments.clear();
    };

    auto start_queued_regeneration = [&](size_t erase_from) {
        const std::string prompt = queued_regeneration_prompt;
        const std::vector<provider::ImageInput> images = std::move(queued_regen_images);
        const std::vector<provider::TextAttachment> text_attachments =
            std::move(queued_regen_text_attachments);
        clear_queued_regeneration();
        if (app::detail::trim_ascii(prompt).empty()) {
            status = "No previous user prompt to regenerate";
            return;
        }
        if (erase_from != static_cast<size_t>(-1) && erase_from < session.messages.size()) {
            session.messages.erase(session.messages.begin() + static_cast<long>(erase_from), session.messages.end());
        }
        attachments_committed_for_turn = 0;
        // Use stashed full content (if any) for the model; history will get the (short) prompt.
        start_turn_with_payload(prompt, images, text_attachments, 0);
        status = "Regenerating...";
    };

    auto cancel_active_request = [&]() {
        if (active_job == ActiveJob::None) {
            return;
        }
        clear_queued_regeneration();
        if (agent_approval_gate) agent_approval_gate->cancel_pending();
        if (mode == TuiMode::GuardApprovalConfirm) {
            mode = TuiMode::Chat;
            have_pending_guard_request = false;
        }
        model_job.cancel();
        status = "Cancelling...";
    };

    auto start_history_edit = [&]() {
        if (session.read_only) {
            status = "Thread is read-only: " + session.read_only_reason;
            return;
        }
        if (active_job != ActiveJob::None) {
            status = "Cannot edit history while a job is running";
            return;
        }
        if (mode != TuiMode::Chat) {
            return;
        }
        size_t index = 0;
        if (!last_editable_chat_message(session, index)) {
            status = "No user or assistant message to edit";
            return;
        }
        input = editor::EditorState::from_text(session.messages[index].content);
        input.set_undo_limit(input_undo_limit);
        input.mode = editor::EditorMode::Chat;
        input.vertical_movement = editor::VerticalMovementMode::VisualRow;
        history_edit_index = index;
        mode = TuiMode::HistoryEdit;
        status = "Editing last " + session.messages[index].role + " message";
    };

    auto pop_last_message = [&]() {
        if (session.read_only) {
            status = "Thread is read-only: " + session.read_only_reason;
            return;
        }
        if (active_job != ActiveJob::None) {
            status = "Cannot pop while a model job is running";
            return;
        }
        std::string removed_role;
        if (!pop_last_chat_message(session, removed_role)) {
            status = "No user or assistant message to pop";
            return;
        }
        history_scroll = 0;
        status = "Popped last " + removed_role + " message";
        start_save(context.options.save_chat_path, session, true);
        start_store_save();
    };

    // True while chat startup is waiting for the user to pick New or a saved
    // thread (Ctrl+L-style selector). Cleared once that choice is finished.
    bool thread_list_from_startup = false;
    // Assigned after provider/model startup helpers exist; used by thread-list
    // callbacks that are wired earlier in this function.
    std::function<void()> continue_after_startup_thread_choice;

    auto start_thread_list = [&]() {
        if (!sqlite_available) {
            status = sqlite_unavailable_message();
            return;
        }
        if (active_job != ActiveJob::None) {
            status = "Cannot list threads while a model job is running";
            return;
        }
        Error list_error = sqlite_store.list_threads(thread_picker_threads, 200);
        if (!list_error.ok()) {
            set_status_maybe_agent_error(detail::error_line(list_error), true);
            return;
        }
        thread_picker_selected = 0;
        pending_thread_delete = static_cast<size_t>(-1);
        mode = TuiMode::ThreadList;
        history_scroll = 0;
        help_text.clear();
        settings_text.clear();
        if (thread_picker_threads.empty()) {
            status = context.options.agent
                         ? "No saved threads · Esc continues"
                         : "No saved threads · Tab/Insert new · Esc continues";
        } else {
            status = ui::text_selector_status("Selected thread", thread_picker_selected,
                                              thread_picker_threads.size());
        }
    };

    auto start_new_chat_thread = [&](const std::string& name = "") -> bool {
        if (active_job != ActiveJob::None) {
            status = "Cannot create a thread while a model job is running";
            return false;
        }
        if (file_job.joinable()) {
            status = "Cannot create a thread while a file job is running";
            return false;
        }
        start_new_thread_from_cli();
        session.name = app::detail::trim_ascii(name);
        status = session.name.empty() ? "New chat thread" : "New chat thread: " + session.name;
        start_store_save();
        return true;
    };

    auto regenerate_last_turn = [&]() {
        if (session.read_only) {
            status = "Thread is read-only: " + session.read_only_reason;
            return;
        }
        if (!require_provider_model_for_send()) {
            return;
        }
        if (active_job == ActiveJob::Models) {
            status = "Cannot regenerate while listing models";
            return;
        }
        if (active_job == ActiveJob::Chat) {
            std::string prompt;
            if (pending_user != static_cast<size_t>(-1) && pending_user < session.messages.size() &&
                session.messages[pending_user].role == "user") {
                prompt = session.messages[pending_user].content;
            } else {
                const RegenerationPlan plan = regeneration_plan_for_session(session);
                if (plan.available) {
                    prompt = plan.prompt;
                }
            }
            if (app::detail::trim_ascii(prompt).empty()) {
                status = "No previous user prompt to regenerate";
                return;
            }
            regenerate_after_cancel = true;
            queued_regeneration_prompt = prompt;
            if (pending_user != static_cast<size_t>(-1) && pending_user < session.messages.size()) {
                queued_regen_images = session.messages[pending_user].images;
                queued_regen_text_attachments =
                    session.messages[pending_user].text_attachments;
            }
            model_job.cancel();
            status = "Cancelling before regenerate...";
            return;
        }

        const RegenerationPlan plan = regeneration_plan_for_session(session);
        if (!plan.available || app::detail::trim_ascii(plan.prompt).empty()) {
            status = "No previous user prompt to regenerate";
            return;
        }
        const std::vector<provider::ImageInput> images =
            plan.erase_from < session.messages.size()
                ? session.messages[plan.erase_from].images
                : std::vector<provider::ImageInput>();
        const std::vector<provider::TextAttachment> text_attachments =
            plan.erase_from < session.messages.size()
                ? session.messages[plan.erase_from].text_attachments
                : std::vector<provider::TextAttachment>();
        session.messages.erase(session.messages.begin() + static_cast<long>(plan.erase_from), session.messages.end());
        start_turn_with_payload(plan.prompt, images, text_attachments, 0);
        status = "Regenerating...";
    };

    chat_assist_callbacks.start_turn = start_turn;  // one-arg version (no attachment bodies)
    chat_assist_callbacks.regenerate_last_turn = regenerate_last_turn;
    chat_assist_callbacks.start_store_save = start_store_save;
    chat_assist_callbacks.switch_to_editor_new_buffer_assist =
        [&](const ChatEditorNewBufferAssist& pending) {
            if (interactive == nullptr) {
                status = "Editor mode is unavailable";
                return false;
            }
            if (active_job != ActiveJob::None) {
                status = "Cannot switch to editor while a model job is running";
                return false;
            }
            interactive->pending_editor_assist.active = true;
            interactive->pending_editor_assist.command_index = pending.command_index;
            interactive->pending_editor_assist.selection_text = pending.selection_text;
            leave_target = app::InteractiveUiTarget::Editor;
            quit = true;
            return true;
        };

    auto commit_reasoning_selection = [&](const std::string& reasoning) {
        Error err = chat::apply_chat_setting(context.options, "reasoning", reasoning);
        if (!err.ok()) {
            mode = TuiMode::Chat;
            status = err.message;
            return;
        }
        picker_items.clear();
        picker_selected = 0;
        mode = TuiMode::Chat;
        std::string message = "Reasoning set to " +
            config::reasoning_selection_value(context.options.reasoning);
        const std::string advisory = provider::reasoning_temperature_advisory(context);
        if (!advisory.empty()) message += ". Warning: " + advisory;
        persist_settings_change(message);
        refresh_settings_panel_if_visible();
    };

    auto cycle_reasoning = [&]() {
        if (context.options.model.empty()) return;
        if (active_job != ActiveJob::None) {
            status = "Cannot change reasoning while a model job is running";
            return;
        }
        ReasoningSelection next;
        if (!config::next_reasoning_selection(
                context.options.model_catalog,
                context.profile.name,
                context.api_kind == provider::ApiKind::Responses ? "responses" : "chat",
                context.options.model,
                context.options.reasoning,
                next)) {
            return;
        }
        commit_reasoning_selection(config::reasoning_selection_value(next));
    };

    auto launch_agent_project_new = [&](agent::NewProjectTarget target,
                                        bool indexing_enabled) {
        if (!context.options.agent || !agent_runtime || !agent_runtime->prepared()) {
            report_agent_error("Agent project runtime is unavailable");
            return;
        }
        if (active_job != ActiveJob::None) {
            status = "Cannot create a project while an agent job is running; wait or cancel it first";
            return;
        }
        if (file_job.joinable()) {
            status = "Cannot create a project while an agent file job is running; wait or cancel it first";
            return;
        }

        provider::RequestContext job_context = context;
        job_context.options.quiet = true;
        context.options.disable_indexing = !indexing_enabled;
        // Clear the old project immediately. Keep a UI-only snapshot so a failed
        // initialization can restore it even if reopening the old DB also fails.
        project_switch_previous_history = std::move(session.messages);
        session.messages.clear();
        pending_user = static_cast<size_t>(-1);
        pending_assistant = static_cast<size_t>(-1);
        pending_user_added_for_job = false;
        history_edit_index = static_cast<size_t>(-1);
        history_scroll = history_scroll_for_thread_end();
        help_text.clear();
        settings_text.clear();
        active_job = ActiveJob::Chat;
        model_job.start(
            [runtime = agent_runtime, target = std::move(target),
             job_context = std::move(job_context), indexing_enabled,
             &events](runtime::CancellationToken token) mutable {
                TuiEvent event;
                event.type = TuiEventType::AgentProjectNewDone;
                event.agent_workspace = target.root;
                const agent::SessionProjectReplaceResult replaced =
                    runtime->replace_project(job_context, target, token,
                                             indexing_enabled);
                event.error = replaced.error;
                event.text = replaced.warning;
                if (!replaced.workspace.empty())
                    event.agent_workspace = replaced.workspace;
                if (replaced.error.ok()) {
                    event.agent_index_enabled = runtime->indexing_enabled();
                    event.error = runtime->load_display_messages(
                        event.agent_history);
                    if (!event.error.ok())
                        event.error.message =
                            "fresh project initialized at " + event.agent_workspace +
                            ", but its transcript could not be loaded: " +
                            event.error.message;
                }
                events.push(std::move(event));
            });
        status = "Initializing fresh agent project...";
    };

    auto start_new_agent_project = [&](const std::string& requested_path) {
        if (!context.options.agent || !agent_runtime || !agent_runtime->prepared()) {
            report_agent_error("Agent project runtime is unavailable");
            return;
        }
        agent::NewProjectTarget target;
        Error target_error = agent::resolve_new_project_target(
            agent_runtime->workspace(), requested_path, target);
        if (!target_error.ok()) {
            report_agent_error(target_error.message);
            return;
        }
        if (target.state_dir_exists) {
            pending_new_project = std::move(target);
            have_pending_new_project = true;
            help_text.clear();
            settings_text.clear();
            history_scroll = 0;
            mode = TuiMode::AgentNewConfirm;
            status = "Reset agent project? Default is No · y/n";
            return;
        }
        pending_new_project = std::move(target);
        have_pending_new_project = true;
        agent::NewProjectTarget fresh_target =
            std::move(pending_new_project);
        pending_new_project = {};
        have_pending_new_project = false;
        mode = TuiMode::Chat;
        launch_agent_project_new(std::move(fresh_target), true);
    };

    auto start_agent_compaction =
        [&](std::optional<CompactionStrategy> strategy) {
        if (!context.options.agent || !agent_runtime || !agent_runtime->prepared()) {
            report_agent_error("Agent project runtime is unavailable");
            return;
        }
        if (active_job != ActiveJob::None) {
            status = "Cannot compact while an agent job is running; wait or cancel it first";
            return;
        }
        if (file_job.joinable()) {
            status = "Cannot compact while an agent file job is running; wait or cancel it first";
            return;
        }
        provider::RequestContext job_context = context;
        job_context.options.quiet = true;
        const CompactionStrategy requested_strategy =
            strategy.value_or(context.options.agent_compact_strategy);
        agent_compaction_strategy = requested_strategy;
        const std::string starting_notice =
            "Compacting context using " +
            std::string(agent::compaction_strategy_name(requested_strategy)) +
            "...";
        append_agent_history_notice(starting_notice);
        agent_compaction_active = true;
        agent_activity_state = AgentActivityState::Compacting;
        agent_task_started = std::chrono::steady_clock::now();
        agent_task_active = true;
        agent_completed_task_ms = -1;
        status = agent::format_compaction_progress(requested_strategy, 0);
        active_job = ActiveJob::Chat;
        model_job.start(
            [runtime = agent_runtime, job_context = std::move(job_context), strategy,
             &events](runtime::CancellationToken token) mutable {
                TuiEvent event;
                event.type = TuiEventType::AgentCompactDone;
                const agent::SessionCompactionResult compacted =
                    runtime->compact(job_context, agent::CompactionReason::Manual,
                                     token, strategy);
                event.error = compacted.error;
                event.agent_compacted = compacted.compacted;
                event.agent_compact_no_op = compacted.no_op;
                event.agent_compact_requested = compacted.requested_strategy;
                event.agent_compact_applied = compacted.applied_strategy;
                event.text =
                    compacted.error.ok()
                        ? compacted.notice
                        : agent::format_compaction_failure_notice(
                              compacted.error.message);
                const Error notice_error =
                    runtime->append_display_notice(event.text);
                if (event.error.ok() && !notice_error.ok())
                    event.error = notice_error;
                const Error history_error =
                    runtime->load_display_messages(event.agent_history);
                event.agent_history_loaded = history_error.ok();
                if (event.error.ok() && !history_error.ok())
                    event.error = history_error;
                events.push(std::move(event));
            });
    };

    auto start_agent_index_code = [&]() {
        if (!context.options.agent || !agent_runtime ||
            !agent_runtime->prepared()) {
            report_agent_error("Agent project runtime is unavailable");
            return;
        }
        if (active_job != ActiveJob::None) {
            status =
                "Cannot index code while an agent job is running; wait or cancel it first";
            return;
        }
        if (file_job.joinable()) {
            status =
                "Cannot index code while an agent file job is running; wait or cancel it first";
            return;
        }
        status = agent_runtime->indexing_enabled()
                     ? "Refreshing code index..."
                     : "Creating code index...";
        file_job.start(
            [runtime = agent_runtime,
             &events](runtime::CancellationToken token) {
                TuiEvent event;
                event.type = TuiEventType::AgentIndexReportDone;
                const agent::SessionIndexReportResult report =
                    runtime->index_code(token);
                event.error = report.error;
                event.agent_index_enabled = report.indexing_enabled;
                if (event.error.ok()) {
                    event.error =
                        runtime->load_display_messages(event.agent_history);
                    event.agent_history_loaded = event.error.ok();
                    // Prefer the history intro line (includes fractional seconds);
                    // fall back if the report did not carry markdown.
                    if (!report.markdown.empty()) {
                        const std::size_t newline =
                            report.markdown.find('\n');
                        event.text = newline == std::string::npos
                                         ? report.markdown
                                         : report.markdown.substr(0, newline);
                    } else {
                        event.text =
                            (report.created ? "Code index created in "
                                            : "Code index refreshed in ") +
                            std::to_string(std::max(0LL, report.elapsed_ms)) +
                            " ms";
                    }
                }
                events.push(std::move(event));
            });
    };

    // Offer a one-shot Yes/No to create the project index when prepare finds none.
    // Deferred while provider/model setup pickers are open so the prompt is not lost.
    bool pending_index_build_offer = false;
    auto open_index_build_offer = [&]() {
        if (!context.options.agent || !agent_runtime ||
            !agent_runtime->prepared())
            return;
        if (context.options.disable_indexing) return;
        if (agent_runtime->indexing_enabled()) return;
        if (active_job != ActiveJob::None || file_job.joinable()) return;
        if (mode != TuiMode::Chat) {
            pending_index_build_offer = true;
            return;
        }
        pending_index_build_offer = false;
        help_text.clear();
        settings_text.clear();
        mode = TuiMode::AgentIndexBuildConfirm;
        status = "Build code index? Default is No · y/n";
    };
    auto flush_pending_index_build_offer = [&]() {
        if (!pending_index_build_offer) return;
        if (mode != TuiMode::Chat) return;
        if (active_job != ActiveJob::None || file_job.joinable()) return;
        open_index_build_offer();
    };

    auto start_agent_show_index = [&]() {
        if (!context.options.agent || !agent_runtime ||
            !agent_runtime->prepared()) {
            report_agent_error("Agent project runtime is unavailable");
            return;
        }
        if (active_job != ActiveJob::None) {
            status =
                "Cannot show the index while an agent job is running; wait or cancel it first";
            return;
        }
        if (file_job.joinable()) {
            status =
                "Cannot show the index while an agent file job is running; wait or cancel it first";
            return;
        }
        status = "Refreshing code index...";
        file_job.start(
            [runtime = agent_runtime,
             &events](runtime::CancellationToken token) {
                TuiEvent event;
                event.type = TuiEventType::AgentIndexReportDone;
                const agent::SessionIndexReportResult report =
                    runtime->show_index(true, token);
                event.error = report.error;
                event.agent_index_enabled = report.indexing_enabled;
                if (event.error.ok()) {
                    event.error =
                        runtime->load_display_messages(event.agent_history);
                    event.agent_history_loaded = event.error.ok();
                    event.text = "Code index refreshed";
                }
                events.push(std::move(event));
            });
    };

    TuiCommandHandlers command_handlers;
    command_handlers.quit = [&]() { quit = true; };
    command_handlers.start_history_edit = start_history_edit;
    command_handlers.start_thread_list = start_thread_list;
    command_handlers.start_new_chat_thread = [&](const std::string& name) { start_new_chat_thread(name); };
    command_handlers.start_new_agent_project = start_new_agent_project;
    command_handlers.start_agent_compaction = start_agent_compaction;
    command_handlers.start_agent_index_code = start_agent_index_code;
    command_handlers.start_agent_show_index = start_agent_show_index;
    command_handlers.switch_agent_task_mode = [&](agent::AgentTaskMode mode) {
        if (!agent_runtime || !agent_runtime->prepared()) {
            status = "Agent session runtime is not ready";
            return;
        }
        if (file_job.joinable()) {
            status =
                "Cannot switch task mode while an agent file job is running; wait or cancel it first";
            return;
        }
        const agent::AgentTaskMode before = agent_runtime->task_mode();
        const Error error = agent_runtime->switch_task_mode(mode);
        if (!error.ok()) {
            status = error.message;
            return;
        }
        status = before == mode
                     ? std::string("Already in ") + agent::agent_task_mode_name(mode) + " mode"
                     : std::string("Switched agent task mode to ") +
                           agent::agent_task_mode_name(mode);
    };
    command_handlers.switch_agent_permission_mode =
        [&](const std::string& requested) {
            if (!agent_runtime || !agent_runtime->prepared()) {
                status = "Agent session runtime is not ready";
                return;
            }
            if (file_job.joinable() ||
                (agent_approval_gate && agent_approval_gate->has_pending())) {
                status =
                    "Cannot switch permissions while an agent operation or approval is active";
                return;
            }
            agent::PermissionMode mode = agent::PermissionMode::Smart;
            if (!agent::parse_permission_mode(requested, mode)) {
                status = "Usage: /permissions [confirm|smart|yolo]";
                return;
            }
            const agent::PermissionMode before =
                agent_runtime->permission_mode();
            const Error error =
                agent_runtime->switch_permission_mode(mode, context);
            if (!error.ok()) {
                status = "Permission change failed; mode remains " +
                         std::string(agent::permission_mode_name(before)) + ": " +
                         error.message;
                return;
            }
            status = before == mode
                         ? "Permissions already " +
                               std::string(agent::permission_mode_name(mode))
                         : "Permissions set to " +
                               std::string(agent::permission_mode_name(mode));
        };
    command_handlers.open_agent_permission_picker = [&]() {
        mode = TuiMode::AgentPermissionSelect;
        status = "Select permissions";
    };
    command_handlers.show_agent_goal_status = [&]() {
        if (!agent_runtime || !agent_runtime->prepared()) {
            status = "Agent session runtime is not ready";
            return;
        }
        status = agent::format_goal_status(agent_runtime->goal());
    };
    command_handlers.set_agent_goal = [&](const std::string& condition) {
        if (!agent_runtime || !agent_runtime->prepared()) {
            status = "Agent session runtime is not ready";
            return;
        }
        if (file_job.joinable()) {
            status =
                "Cannot set a goal while an agent file job is running; wait or cancel it first";
            return;
        }
        if (active_job != ActiveJob::None) {
            status = "Cannot set a goal while an agent job is running; wait or cancel it first";
            return;
        }
        if (!require_provider_model_for_send()) return;
        const Error error = agent_runtime->set_goal(condition);
        if (!error.ok()) {
            status = error.message;
            return;
        }
        status = agent::format_goal_status(agent_runtime->goal());
        // Setting a goal is the directive for the current turn.
        if (!start_turn(condition)) {
            status = "Goal saved but could not start the agent turn";
        }
    };
    command_handlers.clear_agent_goal = [&]() {
        if (!agent_runtime || !agent_runtime->prepared()) {
            status = "Agent session runtime is not ready";
            return;
        }
        const Error error = agent_runtime->clear_goal();
        status = error.ok() ? "Goal cleared" : error.message;
    };
    command_handlers.pause_agent_goal = [&]() {
        if (!agent_runtime || !agent_runtime->prepared()) {
            status = "Agent session runtime is not ready";
            return;
        }
        const Error error = agent_runtime->pause_goal();
        status = error.ok() ? agent::format_goal_status(agent_runtime->goal())
                            : error.message;
    };
    command_handlers.resume_agent_goal = [&]() {
        if (!agent_runtime || !agent_runtime->prepared()) {
            status = "Agent session runtime is not ready";
            return;
        }
        if (file_job.joinable() || active_job != ActiveJob::None) {
            status =
                "Cannot resume the goal while an agent job is running; wait or cancel it first";
            return;
        }
        if (!require_provider_model_for_send()) return;
        const Error error = agent_runtime->resume_goal();
        if (!error.ok()) {
            status = error.message;
            return;
        }
        status = agent::format_goal_status(agent_runtime->goal());
        if (!start_turn("Continue the active goal.")) {
            status = "Goal resumed but could not start the agent turn";
        }
    };
    command_handlers.open_provider_picker = open_provider_picker;
    command_handlers.apply_selected_provider = [&](const std::string& provider_target) {
        const bool applied =
            apply_selected_provider(context, session, show_thinking_traces, provider_target, status);
        if (applied) {
            loaded_thread_requires_provider_selection = false;
            if (context.options.agent && agent_runtime && agent_runtime->prepared()) {
                const Error error = agent_runtime->update_project_settings(context);
                if (!error.ok()) status = "Agent settings save failed: " + error.message;
            } else if (ensure_agent_prepare) {
                ensure_agent_prepare();
            }
            refresh_credit_balance();
        }
        return applied;
    };
    command_handlers.start_store_save = start_store_save;
    command_handlers.start_models = start_models;
    command_handlers.refresh_model_context =
        refresh_model_context;
    command_handlers.open_reasoning_picker = open_reasoning_picker;
    command_handlers.request_reasoning_confirmation =
        [&](const std::string& reasoning, const std::string& warning) {
            pending_reasoning = reasoning;
            pending_reasoning_warning = warning;
            help_text.clear();
            settings_text.clear();
            history_scroll = 0;
            mode = TuiMode::ReasoningConfirm;
            status = "Warning: " + warning + ". Proceed? y/n";
        };
    command_handlers.persist_settings_change = persist_settings_change;
    command_handlers.refresh_settings_panel_if_visible = refresh_settings_panel_if_visible;
    command_handlers.start_save = [&](const std::string& path) { start_save(path, session); };
    command_handlers.start_load = [&](const std::string& path) { file_jobs.start_load(path); };
    command_handlers.pop_last_message = pop_last_message;
    command_handlers.start_response_to_unanswered_user = start_response_to_unanswered_user;
    command_handlers.start_insert = [&](const std::string& path) { file_jobs.start_insert(path); };
    command_handlers.start_attach = [&](const std::string& path) {
        if (path.empty()) {
            if (active_job != ActiveJob::None) {
                status = "Cannot manage attachments while a model job is running";
                return;
            }
            if (chat_attachments.empty()) {
                status = "No attachments. Use /attach PATH or URL to add one.";
                return;
            }
            attachment_picker_selected = 0;
            mode = TuiMode::AttachmentList;
            history_scroll = 0;
            status = ui::text_selector_status("Selected attachment", attachment_picker_selected,
                                              chat_attachments.size());
            return;
        }
        file_jobs.start_attach(path);
    };
    command_handlers.start_fetch = [&](const std::string& url) { file_jobs.start_fetch(url); };
    command_handlers.start_search = [&](const std::string& query) { file_jobs.start_search(query); };
    command_handlers.start_shell = [&](const std::string& command, bool to_draft) {
        file_jobs.start_shell(command, to_draft);
    };
    command_handlers.start_media_cleanup = [&]() {
        file_jobs.start_media_cleanup(context.options.media_expiration_days,
                                      session.thread_id, false);
    };
    command_handlers.set_thinking_trace_mode = set_thinking_trace_mode;
    auto leave_for = [&](app::InteractiveUiTarget target) {
        if (active_job != ActiveJob::None) {
            status = "Cannot switch mode while a model job is running";
            return;
        }
        if (target == app::InteractiveUiTarget::Editor && interactive == nullptr) {
            status = "Editor mode is unavailable";
            return;
        }
        // Leaving agent: finish project session without tearing down chat DB.
        if (context.options.agent && agent_runtime && agent_runtime->prepared() &&
            agent_runtime->session_id() > 0) {
            (void)agent_runtime->finish_session("cancelled", "", "Cancelled",
                                                "left agent mode");
        }
        leave_target = target;
        quit = true;
    };
    command_handlers.switch_to_editor = [&]() { leave_for(app::InteractiveUiTarget::Editor); };
    command_handlers.switch_to_chat = [&]() {
        if (!context.options.agent) {
            status = "Already in chat mode";
            return;
        }
        leave_for(app::InteractiveUiTarget::Chat);
    };
    command_handlers.switch_to_agent = [&]() {
        if (context.options.agent) {
            status = "Already in agent mode · tools enabled for this workspace";
            return;
        }
        leave_for(app::InteractiveUiTarget::Agent);
    };
    command_handlers.cycle_mode = [&]() {
        if (interactive != nullptr) {
            leave_for(app::InteractiveUiTarget::Editor);
        } else {
            status = "Editor mode is unavailable";
        }
    };

    TuiCommandContext command_context{context,
                                      session,
                                      input,
                                      status,
                                      help_text,
                                      settings_text,
                                      history_scroll,
                                      show_thinking_traces,
                                      syntax_highlight,
                                      context.options.tui_themes,
                                      theme,
                                      use_colors,
                                      active_job,
                                      mode,
                                      input_undo_limit,
                                      sqlite_available,
                                      sqlite_store,
                                      sqlite_unavailable_message,
                                      pending_images,
                                      inflight_image_count,
                                      chat_attachments,
                                      attachments_committed_for_turn};

    auto handle_command = [&](const std::string& text) {
        if (context.options.agent) {
            static const std::vector<std::string> chat_only = {
                "/clone",        "/cleanup", "/remove", "/remove-empty",
                "/pop",          "/load",    "/save",   "/prompt",
                "/regenerate"};
            for (const std::string& command : chat_only) {
                if (text == command || text.rfind(command + " ", 0) == 0) {
                    status = command +
                             " is unavailable in Agent mode; switch to Chat with /chat";
                    return;
                }
            }
            for (const editor::EditorAssistCommand& command :
                 ai_continue.assist_config.commands) {
                if (text == command.command ||
                    text.rfind(command.command + " ", 0) == 0) {
                    status = command.command +
                             " is a Chat/editor AI command and is unavailable in Agent mode";
                    return;
                }
            }
        }
        if (loaded_thread_requires_provider_selection &&
            (text == "/model" || text.rfind("/model ", 0) == 0)) {
            status = "Thread setup requires /provider first; model selection follows";
            return;
        }
        handle_tui_command(text, command_context, command_handlers);
        // Agent mode: durable actionable failures belong in history, not only the status line.
        // Leave "Usage:" hints on the status line.
        if (context.options.agent && !status.empty() && status.rfind("Usage:", 0) != 0 &&
            detail::status_role_for_text(status) == StyleRole::Error) {
            const std::string err = status;
            append_agent_history_notice(err);
            status = agent_runtime && agent_runtime->prepared()
                         ? agent_ready_with_index_controls()
                         : "Preparing agent...";
        }
    };

    TuiPickerCallbacks picker_callbacks;
    picker_callbacks.on_provider_selected = [&](const std::string& provider_name) {
        if (apply_selected_provider(context, session, show_thinking_traces, provider_name, status)) {
            loaded_thread_requires_provider_selection = false;
            picker_items.clear();
            picker_selected = 0;
            mode = TuiMode::Chat;
            status = "Provider set to " + provider::display_name_for_profile(context.profile.name);
            start_store_save();
            if (context.options.agent && agent_runtime && agent_runtime->prepared()) {
                const Error error = agent_runtime->update_project_settings(context);
                if (!error.ok()) status = "Agent settings save failed: " + error.message;
            } else if (ensure_agent_prepare) {
                ensure_agent_prepare();
            }
            refresh_credit_balance();
            start_models(ModelsRequestPurpose::Picker);
        }
    };
    picker_callbacks.on_model_selected = [&](const std::string& model_name) {
        const bool changed = context.options.model != model_name;
        context.options.model = model_name;
        session.model = model_name;
        if (changed) {
            if (!context.options.has_context_tokens) {
                context.options.context_tokens = 0;
            }
            context.options.reasoning = ReasoningSelection::automatic();
            context.options.reasoning_explicit = true;
        }
        loaded_thread_requires_provider_selection = false;
        if (have_cached_models && !context.options.has_context_tokens) {
            provider::apply_context_window_from_models(context, cached_models, model_name);
        }
        picker_items.clear();
        picker_selected = 0;
        mode = TuiMode::Chat;
        status = provider_model_status_message(context, "ready");
        start_store_save();
        if (context.options.agent && agent_runtime && agent_runtime->prepared()) {
            const Error error = agent_runtime->update_project_settings(context);
            if (!error.ok()) status = "Agent settings save failed: " + error.message;
        }
        flush_pending_index_build_offer();
    };
    picker_callbacks.on_reasoning_selected = [&](const std::string& reasoning) {
        commit_reasoning_selection(reasoning);
    };
    picker_callbacks.on_reasoning_confirm_accepted = [&]() {
        const std::string reasoning = pending_reasoning;
        pending_reasoning.clear();
        pending_reasoning_warning.clear();
        commit_reasoning_selection(reasoning);
    };
    picker_callbacks.on_reasoning_confirm_rejected = [&]() {
        pending_reasoning.clear();
        pending_reasoning_warning.clear();
        mode = TuiMode::Chat;
        status = "Reasoning change cancelled";
    };
    picker_callbacks.on_reasoning_confirm_retry =
        [&](const std::string& message) { status = message; };
    picker_callbacks.on_thread_selected = [&](long long thread_id) {
        mode = TuiMode::Chat;
        thread_picker_threads.clear();
        thread_picker_selected = 0;
        pending_thread_delete = static_cast<size_t>(-1);
        // Keep thread_list_from_startup until StoreLoadDone so a loaded
        // thread can supply provider/model before any CLI setup pickers.
        start_store_load(thread_id);
    };
    picker_callbacks.on_thread_new = [&]() {
        if (start_new_chat_thread()) {
            mode = TuiMode::Chat;
            thread_picker_threads.clear();
            thread_picker_selected = 0;
            pending_thread_delete = static_cast<size_t>(-1);
            history_scroll = 0;
            if (continue_after_startup_thread_choice) {
                continue_after_startup_thread_choice();
            }
        }
    };
    picker_callbacks.on_thread_list_cancelled = [&]() {
        if (continue_after_startup_thread_choice) {
            continue_after_startup_thread_choice();
        }
    };
    picker_callbacks.on_remove_accepted = [&]() {
        const long long removed_thread_id = session.thread_id;
        Error remove_error = sqlite_store.soft_delete_thread(removed_thread_id);
        if (remove_error.ok()) {
            sqlite_store.set_last_thread_id(0);
            start_new_thread_from_cli();
            status = "Removed thread " + std::to_string(removed_thread_id);
        } else {
            set_status_maybe_agent_error(detail::error_line(remove_error), true);
        }
        mode = TuiMode::Chat;
    };
    picker_callbacks.on_remove_rejected = [&]() {
        mode = TuiMode::Chat;
        status = "Remove cancelled";
    };
    picker_callbacks.on_remove_retry = [&](const std::string& message) { status = message; };
    picker_callbacks.on_guard_approval_accepted = [&]() {
        if (agent_approval_gate) agent_approval_gate->resolve(agent::GuardApprovalDecision::Allow);
        have_pending_guard_request = false;
        mode = TuiMode::Chat;
        status = "Guard: allowed";
    };
    picker_callbacks.on_guard_approval_rejected = [&]() {
        if (agent_approval_gate)
            agent_approval_gate->resolve(agent::GuardApprovalDecision::Deny);
        have_pending_guard_request = false;
        mode = TuiMode::Chat;
        status = "Guard: denied";
    };
    picker_callbacks.on_agent_permission_selected = [&](size_t selected) {
        static const char* modes[] = {"confirm", "smart", "yolo"};
        if (selected >= 3) return;
        mode = TuiMode::Chat;
        command_handlers.switch_agent_permission_mode(modes[selected]);
    };
    picker_callbacks.on_guard_approval_retry =
        [&](const std::string& message) { status = message; };
    picker_callbacks.on_agent_continue_accepted = [&]() {
        mode = TuiMode::Chat;
        start_turn("Continue the current task.");
        status = "Continuing agent task...";
    };
    picker_callbacks.on_agent_continue_rejected = [&]() {
        mode = TuiMode::Chat;
        status = "Agent stopped at the turn cap";
    };
    picker_callbacks.on_agent_new_accepted = [&]() {
        if (!have_pending_new_project) {
            mode = TuiMode::Chat;
            status = "No agent project selected";
            return;
        }
        agent::NewProjectTarget target = std::move(pending_new_project);
        pending_new_project = {};
        have_pending_new_project = false;
        mode = TuiMode::Chat;
        launch_agent_project_new(std::move(target), true);
    };
    picker_callbacks.on_agent_new_rejected = [&]() {
        pending_new_project = {};
        have_pending_new_project = false;
        mode = TuiMode::Chat;
        status = "New agent project cancelled; current project unchanged";
        flush_pending_index_build_offer();
    };
    picker_callbacks.on_agent_new_retry =
        [&](const std::string& message) { status = message; };
    picker_callbacks.on_agent_index_build_accepted = [&]() {
        mode = TuiMode::Chat;
        pending_index_build_offer = false;
        start_agent_index_code();
    };
    picker_callbacks.on_agent_index_build_rejected = [&]() {
        mode = TuiMode::Chat;
        pending_index_build_offer = false;
        status = agent_ready_with_index_controls();
    };
    picker_callbacks.on_thread_delete_accepted = [&]() {
        if (pending_thread_delete < thread_picker_threads.size()) {
            const long long tid = thread_picker_threads[pending_thread_delete].id;
            Error remove_error = sqlite_store.soft_delete_thread(tid);
            if (remove_error.ok()) {
                if (tid == session.thread_id) {
                    sqlite_store.set_last_thread_id(0);
                    start_new_thread_from_cli();
                }
                thread_picker_threads.erase(thread_picker_threads.begin() +
                                            static_cast<std::ptrdiff_t>(pending_thread_delete));
                if (thread_picker_threads.empty()) {
                    mode = TuiMode::Chat;
                    thread_picker_selected = 0;
                    status = "Thread deleted";
                } else {
                    if (thread_picker_selected > pending_thread_delete) {
                        --thread_picker_selected;
                    }
                    thread_picker_selected = std::min(thread_picker_selected,
                                                      thread_picker_threads.size() - 1);
                    mode = TuiMode::ThreadList;
                    status = ui::text_selector_status("Selected thread", thread_picker_selected,
                                                      thread_picker_threads.size());
                }
            } else {
                set_status_maybe_agent_error(detail::error_line(remove_error), true);
                mode = TuiMode::ThreadList;
            }
        } else {
            mode = TuiMode::ThreadList;
            status = "Nothing to delete";
        }
        pending_thread_delete = static_cast<size_t>(-1);
    };
    picker_callbacks.on_thread_delete_rejected = [&]() {
        mode = TuiMode::ThreadList;
        pending_thread_delete = static_cast<size_t>(-1);
        status = "Delete cancelled";
    };
    picker_callbacks.on_thread_delete_retry = [&](const std::string& message) { status = message; };
    picker_callbacks.on_model_confirm_accepted = [&]() {
        app::refresh_session_metadata(session, context);
        mode = TuiMode::Chat;
        status = "Using current model: " +
                 ui::provider_model_display_label(context.profile.name,
                                                  context.options.model);
        start_store_save();
    };
    picker_callbacks.on_model_confirm_rejected = [&]() {
        Error context_error = apply_loaded_session_context(session);
        mode = TuiMode::Chat;
        if (context_error.ok()) {
            status = "Using thread model: " +
                     ui::provider_model_display_label(context.profile.name,
                                                      context.options.model);
            start_store_save();
            if (!context.profile.offline && !context.options.model.empty() &&
                !context.options.has_context_tokens) {
                refresh_model_context();
            }
        } else {
            set_status_maybe_agent_error(detail::error_line(context_error), true);
        }
    };
    picker_callbacks.on_model_confirm_retry = [&](const std::string& message) { status = message; };

    auto submit_input = [&]() {
        std::string raw = input.text.str();
        const std::string text = app::detail::trim_ascii(raw);
        if (text.empty() && chat_attachments.empty() && pending_images.empty()) {
            input = new_input_editor();
            return;
        }
        // User shell: !cmd /shell (notice) or !!cmd /shell-stdout (draft).
        if (raw.find('\n') == std::string::npos && !text.empty()) {
            std::string shell_command;
            std::string shell_error;
            app::UserShellDestination shell_dest = app::UserShellDestination::Notice;
            if (app::parse_user_shell_invocation(text, shell_command, shell_error, shell_dest)) {
                input = new_input_editor();
                if (!shell_error.empty()) {
                    set_status_maybe_agent_error(shell_error, true);
                    return;
                }
                file_jobs.start_shell(shell_command,
                                     shell_dest == app::UserShellDestination::Draft);
                return;
            }
        }
        if (raw.find('\n') == std::string::npos && !text.empty() && text[0] == '/') {
            if (session.read_only) {
                input = new_input_editor();
                handle_command(text);
                return;
            }
            if (context.options.agent && (text == "/plan" || text == "/act")) {
                input = new_input_editor();
                handle_command(text);
                return;
            }
            if (!context.options.agent &&
                try_handle_chat_assist_command(text,
                                               input,
                                               ai_continue.assist_config,
                                               context,
                                               session,
                                               status,
                                               history_scroll,
                                               chat_assist_callbacks)) {
                input = new_input_editor();
                return;
            }
            input = new_input_editor();
            handle_command(text);
            return;
        }
        if (context.options.agent &&
            (!agent_runtime || !agent_runtime->prepared())) {
            // Recover if preparation was skipped (e.g. older offline gate) or
            // cancelled without a follow-up start.
            if (ensure_agent_prepare) ensure_agent_prepare();
            if (deferred_agent_prompt.has_value()) {
                status = "Agent is still preparing; one prompt is already queued";
            } else if (chat_attachments.empty() && pending_images.empty()) {
                deferred_agent_prompt = raw;
                input = new_input_editor();
                status = "Agent is still preparing; prompt queued";
            } else {
                status = "Agent is still preparing; attachment prompt kept in draft";
            }
            return;
        }
        if (active_job != ActiveJob::None) {
            status = "A model job is already running";
            return;
        }
        if (session.read_only) {
            status = "Thread is read-only: " + session.read_only_reason;
            return;
        }
        if (!require_provider_model_for_send()) {
            return;
        }

        const std::string typed_prompt = raw;

        std::string display_content = typed_prompt;

        if (!chat_attachments.empty()) {
            // History stays compact. Durable canonical Markdown is attached to the
            // message and materialized only in the cancellable request worker.
            if (!display_content.empty()) {
                display_content += "\n\n";
            }
            display_content += "Attached files (in order):\n";
            for (const auto& att : chat_attachments) {
                display_content += "- " + att.source + "\n";
            }

            attachments_committed_for_turn = chat_attachments.size();
        }
        if (!pending_images.empty()) {
            // Display/provenance only — image bytes stay on Message.images (request path).
            if (!display_content.empty()) {
                display_content += "\n\n";
            }
            display_content += "Attached images (in order):\n";
            for (const provider::ImageInput& image : pending_images) {
                const std::string label = !image.display_name.empty()
                                              ? image.display_name
                                              : (!image.source_ref.empty() ? image.source_ref
                                                                           : "image");
                display_content += "- " + label + "\n";
            }
            if (context.options.agent) {
                display_content +=
                    "(images are request-local for this agent turn; not stored in the project)\n";
            }
        }

        if (start_turn_with_pending_attachments(display_content)) {
            input = new_input_editor();
        } else {
            attachments_committed_for_turn = 0;
        }
    };

    refresh_model_context();
    refresh_credit_balance();

    auto start_agent_prepare = [&](bool indexing_enabled) {
        context.options.disable_indexing = !indexing_enabled;
        agent_activity_state = AgentActivityState::Preparing;
        agent_activity_started = std::chrono::steady_clock::now();
        agent_task_active = false;
        status = indexing_enabled ? "Preparing agent..."
                                  : "Preparing agent · indexing off...";
        provider::RequestContext prep_context = context;
        prep_context.options.quiet = true;
        agent::SessionRuntimeOptions prep_options =
            make_agent_runtime_options(initial_agent_workspace);
        prep_options.index_mode =
            indexing_enabled
                ? agent::SessionRuntimeOptions::IndexMode::UseExistingLazy
                : agent::SessionRuntimeOptions::IndexMode::Disabled;
        prep_options.on_index_progress =
            [&events](const agent::index::Progress& progress) {
                TuiEvent event;
                event.type = TuiEventType::AgentIndexProgress;
                event.agent_index_progress = progress;
                events.push(std::move(event));
            };
        file_job.start([prep_context = std::move(prep_context),
                        prep_options = std::move(prep_options), agent_runtime,
                        &events](runtime::CancellationToken token) mutable {
            TuiEvent event;
            event.type = TuiEventType::AgentPrepareDone;
            event.error =
                agent_runtime->prepare(prep_context, token, {}, prep_options);
            if (event.error.ok()) {
                event.agent_index_enabled = agent_runtime->indexing_enabled();
                event.error =
                    agent_runtime->load_display_messages(event.agent_history);
            }
            events.push(std::move(event));
        });
    };
    // prepare() is local-only (tools, project DB/history, AGENTS.md, cheap index
    // probe). It must not wait for provider/model selection: bare `ainiux -a`
    // defaults the profile to offline "none" only so the picker can open, and
    // that offline gate previously left the UI stuck on "Agent preparing"
    // forever because nothing ever started prepare after the user picked a
    // provider.
    ensure_agent_prepare = [&]() {
        if (!context.options.agent || !agent_runtime) return;
        if (agent_runtime->prepared()) return;
        if (file_job.joinable()) return;
        start_agent_prepare(!context.options.disable_indexing);
    };

    auto begin_chat_provider_model_startup = [&]() {
        if (context.options.agent) {
            return;
        }
        // Prefer an already-ready context (for example a loaded thread that
        // restored provider+model while the CLI left both unset).
        if (chat_provider_model_ready(context)) {
            if (!app::detail::trim_ascii(context.options.prompt).empty()) {
                start_turn(context.options.prompt);
                return;
            }
            refresh_startup_status();
            return;
        }
        if (provider::tui_needs_startup_provider_selection(context.options) ||
            !active_context_has_provider_selection(context) || context.profile.offline) {
            open_provider_picker(true);
            return;
        }
        if (provider::needs_interactive_model_selection(context) ||
            context.options.model.empty()) {
            start_models(ModelsRequestPurpose::Picker);
            return;
        }
        if (!app::detail::trim_ascii(context.options.prompt).empty()) {
            start_turn(context.options.prompt);
            return;
        }
        refresh_startup_status();
    };

    continue_after_startup_thread_choice = [&]() {
        if (!thread_list_from_startup) {
            return;
        }
        thread_list_from_startup = false;
        begin_chat_provider_model_startup();
    };

    // Agent preparation is fully asynchronous so discovery, SQLite, and history
    // I/O never block terminal input/rendering.
    if (context.options.agent && agent_runtime) {
        ensure_agent_prepare();
    } else {
        refresh_startup_status();
    }

    file_jobs.start_media_cleanup(context.options.media_auto_expiration_days,
                                  session.thread_id, true);

    if (context.options.agent) {
        if (provider::tui_needs_startup_provider_selection(context.options)) {
            open_provider_picker(true);
        } else if (provider::needs_interactive_model_selection(context)) {
            start_models(ModelsRequestPurpose::Picker);
        } else if (!app::detail::trim_ascii(context.options.prompt).empty()) {
            if (!agent_runtime || !agent_runtime->prepared()) {
                deferred_agent_prompt = context.options.prompt;
            } else {
                start_turn(context.options.prompt);
            }
        }
    } else {
        // Chat starts on the thread selector (same as Ctrl+L / /list) so the
        // user can open a saved thread or press N for a new one. Provider/model
        // pickers run after that choice. Returning from editor with an existing
        // thread skips this; mid-session /list still uses start_thread_list.
        const bool fresh_chat_session =
            session.thread_id <= 0 && !chat::session_has_chat_messages(session);
        if (sqlite_available && fresh_chat_session) {
            thread_list_from_startup = true;
            start_thread_list();
            if (mode != TuiMode::ThreadList) {
                // SQLite list failed after open; continue normal setup.
                thread_list_from_startup = false;
                begin_chat_provider_model_startup();
            }
        } else {
            begin_chat_provider_model_startup();
        }
    }

    std::string visible_panel = panel_text();
    size_t render_frame = 0;
    const auto render_animation_started = std::chrono::steady_clock::now();
    detail::TerminalFrameRenderer terminal_frame_renderer;
    ActivityKind activity_kind = ActivityKind::None;
    detail::render(session, input, status, history_scroll, show_thinking_traces, mode, visible_panel,
                   activity_kind, render_frame, syntax_highlight,
                   detail::RenderStyle{&context.options.tui_themes, theme, use_colors},
                   terminal_frame_renderer, panel_title(), context.options.agent,
                   build_agent_chrome());
    while (!quit) {
        credit_jobs.reap_finished();
        process_clipboard_events();
        TuiEvent event;
        while (events.try_pop(event)) {
            bool completed_file_job = false;
            switch (event.type) {
                case TuiEventType::Delta:
                    if (context.options.agent) break;
                    if (pending_assistant != static_cast<size_t>(-1) && pending_assistant < session.messages.size()) {
                        session.messages[pending_assistant].content += event.text;
                    }
                    break;
                case TuiEventType::AgentProgress: {
                    apply_agent_progress_update(session, live_agent_rows,
                                                event.agent_progress);
                    break;
                }
                case TuiEventType::AgentPrepareDone:
                    file_job.join();
                    {
                    const bool preserve_setup_picker =
                        mode == TuiMode::ProviderList ||
                        mode == TuiMode::ModelList ||
                        mode == TuiMode::ReasoningList ||
                        mode == TuiMode::ReasoningConfirm ||
                        mode == TuiMode::ModelConfirm;
                    const std::string setup_status = status;
                    if (!preserve_setup_picker) mode = TuiMode::Chat;
                    if (!event.error.ok()) {
                        append_agent_history_notice(event.error.message);
                        agent_activity_state = AgentActivityState::Unavailable;
                        status = "Agent unavailable";
                    } else if (!event.agent_history.empty()) {
                        agent_activity_state = AgentActivityState::Ready;
                        session.messages = std::move(event.agent_history);
                        history_scroll = history_scroll_for_thread_end();
                        status = event.text.empty()
                                     ? "agent · resumed · " +
                                           std::to_string(session.messages.size()) +
                                           " message(s)"
                                     : event.text;
                        if (context.options.disable_indexing)
                            status += " · indexing off";
                    } else {
                        agent_activity_state = AgentActivityState::Ready;
                        status = agent_ready_with_index_controls();
                    }
                    agent_runtime->begin_background_index_freshness();
                    if (preserve_setup_picker) {
                        status = setup_status;
                        // Offer after provider/model setup returns to Chat.
                        if (event.error.ok() && !event.agent_index_enabled &&
                            !context.options.disable_indexing)
                            pending_index_build_offer = true;
                    } else if (event.error.ok() && !event.agent_index_enabled &&
                               !context.options.disable_indexing) {
                        open_index_build_offer();
                    }
                    break;
                    }
                case TuiEventType::AgentPrepareProgress: {
                    agent_activity_state = AgentActivityState::Preparing;
                    const agent::PreparationProgress& progress =
                        event.agent_prepare_progress;
                    status = std::string("Preparing agent · ") +
                             agent::preparation_phase_name(progress.phase);
                    if (progress.completed) {
                        std::ostringstream elapsed;
                        elapsed << " · " << std::fixed << std::setprecision(2)
                                << static_cast<double>(
                                       progress.phase_elapsed_ms) /
                                       1000.0
                                << "s";
                        status += elapsed.str();
                    }
                    break;
                }
                case TuiEventType::AgentIndexProgress: {
                    const agent::index::Progress& progress =
                        event.agent_index_progress;
                    status = std::string("Index ") +
                             app::index_progress_phase_name(progress.phase) +
                             ": " + std::to_string(progress.completed);
                    if (progress.total > 0)
                        status += "/" + std::to_string(progress.total);
                    status += " · " + std::to_string(progress.discovered) +
                              " discovered · " +
                              std::to_string(progress.changed) + " changed";
                    break;
                }
                case TuiEventType::Done: {
                    model_job.join();
                    if (mode == TuiMode::GuardApprovalConfirm) {
                        mode = TuiMode::Chat;
                        have_pending_guard_request = false;
                    }
                    const bool should_regenerate = regenerate_after_cancel;
                    const size_t regenerate_erase_from = pending_user;
                    if (event.agent_turn) {
                        // Expand timed tool rows; renderer adds only the final
                        // "Task complete in …" from wall-clock timestamps.
                        if (pending_user != static_cast<size_t>(-1) &&
                            pending_user < session.messages.size() &&
                            session.messages[pending_user].created_at_ms <= 0 &&
                            event.agent_turn_started_ms > 0) {
                            session.messages[pending_user].created_at_ms =
                                event.agent_turn_started_ms;
                        }
                        if (pending_assistant != static_cast<size_t>(-1) &&
                            pending_assistant < session.messages.size()) {
                            const std::size_t erased = pending_assistant;
                            session.messages.erase(session.messages.begin() +
                                                   static_cast<long>(pending_assistant));
                            adjust_agent_live_rows_after_erase(live_agent_rows, erased);
                        }
                        if (live_agent_rows.empty()) {
                            for (std::size_t i = 0; i < event.agent_tool_lines.size(); ++i) {
                                provider::Message tool_msg{"tool", event.agent_tool_lines[i]};
                                if (i < event.agent_tool_line_ms.size()) {
                                    tool_msg.created_at_ms = event.agent_tool_line_ms[i];
                                }
                                session.messages.push_back(std::move(tool_msg));
                            }
                        }
                        if (!event.agent_final_text.empty()) {
                            provider::Message assistant_msg{"assistant", event.agent_final_text};
                            assistant_msg.created_at_ms = event.agent_finished_at_ms > 0
                                                              ? event.agent_finished_at_ms
                                                              : agent::now_unix_ms();
                            session.messages.push_back(std::move(assistant_msg));
                        }
                    } else if (pending_assistant != static_cast<size_t>(-1) &&
                               pending_assistant < session.messages.size()) {
                        session.messages[pending_assistant].content = event.chat.content;
                        if (session.messages[pending_assistant].created_at_ms <= 0) {
                            session.messages[pending_assistant].created_at_ms =
                                agent::now_unix_ms();
                        }
                    }
                    if (!event.chat.model.empty()) {
                        context.options.model = event.chat.model;
                        session.model = event.chat.model;
                    }
                    if (!event.chat.usage_json.empty() && event.chat.usage_json != "null") {
                        session.usage_json = event.chat.usage_json;
                    }
                    if (event.compacted) {
                        event.compaction.timestamp = chat::current_timestamp_utc();
                        session.compaction_events.push_back(event.compaction);
                    }
                    if (inflight_image_count > 0 && inflight_image_count <= pending_images.size()) {
                        pending_images.erase(pending_images.begin(),
                                             pending_images.begin() + static_cast<long>(inflight_image_count));
                    }
                    inflight_image_count = 0;
                    // Consume attachments that were used for the just-completed user prompt.
                    // This happens after the model has responded fully. Any attachments the
                    // user added during this turn (for a subsequent prompt) are preserved.
                    if (attachments_committed_for_turn > 0) {
                        const size_t n = attachments_committed_for_turn;
                        if (n >= chat_attachments.size()) {
                            chat_attachments.clear();
                        } else {
                            chat_attachments.erase(
                                chat_attachments.begin(),
                                chat_attachments.begin() + static_cast<std::ptrdiff_t>(n));
                        }
                        attachments_committed_for_turn = 0;
                        attachment_picker_selected = chat_attachments.empty() ? 0
                            : std::min(attachment_picker_selected, chat_attachments.size() - 1);
                    }
                    pending_user = static_cast<size_t>(-1);
                    pending_assistant = static_cast<size_t>(-1);
                    pending_user_added_for_job = false;
                    active_job = ActiveJob::None;
                    if (context.options.agent) {
                        agent_activity_state = AgentActivityState::Ready;
                        agent_task_active = false;
                        if (event.agent_turn) {
                            const bool valid_wall_times =
                                event.agent_turn_started_ms > 0 &&
                                event.agent_finished_at_ms >= event.agent_turn_started_ms;
                            const long long wall_ms = valid_wall_times
                                                          ? event.agent_finished_at_ms -
                                                                event.agent_turn_started_ms
                                                          : -1;
                            agent_completed_task_ms =
                                wall_ms >= 0
                                    ? wall_ms
                                    : std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() -
                                          agent_task_started)
                                          .count();
                        }
                    }
                    if (should_regenerate) {
                        start_queued_regeneration(regenerate_erase_from);
                    } else {
                        if (event.compacted) {
                            status = event.compaction.notice;
                        } else if (event.agent_needs_user_continue) {
                            mode = TuiMode::AgentContinueConfirm;
                            status = "Agent turn cap reached";
                        } else if (context.options.agent) {
                            // Provider/model/tokens live on the permanent agent chrome line.
                            status = agent_ready_with_index_controls();
                        } else {
                            status = generation_ready_status(context.profile.name,
                                                             context.options.model,
                                                             event.chat,
                                                             context.options.stream,
                                                             session.messages,
                                                             context.options.context_tokens);
                        }
                        start_save(context.options.save_chat_path, session, true);
                        start_store_save();
                    }
                    if (context.options.agent) refresh_credit_balance();
                    break;
                }
                case TuiEventType::Error: {
                    if (mode == TuiMode::GuardApprovalConfirm) {
                        mode = TuiMode::Chat;
                        have_pending_guard_request = false;
                    }
                    model_job.join();
                    const bool should_regenerate = regenerate_after_cancel && event.error.code == ErrorCode::Cancelled;
                    const size_t regenerate_erase_from = pending_user_added_for_job ? static_cast<size_t>(-1) : pending_user;
                    active_job = ActiveJob::None;
                    if (context.options.agent) {
                        agent_activity_state = AgentActivityState::Ready;
                        agent_task_active = false;
                        agent_completed_task_ms = -1;
                    }
                    inflight_image_count = 0;
                    if (should_regenerate) {
                        // Regeneration will reuse the prior prompt content (which already included
                        // any folded attachments). Reset committed so we don't double-consume.
                        attachments_committed_for_turn = 0;
                        rollback_pending_turn();
                        start_queued_regeneration(regenerate_erase_from);
                    } else {
                        clear_queued_regeneration();
                        const bool user_or_agent_stop = event.error.code == ErrorCode::Cancelled;
                        // Agent aborts (policy/tool failures) and user cancel both use Cancelled;
                        // always prefer the concrete message over a bare "Cancelled" status.
                        auto failure_status = [&]() -> std::string {
                            if (!event.error.message.empty()) return event.error.message;
                            if (user_or_agent_stop) return "Cancelled";
                            return detail::error_line(event.error);
                        };
                        if (user_or_agent_stop || (context.options.agent && event.agent_turn)) {
                            // Keep the prompt + any tool activity; explain the stop.
                            if (attachments_committed_for_turn > 0) {
                                const size_t n = attachments_committed_for_turn;
                                if (n >= chat_attachments.size()) {
                                    chat_attachments.clear();
                                } else {
                                    chat_attachments.erase(
                                        chat_attachments.begin(),
                                        chat_attachments.begin() + static_cast<std::ptrdiff_t>(n));
                                }
                                attachments_committed_for_turn = 0;
                                attachment_picker_selected = chat_attachments.empty() ? 0
                                    : std::min(attachment_picker_selected, chat_attachments.size() - 1);
                            }
                            if (event.agent_turn) {
                                // Expand structured agent rows (same as Done).
                                if (pending_user != static_cast<size_t>(-1) &&
                                    pending_user < session.messages.size() &&
                                    session.messages[pending_user].created_at_ms <= 0 &&
                                    event.agent_turn_started_ms > 0) {
                                    session.messages[pending_user].created_at_ms =
                                        event.agent_turn_started_ms;
                                }
                                if (pending_assistant != static_cast<size_t>(-1) &&
                                    pending_assistant < session.messages.size()) {
                                    const std::size_t erased = pending_assistant;
                                    session.messages.erase(session.messages.begin() +
                                                           static_cast<long>(pending_assistant));
                                    adjust_agent_live_rows_after_erase(live_agent_rows, erased);
                                }
                                if (live_agent_rows.empty()) {
                                    for (std::size_t i = 0;
                                         i < event.agent_tool_lines.size(); ++i) {
                                        provider::Message tool_msg{
                                            "tool", event.agent_tool_lines[i]};
                                        if (i < event.agent_tool_line_ms.size()) {
                                            tool_msg.created_at_ms =
                                                event.agent_tool_line_ms[i];
                                        }
                                        session.messages.push_back(std::move(tool_msg));
                                    }
                                }
                                const std::string fail_text =
                                    !event.agent_final_text.empty()
                                        ? event.agent_final_text
                                        : failure_status();
                                if (!fail_text.empty()) {
                                    provider::Message notice_msg{"notice", fail_text};
                                    notice_msg.created_at_ms = event.agent_finished_at_ms > 0
                                                                   ? event.agent_finished_at_ms
                                                                   : agent::now_unix_ms();
                                    session.messages.push_back(std::move(notice_msg));
                                    if (agent_runtime && agent_runtime->prepared()) {
                                        (void)agent_runtime->append_display_notice(fail_text);
                                    }
                                }
                                pending_user = static_cast<size_t>(-1);
                                pending_assistant = static_cast<size_t>(-1);
                                pending_user_added_for_job = false;
                            } else {
                                keep_cancelled_turn();
                                if (context.options.agent) {
                                    append_agent_history_notice(failure_status());
                                }
                            }
                            status = context.options.agent
                                         ? agent_ready_with_index_controls()
                                         : failure_status();
                            start_store_save();
                        } else {
                            // Error before/during; the user message is rolled back.
                            // Do not consume attachments; leave them for the user to re-send.
                            attachments_committed_for_turn = 0;
                            rollback_pending_turn();
                            if (context.options.agent) {
                                report_agent_error(failure_status());
                            } else {
                                status = failure_status();
                            }
                        }
                    }
                    break;
                }
                case TuiEventType::SaveDone:
                    file_job.join();
                    completed_file_job = true;
                    if (event.error.ok()) {
                        if (!event.quiet_success) {
                            status = "Saved " + event.text;
                        }
                    } else if (!event.quiet_success || event.error.code != ErrorCode::Cancelled) {
                        set_status_maybe_agent_error(detail::error_line(event.error), true);
                    }
                    break;
                case TuiEventType::LoadDone:
                    file_job.join();
                    completed_file_job = true;
                    if (event.error.ok()) {
                        session = std::move(event.session);
                        app::apply_system_prompt(session, context.options.system);
                        finish_loaded_session("Loaded " + event.text);
                        if (mode == TuiMode::Chat && !context.profile.offline &&
                            !context.options.model.empty() &&
                            !context.options.has_context_tokens &&
                            active_job == ActiveJob::None) {
                            refresh_model_context();
                        }
                    } else {
                        set_status_maybe_agent_error(detail::error_line(event.error), true);
                    }
                    break;
                case TuiEventType::StoreSaveDone:
                    file_job.join();
                    completed_file_job = true;
                    if (event.error.ok()) {
                        apply_store_save_result(event.session);
                    } else if (event.error.code != ErrorCode::Cancelled) {
                        set_status_maybe_agent_error(detail::error_line(event.error), true);
                    }
                    break;
                case TuiEventType::StoreLoadDone:
                    file_job.join();
                    completed_file_job = true;
                    if (event.error.ok()) {
                        rotate_routing_session_id();
                        session = std::move(event.session);
                        app::apply_system_prompt(session, context.options.system);
                        finish_loaded_session("Loaded thread: " +
                                              (session.name.empty() ? event.text : session.name));
                        if (mode == TuiMode::Chat && !context.profile.offline &&
                            !context.options.model.empty() &&
                            !context.options.has_context_tokens &&
                            active_job == ActiveJob::None) {
                            refresh_model_context();
                        }
                        // After startup thread selection, finish provider/model
                        // setup only when still in chat (not ModelConfirm /
                        // ProviderList opened by finish_loaded_session).
                        if (thread_list_from_startup && mode == TuiMode::Chat) {
                            if (continue_after_startup_thread_choice) {
                                continue_after_startup_thread_choice();
                            }
                        } else if (thread_list_from_startup) {
                            // ModelConfirm / incomplete-thread ProviderList own
                            // the next step; do not force another startup picker.
                            thread_list_from_startup = false;
                        }
                    } else {
                        set_status_maybe_agent_error(detail::error_line(event.error), true);
                        if (thread_list_from_startup) {
                            if (continue_after_startup_thread_choice) {
                                continue_after_startup_thread_choice();
                            }
                        }
                    }
                    break;
                case TuiEventType::MediaCleanupDone:
                    file_job.join();
                    completed_file_job = true;
                    if (!event.error.ok()) {
                        if (!event.automatic_cleanup || event.error.code != ErrorCode::Cancelled) {
                            set_status_maybe_agent_error(detail::error_line(event.error), true);
                        }
                    } else if (!event.automatic_cleanup || event.media_cleanup.files_removed > 0) {
                        status = "Media cleanup: " +
                                 std::to_string(event.media_cleanup.files_removed) +
                                 " file(s), " +
                                 std::to_string(event.media_cleanup.bytes_reclaimed) +
                                 " bytes reclaimed, " +
                                 std::to_string(event.media_cleanup.threads_locked) +
                                 " thread(s) locked";
                    }
                    break;
                case TuiEventType::InsertDone:
                    file_job.join();
                    completed_file_job = true;
                    if (event.error.ok()) {
                        const Error insert_error = input.insert(event.inserted_text);
                        if (insert_error.ok()) {
                            status = "Inserted " + event.text + " at cursor";
                        } else {
                            set_status_maybe_agent_error(detail::error_line(insert_error), true);
                        }
                    } else {
                        set_status_maybe_agent_error(detail::error_line(event.error), true);
                    }
                    break;
                case TuiEventType::AttachDone: {
                    // Sync validation failures push AttachDone without starting file_job.
                    if (file_job.joinable()) {
                        file_job.join();
                        completed_file_job = true;
                    }
                    auto attach_user_notice = [&](const std::string& text, bool as_error) {
                        if (text.empty()) return;
                        if (context.options.agent) {
                            // Durable agent history notice (status chrome alone is easy to miss).
                            append_agent_history_notice(text);
                            if (as_error) {
                                status = agent_runtime && agent_runtime->prepared() &&
                                                 !agent_task_active
                                             ? agent_ready_with_index_controls()
                                             : status;
                            } else {
                                status = text;
                            }
                        } else {
                            // Chat: keep compact status; also leave a display-only notice row.
                            provider::Message notice{"notice", text};
                            notice.created_at_ms = agent::now_unix_ms();
                            session.messages.push_back(std::move(notice));
                            history_scroll = history_scroll_for_thread_end();
                            status = text;
                        }
                    };
                    if (event.error.ok() && event.image_attachment) {
                        pending_images.push_back(std::move(event.image));
                        std::string msg =
                            "Attached image for next prompt: " + event.text + " (" +
                            std::to_string(pending_images.size()) + " pending";
                        if (context.options.agent) {
                            msg += "; request-local for that turn, not stored";
                        }
                        msg += ")";
                        attach_user_notice(msg, false);
                    } else if (event.error.ok() && event.text_attachment_ready) {
                        chat_attachments.push_back(
                            {event.attached_source, std::move(event.text_attachment)});
                        attach_user_notice(
                            "Attached " + event.text + " (" +
                                std::to_string(chat_attachments.size()) + " attachment" +
                                (chat_attachments.size() == 1 ? "" : "s") +
                                (context.options.agent ? "; included on next agent turn" : "") +
                                ")",
                            false);
                    } else if (event.error.ok()) {
                        // Fallback for any legacy inserted_message path
                        if (!event.inserted_message.content.empty()) {
                            session.messages.push_back(std::move(event.inserted_message));
                        }
                        attach_user_notice("Attached context from " + event.text, false);
                    } else {
                        const std::string fail =
                            "Attach failed: " +
                            (event.text.empty() ? std::string("(unknown path)") : event.text) +
                            " — " +
                            (event.error.message.empty() ? detail::error_line(event.error)
                                                         : event.error.message);
                        attach_user_notice(fail, true);
                        if (!context.options.agent) {
                            set_status_maybe_agent_error(detail::error_line(event.error), true);
                        }
                    }
                    break;
                }
                case TuiEventType::FetchDone:
                    file_job.join();
                    completed_file_job = true;
                    if (event.error.ok()) {
                        session.messages.push_back(std::move(event.inserted_message));
                        history_scroll = 0;
                        status = "Fetched and inserted " + event.text;
                    } else {
                        set_status_maybe_agent_error(detail::error_line(event.error), true);
                    }
                    break;
                case TuiEventType::SearchDone:
                    file_job.join();
                    completed_file_job = true;
                    if (event.error.ok()) {
                        session.messages.push_back(std::move(event.inserted_message));
                        history_scroll = 0;
                        status = "Inserted web search results for " + event.text;
                    } else {
                        set_status_maybe_agent_error(detail::error_line(event.error), true);
                    }
                    break;
                case TuiEventType::ShellDone: {
                    file_job.join();
                    completed_file_job = true;
                    if (event.shell_to_draft) {
                        // Replace input draft with pure stdout; user edits before send.
                        input = editor::EditorState::from_text(event.inserted_text);
                        input.set_undo_limit(input_undo_limit);
                        input.set_language(highlight::Language::Markdown, false);
                        input.highlight_enabled = syntax_highlight;
                        // On failure: display-only diagnostic notice (stderr/reason).
                        if (event.shell_failed && !event.inserted_message.content.empty()) {
                            session.messages.push_back(event.inserted_message);
                            history_scroll = 0;
                            if (context.options.agent && agent_runtime &&
                                agent_runtime->prepared()) {
                                (void)agent_runtime->append_display_notice(
                                    event.inserted_message.content);
                            }
                        }
                        // event.text is preformatted draft status (success or clear failure).
                        status = event.text.empty()
                                     ? (event.shell_failed ? "Shell-stdout failed"
                                                           : "Shell → draft")
                                     : event.text;
                    } else {
                        // Notice mode: show captured output in history (not model context).
                        if (!event.inserted_message.content.empty()) {
                            session.messages.push_back(event.inserted_message);
                            history_scroll = 0;
                            if (context.options.agent && agent_runtime &&
                                agent_runtime->prepared()) {
                                (void)agent_runtime->append_display_notice(
                                    event.inserted_message.content);
                            }
                        }
                        if (event.error.ok()) {
                            status = event.quiet_success
                                         ? ("Shell ok · " + event.text)
                                         : ("Shell finished · " + event.text);
                        } else if (event.error.code == ErrorCode::Cancelled) {
                            status = "Shell cancelled";
                        } else if (event.error.code == ErrorCode::Timeout) {
                            set_status_maybe_agent_error("Shell timed out · " + event.text, true);
                        } else {
                            set_status_maybe_agent_error(detail::error_line(event.error), true);
                        }
                    }
                    break;
                }
                case TuiEventType::ModelsDone: {
                    model_job.join();
                    active_job = ActiveJob::None;
                    const ModelsRequestPurpose completed_purpose = models_request_purpose;
                    models_request_purpose = ModelsRequestPurpose::Preview;
                    if (event.error.ok()) {
                        cached_models = std::move(event.models_result);
                        have_cached_models = true;
                        provider::apply_context_window_from_models(
                            context, cached_models, context.options.model);
                    }
                    if (completed_purpose == ModelsRequestPurpose::Picker) {
                        if (!event.error.ok()) {
                            set_status_maybe_agent_error(detail::error_line(event.error), true);
                        } else if (event.models.empty()) {
                            set_status_maybe_agent_error("No models returned", true);
                        } else if (ui::should_auto_select_only_model(event.models)) {
                            const std::string only_model = event.models.front();
                            picker_callbacks.on_model_selected(only_model);
                            status = context.options.agent
                                         ? (agent_runtime &&
                                                    agent_runtime->prepared()
                                                ? agent_ready_with_index_controls()
                                                : "Preparing agent...")
                                         : provider_model_status_message(context,
                                                                        "only model auto-selected");
                        } else {
                            picker_items = std::move(event.models);
                            picker_selected = 0;
                            picker_cancel_quits = false;
                            mode = TuiMode::ModelList;
                            history_scroll = 0;
                            help_text.clear();
                            settings_text.clear();
                            status = ui::text_selector_status("Selected model", picker_selected,
                                                              picker_items.size());
                        }
                    } else if (event.error.ok()) {
                        status = join_models_preview(event.models);
                    } else {
                        set_status_maybe_agent_error(detail::error_line(event.error), true);
                    }
                    break;
                }
                case TuiEventType::CreditBalanceDone: {
                    if (event.background_generation == credit_jobs.generation() &&
                        event.requested_provider ==
                            provider::normalize_provider_key(context.profile.name)) {
                        credit_balance_label =
                            event.error.ok()
                                ? provider::format_credit_balance(
                                      event.credit_balance)
                                : std::string();
                    }
                    break;
                }
                case TuiEventType::CompletionDone:
                    completion_job.join();
                    completion_pending = false;
                    if (event.completion_generation == completion_generation &&
                        event.completion.handled) {
                        input = std::move(event.completed_input);
                        path_completer = std::move(event.path_completer);
                        path_completer.set_assist_config(
                            context.options.agent ? nullptr
                                                  : &ai_continue.assist_config);
                        path_completer.set_agent_mode(context.options.agent);
                        status = editor::path_completion_status(event.completion);
                    }
                    break;
                case TuiEventType::AgentProjectNewDone: {
                    model_job.join();
                    active_job = ActiveJob::None;
                    mode = TuiMode::Chat;
                    pending_new_project = {};
                    have_pending_new_project = false;
                    if (!event.error.ok()) {
                        std::vector<provider::Message> restored;
                        if (agent_runtime && agent_runtime->prepared() &&
                            !agent_runtime->load_display_messages(restored).ok())
                            restored.clear();
                        apply_agent_project_history_handoff(
                            session, project_switch_previous_history,
                            std::move(restored), false);
                        report_agent_error(event.error.message);
                        break;
                    }
                    apply_agent_project_history_handoff(
                        session, project_switch_previous_history,
                        std::move(event.agent_history), true);
                    rotate_routing_session_id();
                    pending_images.clear();
                    inflight_image_count = 0;
                    chat_attachments.clear();
                    attachments_committed_for_turn = 0;
                    history_scroll = history_scroll_for_thread_end();
                    initial_agent_workspace = event.agent_workspace;
                    status = event.text.empty()
                                 ? "Fresh agent project · " + event.agent_workspace
                                 : event.text;
                    agent_runtime->begin_background_index_freshness();
                    if (!event.agent_index_enabled &&
                        !context.options.disable_indexing) {
                        open_index_build_offer();
                    }
                    break;
                }
                case TuiEventType::AgentCompactDone:
                    model_job.join();
                    active_job = ActiveJob::None;
                    agent_compaction_active = false;
                    agent_task_active = false;
                    agent_activity_state = AgentActivityState::Ready;
                    agent_completed_task_ms = -1;
                    if (event.agent_history_loaded) {
                        session.messages = std::move(event.agent_history);
                        history_scroll = history_scroll_for_thread_end();
                    }
                    if (!event.text.empty() &&
                        (session.messages.empty() ||
                         session.messages.back().role != "notice" ||
                         session.messages.back().content != event.text)) {
                        append_agent_history_notice(event.text);
                    }
                    if (!event.error.ok()) {
                        status = agent_ready_with_index_controls();
                    } else if (event.agent_compacted) {
                        status = event.text.empty()
                                     ? "Agent context compacted"
                                     : event.text;
                    } else {
                        status = event.text.empty() ? "Nothing new to compact" : event.text;
                    }
                    break;
                case TuiEventType::AgentIndexReportDone:
                    file_job.join();
                    if (event.agent_index_enabled) {
                        context.options.disable_indexing = false;
                    }
                    if (!event.error.ok()) {
                        report_agent_error(event.error.message);
                    } else {
                        if (event.agent_history_loaded) {
                            session.messages = std::move(event.agent_history);
                            history_scroll = history_scroll_for_thread_end();
                        }
                        status = event.text.empty() ? "Code index refreshed"
                                                    : event.text;
                    }
                    break;
                case TuiEventType::GuardApproval:
                    agent_activity_state = AgentActivityState::Working;
                    pending_guard_request = {};
                    pending_guard_request.tool_name = event.guard_tool_name;
                    pending_guard_request.command_preview = event.guard_command_preview;
                    pending_guard_request.rule_id = event.guard_rule_id;
                    pending_guard_request.message = event.guard_message;
                    have_pending_guard_request = true;
                    mode = TuiMode::GuardApprovalConfirm;
                    status = "Guard approval required";
                    break;
                case TuiEventType::AgentPhase:
                    if (agent_task_active)
                        agent_activity_state =
                            event.agent_phase == agent::AgentActivityPhase::Thinking
                                ? AgentActivityState::Thinking
                                : AgentActivityState::Working;
                    break;
            }
            if (completed_file_job) {
                resume_deferred_store_save();
            }
        }

        // After setup pickers or other modal work returns to Chat, show the
        // deferred first-run index offer once the UI is free.
        flush_pending_index_build_offer();

        if (deferred_agent_prompt.has_value() &&
            mode == TuiMode::Chat &&
            active_job == ActiveJob::None &&
            !file_job.joinable() &&
            !pending_index_build_offer &&
            agent_runtime && agent_runtime->prepared() &&
            chat_provider_model_ready(context)) {
            std::string prompt = std::move(*deferred_agent_prompt);
            deferred_agent_prompt.reset();
            start_turn(prompt);
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        timeval timeout{};
        timeout.tv_usec = 50000;
        const int ready = select(STDIN_FILENO + 1, &readfds, nullptr, nullptr, &timeout);
        if (ready < 0 && errno != EINTR) {
            set_status_maybe_agent_error(std::string("terminal input error: ") + std::strerror(errno),
                                         true);
        }
        if (ready > 0 && FD_ISSET(STDIN_FILENO, &readfds)) {
            editor::TerminalInputEvent event;
            while (editor::read_terminal_input(event, 0)) {
                if (event.type == editor::TerminalInputType::BracketedPaste) {
                    cancel_pending_clipboard();
                    if (!clipboard_mode_editable(mode)) {
                        status = "Paste is not accepted in this confirmation or picker";
                        continue;
                    }
                    path_completer.reset();
                    ++completion_generation;
                    completion_job.cancel();
                    editor::Clipboard external;
                    external.set(event.text);
                    Error paste_error =
                        event.text.empty()
                            ? Error{ErrorCode::BadArgs,
                                    "terminal paste is empty"}
                            : input.paste(external);
                    status = paste_error.ok() ? "Pasted" : paste_error.message;
                    continue;
                }
                if (event.type ==
                    editor::TerminalInputType::Osc52ClipboardResponse) {
                    if (pending_clipboard.active &&
                        pending_clipboard.terminal_query) {
                        if (!event.text.empty()) {
                            apply_external_clipboard(event.text,
                                                     "terminal clipboard");
                        } else {
                            editor::ClipboardCommand native_command;
                            if (editor::resolve_clipboard_command(
                                    clipboard_environment, false,
                                    native_command)) {
                                pending_clipboard.terminal_query = false;
                                pending_clipboard.generation =
                                    clipboard_runtime.start_read(
                                        clipboard_environment);
                            } else {
                                status = (event.message.empty()
                                              ? "Terminal clipboard query failed"
                                              : event.message) +
                                         std::string(
                                             "; use the terminal paste shortcut");
                                pending_clipboard =
                                    PendingTuiClipboardPaste{};
                            }
                        }
                    }
                    continue;
                }
                if (event.type != editor::TerminalInputType::Byte) {
                    continue;
                }
                const unsigned char ch = event.byte;
                TuiPickerInputState picker_state{mode,
                                                 quit,
                                                 status,
                                                 picker_items,
                                                 picker_selected,
                                                 picker_cancel_quits,
                                                 thread_picker_threads,
                                                 thread_picker_selected,
                                                 input.text.empty(),
                                                 pending_thread_delete,
                                                 context.options.agent};
                if (handle_tui_picker_input(ch, picker_state, picker_callbacks)) {
                    if (loaded_thread_requires_provider_selection && mode == TuiMode::Chat) {
                        status = chat_provider_model_required_status(context, true);
                    }
                    continue;
                }
                if (mode == TuiMode::AttachmentList) {
                    if (ch == 17) {
                        quit = true;
                        continue;
                    }
                    if (ch == 27) {
                        const PickerEscapeResult res =
                            handle_attachment_list_escape(chat_attachments.size(),
                                                          attachment_picker_selected,
                                                          status,
                                                          pending_attachment_delete,
                                                          mode);
                        if (res == PickerEscapeResult::Cancelled) {
                            mode = TuiMode::Chat;
                            status = "Attachment list closed";
                        }
                        continue;
                    }
                    if (ch == '\r' || ch == '\n') {
                        // Enter on attachment list: just show status, no auto-insert
                        if (attachment_picker_selected < chat_attachments.size()) {
                            status = "Attachment: " + chat_attachments[attachment_picker_selected].source +
                                     " (use DEL to remove, Esc to close)";
                        }
                        continue;
                    }
                    // DEL key: 127 or [3~ forward delete (Ctrl+H is help, not delete).
                    if (ch == 127) {
                        if (attachment_picker_selected < chat_attachments.size()) {
                            pending_attachment_delete = attachment_picker_selected;
                            mode = TuiMode::AttachmentDeleteConfirm;
                            status = "Delete attachment? y/n (Esc cancels)";
                        }
                        continue;
                    }
                    if (ch == 8) {
                        handle_command("/help");
                        continue;
                    }
                    // Also support Delete via escape sequence detection for [3~
                    // (fall through to escape handler below for sequences)
                    if (ch >= 32) {
                        // ignore printable in list
                    }
                    continue;
                }
                if (mode == TuiMode::AttachmentDeleteConfirm) {
                    if (ch == 17) {
                        quit = true;
                        continue;
                    }
                    switch (ui::parse_confirmation_key(ch)) {
                        case ui::ConfirmationKeyResult::Accepted:
                            if (pending_attachment_delete < chat_attachments.size()) {
                                const std::string removed = chat_attachments[pending_attachment_delete].source;
                                chat_attachments.erase(chat_attachments.begin() +
                                                       static_cast<std::ptrdiff_t>(pending_attachment_delete));
                                attachment_picker_selected = std::min(attachment_picker_selected,
                                                                      chat_attachments.empty() ? 0 : chat_attachments.size() - 1);
                                if (chat_attachments.empty()) {
                                    mode = TuiMode::Chat;
                                    status = "Deleted " + removed + "; no attachments left";
                                } else {
                                    mode = TuiMode::AttachmentList;
                                    status = ui::text_selector_status("Selected attachment",
                                                                      attachment_picker_selected,
                                                                      chat_attachments.size());
                                }
                            } else {
                                mode = TuiMode::AttachmentList;
                                status = "Nothing to delete";
                            }
                            pending_attachment_delete = static_cast<size_t>(-1);
                            continue;
                        case ui::ConfirmationKeyResult::Rejected:
                            mode = TuiMode::AttachmentList;
                            pending_attachment_delete = static_cast<size_t>(-1);
                            status = "Delete cancelled";
                            continue;
                        case ui::ConfirmationKeyResult::Pending:
                            status = "Press y to delete, n or Esc to cancel";
                            continue;
                    }
                    continue;
                }
                if (mode == TuiMode::SystemEdit) {
                    if (ch == 27) {
                        const detail::TuiSize screen = detail::terminal_size();
                        const EscapeResult escape_result = handle_escape(
                            input, current_layout(screen.rows, screen.cols), history_scroll, status, true);
                        if (escape_result == EscapeResult::Unhandled) {
                            mode = TuiMode::Chat;
                            input = new_input_editor();
                            status = "System prompt edit cancelled";
                        }
                        continue;
                    }
                    if (ch == '\r' || ch == '\n' || ch == 19) {
                        const std::string system_text = input.text.str();
                        app::replace_system_prompt(session, system_text);
                        context.options.system = system_text;
                        mode = TuiMode::Chat;
                        input = new_input_editor();
                        persist_settings_change("System prompt updated");
                        continue;
                    }
                }
                if (mode == TuiMode::HistoryEdit) {
                    if (ch == 27) {
                        const detail::TuiSize screen = detail::terminal_size();
                        const EscapeResult escape_result = handle_escape(
                            input, current_layout(screen.rows, screen.cols), history_scroll, status, true);
                        if (escape_result == EscapeResult::Unhandled) {
                            mode = TuiMode::Chat;
                            history_edit_index = static_cast<size_t>(-1);
                            input = new_input_editor();
                            status = "Message edit cancelled";
                        }
                        continue;
                    }
                    if (ch == '\r' || ch == '\n' || ch == 19) {
                        if (history_edit_index < session.messages.size()) {
                            const std::string role = session.messages[history_edit_index].role;
                            session.messages[history_edit_index].content = input.text.str();
                            if (role == "assistant") {
                                session.usage_json = "{}";
                            }
                            history_scroll = 0;
                            start_save(context.options.save_chat_path, session, true);
                            start_store_save();
                            status = "Updated " + role + " message";
                        }
                        mode = TuiMode::Chat;
                        history_edit_index = static_cast<size_t>(-1);
                        input = new_input_editor();
                        continue;
                    }
                }
                if (ch == '\t') {
                    start_path_completion();
                    continue;
                }
                path_completer.reset();
                ++completion_generation;
                completion_job.cancel();
                if (ch == 17) {
                    quit = true;
                    continue;
                }
                if (ch == 27) {
                    const detail::TuiSize screen = detail::terminal_size();
                    const EscapeResult escape_result =
                        handle_escape(input, current_layout(screen.rows, screen.cols), history_scroll, status);
                    if (escape_result == EscapeResult::Unhandled) {
                        if (active_job != ActiveJob::None) {
                            cancel_active_request();
                        } else if (file_job.running()) {
                            file_job.cancel();
                            status = "Cancelling file job...";
                        }
                    }
                    continue;
                }
                if (ch == 3) {
                    Error copy_error = input.copy_selection(editor::shared_clipboard());
                    if (copy_error.ok()) publish_internal_clipboard();
                    status = copy_error.ok() ? "Copied selection" : copy_error.message;
                    continue;
                }
                if (ch == 8) {
                    // Ctrl+H: mode-dependent help (chat/agent panel).
                    handle_command("/help");
                    continue;
                }
                if (ch == 22) {
                    if (editor::shared_clipboard().empty()) {
                        begin_external_clipboard_paste();
                    } else {
                        cancel_pending_clipboard();
                        Error paste_error =
                            editor::paste_with_clipboard_preference(
                                input, editor::shared_clipboard(), "");
                        status =
                            paste_error.ok() ? "Pasted" : paste_error.message;
                    }
                    continue;
                }
                if (editor::is_editor_undo_key(ch)) {
                    status = input.undo() ? "Undone" : "Nothing to undo";
                    continue;
                }
                if (editor::is_editor_redo_key(ch)) {
                    status = input.redo() ? "Redone" : "Nothing to redo";
                    continue;
                }
                if (mode == TuiMode::Chat && ch == 2) {
                    const detail::TuiSize screen = detail::terminal_size();
                    scroll_chat_history_page_up(current_layout(screen.rows, screen.cols), history_scroll);
                    continue;
                }
                if (mode == TuiMode::Chat && ch == 4) {
                    const detail::TuiSize screen = detail::terminal_size();
                    scroll_chat_history_page_down(current_layout(screen.rows, screen.cols), history_scroll);
                    continue;
                }
                if (ch == 18 && mode == TuiMode::Chat &&
                    !context.options.agent) {
                    regenerate_last_turn();
                    continue;
                }
                if (ch == editor::editor_key_toggle_thinking_traces()) {
                    set_thinking_trace_mode(!show_thinking_traces);
                    continue;
                }
                if (ch == 23 && mode == TuiMode::Chat && !context.options.agent) {
                    set_thinking_trace_mode(!show_thinking_traces);
                    continue;
                }
                if (ch == 20) {
                    cycle_reasoning();
                    continue;
                }
                if (ch == 19) {
                    submit_input();
                    continue;
                }
                if (ch == 1) {
                    input.select_all();
                    continue;
                }
                if (ch == 0 && mode == TuiMode::Chat &&
                    !context.options.agent && active_job == ActiveJob::None) {
                    handle_chat_assist_continue_key(ai_continue.assist_config,
                                                    status,
                                                    chat_assist_callbacks);
                    continue;
                }
                if (ch == 7 && mode == TuiMode::Chat && active_job == ActiveJob::None) {
                    // Ctrl+G: cycle chat/agent ↔ editor (same as /cycle).
                    command_handlers.switch_to_editor();
                    continue;
                }
                if (ch == 16 && mode == TuiMode::Chat && active_job == ActiveJob::None) {
                    // Ctrl+P: open provider picker (same as bare /provider).
                    open_provider_picker(false);
                    continue;
                }
                if (ch == 5 && mode == TuiMode::Chat) {
                    start_history_edit();
                    continue;
                }
                if (ch == 12 && mode == TuiMode::Chat) {
                    start_thread_list();
                    continue;
                }
                if (ch == 11) {
                    const std::uint64_t before = input.revision();
                    const Error kill_error =
                        input.kill_to_line_end(editor::shared_clipboard());
                    detail::set_status_from_error(kill_error, status);
                    if (kill_error.ok() && input.revision() != before)
                        publish_internal_clipboard();
                    continue;
                }
                if (ch == 127) {
                    const bool had_selection = input.selection.has_range();
                    const Error erase_error = input.erase_before_cursor();
                    if (!erase_error.ok()) {
                        detail::set_status_from_error(erase_error, status);
                    } else if (had_selection) {
                        publish_internal_clipboard();
                        status = "Cut selection";
                    }
                    continue;
                }
                if (ch == '\r' || ch == '\n') {
                    submit_input();
                    continue;
                }
                if (ch >= 32) {
                    detail::insert_input(input, std::string(1, static_cast<char>(ch)), status);
                }
            }
        }
        if (agent_compaction_active) {
            const long long seconds =
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - agent_task_started)
                    .count();
            status = agent::format_compaction_progress(
                agent_compaction_strategy, seconds);
        }
        visible_panel = panel_text();
        activity_kind = active_job == ActiveJob::Chat
                            ? activity_kind_for_pending_assistant(session, pending_assistant,
                                                                  show_thinking_traces)
                            : ActivityKind::None;
        render_frame = static_cast<size_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - render_animation_started)
                .count() /
            200);
        detail::render(session, input, status, history_scroll, show_thinking_traces, mode, visible_panel,
                       activity_kind, render_frame, syntax_highlight,
                       detail::RenderStyle{&context.options.tui_themes, theme, use_colors},
                       terminal_frame_renderer, panel_title(), context.options.agent,
                       build_agent_chrome());
    }

    model_job.cancel();
    credit_jobs.cancel_all();
    clipboard_runtime.cancel_all();
    completion_job.cancel();
    model_job.join();
    credit_jobs.shutdown();
    completion_job.join();
    file_job.cancel();
    file_job.join();
    TuiEvent shutdown_event;
    while (events.try_pop(shutdown_event)) {
        if (shutdown_event.type == TuiEventType::StoreSaveDone &&
            shutdown_event.error.ok()) {
            apply_store_save_result(shutdown_event.session);
        }
    }
    if (deferred_store_save.has_value() && sqlite_available) {
        chat::Session snapshot = std::move(*deferred_store_save);
        chat::SqliteStore shutdown_store;
        Error save_error = shutdown_store.open(sqlite_path);
        if (save_error.ok()) {
            save_error = shutdown_store.save_session(snapshot);
        }
        if (save_error.ok()) {
            session.thread_id = snapshot.thread_id;
            session.name = snapshot.name;
            session.created_at = snapshot.created_at;
            session.updated_at = snapshot.updated_at;
        }
        deferred_store_save.reset();
    }
    remove_empty_thread_on_exit();
    if (leave_target == app::InteractiveUiTarget::Editor ||
        leave_target == app::InteractiveUiTarget::Chat ||
        leave_target == app::InteractiveUiTarget::Agent) {
        if (interactive != nullptr) {
            interactive->context = context;
            // Never write agent transcript into the chat library session. Agent
            // history is project-local (.ainiux-pr/agent.sqlite) and must not
            // reappear under Chat after Ctrl+G /cycle.
            if (!context.options.agent) {
                interactive->chat_session = session;
                interactive->chat_session_initialized = true;
            }
            interactive->ai_continue = ai_continue;
            interactive->assist_config = ai_continue.assist_config;
            interactive->highlight_enabled = syntax_highlight;
            interactive->theme_name = theme;
            interactive->use_colors = use_colors;
        }
        return {0, leave_target};
    }
    // Process exit from agent: finish open session if any.
    if (context.options.agent && agent_runtime && agent_runtime->prepared() &&
        agent_runtime->session_id() > 0) {
        (void)agent_runtime->finish_session("success", "", "", "");
    }
    return {0, app::InteractiveUiTarget::Quit};
}

}  // namespace ainiux::tui
