#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

#include "chat/session.hpp"
#include "cli/args.hpp"
#include "common.hpp"
#include "context/context.hpp"
#include "editor/editor.hpp"
#include "fetch/fetch.hpp"
#include "html/html.hpp"
#include "input/input.hpp"
#include "json/json.hpp"
#include "markdown/markdown.hpp"
#include "output/thinking.hpp"
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
    if (options.output_path.empty() || options.output_path == "stdout") {
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

pkchat::Error read_local_file(const std::string& path,
                              const std::string& description,
                              size_t max_bytes,
                              std::string& body) {
    std::ifstream file;
    std::istream* input = &std::cin;
    if (path != "stdin") {
        file.open(path, std::ios::binary);
        if (!file) {
            return {pkchat::ErrorCode::FileRead, "could not open " + description + " for reading: " + path};
        }
        input = &file;
    }

    std::string loaded;
    std::array<char, 8192> buffer{};
    while (*input) {
        input->read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input->gcount();
        if (count <= 0) {
            break;
        }
        const size_t chunk_size = static_cast<size_t>(count);
        if (loaded.size() > max_bytes || chunk_size > max_bytes - loaded.size()) {
            return {pkchat::ErrorCode::UnsupportedFeature,
                    description + " exceeds --max-input-bytes limit of " + std::to_string(max_bytes) +
                        " bytes: " + (path == "stdin" ? std::string("stdin") : path)};
        }
        loaded.append(buffer.data(), chunk_size);
    }
    if (input->bad()) {
        return {pkchat::ErrorCode::FileRead,
                "could not read " + description + (path == "stdin" ? std::string(" from stdin") : ": " + path)};
    }
    body = std::move(loaded);
    return pkchat::ok_error();
}

pkchat::Error validate_html_utf8(const std::string& body, const std::string& source) {
    size_t offset = 0;
    if (pkchat::html::is_valid_utf8(body, &offset)) {
        return pkchat::ok_error();
    }
    return {pkchat::ErrorCode::UnsupportedFeature,
            "HTML extraction expects UTF-8 input; charset conversion is not implemented yet for " + source +
                " (invalid byte at offset " + std::to_string(offset) +
                "). Convert the document to UTF-8 and try again."};
}

pkchat::fetch::Options fetch_options_for(const pkchat::cli::Options& options) {
    pkchat::fetch::Options fetch_options;
    fetch_options.connect_timeout_seconds = options.connect_timeout_seconds;
    fetch_options.timeout_seconds = options.timeout_seconds > 0 ? options.timeout_seconds : 30;
    fetch_options.max_bytes = options.max_fetch_bytes;
    fetch_options.proxy = options.proxy;
    fetch_options.insecure_tls = options.insecure_tls;
    fetch_options.trace_http = options.trace_http;
    fetch_options.allow_private = options.allow_private_url_fetch;
    return fetch_options;
}

pkchat::Error fetch_html_url(const pkchat::cli::Options& options, std::string& body) {
    pkchat::Error err = pkchat::fetch::fetch_html(
        options.fetch_url, fetch_options_for(options), body);
    if (!err.ok()) {
        return err;
    }
    if (!options.quiet) {
        std::cerr << "Fetched URL: " << options.fetch_url << "\n";
    }
    return pkchat::ok_error();
}

using InputKind = pkchat::input::Kind;

const char* input_kind_name(InputKind kind) {
    switch (kind) {
        case InputKind::Plaintext:
            return "plaintext";
        case InputKind::Markdown:
            return "markdown";
        case InputKind::Html:
            return "html";
        case InputKind::Image:
            return "image";
    }
    return "plaintext";
}

std::string local_input_path(const pkchat::cli::Options& options) {
    return !options.input_path.empty() ? options.input_path : options.html_file;
}

bool has_local_input_source(const pkchat::cli::Options& options) {
    return !options.input_path.empty() || !options.html_file.empty();
}

bool has_document_source(const pkchat::cli::Options& options) {
    return !options.fetch_url.empty() || has_local_input_source(options);
}

bool wants_document_prompt_context(const pkchat::cli::Options& options) {
    return has_document_source(options) && (!options.prompt.empty() || !options.prompt_file.empty());
}

std::string document_source_label(const pkchat::cli::Options& options) {
    if (!options.fetch_url.empty()) {
        return "URL " + options.fetch_url;
    }
    const std::string path = local_input_path(options);
    return path == "stdin" ? std::string("stdin") : "file " + path;
}

pkchat::Error validate_document_source_options(const pkchat::cli::Options& options) {
    if (!options.fetch_url.empty() && has_local_input_source(options)) {
        return {pkchat::ErrorCode::BadArgs, "--fetch-url and --input cannot be combined"};
    }
    if (!options.input_path.empty() && !options.html_file.empty()) {
        return {pkchat::ErrorCode::BadArgs, "--input and --html-file cannot be combined"};
    }
    if (has_local_input_source(options) && local_input_path(options).empty()) {
        return {pkchat::ErrorCode::BadArgs, "--input requires a non-empty path"};
    }
    return pkchat::ok_error();
}

pkchat::Error validate_stdin_sources(const pkchat::cli::Options& options) {
    size_t consumers = 0;
    consumers += local_input_path(options) == "stdin" ? 1U : 0U;
    consumers += options.prompt_file == "-" ? 1U : 0U;
    consumers += options.system_file == "-" ? 1U : 0U;
    consumers += options.key_stdin ? 1U : 0U;
    consumers += options.key_file == "-" ? 1U : 0U;
    for (const std::string& path : options.attachment_paths) {
        consumers += path == "stdin" ? 1U : 0U;
    }
    if (consumers > 1) {
        return {pkchat::ErrorCode::BadArgs,
                "stdin can only be consumed once; use one of --input stdin, --attach stdin, "
                "--prompt-file -, --system-file -, --key-file -, or --key-stdin"};
    }
    return pkchat::ok_error();
}

pkchat::Error local_input_type_for_options(const pkchat::cli::Options& options,
                                           pkchat::input::FileType& type) {
    if (!options.html_file.empty()) {
        type = {InputKind::Html, "html", "text/html"};
        return pkchat::ok_error();
    }
    return pkchat::input::classify_file_type(local_input_path(options), type);
}

pkchat::Error validate_not_binary(const std::string& body, const std::string& source) {
    const size_t nul = body.find('\0');
    if (nul != std::string::npos) {
        return {pkchat::ErrorCode::UnsupportedFeature,
                "input appears to be binary: " + source + " contains a NUL byte at offset " +
                    std::to_string(nul)};
    }
    return pkchat::ok_error();
}

pkchat::Error validate_text_utf8(const std::string& body, const std::string& source) {
    size_t offset = 0;
    if (pkchat::html::is_valid_utf8(body, &offset)) {
        return pkchat::ok_error();
    }
    return {pkchat::ErrorCode::UnsupportedFeature,
            "Input expects UTF-8 text; charset conversion is not implemented yet for " + source +
                " (invalid byte at offset " + std::to_string(offset) +
                "). Convert the document to UTF-8 and try again."};
}

pkchat::markdown::OutputFormat legacy_html_output_format(const pkchat::cli::Options& options) {
    pkchat::html::OutputFormat html_format = pkchat::html::OutputFormat::Markdown;
    if (pkchat::html::parse_output_format(options.html_format, html_format) &&
        html_format == pkchat::html::OutputFormat::Text) {
        return pkchat::markdown::OutputFormat::Plaintext;
    }
    return pkchat::markdown::OutputFormat::Markdown;
}

pkchat::markdown::OutputFormat document_output_format(const pkchat::cli::Options& options,
                                                       InputKind kind,
                                                       bool standalone) {
    if (standalone && options.rendered_output_format_explicit) {
        return options.output_format;
    }
    if (kind == InputKind::Html) {
        return legacy_html_output_format(options);
    }
    if (kind == InputKind::Markdown) {
        return pkchat::markdown::OutputFormat::Markdown;
    }
    return pkchat::markdown::OutputFormat::Plaintext;
}

std::string render_document_body(const std::string& body,
                                 InputKind kind,
                                 pkchat::markdown::OutputFormat output_format,
                                 bool complete_html_document) {
    if (kind == InputKind::Html) {
        if (output_format == pkchat::markdown::OutputFormat::Plaintext) {
            return pkchat::html::convert(body, pkchat::html::OutputFormat::Text);
        }
        const std::string markdown = pkchat::html::convert(body, pkchat::html::OutputFormat::Markdown);
        if (output_format == pkchat::markdown::OutputFormat::Html) {
            return pkchat::markdown::render(markdown, output_format, complete_html_document);
        }
        return markdown;
    }
    if (kind == InputKind::Markdown) {
        return pkchat::markdown::render(body, output_format, complete_html_document);
    }
    if (kind == InputKind::Image) {
        return "";
    }
    if (output_format == pkchat::markdown::OutputFormat::Plaintext ||
        output_format == pkchat::markdown::OutputFormat::Markdown) {
        return body;
    }
    return pkchat::markdown::render(body, output_format, complete_html_document);
}

struct LoadedDocument {
    std::string source;
    InputKind input_kind = InputKind::Plaintext;
    pkchat::markdown::OutputFormat output_format = pkchat::markdown::OutputFormat::Markdown;
    std::string converted;
    pkchat::provider::ImageInput image;
};

pkchat::Error load_document(const pkchat::cli::Options& options, bool standalone, LoadedDocument& document) {
    pkchat::Error err = validate_document_source_options(options);
    if (!err.ok()) {
        return err;
    }

    std::string body;
    pkchat::input::FileType input_type;
    if (!options.fetch_url.empty()) {
        input_type = {InputKind::Html, "html", "text/html"};
        err = fetch_html_url(options, body);
    } else {
        err = local_input_type_for_options(options, input_type);
        if (!err.ok()) {
            return err;
        }
        if (input_type.kind == InputKind::Image) {
            if (standalone) {
                return {pkchat::ErrorCode::BadArgs,
                        "image input cannot be extracted as text; combine --input IMAGE with -p or --prompt"};
            }
            if (options.max_image_bytes <= 0) {
                return {pkchat::ErrorCode::BadArgs, "--max-image-bytes must be greater than zero"};
            }
            pkchat::input::ImageData image;
            err = pkchat::input::load_image_file(local_input_path(options), input_type,
                                                  static_cast<size_t>(options.max_image_bytes), image);
            if (!err.ok()) {
                return err;
            }
            document.source = document_source_label(options);
            document.input_kind = InputKind::Image;
            document.image = {image.mime_type, std::move(image.base64_data)};
            if (!options.quiet) {
                std::cerr << "Attached image: " << local_input_path(options) << " (" << image.mime_type
                          << ", " << image.byte_size << " bytes)\n";
            }
            return pkchat::ok_error();
        }
        if (options.max_input_bytes <= 0) {
            return {pkchat::ErrorCode::BadArgs, "--max-input-bytes must be greater than zero"};
        }
        err = read_local_file(local_input_path(options), input_type.name,
                              static_cast<size_t>(options.max_input_bytes), body);
    }
    if (!err.ok()) {
        return err;
    }

    document.source = document_source_label(options);
    document.input_kind = input_type.kind;
    err = validate_not_binary(body, document.source);
    if (!err.ok()) {
        return err;
    }
    if (document.input_kind == InputKind::Html) {
        err = validate_html_utf8(body, document.source);
    } else {
        err = validate_text_utf8(body, document.source);
    }
    if (!err.ok()) {
        return err;
    }

    document.output_format = document_output_format(options, document.input_kind, standalone);
    const bool complete_html_document = standalone &&
                                        document.output_format == pkchat::markdown::OutputFormat::Html &&
                                        !options.output_path.empty() && options.output_path != "stdout";
    document.converted = render_document_body(body, document.input_kind, document.output_format, complete_html_document);
    return pkchat::ok_error();
}

pkchat::Error validate_document_extract_options(const pkchat::cli::Options& options) {
    pkchat::Error err = validate_document_source_options(options);
    if (!err.ok()) {
        return err;
    }
    if (options.editor || options.repl || options.tui || options.list_models) {
        return {pkchat::ErrorCode::BadArgs,
                "input extraction cannot be combined with --editor, --repl, --chat, or --list-models"};
    }
    if (!options.prompt.empty() || !options.prompt_file.empty() || !options.system.empty() || !options.system_file.empty()) {
        return {pkchat::ErrorCode::BadArgs, "input extraction cannot be combined with prompt or system options"};
    }
    if (!options.load_chat_path.empty() || !options.save_chat_path.empty()) {
        return {pkchat::ErrorCode::BadArgs, "input extraction cannot be combined with --load-chat or --save-chat"};
    }
    return pkchat::ok_error();
}

void write_document_json(std::ostream& out, const LoadedDocument& document) {
    out << "{"
        << "\"source\":" << pkchat::json::quote(document.source) << ","
        << "\"input_format\":" << pkchat::json::quote(input_kind_name(document.input_kind)) << ","
        << "\"output_format\":" << pkchat::json::quote(pkchat::markdown::output_format_name(document.output_format)) << ","
        << "\"content\":" << pkchat::json::quote(document.converted)
        << "}\n";
}

std::string document_context_message(const LoadedDocument& document) {
    std::string message = "Input context from " + document.source + "\n";
    message += "Format: ";
    message += pkchat::markdown::output_format_name(document.output_format);
    message += "\n\n";
    message += document.converted;
    return message;
}

pkchat::Error load_text_context_file(const pkchat::cli::Options& options,
                                     const std::string& path,
                                     const std::string& option_name,
                                     LoadedDocument& document) {
    if (path.empty()) {
        return {pkchat::ErrorCode::BadArgs, option_name + " requires a non-empty path"};
    }
    if (options.max_input_bytes <= 0) {
        return {pkchat::ErrorCode::BadArgs, "--max-input-bytes must be greater than zero"};
    }
    pkchat::input::TextContext loaded;
    pkchat::Error err = pkchat::input::load_text_context_file(
        path, static_cast<size_t>(options.max_input_bytes), loaded);
    if (!err.ok()) {
        return err;
    }
    document.source = std::move(loaded.source);
    document.input_kind = loaded.kind;
    document.output_format = loaded.kind == InputKind::Plaintext
                                 ? pkchat::markdown::OutputFormat::Plaintext
                                 : pkchat::markdown::OutputFormat::Markdown;
    document.converted = std::move(loaded.content);
    return pkchat::ok_error();
}

int run_document_extract(const pkchat::cli::Options& options, std::ostream& out) {
    pkchat::Error err = validate_document_extract_options(options);
    if (!err.ok()) {
        print_error(err);
        return exit_code_for(err.code);
    }

    LoadedDocument document;
    err = load_document(options, true, document);
    if (!err.ok()) {
        print_error(err);
        return exit_code_for(err.code);
    }

    if (options.format == pkchat::cli::OutputFormat::Json) {
        write_document_json(out, document);
    } else if (options.format == pkchat::cli::OutputFormat::Ndjson) {
        out << "{\"event\":\"start\",\"source\":" << pkchat::json::quote(document.source) << "}\n";
        out << "{\"event\":\"content\",\"text\":" << pkchat::json::quote(document.converted) << "}\n";
        out << "{\"event\":\"done\",\"output_format\":"
            << pkchat::json::quote(pkchat::markdown::output_format_name(document.output_format)) << "}\n";
    } else {
        out << document.converted;
    }
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

bool streams_raw_markdown_output(const pkchat::cli::Options& options) {
    return options.format == pkchat::cli::OutputFormat::Text &&
           options.output_format == pkchat::markdown::OutputFormat::Markdown;
}

void write_rendered_assistant_output(const pkchat::cli::Options& options,
                                     const std::string& content,
                                     std::ostream& out) {
    const bool complete_html_document = options.output_format == pkchat::markdown::OutputFormat::Html &&
                                        !options.output_path.empty() && options.output_path != "stdout";
    const std::string rendered = pkchat::markdown::render(content, options.output_format, complete_html_document);
    out << rendered;
    if (rendered.empty() || rendered.back() != '\n') {
        out << '\n';
    }
}

pkchat::Error send_session_turn(pkchat::provider::RequestContext& context,
                                pkchat::chat::Session& session,
                                const std::string& prompt,
                                std::ostream& out,
                                pkchat::provider::ChatResult& chat,
                                std::vector<pkchat::provider::ImageInput> images = {},
                                bool separate_thinking_traces = false) {
    session.messages.push_back({"user", prompt, std::move(images)});
    pkchat::context::PreparedMessages prepared = pkchat::context::prepare(
        session.messages,
        context.options.context_policy,
        context.options.max_context_bytes > 0
            ? static_cast<size_t>(context.options.max_context_bytes)
            : 0U);
    if (!prepared.error.ok()) {
        session.messages.pop_back();
        return prepared.error;
    }
    session.messages.back().images.clear();
    bool started_ndjson = false;
    pkchat::output::ThinkingTraceSplitter thinking_splitter;
    std::string visible_content;
    bool emitted_trace = false;
    char last_trace_character = '\0';
    auto emit_trace = [&](const std::string& trace) {
        if (trace.empty()) {
            return;
        }
        emitted_trace = true;
        last_trace_character = trace.back();
        std::cerr << trace;
        std::cerr.flush();
    };
    auto on_delta = [&](const std::string& delta) -> pkchat::Error {
        std::string visible_delta = delta;
        if (separate_thinking_traces) {
            pkchat::output::ThinkingChunk split = thinking_splitter.feed(delta);
            visible_delta = std::move(split.visible);
            visible_content += visible_delta;
            emit_trace(split.trace);
        }
        if (visible_delta.empty()) {
            return pkchat::ok_error();
        }
        if (context.options.format == pkchat::cli::OutputFormat::Text) {
            if (streams_raw_markdown_output(context.options)) {
                out << visible_delta;
                out.flush();
            }
        } else if (context.options.format == pkchat::cli::OutputFormat::Ndjson) {
            if (!started_ndjson) {
                out << "{\"event\":\"start\",\"model\":" << pkchat::json::quote(context.options.model) << "}\n";
                started_ndjson = true;
            }
            out << "{\"event\":\"delta\",\"text\":" << pkchat::json::quote(visible_delta) << "}\n";
            out.flush();
        }
        return pkchat::ok_error();
    };

    pkchat::Error err = pkchat::provider::send_chat_messages(context, prepared.messages, on_delta, chat);
    if (!err.ok()) {
        session.messages.pop_back();
        return err;
    }
    if (separate_thinking_traces) {
        if (context.options.stream) {
            pkchat::output::ThinkingChunk final = thinking_splitter.finish();
            visible_content += final.visible;
            emit_trace(final.trace);
            if (!final.visible.empty()) {
                if (context.options.format == pkchat::cli::OutputFormat::Text &&
                    streams_raw_markdown_output(context.options)) {
                    out << final.visible;
                    out.flush();
                } else if (context.options.format == pkchat::cli::OutputFormat::Ndjson) {
                    if (!started_ndjson) {
                        out << "{\"event\":\"start\",\"model\":"
                            << pkchat::json::quote(context.options.model) << "}\n";
                        started_ndjson = true;
                    }
                    out << "{\"event\":\"delta\",\"text\":"
                        << pkchat::json::quote(final.visible) << "}\n";
                }
            }
        } else {
            pkchat::output::ThinkingChunk split = pkchat::output::split_thinking_traces(chat.content);
            visible_content = std::move(split.visible);
            emit_trace(split.trace);
        }
        if (emitted_trace && last_trace_character != '\n') {
            std::cerr << '\n';
        }
    } else {
        visible_content = chat.content;
    }
    session.messages.push_back({"assistant", chat.content});
    if (prepared.compacted) {
        prepared.event.timestamp = pkchat::chat::current_timestamp_utc();
        session.compaction_events.push_back(prepared.event);
        if (!context.options.quiet) {
            std::cerr << prepared.event.notice << "\n";
        }
    }
    if (!chat.model.empty()) {
        session.model = chat.model;
    }
    if (!chat.usage_json.empty() && chat.usage_json != "null") {
        session.usage_json = chat.usage_json;
    }

    if (context.options.format == pkchat::cli::OutputFormat::Text) {
        if (context.options.output_format == pkchat::markdown::OutputFormat::Markdown) {
            if (!context.options.stream) {
                out << visible_content;
            }
            out << "\n";
        } else {
            write_rendered_assistant_output(context.options, visible_content, out);
        }
    } else if (context.options.format == pkchat::cli::OutputFormat::Json) {
        pkchat::provider::ChatResult visible_chat = chat;
        visible_chat.content = std::move(visible_content);
        write_json_chat(out, context, visible_chat);
    } else {
        if (!started_ndjson) {
            out << "{\"event\":\"start\",\"model\":" << pkchat::json::quote(context.options.model) << "}\n";
        }
        if (!context.options.stream && !visible_content.empty()) {
            out << "{\"event\":\"delta\",\"text\":" << pkchat::json::quote(visible_content) << "}\n";
        }
        out << "{\"event\":\"done\",\"usage\":null}\n";
    }
    return pkchat::ok_error();
}

void print_repl_help() {
    std::cerr << "Commands: /help, /quit, /exit, /save [PATH], /load PATH, /insert PATH, /attach PATH, "
                 "/fetch URL, /clear, /system TEXT, /model MODEL\n";
}

int run_repl(pkchat::provider::RequestContext context, pkchat::chat::Session session, std::ostream& out) {
    refresh_session_metadata(session, context);
    apply_system_prompt(session, context.options.system);
    if (!context.options.quiet) {
        std::cerr << "pkchat REPL. Type /help for commands, /quit to exit.\n";
    }

    std::vector<pkchat::provider::ImageInput> pending_images;

    auto send_prompt = [&](const std::string& text) -> int {
        pkchat::provider::ChatResult chat;
        pkchat::Error err = send_session_turn(context, session, text, out, chat, pending_images);
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
                pending_images.clear();
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
                pending_images.clear();
                refresh_session_metadata(session, context);
                if (!context.options.quiet) {
                    std::cerr << "Loaded chat from " << path << "\n";
                }
                continue;
            }
            if (text == "/insert" || text.rfind("/insert ", 0) == 0 ||
                text == "/attach" || text.rfind("/attach ", 0) == 0) {
                const std::string path = trim_ascii(text.substr(7));
                if (path.empty()) {
                    std::cerr << "Usage: /insert PATH or /attach PATH\n";
                    continue;
                }
                if (path == "stdin") {
                    std::cerr << "stdin input is only supported by non-interactive --input and --attach\n";
                    continue;
                }
                pkchat::input::FileType type;
                pkchat::Error err = pkchat::input::classify_file_type(path, type);
                if (!err.ok()) {
                    print_error(err);
                    continue;
                }
                if (type.kind == InputKind::Image) {
                    err = pkchat::provider::validate_image_input(context);
                    if (!err.ok()) {
                        print_error(err);
                        continue;
                    }
                    if (context.options.max_image_bytes <= 0) {
                        print_error({pkchat::ErrorCode::BadArgs, "--max-image-bytes must be greater than zero"});
                        continue;
                    }
                    pkchat::input::ImageData image;
                    err = pkchat::input::load_image_file(
                        path, type, static_cast<size_t>(context.options.max_image_bytes), image);
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
                    err = load_text_context_file(context.options, path, "/insert", document);
                    if (!err.ok()) {
                        print_error(err);
                        continue;
                    }
                    session.messages.push_back({"user", document_context_message(document)});
                    if (!context.options.quiet) {
                        std::cerr << "Inserted context from " << path << "\n";
                    }
                }
                continue;
            }
            if (text == "/fetch" || text.rfind("/fetch ", 0) == 0) {
                const std::string url = trim_ascii(text.substr(6));
                if (url.empty()) {
                    std::cerr << "Usage: /fetch URL\n";
                    continue;
                }
                std::string markdown;
                pkchat::Error err = pkchat::fetch::fetch_markdown(
                    url, fetch_options_for(context.options), markdown);
                if (!err.ok()) {
                    print_error(err);
                    continue;
                }
                pkchat::input::TextContext fetched;
                fetched.source = "URL " + url;
                fetched.kind = InputKind::Markdown;
                fetched.content = std::move(markdown);
                session.messages.push_back({"user", pkchat::input::text_context_message(fetched)});
                if (!context.options.quiet) {
                    std::cerr << "Fetched and inserted URL: " << url << "\n";
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
    pkchat::Error stdin_error = validate_stdin_sources(options);
    if (!stdin_error.ok()) {
        print_error(stdin_error);
        return exit_code_for(stdin_error.code);
    }
    if (!options.attachment_paths.empty()) {
        for (const std::string& path : options.attachment_paths) {
            if (path.empty()) {
                print_error({pkchat::ErrorCode::BadArgs, "--attach requires a non-empty path"});
                return exit_code_for(pkchat::ErrorCode::BadArgs);
            }
        }
        if (options.prompt.empty() && options.prompt_file.empty()) {
            print_error({pkchat::ErrorCode::BadArgs,
                         "--attach requires -p/--prompt or --prompt-file in non-interactive mode"});
            return exit_code_for(pkchat::ErrorCode::BadArgs);
        }
        if (options.editor || options.repl || options.tui || options.list_models) {
            print_error({pkchat::ErrorCode::BadArgs,
                         "--attach currently supports non-interactive prompt mode only; use /insert in the REPL or TUI"});
            return exit_code_for(pkchat::ErrorCode::BadArgs);
        }
    }
    if (has_document_source(options) && !wants_document_prompt_context(options)) {
        std::ofstream out_file;
        pkchat::Error output_error;
        std::ostream* out = output_stream(options, out_file, output_error);
        if (!output_error.ok()) {
            print_error(output_error);
            return exit_code_for(output_error.code);
        }
        return run_document_extract(options, *out);
    }
    if (wants_document_prompt_context(options) && (options.editor || options.repl || options.tui || options.list_models)) {
        print_error({pkchat::ErrorCode::BadArgs,
                     "--fetch-url/--input prompt context currently supports non-interactive prompt mode only"});
        return exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.rendered_output_format_explicit && options.format != pkchat::cli::OutputFormat::Text) {
        print_error({pkchat::ErrorCode::BadArgs, "--output-format can only be combined with --format text"});
        return exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.rendered_output_format_explicit && options.list_models) {
        print_error({pkchat::ErrorCode::BadArgs, "rendered --output-format cannot be combined with --list-models"});
        return exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.editor && (options.repl || options.tui)) {
        print_error({pkchat::ErrorCode::BadArgs, "--editor cannot be combined with --repl or --chat"});
        return exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.repl && options.tui) {
        print_error({pkchat::ErrorCode::BadArgs, "--repl cannot be combined with --chat"});
        return exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.repl && options.list_models) {
        print_error({pkchat::ErrorCode::BadArgs, "--repl cannot be combined with --list-models"});
        return exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.tui && options.list_models) {
        print_error({pkchat::ErrorCode::BadArgs, "--chat cannot be combined with --list-models; use /models inside the chat UI"});
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
        print_error({pkchat::ErrorCode::BadArgs, "--chat currently supports --format text only"});
        return exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.tui && !options.output_path.empty()) {
        print_error({pkchat::ErrorCode::BadArgs, "--chat cannot be combined with --output"});
        return exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.tui && options.rendered_output_format_explicit) {
        print_error({pkchat::ErrorCode::BadArgs, "--chat does not use rendered --output-format"});
        return exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.editor && options.format != pkchat::cli::OutputFormat::Text) {
        print_error({pkchat::ErrorCode::BadArgs, "--editor does not use --format"});
        return exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.editor && options.rendered_output_format_explicit) {
        print_error({pkchat::ErrorCode::BadArgs, "--editor does not use rendered --output-format"});
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

    std::string fetched_context_message;
    std::vector<std::string> attachment_context_messages;
    std::vector<pkchat::provider::ImageInput> prompt_images;
    std::vector<pkchat::input::FileType> attachment_types;
    attachment_types.reserve(context.options.attachment_paths.size());
    bool image_requested = false;
    if (wants_document_prompt_context(context.options) && !context.options.fetch_url.empty()) {
        image_requested = false;
    } else if (wants_document_prompt_context(context.options)) {
        pkchat::input::FileType type;
        pkchat::Error err = local_input_type_for_options(context.options, type);
        if (!err.ok()) {
            print_error(err);
            return exit_code_for(err.code);
        }
        image_requested = type.kind == InputKind::Image;
    }
    for (const std::string& path : context.options.attachment_paths) {
        pkchat::input::FileType type;
        pkchat::Error err = pkchat::input::classify_file_type(path, type);
        if (!err.ok()) {
            print_error(err);
            return exit_code_for(err.code);
        }
        image_requested = image_requested || type.kind == InputKind::Image;
        attachment_types.push_back(std::move(type));
    }
    bool model_chosen = false;
    if (image_requested) {
        pkchat::Error model_err = choose_default_model(context);
        if (!model_err.ok()) {
            print_error(model_err);
            return exit_code_for(model_err.code);
        }
        model_chosen = true;
        pkchat::Error capability_error = pkchat::provider::validate_image_input(context);
        if (!capability_error.ok()) {
            print_error(capability_error);
            return exit_code_for(capability_error.code);
        }
    }
    if (wants_document_prompt_context(context.options)) {
        LoadedDocument document;
        pkchat::Error err = load_document(context.options, false, document);
        if (!err.ok()) {
            print_error(err);
            return exit_code_for(err.code);
        }
        if (document.input_kind == InputKind::Image) {
            prompt_images.push_back(std::move(document.image));
        } else {
            fetched_context_message = document_context_message(document);
        }
    }
    attachment_context_messages.reserve(context.options.attachment_paths.size());
    for (size_t attachment_index = 0; attachment_index < context.options.attachment_paths.size(); ++attachment_index) {
        const std::string& path = context.options.attachment_paths[attachment_index];
        const pkchat::input::FileType& type = attachment_types[attachment_index];
        pkchat::Error err;
        if (type.kind == InputKind::Image) {
            if (context.options.max_image_bytes <= 0) {
                err = {pkchat::ErrorCode::BadArgs, "--max-image-bytes must be greater than zero"};
            } else {
                pkchat::input::ImageData image;
                err = pkchat::input::load_image_file(path, type,
                                                     static_cast<size_t>(context.options.max_image_bytes), image);
                if (err.ok()) {
                    prompt_images.push_back({image.mime_type, std::move(image.base64_data)});
                    if (!context.options.quiet) {
                        std::cerr << "Attached image: " << path << " (" << image.mime_type << ", "
                                  << image.byte_size << " bytes)\n";
                    }
                }
            }
        } else {
            LoadedDocument document;
            err = load_text_context_file(context.options, path, "--attach", document);
            if (err.ok()) {
                attachment_context_messages.push_back(document_context_message(document));
                if (!context.options.quiet) {
                    std::cerr << "Attached context: " << path << "\n";
                }
            }
        }
        if (!err.ok()) {
            print_error(err);
            return exit_code_for(err.code);
        }
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
    if (!model_chosen) {
        pkchat::Error model_err = choose_default_model(context);
        if (!model_err.ok()) {
            print_error(model_err);
            return exit_code_for(model_err.code);
        }
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

    if (!fetched_context_message.empty()) {
        session.messages.push_back({"user", fetched_context_message});
    }
    for (std::string& message : attachment_context_messages) {
        session.messages.push_back({"user", std::move(message)});
    }

    pkchat::provider::ChatResult chat;
    pkchat::Error err = send_session_turn(context, session, context.options.prompt, *out, chat,
                                          std::move(prompt_images), true);
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
