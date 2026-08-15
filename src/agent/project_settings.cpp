#include "agent/project_settings.hpp"

#include <filesystem>

#include "agent/project_root.hpp"
#include "agent/session_store.hpp"
#include "chat/settings.hpp"
#include "json/json.hpp"

namespace ainiux::agent {

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

    error = chat::apply_settings_json(
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
    if (!project.provider.empty() && project.provider != "none") {
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
