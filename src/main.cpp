#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>

#include "chat/session.hpp"
#include "cli/args.hpp"
#include "common.hpp"
#include "json/json.hpp"
#include "pkchat/version.hpp"
#include "provider/provider.hpp"

namespace {

int exit_code_for(pkchat::ErrorCode code) {
    using pkchat::ErrorCode;
    switch (code) {
        case ErrorCode::Ok:
            return 0;
        case ErrorCode::BadArgs:
        case ErrorCode::BadUrl:
            return 2;
        case ErrorCode::Dns:
        case ErrorCode::Connect:
        case ErrorCode::Tls:
        case ErrorCode::Timeout:
            return 3;
        case ErrorCode::HttpStatus:
        case ErrorCode::Auth:
        case ErrorCode::RateLimit:
        case ErrorCode::JsonParse:
        case ErrorCode::SseParse:
        case ErrorCode::ProviderSchema:
            return 4;
        case ErrorCode::FileRead:
        case ErrorCode::FileWrite:
        case ErrorCode::Config:
            return 5;
        case ErrorCode::UnsupportedFeature:
        case ErrorCode::Internal:
            return 6;
    }
    return 6;
}

void print_error(const pkchat::Error& error) {
    std::cerr << pkchat::error_code_name(error.code) << ": " << error.message << "\n";
}

std::ostream* output_stream(const pkchat::cli::Options& options, std::ofstream& file, pkchat::Error& error) {
    if (options.output_path.empty()) {
        return &std::cout;
    }
    file.open(options.output_path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = {pkchat::ErrorCode::FileWrite, "could not open output file for writing: " + options.output_path};
        return nullptr;
    }
    return &file;
}

std::string trim_ascii(std::string text) {
    auto is_ws = [](unsigned char ch) { return ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t'; };
    while (!text.empty() && is_ws(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
    }
    while (!text.empty() && is_ws(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    return text;
}

void write_json_chat(std::ostream& out,
                     const pkchat::provider::RequestContext& context,
                     const pkchat::provider::ChatResult& result) {
    out << "{"
        << "\"model\":" << pkchat::json::quote(result.model) << ","
        << "\"provider\":" << pkchat::json::quote(context.profile.name) << ","
        << "\"content\":" << pkchat::json::quote(result.content) << ","
        << "\"usage\":" << result.usage_json << ","
        << "\"timing\":{\"ttft_ms\":" << result.ttft_ms << ",\"total_ms\":" << result.total_ms << "}"
        << "}\n";
}

double tokens_per_second(const pkchat::provider::ChatResult& result, bool stream) {
    long long denominator_ms = result.total_ms;
    if (stream && result.ttft_ms >= 0 && result.total_ms > result.ttft_ms) {
        denominator_ms = result.total_ms - result.ttft_ms;
    }
    if (denominator_ms <= 0) {
        denominator_ms = 1;
    }
    return static_cast<double>(result.completion_tokens) * 1000.0 / static_cast<double>(denominator_ms);
}

void print_verbose_metrics(const pkchat::cli::Options& options, const pkchat::provider::ChatResult& result) {
    if (!options.verbose || options.quiet) {
        return;
    }
    std::cerr << "TTFT: " << result.ttft_ms << " ms, ";
    std::cerr << "Token/s: " << std::fixed << std::setprecision(1) << tokens_per_second(result, options.stream);
    if (result.completion_tokens_estimated) {
        std::cerr << " (estimated)";
    }
    std::cerr << "\n";
}

void refresh_session_metadata(pkchat::chat::Session& session, const pkchat::provider::RequestContext& context) {
    session.provider = context.profile.name;
    session.base_url = context.base_url;
    session.model = context.options.model;
}

bool has_system_message(const pkchat::chat::Session& session) {
    for (const pkchat::provider::Message& message : session.messages) {
        if (message.role == "system") {
            return true;
        }
    }
    return false;
}

void apply_system_prompt(pkchat::chat::Session& session, const std::string& system) {
    if (trim_ascii(system).empty() || has_system_message(session)) {
        return;
    }
    session.messages.insert(session.messages.begin(), {"system", system});
}

void replace_system_prompt(pkchat::chat::Session& session, const std::string& system) {
    for (auto it = session.messages.begin(); it != session.messages.end();) {
        if (it->role == "system") {
            it = session.messages.erase(it);
        } else {
            ++it;
        }
    }
    if (!trim_ascii(system).empty()) {
        session.messages.insert(session.messages.begin(), {"system", system});
    }
}

pkchat::Error save_if_requested(const pkchat::cli::Options& options, const pkchat::chat::Session& session) {
    if (options.save_chat_path.empty()) {
        return pkchat::ok_error();
    }
    return pkchat::chat::save_session_atomic(options.save_chat_path, session);
}
pkchat::Error choose_default_model(pkchat::provider::RequestContext& context) {
    if (!context.options.model.empty()) {
        return pkchat::ok_error();
    }
    pkchat::provider::ModelsResult models;
    pkchat::Error err = pkchat::provider::list_models(context, models);
    if (!err.ok()) {
        return err;
    }
    if (!models.model_ids.empty()) {
        context.options.model = models.model_ids.front();
    }
    return pkchat::ok_error();
}
void print_chat_start(const pkchat::provider::RequestContext& context) {
    if (context.options.quiet) {
        return;
    }
    std::cerr << "Endpoint: " << context.chat_url << std::endl;
    std::cerr << "Model: " << (context.options.model.empty() ? "unknown" : context.options.model) << std::endl;
}

pkchat::Error send_session_turn(pkchat::provider::RequestContext& context,
                                pkchat::chat::Session& session,
                                const std::string& prompt,
                                std::ostream& out,
                                pkchat::provider::ChatResult& chat) {
    session.messages.push_back({"user", prompt});
    bool started_ndjson = false;
    auto on_delta = [&](const std::string& delta) -> pkchat::Error {
        if (context.options.format == pkchat::cli::OutputFormat::Text) {
            out << delta;
            out.flush();
        } else if (context.options.format == pkchat::cli::OutputFormat::Ndjson) {
            if (!started_ndjson) {
                out << "{\"event\":\"start\",\"model\":" << pkchat::json::quote(context.options.model) << "}\n";
                started_ndjson = true;
            }
            out << "{\"event\":\"delta\",\"text\":" << pkchat::json::quote(delta) << "}\n";
            out.flush();
        }
        return pkchat::ok_error();
    };

    pkchat::Error err = pkchat::provider::send_chat_messages(context, session.messages, on_delta, chat);
    if (!err.ok()) {
        session.messages.pop_back();
        return err;
    }
    session.messages.push_back({"assistant", chat.content});
    if (!chat.model.empty()) {
        session.model = chat.model;
    }
    if (!chat.usage_json.empty() && chat.usage_json != "null") {
        session.usage_json = chat.usage_json;
    }

    if (context.options.format == pkchat::cli::OutputFormat::Text) {
        if (!context.options.stream) {
            out << chat.content;
        }
        out << "\n";
    } else if (context.options.format == pkchat::cli::OutputFormat::Json) {
        write_json_chat(out, context, chat);
    } else {
        if (!started_ndjson) {
            out << "{\"event\":\"start\",\"model\":" << pkchat::json::quote(context.options.model) << "}\n";
        }
        if (!context.options.stream && !chat.content.empty()) {
            out << "{\"event\":\"delta\",\"text\":" << pkchat::json::quote(chat.content) << "}\n";
        }
        out << "{\"event\":\"done\",\"usage\":null}\n";
    }
    return pkchat::ok_error();
}

void print_repl_help() {
    std::cerr << "Commands: /help, /quit, /exit, /save [PATH], /load PATH, /clear, /system TEXT, /model MODEL\n";
}

int run_repl(pkchat::provider::RequestContext context, pkchat::chat::Session session, std::ostream& out) {
    refresh_session_metadata(session, context);
    apply_system_prompt(session, context.options.system);
    if (!context.options.quiet) {
        std::cerr << "pkchat REPL. Type /help for commands, /quit to exit.\n";
    }

    auto send_prompt = [&](const std::string& text) -> int {
        pkchat::provider::ChatResult chat;
        pkchat::Error err = send_session_turn(context, session, text, out, chat);
        if (!err.ok()) {
            print_error(err);
            return exit_code_for(err.code);
        }
        err = save_if_requested(context.options, session);
        if (!err.ok()) {
            print_error(err);
            return exit_code_for(err.code);
        }
        print_verbose_metrics(context.options, chat);
        return 0;
    };

    if (!trim_ascii(context.options.prompt).empty()) {
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
        const std::string text = trim_ascii(line);
        if (text.empty()) {
            continue;
        }
        if (text[0] == '/') {
            if (text == "/quit" || text == "/exit") {
                break;
            }
            if (text == "/help") {
                print_repl_help();
                continue;
            }
            if (text == "/clear") {
                session.messages.clear();
                apply_system_prompt(session, context.options.system);
                if (!context.options.quiet) {
                    std::cerr << "Chat history cleared.\n";
                }
                continue;
            }
            if (text.rfind("/system", 0) == 0) {
                replace_system_prompt(session, trim_ascii(text.substr(7)));
                if (!context.options.quiet) {
                    std::cerr << "System prompt updated.\n";
                }
                continue;
            }
            if (text.rfind("/model", 0) == 0) {
                const std::string model = trim_ascii(text.substr(6));
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
            if (text.rfind("/save", 0) == 0) {
                std::string path = trim_ascii(text.substr(5));
                if (path.empty()) {
                    path = context.options.save_chat_path;
                }
                if (path.empty()) {
                    std::cerr << "Usage: /save PATH\n";
                    continue;
                }
                pkchat::Error err = pkchat::chat::save_session_atomic(path, session);
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
                const std::string path = trim_ascii(text.substr(5));
                if (path.empty()) {
                    std::cerr << "Usage: /load PATH\n";
                    continue;
                }
                pkchat::chat::Session loaded;
                pkchat::Error err = pkchat::chat::load_session(path, loaded);
                if (!err.ok()) {
                    print_error(err);
                    continue;
                }
                session = std::move(loaded);
                refresh_session_metadata(session, context);
                if (!context.options.quiet) {
                    std::cerr << "Loaded chat from " << path << "\n";
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

}  // namespace

int main(int argc, char** argv) {
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(argc, argv);
    if (!parsed.error.ok()) {
        print_error(parsed.error);
        return exit_code_for(parsed.error.code);
    }
    pkchat::cli::Options options = parsed.options;
    if (options.help) {
        std::cout << pkchat::cli::help_text();
        return 0;
    }
    if (options.version) {
        std::cout << "pkchat " << pkchat::kVersion << "\n";
        return 0;
    }
    if (options.repl && options.list_models) {
        print_error({pkchat::ErrorCode::BadArgs, "--repl cannot be combined with --list-models"});
        return exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.repl && options.format != pkchat::cli::OutputFormat::Text) {
        print_error({pkchat::ErrorCode::BadArgs, "--repl currently supports --format text only"});
        return exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (!options.key.empty() && !options.quiet) {
        std::cerr << "Warning: command line API keys may be visible to other local users; prefer --key-env, --key-file, or --key-stdin.\n";
    }

    pkchat::chat::Session session;
    bool loaded_session = false;
    if (!options.load_chat_path.empty()) {
        pkchat::Error err = pkchat::chat::load_session(options.load_chat_path, session);
        if (!err.ok()) {
            print_error(err);
            return exit_code_for(err.code);
        }
        loaded_session = true;
        if (options.model.empty()) {
            options.model = session.model;
        }
        if (options.base_url.empty() && options.positional_url.empty() && options.chat_url.empty()) {
            options.base_url = session.base_url;
        }
        if (options.provider == "openai" && options.positional_url.empty() && options.base_url == session.base_url) {
            options.provider = session.provider;
        }
    }

    pkchat::provider::ContextResult context_result = pkchat::provider::build_context(options);
    if (!context_result.error.ok()) {
        print_error(context_result.error);
        return exit_code_for(context_result.error.code);
    }
    pkchat::provider::RequestContext context = context_result.context;

    std::ofstream out_file;
    pkchat::Error output_error;
    std::ostream* out = output_stream(context.options, out_file, output_error);
    if (!output_error.ok()) {
        print_error(output_error);
        return exit_code_for(output_error.code);
    }

    if (context.options.list_models) {
        pkchat::provider::ModelsResult models;
        pkchat::Error err = pkchat::provider::list_models(context, models);
        if (!err.ok()) {
            print_error(err);
            return exit_code_for(err.code);
        }
        if (context.options.format == pkchat::cli::OutputFormat::Json) {
            *out << "{\"provider\":" << pkchat::json::quote(context.profile.name) << ",\"models\":[";
            for (size_t i = 0; i < models.model_ids.size(); ++i) {
                if (i != 0) {
                    *out << ",";
                }
                *out << pkchat::json::quote(models.model_ids[i]);
            }
            *out << "]}\n";
        } else {
            for (const std::string& id : models.model_ids) {
                *out << id << "\n";
            }
        }
        return 0;
    }

    if (!loaded_session) {
        session = pkchat::chat::new_session(context);
    }
    pkchat::Error model_err = choose_default_model(context);
    if (!model_err.ok()) {
        print_error(model_err);
        return exit_code_for(model_err.code);
    }
    refresh_session_metadata(session, context);
    apply_system_prompt(session, context.options.system);
    print_chat_start(context);

    if (context.options.repl) {
        return run_repl(context, std::move(session), *out);
    }

    pkchat::provider::ChatResult chat;
    pkchat::Error err = send_session_turn(context, session, context.options.prompt, *out, chat);
    if (!err.ok()) {
        print_error(err);
        return exit_code_for(err.code);
    }
    err = save_if_requested(context.options, session);
    if (!err.ok()) {
        print_error(err);
        return exit_code_for(err.code);
    }
    print_verbose_metrics(context.options, chat);
    return 0;
}
