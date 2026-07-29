#include "agent/test_session_runtime.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>
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
    std::vector<agent::PreparationProgress> preparation;
    options.on_prepare_progress =
        [&](const agent::PreparationProgress& progress) {
            preparation.push_back(progress);
        };

    provider::RequestContext context = offline_context(workspace);
    // prepare is local-only and must succeed on the offline "none" profile.
    // Interactive Agent temporarily uses that profile while the startup
    // provider picker is open; gating prepare on a live provider leaves the
    // TUI stuck on "Agent preparing" forever.
    Error error = runtime.prepare(context, {}, {}, options);
    check(error.ok(), "prepare agent runtime offline: " + error.message);
    check(runtime.prepared(), "runtime reports prepared");
    check(runtime.session_db_path().find("agent.sqlite") != std::string::npos,
          "session db path under .ainiux-pr");
    check(fs::exists(runtime.session_db_path()), "agent.sqlite created on prepare");
    check(runtime.session_id() == 0, "session id deferred until first user turn");
    std::vector<agent::PreparationPhase> completed_phases;
    for (const agent::PreparationProgress& progress : preparation) {
        if (progress.completed) completed_phases.push_back(progress.phase);
    }
    check(completed_phases ==
              std::vector<agent::PreparationPhase>{
                  agent::PreparationPhase::IndexProbe,
                  agent::PreparationPhase::ToolSetup,
                  agent::PreparationPhase::SessionDatabase,
                  agent::PreparationPhase::History,
                  agent::PreparationPhase::ProjectInstructions},
          "prepare publishes every lifecycle phase in order");
    bool nonnegative_elapsed = true;
    for (const agent::PreparationProgress& progress : preparation)
        nonnegative_elapsed =
            nonnegative_elapsed && progress.phase_elapsed_ms >= 0 &&
            progress.total_elapsed_ms >= 0;
    check(nonnegative_elapsed, "prepare phase events include elapsed time");

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

