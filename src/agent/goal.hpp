#pragma once

#include <string>

#include "common.hpp"

namespace ainiux::agent {

// Session-scoped completion condition for interactive /goal (one per project).
enum class GoalStatus { Active, Paused, Complete, Cleared };

struct SessionGoal {
    std::string condition;
    GoalStatus status = GoalStatus::Cleared;
    int turns = 0;  // auto-continuation count while Active
    std::string last_reason;  // last complete/block/clear note (bounded)
};

const char* goal_status_name(GoalStatus status);
bool parse_goal_status(const std::string& text, GoalStatus& status);

// Human-readable one-line status for /goal and TUI notices.
std::string format_goal_status(const SessionGoal& goal);

// Bound evidence / reasons stored on the goal and in notices.
std::string bound_goal_text(const std::string& text, std::size_t max_bytes = 2000);

// Control fragment injected only while status == Active (user-role, not system).
std::string agent_goal_control(const SessionGoal& goal);
// Auto-continue nudge after a tool-less FinalText while the goal remains Active.
std::string agent_goal_continue_control(const SessionGoal& goal);

bool goal_is_active(const SessionGoal& goal);

// JSON helpers for project.settings_json["goal"].
Error goal_from_settings_json(const std::string& settings_json, SessionGoal& goal);
Error settings_json_with_goal(const std::string& settings_json,
                              const SessionGoal& goal,
                              std::string& updated);

}  // namespace ainiux::agent
