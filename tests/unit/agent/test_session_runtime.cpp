#include "agent/test_session_runtime.hpp"

#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <unistd.h>

#include "agent/session_runtime.hpp"
#include "app/app.hpp"
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
    options.task_mode = agent::AgentTaskMode::Act;
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

void test_agent_token_usage_aggregation_is_bounded() {
    provider::ChatResult absent;
    agent::AgentTokenUsage usage;
    agent::accumulate_agent_token_usage(absent, usage);
    check(usage.reported_rounds == 0 && usage.input_tokens == 0,
          "absent provider usage does not create an agent usage round");

    provider::ChatResult first;
    first.usage_json = "{}";
    first.prompt_tokens = 100;
    first.fresh_prompt_tokens = 20;
    first.cache_read_tokens = 80;
    first.cache_write_tokens = 5;
    first.completion_tokens = 7;
    agent::accumulate_agent_token_usage(first, usage);
    check(usage.reported_rounds == 1 && usage.input_tokens == 100 &&
              usage.fresh_input_tokens == 20 && usage.cache_read_tokens == 80 &&
              usage.cache_write_tokens == 5 && usage.output_tokens == 7,
          "agent usage aggregates normalized input/cache/output metrics");

    provider::ChatResult overflow = first;
    overflow.prompt_tokens = std::numeric_limits<long long>::max();
    overflow.fresh_prompt_tokens = std::numeric_limits<long long>::max();
    overflow.cache_read_tokens = std::numeric_limits<long long>::max();
    overflow.cache_write_tokens = std::numeric_limits<long long>::max();
    overflow.completion_tokens = std::numeric_limits<long long>::max();
    agent::accumulate_agent_token_usage(overflow, usage);
    check(usage.input_tokens == std::numeric_limits<long long>::max() &&
              usage.fresh_input_tokens == std::numeric_limits<long long>::max() &&
              usage.cache_read_tokens == std::numeric_limits<long long>::max() &&
              usage.cache_write_tokens == std::numeric_limits<long long>::max() &&
              usage.output_tokens == std::numeric_limits<long long>::max(),
          "agent usage aggregation saturates instead of overflowing");

    provider::ChatResult estimated;
    estimated.usage_json = "null";
    estimated.completion_tokens = 11;
    estimated.completion_tokens_estimated = true;
    agent::AgentTokenUsage fallback;
    agent::accumulate_agent_token_usage(estimated, fallback, 250, 11);
    check(fallback.reported_rounds == 1 && fallback.input_tokens == 250 &&
              fallback.output_tokens == 11 && fallback.input_estimated &&
              fallback.output_estimated,
          "agent usage falls back to per-round input and output estimates");

    app::AgentGoalResult run;
    run.token_usage = fallback;
    run.elapsed_ms = 6540;
    check(app::format_agent_run_metrics(run) ==
              "Agent metrics: input 250 tokens (estimated), output 11 tokens (estimated), time 6.54 s",
          "one-shot agent metrics format includes input, output, estimate labels, and time");
}

