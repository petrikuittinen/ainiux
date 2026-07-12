#pragma once

#include <string>

#include "common.hpp"

namespace pkchat::editor {

enum class EditorSlashCommand {
    None,
    Save,
    SaveAs,
    Find,
    Replace,
    Open,
    New,
    List,
    Close,
    Chat,
};

struct ParsedEditorSlashCommand {
    EditorSlashCommand command = EditorSlashCommand::None;
    std::string path;
};

std::string embedded_editor_help_markdown();
Error load_editor_help_markdown(std::string& out);
bool is_editor_help_command(const std::string& line);
ParsedEditorSlashCommand parse_editor_slash_command(const std::string& line);
// Returns the index in input where the path token starts, or npos when not in path mode.
size_t editor_assist_path_prefix_length(const std::string& input);

}  // namespace pkchat::editor
