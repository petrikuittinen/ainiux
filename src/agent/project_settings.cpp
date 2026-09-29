#include "agent/project_settings.hpp"

#include <algorithm>
#include <filesystem>

#include "agent/project_root.hpp"
#include "agent/session_store.hpp"
#include "chat/settings.hpp"
#include "json/json.hpp"

namespace ainiux::agent {

Error saved_agent_lane(const std::string& settings_json, AgentLane& lane) {
    lane = AgentLane::Act;
    const auto parsed = json::parse(settings_json.empty() ? "{}" : settings_json);
    if (!parsed.error.ok() || !parsed.value.is_object()) return {ErrorCode::Config, "invalid project settings"};
    const auto* mode = parsed.value.get("active_lane");
    if (!mode) return ok_error();
    if (!mode->is_string() || (mode->string != "act" && mode->string != "lead"))
        return {ErrorCode::Config, "saved active_lane must be act or lead"};
    lane = mode->string == "lead" ? AgentLane::Lead : AgentLane::Act;
    return ok_error();
}

Error settings_with_agent_lane(const std::string& settings_json, AgentLane lane,
                               std::string& updated) {
    auto parsed = json::parse(settings_json.empty() ? "{}" : settings_json);
    if (!parsed.error.ok() || !parsed.value.is_object()) return {ErrorCode::Config, "invalid project settings"};
    json::Value mode;
    mode.type = json::Value::Type::String;
    mode.string = lane == AgentLane::Lead ? "lead" : "act";
    parsed.value.object["active_lane"] = std::move(mode);
    updated = json::stringify(parsed.value);
    return ok_error();
}

Error merge_project_model_settings(const std::string& settings_json,
                                   const cli::Options& options, std::string& updated) {
    auto stored = json::parse(settings_json.empty() ? "{}" : settings_json);
    const auto next = json::parse(chat::settings_json_from_options(options));
    if (!stored.error.ok() || !stored.value.is_object()) return {ErrorCode::Config, "invalid project settings"};
    for (const auto& entry : next.value.object) stored.value.object[entry.first] = entry.second;
    updated = json::stringify(stored.value);
    return ok_error();
}

Error merge_project_lane_settings(const std::string& settings_json,
                                  AgentLane lane,
                                  const cli::Options& options,
                                  std::string& updated) {
    auto root = json::parse(settings_json.empty() ? "{}" : settings_json);
    auto values = json::parse(chat::settings_json_from_options(options));
    if (!root.error.ok() || !root.value.is_object() ||
        !values.error.ok() || !values.value.is_object())
        return {ErrorCode::Config, "invalid project mode settings"};
    json::Value text;
    text.type = json::Value::Type::String;
    text.string = options.provider;
    values.value.object["provider"] = text;
    text.string = options.base_url;
    values.value.object["base_url"] = text;
    text.string = options.api;
    values.value.object["api"] = text;
    text.string = options.model;
    values.value.object["model"] = text;
    json::Value& lanes = root.value.object["lanes"];
    if (!lanes.is_object()) {
        lanes = {};
        lanes.type = json::Value::Type::Object;
    }
    lanes.object[agent_lane_name(lane)] = std::move(values.value);
    updated = json::stringify(root.value);
    return ok_error();
}

Error apply_project_lane_settings(const std::string& settings_json,
                                  AgentLane lane,
                                  cli::Options& options,
                                  bool& found) {
    found = false;
    auto root = json::parse(settings_json.empty() ? "{}" : settings_json);
    if (!root.error.ok() || !root.value.is_object())
        return {ErrorCode::Config, "invalid project mode settings"};
    const json::Value* lanes = root.value.get("lanes");
    if (lanes == nullptr || !lanes->is_object()) return ok_error();
    const json::Value* values = lanes->get(agent_lane_name(lane));
    if (values == nullptr || !values->is_object()) return ok_error();
    Error error = chat::apply_settings_json(options, json::stringify(*values));
    if (!error.ok()) return error;
    auto copy_string = [&](const char* name, std::string& output) {
        const json::Value* value = values->get(name);
        if (value != nullptr && value->is_string()) output = value->string;
    };
    copy_string("provider", options.provider);
    copy_string("base_url", options.base_url);
    copy_string("api", options.api);
    copy_string("model", options.model);
    found = true;
    return ok_error();
}

Error restore_project_lane_settings(const std::string& workspace,
                                    AgentLane lane,
                                    cli::Options& options,
                                    bool& found) {
    found = false;
    AgentSessionStore store;
    Error error = store.open(workspace);
    if (!error.ok()) return error;
    AgentProjectRecord project;
    error = store.open_project(project);
    if (!error.ok()) return error;
    return apply_project_lane_settings(project.settings_json, lane, options, found);
}

Error save_project_model_settings(const std::string& workspace, const cli::Options& options) {
    AgentSessionStore store;
    Error error = store.open(workspace);
    if (!error.ok()) return error;
    AgentProjectRecord project;
    error = store.open_project(project);
    if (!error.ok()) return error;
    error = merge_project_model_settings(project.settings_json, options, project.settings_json);
    if (!error.ok()) return error;
    project.provider = options.provider; project.model = options.model;
    project.api = options.api; project.base_url = options.base_url;
    return store.update_project_meta(project);
}

Error permission_mode_from_settings_json(const std::string& settings_json,
                                         PermissionMode& mode) {
    mode = PermissionMode::Smart;
    if (settings_json.empty() || settings_json == "{}") return ok_error();
    const json::ParseResult parsed = json::parse(settings_json);
    if (!parsed.error.ok() || !parsed.value.is_object())
        return {ErrorCode::Config, "agent project settings must be a JSON object"};
    const json::Value* value = parsed.value.get("permission_mode");
    if (value == nullptr || value->type == json::Value::Type::Null) return ok_error();
    if (!value->is_string() || !parse_permission_mode(value->string, mode))
        return {ErrorCode::Config,
                "agent permission_mode must be confirm, smart, or yolo"};
    return ok_error();
}

Error settings_json_with_permission_mode(const std::string& settings_json,
                                         PermissionMode mode,
                                         std::string& updated) {
    json::Value root;
    if (settings_json.empty()) {
        root.type = json::Value::Type::Object;
    } else {
        json::ParseResult parsed = json::parse(settings_json);
        if (!parsed.error.ok() || !parsed.value.is_object())
            return {ErrorCode::Config, "agent project settings must be a JSON object"};
        root = std::move(parsed.value);
    }
    json::Value value;
    value.type = json::Value::Type::String;
    value.string = permission_mode_name(mode);
    root.object["permission_mode"] = std::move(value);
    updated = json::stringify(root);
    return ok_error();
}

Error context_reset_after_seq_from_settings_json(const std::string& settings_json,
                                                 long long& seq) {
    seq = 0;
    if (settings_json.empty() || settings_json == "{}") return ok_error();
    const json::ParseResult parsed = json::parse(settings_json);
    if (!parsed.error.ok() || !parsed.value.is_object())
        return {ErrorCode::Config, "agent project settings must be a JSON object"};
    const json::Value* value = parsed.value.get("context_reset_after_seq");
    if (value == nullptr || value->type == json::Value::Type::Null) return ok_error();
    if (value->type != json::Value::Type::Number || value->number < 0 ||
        value->number > 1.0e15)
        return {ErrorCode::Config, "agent context_reset_after_seq must be a non-negative integer"};
    seq = static_cast<long long>(value->number);
    return ok_error();
}

Error settings_json_with_context_reset_after_seq(const std::string& settings_json,
                                                 long long seq,
                                                 std::string& updated) {
    if (seq < 0) seq = 0;
    json::Value root;
    if (settings_json.empty()) {
        root.type = json::Value::Type::Object;
    } else {
        json::ParseResult parsed = json::parse(settings_json);
        if (!parsed.error.ok() || !parsed.value.is_object())
            return {ErrorCode::Config, "agent project settings must be a JSON object"};
        root = std::move(parsed.value);
    }
    json::Value value;
    value.type = json::Value::Type::Number;
    value.number = static_cast<double>(seq);
    root.object["context_reset_after_seq"] = std::move(value);
    updated = json::stringify(root);
    return ok_error();
}

Error lane_imported_through_seq_from_settings_json(const std::string& settings_json,
                                                   AgentLane lane,
                                                   long long& seq) {
    seq = 0;
    const json::ParseResult parsed = json::parse(settings_json.empty() ? "{}" : settings_json);
    if (!parsed.error.ok() || !parsed.value.is_object())
        return {ErrorCode::Config, "agent project settings must be a JSON object"};
    const std::string key = std::string(agent_lane_name(lane)) + "_imported_through_seq";
    const json::Value* value = parsed.value.get(key);
    if (value == nullptr) return ok_error();
    if (value->type != json::Value::Type::Number || value->number < 0 ||
        value->number > 1.0e15)
        return {ErrorCode::Config, "agent mode handoff sequence is invalid"};
    seq = static_cast<long long>(value->number);
    return ok_error();
}

Error settings_json_with_lane_imported_through_seq(const std::string& settings_json,
                                                   AgentLane lane,
                                                   long long seq,
                                                   std::string& updated) {
    json::ParseResult parsed = json::parse(settings_json.empty() ? "{}" : settings_json);
    if (!parsed.error.ok() || !parsed.value.is_object())
        return {ErrorCode::Config, "agent project settings must be a JSON object"};
    json::Value value;
    value.type = json::Value::Type::Number;
    value.number = static_cast<double>(std::max(0LL, seq));
    parsed.value.object[std::string(agent_lane_name(lane)) + "_imported_through_seq"] =
        std::move(value);
    updated = json::stringify(parsed.value);
    return ok_error();
}

Error lane_context_after_seq_from_settings_json(const std::string& settings_json,
                                                AgentLane lane,
                                                long long& seq) {
    seq = 0;
    const json::ParseResult parsed = json::parse(settings_json.empty() ? "{}" : settings_json);
    if (!parsed.error.ok() || !parsed.value.is_object())
        return {ErrorCode::Config, "agent project settings must be a JSON object"};
    const std::string key = std::string(agent_lane_name(lane)) + "_context_after_seq";
    const json::Value* value = parsed.value.get(key);
    if (value == nullptr) return ok_error();
    if (value->type != json::Value::Type::Number || value->number < 0 ||
        value->number > 1.0e15)
        return {ErrorCode::Config, "agent mode context sequence is invalid"};
    seq = static_cast<long long>(value->number);
    return ok_error();
}

Error settings_json_with_lane_context_after_seq(const std::string& settings_json,
                                                AgentLane lane,
                                                long long seq,
                                                std::string& updated) {
    json::ParseResult parsed = json::parse(settings_json.empty() ? "{}" : settings_json);
    if (!parsed.error.ok() || !parsed.value.is_object())
        return {ErrorCode::Config, "agent project settings must be a JSON object"};
    json::Value value;
    value.type = json::Value::Type::Number;
    value.number = static_cast<double>(std::max(0LL, seq));
    parsed.value.object[std::string(agent_lane_name(lane)) + "_context_after_seq"] =
        std::move(value);
    updated = json::stringify(parsed.value);
    return ok_error();
}

Error restore_project_settings(const std::string& workspace,
                               cli::Options& options,
                               bool& restored,
                               PermissionMode* permission_mode) {
    restored = false;
    std::string root;
    Error error = resolve_agent_project_root(workspace, root);
    if (!error.ok()) return error;

    std::error_code filesystem_error;
    const std::string database = AgentSessionStore::database_path(root);
    const bool exists = std::filesystem::exists(database, filesystem_error);
    if (filesystem_error) {
        return {ErrorCode::FileRead,
                "could not inspect agent project settings at " + database + ": " +
                    filesystem_error.message()};
    }
    if (!exists) return ok_error();

    AgentSessionStore store;
    error = store.open(root);
    if (!error.ok()) return error;
    AgentProjectRecord project;
    error = store.open_project(project);
    if (!error.ok()) return error;

    AgentLane lane = AgentLane::Act;
    error = saved_agent_lane(project.settings_json, lane);
    if (!error.ok()) return error;
    bool lane_found = false;
    error = apply_project_lane_settings(project.settings_json, lane, options, lane_found);
    if (!error.ok()) return error;
    if (lane_found) {
        options.agent_project_settings_restored = true;
        restored = true;
    }
    if (!lane_found) error = chat::apply_settings_json(
        options, project.settings_json.empty() ? "{}" : project.settings_json);
    if (!error.ok()) {
        return {error.code,
                "could not restore agent project settings from " + database + ": " +
                    error.message};
    }
    if (permission_mode != nullptr) {
        error = permission_mode_from_settings_json(project.settings_json, *permission_mode);
        if (!error.ok()) {
            return {error.code,
                    "could not restore agent permission settings from " + database + ": " +
                        error.message};
        }
    }
    if (!lane_found && !project.provider.empty()) {
        options.provider = project.provider;
        options.base_url = project.base_url;
        options.chat_url.clear();
        options.models_url.clear();
        options.responses_url.clear();
        options.positional_url.clear();
        if (!project.model.empty()) options.model = project.model;
        if (!project.api.empty()) options.api = project.api;
        options.agent_project_settings_restored = true;
        restored = true;
    }
    return ok_error();
}

}  // namespace ainiux::agent
