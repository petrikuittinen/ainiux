#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace ainiux::editor {

// Default max lines per side for full LCS. Larger inputs fall back to a coarse mark.
constexpr size_t kDefaultLineDiffMaxLines = 4000;

// Split text into logical lines without terminators (matches PieceTable line_text after LF
// normalization: trailing '\n' yields a final empty line).
std::vector<std::string> split_logical_lines(const std::string& text);

// Normalize CR / CRLF to LF so history backups compare cleanly to editor buffers.
std::string normalize_newlines_for_diff(const std::string& text);

// Mark each line of `current` that is not an LCS equal-partner of `previous`.
// Output size equals the line count of current. true = changed or added.
std::vector<bool> mark_changed_lines(const std::string& previous,
                                     const std::string& current,
                                     size_t max_lines = kDefaultLineDiffMaxLines);

// Count of true entries.
size_t count_changed_lines(const std::vector<bool>& marks);

}  // namespace ainiux::editor