void test_prepare_with_indexing_disabled_never_touches_index_database() {
    const std::string workspace = temp_workspace("indexing-off");
    const fs::path database =
        fs::path(workspace) / ".ainiux-pr" / "index.sqlite";
    fs::create_directories(database.parent_path());
    {
        std::ofstream out(database, std::ios::binary);
        out << "sentinel-index-bytes";
    }
    const auto before_size = fs::file_size(database);
    const auto before_time = fs::last_write_time(database);

    agent::AgentSessionRuntime runtime;
    agent::SessionRuntimeOptions options;
    options.workspace = workspace;
    options.interactive = true;
    options.enable_session_db = true;
    options.enable_agent_log = false;
    options.index_mode =
        agent::SessionRuntimeOptions::IndexMode::Disabled;
    provider::RequestContext context = offline_context(workspace);
    const Error error = runtime.prepare(context, {}, {}, options);
    check(error.ok() && runtime.prepared(),
          "agent runtime prepares without a code index: " + error.message);
    check(fs::file_size(database) == before_size &&
              fs::last_write_time(database) == before_time,
          "disabled preparation leaves a pre-existing index database untouched");
    check(fs::exists(fs::path(workspace) / ".ainiux-pr" / "agent.sqlite"),
          "disabled indexing retains project session persistence");
    const agent::SessionIndexReportResult disabled_report =
        runtime.show_index(true);
    check(!disabled_report.error.ok() &&
              disabled_report.error.code == ErrorCode::UnsupportedFeature &&
              disabled_report.error.message.find("/index-code") !=
                  std::string::npos,
          "show-index explains that this Agent session has indexing disabled");

    runtime.reset();
    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_index_code_enables_a_skipped_index_in_place() {
    const std::string workspace = temp_workspace("index-code");
    const fs::path database =
        fs::path(workspace) / ".ainiux-pr" / "index.sqlite";
    std::atomic<int> progress_updates{0};
    agent::SessionRuntimeOptions options;
    options.workspace = workspace;
    options.interactive = true;
    options.enable_session_db = true;
    options.enable_agent_log = false;
    options.index_mode =
        agent::SessionRuntimeOptions::IndexMode::Disabled;
    options.on_index_progress =
        [&](const agent::index::Progress&) { ++progress_updates; };
    provider::RequestContext context = offline_context(workspace);
    agent::AgentSessionRuntime runtime;
    Error error = runtime.prepare(context, {}, {}, options);
    check(error.ok() && !runtime.indexing_enabled() &&
              !fs::exists(database),
          "skipping startup indexing keeps the live Agent session unindexed");

    ainiux::runtime::CancellationSource cancellation;
    cancellation.cancel();
    const agent::SessionIndexReportResult cancelled =
        runtime.index_code(cancellation.token());
    agent::index::Options probe_options;
    probe_options.workspace = workspace;
    agent::index::ProbeResult probe;
    const Error probe_error = agent::index::probe(probe_options, probe);
    check(!cancelled.error.ok() &&
              cancelled.error.code == ErrorCode::Cancelled &&
              !runtime.indexing_enabled() && probe_error.ok() &&
              probe.state != agent::index::ProbeState::Completed,
          "cancelled index-code leaves the live registry and completed snapshot unchanged");

    const agent::SessionIndexReportResult created =
        runtime.index_code();
    check(created.error.ok() && created.created &&
              created.indexing_enabled && runtime.indexing_enabled() &&
              fs::exists(database) && progress_updates.load() > 0 &&
              created.elapsed_ms >= 0 &&
              created.markdown.find(
                  "| **All languages** | **1** |") !=
                  std::string::npos,
          "index-code creates and enables the index without restarting Agent");

    {
        std::ofstream out(fs::path(workspace) / "src" / "later.py");
        out << "def later():\n    return 1\n";
    }
    const int progress_before = progress_updates.load();
    const agent::SessionIndexReportResult refreshed =
        runtime.show_index(true);
    check(refreshed.error.ok() && !refreshed.created &&
              refreshed.indexing_enabled &&
              refreshed.markdown.find(
                  "| **All languages** | **2** |") !=
                  std::string::npos &&
              progress_updates.load() > progress_before,
          "enabled session uses the task-end incremental refresh path for new files");

    std::vector<provider::Message> display;
    check(runtime.load_display_messages(display).ok() &&
              display.size() == 2 && display.front().role == "index" &&
              display.back().role == "index",
          "index-code and later refresh reports persist in display-only history");

    runtime.reset();
    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_index_report_refreshes_and_stays_display_only() {
    const std::string workspace = temp_workspace("index-report");
    std::atomic<int> progress_updates{0};
    agent::SessionRuntimeOptions options;
    options.workspace = workspace;
    options.interactive = true;
    options.enable_session_db = true;
    options.enable_agent_log = false;
    options.index_mode =
        agent::SessionRuntimeOptions::IndexMode::UseExistingLazy;
    options.on_index_progress =
        [&](const agent::index::Progress&) { ++progress_updates; };
    provider::RequestContext context = offline_context(workspace);
    agent::index::Options initial_index_options;
    initial_index_options.workspace = workspace;
    agent::index::RefreshStats initial_index_stats;
    Error error = agent::index::refresh(initial_index_options,
                                        initial_index_stats);
    check(error.ok(), "create index fixture for compact report");
    progress_updates.store(0);
    agent::AgentSessionRuntime runtime;
    error = runtime.prepare(context, {}, {}, options);
    check(error.ok(), "prepare indexed runtime for compact report: " +
                          error.message);

    const agent::SessionIndexReportResult initial =
        runtime.show_index(false);
    check(initial.error.ok() &&
              initial.markdown.find("| Language | Files | Lines of code |") ==
                  0 &&
              initial.markdown.find("# ainiux Code Index") ==
                  std::string::npos,
          "startup index report contains only the compact totals table");
    std::vector<provider::Message> display;
    check(runtime.load_display_messages(display).ok() &&
              display.size() == 1 && display.front().role == "index" &&
              display.front().content == initial.markdown,
          "index table persists as an unclipped display-only Agent history row");

    {
        std::ofstream out(fs::path(workspace) / "src" / "later.py");
        out << "def later():\n    return 1\n";
    }
    const int progress_before = progress_updates.load();
    const agent::SessionIndexReportResult refreshed =
        runtime.show_index(true);
    check(refreshed.error.ok() &&
              refreshed.markdown.find(
                  "| **All languages** | **2** |") !=
                  std::string::npos &&
              progress_updates.load() > progress_before,
          "show-index refreshes changed files with progress before formatting");
    display.clear();
    check(runtime.load_display_messages(display).ok() &&
              display.size() == 2 && display.back().role == "index" &&
              display.back().content == refreshed.markdown,
          "each explicit show-index appends the current table to Agent history");

    runtime.reset();
    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_optional_index_modes_create_refresh_and_fallback() {
    const std::string missing_workspace = temp_workspace("index-missing");
    const fs::path missing_database =
        fs::path(missing_workspace) / ".ainiux-pr" / "index.sqlite";
    provider::RequestContext context = offline_context(missing_workspace);
    agent::SessionRuntimeOptions options;
    options.workspace = missing_workspace;
    options.enable_session_db = false;
    options.enable_agent_log = false;
    options.index_mode =
        agent::SessionRuntimeOptions::IndexMode::UseExisting;
    agent::AgentSessionRuntime runtime;
    Error error = runtime.prepare(context, {}, {}, options);
    check(error.ok() && runtime.prepared() && !fs::exists(missing_database),
          "headless-style use-existing mode continues without creating a missing index");
    runtime.reset();

    error = runtime.prepare(context, {}, {}, options);
    const agent::SessionIndexReportResult created = runtime.index_code();
    check(error.ok() && created.error.ok() && created.created &&
              fs::exists(missing_database),
          "explicit index-code creates a missing index after Agent is ready");
    runtime.reset();

    {
        std::ofstream out(fs::path(missing_workspace) / "src" / "hello.cpp",
                          std::ios::app);
        out << "int changed() { return 1; }\n";
    }
    options.index_mode =
        agent::SessionRuntimeOptions::IndexMode::UseExistingLazy;
    error = runtime.prepare(context, {}, {}, options);
    const agent::SessionIndexReportResult refreshed =
        runtime.show_index(true);
    ainiux::agent::index::Options index_options;
    index_options.workspace = missing_workspace;
    ainiux::agent::index::Freshness freshness;
    const Error freshness_error =
        ainiux::agent::index::check_freshness(index_options, freshness);
    check(error.ok() && refreshed.error.ok() && freshness_error.ok() &&
              freshness.fresh,
          "use-existing mode refreshes an existing index after readiness");
    runtime.reset();

    const std::string corrupt_workspace = temp_workspace("index-corrupt");
    const fs::path corrupt_database =
        fs::path(corrupt_workspace) / ".ainiux-pr" / "index.sqlite";
    fs::create_directories(corrupt_database.parent_path());
    {
        std::ofstream out(corrupt_database, std::ios::binary);
        out << "not sqlite";
    }
    const auto corrupt_size = fs::file_size(corrupt_database);
    context = offline_context(corrupt_workspace);
    options.workspace = corrupt_workspace;
    error = runtime.prepare(context, {}, {}, options);
    check(error.ok() && runtime.prepared() &&
              fs::file_size(corrupt_database) == corrupt_size,
          "optional corrupt-index failure falls back without rewriting the index");
    runtime.reset();

    std::error_code ec;
    fs::remove_all(missing_workspace, ec);
    ec.clear();
    fs::remove_all(corrupt_workspace, ec);
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
    run.tool_calls = 42;
    run.failed_tool_calls = 3;
    run.elapsed_ms = 6540;
    check(app::format_agent_run_metrics(run) ==
              "Agent metrics: tool calls 42 (3 failed), input 250 tokens (estimated), output 11 tokens (estimated), time 6.54 s",
          "one-shot agent metrics format includes tool failures, tokens, estimate labels, and time");
    run.tool_calls = 0;
    run.failed_tool_calls = 0;
    check(app::format_agent_run_metrics(run).find(
              "tool calls 0 (0 failed)") != std::string::npos,
          "one-shot agent metrics always prints zero tool failures");
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
                                       "stored message " + std::to_string(index) +
                                           std::string(1000, 'x'))
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
    int summary_calls = 0;
    options.summary_call =
        [&](const provider::RequestContext&,
            const std::vector<provider::Message>&, int,
            runtime::CancellationToken, std::string& summary) {
            ++summary_calls;
            summary = "should not be called";
            return ok_error();
        };
    provider::RequestContext context = offline_context(workspace);
    check(runtime.prepare(context, {}, {}, options).ok(), "prepare manual compact runtime");

    const agent::SessionCompactionResult compacted =
        runtime.compact(context, agent::CompactionReason::Manual, {},
                        CompactionStrategy::Fast);
    check(compacted.error.ok() && compacted.compacted && !compacted.no_op,
          "manual compact runs below automatic threshold");
    check(summary_calls == 0 &&
              compacted.applied_strategy == CompactionStrategy::Fast,
          "fast compaction never calls the model seam");

    std::vector<provider::Message> display;
    check(runtime.load_display_messages(display).ok(), "load transcript after manual compact");
    check(display.size() == 15,
          "manual compact keeps the internal checkpoint out of visible history");
    bool visible_summary = false;
    for (const provider::Message& message : display)
        visible_summary = visible_summary || message.role == "summary";
    check(!visible_summary,
          "raw compaction checkpoint payload is never replayed in the TUI transcript");
    {
        agent::AgentSessionStore stored;
        check(stored.open(workspace).ok(), "inspect durable compact checkpoint");
        std::vector<agent::AgentMessageRecord> rows;
        check(stored.load_messages(rows).ok() && rows.size() == 16 &&
                  rows.back().role == "summary" &&
                  rows.back().content.find("stored message 0") != std::string::npos,
              "checkpoint remains durable and carries the protected initial head");
    }

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

void test_summary_compaction_is_transactional_and_uses_active_api_context() {
    const std::string workspace = temp_workspace("summary-compact");
    {
        agent::AgentSessionStore store;
        check(store.open(workspace).ok(), "summary seed open");
        agent::AgentProjectRecord project;
        project.workspace = workspace;
        check(store.open_project(project).ok(), "summary seed project");
        for (int index = 0; index < 50; ++index)
            check(store.append_message(index % 2 == 0 ? "user" : "assistant",
                                       std::string(1000,
                                                   static_cast<char>('a' + index % 20)))
                      .ok(),
                  "summary seed message");
    }

    agent::SessionRuntimeOptions options;
    options.workspace = workspace;
    options.enable_session_db = true;
    options.enable_agent_log = false;
    options.compact_strategy = CompactionStrategy::Summary;
    int calls = 0;
    provider::ApiKind seen_api = provider::ApiKind::ChatCompletions;
    options.summary_call =
        [&](const provider::RequestContext& request,
            const std::vector<provider::Message>& messages, int max_output,
            runtime::CancellationToken, std::string& summary) {
            ++calls;
            seen_api = request.api_kind;
            check(messages.size() == 2 && messages[0].role == "system" &&
                      messages[0].content.find("Active Task") != std::string::npos &&
                      max_output >= 512,
                  "summary seam receives schema-only, non-tool request context");
            summary =
                "Active Task\nContinue\nGoal\nFinish\nConstraints\nNone\n"
                "Decisions\nKeep tests\nCompleted Work\nSeeded\nActive State\nReady\n"
                "Relevant Files/Evidence\nsrc/main.cpp\nBlockers\nNone\n"
                "Remaining Work\nVerify";
            return ok_error();
        };
    provider::RequestContext context = offline_context(workspace);
    context.api_kind = provider::ApiKind::Responses;
    agent::AgentSessionRuntime runtime;
    check(runtime.prepare(context, {}, {}, options).ok(),
          "prepare summary compaction runtime");
    const auto compacted =
        runtime.compact(context, agent::CompactionReason::Manual);
    check(compacted.error.ok() && compacted.compacted && calls >= 3 &&
              seen_api == provider::ApiKind::Responses &&
              compacted.requested_strategy == CompactionStrategy::Summary &&
              compacted.applied_strategy == CompactionStrategy::Summary,
          "summary compaction uses the active Responses context, chunks, consolidates, and commits once");

    runtime.reset();

    // A failed summarizer must not append a summary row.
    const std::string failed_workspace = temp_workspace("summary-failure");
    {
        agent::AgentSessionStore store;
        check(store.open(failed_workspace).ok(), "failed summary seed open");
        agent::AgentProjectRecord project;
        project.workspace = failed_workspace;
        check(store.open_project(project).ok(), "failed summary seed project");
        for (int index = 0; index < 10; ++index)
            check(store.append_message(index % 2 ? "assistant" : "user",
                                       "failure row " + std::to_string(index) +
                                           std::string(1000, 'z'))
                      .ok(),
                  "failed summary seed message");
    }
    options.workspace = failed_workspace;
    options.summary_call =
        [](const provider::RequestContext&,
           const std::vector<provider::Message>&, int,
           runtime::CancellationToken, std::string& summary) {
            summary.clear();
            return ok_error();
        };
    context = offline_context(failed_workspace);
    check(runtime.prepare(context, {}, {}, options).ok(),
          "prepare failed summary runtime");
    const auto empty =
        runtime.compact(context, agent::CompactionReason::Manual);
    check(empty.error.code == ErrorCode::ProviderSchema && !empty.compacted,
          "empty summary is rejected without a fallback commit");
    std::vector<provider::Message> display;
    check(runtime.load_display_messages(display).ok() && display.size() == 10,
          "empty summary preserves every SQLite row transactionally");
    runtime.reset();

    options.summary_call =
        [](const provider::RequestContext&,
           const std::vector<provider::Message>&, int,
           runtime::CancellationToken, std::string&) {
            return Error{ErrorCode::Timeout, "injected summary timeout"};
        };
    check(runtime.prepare(context, {}, {}, options).ok(),
          "prepare failed summary runtime");
    const auto failed =
        runtime.compact(context, agent::CompactionReason::Manual);
    check(failed.error.code == ErrorCode::Timeout && !failed.compacted,
          "summary failure is returned without a fallback commit");
    display.clear();
    check(runtime.load_display_messages(display).ok() && display.size() == 10,
          "summary failure preserves every SQLite row transactionally");
    runtime.reset();

    std::error_code ec;
    fs::remove_all(workspace, ec);
    fs::remove_all(failed_workspace, ec);
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

void test_display_notices_dedupe_consecutive_duplicates() {
    const std::string workspace = temp_workspace("notice-dedupe");
    const std::string previous = fs::current_path().string();
    fs::current_path(workspace);

    agent::AgentSessionRuntime runtime;
    agent::SessionRuntimeOptions options;
    options.workspace = workspace;
    options.interactive = true;
    options.enable_session_db = true;
    options.enable_agent_log = false;
    options.index_mode = agent::SessionRuntimeOptions::IndexMode::Disabled;
    provider::RequestContext context = offline_context(workspace);
    Error error = runtime.prepare(context, {}, {}, options);
    check(error.ok(), "prepare for notice dedupe: " + error.message);

    const std::string notice =
        "Code indexing is off; run /index-code to create and enable it. "
        "Code indexing can speed up certain lookup calls.";
    check(runtime.append_display_notice(notice).ok(), "first indexing-off notice");
    check(runtime.append_display_notice(notice).ok(),
          "duplicate indexing-off notice is a no-op");
    check(runtime.append_display_notice(notice).ok(),
          "third identical indexing-off notice is still a no-op");

    auto count_indexing_off_notices =
        [](const std::vector<provider::Message>& messages) {
            std::size_t count = 0;
            for (const provider::Message& message : messages) {
                // Display load may clip notice width; match the stable prefix.
                if (message.role == "notice" &&
                    message.content.find("Code indexing is off") !=
                        std::string::npos)
                    ++count;
            }
            return count;
        };

    std::vector<provider::Message> display;
    check(runtime.load_display_messages(display).ok(), "load after notice dedupe");
    check(count_indexing_off_notices(display) == 1,
          "identical consecutive indexing-off notices appear once in history");

    // Pre-existing stacked rows from earlier agent versions collapse on load.
    {
        agent::AgentSessionStore store;
        check(store.open(workspace).ok(), "reopen store to inject stacked notices");
        check(store.append_message("notice", notice).ok(), "inject stacked notice 1");
        check(store.append_message("notice", notice).ok(), "inject stacked notice 2");
        check(store.append_message("notice", notice).ok(), "inject stacked notice 3");
        store.close();
    }
    display.clear();
    check(runtime.load_display_messages(display).ok(),
          "load collapses already-stacked notices");
    check(count_indexing_off_notices(display) == 1,
          "load_display_messages collapses consecutive identical notices");

    runtime.reset();
    fs::current_path(previous);
    std::error_code ec;
    fs::remove_all(workspace, ec);
}

}  // namespace

void run_all() {
    test_prepare_opens_session_db_and_tools();
    test_empty_turn_rejected_when_unprepared();
    test_prepare_with_indexing_disabled_never_touches_index_database();
    test_index_code_enables_a_skipped_index_in_place();
    test_index_report_refreshes_and_stays_display_only();
    test_optional_index_modes_create_refresh_and_fallback();
    test_agent_token_usage_aggregation_is_bounded();
    test_task_mode_switch_is_session_scoped_and_failure_safe();
    test_prepare_loads_existing_display_history();
    test_manual_compaction_preserves_transcript_and_noops_until_new_history();
    test_summary_compaction_is_transactional_and_uses_active_api_context();
    test_project_replacement_resets_exact_state_and_switches_workspace();
    test_project_replacement_failure_reopens_prior_project();
    test_display_notices_dedupe_consecutive_duplicates();
}

}  // namespace ainiux::test::agent_session_runtime
