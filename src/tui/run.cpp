#include "tui/activity.hpp"
#include "tui/tui.hpp"
#include "tui/events.hpp"
#include "tui/chat_assist.hpp"
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

#include "app/app.hpp"
#include "app/interactive_mode.hpp"
#include "app/detail.hpp"
#include "agent/session_runtime.hpp"
#include "chat/settings.hpp"
#include "chat/media_store.hpp"
#include "ainiux/model_setting.hpp"
#include "chat/sqlite_store.hpp"
#include "cli/args.hpp"
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
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <sys/select.h>
#include <unistd.h>

namespace ainiux::tui {

using detail::RenderStyle;

app::TuiRunResult run(provider::RequestContext context,
                      chat::Session session,
                      app::InteractiveSession* interactive) {
    const provider::RequestContext cli_context = context;
    TerminalSession terminal;
    Error err = terminal.enter();
    if (!err.ok()) {
        std::cerr << error_code_name(err.code) << ": " << err.message << "\n";
        return {app::exit_code_for(err.code), app::InteractiveUiTarget::Quit};
    }

    runtime::EventQueue<TuiEvent> events;
    runtime::JobHandle model_job;
    runtime::JobHandle file_job;
    runtime::JobHandle completion_job;
    ActiveJob active_job = ActiveJob::None;
    std::optional<chat::Session> deferred_store_save;
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
    path_completer.set_assist_config(&ai_continue.assist_config);
    ChatAssistCallbacks chat_assist_callbacks;
    size_t completion_generation = 0;
    bool completion_pending = false;
    std::string status = ready_status();
    if (context.options.agent) {
        status = "Agent mode · workspace tools enabled · /mode · /chat · /editor";
    }
    std::string theme = "dark";
    context.options.tui_themes.normalize_name(context.options.tui_theme, theme);
    const bool use_colors = !context.options.no_colors;
    bool quit = false;
    app::InteractiveUiTarget leave_target = app::InteractiveUiTarget::Quit;
    // Warm multi-turn agent session (project .ainiux/agent.sqlite). Prepared on first turn.
    std::shared_ptr<agent::AgentSessionRuntime> agent_runtime =
        context.options.agent ? std::make_shared<agent::AgentSessionRuntime>()
                              : std::shared_ptr<agent::AgentSessionRuntime>{};
    bool show_thinking_traces = context.options.show_thinking_traces;
    size_t pending_user = static_cast<size_t>(-1);
    size_t pending_assistant = static_cast<size_t>(-1);
    bool pending_user_added_for_job = false;
    int history_scroll = 0;
    bool regenerate_after_cancel = false;
    std::string queued_regeneration_prompt;
    std::vector<provider::ImageInput> pending_images;
    size_t inflight_image_count = 0;
    std::string help_text;
    std::string settings_text;
    std::string pending_reasoning;
    std::string pending_reasoning_warning;
    TuiMode mode = TuiMode::Chat;
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
            return pending_reasoning_warning + "\n\nUse '" + pending_reasoning +
                   "' anyway? Press y to proceed · n or Esc to cancel";
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
                if (!th.last_model.empty()) {
                    label += " [" + ui::compact_model_name_for_display(th.last_model) + "]";
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
        completion_completer.set_assist_config(&ai_continue.assist_config);
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

    auto start_save = [&](const std::string& path, chat::Session snapshot, bool quiet_success = false) {
        file_jobs.start_save(path, std::move(snapshot), quiet_success);
    };

    auto start_store_save = [&]() {
        if (!sqlite_available || session.read_only) {
            return;
        }
        chat::Session snapshot = session;
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
            status = provider_model_status_message(
                context, "agent · tools on · /mode chat|editor · /cycle");
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

        std::vector<provider::Message> request_messages = session.messages;
        request_messages.pop_back();

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
        std::string agent_goal;
        if (agent_mode) {
            for (auto it = request_messages.rbegin(); it != request_messages.rend(); ++it) {
                if (it->role == "user") {
                    agent_goal = it->content;
                    break;
                }
            }
        }
        model_job.start([job_context, request_messages = std::move(request_messages),
                         media_database_path, max_image_bytes, max_attachment_bytes, agent_mode,
                         agent_goal = std::move(agent_goal), agent_runtime,
                         &events](runtime::CancellationToken token) mutable {
            provider::ChatResult chat_result;
            Error send_error = ok_error();
            ainiux::context::PreparedMessages prepared;
            if (agent_mode) {
                // Interactive agent turn: multi-turn session runtime (shared tools/DB).
                const std::string model_name = job_context.options.model;
                job_context.options.quiet = true;
                std::shared_ptr<agent::AgentSessionRuntime> runtime = agent_runtime;
                if (!runtime) {
                    send_error = {ErrorCode::Internal, "agent session runtime is missing"};
                } else {
                    if (!runtime->prepared()) {
                        agent::SessionRuntimeOptions options;
                        options.workspace = ".";
                        options.allow_mutations = true;
                        options.interactive = true;
                        options.enable_session_db = true;
                        options.enable_agent_log = job_context.options.agent_log_enabled;
                        options.security_review_log_keep_runs =
                            job_context.options.security_review_log_keep_runs;
                        options.trusted_prompt_dir = job_context.options.trusted_prompt_dir;
                        options.max_source_code_file_size =
                            job_context.options.max_source_code_file_size;
                        send_error = runtime->prepare(job_context, token, {}, options);
                    }
                    if (send_error.ok()) {
                        agent::SessionTurnResult turn =
                            runtime->run_user_turn(job_context, agent_goal, token, {});
                        send_error = turn.error;
                        if (send_error.ok()) {
                            chat_result.content = turn.final_text;
                            chat_result.model = model_name;
                            if (!turn.final_text.empty()) {
                                TuiEvent delta;
                                delta.type = TuiEventType::Delta;
                                delta.text = turn.final_text;
                                events.push(std::move(delta));
                            }
                        }
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
            } else {
                event.type = TuiEventType::Error;
                event.error = send_error;
            }
            events.push(std::move(event));
        });
        status = agent_mode ? "Agent working (tools enabled)..." : "Waiting for response...";
    };

    auto start_turn_with_payload = [&](const std::string& history_content,
                                       const std::vector<provider::ImageInput>& images,
                                       const std::vector<provider::TextAttachment>& text_attachments,
                                       size_t pending_image_count) {
        if (session.read_only) {
            status = "Thread is read-only: " + session.read_only_reason;
            return;
        }
        if (!require_provider_model_for_send()) {
            return;
        }
        if (active_job != ActiveJob::None) {
            status = "A model job is already running";
            return;
        }
        pending_user = session.messages.size();
        pending_user_added_for_job = true;
        inflight_image_count = pending_image_count;
        session.messages.push_back({"user", history_content, images, text_attachments});
        start_assistant_response();
    };

    auto start_turn_with_pending_attachments = [&](const std::string& history_content) {
        std::vector<provider::TextAttachment> text_attachments;
        text_attachments.reserve(chat_attachments.size());
        for (const ChatAttachment& attachment : chat_attachments) {
            text_attachments.push_back(attachment.attachment);
        }
        start_turn_with_payload(history_content, pending_images, text_attachments,
                                pending_images.size());
    };

    // One-arg version for call sites and callbacks that don't involve attachment bodies.
    auto start_turn = [&](const std::string& history_content) {
        start_turn_with_payload(history_content, {}, {}, 0);
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
            status = detail::error_line(list_error);
            return;
        }
        if (thread_picker_threads.empty()) {
            status = "No saved chat threads";
            return;
        }
        thread_picker_selected = 0;
        pending_thread_delete = static_cast<size_t>(-1);
        mode = TuiMode::ThreadList;
        history_scroll = 0;
        status = ui::text_selector_status("Selected thread", thread_picker_selected,
                                          thread_picker_threads.size());
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

    TuiCommandHandlers command_handlers;
    command_handlers.quit = [&]() { quit = true; };
    command_handlers.start_history_edit = start_history_edit;
    command_handlers.start_thread_list = start_thread_list;
    command_handlers.start_new_chat_thread = [&](const std::string& name) { start_new_chat_thread(name); };
    command_handlers.open_provider_picker = open_provider_picker;
    command_handlers.apply_selected_provider = [&](const std::string& provider_target) {
        const bool applied =
            apply_selected_provider(context, session, show_thinking_traces, provider_target, status);
        if (applied) {
            loaded_thread_requires_provider_selection = false;
        }
        return applied;
    };
    command_handlers.start_store_save = start_store_save;
    command_handlers.start_models = start_models;
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
        if (context.options.agent) {
            leave_for(app::InteractiveUiTarget::Chat);
        } else {
            // chat → editor when interactive shell is available, else chat → agent
            if (interactive != nullptr) {
                leave_for(app::InteractiveUiTarget::Editor);
            } else {
                leave_for(app::InteractiveUiTarget::Agent);
            }
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
        if (loaded_thread_requires_provider_selection &&
            (text == "/model" || text.rfind("/model ", 0) == 0)) {
            status = "Thread setup requires /provider first; model selection follows";
            return;
        }
        handle_tui_command(text, command_context, command_handlers);
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
            start_models(ModelsRequestPurpose::Picker);
        }
    };
    picker_callbacks.on_model_selected = [&](const std::string& model_name) {
        const bool changed = context.options.model != model_name;
        context.options.model = model_name;
        session.model = model_name;
        if (changed) {
            context.options.reasoning = ReasoningSelection::automatic();
            context.options.reasoning_explicit = true;
        }
        loaded_thread_requires_provider_selection = false;
        if (have_cached_models) {
            provider::apply_context_window_from_models(context, cached_models);
        }
        picker_items.clear();
        picker_selected = 0;
        mode = TuiMode::Chat;
        status = provider_model_status_message(context, "ready");
        start_store_save();
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
        start_store_load(thread_id);
    };
    picker_callbacks.on_thread_new = [&]() {
        if (start_new_chat_thread()) {
            mode = TuiMode::Chat;
            thread_picker_threads.clear();
            thread_picker_selected = 0;
            pending_thread_delete = static_cast<size_t>(-1);
            history_scroll = 0;
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
            status = detail::error_line(remove_error);
        }
        mode = TuiMode::Chat;
    };
    picker_callbacks.on_remove_rejected = [&]() {
        mode = TuiMode::Chat;
        status = "Remove cancelled";
    };
    picker_callbacks.on_remove_retry = [&](const std::string& message) { status = message; };
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
                status = detail::error_line(remove_error);
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
        status = context.options.model.empty()
                     ? "Using current provider: " + provider::display_name_for_profile(context.profile.name)
                     : "Using current model: " +
                           ui::compact_model_name_for_display(context.options.model);
        start_store_save();
    };
    picker_callbacks.on_model_confirm_rejected = [&]() {
        Error context_error = apply_loaded_session_context(session);
        mode = TuiMode::Chat;
        if (context_error.ok()) {
            status = "Using thread model: " +
                     ui::compact_model_name_for_display(context.options.model);
            start_store_save();
        } else {
            status = detail::error_line(context_error);
        }
    };
    picker_callbacks.on_model_confirm_retry = [&](const std::string& message) { status = message; };

    auto submit_input = [&]() {
        std::string raw = input.text.str();
        const std::string text = app::detail::trim_ascii(raw);
        if (text.empty() && chat_attachments.empty()) {
            input = new_input_editor();
            return;
        }
        if (raw.find('\n') == std::string::npos && !text.empty() && text[0] == '/') {
            if (session.read_only) {
                input = new_input_editor();
                handle_command(text);
                return;
            }
            if (try_handle_chat_assist_command(text,
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

        input = new_input_editor();
        start_turn_with_pending_attachments(display_content);
    };

    if (!context.profile.offline && !context.options.has_context_tokens &&
        context.options.context_tokens <= 0) {
        provider::resolve_context_window(context);
    }
    if (!context.profile.offline && !context.options.model.empty() && !context.options.has_context_tokens &&
        context.options.context_tokens <= 0 && active_job == ActiveJob::None) {
        start_models(ModelsRequestPurpose::Preview);
    }

    refresh_startup_status();

    file_jobs.start_media_cleanup(context.options.media_auto_expiration_days,
                                  session.thread_id, true);

    if (provider::needs_interactive_model_selection(context)) {
        start_models(ModelsRequestPurpose::Picker);
    } else if (!app::detail::trim_ascii(context.options.prompt).empty()) {
        start_turn(context.options.prompt);
    }

    std::string visible_panel = panel_text();
    size_t render_frame = 0;
    ActivityKind activity_kind = ActivityKind::None;
    detail::render(session, input, status, history_scroll, show_thinking_traces, mode, visible_panel,
                   activity_kind, render_frame, syntax_highlight,
                   detail::RenderStyle{&context.options.tui_themes, theme, use_colors}, panel_title());
    while (!quit) {
        TuiEvent event;
        while (events.try_pop(event)) {
            bool completed_file_job = false;
            switch (event.type) {
                case TuiEventType::Delta:
                    if (pending_assistant != static_cast<size_t>(-1) && pending_assistant < session.messages.size()) {
                        session.messages[pending_assistant].content += event.text;
                    }
                    break;
                case TuiEventType::Done: {
                    model_job.join();
                    const bool should_regenerate = regenerate_after_cancel;
                    const size_t regenerate_erase_from = pending_user;
                    if (pending_assistant != static_cast<size_t>(-1) && pending_assistant < session.messages.size()) {
                        session.messages[pending_assistant].content = event.chat.content;
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
                    if (should_regenerate) {
                        start_queued_regeneration(regenerate_erase_from);
                    } else {
                        if (!context.options.has_context_tokens && context.options.context_tokens <= 0) {
                            const std::string selector =
                                !event.chat.model.empty() ? event.chat.model : context.options.model;
                            provider::resolve_context_window(context, selector);
                        }
                        status = event.compacted
                                     ? event.compaction.notice
                                     : generation_ready_status(context.profile.name,
                                                               context.options.model,
                                                               event.chat,
                                                               context.options.stream,
                                                               session.messages,
                                                               context.options.context_tokens);
                        start_save(context.options.save_chat_path, session, true);
                        start_store_save();
                    }
                    break;
                }
                case TuiEventType::Error: {
                    model_job.join();
                    const bool should_regenerate = regenerate_after_cancel && event.error.code == ErrorCode::Cancelled;
                    const size_t regenerate_erase_from = pending_user_added_for_job ? static_cast<size_t>(-1) : pending_user;
                    active_job = ActiveJob::None;
                    inflight_image_count = 0;
                    if (should_regenerate) {
                        // Regeneration will reuse the prior prompt content (which already included
                        // any folded attachments). Reset committed so we don't double-consume.
                        attachments_committed_for_turn = 0;
                        rollback_pending_turn();
                        start_queued_regeneration(regenerate_erase_from);
                    } else {
                        clear_queued_regeneration();
                        if (event.error.code == ErrorCode::Cancelled) {
                            // Prompt was sent and (partially) processed; consume the attachments
                            // that were used for it.
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
                            keep_cancelled_turn();
                            status = "Cancelled";
                            start_store_save();
                        } else {
                            // Error before/during; the user message is rolled back.
                            // Do not consume attachments; leave them for the user to re-send.
                            attachments_committed_for_turn = 0;
                            rollback_pending_turn();
                            status = detail::error_line(event.error);
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
                        status = detail::error_line(event.error);
                    }
                    break;
                case TuiEventType::LoadDone:
                    file_job.join();
                    completed_file_job = true;
                    if (event.error.ok()) {
                        session = std::move(event.session);
                        app::apply_system_prompt(session, context.options.system);
                        finish_loaded_session("Loaded " + event.text);
                    } else {
                        status = detail::error_line(event.error);
                    }
                    break;
                case TuiEventType::StoreSaveDone:
                    file_job.join();
                    completed_file_job = true;
                    if (event.error.ok()) {
                        apply_store_save_result(event.session);
                    } else if (event.error.code != ErrorCode::Cancelled) {
                        status = detail::error_line(event.error);
                    }
                    break;
                case TuiEventType::StoreLoadDone:
                    file_job.join();
                    completed_file_job = true;
                    if (event.error.ok()) {
                        session = std::move(event.session);
                        app::apply_system_prompt(session, context.options.system);
                        finish_loaded_session("Loaded thread: " +
                                              (session.name.empty() ? event.text : session.name));
                    } else {
                        status = detail::error_line(event.error);
                    }
                    break;
                case TuiEventType::MediaCleanupDone:
                    file_job.join();
                    completed_file_job = true;
                    if (!event.error.ok()) {
                        if (!event.automatic_cleanup || event.error.code != ErrorCode::Cancelled) {
                            status = detail::error_line(event.error);
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
                        status = insert_error.ok() ? "Inserted " + event.text + " at cursor"
                                                   : detail::error_line(insert_error);
                    } else {
                        status = detail::error_line(event.error);
                    }
                    break;
                case TuiEventType::AttachDone:
                    file_job.join();
                    completed_file_job = true;
                    if (event.error.ok() && event.image_attachment) {
                        pending_images.push_back(std::move(event.image));
                        status = "Attached image for next prompt: " + event.text + " (" +
                                 std::to_string(pending_images.size()) + " pending)";
                    } else if (event.error.ok() && event.text_attachment_ready) {
                        chat_attachments.push_back(
                            {event.attached_source, std::move(event.text_attachment)});
                        history_scroll = 0;
                        status = "Attached " + event.text + " (" +
                                 std::to_string(chat_attachments.size()) + " attachment" +
                                 (chat_attachments.size() == 1 ? "" : "s") + ")";
                    } else if (event.error.ok()) {
                        // Fallback for any legacy inserted_message path
                        if (!event.inserted_message.content.empty()) {
                            session.messages.push_back(std::move(event.inserted_message));
                        }
                        history_scroll = 0;
                        status = "Attached context from " + event.text;
                    } else {
                        status = detail::error_line(event.error);
                    }
                    break;
                case TuiEventType::FetchDone:
                    file_job.join();
                    completed_file_job = true;
                    if (event.error.ok()) {
                        session.messages.push_back(std::move(event.inserted_message));
                        history_scroll = 0;
                        status = "Fetched and inserted " + event.text;
                    } else {
                        status = detail::error_line(event.error);
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
                        status = detail::error_line(event.error);
                    }
                    break;
                case TuiEventType::ModelsDone:
                    model_job.join();
                    active_job = ActiveJob::None;
                    if (event.error.ok()) {
                        cached_models = std::move(event.models_result);
                        have_cached_models = true;
                        provider::apply_context_window_from_models(context, cached_models);
                    }
                    if (models_request_purpose == ModelsRequestPurpose::Picker) {
                        models_request_purpose = ModelsRequestPurpose::Preview;
                        if (!event.error.ok()) {
                            status = detail::error_line(event.error);
                        } else if (event.models.empty()) {
                            status = "No models returned";
                        } else if (ui::should_auto_select_only_model(event.models)) {
                            const std::string only_model = event.models.front();
                            picker_callbacks.on_model_selected(only_model);
                            status = provider_model_status_message(context, "only model auto-selected");
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
                    } else {
                        status = event.error.ok() ? join_models_preview(event.models)
                                                  : detail::error_line(event.error);
                    }
                    break;
                case TuiEventType::CompletionDone:
                    completion_job.join();
                    completion_pending = false;
                    if (event.completion_generation == completion_generation &&
                        event.completion.handled) {
                        input = std::move(event.completed_input);
                        path_completer = std::move(event.path_completer);
                        path_completer.set_assist_config(&ai_continue.assist_config);
                        status = editor::path_completion_status(event.completion);
                    }
                    break;
            }
            if (completed_file_job) {
                resume_deferred_store_save();
            }
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        timeval timeout{};
        timeout.tv_usec = 50000;
        const int ready = select(STDIN_FILENO + 1, &readfds, nullptr, nullptr, &timeout);
        if (ready < 0 && errno != EINTR) {
            status = std::string("terminal input error: ") + std::strerror(errno);
        }
        if (ready > 0 && FD_ISSET(STDIN_FILENO, &readfds)) {
            editor::TerminalInputEvent event;
            while (editor::read_terminal_input(event, 0)) {
                if (event.type == editor::TerminalInputType::BracketedPaste) {
                    if (mode == TuiMode::ReasoningConfirm) {
                        status = "Paste is not accepted by the reasoning confirmation; press y or n";
                        continue;
                    }
                    path_completer.reset();
                    ++completion_generation;
                    completion_job.cancel();
                    Error paste_error =
                        editor::paste_with_clipboard_preference(input,
                                                                editor::shared_clipboard(),
                                                                event.text);
                    status = paste_error.ok() ? "Pasted" : paste_error.message;
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
                                                 pending_thread_delete};
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
                    // DEL key: 127 or [3~ forward delete, or sometimes 8
                    if (ch == 127 || ch == 8) {
                        if (attachment_picker_selected < chat_attachments.size()) {
                            pending_attachment_delete = attachment_picker_selected;
                            mode = TuiMode::AttachmentDeleteConfirm;
                            status = "Delete attachment? y/n (Esc cancels)";
                        }
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
                            input, layout_for_terminal(screen.rows, screen.cols), history_scroll, status, true);
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
                            input, layout_for_terminal(screen.rows, screen.cols), history_scroll, status, true);
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
                        handle_escape(input, layout_for_terminal(screen.rows, screen.cols), history_scroll, status);
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
                    status = copy_error.ok() ? "Copied selection" : copy_error.message;
                    continue;
                }
                if (ch == 24) {
                    Error cut_error = input.cut_selection(editor::shared_clipboard());
                    status = cut_error.ok() ? "Cut selection" : cut_error.message;
                    continue;
                }
                if (ch == 22) {
                    Error paste_error =
                        editor::paste_with_clipboard_preference(input, editor::shared_clipboard(), "");
                    status = paste_error.ok() ? "Pasted" : paste_error.message;
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
                    scroll_chat_history_page_up(layout_for_terminal(screen.rows, screen.cols), history_scroll);
                    continue;
                }
                if (mode == TuiMode::Chat && ch == 4) {
                    const detail::TuiSize screen = detail::terminal_size();
                    scroll_chat_history_page_down(layout_for_terminal(screen.rows, screen.cols), history_scroll);
                    continue;
                }
                if (ch == 18 && mode == TuiMode::Chat) {
                    regenerate_last_turn();
                    continue;
                }
                if (ch == 20) {
                    set_thinking_trace_mode(!show_thinking_traces);
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
                if (ch == 0 && mode == TuiMode::Chat && active_job == ActiveJob::None) {
                    handle_chat_assist_continue_key(ai_continue.assist_config,
                                                    status,
                                                    chat_assist_callbacks);
                    continue;
                }
                if (ch == 16 && mode == TuiMode::Chat && active_job == ActiveJob::None) {
                    command_handlers.switch_to_editor();
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
                    detail::set_status_from_error(input.kill_to_line_end(editor::shared_clipboard()), status);
                    continue;
                }
                if (ch == 127 || ch == 8) {
                    detail::set_status_from_error(input.erase_before_cursor(), status);
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
        visible_panel = panel_text();
        activity_kind = active_job == ActiveJob::Chat
                            ? activity_kind_for_pending_assistant(session, pending_assistant,
                                                                  show_thinking_traces)
                            : ActivityKind::None;
        ++render_frame;
        detail::render(session, input, status, history_scroll, show_thinking_traces, mode, visible_panel,
                       activity_kind, render_frame, syntax_highlight,
                       detail::RenderStyle{&context.options.tui_themes, theme, use_colors}, panel_title());
    }

    model_job.cancel();
    completion_job.cancel();
    model_job.join();
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
            interactive->chat_session = session;
            interactive->chat_session_initialized = true;
            interactive->ai_continue = ai_continue;
            interactive->assist_config = ai_continue.assist_config;
            interactive->highlight_enabled = syntax_highlight;
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
