#include "input/input.hpp"

#include "common.hpp"

#include <array>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <string>
#include <utility>

#include "html/html.hpp"

namespace ainiux::input {
namespace {

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

Error validate_insert_text(const std::string& body, const std::string& source_description) {
    const size_t nul = body.find('\0');
    if (nul != std::string::npos) {
        return {ErrorCode::UnsupportedFeature,
                "cannot insert binary data from " + source_description +
                    ": NUL byte at offset " + std::to_string(nul)};
    }
    size_t invalid_offset = 0;
    if (!html::is_valid_utf8(body, &invalid_offset)) {
        return {ErrorCode::UnsupportedFeature,
                "cannot insert " + source_description +
                    ": input is not valid UTF-8 (invalid byte at offset " +
                    std::to_string(invalid_offset) +
                    "). Convert it to UTF-8 and try again."};
    }
    return ok_error();
}

Error normalize_insert_linebreaks(std::string& body, const std::string& source_description) {
    if (body.find('\r') == std::string::npos) {
        return ok_error();
    }
    try {
        std::string normalized;
        normalized.reserve(body.size());
        for (size_t index = 0; index < body.size(); ++index) {
            if (body[index] == '\r') {
                if (index + 1 < body.size() && body[index + 1] == '\n') {
                    ++index;
                }
                normalized.push_back('\n');
            } else {
                normalized.push_back(body[index]);
            }
        }
        body = std::move(normalized);
    } catch (const std::bad_alloc&) {
        return {ErrorCode::Internal,
                "not enough memory to normalize line endings from " + source_description};
    } catch (const std::length_error&) {
        return {ErrorCode::FileRead,
                "inserted text is too large to normalize from " + source_description};
    }
    return ok_error();
}

Error read_insert_file(const std::string& path,
                       size_t max_bytes,
                       std::string& body,
                       runtime::CancellationToken cancellation) {
    if (max_bytes == 0) {
        return {ErrorCode::BadArgs, "input.max_input_bytes must be greater than zero for /insert"};
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {ErrorCode::FileRead, "could not open file for insertion: " + path};
    }
    std::array<char, 8192> buffer{};
    while (file) {
        if (cancellation.cancelled()) {
            return {ErrorCode::Cancelled, "file insertion cancelled: " + path};
        }
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = file.gcount();
        if (count <= 0) {
            break;
        }
        const size_t chunk_size = static_cast<size_t>(count);
        if (body.size() > max_bytes || chunk_size > max_bytes - body.size()) {
            return {ErrorCode::UnsupportedFeature,
                    "file exceeds input.max_input_bytes limit of " +
                        std::to_string(max_bytes) + " bytes: " + path};
        }
        try {
            body.append(buffer.data(), chunk_size);
        } catch (const std::bad_alloc&) {
            return {ErrorCode::Internal, "not enough memory to insert file: " + path};
        } catch (const std::length_error&) {
            return {ErrorCode::FileRead, "file is too large to insert: " + path};
        }
    }
    if (file.bad()) {
        return {ErrorCode::FileRead, "could not read file for insertion: " + path};
    }
    return ok_error();
}

bool has_prefix(const std::string& data, const std::initializer_list<unsigned char>& prefix) {
    if (data.size() < prefix.size()) {
        return false;
    }
    size_t index = 0;
    for (unsigned char expected : prefix) {
        if (static_cast<unsigned char>(data[index++]) != expected) {
            return false;
        }
    }
    return true;
}

bool signature_matches(const std::string& data, const std::string& mime_type) {
    if (mime_type == "image/png") {
        return has_prefix(data, {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a});
    }
    if (mime_type == "image/jpeg") {
        return has_prefix(data, {0xff, 0xd8, 0xff});
    }
    if (mime_type == "image/gif") {
        return data.compare(0, 6, "GIF87a") == 0 || data.compare(0, 6, "GIF89a") == 0;
    }
    return false;
}

std::string base64_encode(const std::string& data) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= data.size()) {
        const unsigned int a = static_cast<unsigned char>(data[i++]);
        const unsigned int b = static_cast<unsigned char>(data[i++]);
        const unsigned int c = static_cast<unsigned char>(data[i++]);
        out.push_back(alphabet[a >> 2]);
        out.push_back(alphabet[((a & 0x03U) << 4U) | (b >> 4U)]);
        out.push_back(alphabet[((b & 0x0fU) << 2U) | (c >> 6U)]);
        out.push_back(alphabet[c & 0x3fU]);
    }
    const size_t remaining = data.size() - i;
    if (remaining == 1) {
        const unsigned int a = static_cast<unsigned char>(data[i]);
        out.push_back(alphabet[a >> 2]);
        out.push_back(alphabet[(a & 0x03U) << 4U]);
        out += "==";
    } else if (remaining == 2) {
        const unsigned int a = static_cast<unsigned char>(data[i]);
        const unsigned int b = static_cast<unsigned char>(data[i + 1]);
        out.push_back(alphabet[a >> 2]);
        out.push_back(alphabet[((a & 0x03U) << 4U) | (b >> 4U)]);
        out.push_back(alphabet[(b & 0x0fU) << 2U]);
        out.push_back('=');
    }
    return out;
}

}  // namespace

