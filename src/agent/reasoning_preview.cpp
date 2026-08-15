#include "agent/reasoning_preview.hpp"

#include <cctype>
#include <vector>

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

// Last `count` graphemes of text (or all of text when shorter).
std::string grapheme_suffix(const std::string& text, std::size_t count) {
    if (count == 0 || text.empty()) return {};
    std::vector<std::size_t> starts;
    starts.reserve(text.size());
    for (std::size_t position = 0; position < text.size();) {
        starts.push_back(position);
        position = editor::detail::next_grapheme_offset(text, position);
    }
    if (starts.size() <= count) return text;
    return text.substr(starts[starts.size() - count]);
}

// Short closers like "Good." / "OK." that should pull preceding context.
constexpr std::size_t kShortStickyGraphemes = 24;
constexpr std::size_t kShortStickyTokens = 3;

std::size_t count_whitespace_tokens(const std::string& text) {
    std::size_t tokens = 0;
    bool in_token = false;
    for (unsigned char ch : text) {
        if (std::isspace(ch) != 0) {
            in_token = false;
            continue;
        }
        if (!in_token) {
            ++tokens;
            in_token = true;
        }
    }
    return tokens;
}

bool is_short_sticky_text(const std::string& text) {
    if (text.empty()) return true;
    if (grapheme_count(text) < kShortStickyGraphemes) return true;
    return count_whitespace_tokens(text) < kShortStickyTokens;
}

// If suffix begins mid-word, drop the leading partial word so the sticky row
// starts on a cleaner boundary.
std::string trim_leading_partial_word(std::string text) {
    if (text.empty()) return text;
    const unsigned char first = static_cast<unsigned char>(text.front());
    if (!is_word_char(first)) return text;
    std::size_t i = 0;
    while (i < text.size() && is_word_char(static_cast<unsigned char>(text[i])))
        ++i;
    while (i < text.size() &&
           std::isspace(static_cast<unsigned char>(text[i])) != 0)
        ++i;
    if (i == 0 || i >= text.size()) return text;
    return text.substr(i);
}

// Prefer starting just after a sentence end inside the reverse window when the
// remainder still carries useful content; otherwise keep the window as-is.
// Leading partial-word trimming is handled by the caller only when the reverse
// window actually cut mid-token.
std::string refine_backtrack_window(const std::string& window) {
    if (window.empty()) return window;
    // Look for a sentence end in the first half so the row still ends on the
    // short closer but opens on a thought boundary when possible.
    const std::size_t limit = window.size() / 2;
    std::size_t best = std::string::npos;
    for (std::size_t pos = 0; pos < limit; ++pos) {
        if (!is_sentence_end(window, pos)) continue;
        const std::size_t after =
            skip_spaces(window, pos + sentence_end_byte_length(window, pos));
        if (after > 0 && after < window.size() &&
            grapheme_count(window.substr(after)) >= kShortStickyGraphemes)
            best = after;
    }
    if (best != std::string::npos) return window.substr(best);
    return window;
}

