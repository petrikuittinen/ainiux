#include "editor/editor_help.hpp"

#include "detail/editor_common.hpp"
#include "embedded_editor_help.hpp"
#include "platform/environment.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace ainiux::editor {
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
    if (command_token == "replace" || command_token == "replace-string") {
        return EditorSlashCommand::Replace;
    }
    if (command_token == "open") {
        return EditorSlashCommand::Open;
    }
    if (command_token == "new") {
        return EditorSlashCommand::New;
    }
    if (command_token == "list") {
        return EditorSlashCommand::List;
    }
    if (command_token == "close") {
        return EditorSlashCommand::Close;
    }
    if (command_token == "chat") {
        return EditorSlashCommand::Chat;
    }
    if (command_token == "agent") {
        return EditorSlashCommand::Agent;
    }
    if (command_token == "editor") {
        return EditorSlashCommand::Editor;
    }
    if (command_token == "vsplit") {
        return EditorSlashCommand::VSplit;
    }
    if (command_token == "hsplit") {
        return EditorSlashCommand::HSplit;
    }
    if (command_token == "closesplit") {
        return EditorSlashCommand::CloseSplit;
    }
    if (command_token == "maximize" || command_token == "nosplit") {
        return EditorSlashCommand::Maximize;
    }
    if (command_token == "goto-line" || command_token == "gotoline" ||
        command_token == "goto_line" || command_token == "goto") {
        return EditorSlashCommand::GotoLine;
    }
    if (command_token == "dired") {
        return EditorSlashCommand::Dired;
    }
    if (command_token == "statistics" || command_token == "word-count") {
        return EditorSlashCommand::Statistics;
    }
    return EditorSlashCommand::None;
}

std::string read_file_text(const std::string& path) {
    std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
    if (!input) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::vector<std::string> editor_help_search_paths() {
    std::vector<std::string> paths;
    const std::string override_path = platform::environment_value("AINIUX_EDITOR_HELP");
    if (!override_path.empty()) paths.push_back(override_path);
    const std::string xdg_data = platform::environment_value("XDG_DATA_HOME");
    const std::string home = platform::home_directory();
    if (!xdg_data.empty())
        paths.push_back(xdg_data + "/ainiux/editor_help.md");
    else if (!home.empty())
        paths.push_back(home + "/.local/share/ainiux/editor_help.md");
    const std::string executable = platform::executable_directory();
    if (!executable.empty()) {
        const std::filesystem::path exe = std::filesystem::u8path(executable);
        paths.push_back(
            (exe / ".." / "share" / "ainiux" / "editor_help.md").lexically_normal().u8string());
        paths.push_back((exe / "share" / "ainiux" / "editor_help.md").u8string());
    }
    paths.emplace_back("/usr/local/share/ainiux/editor_help.md");
    paths.emplace_back("/usr/share/ainiux/editor_help.md");
    paths.emplace_back("docs/editor_help.md");
    paths.emplace_back("editor_help.md");
    return paths;
}

}  // namespace

std::string embedded_editor_help_markdown() {
    return kEditorHelpMarkdown;
}

size_t editor_assist_path_prefix_length(const std::string& input) {
    if (input.empty()) {
        return std::string::npos;
    }

    const size_t command_start = input[0] == '/' ? 1 : 0;
    size_t command_end = command_start;
    while (command_end < input.size() && !is_token_separator(input[command_end])) {
        ++command_end;
    }
    const std::string command_token = command_token_lower(input, command_start, command_end);
    if (command_token != "open" && command_token != "saveas" && command_token != "insert" &&
        command_token != "dired") {
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
    if (trimmed.empty()) {
        return parsed;
    }

    const size_t command_start = trimmed[0] == '/' ? 1 : 0;
    size_t command_end = command_start;
    while (command_end < trimmed.size() && !is_token_separator(trimmed[command_end])) {
        ++command_end;
    }
    parsed.command = command_from_token(command_token_lower(trimmed, command_start, command_end));
    if (parsed.command == EditorSlashCommand::None) {
        return parsed;
    }

    size_t path_start = command_end;
    while (path_start < trimmed.size() && is_token_separator(trimmed[path_start])) {
        ++path_start;
    }
    if (path_start < trimmed.size()) {
        const std::string path = trim_ascii_copy(trimmed.substr(path_start));
        if (parsed.command == EditorSlashCommand::New || parsed.command == EditorSlashCommand::List ||
            parsed.command == EditorSlashCommand::Close || parsed.command == EditorSlashCommand::Chat ||
            parsed.command == EditorSlashCommand::Agent ||
            parsed.command == EditorSlashCommand::Editor ||
            parsed.command == EditorSlashCommand::VSplit || parsed.command == EditorSlashCommand::HSplit ||
            parsed.command == EditorSlashCommand::CloseSplit ||
            parsed.command == EditorSlashCommand::Maximize ||
            parsed.command == EditorSlashCommand::Statistics) {
            parsed.command = EditorSlashCommand::None;
            return parsed;
        }
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

}  // namespace ainiux::editor
