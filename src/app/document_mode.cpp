#include "app/app.hpp"

#include <array>
#include <fstream>
#include <iostream>
#include <utility>

#include "html/html.hpp"
#include "json/json.hpp"
#include "markdown/markdown.hpp"

namespace pkchat::app {

namespace {

using InputKind = input::Kind;

Error read_local_file(const std::string& path,
                      const std::string& description,
                      size_t max_bytes,
                      std::string& body) {
    std::ifstream file;
    std::istream* input = &std::cin;
    if (path != "stdin") {
        file.open(path, std::ios::binary);
        if (!file) {
            return {ErrorCode::FileRead, "could not open " + description + " for reading: " + path};
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
            return {ErrorCode::UnsupportedFeature,
                    description + " exceeds --max-input-bytes limit of " + std::to_string(max_bytes) +
                        " bytes: " + (path == "stdin" ? std::string("stdin") : path)};
        }
        loaded.append(buffer.data(), chunk_size);
    }
    if (input->bad()) {
        return {ErrorCode::FileRead,
                "could not read " + description + (path == "stdin" ? std::string(" from stdin") : ": " + path)};
    }
    body = std::move(loaded);
    return ok_error();
}

Error validate_html_utf8(const std::string& body, const std::string& source) {
    size_t offset = 0;
    if (html::is_valid_utf8(body, &offset)) {
        return ok_error();
    }
    return {ErrorCode::UnsupportedFeature,
            "HTML extraction expects UTF-8 input; charset conversion is not implemented yet for " + source +
                " (invalid byte at offset " + std::to_string(offset) +
                "). Convert the document to UTF-8 and try again."};
}

Error fetch_html_url(const cli::Options& options, std::string& body) {
    Error err = fetch::fetch_html(options.fetch_url, fetch_options_for(options), body);
    if (!err.ok()) {
        return err;
    }
    if (!options.quiet) {
        std::cerr << "Fetched URL: " << options.fetch_url << "\n";
    }
    return ok_error();
}

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

std::string local_input_path(const cli::Options& options) {
    return !options.input_path.empty() ? options.input_path : options.html_file;
}

bool has_local_input_source(const cli::Options& options) {
    return !options.input_path.empty() || !options.html_file.empty();
}

std::string document_source_label(const cli::Options& options) {
    if (!options.fetch_url.empty()) {
        return "URL " + options.fetch_url;
    }
    const std::string path = local_input_path(options);
    return path == "stdin" ? std::string("stdin") : "file " + path;
}

Error validate_document_source_options(const cli::Options& options) {
    if (!options.fetch_url.empty() && has_local_input_source(options)) {
        return {ErrorCode::BadArgs, "--fetch-url and --input cannot be combined"};
    }
    if (!options.input_path.empty() && !options.html_file.empty()) {
        return {ErrorCode::BadArgs, "--input and --html-file cannot be combined"};
    }
    if (has_local_input_source(options) && local_input_path(options).empty()) {
        return {ErrorCode::BadArgs, "--input requires a non-empty path"};
    }
    return ok_error();
}

Error validate_not_binary(const std::string& body, const std::string& source) {
    const size_t nul = body.find('\0');
    if (nul != std::string::npos) {
        return {ErrorCode::UnsupportedFeature,
                "input appears to be binary: " + source + " contains a NUL byte at offset " +
                    std::to_string(nul)};
    }
    return ok_error();
}

Error validate_text_utf8(const std::string& body, const std::string& source) {
    size_t offset = 0;
    if (html::is_valid_utf8(body, &offset)) {
        return ok_error();
    }
    return {ErrorCode::UnsupportedFeature,
            "Input expects UTF-8 text; charset conversion is not implemented yet for " + source +
                " (invalid byte at offset " + std::to_string(offset) +
                "). Convert the document to UTF-8 and try again."};
}

markdown::OutputFormat legacy_html_output_format(const cli::Options& options) {
    html::OutputFormat html_format = html::OutputFormat::Markdown;
    if (html::parse_output_format(options.html_format, html_format) &&
        html_format == html::OutputFormat::Text) {
        return markdown::OutputFormat::Plaintext;
    }
    return markdown::OutputFormat::Markdown;
}

markdown::OutputFormat document_output_format(const cli::Options& options,
                                              InputKind kind,
                                              bool standalone) {
    if (standalone && options.rendered_output_format_explicit) {
        return options.output_format;
    }
    if (kind == InputKind::Html) {
        return legacy_html_output_format(options);
    }
    if (kind == InputKind::Markdown) {
        return markdown::OutputFormat::Markdown;
    }
    return markdown::OutputFormat::Plaintext;
}

std::string render_document_body(const std::string& body,
                                 InputKind kind,
                                 markdown::OutputFormat output_format,
                                 bool complete_html_document) {
    if (kind == InputKind::Html) {
        if (output_format == markdown::OutputFormat::Plaintext) {
            return html::convert(body, html::OutputFormat::Text);
        }
        const std::string markdown = html::convert(body, html::OutputFormat::Markdown);
        if (output_format == markdown::OutputFormat::Html) {
            return markdown::render(markdown, output_format, complete_html_document);
        }
        return markdown;
    }
    if (kind == InputKind::Markdown) {
        return markdown::render(body, output_format, complete_html_document);
    }
    if (kind == InputKind::Image) {
        return "";
    }
    if (output_format == markdown::OutputFormat::Plaintext ||
        output_format == markdown::OutputFormat::Markdown) {
        return body;
    }
    return markdown::render(body, output_format, complete_html_document);
}

Error validate_document_extract_options(const cli::Options& options) {
    Error err = validate_document_source_options(options);
    if (!err.ok()) {
        return err;
    }
    if (options.editor || options.repl || options.tui || options.list_models) {
        return {ErrorCode::BadArgs,
                "input extraction cannot be combined with --editor, --repl, --chat, or --list-models"};
    }
    if (!options.prompt.empty() || !options.prompt_file.empty() || !options.system.empty() ||
        !options.system_file.empty()) {
        return {ErrorCode::BadArgs, "input extraction cannot be combined with prompt or system options"};
    }
    if (!options.load_chat_path.empty() || !options.save_chat_path.empty()) {
        return {ErrorCode::BadArgs, "input extraction cannot be combined with --load-chat or --save-chat"};
    }
    return ok_error();
}

void write_document_json(std::ostream& out, const LoadedDocument& document) {
    out << "{"
        << "\"source\":" << json::quote(document.source) << ","
        << "\"input_format\":" << json::quote(input_kind_name(document.input_kind)) << ","
        << "\"output_format\":" << json::quote(markdown::output_format_name(document.output_format)) << ","
        << "\"content\":" << json::quote(document.converted)
        << "}\n";
}

}  // namespace

fetch::Options fetch_options_for(const cli::Options& options) {
    fetch::Options fetch_options;
    fetch_options.connect_timeout_seconds = options.connect_timeout_seconds;
    fetch_options.timeout_seconds = options.timeout_seconds > 0 ? options.timeout_seconds : 30;
    fetch_options.max_bytes = options.max_fetch_bytes;
    fetch_options.proxy = options.proxy;
    fetch_options.insecure_tls = options.insecure_tls;
    fetch_options.trace_http = options.trace_http;
    fetch_options.allow_private = options.allow_private_url_fetch;
    return fetch_options;
}

bool has_document_source(const cli::Options& options) {
    return !options.fetch_url.empty() || !options.input_path.empty() || !options.html_file.empty();
}

bool wants_document_prompt_context(const cli::Options& options) {
    return has_document_source(options) && (!options.prompt.empty() || !options.prompt_file.empty());
}

Error validate_stdin_sources(const cli::Options& options) {
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
        return {ErrorCode::BadArgs,
                "stdin can only be consumed once; use one of --input stdin, --attach stdin, "
                "--prompt-file -, --system-file -, --key-file -, or --key-stdin"};
    }
    return ok_error();
}