Error classify_file_type(const std::string& path, FileType& type) {
    const std::string resolved = expand_user_path(path);
    if (resolved == "stdin") {
        type = {Kind::Plaintext, "plaintext", "text/plain"};
        return ok_error();
    }
    if (resolved == "-") {
        return {ErrorCode::BadArgs,
                "--input - cannot infer a file type from the path ending; use a path with a supported extension"};
    }
    const std::string lower = ascii_lower(resolved);
    if (ends_with(lower, ".txt") || ends_with(lower, ".text")) {
        type = {Kind::Plaintext, "plaintext", "text/plain"};
        return ok_error();
    }
    if (ends_with(lower, ".md") || ends_with(lower, ".markdown")) {
        type = {Kind::Markdown, "markdown", "text/markdown"};
        return ok_error();
    }
    if (ends_with(lower, ".html") || ends_with(lower, ".htm")) {
        type = {Kind::Html, "html", "text/html"};
        return ok_error();
    }
    if (ends_with(lower, ".png")) {
        type = {Kind::Image, "image", "image/png"};
        return ok_error();
    }
    if (ends_with(lower, ".jpg") || ends_with(lower, ".jpeg")) {
        type = {Kind::Image, "image", "image/jpeg"};
        return ok_error();
    }
    if (ends_with(lower, ".gif")) {
        type = {Kind::Image, "image", "image/gif"};
        return ok_error();
    }
    // WebP input is intentionally disabled: common vision endpoints tested here do not decode it reliably.
    // if (ends_with(lower, ".webp")) type = {Kind::Image, "image", "image/webp"};
    return {ErrorCode::UnsupportedFeature,
            "unsupported input file type for " + resolved +
                "; supported endings are .txt, .text, .md, .markdown, .html, .htm, .png, .jpg, .jpeg, "
                "and .gif "
                "(case-insensitive)"};
}

Error load_image_file_bytes(const std::string& path,
                            const FileType& type,
                            size_t max_bytes,
                            std::string& bytes,
                            runtime::CancellationToken cancellation) {
    const std::string resolved = expand_user_path(path);
    if (type.kind != Kind::Image) {
        return {ErrorCode::Internal, "load_image_file_bytes called for non-image input: " + resolved};
    }
    std::ifstream file(resolved, std::ios::binary);
    if (!file) {
        return {ErrorCode::FileRead, "could not open image for reading: " + resolved};
    }
    std::string data;
    std::array<char, 8192> buffer{};
    while (file) {
        if (cancellation.cancelled()) {
            return {ErrorCode::Cancelled, "image read cancelled: " + resolved};
        }
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = file.gcount();
        if (count <= 0) {
            break;
        }
        const size_t chunk_size = static_cast<size_t>(count);
        if (data.size() > max_bytes || chunk_size > max_bytes - data.size()) {
            return {ErrorCode::UnsupportedFeature,
                    "image exceeds --max-image-bytes limit of " + std::to_string(max_bytes) +
                        " bytes: " + resolved};
        }
        data.append(buffer.data(), chunk_size);
    }
    if (file.bad()) {
        return {ErrorCode::FileRead, "could not read image: " + resolved};
    }
    if (data.empty()) {
        return {ErrorCode::UnsupportedFeature, "image file is empty: " + resolved};
    }
    if (!signature_matches(data, type.mime_type)) {
        return {ErrorCode::UnsupportedFeature,
                "image content does not match its " + type.mime_type + " file extension: " + resolved};
    }
    if (cancellation.cancelled()) {
        return {ErrorCode::Cancelled, "image read cancelled: " + resolved};
    }

    bytes = std::move(data);
    return ok_error();
}

