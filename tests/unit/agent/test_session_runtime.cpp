#include "agent/test_session_runtime.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "agent/session_runtime.hpp"
#include "cli/args.hpp"
#include "provider/provider.hpp"
#include "support/test_support.hpp"

namespace ainiux::test::agent_session_runtime {
namespace {
using ainiux::test::check;
namespace fs = std::filesystem;

std::string temp_workspace(const std::string& name) {
    const fs::path root =
        fs::temp_directory_path() / ("ainiux-agent-runtime-" + name + "-" +
                                     std::to_string(static_cast<long long>(::getpid())));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    // Minimal source file so the index has something to discover.
    fs::create_directories(root / "src", ec);
    {
        std::ofstream out(root / "src" / "hello.cpp");
        out << "int main() { return 0; }\n";
    }
    return root.string();
}

provider::RequestContext offline_context(const std::string& workspace) {
    (void)workspace;
    provider::RequestContext context;
    context.options.provider = "none";
    context.options.model = "test";
    context.options.quiet = true;
    context.options.agent_log_enabled = false;
    context.profile.name = "none";
    context.profile.offline = true;
    return context;
}

void test_prepare_opens_session_db_and_tools() {
    const std::string workspace = temp_workspace("prepare");
    const std::string previous = fs::current_path().string();
    fs::current_path(workspace);

    agent::AgentSessionRuntime runtime;
    agent::SessionRuntimeOptions options;
    options.workspace = workspace;
    options.allow_mutations = true;
    options.interactive = true;
    options.enable_session_db = true;
    options.enable_agent_log = false;
    options.max_source_code_file_size = 1024 * 1024;
    options.history_backup.enabled = true;
    options.history_backup.max_bytes = 1024 * 1024;
    options.history_backup.ttl_days = 7;

    provider::RequestContext context = offline_context(workspace);
    // prepare does not need a live model; tool registry + index only.
    Error error = runtime.prepare(context, {}, {}, options);
    check(error.ok(), "prepare agent runtime: " + error.message);
    check(runtime.prepared(), "runtime reports prepared");
    check(runtime.session_db_path().find("agent.sqlite") != std::string::npos,
          "session db path under .ainiux-pr");
    check(fs::exists(runtime.session_db_path()), "agent.sqlite created on prepare");
    check(runtime.session_id() == 0, "session id deferred until first user turn");

    // Clean finish without a turn.
    error = runtime.finish_session("success");
    check(error.ok(), "finish without session is ok");
    runtime.reset();
    check(!runtime.prepared(), "reset clears prepared");

    fs::current_path(previous);
    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_empty_turn_rejected_when_unprepared() {
    agent::AgentSessionRuntime runtime;
    provider::RequestContext context = offline_context(".");
    agent::SessionTurnResult turn = runtime.run_user_turn(context, "hello");
    check(!turn.error.ok(), "unprepared runtime rejects turns");
}

void test_prepare_loads_existing_display_history() {
    const std::string workspace = temp_workspace("history");
    const std::string previous = fs::current_path().string();
    fs::current_path(workspace);

    // Seed agent.sqlite as if a prior agent run completed.
    {
        agent::AgentSessionStore store;
        check(store.open(workspace).ok(), "seed open store");
        agent::AgentProjectRecord project;
        project.workspace = workspace;
        project.status = "idle";
        project.provider = "none";
        project.model = "test";
        check(store.open_project(project).ok(), "seed project");
        check(store.append_message("user", "earlier goal").ok(), "seed user");
        check(store.append_message("tool", "1: read_file(\"src/hello.cpp\") → ok", "read_file",
                                   true, "\"src/hello.cpp\"")
                  .ok(),
              "seed tool");
        check(store.append_message("assistant", "all done").ok(), "seed assistant");
        store.close();
    }

    agent::AgentSessionRuntime runtime;
    agent::SessionRuntimeOptions options;
    options.workspace = workspace;
    options.allow_mutations = true;
    options.interactive = true;
    options.enable_session_db = true;
    options.enable_agent_log = false;
    options.max_source_code_file_size = 1024 * 1024;

    provider::RequestContext context = offline_context(workspace);
    Error error = runtime.prepare(context, {}, {}, options);
    check(error.ok(), "prepare with existing history: " + error.message);

    std::vector<provider::Message> display;
    error = runtime.load_display_messages(display);
    check(error.ok(), "load_display_messages: " + error.message);
    check(display.size() >= 3, "history has at least user/tool/assistant");
    check(display[0].role == "user" && display[0].content.find("earlier goal") != std::string::npos,
          "first message is prior user goal");
    bool saw_tool = false;
    bool saw_assistant = false;
    for (const auto& message : display) {
        if (message.role == "tool") saw_tool = true;
        if (message.role == "assistant" && message.content.find("all done") != std::string::npos)
            saw_assistant = true;
    }
    check(saw_tool, "tool activity restored for UI");
    check(saw_assistant, "assistant reply restored for UI");

    runtime.reset();
    fs::current_path(previous);
    std::error_code ec;
    fs::remove_all(workspace, ec);
}

}  // namespace

void run_all() {
    test_prepare_opens_session_db_and_tools();
    test_empty_turn_rejected_when_unprepared();
    test_prepare_loads_existing_display_history();
}

}  // namespace ainiux::test::agent_session_runtime
