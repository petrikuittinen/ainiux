#include "agent/tool_display.hpp"

#include <cctype>
#include <sstream>

#include "json/json.hpp"

namespace ainiux::agent {
namespace {

std::string truncate_cells(std::string text, std::size_t max_cells) {
    if (max_cells == 0) return {};
    // Approximate cells as UTF-8 bytes for now (agent lines stay ASCII-heavy).
    if (text.size() <= max_cells) return text;
    if (max_cells <= 3) return "...";
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
                                     std::size_t max_arg_cells) {
    std::ostringstream out;
    out << index << ": " << (tool_name.empty() ? "tool" : tool_name) << '(';
    out << compact_tool_args_preview(arguments_json, max_arg_cells);
    out << ") → " << compact_tool_status(result_json);
    return out.str();
}

}  // namespace ainiux::agent
