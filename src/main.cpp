#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <vector>

#include "chat/session.hpp"
#include "cli/args.hpp"
#include "common.hpp"
#include "editor/editor.hpp"
#include "html/html.hpp"
#include "http/http.hpp"
#include "json/json.hpp"
#include "pkchat/version.hpp"
#include "provider/provider.hpp"
#include "tui/tui.hpp"

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
        case ErrorCode::Cancelled:
            return 130;
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

std::string lower_ascii(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool starts_with(const std::string& text, const std::string& prefix) {
    return text.rfind(prefix, 0) == 0;
}

bool parse_ipv4_literal(const std::string& host, std::vector<int>& parts) {
    parts.clear();
    size_t start = 0;
    while (start <= host.size()) {
        const size_t dot = host.find('.', start);
        const size_t end = dot == std::string::npos ? host.size() : dot;
        if (end == start) {
            return false;
        }
        int value = 0;
        for (size_t i = start; i < end; ++i) {
            if (host[i] < '0' || host[i] > '9') {
                return false;
            }
            value = value * 10 + (host[i] - '0');
            if (value > 255) {
                return false;
            }
        }
        parts.push_back(value);
        if (dot == std::string::npos) {
            break;
        }
        start = dot + 1;
    }
    return parts.size() == 4;
}

bool is_blocked_ipv4(const std::vector<int>& ip) {
    if (ip.size() != 4) {
        return false;
    }
    if (ip[0] == 0 || ip[0] == 10 || ip[0] == 127) {
        return true;
    }
    if (ip[0] == 100 && ip[1] >= 64 && ip[1] <= 127) {
        return true;
    }
    if (ip[0] == 169 && ip[1] == 254) {
        return true;
    }
    if (ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31) {
        return true;
    }
    if (ip[0] == 192 && ip[1] == 168) {
        return true;
    }
    if (ip[0] >= 224) {
        return true;
    }
    return false;
}

struct FetchUrlParts {
    std::string scheme;
    std::string host;
};

pkchat::Error parse_fetch_url(const std::string& url, FetchUrlParts& parts) {
    const size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        return {pkchat::ErrorCode::BadUrl, "--fetch-url expects an absolute http:// or https:// URL: " + url};
    }
    parts.scheme = lower_ascii(url.substr(0, scheme_end));
    if (parts.scheme != "http" && parts.scheme != "https") {
        return {pkchat::ErrorCode::BadUrl, "--fetch-url only supports http:// and https:// URLs: " + url};
    }
    const size_t authority_start = scheme_end + 3;
    const size_t authority_end = url.find_first_of("/?#", authority_start);
    std::string authority = authority_end == std::string::npos
                                ? url.substr(authority_start)
                                : url.substr(authority_start, authority_end - authority_start);
    if (authority.empty()) {
        return {pkchat::ErrorCode::BadUrl, "--fetch-url URL has no host: " + url};
    }
    const size_t at = authority.rfind('@');
    if (at != std::string::npos) {
        authority = authority.substr(at + 1);
    }
    if (!authority.empty() && authority.front() == '[') {
        const size_t close = authority.find(']');
        if (close == std::string::npos) {
            return {pkchat::ErrorCode::BadUrl, "--fetch-url has an unterminated IPv6 host: " + url};
        }
        parts.host = authority.substr(1, close - 1);
    } else {
        const size_t colon = authority.rfind(':');
        parts.host = colon == std::string::npos ? authority : authority.substr(0, colon);
    }
    parts.host = lower_ascii(parts.host);
    while (!parts.host.empty() && parts.host.back() == '.') {
        parts.host.pop_back();
    }
    if (parts.host.empty()) {
        return {pkchat::ErrorCode::BadUrl, "--fetch-url URL has no host: " + url};
    }
    return pkchat::ok_error();
}

