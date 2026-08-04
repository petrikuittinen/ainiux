#include "agent/tool_display.hpp"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <sstream>

#include "json/json.hpp"
#include "tui/terminal.hpp"

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
    const tui::TerminalDimensions terminal = tui::terminal_dimensions();
    if (tui::terminal_output_is_interactive() && terminal.cols >= 20)
        return static_cast<std::size_t>(terminal.cols);
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

long long now_unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

long long normalize_timestamp_ms(long long stored_created_at) {
    if (stored_created_at <= 0) return 0;
    // Unix seconds are ~1e9; milliseconds are ~1e12.
    if (stored_created_at < 1000000000000LL) return stored_created_at * 1000;
    return stored_created_at;
}

std::string format_elapsed_ms(long long elapsed_ms) {
    if (elapsed_ms < 0) elapsed_ms = 0;
    return std::to_string(elapsed_ms) + " ms";
}

long long execution_only_elapsed_ms(long long wall_elapsed_ms,
                                    long long approval_wait_before_ms,
                                    long long approval_wait_after_ms) {
    const long long approval_delta =
        std::max(0LL, approval_wait_after_ms - approval_wait_before_ms);
    return std::max(0LL, wall_elapsed_ms - approval_delta);
}

std::string format_task_complete(long long elapsed_ms) {
    if (elapsed_ms < 0) elapsed_ms = 0;
    const double seconds = static_cast<double>(elapsed_ms) / 1000.0;
    std::ostringstream out;
    out << "Task complete in " << std::fixed << std::setprecision(2) << seconds << " seconds.";
    return out.str();
}

