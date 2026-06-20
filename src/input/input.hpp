#pragma once

#include <cstddef>
#include <string>

#include "common.hpp"
#include "runtime/runtime.hpp"

namespace pkchat::input {

enum class Kind {
    Plaintext,
    Markdown,
    Html,
    Image,
};

struct FileType {
    Kind kind = Kind::Plaintext;
    std::string name = "plaintext";
    std::string mime_type = "text/plain";
};

struct ImageData {
    std::string mime_type;
    std::string base64_data;
    size_t byte_size = 0;
};

struct TextContext {
    std::string source;
    Kind kind = Kind::Plaintext;
    std::string content;
};

Error classify_file_type(const std::string& path, FileType& type);
Error load_image_file(const std::string& path,
                      const FileType& type,
                      size_t max_bytes,
                      ImageData& image,
                      runtime::CancellationToken cancellation = runtime::CancellationToken());
Error load_text_context_file(const std::string& path,
                             size_t max_bytes,
                             TextContext& context,
                             runtime::CancellationToken cancellation = runtime::CancellationToken());
std::string text_context_message(const TextContext& context);

}  // namespace pkchat::input
