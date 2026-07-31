#include "agent/goal.hpp"

#include <cctype>

#include "json/json.hpp"

namespace ainiux::agent {
namespace {

std::string ascii_lower(std::string text) {
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return text;
}

json::Value string_value(const std::string& text) {
    json::Value value;
    value.type = json::Value::Type::String;
    value.string = text;
    return value;
}

json::Value number_value(double number) {
    json::Value value;
    value.type = json::Value::Type::Number;
    value.number = number;
    return value;
}

json::Value object_value() {
    json::Value value;
    value.type = json::Value::Type::Object;
    return value;
}

}  // namespace

const char* goal_status_name(GoalStatus status) {
    switch (status) {
        case GoalStatus::Active:
            return "active";
        case GoalStatus::Paused:
            return "paused";
        case GoalStatus::Complete:
            return "complete";
        case GoalStatus::Cleared:
            return "cleared";
    }
    return "cleared";
}

bool parse_goal_status(const std::string& text, GoalStatus& status) {
    const std::string lower = ascii_lower(ascii_trim(text));
    if (lower == "active") {
        status = GoalStatus::Active;
        return true;
    }
    if (lower == "paused") {
        status = GoalStatus::Paused;
        return true;
    }
    if (lower == "complete" || lower == "completed") {
        status = GoalStatus::Complete;
        return true;
    }
    if (lower == "cleared" || lower == "clear" || lower.empty()) {
        status = GoalStatus::Cleared;
        return true;
    }
    return false;
}

std::string bound_goal_text(const std::string& text, std::size_t max_bytes) {
    std::string out = ascii_trim(text);
    if (max_bytes == 0) return {};
    if (out.size() <= max_bytes) return out;
    if (max_bytes <= 3) return out.substr(0, max_bytes);
    out.resize(max_bytes - 3);
    out += "...";
    return out;
}

bool goal_is_active(const SessionGoal& goal) {
    return goal.status == GoalStatus::Active && !ascii_trim(goal.condition).empty();
}

std::string format_goal_status(const SessionGoal& goal) {
    if (goal.status == GoalStatus::Cleared || goal.condition.empty()) {
        return "Goal: none";
    }
    std::string line = "Goal (" + std::string(goal_status_name(goal.status)) + "): " +
                       bound_goal_text(goal.condition, 160);
    if (goal.turns > 0) line += " · turns " + std::to_string(goal.turns);
    if (!goal.last_reason.empty() &&
        (goal.status == GoalStatus::Complete || goal.status == GoalStatus::Paused)) {
        line += " · " + bound_goal_text(goal.last_reason, 80);
    }
    return line;
}

std::string agent_goal_control(const SessionGoal& goal) {
    if (!goal_is_active(goal)) return {};
    return std::string("ACTIVE GOAL: ") + goal.condition +
           "\n\n"
           "You must keep working until the goal is met. After each meaningful step, decide:\n"
           "- If the condition is clearly satisfied by evidence already obtained → call "
           "goal_met with that evidence.\n"
           "- If blocked (missing info, impossible, needs user decision) → stop and report "
           "the blocker.\n"
           "- Otherwise continue: plan the next concrete action and take it.\n\n"
           "Do not claim the goal is done without calling goal_met. Do not invent evidence.";
}

std::string agent_goal_continue_control(const SessionGoal& goal) {
    if (!goal_is_active(goal)) return {};
    return agent_goal_control(goal) +
           "\n\n"
           "The previous assistant reply ended without calling goal_met. The goal is still "
           "active. Either call goal_met with concrete evidence, report a clear blocker and "
           "stop taking tools, or take the next concrete action.";
}

Error goal_from_settings_json(const std::string& settings_json, SessionGoal& goal) {
    goal = SessionGoal{};
    if (settings_json.empty() || settings_json == "{}") return ok_error();
    const json::ParseResult parsed = json::parse(settings_json);
    if (!parsed.error.ok() || !parsed.value.is_object())
        return {ErrorCode::Config, "agent project settings must be a JSON object"};
    const json::Value* value = parsed.value.get("goal");
    if (value == nullptr || value->type == json::Value::Type::Null) return ok_error();
    if (!value->is_object())
        return {ErrorCode::Config, "agent goal settings must be a JSON object"};
    if (const json::Value* condition = value->get("condition");
        condition != nullptr && condition->is_string()) {
        goal.condition = condition->string;
    }
    if (const json::Value* status = value->get("status");
        status != nullptr && status->is_string()) {
        if (!parse_goal_status(status->string, goal.status))
            return {ErrorCode::Config,
                    "agent goal status must be active, paused, complete, or cleared"};
    } else if (!goal.condition.empty()) {
        goal.status = GoalStatus::Active;
    }
    if (const json::Value* turns = value->get("turns");
        turns != nullptr && turns->type == json::Value::Type::Number) {
        if (turns->number < 0 || turns->number > 1000000)
            return {ErrorCode::Config, "agent goal turns out of range"};
        goal.turns = static_cast<int>(turns->number);
    }
    if (const json::Value* reason = value->get("last_reason");
        reason != nullptr && reason->is_string()) {
        goal.last_reason = bound_goal_text(reason->string);
    }
    if (goal.status == GoalStatus::Active && ascii_trim(goal.condition).empty())
        goal.status = GoalStatus::Cleared;
    return ok_error();
}

Error settings_json_with_goal(const std::string& settings_json,
                              const SessionGoal& goal,
                              std::string& updated) {
    json::Value root;
    if (settings_json.empty()) {
        root = object_value();
    } else {
        json::ParseResult parsed = json::parse(settings_json);
        if (!parsed.error.ok() || !parsed.value.is_object())
            return {ErrorCode::Config, "agent project settings must be a JSON object"};
        root = std::move(parsed.value);
    }
    json::Value object = object_value();
    object.object["condition"] = string_value(goal.condition);
    object.object["status"] = string_value(goal_status_name(goal.status));
    object.object["turns"] = number_value(static_cast<double>(goal.turns));
    object.object["last_reason"] = string_value(bound_goal_text(goal.last_reason));
    root.object["goal"] = std::move(object);
    updated = json::stringify(root);
    return ok_error();
}

}  // namespace ainiux::agent