Error local_input_type_for_options(const cli::Options& options, input::FileType& type) {
    if (!options.html_file.empty()) {
        type = {InputKind::Html, "html", "text/html"};
        return ok_error();
    }
    return input::classify_file_type(local_input_path(options), type);
}

Error load_document(const cli::Options& options, bool standalone, LoadedDocument& document) {
    Error err = validate_document_source_options(options);
    if (!err.ok()) {
        return err;
    }

    std::string body;
    input::FileType input_type;
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
                return {ErrorCode::BadArgs,
                        "image input cannot be extracted as text; combine --input IMAGE with -p or --prompt"};
            }
            if (options.max_image_bytes <= 0) {
                return {ErrorCode::BadArgs, "--max-image-bytes must be greater than zero"};
            }
            input::ImageData image;
            err = input::load_image_file(local_input_path(options), input_type,
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
            return ok_error();
        }
        if (options.max_input_bytes <= 0) {
            return {ErrorCode::BadArgs, "--max-input-bytes must be greater than zero"};
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
                                        document.output_format == markdown::OutputFormat::Html &&
                                        !options.output_path.empty() && options.output_path != "stdout";
    document.converted =
        render_document_body(body, document.input_kind, document.output_format, complete_html_document);
    return ok_error();
}

std::string document_context_message(const LoadedDocument& document) {
    std::string message = "Input context from " + document.source + "\n";
    message += "Format: ";
    message += markdown::output_format_name(document.output_format);
    message += "\n\n";
    message += document.converted;
    return message;
}

Error load_text_context_file(const cli::Options& options,
                             const std::string& path,
                             const std::string& option_name,
                             LoadedDocument& document) {
    if (path.empty()) {
        return {ErrorCode::BadArgs, option_name + " requires a non-empty path"};
    }
    if (options.max_input_bytes <= 0) {
        return {ErrorCode::BadArgs, "--max-input-bytes must be greater than zero"};
    }
    input::TextContext loaded;
    Error err = input::load_text_context_file(path, static_cast<size_t>(options.max_input_bytes), loaded);
    if (!err.ok()) {
        return err;
    }
    document.source = std::move(loaded.source);
    document.input_kind = loaded.kind;
    document.output_format = loaded.kind == InputKind::Plaintext ? markdown::OutputFormat::Plaintext
                                                                 : markdown::OutputFormat::Markdown;
    document.converted = std::move(loaded.content);
    return ok_error();
}

int run_document_extract(const cli::Options& options, std::ostream& out) {
    Error err = validate_document_extract_options(options);
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

    if (options.format == cli::OutputFormat::Json) {
        write_document_json(out, document);
    } else if (options.format == cli::OutputFormat::Ndjson) {
        out << "{\"event\":\"start\",\"source\":" << json::quote(document.source) << "}\n";
        out << "{\"event\":\"content\",\"text\":" << json::quote(document.converted) << "}\n";
        out << "{\"event\":\"done\",\"output_format\":"
            << json::quote(markdown::output_format_name(document.output_format)) << "}\n";
    } else {
        out << document.converted;
    }
    return 0;
}

}  // namespace pkchat::app