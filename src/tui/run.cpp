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
#include "chat/settings.hpp"
#include "pkchat/model_setting.hpp"
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

#include <cerrno>
#include <cstring>
#include <iostream>
#include <sys/select.h>
#include <unistd.h>

namespace pkchat::tui {

using detail::RenderStyle;

app::TuiRunResult run(provider::RequestContext context,
                      chat::Session session,
                      app::InteractiveSession* interactive) {
    const provider::RequestContext cli_context = context;
    if (context.options.model.empty() &&
        provider::profile_auto_selects_default_model(context.profile, context.base_url)) {
        Error model_err = app::choose_default_model(context);
        if (!model_err.ok()) {
            std::cerr << error_code_name(model_err.code) << ": " << model_err.message << "\n";
            return {app::exit_code_for(model_err.code), app::InteractiveUiTarget::Quit};
        }
        app::refresh_session_metadata(session, context);
    }
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
    std::string theme = "dark";
    context.options.tui_themes.normalize_name(context.options.tui_theme, theme);
    const bool use_colors = !context.options.no_colors;
    bool quit = false;
    bool switch_to_editor = false;
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
    TuiMode mode = TuiMode::Chat;
    size_t history_edit_index = static_cast<size_t>(-1);
    std::vector<chat::ThreadSummary> thread_picker_threads;
    size_t thread_picker_selected = 0;
    std::vector<std::string> picker_items;
    size_t picker_selected = 0;
    bool picker_cancel_quits = false;
    std::vector<ChatAttachment> chat_attachments;
    size_t attachment_picker_selected = 0;
    size_t pending_attachment_delete = static_cast<size_t>(-1);
    size_t attachments_committed_for_turn = 0;
    // Full (with bodies) content for the most recent user turn that had text attachments.
    // Used to send the real data to the model on initial send and immediate regenerates,
    // without storing the bodies in the visible/persisted chat history.
    std::string pending_full_model_content;
    // For queued regeneration that was triggered while a job was active.
    std::string queued_regen_full_content;
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
        pending_full_model_content.clear();
        queued_regen_full_content.clear();
        history_scroll = 0;
        if (loaded_session_differs_from_context(context, session)) {
            mode = TuiMode::ModelConfirm;
            status = loaded_label;
            return;
        }
        if (active_context_has_provider_selection(context)) {
            app::refresh_session_metadata(session, context);
            status = loaded_label;
            return;
        }
        Error context_error = apply_loaded_session_context(session);
        status = context_error.ok() ? loaded_label : detail::error_line(context_error);
    };

    auto start_new_thread_from_cli = [&]() {
        restore_cli_context(context, cli_context);
        show_thinking_traces = context.options.show_thinking_traces;
        session = chat::new_session(context);
        pending_images.clear();
        inflight_image_count = 0;
        chat_attachments.clear();
        attachment_picker_selected = 0;
        attachments_committed_for_turn = 0;
        pending_full_model_content.clear();
        queued_regen_full_content.clear();
        app::apply_system_prompt(session, context.options.system);
        history_scroll = 0;
    };

