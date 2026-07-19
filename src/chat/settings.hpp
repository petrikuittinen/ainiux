#pragma once

#include <string>
#include <vector>

#include "chat/session.hpp"
#include "cli/args.hpp"
#include "common.hpp"
#include "config/model_catalog.hpp"
#include "ainiux/model_setting.hpp"

namespace ainiux::chat {

Error apply_model_setting_preset(cli::Options& options,
                                 const ModelSetting& preset,
                                 const ModelCapability* capability = nullptr);
Error apply_chat_setting(cli::Options& options, const std::string& name, const std::string& value);
void reset_thread_setting_overrides(cli::Options& options);
std::string settings_json_from_options(const cli::Options& options);
Error apply_settings_json(cli::Options& options, const std::string& settings_json);
std::string current_system_prompt(const Session& session);
std::string format_settings_summary(const cli::Options& options);
std::string format_settings_panel(const cli::Options& options,
                                  const std::string& advisory = {});

}  // namespace ainiux::chat
