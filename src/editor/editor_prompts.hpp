#pragma once

#include <optional>
#include <string>
#include <vector>

namespace pkchat::editor {

enum class AssistCommandMode {
    Continue,
    Selection,
    All,
    Insert,
    LocalInsert,
    Fact,
};

struct EditorAssistCommand {
    std::string command;
    std::vector<AssistCommandMode> modes;
    std::string prompt;
};

struct EditorAssistConfig {
    std::string behavior_rules;
    std::vector<EditorAssistCommand> commands;
};

EditorAssistConfig default_editor_assist_config();
const EditorAssistCommand* find_assist_command(const EditorAssistConfig& config, const std::string& command);
std::optional<size_t> assist_command_index(const EditorAssistConfig& config, const std::string& command);
bool assist_command_requires_scope(const EditorAssistCommand& command);
bool assist_command_runs_without_scope(const EditorAssistCommand& command);

}  // namespace pkchat::editor