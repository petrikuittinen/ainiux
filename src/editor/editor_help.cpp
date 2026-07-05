#include "editor/editor_help.hpp"

#include "detail/editor_common.hpp"
#include "embedded_editor_help.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

namespace pkchat::editor {
namespace {

bool is_token_separator(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

std::string command_token_lower(const std::string& line, size_t command_start, size_t command_end) {
    std::string command;
    command.reserve(command_end - command_start);
    for (size_t i = command_start; i < command_end; ++i) {
        command.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(line[i]))));
    }
    return command;
}

EditorSlashCommand command_from_token(const std::string& command_token) {
    if (command_token == "save") {
        return EditorSlashCommand::Save;
    }
    if (command_token == "saveas") {
        return EditorSlashCommand::SaveAs;
    }
    if (command_token == "find") {
        return EditorSlashCommand::Find;
    }
    if (command_token == "replace") {
        return EditorSlashCommand::Replace;
    }
    if (command_token == "open") {
        return EditorSlashCommand::Open;
    }
    return EditorSlashCommand::None;
}

std::string read_file_text(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::vector<std::string> editor_help_search_paths() {
    std::vector<std::string> paths;
    if (const char* override_path = std::getenv("PKCHAT_EDITOR_HELP")) {
        if (override_path[0] != '\0') {
            paths.emplace_back(override_path);
        }
    }
    if (const char* xdg_data = std::getenv("XDG_DATA_HOME")) {
        if (xdg_data[0] != '\0') {
            paths.push_back(std::string(xdg_data) + "/pkchat/editor_help.md");
        }
    } else if (const char* home = std::getenv("HOME")) {
        if (home[0] != '\0') {
            paths.push_back(std::string(home) + "/.local/share/pkchat/editor_help.md");
        }
    }
    paths.emplace_back("/usr/local/share/pkchat/editor_help.md");
    paths.emplace_back("/usr/share/pkchat/editor_help.md");
    paths.emplace_back("docs/editor_help.md");
    paths.emplace_back("editor_help.md");
    return paths;
}

}  // namespace

std::string embedded_editor_help_markdown() {
    return kEditorHelpMarkdown;
}

size_t editor_assist_path_prefix_length(const std::string& input) {
    if (input.empty() || input[0] != '/') {
        return std::string::npos;
    }

    size_t command_end = 1;
    while (command_end < input.size() && !is_token_separator(input[command_end])) {
        ++command_end;
    }
    const std::string command_token = command_token_lower(input, 1, command_end);
    if (command_token != "open" && command_token != "saveas") {
        return std::string::npos;
    }
    if (command_end >= input.size()) {
        return std::string::npos;
    }

    size_t path_start = command_end;
    while (path_start < input.size() && is_token_separator(input[path_start])) {
        ++path_start;
    }
    return path_start;
}

ParsedEditorSlashCommand parse_editor_slash_command(const std::string& line) {
    ParsedEditorSlashCommand parsed;
    const std::string trimmed = trim_ascii_copy(line);
    if (trimmed.empty() || trimmed[0] != '/') {
        return parsed;
    }

    size_t command_end = 1;
    while (command_end < trimmed.size() && !is_token_separator(trimmed[command_end])) {
        ++command_end;
    }
    parsed.command = command_from_token(command_token_lower(trimmed, 1, command_end));
    if (parsed.command == EditorSlashCommand::None) {
        return parsed;
    }

    size_t path_start = command_end;
    while (path_start < trimmed.size() && is_token_separator(trimmed[path_start])) {
        ++path_start;
    }
    if (path_start < trimmed.size()) {
        const std::string path = trim_ascii_copy(trimmed.substr(path_start));
        if (path.find(' ') != std::string::npos || path.find('\t') != std::string::npos) {
            parsed.command = EditorSlashCommand::None;
            return parsed;
        }
        parsed.path = path;
    }
    return parsed;
}

bool is_editor_help_command(const std::string& line) {
    std::string trimmed;
    trimmed.reserve(line.size());
    for (char ch : line) {
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            trimmed.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
    }
    return trimmed == "/help" || trimmed == "help";
}

Error load_editor_help_markdown(std::string& out) {
    for (const std::string& path : editor_help_search_paths()) {
        const std::string loaded = read_file_text(path);
        if (!loaded.empty()) {
            out = loaded;
            return ok_error();
        }
    }
    out = embedded_editor_help_markdown();
    if (out.empty()) {
        return {ErrorCode::FileRead, "editor help document is unavailable"};
    }
    return ok_error();
}

}  // namespace pkchat::editor