#include "editor/editor_help.hpp"

#include "embedded_editor_help.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

namespace pkchat::editor {
namespace {

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