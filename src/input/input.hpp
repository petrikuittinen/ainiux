#pragma once

#include <cstddef>
#include <string>

#include "common.hpp"
#include "fetch/fetch.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::input {

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

struct InsertSourceOptions {
    size_t max_file_bytes = 1048576;
    fetch::Options fetch;
    bool auto_convert_html_to_markdown = true;
};

struct InsertSource {
    std::string source;
    std::string content;
    bool url = false;
    bool converted_html = false;
};

Error classify_file_type(const std::string& path, FileType& type);
// True when the path ending is a supported vision image type (.png/.jpg/.jpeg/.gif).
bool path_has_supported_image_extension(const std::string& path);
// Paths mentioned in free text that look like local image files (quoted or bare).
// Does not open files or resolve existence; caller validates with classify/load.
std::vector<std::string> extract_local_image_path_candidates(const std::string& text);
Error load_image_file(const std::string& path,
                      const FileType& type,
                      size_t max_bytes,
                      ImageData& image,
                      runtime::CancellationToken cancellation = runtime::CancellationToken());
Error load_image_file_bytes(const std::string& path,
                            const FileType& type,
                            size_t max_bytes,
                            std::string& bytes,
                            runtime::CancellationToken cancellation = runtime::CancellationToken());
Error load_text_context_file(const std::string& path,
                             size_t max_bytes,
                             TextContext& context,
                             runtime::CancellationToken cancellation = runtime::CancellationToken());
bool is_http_url(const std::string& source);
Error load_insert_source(const std::string& source,
                         const InsertSourceOptions& options,
                         InsertSource& inserted,
                         runtime::CancellationToken cancellation = runtime::CancellationToken());
std::string text_context_message(const TextContext& context);

// Raw loader for chat /attach of local files: reads UTF-8 text up to limit,
// validates no NUL and UTF-8, but does NOT perform HTML conversion.
// Conversion is applied by caller when appropriate.
Error read_local_text_file_for_attach(const std::string& path,
                                      size_t max_bytes,
                                      std::string& content,
                                      runtime::CancellationToken cancellation = runtime::CancellationToken());

}  // namespace ainiux::input
