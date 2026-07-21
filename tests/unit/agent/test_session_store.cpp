#include "agent/test_session_store.hpp"

#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

#include "agent/session_store.hpp"
#include "support/test_support.hpp"

namespace ainiux::test::agent_session_store {
namespace {
using ainiux::test::check;
namespace fs = std::filesystem;

std::string temp_workspace(const std::string& name) {
    const fs::path root =
        fs::temp_directory_path() / ("ainiux-agent-session-" + name + "-" +
                                     std::to_string(static_cast<long long>(::getpid())));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    return root.string();
}

void test_open_create_append_finish_load() {
    const std::string workspace = temp_workspace("basic");
    agent::AgentSessionStore store;
    Error error = store.open(workspace);
    check(error.ok(), "open agent.sqlite: " + error.message);
    check(store.is_open(), "store is open");
    check(fs::exists(store.path()), "agent.sqlite exists on disk");
    check(store.path().find(".ainiux") != std::string::npos &&
              store.path().find("agent.sqlite") != std::string::npos,
          "path is project-local .ainiux/agent.sqlite");

    agent::AgentSessionRecord session;
    session.goal = "fix the thing";
    session.provider = "openai";
    session.model = "test-model";
    session.api = "chat";
    session.protocol = "native";
    session.workspace = workspace;
    session.run_id = "run-1";
    error = store.create_session(session);
    check(error.ok() && session.id > 0, "create session: " + error.message);
    check(session.status == "running", "default status running");

    error = store.append_message(session.id, "user", "fix the thing");
    check(error.ok(), "append user message");
    error = store.append_tool_event(session.id, 1, "call-1", "read_file",
                                    R"({"path":"a.cpp"})", R"({"ok":true})", true);
    check(error.ok(), "append tool event");
    error = store.append_message(session.id, "assistant", "done");
    check(error.ok(), "append assistant");

    error = store.finish_session(session.id, "success", "done", "", "", 2, 1);
    check(error.ok(), "finish session");

    agent::AgentSessionRecord loaded;
    std::vector<agent::AgentMessageRecord> messages;
    std::vector<agent::AgentToolEventRecord> tools;
    error = store.load_session(session.id, loaded, messages, tools);
    check(error.ok(), "load session: " + error.message);
    check(loaded.status == "success" && loaded.goal == "fix the thing", "loaded status/goal");
    check(loaded.turns == 2 && loaded.tool_calls == 1, "loaded counters");
    check(loaded.provider == "openai" && loaded.model == "test-model", "loaded provider/model");
    check(messages.size() == 2, "two messages");
    check(messages[0].role == "user" && messages[1].role == "assistant", "message roles");
    check(tools.size() == 1 && tools[0].tool_name == "read_file" && tools[0].ok, "one tool event");
    check(messages[0].seq == 1 && tools[0].seq == 2 && messages[1].seq == 3,
          "global seq ordering across messages and tools");

    std::vector<agent::AgentSessionRecord> listed;
    error = store.list_sessions(listed, 10);
    check(error.ok() && listed.size() == 1 && listed[0].id == session.id, "list sessions");

    // Re-open existing DB.
    store.close();
    agent::AgentSessionStore again;
    check(again.open(workspace).ok(), "reopen existing agent.sqlite");
    listed.clear();
    check(again.list_sessions(listed, 10).ok() && listed.size() == 1, "persist across reopen");

    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_never_uses_home_db_path() {
    const std::string workspace = temp_workspace("path");
    const std::string path = agent::AgentSessionStore::database_path(workspace);
    check(path.find(workspace) != std::string::npos, "path under workspace");
    check(path.find("ainiux.db") == std::string::npos, "not central chat DB name");
    check(path.find("agent.sqlite") != std::string::npos, "agent.sqlite name");
    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_missing_session() {
    const std::string workspace = temp_workspace("missing");
    agent::AgentSessionStore store;
    check(store.open(workspace).ok(), "open");
    agent::AgentSessionRecord session;
    std::vector<agent::AgentMessageRecord> messages;
    std::vector<agent::AgentToolEventRecord> tools;
    const Error error = store.load_session(999, session, messages, tools);
    check(!error.ok() && error.code == ErrorCode::FileRead, "missing session errors");
    std::error_code ec;
    fs::remove_all(workspace, ec);
}

}  // namespace

void run_all() {
    test_open_create_append_finish_load();
    test_never_uses_home_db_path();
    test_missing_session();
}

}  // namespace ainiux::test::agent_session_store
