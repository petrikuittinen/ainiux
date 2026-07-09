#include "tui/file_jobs.hpp"

#include "app/app.hpp"
#include "fetch/fetch.hpp"
#include "input/input.hpp"
#include "provider/provider.hpp"
#include "search/search.hpp"
#include "tui/detail/render.hpp"
#include "tui/tui.hpp"

namespace pkchat::tui {

bool TuiFileJobs::busy(bool quiet) const {
    if (!file_job.running()) {
        return false;
    }
    if (!quiet) {
        status = "A file job is already running";
    }
    return true;
}

void TuiFileJobs::start_save(const std::string& path, chat::Session snapshot, bool quiet_success) {
    if (path.empty()) {
        return;
    }
    if (busy(quiet_success)) {
        return;
    }
    runtime::EventQueue<TuiEvent>& event_queue = events;
    file_job.start([path, snapshot = std::move(snapshot), quiet_success, &event_queue](
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
        event_queue.push(std::move(event));
    });
}

void TuiFileJobs::start_load(const std::string& path) {
    if (busy()) {
        return;
    }
    runtime::EventQueue<TuiEvent>& event_queue = events;
    file_job.start([path, &event_queue](runtime::CancellationToken token) mutable {
        TuiEvent event;
        event.type = TuiEventType::LoadDone;
        event.text = path;
        if (token.cancelled()) {
            event.error = {ErrorCode::Cancelled, "load cancelled: " + path};
        } else {
            event.error = chat::load_session(path, event.session);
        }
        event_queue.push(std::move(event));
    });
    status = "Loading " + path;
}

void TuiFileJobs::start_store_load(long long thread_id) {
    if (!sqlite_available) {
        status = sqlite_unavailable_message ? sqlite_unavailable_message() : sqlite_unavailable_status("");
        return;
    }
    if (busy()) {
        return;
    }
    const std::string db_path = sqlite_path;
    runtime::EventQueue<TuiEvent>& event_queue = events;
    file_job.start([db_path, thread_id, &event_queue](runtime::CancellationToken token) mutable {
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
        event_queue.push(std::move(event));
    });
    status = "Loading thread " + std::to_string(thread_id);
}

void TuiFileJobs::start_store_save() {
    if (!sqlite_available || busy(true)) {
        return;
    }
    chat::Session snapshot = session;
    app::refresh_session_metadata(snapshot, context);
    const std::string db_path = sqlite_path;
    runtime::EventQueue<TuiEvent>& event_queue = events;
    file_job.start([db_path, snapshot = std::move(snapshot), &event_queue](
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
        event_queue.push(std::move(event));
    });
}

void TuiFileJobs::start_insert(const std::string& path) {
    if (busy()) {
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
    runtime::EventQueue<TuiEvent>& event_queue = events;
    file_job.start([path, type, text_limit, image_limit, &event_queue](runtime::CancellationToken token) mutable {
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
        event_queue.push(std::move(event));
    });
    status = (type.kind == input::Kind::Image ? "Attaching " : "Inserting ") + path + "...";
}

void TuiFileJobs::start_fetch(const std::string& url) {
    if (busy()) {
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
    runtime::EventQueue<TuiEvent>& event_queue = events;
    file_job.start([url, options, &event_queue](runtime::CancellationToken token) mutable {
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
        event_queue.push(std::move(event));
    });
    status = "Fetching " + url + "...";
}

void TuiFileJobs::start_search(const std::string& query) {
    if (busy()) {
        return;
    }
    if (query.empty()) {
        status = "Usage: /search QUERY";
        return;
    }
    search::Options options = search::options_for(context.options);
    runtime::EventQueue<TuiEvent>& event_queue = events;
    file_job.start([query, options, &event_queue](runtime::CancellationToken token) mutable {
        TuiEvent event;
        event.type = TuiEventType::SearchDone;
        event.text = query;
        search::SearchResponse response;
        event.error = search::search(query, options, response, token);
        if (event.error.ok()) {
            event.inserted_message = {"user", search::format_context_message(query, response)};
        }
        event_queue.push(std::move(event));
    });
    status = "Searching " + query + "...";
}

}  // namespace pkchat::tui