Error load_image_file(const std::string& path,
                      const FileType& type,
                      size_t max_bytes,
                      ImageData& image,
                      runtime::CancellationToken cancellation) {
    std::string data;
    Error err = load_image_file_bytes(path, type, max_bytes, data, cancellation);
    if (!err.ok()) {
        return err;
    }
    ImageData loaded;
    loaded.mime_type = type.mime_type;
    loaded.byte_size = data.size();
    loaded.base64_data = base64_encode(data);
    if (cancellation.cancelled()) {
        return {ErrorCode::Cancelled, "image encoding cancelled: " + expand_user_path(path)};
    }
    image = std::move(loaded);
    return ok_error();
}

Error load_text_context_file(const std::string& path,
                             size_t max_bytes,
                             TextContext& context,
                             runtime::CancellationToken cancellation) {
    const std::string resolved = expand_user_path(path);
    FileType type;
    Error err = classify_file_type(resolved, type);
    if (!err.ok()) {
        return err;
    }
    if (type.kind == Kind::Image) {
        return {ErrorCode::UnsupportedFeature,
                "text insertion supports .txt, .md, and .html files; attach images to a prompt instead: " +
                    resolved};
    }

    std::ifstream file;
    std::istream* input = &std::cin;
    if (resolved != "stdin") {
        file.open(resolved, std::ios::binary);
        if (!file) {
            return {ErrorCode::FileRead, "could not open " + type.name + " for reading: " + resolved};
        }
        input = &file;
    }
    std::string body;
    std::array<char, 8192> buffer{};
    while (*input) {
        if (cancellation.cancelled()) {
            return {ErrorCode::Cancelled, "text insertion cancelled: " + resolved};
        }
        input->read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input->gcount();
        if (count <= 0) {
            break;
        }
        const size_t chunk_size = static_cast<size_t>(count);
        if (body.size() > max_bytes || chunk_size > max_bytes - body.size()) {
            return {ErrorCode::UnsupportedFeature,
                    type.name + " exceeds --max-input-bytes limit of " + std::to_string(max_bytes) +
                        " bytes: " + resolved};
        }
        body.append(buffer.data(), chunk_size);
    }
    if (input->bad()) {
        return {ErrorCode::FileRead,
                resolved == "stdin" ? "could not read plaintext from stdin"
                                    : "could not read " + type.name + ": " + resolved};
    }
    const size_t nul = body.find('\0');
    if (nul != std::string::npos) {
        return {ErrorCode::UnsupportedFeature,
                "input appears to be binary: " +
                    (resolved == "stdin" ? std::string("stdin") : "file " + resolved) +
                    " contains a NUL byte at offset " +
                    std::to_string(nul)};
    }
    size_t invalid_offset = 0;
    if (!html::is_valid_utf8(body, &invalid_offset)) {
        return {ErrorCode::UnsupportedFeature,
                "Input expects UTF-8 text; charset conversion is not implemented yet for " +
                    (resolved == "stdin" ? std::string("stdin") : "file " + resolved) +
                    " (invalid byte at offset " + std::to_string(invalid_offset) +
                    "). Convert the document to UTF-8 and try again."};
    }
    if (cancellation.cancelled()) {
        return {ErrorCode::Cancelled, "text insertion cancelled: " + resolved};
    }

    TextContext loaded;
    loaded.source = resolved == "stdin" ? "stdin" : "file " + resolved;
    loaded.kind = type.kind;
    loaded.content = type.kind == Kind::Html ? html::convert(body, html::OutputFormat::Markdown) : std::move(body);
    if (cancellation.cancelled()) {
        return {ErrorCode::Cancelled, "text insertion cancelled: " + resolved};
    }
    context = std::move(loaded);
    return ok_error();
}

bool is_http_url(const std::string& source) {
    const std::string lower = ascii_lower(source);
    return lower.rfind("http://", 0) == 0 || lower.rfind("https://", 0) == 0;
}

