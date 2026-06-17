#pragma once

#include <cstddef>
#include <string>

namespace pkchat::html {

enum class OutputFormat {
    Text,
    Markdown,
};

std::string convert(const std::string& input, OutputFormat format);
bool is_valid_utf8(const std::string& input, size_t* error_offset = nullptr);
bool parse_output_format(const std::string& text, OutputFormat& out);
const char* output_format_name(OutputFormat format);

}  // namespace pkchat::html
