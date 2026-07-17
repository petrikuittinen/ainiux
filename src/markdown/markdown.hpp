#pragma once

#include <string>

namespace ainiux::markdown {

enum class OutputFormat {
    Plaintext,
    Markdown,
    Html,
};

bool parse_output_format(const std::string& text, OutputFormat& out);
const char* output_format_name(OutputFormat format);

std::string render(const std::string& markdown, OutputFormat format, bool complete_html_document);
std::string to_html_fragment(const std::string& markdown);
std::string to_html_document(const std::string& markdown);
std::string to_plaintext(const std::string& markdown);

}  // namespace ainiux::markdown
