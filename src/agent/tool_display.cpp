#include "agent/tool_display.hpp"

#include <cctype>
#include <cstdlib>
#include <sstream>
#include <sys/ioctl.h>
#include <unistd.h>

#include "json/json.hpp"

namespace ainiux::agent {
namespace {

std::string truncate_cells(std::string text, std::size_t max_cells) {
    if (max_cells == 0) return {};
    // Approximate cells as UTF-8 bytes for now (agent lines stay ASCII-heavy).
    if (text.size() <= max_cells) return text;
    if (max_cells <= 3) return std::string(max_cells, '.');
    text.resize(max_cells - 3);
    text += "...";
    return text;
}

std::string escape_preview(std::string text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        if (ch == '\n' || ch == '\r' || ch == '\t') {
            out.push_back(' ');
        } else if (static_cast<unsigned char>(ch) < 0x20) {
            out.push_back('?');
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

void append_quoted(std::ostringstream& out, const std::string& value, std::size_t budget) {
    out << '"';
    out << truncate_cells(escape_preview(value), budget);
    out << '"';
}

bool looks_like_path_key(const std::string& key) {
    return key == "path" || key == "file" || key == "cwd" || key == "directory" ||
           key == "dir" || key.rfind("path", 0) == 0;
}

}  // namespace

std::size_t terminal_column_count(std::size_t fallback) {
    if (fallback < 20) fallback = 20;
    winsize ws{};
    if (isatty(STDOUT_FILENO) && ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 20) {
        return static_cast<std::size_t>(ws.ws_col);
    }
    if (isatty(STDERR_FILENO) && ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 20) {
        return static_cast<std::size_t>(ws.ws_col);
    }
    const char* columns = std::getenv("COLUMNS");
    if (columns != nullptr && columns[0] != '\0') {
        char* end = nullptr;
        const long value = std::strtol(columns, &end, 10);
        if (end != columns && value >= 20 && value <= 1000) {
            return static_cast<std::size_t>(value);
        }
    }
    return fallback;
}

std::string clip_to_cells(const std::string& text, std::size_t max_cells) {
    return truncate_cells(text, max_cells);
}

std::string compact_tool_args_preview(const std::string& arguments_json, std::size_t max_cells) {
    if (arguments_json.empty() || arguments_json == "{}") return "";
    json::ParseResult parsed = json::parse(arguments_json);
    if (!parsed.error.ok() || parsed.value.type != json::Value::Type::Object) {
        return truncate_cells(escape_preview(arguments_json), max_cells);
    }

    std::ostringstream out;
    std::size_t remaining = max_cells;
    bool first = true;

    auto emit = [&](const std::string& piece) {
        if (piece.empty() || remaining == 0) return;
        if (!first) {
            if (remaining < 3) {
                remaining = 0;
                return;
            }
            out << ", ";
            remaining -= 2;
        }
        first = false;
        if (piece.size() > remaining) {
            out << truncate_cells(piece, remaining);
            remaining = 0;
        } else {
            out << piece;
            remaining -= piece.size();
        }
    };

    // Prefer path-like keys first.
    for (const auto& entry : parsed.value.object) {
        if (!looks_like_path_key(entry.first)) continue;
        if (entry.second.type != json::Value::Type::String) continue;
        std::ostringstream piece;
        append_quoted(piece, entry.second.string, remaining > 4 ? remaining - 2 : remaining);
        emit(piece.str());
        if (remaining == 0) break;
    }
    // Then a few other short string/number fields.
    for (const auto& entry : parsed.value.object) {
        if (remaining == 0) break;
        if (looks_like_path_key(entry.first)) continue;
        if (entry.second.type == json::Value::Type::String) {
            std::ostringstream piece;
            // Skip huge bodies (content/new_string/etc.) — show short marker.
            if (entry.second.string.size() > 48 || entry.first == "content" ||
                entry.first == "new_string" || entry.first == "old_string" ||
                entry.first == "patch" || entry.first == "diff" || entry.first == "text") {
                piece << entry.first << "=\""
                      << truncate_cells(escape_preview(entry.second.string), 24) << '"';
            } else {
                piece << entry.first << '=';
                append_quoted(piece, entry.second.string, 32);
            }
            emit(piece.str());
        } else if (entry.second.type == json::Value::Type::Number) {
            emit(entry.first + "=" + std::to_string(static_cast<long long>(entry.second.number)));
        } else if (entry.second.type == json::Value::Type::Bool) {
            emit(entry.first + std::string("=") + (entry.second.boolean ? "true" : "false"));
        }
    }
    return out.str();
}

std::string compact_tool_status(const std::string& result_json) {
    if (result_json.find("\"ok\":true") != std::string::npos ||
        result_json.find("\"ok\": true") != std::string::npos) {
        return "ok";
    }
    if (result_json.empty()) return "ok";
    return "error";
}

std::string format_compact_tool_line(std::size_t index,
                                     const std::string& tool_name,
                                     const std::string& arguments_json,
                                     const std::string& result_json,
                                     std::size_t max_line_cells) {
    if (max_line_cells == 0) max_line_cells = terminal_column_count();
    if (max_line_cells < 20) max_line_cells = 20;

    const std::string name = tool_name.empty() ? "tool" : tool_name;
    const std::string status = compact_tool_status(result_json);
    // Fixed framing: "N: name() → status"
    const std::string prefix = std::to_string(index) + ": " + name + "(";
    const std::string suffix = ") → " + status;
    const std::size_t framing = prefix.size() + suffix.size();
    const std::size_t arg_budget =
        max_line_cells > framing ? max_line_cells - framing : 0;

    std::ostringstream out;
    out << prefix;
    if (arg_budget > 0) out << compact_tool_args_preview(arguments_json, arg_budget);
    out << suffix;
    return clip_to_cells(out.str(), max_line_cells);
}

}  // namespace ainiux::agent
