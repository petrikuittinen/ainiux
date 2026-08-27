#pragma once

#include <cstddef>
#include <string>

namespace ainiux::editor {

struct EditorBufferStats {
    size_t characters = 0;
    size_t words = 0;
    size_t total_lines = 0;
    size_t empty_lines = 0;
};

// Character count is Unicode graphemes excluding CR and LF. Word count is
// whitespace-separated tokens. Total lines match the piece-table line count
// (empty text is one empty line; a trailing newline adds a final empty line).
// Empty lines are empty or ASCII-whitespace-only, matching /remove-blank-lines.
EditorBufferStats buffer_statistics(const std::string& text);
std::string format_statistics_message(const EditorBufferStats& stats, bool selection);

}  // namespace ainiux::editor
