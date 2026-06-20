#pragma once

#include <cstddef>
#include <string>

#include "common.hpp"

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

Error classify_file_type(const std::string& path, FileType& type);
Error load_image_file(const std::string& path,
                      const FileType& type,
                      size_t max_bytes,
                      ImageData& image);

}  // namespace pkchat::input