std::string format_elapsed_seconds(long long elapsed_ms) {
    if (elapsed_ms < 0) elapsed_ms = 0;
    const double seconds = static_cast<double>(elapsed_ms) / 1000.0;
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << seconds << " seconds elapsed";
    return out.str();
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

    // read_many carries paths inside an items array rather than at the top
    // level. Show the batch size and first two paths so one batched call is
    // obvious in compact agent activity without dumping the full JSON.
    const json::Value* items = parsed.value.get("items");
    if (items != nullptr && items->is_array()) {
        std::ostringstream piece;
        piece << items->array.size()
              << (items->array.size() == 1 ? " read" : " reads");
        std::size_t shown = 0;
        for (const json::Value& item : items->array) {
            if (!item.is_object()) continue;
            const json::Value* path = item.get("path");
            if (path == nullptr || !path->is_string()) continue;
            piece << (shown == 0 ? ": " : ", ");
            append_quoted(piece, path->string, 32);
            if (++shown == 2) break;
        }
        if (shown < items->array.size() && shown != 0) piece << ", ...";
        emit(piece.str());
    }

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

namespace {

std::string lower_ascii_copy(std::string text) {
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return text;
}

// Collapse long policy messages into a short status token for one-line tool logs.
std::string classify_tool_error_message(const std::string& message) {
    const std::string lower = lower_ascii_copy(message);
    if (lower.find("outside the project") != std::string::npos ||
        lower.find("escapes workspace") != std::string::npos ||
        lower.find("home path") != std::string::npos ||
        (lower.find("~/") != std::string::npos && lower.find("forbidden") != std::string::npos) ||
        lower.find("not absolute, ~/") != std::string::npos ||
        lower.find("\"~\" is not expanded") != std::string::npos) {
        return "outside project (use project-relative path)";
    }
    if (lower.find("allowlist") != std::string::npos ||
        lower.find("not on the agent") != std::string::npos ||
        lower.find("shell wrappers are not allowed") != std::string::npos ||
        lower.find("privilege escalation") != std::string::npos ||
        lower.find("disk/device destructive") != std::string::npos ||
        lower.find("host power/service") != std::string::npos ||
        lower.find("package managers are not allowed") != std::string::npos ||
        lower.find("remote shell") != std::string::npos ||
        lower.find("install/publish changes") != std::string::npos ||
        lower.find("is not allowed via run_command") != std::string::npos) {
        // Prefer a short form from legacy allowlist messages, else "policy denied".
        const std::string marker = "allowlist: ";
        const std::size_t pos = lower.find(marker);
        if (pos != std::string::npos) {
            std::string cmd = message.substr(pos + marker.size());
            const std::size_t cut = cmd.find(' ');
            if (cut != std::string::npos) cmd = cmd.substr(0, cut);
            const std::size_t paren = cmd.find('(');
            if (paren != std::string::npos) cmd = cmd.substr(0, paren);
            if (!cmd.empty()) return cmd + " not allowed";
        }
        if (lower.find("shell") != std::string::npos) return "shell not allowed";
        if (lower.find("sudo") != std::string::npos || lower.find("privilege") != std::string::npos)
            return "privilege escalation denied";
        return "command not allowed";
    }
    if (lower.find("approval") != std::string::npos ||
        lower.find("headless agent denies") != std::string::npos) {
        return "needs user approval (y/n)";
    }
    if (lower.find("policy") != std::string::npos || lower.find("refusing") != std::string::npos)
        return "policy denied";
    if (lower.find("stale_file") != std::string::npos || lower.find("stale") != std::string::npos)
        return "stale file";
    if (lower.find("not found") != std::string::npos) return "not found";
    if (lower.find("cancelled") != std::string::npos) return "cancelled";
    // Fallback: first clause, clipped.
    std::string brief = message;
    const std::size_t semi = brief.find(';');
    if (semi != std::string::npos) brief = brief.substr(0, semi);
    const std::size_t period = brief.find(". ");
    if (period != std::string::npos && period + 2 < brief.size()) brief = brief.substr(0, period);
    return brief;
}

}  // namespace

std::string compact_tool_error_brief(const std::string& result_json, std::size_t max_cells) {
    if (compact_tool_status(result_json) == "ok") return {};
    json::ParseResult parsed = json::parse(result_json);
    std::string message;
    if (parsed.error.ok() && parsed.value.is_object()) {
        const json::Value* err = parsed.value.get("error");
        if (err != nullptr && err->is_object()) {
            const json::Value* msg = err->get("message");
            if (msg != nullptr && msg->is_string()) message = msg->string;
            if (message.empty()) {
                const json::Value* code = err->get("code");
                if (code != nullptr && code->is_string()) message = code->string;
            }
        }
    }
    if (message.empty()) return "failed";
    std::string brief = classify_tool_error_message(message);
    if (brief.empty()) brief = "failed";
    return truncate_cells(escape_preview(brief), max_cells == 0 ? 56 : max_cells);
}

std::string format_compact_tool_line(std::size_t index,
                                     const std::string& tool_name,
                                     const std::string& arguments_json,
                                     const std::string& result_json,
                                     long long execution_ms,
                                     std::size_t max_line_cells) {
    if (max_line_cells == 0) max_line_cells = terminal_column_count();
    if (max_line_cells < 20) max_line_cells = 20;

    const std::string name = tool_name.empty() ? "tool" : tool_name;
    const std::string status = compact_tool_status(result_json);
    std::string error_text;
    if (status == "error") {
        const std::string brief = compact_tool_error_brief(result_json, 48);
        if (!brief.empty()) error_text = ": " + brief;
    }
    // Fixed framing and timing are never sacrificed. Error detail is preferred
    // over arguments when the terminal is narrow.
    const std::string status_suffix = ") → " + status;
    const std::string timing_suffix = " in " + format_elapsed_ms(execution_ms);
    const std::string index_prefix = std::to_string(index) + ": ";
    const std::size_t fixed_without_name =
        index_prefix.size() + 1 + status_suffix.size() + timing_suffix.size();
    const std::size_t name_budget =
        max_line_cells > fixed_without_name ? max_line_cells - fixed_without_name : 0;
    const std::string prefix =
        index_prefix + truncate_cells(name, name_budget) + "(";
    const std::size_t fixed =
        prefix.size() + status_suffix.size() + timing_suffix.size();
    std::size_t variable_budget =
        max_line_cells > fixed ? max_line_cells - fixed : 0;
    if (error_text.size() > variable_budget)
        error_text = clip_to_cells(error_text, variable_budget);
    variable_budget -= std::min(variable_budget, error_text.size());

    std::ostringstream out;
    out << prefix;
    if (variable_budget > 0)
        out << compact_tool_args_preview(arguments_json, variable_budget);
    out << status_suffix << error_text << timing_suffix;
    return clip_to_cells(out.str(), max_line_cells);
}

}  // namespace ainiux::agent