void test_task_mode_switch_is_session_scoped_and_failure_safe() {
    const std::string workspace = temp_workspace("task-mode");
    agent::AgentSessionRuntime runtime;
    agent::SessionRuntimeOptions options;
    options.workspace = workspace;
    options.enable_session_db = false;
    options.enable_agent_log = false;
    provider::RequestContext context = offline_context(workspace);
    Error error = runtime.prepare(context, {}, {}, options);
    check(error.ok() && runtime.task_mode() == agent::AgentTaskMode::Act &&
              runtime.mutation_policy() == agent::MutationPolicy::Full,
          "agent runtime defaults to Act with full mutation policy");

    error = runtime.switch_task_mode(agent::AgentTaskMode::Plan);
    check(error.ok() && runtime.task_mode() == agent::AgentTaskMode::Plan &&
              runtime.mutation_policy() == agent::MutationPolicy::PlanningDocuments,
          "Act to Plan replaces the tool policy");
    check(runtime.switch_task_mode(agent::AgentTaskMode::Plan).ok(),
          "repeated Plan switch is idempotent");

    std::error_code ec;
    fs::create_symlink("src/hello.cpp", fs::path(workspace) / "AGENTS.md", ec);
    error = runtime.switch_task_mode(agent::AgentTaskMode::Act);
    check(!error.ok() && runtime.task_mode() == agent::AgentTaskMode::Plan &&
              runtime.mutation_policy() == agent::MutationPolicy::PlanningDocuments,
          "AGENTS reload failure leaves the prior task mode and policy intact");
    fs::remove(fs::path(workspace) / "AGENTS.md", ec);
    error = runtime.switch_task_mode(agent::AgentTaskMode::Act);
    check(error.ok() && runtime.task_mode() == agent::AgentTaskMode::Act &&
              runtime.mutation_policy() == agent::MutationPolicy::Full,
          "Plan to Act succeeds after AGENTS reload is valid");

    runtime.reset();
    fs::remove_all(workspace, ec);
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
        check(store.append_message("thinking", "Thinking: inspect callers").ok(),
              "seed thinking preview");
        check(store.append_message("tool",
                                   "1: read_file(\"src/hello.cpp\") → ok in 15 ms",
                                   "read_file",
                                   true, "\"src/hello.cpp\"")
                  .ok(),
              "seed tool");
        check(store.append_message("assistant", "all done").ok(), "seed assistant");
        store.close();
    }

    agent::AgentSessionRuntime runtime;
    agent::SessionRuntimeOptions options;
    options.workspace = workspace;
    options.task_mode = agent::AgentTaskMode::Act;
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
    bool saw_thinking = false;
    for (const auto& message : display) {
        if (message.role == "tool" &&
            message.content.find("in 15 ms") != std::string::npos)
            saw_tool = true;
        if (message.role == "assistant" && message.content.find("all done") != std::string::npos)
            saw_assistant = true;
        if (message.role == "thinking" &&
            message.content.find("inspect callers") != std::string::npos)
            saw_thinking = true;
    }
    check(saw_tool, "persisted timed tool activity restored for UI");
    check(saw_assistant, "assistant reply restored for UI");
    check(saw_thinking, "persisted thinking preview restored for UI");

    runtime.reset();
    fs::current_path(previous);
    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_manual_compaction_preserves_transcript_and_noops_until_new_history() {
    const std::string workspace = temp_workspace("manual-compact");
    {
        agent::AgentSessionStore store;
        check(store.open(workspace).ok(), "compact seed open");
        agent::AgentProjectRecord project;
        project.workspace = workspace;
        project.status = "idle";
        check(store.open_project(project).ok(), "compact seed project");
        for (int index = 0; index < 15; ++index) {
            check(store.append_message(index % 2 == 0 ? "user" : "assistant",
                                       "stored message " + std::to_string(index))
                      .ok(),
                  "compact seed message");
        }
    }

    agent::AgentSessionRuntime runtime;
    agent::SessionRuntimeOptions options;
    options.workspace = workspace;
    options.enable_session_db = true;
    options.enable_agent_log = false;
    options.auto_compact = true;
    provider::RequestContext context = offline_context(workspace);
    check(runtime.prepare(context, {}, {}, options).ok(), "prepare manual compact runtime");

    const agent::SessionCompactionResult compacted =
        runtime.compact(context, agent::CompactionReason::Manual);
    check(compacted.error.ok() && compacted.compacted && !compacted.no_op,
          "manual compact runs below automatic threshold");

    std::vector<provider::Message> display;
    check(runtime.load_display_messages(display).ok(), "load transcript after manual compact");
    check(display.size() == 16, "manual compact preserves 15 rows and appends one summary event");
    check(display.back().role == "summary", "manual compact persists summary event");

    const agent::SessionCompactionResult repeated =
        runtime.compact(context, agent::CompactionReason::Manual);
    check(repeated.error.ok() && repeated.no_op && !repeated.compacted,
          "manual compact without new compactable history is a no-op");

    runtime::CancellationSource cancelled;
    cancelled.cancel();
    const agent::SessionCompactionResult cancelled_result =
        runtime.compact(context, agent::CompactionReason::Manual, cancelled.token());
    check(cancelled_result.error.code == ErrorCode::Cancelled,
          "manual compaction observes cancellation");

    runtime.reset();
    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_project_replacement_resets_exact_state_and_switches_workspace() {
    const std::string workspace = temp_workspace("replace-source");
    const fs::path keep_file = fs::path(workspace) / "keep.txt";
    {
        std::ofstream out(keep_file);
        out << "preserve me";
    }
    {
        agent::AgentSessionStore store;
        check(store.open(workspace).ok(), "replace seed open");
        agent::AgentProjectRecord project;
        project.workspace = workspace;
        check(store.open_project(project).ok(), "replace seed project");
        check(store.append_message("user", "old project history").ok(),
              "replace seed history");
    }

    agent::AgentSessionRuntime runtime;
    agent::SessionRuntimeOptions options;
    options.workspace = workspace;
    options.enable_session_db = true;
    options.enable_agent_log = false;
    provider::RequestContext context = offline_context(workspace);
    check(runtime.prepare(context, {}, {}, options).ok(), "prepare replace source");

    agent::NewProjectTarget same;
    check(agent::resolve_new_project_target(workspace, "", same).ok() &&
              same.state_dir_exists,
          "same-root replacement target requires confirmed state removal");
    const agent::SessionProjectReplaceResult reset =
        runtime.replace_project(context, same);
    check(reset.error.ok() && runtime.prepared() && runtime.workspace() == same.root,
          "same-root /new reinitializes and keeps runtime prepared");
    check(fs::exists(keep_file), "same-root /new preserves non-agent project files");
    std::vector<provider::Message> display;
    check(runtime.load_display_messages(display).ok() && display.empty(),
          "same-root /new clears project transcript");

    const fs::path sibling = fs::path(workspace).parent_path() /
                             (fs::path(workspace).filename().string() + "-switched");
    std::error_code ec;
    fs::remove_all(sibling, ec);
    agent::NewProjectTarget switched;
    check(agent::resolve_new_project_target(runtime.workspace(), sibling.string(), switched).ok() &&
              !switched.root_exists,
          "switch target permits missing final component");
    const agent::SessionProjectReplaceResult switch_result =
        runtime.replace_project(context, switched);
    check(switch_result.error.ok() && runtime.workspace() == switched.root &&
              fs::is_directory(sibling / ".ainiux-pr"),
          "/new creates and switches to a sibling project");
    check(context.options.model == "test", "/new retains selected model settings");
    check(fs::exists(keep_file), "switching projects leaves prior workspace contents untouched");

    runtime.reset();
    fs::remove_all(workspace, ec);
    fs::remove_all(sibling, ec);
}

void test_project_replacement_failure_reopens_prior_project() {
    const std::string workspace = temp_workspace("replace-rollback");
    agent::AgentSessionRuntime runtime;
    agent::SessionRuntimeOptions options;
    options.workspace = workspace;
    options.enable_session_db = true;
    options.enable_agent_log = false;
    provider::RequestContext context = offline_context(workspace);
    check(runtime.prepare(context, {}, {}, options).ok(), "prepare rollback source");

    const fs::path nested = fs::path(workspace) / "nested";
    fs::create_directories(nested);
    agent::NewProjectTarget target;
    check(agent::resolve_new_project_target(workspace, "nested", target).ok(),
          "resolve nested rollback target");
    const agent::SessionProjectReplaceResult replaced =
        runtime.replace_project(context, target);
    check(!replaced.error.ok(), "nested target initialization fails under parent project");
    check(runtime.prepared() && runtime.workspace() == fs::canonical(workspace).generic_string(),
          "failed /new reopens the prior project");
    check(!fs::exists(nested / ".ainiux-pr"),
          "failed initialization rolls back target agent state");

    runtime.reset();
    std::error_code ec;
    fs::remove_all(workspace, ec);
}

}  // namespace

void run_all() {
    test_prepare_opens_session_db_and_tools();
    test_empty_turn_rejected_when_unprepared();
    test_agent_token_usage_aggregation_is_bounded();
    test_task_mode_switch_is_session_scoped_and_failure_safe();
    test_prepare_loads_existing_display_history();
    test_manual_compaction_preserves_transcript_and_noops_until_new_history();
    test_project_replacement_resets_exact_state_and_switches_workspace();
    test_project_replacement_failure_reopens_prior_project();
}

}  // namespace ainiux::test::agent_session_runtime
