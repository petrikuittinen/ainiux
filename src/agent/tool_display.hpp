#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace ainiux::agent {

// Compact one-line tool activity for TUI transcript and --run stderr:
//   1: read_file("example.txt") → ok
struct CompactToolLine {
    std::size_t index = 0;
    std::string text;  // full formatted line without trailing newline
};

// Best-effort terminal width (ioctl / COLUMNS), never below 20.
std::size_t terminal_column_count(std::size_t fallback = 80);

// Clip text to at most max_cells code units (ASCII-heavy agent logs).
// Appends "..." when truncated. Empty max_cells yields empty string.
std::string clip_to_cells(const std::string& text, std::size_t max_cells);

// Tool / intermediate timing: "2270 ms" (whole milliseconds, clamps negative to 0).
std::string format_elapsed_ms(long long elapsed_ms);

// Final turn completion: "Task complete in 21.34 seconds." (2 decimal places).
std::string format_task_complete(long long elapsed_ms);

// Legacy alias for callers that still want a seconds-style suffix; prefer the
// two functions above. Formats as "6.54 seconds elapsed".
std::string format_elapsed_seconds(long long elapsed_ms);

// Normalize stored timestamps: values below 1e12 are treated as Unix seconds.
long long normalize_timestamp_ms(long long stored_created_at);

// Current wall-clock time in Unix milliseconds.
long long now_unix_ms();

// Build a truncated preview of JSON tool arguments (paths preferred).
std::string compact_tool_args_preview(const std::string& arguments_json,
                                      std::size_t max_cells = 72);

// Map tool result JSON body to "ok" / "error".
std::string compact_tool_status(const std::string& result_json);

// Short human reason for a failed tool result (empty when ok / missing).
// Examples: "outside project", "shell not allowed", "policy denied".
std::string compact_tool_error_brief(const std::string& result_json,
                                     std::size_t max_cells = 56);

// Format: N: name(args_preview) → ok|error: brief
// When max_line_cells is 0, uses terminal_column_count(). The final line is
// always clipped to that width so agent logs stay on one screen row.
std::string format_compact_tool_line(std::size_t index,
                                     const std::string& tool_name,
                                     const std::string& arguments_json,
                                     const std::string& result_json,
                                     std::size_t max_line_cells = 0);

}  // namespace ainiux::agent
