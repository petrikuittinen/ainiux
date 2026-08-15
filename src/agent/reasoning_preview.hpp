#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace ainiux::agent {

// Collapse provider reasoning to a single display row, redact configured
// secrets, and clip the body by grapheme/word boundary. The returned string
// includes the "Thinking: " prefix. max_chars is the body budget (label extra).
std::string format_reasoning_preview(const std::string& reasoning,
                                     std::size_t max_chars,
                                     const std::vector<std::string>& secrets);

// Opening live/frozen row: "Thinking: " + first ~max_chars at a word boundary.
std::string format_thinking_opening_preview(const std::string& reasoning,
                                            std::size_t max_chars,
                                            const std::vector<std::string>& secrets);

// Live tail while reasoning is still streaming: last sentence / newest
// fragment, clipped, with no label.
std::string format_live_thinking_tail(const std::string& reasoning,
                                      std::size_t max_chars,
                                      const std::vector<std::string>& secrets);

// Frozen closing row after </think> / end of reasoning:
// "Finished thinking: " + last sentence, clipped to ~max_chars from the end
// at a word boundary.
std::string format_finished_thinking_preview(const std::string& reasoning,
                                             std::size_t max_chars,
                                             const std::vector<std::string>& secrets);

// Bodies only (no label). Empty when max_chars is 0 or the source is empty.
std::string thinking_opening_body(const std::string& reasoning,
                                  std::size_t max_chars,
                                  const std::vector<std::string>& secrets);
std::string finished_thinking_body(const std::string& reasoning,
                                   std::size_t max_chars,
                                   const std::vector<std::string>& secrets);

// True when the finished row should be omitted: empty side, same body, or the
// last sentence is still the first sentence of the trace (no later sentence).
bool skip_finished_thinking_preview(const std::string& reasoning,
                                    std::size_t max_chars,
                                    const std::vector<std::string>& secrets);

// True when flattened reasoning continues past the opening word-boundary clip.
bool opening_preview_has_more(const std::string& reasoning,
                              std::size_t max_chars,
                              const std::vector<std::string>& secrets);

// Redact secrets for sentence walking / idle previews. Does not glue tokens.
std::string normalize_reasoning_preview_text(
    const std::string& reasoning, const std::vector<std::string>& secrets);

// Flatten vertical whitespace after redaction (single history line).
std::string flatten_reasoning_preview_text(
    const std::string& reasoning, const std::vector<std::string>& secrets);

// Last complete sentence, else the newest incomplete tail.
std::string last_reasoning_sentence(const std::string& flattened);

// Word-boundary clips. If there is no whitespace (CJK / one token), keep the
// grapheme window as-is.
std::string clip_preview_prefix(const std::string& text, std::size_t max_chars);
std::string clip_preview_suffix(const std::string& text, std::size_t max_chars);

// One idle-thinking slice taken from already-normalized reasoning text.
struct ReasoningIdleSlice {
    std::string text;
    std::size_t next_offset = 0;
};

// Advance one preview unit from start_offset.
// Prefer a complete sentence (.!? / 。). When force_partial is true and no
// sentence end is available, take up to content_graphemes of remaining text so
// long monologues still advance. Empty text means nothing new to show.
ReasoningIdleSlice take_reasoning_idle_slice(const std::string& normalized,
                                             std::size_t start_offset,
                                             std::size_t content_graphemes,
                                             bool force_partial);

// Live in-progress slice from start_offset: the newest incomplete fragment when
// one exists, otherwise the latest complete sentence still uncommitted.
std::string reasoning_active_slice(const std::string& normalized,
                                   std::size_t start_offset);

// Commit-time (sticky) preview for a thinking history row.
// Prefers the first thought(s) of the uncommitted range [start_offset, end),
// packing complete sentences up to content_graphemes. When that result is a
// short closer (e.g. "Good."), expands backward so the window still ends on
// the closer and fills the same budget. next_offset advances only forward
// through uncommitted text (not past backtracked context).
struct ReasoningStickySlice {
    std::string text;
    std::size_t next_offset = 0;
};

ReasoningStickySlice reasoning_sticky_slice(const std::string& normalized,
                                            std::size_t start_offset,
                                            std::size_t content_graphemes);

}  // namespace ainiux::agent
