#pragma once

#include <string>

namespace pkchat::editor {

struct EditorAssistPrompts {
    std::string behavior_rules;
    std::string spell;
    std::string grammar;
    std::string continue_prompt;
    std::string fact;
};

EditorAssistPrompts default_editor_assist_prompts();

}  // namespace pkchat::editor