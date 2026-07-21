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

    // Compact keeps recent tail.
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

    // Re-open existing DB — still singleton.
    store.close();
    agent::AgentSessionStore again;
    error = again.open(workspace);
    check(error.ok(), "reopen: " + error.message);
    agent::AgentProjectRecord again_project;
    error = again.open_project(again_project);
    check(error.ok() && again_project.id == 1, "singleton survives reopen");

    std::error_code ec;
    fs::remove_all(workspace, ec);
}

}  // namespace

void run_all() { test_open_singleton_append_compact_load(); }

}  // namespace ainiux::test::agent_session_store
