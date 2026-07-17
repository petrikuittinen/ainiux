#pragma once

#include <optional>
#include <string>

#include "common.hpp"
#include "editor/ai_continue.hpp"
#include "editor/editor_prompts.hpp"

namespace ainiux::editor {

bool editor_ai_has_provider(const std::optional<AiContinueContext>& context);
bool editor_ai_ready(const std::optional<AiContinueContext>& context);
std::string editor_no_provider_message();
std::string editor_no_model_message();
std::string editor_startup_status(const std::optional<AiContinueContext>& context);
Error ensure_editor_ai_context(std::optional<AiContinueContext>& context, const EditorAssistConfig& assist_config);
Error apply_editor_provider_target(std::optional<AiContinueContext>& context,
                                  const EditorAssistConfig& assist_config,
                                  const std::string& target);
Error apply_editor_model(std::optional<AiContinueContext>& context, const std::string& model);

}  // namespace ainiux::editor