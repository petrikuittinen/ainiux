#include "tui/file_jobs.hpp"

#include "app/user_shell.hpp"
#include "fetch/fetch.hpp"
#include "html/html.hpp"
#include "input/input.hpp"
#include "provider/provider.hpp"
#include "search/search.hpp"
#include "security/redact.hpp"
#include "tui/detail/render.hpp"
#include "tui/tui.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace ainiux::tui {

bool TuiFileJobs::busy(bool quiet) const {
    if (!file_job.joinable()) {
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

void TuiFileJobs::start_store_save(chat::Session snapshot) {
    if (!sqlite_available || busy(true)) {
        return;
    }
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

void TuiFileJobs::start_media_cleanup(int expiration_days,
                                      long long protected_thread_id,
                                      bool automatic) {
    if (!sqlite_available) {
        if (!automatic) {
            status = sqlite_unavailable_message ? sqlite_unavailable_message()
                                                : sqlite_unavailable_status("");
        }
        return;
    }
    if (expiration_days <= 0) {
        if (!automatic) {
            status = "Media cleanup is disabled by its zero-day setting";
        }
        return;
    }
    if (busy(automatic)) {
        return;
    }
    const std::string db_path = sqlite_path;
    runtime::EventQueue<TuiEvent>& event_queue = events;
    file_job.start([db_path, expiration_days, protected_thread_id, automatic, &event_queue](
                       runtime::CancellationToken token) mutable {
        TuiEvent event;
        event.type = TuiEventType::MediaCleanupDone;
        event.automatic_cleanup = automatic;
        if (token.cancelled()) {
            event.error = {ErrorCode::Cancelled, "media cleanup cancelled"};
        } else {
            chat::SqliteStore store;
            event.error = store.open(db_path);
            if (event.error.ok()) {
                const std::string reason =
                    "managed attachment media expired after " +
                    std::to_string(expiration_days) + " days of inactivity";
                event.error = store.cleanup_media(expiration_days, protected_thread_id,
                                                  reason, event.media_cleanup, token);
            }
        }
        event_queue.push(std::move(event));
    });
    if (!automatic) {
        status = "Cleaning managed media...";
    }
}

void TuiFileJobs::start_insert(const std::string& source) {
    if (busy()) {
        return;
    }
    if (source.empty()) {
        status = "Usage: /insert FILE_OR_URL";
        return;
    }
    if (source == "stdin") {
        status = "stdin input is only supported by non-interactive --input and --attach";
        return;
    }
    input::InsertSourceOptions options;
    options.max_file_bytes = context.options.max_input_bytes > 0
                                 ? static_cast<size_t>(context.options.max_input_bytes)
                                 : 0;
    options.fetch.connect_timeout_seconds = context.options.connect_timeout_seconds;
    options.fetch.timeout_seconds = context.options.timeout_seconds > 0
                                        ? context.options.timeout_seconds
                                        : 30;
    options.fetch.max_bytes = context.options.max_fetch_bytes;
    options.fetch.proxy = context.options.proxy;
    options.fetch.insecure_tls = context.options.insecure_tls;
    options.fetch.trace_http = context.options.trace_http;
    options.fetch.allow_private = context.options.allow_private_url_fetch;
    options.auto_convert_html_to_markdown = context.options.auto_convert_html_to_markdown;
    runtime::EventQueue<TuiEvent>& event_queue = events;
    file_job.start([source, options, &event_queue](runtime::CancellationToken token) mutable {
        TuiEvent event;
        event.type = TuiEventType::InsertDone;
        event.text = source;
        input::InsertSource loaded;
        event.error = input::load_insert_source(source, options, loaded, token);
        if (event.error.ok()) {
            event.inserted_text = std::move(loaded.content);
        }
        event_queue.push(std::move(event));
    });
    status = std::string(input::is_http_url(source) ? "Fetching " : "Reading ") + source +
             " for insertion...";
}

void TuiFileJobs::start_attach(const std::string& path) {
    if (busy()) {
        return;
    }
    if (path.empty()) {
        status = "Usage: /attach PATH or URL";
        return;
    }
    if (path == "stdin") {
        status = "stdin input is only supported by non-interactive --input and --attach";
        return;
    }
    const bool is_url = input::is_http_url(path);
    if (is_url) {
        // URL text is converted once to the canonical Markdown replay format.
        fetch::Options options;
        options.connect_timeout_seconds = context.options.connect_timeout_seconds;
        options.timeout_seconds = context.options.timeout_seconds > 0 ? context.options.timeout_seconds : 30;
        options.max_bytes = context.options.max_fetch_bytes;
        options.proxy = context.options.proxy;
        options.insecure_tls = context.options.insecure_tls;
        options.trace_http = context.options.trace_http;
        options.allow_private = context.options.allow_private_url_fetch;
        const long text_limit = context.options.max_input_bytes;
        const long inline_limit = context.options.media_max_size_to_store_to_db;
        const bool persist_attachment = sqlite_available;
        const std::string media_database_path = sqlite_path;
        runtime::EventQueue<TuiEvent>& event_queue = events;
        file_job.start([path, options, text_limit, inline_limit, persist_attachment,
                        media_database_path, &event_queue](
                           runtime::CancellationToken token) mutable {
            TuiEvent event;
            event.type = TuiEventType::AttachDone;
            event.text = path;
            event.attached_source = path;
            std::string body;
            event.error = fetch::fetch_html(path, options, body, token);
            if (event.error.ok()) {
                if (text_limit > 0 && body.size() > static_cast<size_t>(text_limit)) {
                    event.error = {ErrorCode::UnsupportedFeature,
                                   "attachment from URL exceeds --max-input-bytes limit"};
                } else {
                    try {
                        body = html::convert(body, html::OutputFormat::Markdown);
                    } catch (const std::bad_alloc&) {
                        event.error = {ErrorCode::Internal,
                                       "not enough memory to convert HTML from URL: " + path};
                    } catch (const std::length_error&) {
                        event.error = {ErrorCode::UnsupportedFeature,
                                       "converted HTML is too large to attach from URL: " + path};
                    }
                    if (event.error.ok() && text_limit > 0 &&
                        body.size() > static_cast<size_t>(text_limit)) {
                        event.error = {ErrorCode::UnsupportedFeature,
                                       "converted attachment from URL exceeds --max-input-bytes limit"};
                    }
                    if (event.error.ok() && persist_attachment) {
                        chat::SqliteStore store;
                        event.error = store.open(media_database_path);
                        if (event.error.ok()) {
                            event.error = store.import_text_attachment(
                                body, static_cast<size_t>(inline_limit), path, path,
                                event.text_attachment);
                        }
                    } else if (event.error.ok()) {
                        event.text_attachment.markdown_content = std::move(body);
                        event.text_attachment.display_name = path;
                        event.text_attachment.source_ref = path;
                        event.text_attachment.byte_size = static_cast<long long>(
                            event.text_attachment.markdown_content.size());
                    }
                    if (event.error.ok()) {
                        event.text_attachment_ready = true;
                    }
                }
            }
            event_queue.push(std::move(event));
        });
        status = "Attaching " + path + "...";
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
    const long inline_limit = context.options.media_max_size_to_store_to_db;
    const bool persist_attachment = sqlite_available;
    const std::string media_database_path = sqlite_path;
    runtime::EventQueue<TuiEvent>& event_queue = events;
    file_job.start([path, type, text_limit, image_limit, inline_limit, persist_attachment,
                    media_database_path, &event_queue](
                       runtime::CancellationToken token) mutable {
        TuiEvent event;
        event.type = TuiEventType::AttachDone;
        event.text = path;
        if (type.kind == input::Kind::Image) {
            event.image_attachment = true;
            if (image_limit <= 0) {
                event.error = {ErrorCode::BadArgs, "--max-image-bytes must be greater than zero"};
            } else if (persist_attachment) {
                std::string bytes;
                event.error = input::load_image_file_bytes(
                    path, type, static_cast<size_t>(image_limit), bytes, token);
                if (event.error.ok()) {
                    chat::SqliteStore store;
                    event.error = store.open(media_database_path);
                    if (event.error.ok()) {
                        event.error = store.import_media(bytes, type.mime_type, path,
                                                         expand_user_path(path), event.image);
                    }
                }
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
            // Convert once at import; Markdown is the native replay format.
            std::string body;
            event.error = input::read_local_text_file_for_attach(path, static_cast<size_t>(text_limit), body, token);
            if (event.error.ok()) {
                if (type.kind == input::Kind::Html) {
                    try {
                        body = html::convert(body, html::OutputFormat::Markdown);
                    } catch (const std::bad_alloc&) {
                        event.error = {ErrorCode::Internal,
                                       "not enough memory to convert HTML: " + path};
                    } catch (const std::length_error&) {
                        event.error = {ErrorCode::UnsupportedFeature,
                                       "converted HTML is too large: " + path};
                    }
                }
                if (event.error.ok() && body.size() > static_cast<size_t>(text_limit)) {
                    event.error = {ErrorCode::UnsupportedFeature,
                                   "converted attachment exceeds --max-input-bytes limit: " + path};
                }
                if (event.error.ok() && persist_attachment) {
                    chat::SqliteStore store;
                    event.error = store.open(media_database_path);
                    if (event.error.ok()) {
                        event.error = store.import_text_attachment(
                            body, static_cast<size_t>(inline_limit), path,
                            expand_user_path(path), event.text_attachment);
                    }
                } else if (event.error.ok()) {
                    event.text_attachment.markdown_content = std::move(body);
                    event.text_attachment.display_name = path;
                    event.text_attachment.source_ref = expand_user_path(path);
                    event.text_attachment.byte_size = static_cast<long long>(
                        event.text_attachment.markdown_content.size());
                }
                if (event.error.ok()) {
                    event.attached_source = path;
                    event.text_attachment_ready = true;
                }
            }
        }
        event_queue.push(std::move(event));
    });
    status = "Attaching " + path + "...";
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

void TuiFileJobs::start_shell(const std::string& command, bool to_draft) {
    if (busy()) {
        return;
    }
    if (command.empty()) {
        status = to_draft ? "Usage: /shell-stdout COMMAND  or  !!COMMAND"
                          : "Usage: /shell COMMAND  or  !COMMAND";
        return;
    }

    std::vector<std::string> secrets;
    if (!context.api_key.empty()) secrets.push_back(context.api_key);
    if (!context.options.key.empty()) secrets.push_back(context.options.key);
    for (const std::string& header : context.headers) {
        const std::size_t colon = header.find(':');
        if (colon == std::string::npos) continue;
        if (is_sensitive_header_name(ascii_trim(header.substr(0, colon)))) {
            const std::string value = ascii_trim(header.substr(colon + 1));
            if (!value.empty()) secrets.push_back(value);
        }
    }
    std::sort(secrets.begin(), secrets.end());
    secrets.erase(std::unique(secrets.begin(), secrets.end()), secrets.end());

    app::UserShellOptions options;
    if (context.options.timeout_seconds > 0) {
        options.timeout_ms = static_cast<long>(context.options.timeout_seconds) * 1000L;
    }
    runtime::EventQueue<TuiEvent>& event_queue = events;
    file_job.start([command, options, secrets = std::move(secrets), to_draft, &event_queue](
                       runtime::CancellationToken token) mutable {
        TuiEvent event;
        event.type = TuiEventType::ShellDone;
        event.text = command;
        event.shell_to_draft = to_draft;
        options.cancellation = token;
        app::UserShellResult result;
        event.error = app::run_user_shell(command, options, result);
        event.shell_exit_status = result.exit_status;
        event.shell_stdout_truncated = result.stdout_truncated;
        event.shell_failed = app::user_shell_failed(event.error, result);
        if (to_draft) {
            // Pure stdout for the editable input draft (may be partial on cancel/timeout).
            event.inserted_text = app::format_user_shell_draft_stdout(result, secrets);
            // Ready status line (success or failure). Command remains in failure notice body.
            event.text = app::format_user_shell_draft_status(event.error, result, secrets);
            event.quiet_success = !event.shell_failed;
            if (event.shell_failed) {
                event.inserted_message = {
                    "notice", app::format_user_shell_failure_notice(event.error, result, secrets)};
            }
        } else {
            event.inserted_message = {"notice", app::format_user_shell_notice(result, secrets)};
            if (event.error.ok()) {
                event.quiet_success = result.exit_status == 0;
            }
        }
        event_queue.push(std::move(event));
    });
    status = to_draft ? ("Shell → draft: " + command) : ("Running shell: " + command);
}

}  // namespace ainiux::tui
