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

// Build a truncated preview of JSON tool arguments (paths preferred).
std::string compact_tool_args_preview(const std::string& arguments_json,
                                      std::size_t max_cells = 72);

// Map tool result JSON body to "ok" / "error".
std::string compact_tool_status(const std::string& result_json);

// Format: N: name(args_preview) → ok|error
std::string format_compact_tool_line(std::size_t index,
                                     const std::string& tool_name,
                                     const std::string& arguments_json,
                                     const std::string& result_json,
                                     std::size_t max_arg_cells = 72);

}  // namespace ainiux::agent
