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

}  // namespace ainiux::agent
