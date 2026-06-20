#include "input/input.hpp"

#include <array>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <string>
#include <utility>

#include "html/html.hpp"

namespace pkchat::input {
namespace {

std::string lower_ascii(std::string text) {
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return text;
}

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
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
    if (path == "stdin") {
        type = {Kind::Plaintext, "plaintext", "text/plain"};
        return ok_error();
    }
    if (path == "-") {
        return {ErrorCode::BadArgs,
                "--input - cannot infer a file type from the path ending; use a path with a supported extension"};
    }
    const std::string lower = lower_ascii(path);
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
            "unsupported input file type for " + path +
                "; supported endings are .txt, .text, .md, .markdown, .html, .htm, .png, .jpg, .jpeg, "
                "and .gif "
                "(case-insensitive)"};
}

Error load_image_file(const std::string& path,
                      const FileType& type,
                      size_t max_bytes,
                      ImageData& image,
                      runtime::CancellationToken cancellation) {
    if (type.kind != Kind::Image) {
        return {ErrorCode::Internal, "load_image_file called for non-image input: " + path};
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {ErrorCode::FileRead, "could not open image for reading: " + path};
    }
    std::string data;
    std::array<char, 8192> buffer{};
    while (file) {
        if (cancellation.cancelled()) {
            return {ErrorCode::Cancelled, "image read cancelled: " + path};
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
                        " bytes: " + path};
        }
        data.append(buffer.data(), chunk_size);
    }
    if (file.bad()) {
        return {ErrorCode::FileRead, "could not read image: " + path};
    }
    if (data.empty()) {
        return {ErrorCode::UnsupportedFeature, "image file is empty: " + path};
    }
    if (!signature_matches(data, type.mime_type)) {
        return {ErrorCode::UnsupportedFeature,
                "image content does not match its " + type.mime_type + " file extension: " + path};
    }
    if (cancellation.cancelled()) {
        return {ErrorCode::Cancelled, "image read cancelled: " + path};
    }

    ImageData loaded;
    loaded.mime_type = type.mime_type;
    loaded.byte_size = data.size();
    loaded.base64_data = base64_encode(data);
    if (cancellation.cancelled()) {
        return {ErrorCode::Cancelled, "image encoding cancelled: " + path};
    }
    image = std::move(loaded);
    return ok_error();
}

Error load_text_context_file(const std::string& path,
                             size_t max_bytes,
                             TextContext& context,
                             runtime::CancellationToken cancellation) {
    FileType type;
    Error err = classify_file_type(path, type);
    if (!err.ok()) {
        return err;
    }
    if (type.kind == Kind::Image) {
        return {ErrorCode::UnsupportedFeature,
                "text insertion supports .txt, .md, and .html files; attach images to a prompt instead: " + path};
    }

    std::ifstream file;
    std::istream* input = &std::cin;
    if (path != "stdin") {
        file.open(path, std::ios::binary);
        if (!file) {
            return {ErrorCode::FileRead, "could not open " + type.name + " for reading: " + path};
        }
        input = &file;
    }
    std::string body;
    std::array<char, 8192> buffer{};
    while (*input) {
        if (cancellation.cancelled()) {
            return {ErrorCode::Cancelled, "text insertion cancelled: " + path};
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
                        " bytes: " + path};
        }
        body.append(buffer.data(), chunk_size);
    }
    if (input->bad()) {
        return {ErrorCode::FileRead,
                path == "stdin" ? "could not read plaintext from stdin"
                                : "could not read " + type.name + ": " + path};
    }
    const size_t nul = body.find('\0');
    if (nul != std::string::npos) {
        return {ErrorCode::UnsupportedFeature,
                "input appears to be binary: " +
                    (path == "stdin" ? std::string("stdin") : "file " + path) +
                    " contains a NUL byte at offset " +
                    std::to_string(nul)};
    }
    size_t invalid_offset = 0;
    if (!html::is_valid_utf8(body, &invalid_offset)) {
        return {ErrorCode::UnsupportedFeature,
                "Input expects UTF-8 text; charset conversion is not implemented yet for " +
                    (path == "stdin" ? std::string("stdin") : "file " + path) +
                    " (invalid byte at offset " + std::to_string(invalid_offset) +
                    "). Convert the document to UTF-8 and try again."};
    }
    if (cancellation.cancelled()) {
        return {ErrorCode::Cancelled, "text insertion cancelled: " + path};
    }

    TextContext loaded;
    loaded.source = path == "stdin" ? "stdin" : "file " + path;
    loaded.kind = type.kind;
    loaded.content = type.kind == Kind::Html ? html::convert(body, html::OutputFormat::Markdown) : std::move(body);
    if (cancellation.cancelled()) {
        return {ErrorCode::Cancelled, "text insertion cancelled: " + path};
    }
    context = std::move(loaded);
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

}  // namespace pkchat::input
