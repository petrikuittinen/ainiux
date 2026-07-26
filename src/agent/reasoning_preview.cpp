#include "agent/reasoning_preview.hpp"

#include <cctype>

#include "editor/detail/unicode.hpp"
#include "security/redact.hpp"

namespace ainiux::agent {
namespace {

std::string normalize_reasoning_whitespace(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool pending_space = false;
    for (unsigned char ch : text) {
        if (std::isspace(ch) != 0) {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) out.push_back(' ');
        pending_space = false;
        out.push_back(static_cast<char>(ch));
    }
    return out;
}

std::size_t grapheme_count(const std::string& text) {
    std::size_t count = 0;
    for (std::size_t position = 0; position < text.size(); ++count)
        position = editor::detail::next_grapheme_offset(text, position);
    return count;
}

std::string grapheme_prefix(const std::string& text, std::size_t count) {
    std::size_t position = 0;
    for (std::size_t seen = 0; seen < count && position < text.size(); ++seen)
        position = editor::detail::next_grapheme_offset(text, position);
    return text.substr(0, position);
}

}  // namespace

std::string format_reasoning_preview(const std::string& reasoning,
                                     std::size_t max_chars,
                                     const std::vector<std::string>& secrets) {
    if (max_chars == 0) return {};
    const std::string prefix = "Thinking: ";
    if (max_chars <= prefix.size()) return prefix.substr(0, max_chars);
    std::string text = normalize_reasoning_whitespace(redact_secrets(reasoning, secrets));
    if (text.empty()) return {};
    const std::size_t available = max_chars - prefix.size();
    if (grapheme_count(text) <= available) return prefix + text;
    if (available == 1) return prefix + u8"…";
    return prefix + grapheme_prefix(text, available - 1) + u8"…";
}

}  // namespace ainiux::agent
