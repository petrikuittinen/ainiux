#pragma once

#include <string>

namespace pkchat::html {

enum class OutputFormat {
    Text,
    Markdown,
};

std::string convert(const std::string& input, OutputFormat format);
bool parse_output_format(const std::string& text, OutputFormat& out);
const char* output_format_name(OutputFormat format);

}  // namespace pkchat::html