std::string backtrack_sticky_window(const std::string& normalized,
                                    std::size_t sticky_end,
                                    std::size_t content_graphemes) {
    if (sticky_end == 0 || content_graphemes == 0) return {};
    const std::string prefix = normalized.substr(0, sticky_end);
    std::string window = grapheme_suffix(prefix, content_graphemes);
    if (window.empty()) return {};
    // Only drop a leading partial word when the reverse window cut mid-token
    // (previous byte was also a word char). Whole tokens like "Good." stay.
    const std::size_t window_start = sticky_end - window.size();
    if (window_start > 0) {
        const unsigned char at =
            static_cast<unsigned char>(normalized[window_start]);
        const unsigned char before =
            static_cast<unsigned char>(normalized[window_start - 1]);
        if (is_word_char(at) && is_word_char(before))
            window = trim_leading_partial_word(window);
    }
    return refine_backtrack_window(window);
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

std::string flatten_reasoning_preview_text(
    const std::string& reasoning, const std::vector<std::string>& secrets) {
    std::string text = normalize_reasoning_preview_text(reasoning, secrets);
    for (char& ch : text) {
        if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
    }
    return text;
}

std::string clip_preview_prefix(const std::string& text, std::size_t max_chars) {
    if (max_chars == 0 || text.empty()) return {};
    if (grapheme_count(text) <= max_chars) return text;
    std::string prefix = grapheme_prefix(text, max_chars);
    if (prefix.empty()) return {};
    const std::size_t cut = prefix.size();
    if (cut < text.size() && is_word_char(static_cast<unsigned char>(text[cut])) &&
        is_word_char(static_cast<unsigned char>(prefix.back()))) {
        std::size_t end = prefix.size();
        while (end > 0 && is_word_char(static_cast<unsigned char>(prefix[end - 1])))
            --end;
        while (end > 0 &&
               std::isspace(static_cast<unsigned char>(prefix[end - 1])) != 0)
            --end;
        if (end > 0) prefix.resize(end);
    }
    return prefix;
}

std::string clip_preview_suffix(const std::string& text, std::size_t max_chars) {
    if (max_chars == 0 || text.empty()) return {};
    if (grapheme_count(text) <= max_chars) return text;
    std::string suffix = grapheme_suffix(text, max_chars);
    if (suffix.empty()) return {};
    const std::size_t start = text.size() - suffix.size();
    if (start > 0 && is_word_char(static_cast<unsigned char>(suffix.front())) &&
        is_word_char(static_cast<unsigned char>(text[start - 1]))) {
        const std::string trimmed = trim_leading_partial_word(suffix);
        if (!trimmed.empty()) suffix = trimmed;
    }
    return suffix;
}

std::string last_reasoning_sentence(const std::string& flattened) {
    return reasoning_active_slice(flattened, 0);
}

std::string thinking_opening_body(const std::string& reasoning,
                                  std::size_t max_chars,
                                  const std::vector<std::string>& secrets) {
    if (max_chars == 0) return {};
    return clip_preview_prefix(flatten_reasoning_preview_text(reasoning, secrets),
                               max_chars);
}

std::string finished_thinking_body(const std::string& reasoning,
                                   std::size_t max_chars,
                                   const std::vector<std::string>& secrets) {
    if (max_chars == 0) return {};
    const std::string flattened = flatten_reasoning_preview_text(reasoning, secrets);
    return clip_preview_suffix(last_reasoning_sentence(flattened), max_chars);
}

bool opening_preview_has_more(const std::string& reasoning,
                              std::size_t max_chars,
                              const std::vector<std::string>& secrets) {
    const std::string opening =
        thinking_opening_body(reasoning, max_chars, secrets);
    if (opening.empty()) return false;
    const std::string flattened = flatten_reasoning_preview_text(reasoning, secrets);
    const std::size_t after = skip_spaces(flattened, opening.size());
    return after < flattened.size();
}

bool skip_finished_thinking_preview(const std::string& reasoning,
                                    std::size_t max_chars,
                                    const std::vector<std::string>& secrets) {
    const std::string opening =
        thinking_opening_body(reasoning, max_chars, secrets);
    const std::string finished =
        finished_thinking_body(reasoning, max_chars, secrets);
    if (opening.empty() || finished.empty() || opening == finished) return true;
    const std::string flattened = flatten_reasoning_preview_text(reasoning, secrets);
    const std::string last = last_reasoning_sentence(flattened);
    const std::size_t start = skip_spaces(flattened, 0);
    // Last unit is still the first sentence / whole monologue — no later sentence.
    if (start < flattened.size() && start + last.size() <= flattened.size() &&
        flattened.compare(start, last.size(), last) == 0)
        return true;
    return false;
}

std::string format_thinking_opening_preview(const std::string& reasoning,
                                            std::size_t max_chars,
                                            const std::vector<std::string>& secrets) {
    const std::string body = thinking_opening_body(reasoning, max_chars, secrets);
    if (body.empty()) return {};
    return "Thinking: " + body;
}

std::string format_live_thinking_tail(const std::string& reasoning,
                                      std::size_t max_chars,
                                      const std::vector<std::string>& secrets) {
    return finished_thinking_body(reasoning, max_chars, secrets);
}

std::string format_finished_thinking_preview(
    const std::string& reasoning, std::size_t max_chars,
    const std::vector<std::string>& secrets) {
    const std::string body = finished_thinking_body(reasoning, max_chars, secrets);
    if (body.empty()) return {};
    return "Finished thinking: " + body;
}

std::string format_reasoning_preview(const std::string& reasoning,
                                     std::size_t max_chars,
                                     const std::vector<std::string>& secrets) {
    return format_thinking_opening_preview(reasoning, max_chars, secrets);
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

ReasoningStickySlice reasoning_sticky_slice(const std::string& normalized,
                                            std::size_t start_offset,
                                            std::size_t content_graphemes) {
    ReasoningStickySlice out;
    if (content_graphemes == 0) {
        out.next_offset = start_offset;
        return out;
    }
    const std::size_t start = skip_spaces(normalized, start_offset);
    if (start >= normalized.size()) {
        out.next_offset = normalized.size();
        return out;
    }

    std::size_t packed_end = start;
    std::size_t advance_to = start;
    std::size_t cursor = start;
    while (cursor < normalized.size()) {
        const ReasoningIdleSlice slice =
            take_reasoning_idle_slice(normalized, cursor, 0, false);
        if (slice.text.empty()) {
            if (packed_end == start) {
                // No complete sentence yet: clip the monologue head.
                const ReasoningIdleSlice forced = take_reasoning_idle_slice(
                    normalized, start, content_graphemes, true);
                out.text = forced.text;
                out.next_offset = forced.next_offset;
                const std::size_t sticky_end = start + out.text.size();
                if (is_short_sticky_text(out.text) && start > 0) {
                    const std::string expanded = backtrack_sticky_window(
                        normalized, sticky_end, content_graphemes);
                    if (!expanded.empty()) out.text = expanded;
                }
                return out;
            }
            // Prefer complete sentences already packed; do not append a short
            // incomplete tail that would reintroduce closers like "Good".
            break;
        }

        const std::size_t sent_start = skip_spaces(normalized, cursor);
        const std::size_t sent_end = sent_start + slice.text.size();
        const std::string trial = normalized.substr(start, sent_end - start);
        if (grapheme_count(trial) > content_graphemes) {
            if (packed_end == start) {
                // First sentence alone exceeds the budget: show a prefix, but
                // advance past the full sentence so idle rows still progress.
                out.text = grapheme_prefix(slice.text, content_graphemes);
                out.next_offset = slice.next_offset;
                return out;
            }
            break;
        }
        packed_end = sent_end;
        advance_to = slice.next_offset;
        cursor = slice.next_offset;
    }

    if (packed_end > start)
        out.text = normalized.substr(start, packed_end - start);
    out.next_offset = advance_to > start ? advance_to : start;

    if (is_short_sticky_text(out.text) && packed_end > 0) {
        const std::string expanded =
            backtrack_sticky_window(normalized, packed_end, content_graphemes);
        if (!expanded.empty()) out.text = expanded;
        // Forward progress still consumes only the short unit from start.
        if (out.next_offset <= start) out.next_offset = packed_end;
    }
    return out;
}

}  // namespace ainiux::agent

