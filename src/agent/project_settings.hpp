#pragma once

#include <string>

#include "cli/args.hpp"
#include "common.hpp"
#include "agent/approval.hpp"

namespace ainiux::agent {

// Restore an existing project's provider and request settings into startup
// defaults. This does not create .ainiux-pr for a new project.
Error restore_project_settings(const std::string& workspace,
                               cli::Options& options,
                               bool& restored,
                               PermissionMode* permission_mode = nullptr);

// Agent-only values share the existing project settings_json object without
// becoming global CLI/chat settings.
Error permission_mode_from_settings_json(const std::string& settings_json,
                                         PermissionMode& mode);
Error settings_json_with_permission_mode(const std::string& settings_json,
                                         PermissionMode mode,
                                         std::string& updated);

// Persist the /compact all cut: model/display projection ignores seq <= value.
Error context_reset_after_seq_from_settings_json(const std::string& settings_json,
                                                 long long& seq);
Error settings_json_with_context_reset_after_seq(const std::string& settings_json,
                                                 long long seq,
                                                 std::string& updated);

}  // namespace ainiux::agent