bool is_private_or_loopback_host(const std::string& raw_host) {
    std::string host = lower_ascii(raw_host);
    while (!host.empty() && host.back() == '.') {
        host.pop_back();
    }
    if (host == "localhost" || ends_with(host, ".localhost")) {
        return true;
    }
    if (host == "metadata.google.internal" || host == "metadata" || host == "metadata.local") {
        return true;
    }
    std::vector<int> ipv4;
    if (parse_ipv4_literal(host, ipv4)) {
        return is_blocked_ipv4(ipv4);
    }
    if (starts_with(host, "::ffff:") && parse_ipv4_literal(host.substr(7), ipv4)) {
        return is_blocked_ipv4(ipv4);
    }
    if (host.find(':') != std::string::npos) {
        if (host == "::1" || host == "0:0:0:0:0:0:0:1") {
            return true;
        }
        const size_t colon = host.find(':');
        const std::string first = host.substr(0, colon);
        if (starts_with(first, "fc") || starts_with(first, "fd") || starts_with(first, "ff")) {
            return true;
        }
        if (first.size() >= 3 && first[0] == 'f' && first[1] == 'e' &&
            (first[2] == '8' || first[2] == '9' || first[2] == 'a' || first[2] == 'b')) {
            return true;
        }
    }
    return false;
}

pkchat::Error read_html_file(const std::string& path, std::string& body) {
    std::ostringstream buffer;
    if (path == "-") {
        buffer << std::cin.rdbuf();
        if (!std::cin.good() && !std::cin.eof()) {
            return {pkchat::ErrorCode::FileRead, "could not read HTML from stdin"};
        }
        body = buffer.str();
        return pkchat::ok_error();
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {pkchat::ErrorCode::FileRead, "could not open HTML file for reading: " + path};
    }
    buffer << file.rdbuf();
    if (!file.good() && !file.eof()) {
        return {pkchat::ErrorCode::FileRead, "could not read HTML file: " + path};
    }
    body = buffer.str();
    return pkchat::ok_error();
}

bool is_supported_html_content_type(const std::string& content_type) {
    if (content_type.empty()) {
        return true;
    }
    std::string type = lower_ascii(content_type);
    const size_t semi = type.find(';');
    if (semi != std::string::npos) {
        type = type.substr(0, semi);
    }
    type = trim_ascii(type);
    return type == "text/html" || type == "application/xhtml+xml";
}

pkchat::Error fetch_html_url(const pkchat::cli::Options& options, std::string& body) {
    if (options.max_fetch_bytes <= 0) {
        return {pkchat::ErrorCode::BadArgs, "--max-fetch-bytes must be greater than zero for --fetch-url"};
    }
    FetchUrlParts parts;
    pkchat::Error err = parse_fetch_url(options.fetch_url, parts);
    if (!err.ok()) {
        return err;
    }
    if (!options.allow_private_url_fetch && is_private_or_loopback_host(parts.host)) {
        return {pkchat::ErrorCode::BadUrl,
                "refusing to fetch private, loopback, link-local, multicast, or metadata URL without "
                "--allow-private-url-fetch: " + options.fetch_url};
    }

    pkchat::http::Request request;
    request.method = "GET";
    request.url = options.fetch_url;
    request.headers.push_back(
        "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0");
    request.headers.push_back("Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");
    request.headers.push_back("Accept-Language: en-US,en;q=0.5");
    request.headers.push_back("Upgrade-Insecure-Requests: 1");
    request.connect_timeout_seconds = options.connect_timeout_seconds;
    request.timeout_seconds = options.timeout_seconds > 0 ? options.timeout_seconds : 30;
    request.proxy = options.proxy;
    request.insecure_tls = options.insecure_tls;
    request.trace = options.trace_http;
    request.max_body_bytes = options.max_fetch_bytes;

    pkchat::http::Result result = pkchat::http::perform(request, {});
    if (!result.error.ok()) {
        return result.error;
    }
    if (result.response.status < 200 || result.response.status >= 300) {
        return {pkchat::ErrorCode::HttpStatus,
                "HTTP " + std::to_string(result.response.status) + " while fetching URL: " + options.fetch_url};
    }
    if (!is_supported_html_content_type(result.response.content_type)) {
        return {pkchat::ErrorCode::UnsupportedFeature,
                "fetched URL did not return an HTML content type: " + options.fetch_url +
                    " (Content-Type: " + result.response.content_type + ")"};
    }
    body = std::move(result.response.body);
    if (!options.quiet) {
        std::cerr << "Fetched URL: " << options.fetch_url << "\n";
    }
    return pkchat::ok_error();
}

