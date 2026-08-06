#include "agent/reasoning_preview.hpp"

#include <cctype>

#include "editor/detail/unicode.hpp"
#include "security/redact.hpp"

namespace ainiux::agent {
namespace {

bool is_word_char(unsigned char ch) {
    return (std::isalnum(ch) != 0) || ch == '_';
}

// Mid-token punctuation: safe to glue on both sides (HTTP/1.1, end-to-end).
bool is_mid_token_punct(unsigned char ch) {
    switch (ch) {
        case '/':
        case '-':
        case '_':
        case '+':
        case '*':
        case '=':
        case '(':
        case '[':
        case '{':
            return true;
        default:
            return false;
    }
}

// Closing / trailing punctuation: glue only to the preceding token
// ("server .c", "foo ,bar") — never to the following word ("done. The").
bool is_trailing_attach_punct(unsigned char ch) {
    switch (ch) {
        case '.':
        case ',':
        case ';':
        case ':':
        case ')':
        case ']':
        case '}':
        case '\'':
        case '"':
            return true;
        default:
            return false;
    }
}

// Measure the trailing word-run length in `out` (alnum/_ only).
std::size_t trailing_word_run(const std::string& out) {
    std::size_t run = 0;
    for (std::size_t i = out.size(); i > 0; --i) {
        if (!is_word_char(static_cast<unsigned char>(out[i - 1]))) break;
        ++run;
    }
    return run;
}

// Measure the leading word-run length starting at `pos` in `text`.
std::size_t leading_word_run(const std::string& text, std::size_t pos) {
    std::size_t run = 0;
    while (pos + run < text.size() &&
           is_word_char(static_cast<unsigned char>(text[pos + run])))
        ++run;
    return run;
}

// Normalize reasoning for a single history line.
//
// 1) Collapse all whitespace runs to a single space (default).
// 2) Drop the space only for safe token glue:
//    - mid-token punctuation: "HTTP / 1 . 1", "End -to -end", "parse _http"
//    - trailing punct after a word: "server .c"
//    - newline after a single-letter fragment: "h\\nunk" → "hunk"
//
// Do NOT glue multi-letter English words ("user is", "Let me", "look at").
// That was too aggressive for Kimi and produced "useris" / "Letme lookat".
std::string normalize_reasoning_whitespace(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        if (std::isspace(ch) == 0) {
            out.push_back(static_cast<char>(ch));
            ++i;
            continue;
        }

        bool saw_newline = false;
        while (i < text.size() &&
               std::isspace(static_cast<unsigned char>(text[i])) != 0) {
            const unsigned char ws = static_cast<unsigned char>(text[i]);
            if (ws == '\n' || ws == '\r') saw_newline = true;
            ++i;
        }
        if (out.empty() || i >= text.size()) continue;

        const unsigned char prev = static_cast<unsigned char>(out.back());
        const unsigned char next = static_cast<unsigned char>(text[i]);
        bool join = false;
        if (is_word_char(prev) && is_trailing_attach_punct(next)) {
            // "server .c"
            join = true;
        } else if (is_word_char(prev) && is_mid_token_punct(next)) {
            // "HTTP /", "end -"
            join = true;
        } else if (is_mid_token_punct(prev) && is_word_char(next)) {
            // "/ 1", "-to", "_http"
            join = true;
        } else if (prev == '.' && std::isdigit(next) != 0 && out.size() >= 2 &&
                   std::isdigit(static_cast<unsigned char>(out[out.size() - 2])) != 0) {
            // "1 . 1" inside versions (HTTP/1.1) without gluing "done. The"
            join = true;
        } else if ((is_mid_token_punct(prev) || is_trailing_attach_punct(prev)) &&
                   (is_mid_token_punct(next) || is_trailing_attach_punct(next))) {
            // "/ .", ".-"
            join = true;
        } else if (saw_newline && is_word_char(prev) && is_word_char(next)) {
            // Only glue a single-character fragment across a line break
            // ("h\\nunk"). Multi-letter words keep a space ("user\\nis").
            const std::size_t before = trailing_word_run(out);
            const std::size_t after = leading_word_run(text, i);
            join = before == 1 || after == 1;
        }
        if (join) continue;
        out.push_back(' ');
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
