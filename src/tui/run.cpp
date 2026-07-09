#include "tui/activity.hpp"
#include "tui/tui.hpp"
#include "tui/events.hpp"
#include "tui/input_handlers.hpp"
#include "tui/session_load.hpp"
#include "tui/terminal.hpp"
#include "tui/detail/render.hpp"

#include "app/app.hpp"
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
#include "ui/text_selector.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <sys/select.h>
#include <unistd.h>

namespace pkchat::tui {

using detail::RenderStyle;

int run(provider::RequestContext context, chat::Session session) {
    const provider::RequestContext cli_context = context;
    TerminalSession terminal;
    Error err = terminal.enter();
    if (!err.ok()) {
        std::cerr << error_code_name(err.code) << ": " << err.message << "\n";
        return app::exit_code_for(err.code);
    }

    runtime::EventQueue<TuiEvent> events;
    runtime::JobHandle model_job;
    runtime::JobHandle file_job;
    runtime::JobHandle completion_job;
    ActiveJob active_job = ActiveJob::None;
    const size_t input_undo_limit = static_cast<size_t>(std::max(0, context.options.editor_undo_limit));
    auto new_input_editor = [&]() {
        return detail::empty_input_editor(input_undo_limit);
    };
    editor::EditorState input = new_input_editor();
    editor::ContextualCompleter path_completer;
    size_t completion_generation = 0;
    bool completion_pending = false;
    std::string status = ready_status();
    ThemeName theme = ThemeName::Dark;
    parse_theme_name(context.options.tui_theme, theme);
    const bool use_colors = !context.options.no_colors;
    bool quit = false;
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
    ModelsRequestPurpose models_request_purpose = ModelsRequestPurpose::Preview;
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
        if (completion_pending) {
            status = "Tab completion is still running";
            return;
        }

        editor::EditorState completion_input = input;
        const size_t generation = completion_generation;
        completion_job.start(
            [completion_input = std::move(completion_input), generation, &events](
                runtime::CancellationToken token) mutable {
                TuiEvent event;
                event.type = TuiEventType::CompletionDone;
                event.completion_generation = generation;
                event.completion = event.path_completer.complete(
                    completion_input, [&token]() { return token.cancelled(); });
                event.completed_input = std::move(completion_input);
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
        if (path.empty()) {
            return;
        }
        if (file_job.running()) {
            if (!quiet_success) {
                status = "A file job is already running";
            }
            return;
        }
        file_job.start([path, snapshot = std::move(snapshot), quiet_success, &events](
                           runtime::CancellationToken token) mutable {
            TuiEvent event;
            event.type = TuiEventType::SaveDone;
            event.text = path;
            event.quiet_success = quiet_success;
            if (token.cancelled()) {
                event.error = {ErrorCode::Cancelled, "save cancelled: " + path};
            } else {
                event.error = chat::save_session_atomic(path, std::move(snapshot));
            }
            events.push(std::move(event));
        });
    };

    auto start_load = [&](const std::string& path) {
        if (file_job.running()) {
            status = "A file job is already running";
            return;
        }
        file_job.start([path, &events](runtime::CancellationToken token) mutable {
            TuiEvent event;
            event.type = TuiEventType::LoadDone;
            event.text = path;
            if (token.cancelled()) {
                event.error = {ErrorCode::Cancelled, "load cancelled: " + path};
            } else {
                event.error = chat::load_session(path, event.session);
            }
            events.push(std::move(event));
        });
        status = "Loading " + path;
    };

    auto start_store_load = [&](long long thread_id) {
        if (!sqlite_available) {
            status = sqlite_unavailable_message();
            return;
        }
        if (file_job.running()) {
            status = "A file job is already running";
            return;
        }
        const std::string db_path = sqlite_path;
        file_job.start([db_path, thread_id, &events](runtime::CancellationToken token) mutable {
            TuiEvent event;
            event.type = TuiEventType::StoreLoadDone;
            event.text = std::to_string(thread_id);
            if (token.cancelled()) {
                event.error = {ErrorCode::Cancelled, "thread load cancelled: " + event.text};
            } else {
                chat::SqliteStore store;
                event.error = store.open(db_path);
                if (event.error.ok()) {
                    event.error = store.load_session(thread_id, event.session);
                }
            }
            events.push(std::move(event));
        });
        status = "Loading thread " + std::to_string(thread_id);
    };

    auto start_store_save = [&]() {
        if (!sqlite_available) {
            return;
        }
        if (file_job.running()) {
            return;
        }
        chat::Session snapshot = session;
        app::refresh_session_metadata(snapshot, context);
        const std::string db_path = sqlite_path;
        file_job.start([db_path, snapshot = std::move(snapshot), &events](
                           runtime::CancellationToken token) mutable {
            TuiEvent event;
            event.type = TuiEventType::StoreSaveDone;
            if (token.cancelled()) {
                event.error = {ErrorCode::Cancelled, "SQLite autosave cancelled"};
            } else {
                chat::SqliteStore store;
                event.error = store.open(db_path);
                if (event.error.ok()) {
                    event.error = store.save_session(snapshot);
                }
                event.session = std::move(snapshot);
            }
            events.push(std::move(event));
        });
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

    auto start_insert = [&](const std::string& path) {
        if (file_job.running()) {
            status = "A file job is already running";
            return;
        }
        if (path.empty()) {
            status = "Usage: /insert PATH or /attach PATH";
            return;
        }
        if (path == "stdin") {
            status = "stdin input is only supported by non-interactive --input and --attach";
            return;
        }
        input::FileType type;
        Error type_error = input::classify_file_type(path, type);
        if (!type_error.ok()) {
            status = detail::error_line(type_error);
            return;
        }
        if (type.kind == input::Kind::Image) {
            Error capability_error = provider::validate_image_input(context);
            if (!capability_error.ok()) {
                status = detail::error_line(capability_error);
                return;
            }
        }
        const long text_limit = context.options.max_input_bytes;
        const long image_limit = context.options.max_image_bytes;
        file_job.start([path, type, text_limit, image_limit, &events](runtime::CancellationToken token) mutable {
            TuiEvent event;
            event.type = TuiEventType::InsertDone;
            event.text = path;
            if (type.kind == input::Kind::Image) {
                event.image_attachment = true;
                if (image_limit <= 0) {
                    event.error = {ErrorCode::BadArgs, "--max-image-bytes must be greater than zero"};
                } else {
                    input::ImageData loaded;
                    event.error = input::load_image_file(
                        path, type, static_cast<size_t>(image_limit), loaded, token);
                    if (event.error.ok()) {
                        event.image = {loaded.mime_type, std::move(loaded.base64_data)};
                    }
                }
            } else if (text_limit <= 0) {
                event.error = {ErrorCode::BadArgs, "--max-input-bytes must be greater than zero"};
            } else {
                input::TextContext loaded;
                event.error = input::load_text_context_file(
                    path, static_cast<size_t>(text_limit), loaded, token);
                if (event.error.ok()) {
                    event.inserted_message = {"user", input::text_context_message(loaded)};
                }
            }
            events.push(std::move(event));
        });
        status = (type.kind == input::Kind::Image ? "Attaching " : "Inserting ") + path + "...";
    };

    auto start_fetch = [&](const std::string& url) {
        if (file_job.running()) {
            status = "A file job is already running";
            return;
        }
        if (url.empty()) {
            status = "Usage: /fetch URL";
            return;
        }
        fetch::Options options;
        options.connect_timeout_seconds = context.options.connect_timeout_seconds;
        options.timeout_seconds = context.options.timeout_seconds > 0 ? context.options.timeout_seconds : 30;
        options.max_bytes = context.options.max_fetch_bytes;
        options.proxy = context.options.proxy;
        options.insecure_tls = context.options.insecure_tls;
        options.trace_http = context.options.trace_http;
        options.allow_private = context.options.allow_private_url_fetch;
        file_job.start([url, options, &events](runtime::CancellationToken token) mutable {
            TuiEvent event;
            event.type = TuiEventType::FetchDone;
            event.text = url;
            std::string markdown;
            event.error = fetch::fetch_markdown(url, options, markdown, token);
            if (event.error.ok()) {
                input::TextContext fetched;
                fetched.source = "URL " + url;
                fetched.kind = input::Kind::Markdown;
                fetched.content = std::move(markdown);
                event.inserted_message = {"user", input::text_context_message(fetched)};
            }
            events.push(std::move(event));
        });
        status = "Fetching " + url + "...";
    };

    auto start_search = [&](const std::string& query) {
        if (file_job.running()) {
            status = "A file job is already running";
            return;
        }
        if (query.empty()) {
            status = "Usage: /search QUERY";
            return;
        }
        search::Options options = search::options_for(context.options);
        file_job.start([query, options, &events](runtime::CancellationToken token) mutable {
            TuiEvent event;
            event.type = TuiEventType::SearchDone;
            event.text = query;
            search::SearchResponse response;
            event.error = search::search(query, options, response, token);
            if (event.error.ok()) {
                event.inserted_message = {"user", search::format_context_message(query, response)};
            }
            events.push(std::move(event));
        });
        status = "Searching " + query + "...";
    };

    auto start_models = [&](ModelsRequestPurpose purpose = ModelsRequestPurpose::Preview) {
        if (active_job != ActiveJob::None) {
            status = "A model job is already running";
            return;
        }
        models_request_purpose = purpose;
        active_job = ActiveJob::Models;
        provider::RequestContext job_context = context;
        model_job.start([job_context, &events](runtime::CancellationToken token) mutable {
            TuiEvent event;
            event.type = TuiEventType::ModelsDone;
            provider::ModelsResult models;
            event.error = provider::list_models(job_context, models, token);
            event.models = std::move(models.model_ids);
            events.push(std::move(event));
        });
        status = purpose == ModelsRequestPurpose::Picker ? "Loading models..." : "Listing models...";
    };

    auto apply_selected_provider = [&](const std::string& provider_target) -> bool {
        cli::Options next = context.options;
        provider::apply_provider_target(next, provider_target);
        next.model.clear();
        provider::ContextResult rebuilt = provider::build_context(next);
        if (!rebuilt.error.ok()) {
            status = detail::error_line(rebuilt.error);
            return false;
        }
        context = std::move(rebuilt.context);
        show_thinking_traces = context.options.show_thinking_traces;
        app::refresh_session_metadata(session, context);
        return true;
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

    auto start_turn = [&](const std::string& prompt) {
        if (active_job != ActiveJob::None) {
            status = "A model job is already running";
            return;
        }
        pending_user = session.messages.size();
        pending_user_added_for_job = true;
        inflight_image_count = pending_images.size();
        session.messages.push_back({"user", prompt, pending_images});
        start_assistant_response();
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
        start_turn(prompt);
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
        start_turn(plan.prompt);
        status = "Regenerating...";
    };

    auto handle_command = [&](const std::string& text) {
        if (text == "/quit" || text == "/exit") {
            quit = true;
            return;
        }
        if (text == "/help") {
            if (help_text.empty()) {
                settings_text.clear();
                help_text =
                    "/help (hide/show this panel)\n"
                    "/quit or /exit\n"
                    "/clear\n"
                    "/edit\n"
                    "/list (Ctrl+L; N new thread)\n"
                    "/new [NAME]\n"
                    "/provider [PROVIDER]\n"
                    "/models\n"
                    "/model [MODEL]\n"
                    "/system [TEXT]\n"
                    "/setting (hide/show current settings)\n"
                    "/setting NAME=VALUE\n"
                    "/setting general|coding|instruct|creative\n"
                    "/clone\n"
                    "/save [PATH]\n"
                    "/load PATH\n"
                    "/remove\n"
                    "/remove-empty\n"
                    "/pop\n"
                    "/response\n"
                    "/insert PATH or /attach PATH (text or image)\n"
                    "/fetch URL\n"
                    "/search QUERY\n"
                    "/theme [dark|light]\n"
                    "/thinking [trace|notrace]";
                status = "Help shown; /help hides it";
            } else {
                help_text.clear();
                status = "Help hidden";
            }
            history_scroll = 0;
            return;
        }
        if (text.rfind("/thinking", 0) == 0) {
            const std::string requested = app::detail::trim_ascii(text.substr(9));
            if (requested.empty()) {
                status = std::string("Thinking traces: ") + (show_thinking_traces ? "trace" : "notrace") +
                         ". Use /thinking trace or /thinking notrace";
                return;
            }
            if (requested == "trace") {
                set_thinking_trace_mode(true);
                return;
            }
            if (requested == "notrace") {
                set_thinking_trace_mode(false);
                return;
            }
            status = "Usage: /thinking trace|notrace";
            return;
        }
        if (text.rfind("/theme", 0) == 0) {
            const std::string requested = app::detail::trim_ascii(text.substr(6));
            if (requested.empty()) {
                status = std::string("Theme: ") + theme_name(theme) + ". Available: dark, light";
                if (!use_colors) {
                    status += " (colors disabled by --nocolors)";
                }
                return;
            }
            ThemeName next = theme;
            if (!parse_theme_name(requested, next)) {
                status = "Unknown theme: " + requested + ". Available: dark, light";
                return;
            }
            theme = next;
            status = std::string("Theme set to ") + theme_name(theme);
            if (!use_colors) {
                status += " (colors disabled by --nocolors)";
            }
            return;
        }
        if (text == "/edit") {
            start_history_edit();
            return;
        }
        if (text == "/clear") {
            session.messages.clear();
            pending_images.clear();
            inflight_image_count = 0;
            app::apply_system_prompt(session, context.options.system);
            history_scroll = 0;
            status = "Chat history cleared";
            return;
        }
        if (text == "/list") {
            start_thread_list();
            return;
        }
        if (text == "/new" || text.rfind("/new ", 0) == 0) {
            start_new_chat_thread(text.size() <= 4 ? "" : text.substr(4));
            return;
        }
        if (text == "/provider" || text.rfind("/provider ", 0) == 0) {
            const std::string provider_name = app::detail::trim_ascii(text.substr(9));
            if (provider_name.empty()) {
                open_provider_picker(false);
                return;
            }
            if (!apply_selected_provider(provider_name)) {
                return;
            }
            start_store_save();
            start_models(ModelsRequestPurpose::Picker);
            return;
        }
        if (text == "/model" || text.rfind("/model ", 0) == 0) {
            const std::string model = app::detail::trim_ascii(text.substr(6));
            if (model.empty()) {
                if (context.profile.offline) {
                    status = "Select a provider with /provider first";
                    return;
                }
                start_models(ModelsRequestPurpose::Picker);
                return;
            }
            context.options.model = model;
            session.model = model;
            status = provider_model_status_message(context, "ready");
            start_store_save();
            return;
        }
        if (text == "/system" || text.rfind("/system ", 0) == 0) {
            const std::string system_text = text.size() <= 7 ? "" : app::detail::trim_ascii(text.substr(7));
            if (system_text.empty()) {
                if (active_job != ActiveJob::None) {
                    status = "Cannot edit system prompt while a model job is running";
                    return;
                }
                input = editor::EditorState::from_text(chat::current_system_prompt(session));
                input.set_undo_limit(input_undo_limit);
                mode = TuiMode::SystemEdit;
                status = "Editing system prompt";
                return;
            }
            app::replace_system_prompt(session, system_text);
            context.options.system = system_text;
            persist_settings_change("System prompt updated");
            return;
        }
        if (text == "/clone") {
            if (active_job != ActiveJob::None) {
                status = "Cannot clone a thread while a model job is running";
                return;
            }
            chat::Session cloned = session;
            cloned.thread_id = 0;
            cloned.created_at = chat::current_timestamp_utc();
            cloned.updated_at = cloned.created_at;
            if (cloned.name.empty()) {
                cloned.name = "Copy";
            } else {
                cloned.name += " (copy)";
            }
            session = std::move(cloned);
            history_scroll = 0;
            persist_settings_change("Cloned chat thread");
            return;
        }
        if (text == "/setting" || text.rfind("/setting ", 0) == 0) {
            const std::string requested = text.size() <= 8 ? "" : app::detail::trim_ascii(text.substr(8));
            if (requested.empty()) {
                if (settings_text.empty()) {
                    help_text.clear();
                    settings_text = chat::format_settings_panel(context.options);
                    status = "Settings shown; /setting hides them";
                } else {
                    settings_text.clear();
                    status = "Settings hidden";
                }
                history_scroll = 0;
                return;
            }
            if (requested == "general" || requested == "coding" || requested == "instruct" ||
                requested == "creative") {
                if (context.options.model.empty()) {
                    status = "Set a model with /model before applying a purpose preset";
                    return;
                }
                const ModelSetting* preset =
                    chat::find_model_setting(context.options.model, requested, context.options.model_settings);
                if (preset == nullptr) {
                    status = "No [Model-setting] preset for model " + context.options.model + " purpose " + requested;
                    return;
                }
                Error preset_error = chat::apply_model_setting_preset(context.options, *preset);
                if (!preset_error.ok()) {
                    status = detail::error_line(preset_error);
                    return;
                }
                if (!preset->default_system_prompt.empty()) {
                    app::replace_system_prompt(session, preset->default_system_prompt);
                    context.options.system = preset->default_system_prompt;
                }
                persist_settings_change("Applied " + requested + " settings for " + context.options.model);
                refresh_settings_panel_if_visible();
                return;
            }
            const size_t equals = requested.find('=');
            if (equals == std::string::npos) {
                status = "Usage: /setting NAME=VALUE or /setting general|coding|instruct|creative";
                return;
            }
            const std::string name = app::detail::trim_ascii(requested.substr(0, equals));
            const std::string value = app::detail::trim_ascii(requested.substr(equals + 1));
            if (name.empty()) {
                status = "Usage: /setting NAME=VALUE";
                return;
            }
            Error setting_error = chat::apply_chat_setting(context.options, name, value);
            if (!setting_error.ok()) {
                status = setting_error.message;
                return;
            }
            persist_settings_change("Updated " + name);
            refresh_settings_panel_if_visible();
            return;
        }
        if (text == "/models") {
            start_models();
            return;
        }
        if (text.rfind("/save", 0) == 0) {
            std::string path = app::detail::trim_ascii(text.substr(5));
            if (path.empty()) {
                path = context.options.save_chat_path;
            }
            if (path.empty()) {
                status = "Usage: /save PATH";
                return;
            }
            start_save(path, session);
            status = "Saving " + path;
            return;
        }
        if (text.rfind("/load", 0) == 0) {
            const std::string path = app::detail::trim_ascii(text.substr(5));
            if (path.empty()) {
                status = "Usage: /load PATH";
                return;
            }
            start_load(path);
            return;
        }
        if (text == "/remove") {
            if (!sqlite_available) {
                status = sqlite_unavailable_message();
                return;
            }
            if (session.thread_id <= 0) {
                status = "No saved thread to remove";
                return;
            }
            mode = TuiMode::RemoveConfirm;
            status = "Confirm removal with y or cancel with n/Esc";
            return;
        }
        if (text == "/remove-empty") {
            if (!sqlite_available) {
                status = sqlite_unavailable_message();
                return;
            }
            if (active_job != ActiveJob::None) {
                status = "Cannot remove empty threads while a model job is running";
                return;
            }
            long long deleted_count = 0;
            bool current_removed = false;
            Error remove_error =
                sqlite_store.soft_delete_empty_threads(deleted_count, session.thread_id, current_removed);
            if (!remove_error.ok()) {
                status = detail::error_line(remove_error);
                return;
            }
            if (current_removed) {
                sqlite_store.set_last_thread_id(0);
                start_new_thread_from_cli();
            }
            if (deleted_count == 0) {
                status = "No empty threads to remove";
            } else if (deleted_count == 1) {
                status = current_removed ? "Removed 1 empty thread and started a new chat"
                                         : "Removed 1 empty thread";
            } else {
                status = current_removed
                             ? "Removed " + std::to_string(deleted_count) +
                                   " empty threads and started a new chat"
                             : "Removed " + std::to_string(deleted_count) + " empty threads";
            }
            return;
        }
        if (text == "/pop") {
            pop_last_message();
            return;
        }
        if (text == "/response") {
            start_response_to_unanswered_user();
            return;
        }
        if (text == "/insert" || text.rfind("/insert ", 0) == 0 ||
            text == "/attach" || text.rfind("/attach ", 0) == 0) {
            start_insert(app::detail::trim_ascii(text.substr(7)));
            return;
        }
        if (text == "/fetch" || text.rfind("/fetch ", 0) == 0) {
            start_fetch(app::detail::trim_ascii(text.substr(6)));
            return;
        }
        if (text == "/search" || text.rfind("/search ", 0) == 0) {
            start_search(app::detail::trim_ascii(text.substr(7)));
            return;
        }
        status = "Unknown command: " + text;
    };

    auto submit_input = [&]() {
        const std::string raw = input.text.str();
        const std::string text = app::detail::trim_ascii(raw);
        if (text.empty()) {
            input = new_input_editor();
            return;
        }
        if (raw.find('\n') == std::string::npos && text[0] == '/') {
            input = new_input_editor();
            handle_command(text);
            return;
        }
        if (active_job != ActiveJob::None) {
            status = "A model job is already running";
            return;
        }
        input = new_input_editor();
        start_turn(raw);
    };

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
                   activity_kind, render_frame, detail::RenderStyle{theme, use_colors}, panel_title());
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
                    pending_user = static_cast<size_t>(-1);
                    pending_assistant = static_cast<size_t>(-1);
                    pending_user_added_for_job = false;
                    active_job = ActiveJob::None;
                    if (should_regenerate) {
                        start_queued_regeneration(regenerate_erase_from);
                    } else {
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
                        rollback_pending_turn();
                        start_queued_regeneration(regenerate_erase_from);
                    } else {
                        clear_queued_regeneration();
                        if (event.error.code == ErrorCode::Cancelled) {
                            keep_cancelled_turn();
                            status = "Cancelled";
                            start_store_save();
                        } else {
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
                    if (event.error.ok() && event.image_attachment) {
                        pending_images.push_back(std::move(event.image));
                        status = "Attached image for next prompt: " + event.text + " (" +
                                 std::to_string(pending_images.size()) + " pending)";
                    } else if (event.error.ok()) {
                        session.messages.push_back(std::move(event.inserted_message));
                        history_scroll = 0;
                        status = "Inserted context from " + event.text;
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
                if (mode == TuiMode::ProviderList || mode == TuiMode::ModelList) {
                    if (ch == 17) {
                        quit = true;
                        continue;
                    }
                    if (ch == 27) {
                        const std::string selection_label =
                            mode == TuiMode::ProviderList ? "Selected provider" : "Selected model";
                        const PickerEscapeResult result = handle_list_picker_escape(
                            picker_items.size(), picker_selected, status, selection_label);
                        if (result == PickerEscapeResult::Cancelled) {
                            const bool provider_picker = mode == TuiMode::ProviderList;
                            picker_items.clear();
                            picker_selected = 0;
                            if (picker_cancel_quits) {
                                quit = true;
                            } else {
                                mode = TuiMode::Chat;
                            }
                            status = provider_picker ? "Provider selection cancelled"
                                                     : "Model selection cancelled";
                        }
                        continue;
                    }
                    if (ch == '\r' || ch == '\n') {
                        if (picker_selected < picker_items.size()) {
                            if (mode == TuiMode::ProviderList) {
                                const std::string provider_name = picker_items[picker_selected];
                                if (apply_selected_provider(provider_name)) {
                                    picker_items.clear();
                                    picker_selected = 0;
                                    mode = TuiMode::Chat;
                                    status = "Provider set to " +
                                             provider::display_name_for_profile(context.profile.name);
                                    start_store_save();
                                    start_models(ModelsRequestPurpose::Picker);
                                }
                            } else {
                                context.options.model = picker_items[picker_selected];
                                session.model = context.options.model;
                                picker_items.clear();
                                picker_selected = 0;
                                mode = TuiMode::Chat;
                                status = provider_model_status_message(context, "ready");
                                start_store_save();
                            }
                        }
                        continue;
                    }
                    continue;
                }
                if (mode == TuiMode::ThreadList) {
                    if (ch == 17) {
                        quit = true;
                        continue;
                    }
                    if (ch == 27) {
                        handle_thread_picker_escape(thread_picker_threads,
                                                    thread_picker_selected,
                                                    mode,
                                                    status);
                        continue;
                    }
                    if (ch == '\r' || ch == '\n') {
                        if (thread_picker_selected < thread_picker_threads.size()) {
                            const long long thread_id = thread_picker_threads[thread_picker_selected].id;
                            mode = TuiMode::Chat;
                            thread_picker_threads.clear();
                            thread_picker_selected = 0;
                            start_store_load(thread_id);
                        }
                        continue;
                    }
                    if (ch == 'n' || ch == 'N') {
                        if (start_new_chat_thread()) {
                            mode = TuiMode::Chat;
                            thread_picker_threads.clear();
                            thread_picker_selected = 0;
                            history_scroll = 0;
                        }
                        continue;
                    }
                    continue;
                }
                if (mode == TuiMode::RemoveConfirm) {
                    if (ch == 17) {
                        quit = true;
                        continue;
                    }
                    if (ch == 27 || ch == 'n' || ch == 'N') {
                        mode = TuiMode::Chat;
                        status = "Remove cancelled";
                        continue;
                    }
                    if (ch == 'y' || ch == 'Y') {
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
                        continue;
                    }
                    status = "Press y to remove, n or Esc to cancel";
                    continue;
                }
                if (mode == TuiMode::ModelConfirm) {
                    if (ch == 17) {
                        quit = true;
                        continue;
                    }
                    const bool model_confirm_shortcut = input.text.empty();
                    if (model_confirm_shortcut && (ch == 27 || ch == 'n' || ch == 'N')) {
                        Error context_error = apply_loaded_session_context(session);
                        mode = TuiMode::Chat;
                        status = context_error.ok() ? "Using thread model: " + context.options.model
                                                    : detail::error_line(context_error);
                        start_store_save();
                        continue;
                    }
                    if (model_confirm_shortcut && (ch == 'y' || ch == 'Y')) {
                        app::refresh_session_metadata(session, context);
                        mode = TuiMode::Chat;
                        status = context.options.model.empty()
                                     ? "Using current provider: " +
                                           provider::display_name_for_profile(context.profile.name)
                                     : "Using current model: " + context.options.model;
                        start_store_save();
                        continue;
                    }
                    if (model_confirm_shortcut && ch != '/' && ch != '\r' && ch != '\n' && ch < 32) {
                        status = "Press y to keep current provider/model, n or Esc to use thread model";
                        continue;
                    }
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
                if (ch == 5 && mode == TuiMode::Chat) {
                    start_history_edit();
                    continue;
                }
                if (ch == 12 && mode == TuiMode::Chat) {
                    start_thread_list();
                    continue;
                }
                if (ch == 11) {
                    detail::set_status_from_error(input.kill_to_line_end(), status);
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
                       activity_kind, render_frame, detail::RenderStyle{theme, use_colors}, panel_title());
    }

    model_job.cancel();
    completion_job.cancel();
    model_job.join();
    completion_job.join();
    remove_empty_thread_on_exit();
    file_job.cancel();
    file_job.join();
    return 0;
}

}  // namespace pkchat::tui
