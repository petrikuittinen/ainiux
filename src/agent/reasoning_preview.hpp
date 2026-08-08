#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace ainiux::agent {

// Collapse provider reasoning to a single display row, redact configured
// secrets, and clip by grapheme count. The returned string includes the
// "Thinking: " prefix and any ellipsis inside max_chars.
std::string format_reasoning_preview(const std::string& reasoning,
                                     std::size_t max_chars,
                                     const std::vector<std::string>& secrets);

// Redact secrets for sentence walking / idle previews. Does not glue tokens.
std::string normalize_reasoning_preview_text(
    const std::string& reasoning, const std::vector<std::string>& secrets);

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

}  // namespace ainiux::agent