pkchat::Error validate_html_extract_options(const pkchat::cli::Options& options) {
    if (!options.fetch_url.empty() && !options.html_file.empty()) {
        return {pkchat::ErrorCode::BadArgs, "--fetch-url and --html-file cannot be combined"};
    }
    if (options.editor || options.repl || options.tui || options.list_models) {
        return {pkchat::ErrorCode::BadArgs,
                "HTML extraction cannot be combined with --editor, --repl, --tui, or --list-models"};
    }
    if (!options.prompt.empty() || !options.prompt_file.empty() || !options.system.empty() || !options.system_file.empty()) {
        return {pkchat::ErrorCode::BadArgs, "HTML extraction cannot be combined with prompt or system options"};
    }
    if (!options.load_chat_path.empty() || !options.save_chat_path.empty()) {
        return {pkchat::ErrorCode::BadArgs, "HTML extraction cannot be combined with --load-chat or --save-chat"};
    }
    if (options.format != pkchat::cli::OutputFormat::Text) {
        return {pkchat::ErrorCode::BadArgs, "HTML extraction uses --html-format text|markdown, not --format"};
    }
    return pkchat::ok_error();
}

int run_html_extract(const pkchat::cli::Options& options, std::ostream& out) {
    pkchat::Error err = validate_html_extract_options(options);
    if (!err.ok()) {
        print_error(err);
        return exit_code_for(err.code);
    }
    pkchat::html::OutputFormat format = pkchat::html::OutputFormat::Markdown;
    if (!pkchat::html::parse_output_format(options.html_format, format)) {
        err = {pkchat::ErrorCode::BadArgs, "--html-format must be text or markdown"};
        print_error(err);
        return exit_code_for(err.code);
    }

    std::string body;
    if (!options.html_file.empty()) {
        err = read_html_file(options.html_file, body);
    } else {
        err = fetch_html_url(options, body);
    }
    if (!err.ok()) {
        print_error(err);
        return exit_code_for(err.code);
    }
    out << pkchat::html::convert(body, format);
    return 0;
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
    std::cerr << "Endpoint: " << pkchat::provider::active_request_url(context) << std::endl;
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
    if (!options.fetch_url.empty() || !options.html_file.empty()) {
        std::ofstream out_file;
        pkchat::Error output_error;
        std::ostream* out = output_stream(options, out_file, output_error);
        if (!output_error.ok()) {
            print_error(output_error);
            return exit_code_for(output_error.code);
        }
        return run_html_extract(options, *out);
    }
    if (options.editor && (options.repl || options.tui)) {
        print_error({pkchat::ErrorCode::BadArgs, "--editor cannot be combined with --repl or --tui"});
        return exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.repl && options.tui) {
        print_error({pkchat::ErrorCode::BadArgs, "--repl cannot be combined with --tui"});
        return exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.repl && options.list_models) {
        print_error({pkchat::ErrorCode::BadArgs, "--repl cannot be combined with --list-models"});
        return exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.tui && options.list_models) {
        print_error({pkchat::ErrorCode::BadArgs, "--tui cannot be combined with --list-models; use /models inside the TUI"});
        return exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.editor && options.list_models) {
        print_error({pkchat::ErrorCode::BadArgs, "--editor cannot be combined with --list-models"});
        return exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.repl && options.format != pkchat::cli::OutputFormat::Text) {
        print_error({pkchat::ErrorCode::BadArgs, "--repl currently supports --format text only"});
        return exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.tui && options.format != pkchat::cli::OutputFormat::Text) {
        print_error({pkchat::ErrorCode::BadArgs, "--tui currently supports --format text only"});
        return exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.tui && !options.output_path.empty()) {
        print_error({pkchat::ErrorCode::BadArgs, "--tui cannot be combined with --output"});
        return exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.editor && options.format != pkchat::cli::OutputFormat::Text) {
        print_error({pkchat::ErrorCode::BadArgs, "--editor does not use --format"});
        return exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.editor && (!options.prompt.empty() || !options.prompt_file.empty() ||
                           !options.system.empty() || !options.system_file.empty())) {
        print_error({pkchat::ErrorCode::BadArgs, "--editor cannot be combined with prompt or system options"});
        return exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.editor && (!options.load_chat_path.empty() || !options.save_chat_path.empty())) {
        print_error({pkchat::ErrorCode::BadArgs, "--editor cannot be combined with --load-chat or --save-chat"});
        return exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.editor) {
        return pkchat::editor::run_editor(options.positional_url, options.output_path);
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

    if (context.options.tui) {
        return pkchat::tui::run(context, std::move(session));
    }

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
