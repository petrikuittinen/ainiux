#include "agent/project_settings.hpp"

#include <filesystem>

#include "agent/project_root.hpp"
#include "agent/session_store.hpp"
#include "chat/settings.hpp"

namespace ainiux::agent {

Error restore_project_settings(const std::string& workspace,
                               cli::Options& options,
                               bool& restored) {
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