    auto panel_text = [&]() {
        if (mode == TuiMode::ThreadList) {
            return thread_picker_text(thread_picker_threads, thread_picker_selected);
        }
        if (mode == TuiMode::ProviderList) {
            return provider_picker_text(picker_items, picker_selected);
        }
        if (mode == TuiMode::ModelList) {
            return model_picker_text(picker_items, picker_selected);
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
            settings_text = chat::format_settings_panel(context.options);
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

    auto start_store_save = [&]() { file_jobs.start_store_save(); };

    auto start_store_load = [&](long long thread_id) { file_jobs.start_store_load(thread_id); };

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
        picker_items = selectable_provider_ids();
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

    auto refresh_startup_status = [&]() {
        if (!sqlite_available) {
            status = sqlite_unavailable_message();
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

    auto start_assistant_response = [&]() {
        active_job = ActiveJob::Chat;
        history_scroll = 0;
        pending_assistant = session.messages.size();
        session.messages.push_back({"assistant", ""});

        std::vector<provider::Message> request_messages = session.messages;
        request_messages.pop_back();

        // If this turn had text attachments, the history message contains only the
        // filenames (to keep chat history readable). Patch the *copy* we send to the
        // model with the actual full contents (including file data after the # marker).
        if (!pending_full_model_content.empty() &&
            !request_messages.empty() &&
            request_messages.back().role == "user") {
            request_messages.back().content = pending_full_model_content;
        }

        pkchat::context::PreparedMessages prepared = pkchat::context::prepare(
            request_messages,
            context.options.context_policy,
            context.options.max_context_bytes > 0
                ? static_cast<size_t>(context.options.max_context_bytes)
                : 0U);
        if (!prepared.error.ok()) {
            rollback_pending_turn();
            active_job = ActiveJob::None;
            status = detail::error_line(prepared.error);
            inflight_image_count = 0;
            attachments_committed_for_turn = 0;
            pending_full_model_content.clear();
            return;
        }
        if (pending_user != static_cast<size_t>(-1) && pending_user < session.messages.size()) {
            session.messages[pending_user].images.clear();
        }
        provider::RequestContext job_context = context;
        model_job.start([job_context, request_messages = std::move(prepared.messages),
                         compaction = std::move(prepared.event), compacted = prepared.compacted,
                         &events](runtime::CancellationToken token) mutable {
            provider::ChatResult chat;
            Error send_error = provider::send_chat_messages(
                job_context,
                request_messages,
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
                chat,
                token);
            TuiEvent event;
            if (send_error.ok()) {
                event.type = TuiEventType::Done;
                event.chat = std::move(chat);
                event.compaction = std::move(compaction);
                event.compacted = compacted;
            } else {
                event.type = TuiEventType::Error;
                event.error = send_error;
            }
            events.push(std::move(event));
        });
        status = "Waiting for response...";
    };

    auto start_turn_with_full = [&](const std::string& history_content,
                                      const std::string& full_model_content) {
        if (active_job != ActiveJob::None) {
            status = "A model job is already running";
            return;
        }
        pending_user = session.messages.size();
        pending_user_added_for_job = true;
        inflight_image_count = pending_images.size();
        session.messages.push_back({"user", history_content, pending_images});
        pending_full_model_content = full_model_content;
        start_assistant_response();
    };

    // One-arg version for call sites and callbacks that don't involve attachment bodies.
    auto start_turn = [&](const std::string& history_content) {
        start_turn_with_full(history_content, {});
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
        pending_full_model_content.clear();
        queued_regen_full_content.clear();
        start_assistant_response();
    };

    auto clear_queued_regeneration = [&]() {
        regenerate_after_cancel = false;
        queued_regeneration_prompt.clear();
    };

    auto start_queued_regeneration = [&](size_t erase_from) {
        const std::string prompt = queued_regeneration_prompt;
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
        start_turn_with_full(prompt, queued_regen_full_content);
        queued_regen_full_content.clear();
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
        start_new_thread_from_cli();
        session.name = app::detail::trim_ascii(name);
        status = session.name.empty() ? "New chat thread" : "New chat thread: " + session.name;
        start_store_save();
        return true;
    };

    auto regenerate_last_turn = [&]() {
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
            // Stash the full (bodies) version for the queued regen so the model receives data,
            // while the history will re-use the short "prompt" (names only).
            queued_regen_full_content = pending_full_model_content;
            model_job.cancel();
            status = "Cancelling before regenerate...";
            return;
        }

        const RegenerationPlan plan = regeneration_plan_for_session(session);
        if (!plan.available || app::detail::trim_ascii(plan.prompt).empty()) {
            status = "No previous user prompt to regenerate";
            return;
        }
        session.messages.erase(session.messages.begin() + static_cast<long>(plan.erase_from), session.messages.end());
        // Pass any stashed full content (for the turn being regenerated) so the model gets the bodies.
        // The first arg is the short history content (names only) that will be stored.
        start_turn_with_full(plan.prompt, pending_full_model_content);
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
            switch_to_editor = true;
            quit = true;
            return true;
        };

    TuiCommandHandlers command_handlers;
    command_handlers.quit = [&]() { quit = true; };
    command_handlers.start_history_edit = start_history_edit;
    command_handlers.start_thread_list = start_thread_list;
    command_handlers.start_new_chat_thread = [&](const std::string& name) { start_new_chat_thread(name); };
    command_handlers.open_provider_picker = open_provider_picker;
    command_handlers.apply_selected_provider = [&](const std::string& provider_target) {
        return apply_selected_provider(context, session, show_thinking_traces, provider_target, status);
    };
    command_handlers.start_store_save = start_store_save;
    command_handlers.start_models = start_models;
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
    command_handlers.set_thinking_trace_mode = set_thinking_trace_mode;
    command_handlers.switch_to_editor = [&]() {
        if (interactive == nullptr) {
            status = "Editor mode is unavailable";
            return;
        }
        if (active_job != ActiveJob::None) {
            status = "Cannot switch to editor while a model job is running";
            return;
        }
        switch_to_editor = true;
        quit = true;
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
                                      attachments_committed_for_turn,
                                      pending_full_model_content};

    auto handle_command = [&](const std::string& text) {
        handle_tui_command(text, command_context, command_handlers);
    };

    TuiPickerCallbacks picker_callbacks;
    picker_callbacks.on_provider_selected = [&](const std::string& provider_name) {
        if (apply_selected_provider(context, session, show_thinking_traces, provider_name, status)) {
            picker_items.clear();
            picker_selected = 0;
            mode = TuiMode::Chat;
            status = "Provider set to " + provider::display_name_for_profile(context.profile.name);
            start_store_save();
            start_models(ModelsRequestPurpose::Picker);
        }
    };
    picker_callbacks.on_model_selected = [&](const std::string& model_name) {
        context.options.model = model_name;
        session.model = model_name;
        if (have_cached_models) {
            provider::apply_context_window_from_models(context, cached_models);
        }
        picker_items.clear();
        picker_selected = 0;
        mode = TuiMode::Chat;
        status = provider_model_status_message(context, "ready");
        start_store_save();
    };
    picker_callbacks.on_thread_selected = [&](long long thread_id) {
        mode = TuiMode::Chat;
        thread_picker_threads.clear();
        thread_picker_selected = 0;
        start_store_load(thread_id);
    };
    picker_callbacks.on_thread_new = [&]() {
        if (start_new_chat_thread()) {
            mode = TuiMode::Chat;
            thread_picker_threads.clear();
            thread_picker_selected = 0;
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
    picker_callbacks.on_model_confirm_accepted = [&]() {
        app::refresh_session_metadata(session, context);
        mode = TuiMode::Chat;
        status = context.options.model.empty()
                     ? "Using current provider: " + provider::display_name_for_profile(context.profile.name)
                     : "Using current model: " + context.options.model;
        start_store_save();
    };
    picker_callbacks.on_model_confirm_rejected = [&]() {
        Error context_error = apply_loaded_session_context(session);
        mode = TuiMode::Chat;
        status = context_error.ok() ? "Using thread model: " + context.options.model
                                    : detail::error_line(context_error);
        start_store_save();
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

        // User is sending a fresh prompt (not a regeneration). Any stashed full content
        // for a previous turn is no longer needed (user has moved on to a new prompt).
        pending_full_model_content.clear();
        queued_regen_full_content.clear();

        const std::string typed_prompt = raw;

        std::string display_content = typed_prompt;
        std::string full_model_content;

        if (!chat_attachments.empty()) {
            // Build the FULL content for the MODEL (includes actual file data after #).
            // This must never be stored in session.messages (to avoid flooding history).
            full_model_content = typed_prompt;
            if (!full_model_content.empty() && full_model_content.back() != '\n') {
                full_model_content += '\n';
            }
            full_model_content += "#\n\n";
            for (size_t i = 0; i < chat_attachments.size(); ++i) {
                const auto& att = chat_attachments[i];
                full_model_content += "---" + att.source + "---\n";
                full_model_content += att.content;
                if (!att.content.empty() && att.content.back() != '\n') {
                    full_model_content += '\n';
                }
                if (i + 1 < chat_attachments.size()) {
                    full_model_content += '\n';  // one line break between attached files
                }
            }

            // Build the DISPLAY content for HISTORY: only the typed text + filenames (no bodies).
            if (!display_content.empty()) {
                display_content += "\n\n";
            }
            display_content += "Attached files (in order):\n";
            for (const auto& att : chat_attachments) {
                display_content += "- " + att.source + "\n";
            }

            attachments_committed_for_turn = chat_attachments.size();
            // The bodies stay in chat_attachments (and the full_model_content copy above)
            // until after the model responds and the user starts a new prompt.
        }

        pending_full_model_content = full_model_content;

        input = new_input_editor();
        start_turn_with_full(display_content, full_model_content);
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

    if (should_open_startup_provider_picker(context)) {
        open_provider_picker(false);
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
                            pending_full_model_content.clear();
                            rollback_pending_turn();
                            status = detail::error_line(event.error);
                        }
                    }
                    break;
                }
                case TuiEventType::SaveDone:
                    file_job.join();
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
                    if (event.error.ok()) {
                        if (session.thread_id == 0 || session.thread_id == event.session.thread_id) {
                            session.thread_id = event.session.thread_id;
                            session.name = event.session.name;
                            session.created_at = event.session.created_at;
                            session.updated_at = event.session.updated_at;
                        }
                    } else if (event.error.code != ErrorCode::Cancelled) {
                        status = detail::error_line(event.error);
                    }
                    break;
                case TuiEventType::StoreLoadDone:
                    file_job.join();
                    if (event.error.ok()) {
                        session = std::move(event.session);
                        app::apply_system_prompt(session, context.options.system);
                        finish_loaded_session("Loaded thread: " +
                                              (session.name.empty() ? event.text : session.name));
                    } else {
                        status = detail::error_line(event.error);
                    }
                    break;
                case TuiEventType::InsertDone:
                    file_job.join();
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
                    if (event.error.ok() && event.image_attachment) {
                        pending_images.push_back(std::move(event.image));
                        status = "Attached image for next prompt: " + event.text + " (" +
                                 std::to_string(pending_images.size()) + " pending)";
                    } else if (event.error.ok() && !event.attached_content.empty()) {
                        // New chat attachment list behavior
                        chat_attachments.push_back({event.attached_source, std::move(event.attached_content)});
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
                                                 input.text.empty()};
                if (handle_tui_picker_input(ch, picker_state, picker_callbacks)) {
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
    remove_empty_thread_on_exit();
    file_job.cancel();
    file_job.join();
    if (switch_to_editor && interactive != nullptr) {
        interactive->context = context;
        interactive->chat_session = session;
        interactive->chat_session_initialized = true;
        interactive->ai_continue = ai_continue;
        interactive->assist_config = ai_continue.assist_config;
        interactive->highlight_enabled = syntax_highlight;
        return {0, app::InteractiveUiTarget::Editor};
    }
    return {0, app::InteractiveUiTarget::Quit};
}

}  // namespace pkchat::tui
