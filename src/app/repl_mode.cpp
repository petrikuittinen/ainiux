#include "app/app.hpp"
#include "app/detail.hpp"
#include "app/user_shell.hpp"

#include <iostream>
#include <utility>
#include <vector>

#include "chat/settings.hpp"
#include "fetch/fetch.hpp"
#include "input/input.hpp"
#include "search/search.hpp"
#include "security/redact.hpp"

namespace ainiux::app {

namespace {

using InputKind = input::Kind;

void print_repl_help() {
    std::cerr << "Commands: /help, /quit, /exit, /save [PATH], /load PATH, /insert FILE_OR_URL, /attach PATH, "
                 "/fetch URL, /search QUERY, /shell COMMAND, !COMMAND, /shell-stdout COMMAND, !!COMMAND, "
                 "/clear, /system TEXT, /model MODEL, /reasoning auto|VALUE|TOKENS\n"
                 "  /shell and ! show a full notice; /shell-stdout and !! print pure stdout "
                 "(TUI places that stdout in the input draft).\n";
}

bool allowed_for_read_only_session(const std::string& text) {
    return text == "/help" || text == "/quit" || text == "/exit" ||
           text.rfind("/save", 0) == 0 || text.rfind("/load", 0) == 0 ||
           text == "/shell" || text.rfind("/shell ", 0) == 0 ||
           text == "/shell-stdout" || text.rfind("/shell-stdout ", 0) == 0;
}

std::vector<std::string> repl_secrets(const provider::RequestContext& context) {
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
    return secrets;
}

void run_repl_shell(const std::string& command,
                    const provider::RequestContext& context,
                    UserShellDestination destination) {
    UserShellOptions options;
    if (context.options.timeout_seconds > 0) {
        options.timeout_ms = static_cast<long>(context.options.timeout_seconds) * 1000L;
    }
    UserShellResult result;
    Error err = run_user_shell(command, options, result);
    const std::vector<std::string> secrets = repl_secrets(context);
    if (destination == UserShellDestination::Draft) {
        // No editable draft in line-oriented REPL: print pure stdout for copy/paste.
        const std::string draft = format_user_shell_draft_stdout(result, secrets);
        if (!draft.empty()) {
            std::cerr << draft;
            if (draft.back() != '\n') std::cerr << "\n";
        }
        if (user_shell_failed(err, result)) {
            std::cerr << format_user_shell_failure_notice(err, result, secrets);
            std::cerr << format_user_shell_draft_status(err, result, secrets) << "\n";
        } else {
            std::cerr << format_user_shell_draft_status(err, result, secrets) << "\n";
        }
    } else {
        std::cerr << format_user_shell_notice(result, secrets);
        if (!err.ok() && err.code != ErrorCode::Cancelled && err.code != ErrorCode::Timeout) {
            print_error(err);
        }
    }
}

}  // namespace

int run_repl(provider::RequestContext context, chat::Session session, std::ostream& out) {
    refresh_session_metadata(session, context);
    apply_system_prompt(session, context.options.system);

    std::vector<provider::ImageInput> pending_images;

    auto send_prompt = [&](const std::string& text) -> int {
        provider::ChatResult chat;
        Error err = send_session_turn(context, session, text, out, chat, pending_images);
        if (!err.ok()) {
            print_error(err);
            return exit_code_for(err.code);
        }
        pending_images.clear();
        err = save_if_requested(context.options, session);
        if (!err.ok()) {
            print_error(err);
            return exit_code_for(err.code);
        }
        print_verbose_metrics(context, chat, session.messages);
        return 0;
    };

    if (!detail::trim_ascii(context.options.prompt).empty()) {
        const int rc = send_prompt(context.options.prompt);
        if (rc != 0) {
            return rc;
        }
    }

    std::string line;
    while (true) {
        if (!context.options.quiet) {
            std::cerr << "> ";
        }
        if (!std::getline(std::cin, line)) {
            if (!context.options.quiet) {
                std::cerr << "\n";
            }
            break;
        }
        const std::string text = detail::trim_ascii(line);
        if (text.empty()) {
            continue;
        }
        {
            std::string shell_command;
            std::string shell_error;
            UserShellDestination shell_dest = UserShellDestination::Notice;
            if (parse_user_shell_invocation(text, shell_command, shell_error, shell_dest)) {
                if (!shell_error.empty()) {
                    std::cerr << shell_error << "\n";
                    continue;
                }
                run_repl_shell(shell_command, context, shell_dest);
                continue;
            }
        }
        if (text[0] == '/') {
            if (session.read_only && !allowed_for_read_only_session(text)) {
                const std::string reason = session.read_only_reason.empty()
                                               ? "managed attachment media is unavailable"
                                               : session.read_only_reason;
                print_error({ErrorCode::FileLock,
                             "chat thread is read-only: " + reason});
                continue;
            }
            if (text == "/quit" || text == "/exit") {
                break;
            }
            if (text == "/help") {
                print_repl_help();
                continue;
            }
            if (text == "/clear") {
                session.messages.clear();
                pending_images.clear();
                apply_system_prompt(session, context.options.system);
                if (!context.options.quiet) {
                    std::cerr << "Chat history cleared.\n";
                }
                continue;
            }
            if (text.rfind("/system", 0) == 0) {
                replace_system_prompt(session, detail::trim_ascii(text.substr(7)));
                if (!context.options.quiet) {
                    std::cerr << "System prompt updated.\n";
                }
                continue;
            }
            if (text.rfind("/model", 0) == 0) {
                const std::string model = detail::trim_ascii(text.substr(6));
                if (model.empty()) {
                    std::cerr << "Usage: /model MODEL\n";
                    continue;
                }
                context.options.model = model;
                session.model = model;
                if (!context.options.quiet) {
                    std::cerr << "Model set to " << model << "\n";
                }
                continue;
            }
            if (text == "/reasoning" || text.rfind("/reasoning ", 0) == 0) {
                const std::string requested = detail::trim_ascii(text.substr(10));
                if (requested.empty()) {
                    std::cerr << "Usage: /reasoning auto|VALUE|TOKENS\n";
                    continue;
                }
                ReasoningSelection selection;
                Error err = config::parse_reasoning_selection(requested, selection);
                if (!err.ok()) {
                    print_error(err);
                    continue;
                }
                const std::string warning = config::reasoning_catalog_warning(
                    context.options.model_catalog,
                    context.profile.name,
                    context.api_kind == provider::ApiKind::Responses ? "responses" : "chat",
                    context.options.model,
                    selection);
                if (!warning.empty()) {
                    std::cerr << "Warning: " << warning << ". Proceed? [y/N] ";
                    std::string answer;
                    if (!std::getline(std::cin, answer) ||
                        (ascii_lower(detail::trim_ascii(answer)) != "y" &&
                         ascii_lower(detail::trim_ascii(answer)) != "yes")) {
                        std::cerr << "Reasoning change cancelled.\n";
                        continue;
                    }
                }
                err = chat::apply_chat_setting(context.options, "reasoning", requested);
                if (!err.ok()) {
                    print_error(err);
                    continue;
                }
                refresh_session_metadata(session, context);
                if (!context.options.quiet) {
                    std::cerr << "Reasoning set to "
                              << config::reasoning_selection_value(context.options.reasoning)
                              << "\n";
                }
                continue;
            }
            if (text.rfind("/save", 0) == 0) {
                std::string path = detail::trim_ascii(text.substr(5));
                if (path.empty()) {
                    path = context.options.save_chat_path;
                }
                if (path.empty()) {
                    std::cerr << "Usage: /save PATH\n";
                    continue;
                }
                Error err = chat::save_session_atomic(path, session);
                if (!err.ok()) {
                    print_error(err);
                    continue;
                }
                if (!context.options.quiet) {
                    std::cerr << "Saved chat to " << path << "\n";
                }
                continue;
            }
            if (text.rfind("/load", 0) == 0) {
                const std::string path = detail::trim_ascii(text.substr(5));
                if (path.empty()) {
                    std::cerr << "Usage: /load PATH\n";
                    continue;
                }
                chat::Session loaded;
                Error err = chat::load_session(path, loaded);
                if (!err.ok()) {
                    print_error(err);
                    continue;
                }
                session = std::move(loaded);
                pending_images.clear();
                refresh_session_metadata(session, context);
                if (!context.options.quiet) {
                    std::cerr << "Loaded chat from " << path << "\n";
                }
                continue;
            }
            if (text == "/insert" || text.rfind("/insert ", 0) == 0) {
                const std::string source = detail::trim_ascii(text.substr(7));
                if (source.empty()) {
                    std::cerr << "Usage: /insert FILE_OR_URL\n";
                    continue;
                }
                input::InsertSourceOptions options;
                options.max_file_bytes = context.options.max_input_bytes > 0
                                             ? static_cast<size_t>(context.options.max_input_bytes)
                                             : 0;
                options.fetch = fetch_options_for(context.options);
                options.auto_convert_html_to_markdown =
                    context.options.auto_convert_html_to_markdown;
                input::InsertSource inserted;
                Error err = input::load_insert_source(source, options, inserted);
                if (!err.ok()) {
                    print_error(err);
                    continue;
                }
                input::TextContext document;
                document.source = inserted.url ? "URL " + inserted.source : "file " + inserted.source;
                document.kind = inserted.converted_html ? InputKind::Markdown : InputKind::Plaintext;
                document.content = std::move(inserted.content);
                session.messages.push_back({"user", input::text_context_message(document)});
                if (!context.options.quiet) {
                    std::cerr << "Inserted content from " << source << "\n";
                }
                continue;
            }
            if (text == "/attach" || text.rfind("/attach ", 0) == 0) {
                const std::string path = detail::trim_ascii(text.substr(7));
                if (path.empty()) {
                    std::cerr << "Usage: /attach PATH\n";
                    continue;
                }
                if (path == "stdin") {
                    std::cerr << "stdin input is only supported by non-interactive --input and --attach\n";
                    continue;
                }
                input::FileType type;
                Error err = input::classify_file_type(path, type);
                if (!err.ok()) {
                    print_error(err);
                    continue;
                }
                if (type.kind == InputKind::Image) {
                    err = provider::validate_image_input(context);
                    if (!err.ok()) {
                        print_error(err);
                        continue;
                    }
                    if (context.options.max_image_bytes <= 0) {
                        print_error({ErrorCode::BadArgs, "--max-image-bytes must be greater than zero"});
                        continue;
                    }
                    input::ImageData image;
                    err = input::load_image_file(path, type,
                                                 static_cast<size_t>(context.options.max_image_bytes), image);
                    if (!err.ok()) {
                        print_error(err);
                        continue;
                    }
                    pending_images.push_back({image.mime_type, std::move(image.base64_data)});
                    if (!context.options.quiet) {
                        std::cerr << "Attached image for next prompt: " << path << " ("
                                  << pending_images.size() << " pending)\n";
                    }
                } else {
                    LoadedDocument document;
                    err = load_text_context_file(context.options, path, "/attach", document);
                    if (!err.ok()) {
                        print_error(err);
                        continue;
                    }
                    session.messages.push_back({"user", document_context_message(document)});
                    if (!context.options.quiet) {
                        std::cerr << "Attached context from " << path << "\n";
                    }
                }
                continue;
            }
            if (text == "/fetch" || text.rfind("/fetch ", 0) == 0) {
                const std::string url = detail::trim_ascii(text.substr(6));
                if (url.empty()) {
                    std::cerr << "Usage: /fetch URL\n";
                    continue;
                }
                std::string markdown;
                Error err = fetch::fetch_markdown(url, fetch_options_for(context.options), markdown);
                if (!err.ok()) {
                    print_error(err);
                    continue;
                }
                input::TextContext fetched;
                fetched.source = "URL " + url;
                fetched.kind = InputKind::Markdown;
                fetched.content = std::move(markdown);
                session.messages.push_back({"user", input::text_context_message(fetched)});
                if (!context.options.quiet) {
                    std::cerr << "Fetched and inserted URL: " << url << "\n";
                }
                continue;
            }
            if (text == "/search" || text.rfind("/search ", 0) == 0) {
                const std::string query = detail::trim_ascii(text.substr(7));
                if (query.empty()) {
                    std::cerr << "Usage: /search QUERY\n";
                    continue;
                }
                search::SearchResponse response;
                Error err = search::search(query, search::options_for(context.options), response);
                if (!err.ok()) {
                    print_error(err);
                    continue;
                }
                session.messages.push_back({"user", search::format_context_message(query, response)});
                if (!context.options.quiet) {
                    std::cerr << "Inserted web search results from " << response.provider_used << ": "
                              << query << "\n";
                }
                continue;
            }
            std::cerr << "Unknown command: " << text << "\n";
            continue;
        }
        const int rc = send_prompt(text);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

}  // namespace ainiux::app
