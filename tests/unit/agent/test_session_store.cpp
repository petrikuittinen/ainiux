#include "agent/test_session_store.hpp"

#include <filesystem>
#include <string>
#include <vector>

#include "agent/goal.hpp"
#include "agent/project_settings.hpp"
#include "agent/session_store.hpp"
#include "platform/environment.hpp"
#include "support/test_support.hpp"

namespace ainiux::test::agent_session_store {
namespace {
using ainiux::test::check;
namespace fs = std::filesystem;

std::string temp_workspace(const std::string& name) {
    const fs::path root =
        fs::temp_directory_path() / ("ainiux-agent-session-" + name + "-" +
                                     std::to_string(ainiux::platform::current_process_id()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    return root.string();
}

void test_open_singleton_append_compact_load() {
    const std::string workspace = temp_workspace("basic");
    agent::AgentSessionStore store;
    Error error = store.open(workspace);
    check(error.ok(), "open agent.sqlite: " + error.message);
    check(store.is_open(), "store is open");
    check(fs::exists(store.path()), "agent.sqlite exists on disk");
    check(store.path().find(".ainiux-pr") != std::string::npos &&
              store.path().find("agent.sqlite") != std::string::npos,
          "path is project-local .ainiux-pr/agent.sqlite");

    agent::AgentProjectRecord project;
    project.provider = "openai";
    project.model = "test-model";
    project.api = "chat";
    project.protocol = "native";
    project.base_url = "https://example.test/v1";
    project.settings_json = R"({"reasoning":"high","temperature":0.25})";
    project.workspace = workspace;
    project.status = "running";
    error = store.open_project(project);
    check(error.ok() && project.id == 1, "open singleton project: " + error.message);

    error = store.append_message("user", "fix the thing");
    check(error.ok(), "append user message");
    error = store.append_message("tool", "1: read_file(\"a.cpp\") → ok", "read_file", true,
                                 "\"a.cpp\"");
    check(error.ok(), "append tool line");
    error = store.append_tool_event(1, 1, "call-1", "read_file", R"({"path":"a.cpp"})",
                                    R"({"ok":true})", true);
    check(error.ok(), "append tool event");
    error = store.append_message("assistant", "done");
    check(error.ok(), "append assistant");

    error = store.finish_session(1, "success", "done", "", "", 2, 1);
    check(error.ok(), "finish session");

    agent::AgentSessionRecord loaded;
    std::vector<agent::AgentMessageRecord> messages;
    std::vector<agent::AgentToolEventRecord> tools;
    error = store.load_session(1, loaded, messages, tools);
    check(error.ok(), "load session: " + error.message);
    check(loaded.status == "idle" || loaded.status == "success", "loaded status");
    check(messages.size() >= 3, "messages stored");
    check(tools.size() == 1 && tools[0].tool_name == "read_file" && tools[0].ok, "one tool event");

    const std::size_t transcript_size_before_compact = messages.size();
    // Compaction records a summary but preserves the complete transcript.
    error = store.compact_with_summary("summary of earlier work", 2);
    check(error.ok(), "compact: " + error.message);
    messages.clear();
    error = store.load_messages(messages, 0);
    check(error.ok() && !messages.empty(), "load after compact");
    bool saw_summary = false;
    for (const auto& m : messages) {
        if (m.role == "summary") {
            saw_summary = true;
            break;
        }
    }
    check(saw_summary, "summary present after compact");
    check(messages.size() == transcript_size_before_compact + 1,
          "compaction preserves every original transcript row");

    // Re-open existing DB — still singleton.
    store.close();
    agent::AgentSessionStore again;
    error = again.open(workspace);
    check(error.ok(), "reopen: " + error.message);
    agent::AgentProjectRecord again_project;
    error = again.open_project(again_project);
    check(error.ok() && again_project.id == 1, "singleton survives reopen");
    check(again_project.base_url == "https://example.test/v1",
          "agent project base URL survives reopen");
    check(again_project.settings_json == R"({"reasoning":"high","temperature":0.25})",
          "agent project request settings survive reopen");

    cli::Options restored_options;
    bool restored = false;
    error = agent::restore_project_settings(workspace, restored_options, restored);
    check(error.ok() && restored, "restore existing agent project settings");
    check(restored_options.provider == "openai" &&
              restored_options.model == "test-model" &&
              restored_options.base_url == "https://example.test/v1",
          "restored provider, model, and base URL");
    check(restored_options.reasoning.kind == ReasoningSelectionKind::Named &&
              restored_options.reasoning.value == "high" &&
              restored_options.has_temperature &&
              restored_options.temperature == 0.25,
          "restored reasoning and generation settings");
    check(restored_options.agent_project_settings_restored,
          "restored project suppresses startup provider picker");

    const char* override_argv[] = {
        "ainiux", "-a", "--provider", "deepseek", "-m", "cli-model",
        "--reasoning", "low"};
    cli::ParseResult overridden =
        cli::parse_args(8, const_cast<char**>(override_argv), restored_options);
    check(overridden.error.ok(), "parse explicit agent project overrides");
    check(overridden.options.provider == "deepseek" &&
              overridden.options.model == "cli-model" &&
              overridden.options.reasoning.kind == ReasoningSelectionKind::Named &&
              overridden.options.reasoning.value == "low",
          "explicit CLI provider, model, and reasoning override restored values");

    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_restore_does_not_create_new_project_state() {
    const std::string workspace = temp_workspace("restore-new");
    cli::Options options;
    bool restored = true;
    const Error error = agent::restore_project_settings(workspace, options, restored);
    check(error.ok() && !restored, "new project has no settings to restore");
    check(!fs::exists(fs::path(workspace) / ".ainiux-pr"),
          "settings probe does not create .ainiux-pr for a new project");
    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_record_and_load_approvals() {
    const std::string workspace = temp_workspace("approvals");
    agent::AgentSessionStore store;
    Error error = store.open(workspace);
    check(error.ok(), "open for approvals: " + error.message);

    agent::AgentProjectRecord project;
    project.workspace = workspace;
    project.status = "running";
    error = store.open_project(project);
    check(error.ok(), "open project for approvals");

    agent::AgentApprovalRecord row;
    row.tool_name = "run_command";
    row.command_preview = "git reset --hard";
    row.rule_id = "ask_on_destructive_git";
    row.decision = "allow";
    row.source = "interactive";
    row.message = "user allowed once";
    error = store.record_approval(row);
    check(error.ok(), "record approval: " + error.message);

    row.decision = "deny";
    row.command_preview = "rm -rf build";
    row.rule_id = "ask_on_recursive_force_delete";
    row.message = "user denied";
    error = store.record_approval(row);
    check(error.ok(), "record second approval");

    std::vector<agent::AgentApprovalRecord> loaded;
    error = store.load_approvals(loaded, 0);
    check(error.ok() && loaded.size() == 2, "load two approvals: " + error.message);
    check(loaded[0].decision == "allow" && loaded[0].rule_id == "ask_on_destructive_git",
          "first approval content");
    check(loaded[1].decision == "deny" && loaded[1].tool_name == "run_command",
          "second approval content");

    // Soft-add path: reopen and record again.
    store.close();
    agent::AgentSessionStore again;
    check(again.open(workspace).ok(), "reopen approvals db");
    row.decision = "cancelled";
    row.command_preview = "remove data.sqlite";
    row.rule_id = "ask_on_database_delete";
    row.tool_name = "remove";
    check(again.record_approval(row).ok(), "record after reopen");
    loaded.clear();
    check(again.load_approvals(loaded).ok() && loaded.size() == 3, "three approvals total");

    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_peek_last_message() {
    const std::string workspace = temp_workspace("peek-last");
    agent::AgentSessionStore store;
    Error error = store.open(workspace);
    check(error.ok(), "open store for peek: " + error.message);
    agent::AgentMessageRecord last;
    bool found = true;
    error = store.peek_last_message(last, found);
    check(error.ok() && !found, "empty transcript peeks as not found");
    check(store.append_message("notice", "first").ok(), "append first");
    check(store.append_message("user", "hello").ok(), "append user");
    check(store.append_message("notice", "second").ok(), "append second notice");
    error = store.peek_last_message(last, found);
    check(error.ok() && found && last.role == "notice" && last.content == "second",
          "peek returns newest message by seq");
    store.close();
    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_permission_settings_json() {
    agent::PermissionMode mode = agent::PermissionMode::Yolo;
    Error error = agent::permission_mode_from_settings_json("{}", mode);
    check(error.ok() && mode == agent::PermissionMode::Smart,
          "legacy project settings default permissions to smart");
    check(agent::parse_permission_mode("confirm", mode) &&
              mode == agent::PermissionMode::Confirm &&
              std::string(agent::permission_mode_name(mode)) == "confirm",
          "permission mode parses and formats confirm");
    check(agent::parse_permission_mode("yolo", mode) &&
              mode == agent::PermissionMode::Yolo,
          "permission mode parses yolo");
    check(!agent::parse_permission_mode("allow-all", mode),
          "permission mode rejects non-canonical aliases");

    std::string encoded;
    error = agent::settings_json_with_permission_mode(
        R"({"reasoning":"high"})", agent::PermissionMode::Yolo, encoded);
    check(error.ok() && encoded.find("\"permission_mode\":\"yolo\"") !=
                            std::string::npos &&
              encoded.find("\"reasoning\":\"high\"") != std::string::npos,
          "permission mode merges into existing settings JSON");
    mode = agent::PermissionMode::Smart;
    error = agent::permission_mode_from_settings_json(encoded, mode);
    check(error.ok() && mode == agent::PermissionMode::Yolo,
          "permission mode settings JSON round trip");
}

void test_goal_settings_json_and_control() {
    agent::SessionGoal goal;
    Error error = agent::goal_from_settings_json("{}", goal);
    check(error.ok() && goal.status == agent::GoalStatus::Cleared &&
              goal.condition.empty(),
          "missing goal defaults to cleared");
    check(!agent::goal_is_active(goal), "cleared goal is not active");
    check(agent::agent_goal_control(goal).empty(),
          "inactive goal injects no control fragment");

    goal.condition = "create file X containing Y";
    goal.status = agent::GoalStatus::Active;
    goal.turns = 2;
    goal.last_reason = "partial";
    std::string encoded;
    error = agent::settings_json_with_goal(R"({"permission_mode":"smart"})", goal,
                                           encoded);
    check(error.ok() && encoded.find("\"status\":\"active\"") != std::string::npos &&
              encoded.find("create file X") != std::string::npos &&
              encoded.find("\"permission_mode\":\"smart\"") != std::string::npos,
          "goal merges into existing settings JSON");

    agent::SessionGoal loaded;
    error = agent::goal_from_settings_json(encoded, loaded);
    check(error.ok() && loaded.status == agent::GoalStatus::Active &&
              loaded.condition == goal.condition && loaded.turns == 2 &&
              loaded.last_reason == "partial",
          "goal settings JSON round trip");
    check(agent::goal_is_active(loaded), "active goal with condition is active");
    const std::string control = agent::agent_goal_control(loaded);
    check(control.find("ACTIVE GOAL: create file X containing Y") != std::string::npos &&
              control.find("goal_met") != std::string::npos &&
              control.find("Do not invent evidence") != std::string::npos,
          "active goal control fragment matches the v1 prompt contract");
    check(agent::format_goal_status(loaded).find("active") != std::string::npos,
          "format_goal_status reports active status");

    loaded.status = agent::GoalStatus::Paused;
    check(!agent::goal_is_active(loaded) && agent::agent_goal_control(loaded).empty(),
          "paused goal strips the active control fragment");
}

}  // namespace

void run_all() {
    test_open_singleton_append_compact_load();
    test_restore_does_not_create_new_project_state();
    test_record_and_load_approvals();
    test_peek_last_message();
    test_permission_settings_json();
    test_goal_settings_json_and_control();
}

}  // namespace ainiux::test::agent_session_store
