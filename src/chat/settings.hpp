#pragma once

#include <string>
#include <vector>

#include "chat/session.hpp"
#include "cli/args.hpp"
#include "common.hpp"
#include "pkchat/model_setting.hpp"

namespace pkchat::chat {

bool model_pattern_matches(const std::string& pattern, const std::string& model);
const ModelSetting* find_model_setting(const std::string& model,
                                       const std::string& purpose,
                                       const std::vector<ModelSetting>& presets);
Error apply_model_setting_preset(cli::Options& options, const ModelSetting& preset);
Error apply_chat_setting(cli::Options& options, const std::string& name, const std::string& value);
void reset_thread_setting_overrides(cli::Options& options);
std::string settings_json_from_options(const cli::Options& options);
Error apply_settings_json(cli::Options& options, const std::string& settings_json);
std::string current_system_prompt(const Session& session);
std::string format_settings_summary(const cli::Options& options);
std::string format_settings_panel(const cli::Options& options);
bool thinking_budget_is_token_count(const std::string& value);
void append_thinking_budget_json(std::ostringstream& out, const std::string& value);

}  // namespace pkchat::chat