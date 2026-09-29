#pragma once

#include <string>

#include "cli/args.hpp"
#include "common.hpp"
#include "agent/approval.hpp"
#include "agent/prompts.hpp"

namespace ainiux::agent {

// Restore an existing project's provider and request settings into startup
// defaults. This does not create .ainiux-pr for a new project.
Error restore_project_settings(const std::string& workspace,
                               cli::Options& options,
                               bool& restored,
                               PermissionMode* permission_mode = nullptr);
Error saved_agent_lane(const std::string& settings_json, AgentLane& lane);
Error settings_with_agent_lane(const std::string& settings_json, AgentLane lane,
                               std::string& updated);
Error merge_project_model_settings(const std::string& settings_json,
                                   const cli::Options& options, std::string& updated);
Error merge_project_lane_settings(const std::string& settings_json,
                                  AgentLane lane,
                                  const cli::Options& options,
                                  std::string& updated);
Error apply_project_lane_settings(const std::string& settings_json,
                                  AgentLane lane,
                                  cli::Options& options,
                                  bool& found);
Error restore_project_lane_settings(const std::string& workspace,
                                    AgentLane lane,
                                    cli::Options& options,
                                    bool& found);
// Editor saves merge request settings into the singleton project, preserving
// agent-only state and transcript. Call on a worker, with no active agent turn.
Error save_project_model_settings(const std::string& workspace, const cli::Options& options);

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
Error lane_imported_through_seq_from_settings_json(const std::string& settings_json,
                                                   AgentLane lane,
                                                   long long& seq);
Error settings_json_with_lane_imported_through_seq(const std::string& settings_json,
                                                   AgentLane lane,
                                                   long long seq,
                                                   std::string& updated);
Error lane_context_after_seq_from_settings_json(const std::string& settings_json,
                                                AgentLane lane,
                                                long long& seq);
Error settings_json_with_lane_context_after_seq(const std::string& settings_json,
                                                AgentLane lane,
                                                long long seq,
                                                std::string& updated);

}  // namespace ainiux::agent