Error load_insert_source(const std::string& source,
                         const InsertSourceOptions& options,
                         InsertSource& inserted,
                         runtime::CancellationToken cancellation) {
    if (source.empty()) {
        return {ErrorCode::BadArgs, "/insert requires a file path or http(s) URL"};
    }

    InsertSource loaded;
    loaded.url = is_http_url(source);
    loaded.source = loaded.url ? source : expand_user_path(source);
    if (loaded.url) {
        Error err = fetch::fetch_html(source, options.fetch, loaded.content, cancellation);
        if (!err.ok()) {
            return err;
        }
        err = validate_insert_text(loaded.content, "URL " + source);
        if (!err.ok()) {
            return err;
        }
        if (cancellation.cancelled()) {
            return {ErrorCode::Cancelled, "URL insertion cancelled: " + source};
        }
        if (options.auto_convert_html_to_markdown) {
            try {
                loaded.content = html::convert(loaded.content, html::OutputFormat::Markdown);
            } catch (const std::bad_alloc&) {
                return {ErrorCode::Internal,
                        "not enough memory to convert HTML from URL: " + source};
            } catch (const std::length_error&) {
                return {ErrorCode::UnsupportedFeature,
                        "converted HTML is too large to insert from URL: " + source};
            }
            loaded.converted_html = true;
        }
    } else {
        if (source.find("://") != std::string::npos) {
            return {ErrorCode::BadUrl,
                    "/insert URL only supports http:// and https:// sources: " + source};
        }
        Error err = read_insert_file(loaded.source, options.max_file_bytes, loaded.content, cancellation);
        if (!err.ok()) {
            return err;
        }
        err = validate_insert_text(loaded.content, "file " + loaded.source);
        if (!err.ok()) {
            return err;
        }
    }
    Error normalize_error = normalize_insert_linebreaks(loaded.content,
                                                        loaded.url ? "URL " + source
                                                                   : "file " + loaded.source);
    if (!normalize_error.ok()) {
        return normalize_error;
    }
    if (cancellation.cancelled()) {
        return {ErrorCode::Cancelled,
                std::string(loaded.url ? "URL" : "file") + " insertion cancelled: " + loaded.source};
    }
    inserted = std::move(loaded);
    return ok_error();
}

std::string text_context_message(const TextContext& context) {
    std::string message = "Input context from " + context.source + "\nFormat: ";
    if (context.kind == Kind::Markdown || context.kind == Kind::Html) {
        message += "md";
    } else {
        message += "plaintext";
    }
    message += "\n\n";
    message += context.content;
    return message;
}

Error read_local_text_file_for_attach(const std::string& path,
                                      size_t max_bytes,
                                      std::string& content,
                                      runtime::CancellationToken cancellation) {
    const std::string resolved = expand_user_path(path);
    if (resolved == "stdin" || resolved == "-") {
        return {ErrorCode::BadArgs,
                "stdin is only supported by non-interactive --attach or --input"};
    }
    FileType type;
    Error err = classify_file_type(resolved, type);
    if (!err.ok()) {
        return err;
    }
    if (type.kind == Kind::Image) {
        return {ErrorCode::UnsupportedFeature,
                "text attach supports .txt, .md, .html files; images use the pending image queue: " +
                    resolved};
    }

    std::ifstream file(resolved, std::ios::binary);
    if (!file) {
        return {ErrorCode::FileRead, "could not open " + type.name + " for reading: " + resolved};
    }
    std::string body;
    std::array<char, 8192> buffer{};
    while (file) {
        if (cancellation.cancelled()) {
            return {ErrorCode::Cancelled, "attach read cancelled: " + resolved};
        }
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = file.gcount();
        if (count <= 0) {
            break;
        }
        const size_t chunk_size = static_cast<size_t>(count);
        if (body.size() > max_bytes || chunk_size > max_bytes - body.size()) {
            return {ErrorCode::UnsupportedFeature,
                    type.name + " exceeds --max-input-bytes limit of " + std::to_string(max_bytes) +
                        " bytes: " + resolved};
        }
        body.append(buffer.data(), chunk_size);
    }
    if (file.bad()) {
        return {ErrorCode::FileRead, "could not read " + type.name + ": " + resolved};
    }
    const size_t nul = body.find('\0');
    if (nul != std::string::npos) {
        return {ErrorCode::UnsupportedFeature,
                "input appears to be binary: file " + resolved +
                    " contains a NUL byte at offset " + std::to_string(nul)};
    }
    size_t invalid_offset = 0;
    if (!html::is_valid_utf8(body, &invalid_offset)) {
        return {ErrorCode::UnsupportedFeature,
                "Input expects UTF-8 text; charset conversion is not implemented yet for file " +
                    resolved + " (invalid byte at offset " + std::to_string(invalid_offset) +
                    "). Convert the document to UTF-8 and try again."};
    }
    if (cancellation.cancelled()) {
        return {ErrorCode::Cancelled, "attach read cancelled: " + resolved};
    }
    content = std::move(body);
    return ok_error();
}

}  // namespace ainiux::input
