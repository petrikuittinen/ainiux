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

// True when ch at pos ends a sentence and is not mid-token (HTTP/1.1, server.c).
bool is_sentence_end(const std::string& text, std::size_t pos) {
    if (pos >= text.size()) return false;
    const unsigned char ch = static_cast<unsigned char>(text[pos]);
    if (ch == 0xE3 && pos + 2 < text.size() &&
        static_cast<unsigned char>(text[pos + 1]) == 0x80 &&
        static_cast<unsigned char>(text[pos + 2]) == 0x82) {
        // UTF-8 ideographic full stop U+3002 (。).
        return true;
    }
    if (ch != '.' && ch != '!' && ch != '?') return false;
    if (ch == '.') {
        const bool prev_digit =
            pos > 0 && std::isdigit(static_cast<unsigned char>(text[pos - 1])) != 0;
        const bool next_digit =
            pos + 1 < text.size() &&
            std::isdigit(static_cast<unsigned char>(text[pos + 1])) != 0;
        if (prev_digit && next_digit) return false;
        // "server.c" / "file.h" — letter, '.', letter-or-digit continuation.
        if (pos > 0 && pos + 1 < text.size()) {
            const unsigned char prev = static_cast<unsigned char>(text[pos - 1]);
            const unsigned char next = static_cast<unsigned char>(text[pos + 1]);
            if (std::isalnum(prev) != 0 && std::isalnum(next) != 0 &&
                (std::islower(next) != 0 || std::isdigit(next) != 0))
                return false;
        }
    }
    return true;
}

std::size_t sentence_end_byte_length(const std::string& text, std::size_t pos) {
    if (pos >= text.size()) return 0;
    const unsigned char ch = static_cast<unsigned char>(text[pos]);
    if (ch == 0xE3 && pos + 2 < text.size() &&
        static_cast<unsigned char>(text[pos + 1]) == 0x80 &&
        static_cast<unsigned char>(text[pos + 2]) == 0x82)
        return 3;
    return 1;
}

// Skip ASCII whitespace starting at offset.
std::size_t skip_spaces(const std::string& text, std::size_t offset) {
    while (offset < text.size() &&
           std::isspace(static_cast<unsigned char>(text[offset])) != 0)
        ++offset;
    return offset;
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
//
// Intentionally unused by the preview path (see normalize_reasoning_preview_text):
// single-letter newline gluing corrupted other models ("I\\nneed" → "Ineed").
[[maybe_unused]] std::string normalize_reasoning_whitespace(const std::string& text) {
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

std::string normalize_reasoning_preview_text(
    const std::string& reasoning, const std::vector<std::string>& secrets) {
    // Keep provider spacing intact. The Kimi-oriented
    // normalize_reasoning_whitespace() gluer is retained but intentionally
    // unused: it joins single-letter fragments and mid-token punctuation in
    // ways that corrupt readable previews on other models (e.g. "I need" →
    // "Ineed", "main.c fully" → "main.cfully").
    return redact_secrets(reasoning, secrets);
}

std::string format_reasoning_preview(const std::string& reasoning,
                                     std::size_t max_chars,
                                     const std::vector<std::string>& secrets) {
    if (max_chars == 0) return {};
    const std::string prefix = "Thinking: ";
    if (max_chars <= prefix.size()) return prefix.substr(0, max_chars);
    std::string text = normalize_reasoning_preview_text(reasoning, secrets);
    if (text.empty()) return {};
    // Single history row: map vertical whitespace to a plain space without
    // otherwise altering token spacing.
    for (char& ch : text) {
        if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
    }
    const std::size_t available = max_chars - prefix.size();
    if (grapheme_count(text) <= available) return prefix + text;
    if (available == 1) return prefix + u8"…";
    return prefix + grapheme_prefix(text, available - 1) + u8"…";
}

ReasoningIdleSlice take_reasoning_idle_slice(const std::string& normalized,
                                             std::size_t start_offset,
                                             std::size_t content_graphemes,
                                             bool force_partial) {
    ReasoningIdleSlice slice;
    const std::size_t start = skip_spaces(normalized, start_offset);
    if (start >= normalized.size()) {
        slice.next_offset = normalized.size();
        return slice;
    }

    std::size_t pos = start;
    while (pos < normalized.size()) {
        if (is_sentence_end(normalized, pos)) {
            pos += sentence_end_byte_length(normalized, pos);
            slice.text = normalized.substr(start, pos - start);
            slice.next_offset = skip_spaces(normalized, pos);
            return slice;
        }
        ++pos;
    }

    if (!force_partial) {
        slice.next_offset = start_offset;
        return slice;
    }

    // No sentence boundary yet: emit a clipped partial so long monologues still
    // advance. content_graphemes 0 means take the remainder.
    if (content_graphemes == 0) {
        slice.text = normalized.substr(start);
        slice.next_offset = normalized.size();
        return slice;
    }
    const std::string remainder = normalized.substr(start);
    if (grapheme_count(remainder) <= content_graphemes) {
        slice.text = remainder;
        slice.next_offset = normalized.size();
        return slice;
    }
    slice.text = grapheme_prefix(remainder, content_graphemes);
    slice.next_offset = start + slice.text.size();
    return slice;
}

std::string reasoning_active_slice(const std::string& normalized,
                                   std::size_t start_offset) {
    std::size_t offset = skip_spaces(normalized, start_offset);
    if (offset >= normalized.size()) return {};
    std::string last_complete;
    while (offset < normalized.size()) {
        const ReasoningIdleSlice slice =
            take_reasoning_idle_slice(normalized, offset, 0, false);
        if (slice.text.empty()) {
            // Incomplete monologue after the last complete sentence.
            const std::size_t start = skip_spaces(normalized, offset);
            if (start >= normalized.size()) return last_complete;
            return normalized.substr(start);
        }
        last_complete = slice.text;
        offset = slice.next_offset;
        if (offset >= normalized.size()) return last_complete;
    }
    return last_complete;
}

}  // namespace ainiux::agent

