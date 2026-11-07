#include "agent/test_session_runtime.hpp"

#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "agent/compact.hpp"
#include "agent/project_settings.hpp"
#include "agent/session_runtime.hpp"
#include "agent/session_store.hpp"
#include "agent/statistics.hpp"
#include "app/app.hpp"
#include "cli/args.hpp"
#include "provider/provider.hpp"
#include "platform/environment.hpp"
#include "support/test_support.hpp"

namespace ainiux::test::agent_session_runtime {
namespace {
using ainiux::test::check;
namespace fs = std::filesystem;

std::string temp_workspace(const std::string& name) {
    const fs::path root =
        fs::temp_directory_path() / ("ainiux-agent-runtime-" + name + "-" +
                                     std::to_string(ainiux::platform::current_process_id()));
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

void test_runtime_options_share_cli_projection() {
    provider::RequestContext context;
    context.options.agent_log_enabled = false;
    context.options.agent_log_keep_sessions = 7;
    context.options.trusted_prompt_dir = "trusted";
    context.options.max_source_code_file_size = 123456;
    context.options.agent_history_backup_enabled = false;
    context.options.agent_history_backup_max_bytes = 654321;
    context.options.agent_history_backup_ttl_days = 3;
    context.options.agent_auto_compact = false;
    context.options.agent_compact_strategy = CompactionStrategy::Summary;
    context.options.agent_compact_limit = 42;
    context.options.agent_max_turns = 17;
    context.options.agent_run_timeout_seconds = 900;
    context.options.disable_indexing = true;
    context.options.agent_show_command_output = true;
    context.options.connect_timeout_seconds = 4;
    context.options.timeout_seconds = 9;
    context.options.max_fetch_bytes = 777;
    context.options.proxy = "http://proxy.invalid";
    context.options.insecure_tls = true;
    context.options.trace_http = true;
    context.options.allow_private_url_fetch = true;

    const agent::SessionRuntimeOptions options =
        agent::make_session_runtime_options(
            context, "/workspace", agent::AgentLane::Lead, true);
    check(options.workspace == "/workspace" &&
              options.lane == agent::AgentLane::Lead &&
              options.interactive && options.allow_network &&
              options.enable_session_db,
          "shared runtime options preserve caller-owned session identity");
    check(!options.enable_agent_log &&
              options.agent_log_keep_sessions == 7 &&
              options.trusted_prompt_dir == "trusted" &&
              options.max_source_code_file_size == 123456 &&
              !options.history_backup.enabled &&
              options.history_backup.max_bytes == 654321 &&
              options.history_backup.ttl_days == 3,
          "shared runtime options project persistence settings");
    check(!options.auto_compact &&
              options.compact_strategy == CompactionStrategy::Summary &&
              options.compact_limit == 42 && options.max_agent_turns == 17 &&
              options.run_timeout_seconds == 900 &&
              options.index_mode == agent::SessionRuntimeOptions::IndexMode::Disabled &&
              options.show_command_output,
          "shared runtime options project agent policy settings");
    check(options.fetch_options.connect_timeout_seconds == 4 &&
              options.fetch_options.timeout_seconds == 9 &&
              options.fetch_options.max_bytes == 777 &&
              options.fetch_options.proxy == "http://proxy.invalid" &&
              options.fetch_options.insecure_tls &&
              options.fetch_options.trace_http &&
              options.fetch_options.allow_private,
          "shared runtime options project network settings");
}

void test_prepare_opens_session_db_and_tools() {
    const std::string workspace = temp_workspace("prepare");
    const std::string previous = fs::current_path().string();
    fs::current_path(workspace);

    agent::AgentSessionRuntime runtime;
    agent::SessionRuntimeOptions options;
    options.workspace = workspace;
    options.lane = agent::AgentLane::Act;
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
              created.markdown.find("Code indexing completed in") == 0 &&
              created.markdown.find(" seconds. Here is the summary:\n\n") !=
                  std::string::npos &&
              created.markdown.find(u8"┌") != std::string::npos &&
              created.markdown.find("**All languages**") != std::string::npos &&
              created.markdown.find("**1**") != std::string::npos,
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
              refreshed.markdown.find("**All languages**") != std::string::npos &&
              refreshed.markdown.find("**2**") != std::string::npos &&
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
              initial.markdown.find(u8"┌") == 0 &&
              initial.markdown.find("│ Language") != std::string::npos &&
              initial.markdown.find("Files") != std::string::npos &&
              initial.markdown.find("Lines of code") != std::string::npos &&
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
              refreshed.markdown.find("**All languages**") != std::string::npos &&
              refreshed.markdown.find("**2**") != std::string::npos &&
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

    long long stream_tokens = 0;
    long long stream_ms = 0;
    bool stream_estimated = false;
    provider::ChatResult first_stream;
    first_stream.ttft_ms = 200;
    first_stream.total_ms = 700;
    first_stream.completion_tokens = 100;
    first_stream.usage_json = "{}";
    check(agent::accumulate_agent_stream_decode(first_stream, 0, true, stream_tokens,
                                                stream_ms, stream_estimated) &&
              stream_tokens == 100 && stream_ms == 500 && !stream_estimated,
          "stream decode counts only time after the first token");
    provider::ChatResult second_stream = first_stream;
    second_stream.ttft_ms = 100;
    second_stream.total_ms = 350;
    second_stream.completion_tokens = 50;
    check(agent::accumulate_agent_stream_decode(second_stream, 0, true, stream_tokens,
                                                stream_ms, stream_estimated) &&
              stream_tokens == 150 && stream_ms == 750,
          "stream decode sums across model rounds and ignores tool-call gaps");
    provider::ChatResult waiting;
    waiting.ttft_ms = -1;
    waiting.total_ms = 4000;
    waiting.completion_tokens = 80;
    waiting.usage_json = "{}";
    check(!agent::accumulate_agent_stream_decode(waiting, 0, true, stream_tokens,
                                                 stream_ms, stream_estimated) &&
              stream_tokens == 150 && stream_ms == 750,
          "rounds without a started stream are excluded from the rate");
    provider::ChatResult first_body_only;
    first_body_only.ttft_ms = -1;
    first_body_only.first_body_ms = 80;
    first_body_only.total_ms = 280;
    first_body_only.completion_tokens = 40;
    first_body_only.usage_json = "{}";
    check(agent::accumulate_agent_stream_decode(first_body_only, 0, true, stream_tokens,
                                                stream_ms, stream_estimated) &&
              stream_tokens == 190 && stream_ms == 950,
          "stream decode can start from first body byte when TTFT was not stamped");
    check(!agent::accumulate_agent_stream_decode(first_stream, 0, false, stream_tokens,
                                                 stream_ms, stream_estimated),
          "non-streaming rounds are excluded from the decode rate");
    check(std::abs(agent::agent_stream_tokens_per_second(150, 750) - 200.0) < 0.001,
          "aggregate decode rate is tokens per post-TTFT second");
    check(agent::agent_stream_tokens_per_second(0, 750) < 0 &&
              agent::agent_stream_tokens_per_second(150, 0) < 0,
          "empty stream samples produce no rate");
}

void test_active_lane_switch_is_session_scoped_and_failure_safe() {
    const std::string workspace = temp_workspace("lane-switch");
    agent::AgentSessionRuntime runtime;
    agent::SessionRuntimeOptions options;
    options.workspace = workspace;
    options.enable_session_db = false;
    options.enable_agent_log = false;
    provider::RequestContext context = offline_context(workspace);
    Error error = runtime.prepare(context, {}, {}, options);
    check(error.ok() && runtime.lane() == agent::AgentLane::Act &&
              runtime.mutation_policy() == agent::MutationPolicy::Full,
          "agent runtime defaults to Act with full mutation policy");

    error = runtime.switch_lane(agent::AgentLane::Lead);
    check(error.ok() && runtime.lane() == agent::AgentLane::Lead &&
              runtime.mutation_policy() == agent::MutationPolicy::Full,
          "Act to Lead retains full mutation authority");
    check(runtime.switch_lane(agent::AgentLane::Lead).ok(),
          "repeated Lead switch is idempotent");

    std::error_code ec;
    fs::create_symlink("src/hello.cpp", fs::path(workspace) / "AGENTS.md", ec);
    error = runtime.switch_lane(agent::AgentLane::Act);
    check(!error.ok() && runtime.lane() == agent::AgentLane::Lead &&
              runtime.mutation_policy() == agent::MutationPolicy::Full,
          "AGENTS reload failure leaves the prior lane and policy intact");
    fs::remove(fs::path(workspace) / "AGENTS.md", ec);
    error = runtime.switch_lane(agent::AgentLane::Act);
    check(error.ok() && runtime.lane() == agent::AgentLane::Act &&
              runtime.mutation_policy() == agent::MutationPolicy::Full,
          "Lead to Act succeeds after AGENTS reload is valid");

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
    options.lane = agent::AgentLane::Act;
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

void test_lane_handoffs_are_bounded_and_exclude_tools() {
    const std::string workspace = temp_workspace("lane-handoff");
    std::string long_unicode_response;
    for (int i = 0; i < 240; ++i) long_unicode_response += "é";
    {
        agent::AgentSessionStore store;
        check(store.open(workspace).ok(), "open lane handoff fixture");
        check(store.append_message("user", "first request").ok(), "seed first Act request");
        check(store.append_message("assistant", long_unicode_response).ok(),
              "seed long Act response");
        check(store.append_message("tool", "SECRET TOOL PAYLOAD", "read").ok(),
              "seed excluded tool row");
        check(store.append_message("user", "second request").ok(), "seed second Act request");
        check(store.append_message("assistant", "short response").ok(),
              "seed second Act response");
    }
    provider::RequestContext context = offline_context(workspace);
    agent::SessionRuntimeOptions options;
    options.workspace = workspace;
    options.interactive = true;
    options.enable_agent_log = false;
    agent::AgentSessionRuntime runtime;
    check(runtime.prepare(context, {}, {}, options).ok(), "prepare lane handoff runtime");
    check(runtime.switch_lane(agent::AgentLane::Lead).ok(),
          "default switch to Lead creates a handoff");
    {
        agent::AgentSessionStore store;
        std::vector<agent::AgentMessageRecord> rows;
        check(store.open(workspace).ok() && store.load_messages(rows).ok(),
              "load durable Lead handoff");
        const auto found = std::find_if(rows.rbegin(), rows.rend(), [](const auto& row) {
            return row.role == "handoff" && row.lane == "lead";
        });
        check(found != rows.rend() &&
                  found->content.find("first request") != std::string::npos &&
                  found->content.find("second request") != std::string::npos &&
                  found->content.find(long_unicode_response.substr(0, 400)) !=
                      std::string::npos &&
                  found->content.find(long_unicode_response.substr(0, 402)) ==
                      std::string::npos &&
                  found->content.find("SECRET TOOL PAYLOAD") == std::string::npos,
              "Lead handoff keeps two user turns, truncates responses by Unicode character, and drops tools");
        store.set_active_lane("lead");
        check(store.append_message("user", "lead question").ok() &&
                  store.append_message("assistant", "complete lead answer").ok(),
              "seed Lead exchange for Act return");
    }
    check(runtime.switch_lane(agent::AgentLane::Act).ok(),
          "return to Act creates a full Lead handoff");
    {
        agent::AgentSessionStore store;
        std::vector<agent::AgentMessageRecord> rows;
        check(store.open(workspace).ok() && store.load_messages(rows).ok(),
              "load durable Act handoff");
        const auto found = std::find_if(rows.rbegin(), rows.rend(), [](const auto& row) {
            return row.role == "handoff" && row.lane == "act";
        });
        check(found != rows.rend() &&
                  found->content.find("lead question") != std::string::npos &&
                  found->content.find("complete lead answer") != std::string::npos,
              "Act return receives Lead prompts and full final responses");
    }
    {
        agent::AgentSessionStore store;
        check(store.open(workspace).ok(), "reopen handoff fixture for all scope");
        store.set_active_lane("act");
        check(store.append_message("user", "third request").ok() &&
                  store.append_message("tool", "ALL TOOL PAYLOAD", "read").ok() &&
                  store.append_message("assistant", "third response").ok(),
              "seed new Act tool activity for all-scope handoff");
    }
    agent::LaneHandoff all;
    all.scope = agent::LaneHandoff::Scope::All;
    check(runtime.switch_lane(agent::AgentLane::Lead, all).ok(),
          "all-scope Lead switch succeeds");
    {
        agent::AgentSessionStore store;
        std::vector<agent::AgentMessageRecord> rows;
        check(store.open(workspace).ok() && store.load_messages(rows).ok(),
              "load all-scope Lead handoff");
        const auto found = std::find_if(rows.rbegin(), rows.rend(), [](const auto& row) {
            return row.role == "handoff" && row.lane == "lead";
        });
        check(found != rows.rend() &&
                  found->content.find("third request") != std::string::npos &&
                  found->content.find("ALL TOOL PAYLOAD") != std::string::npos &&
                  found->content.find("third response") != std::string::npos,
              "Lead all handoff retains new user, tool, and assistant rows");
    }
    check(runtime.switch_lane(agent::AgentLane::Act).ok(),
          "return to Act before clean Lead switch");
    agent::LaneHandoff clean;
    clean.scope = agent::LaneHandoff::Scope::Clean;
    check(runtime.switch_lane(agent::AgentLane::Lead, clean).ok(),
          "clean Lead switch succeeds");
    {
        agent::AgentSessionStore store;
        agent::AgentProjectRecord project;
        long long cut = 0;
        check(store.open(workspace).ok() && store.open_project(project).ok() &&
                  agent::lane_context_after_seq_from_settings_json(
                      project.settings_json, agent::AgentLane::Lead, cut).ok() &&
                  cut > 0,
              "clean Lead switch persists a durable lane context boundary");
    }
    std::size_t act_handoffs_before_reset = 0;
    {
        agent::AgentSessionStore store;
        std::vector<agent::AgentMessageRecord> rows;
        check(store.open(workspace).ok() && store.load_messages(rows).ok(),
              "load handoffs before full context reset");
        act_handoffs_before_reset = static_cast<std::size_t>(std::count_if(
            rows.begin(), rows.end(), [](const auto& row) {
                return row.role == "handoff" && row.lane == "act";
            }));
    }
    check(runtime.reset_model_context().ok(), "reset both lane contexts");
    check(runtime.switch_lane(agent::AgentLane::Act).ok(),
          "switch after full context reset succeeds");
    {
        agent::AgentSessionStore store;
        std::vector<agent::AgentMessageRecord> rows;
        check(store.open(workspace).ok() && store.load_messages(rows).ok(),
              "load handoffs after full context reset");
        const std::size_t act_handoffs_after_reset =
            static_cast<std::size_t>(std::count_if(
                rows.begin(), rows.end(), [](const auto& row) {
                    return row.role == "handoff" && row.lane == "act";
                }));
        check(act_handoffs_after_reset == act_handoffs_before_reset,
              "lane switch cannot resurrect history removed by compact all");
    }
    runtime.reset();
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
        store.set_active_lane("lead");
        check(store.append_message("handoff", std::string(100000, 'z')).ok(),
              "compact fixture includes a large inactive-lane checkpoint");
        store.set_active_lane("act");
        check(agent::settings_with_agent_lane(project.settings_json,
                                               agent::AgentLane::Act,
                                               project.settings_json)
                      .ok() &&
                  store.update_project_meta(project).ok(),
              "compact fixture restores Act as the durable active lane");
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
    check(compacted.tokens_before < 10000,
          "active-lane compaction excludes the inactive lane transcript");
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
        check(stored.load_messages(rows).ok() && rows.size() == 17 &&
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
                      messages[1].role == "user" &&
                      messages[1].content.find("Chronological history") !=
                          std::string::npos &&
                      max_output >= 512 && max_output <= 2000,
                  "summary seam receives reduced-history schema prompt and guidance");
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
    check(empty.error.ok() && empty.compacted &&
              empty.requested_strategy == CompactionStrategy::Summary &&
              empty.applied_strategy == CompactionStrategy::Fast &&
              empty.reason.find("falling back") != std::string::npos,
          "empty summary falls back to a reducing fast checkpoint commit");
    std::vector<provider::Message> display;
    check(runtime.load_display_messages(display).ok() && !display.empty(),
          "empty-summary fallback still rebuilds a usable live transcript");
    {
        agent::AgentSessionStore stored;
        check(stored.open(failed_workspace).ok(),
              "inspect empty-summary fallback rows");
        std::vector<agent::AgentMessageRecord> rows;
        check(stored.load_messages(rows).ok() && rows.size() == 11 &&
                  rows.back().role == "summary",
              "empty-summary fallback appends one durable summary without dropping history");
    }
    runtime.reset();

    // Fresh workspace: model error also falls back to fast rather than failing.
    const std::string timeout_workspace = temp_workspace("summary-timeout");
    {
        agent::AgentSessionStore store;
        check(store.open(timeout_workspace).ok(), "timeout summary seed open");
        agent::AgentProjectRecord project;
        project.workspace = timeout_workspace;
        check(store.open_project(project).ok(), "timeout summary seed project");
        for (int index = 0; index < 10; ++index)
            check(store.append_message(index % 2 ? "assistant" : "user",
                                       "timeout row " + std::to_string(index) +
                                           std::string(1000, 'z'))
                      .ok(),
                  "timeout summary seed message");
    }
    options.workspace = timeout_workspace;
    options.summary_call =
        [](const provider::RequestContext&,
           const std::vector<provider::Message>&, int,
           runtime::CancellationToken, std::string&) {
            return Error{ErrorCode::Timeout, "injected summary timeout"};
        };
    context = offline_context(timeout_workspace);
    check(runtime.prepare(context, {}, {}, options).ok(),
          "prepare timeout summary runtime");
    const auto failed =
        runtime.compact(context, agent::CompactionReason::Manual);
    check(failed.error.ok() && failed.compacted &&
              failed.applied_strategy == CompactionStrategy::Fast &&
              failed.reason.find("falling back") != std::string::npos,
          "summary model failure falls back to a fast checkpoint commit");
    display.clear();
    check(runtime.load_display_messages(display).ok() && !display.empty(),
          "timeout-summary fallback still rebuilds a usable live transcript");
    {
        agent::AgentSessionStore stored;
        check(stored.open(timeout_workspace).ok(),
              "inspect timeout-summary fallback rows");
        std::vector<agent::AgentMessageRecord> rows;
        check(stored.load_messages(rows).ok() && rows.size() == 11 &&
                  rows.back().role == "summary",
              "timeout-summary fallback appends one durable summary without dropping history");
    }
    runtime.reset();

    std::error_code ec;
    fs::remove_all(workspace, ec);
    fs::remove_all(failed_workspace, ec);
    fs::remove_all(timeout_workspace, ec);
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

void test_prepare_restores_live_conversation_instead_of_recap() {
    const std::string workspace = temp_workspace("restore-live-conversation");
    const std::string marker = "UNIQUE_RESTORED_TOOL_BODY_" + std::string(2000, 'Q');
    {
        agent::AgentSessionStore store;
        check(store.open(workspace).ok(), "restore seed open");
        agent::AgentProjectRecord project;
        project.workspace = workspace;
        project.status = "idle";
        project.api = "chat";
        project.protocol = "native";
        check(store.open_project(project).ok(), "restore seed project");
        check(store.append_message("user", "implement the parser").ok(), "restore user");
        check(store.append_tool_event(1, 1, "call-restore", "read",
                                      R"({"path":"src/parser.cpp"})", marker, true)
                  .ok(),
              "restore tool event with a large body");
        check(store.append_message("tool", "1: read(\"src/parser.cpp\") → ok", "read", true)
                  .ok(),
              "restore compact tool display row");
        check(store.append_message("assistant", "updated src/parser.cpp").ok(),
              "restore assistant");
        check(store.append_message("thinking", "Thinking: " + std::string(2000, 't')).ok(),
              "restore thinking");
        check(store.append_message("notice", "display only").ok(), "restore notice");
    }

    agent::AgentSessionRuntime runtime;
    agent::SessionRuntimeOptions options;
    options.workspace = workspace;
    options.lane = agent::AgentLane::Act;
    options.interactive = true;
    options.enable_session_db = true;
    options.enable_agent_log = false;
    options.index_mode = agent::SessionRuntimeOptions::IndexMode::Disabled;
    provider::RequestContext context = offline_context(workspace);
    Error error = runtime.prepare(context, {}, {}, options);
    check(error.ok() && runtime.prepared() && runtime.conversation_seeded(),
          "interactive prepare restores model conversation: " + error.message);

    const provider::ToolConversation& conversation = runtime.model_conversation();
    std::string blob;
    for (const provider::Message& message : conversation.messages)
        blob += message.content;
    for (const std::string& item : conversation.continuation_items_json) blob += item;
    check(blob.find("implement the parser") != std::string::npos,
          "restored conversation includes the user goal");
    check(blob.find("updated src/parser.cpp") != std::string::npos,
          "restored conversation includes the assistant reply");
    check(blob.find(marker) != std::string::npos,
          "restored conversation includes the full tool result body");
    check(blob.find("Prior agent work on this project") == std::string::npos,
          "restored conversation is not the 24k recap wrapper");
    check(blob.find("display only") == std::string::npos, "skips display notices");

    const long long chrome = runtime.estimated_request_tokens();
    check(chrome > agent::estimate_tokens_from_text(marker) / 2,
          "idle chrome after restore counts the live restored conversation");

    std::vector<provider::Message> display;
    check(runtime.load_display_messages(display).ok() && !display.empty(),
          "display tail still loads");
    bool saw_summary_role = false;
    for (const provider::Message& message : display)
        saw_summary_role = saw_summary_role || message.role == "summary";
    check(!saw_summary_role, "display still omits summary rows");

    // Snapshot persist makes the second prepare an exact restore.
    runtime.reset();
    check(runtime.prepare(context, {}, {}, options).ok() && runtime.conversation_seeded(),
          "second prepare restores the persisted snapshot");
    const provider::ToolConversation& again = runtime.model_conversation();
    check(again.continuation_items_json == conversation.continuation_items_json,
          "snapshot restore is byte-identical for continuation items");
    check(again.messages.size() == conversation.messages.size(),
          "snapshot restore keeps the same seed message count");
    for (std::size_t i = 0; i < conversation.messages.size(); ++i) {
        check(again.messages[i].role == conversation.messages[i].role &&
                  again.messages[i].content == conversation.messages[i].content,
              "snapshot restore is byte-identical for seed messages");
    }

    runtime.reset();
    options.interactive = false;
    check(runtime.prepare(context, {}, {}, options).ok(), "prepare headless");
    check(!runtime.conversation_seeded(),
          "headless --run does not restore the live conversation");
    check(runtime.estimated_request_tokens() < chrome,
          "headless idle chrome is seed/tools only");

    runtime.reset();
    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_snapshot_round_trip_and_compact_all_clears_restore() {
    const std::string workspace = temp_workspace("snapshot-round-trip");
    {
        agent::AgentSessionStore store;
        check(store.open(workspace).ok(), "snapshot fixture open");
        agent::AgentProjectRecord project;
        project.workspace = workspace;
        project.status = "idle";
        check(store.open_project(project).ok(), "snapshot fixture project");
        agent::ConversationSnapshotRecord record;
        record.lane = "act";
        record.seeded = true;
        record.protocol = "native";
        record.messages_json =
            R"([{"role":"system","content":"placeholder-system"},)"
            R"({"role":"user","content":"saved goal"}])";
        record.continuation_json =
            R"(["{\"role\":\"assistant\",\"content\":\"mid-turn progress CALL_ID_99\"}"])";
        check(store.save_conversation_snapshot(record).ok(), "save act snapshot");
        record.lane = "lead";
        record.messages_json =
            R"([{"role":"system","content":"placeholder-system"},)"
            R"({"role":"user","content":"lead only goal"}])";
        record.continuation_json = R"(["{\"role\":\"assistant\",\"content\":\"lead only\"}"])";
        check(store.save_conversation_snapshot(record).ok(), "save lead snapshot");
    }

    agent::AgentSessionRuntime runtime;
    agent::SessionRuntimeOptions options;
    options.workspace = workspace;
    options.interactive = true;
    options.enable_session_db = true;
    options.enable_agent_log = false;
    options.index_mode = agent::SessionRuntimeOptions::IndexMode::Disabled;
    provider::RequestContext context = offline_context(workspace);
    check(runtime.prepare(context, {}, {}, options).ok() && runtime.conversation_seeded(),
          "prepare restores act snapshot");
    std::string blob;
    for (const provider::Message& message : runtime.model_conversation().messages)
        blob += message.content;
    for (const std::string& item : runtime.model_conversation().continuation_items_json)
        blob += item;
    check(blob.find("saved goal") != std::string::npos, "act snapshot user is restored");
    check(blob.find("CALL_ID_99") != std::string::npos,
          "act snapshot continuation (crash mid-turn) is restored");
    check(blob.find("lead only") == std::string::npos, "act restore does not leak lead");
    check(runtime.model_conversation().messages.front().content == "placeholder-system",
          "restored system prompt stays byte-identical so prefix cache can hit");

    check(runtime.reset_model_context().ok(), "compact all clears live context");
    check(!runtime.conversation_seeded(), "compact all unseeds the live conversation");
    runtime.reset();
    check(runtime.prepare(context, {}, {}, options).ok(), "re-prepare after compact all");
    check(!runtime.conversation_seeded(),
          "explicit unseeded snapshot prevents transcript reconstruction");
    blob.clear();
    for (const std::string& item : runtime.model_conversation().continuation_items_json)
        blob += item;
    check(blob.find("CALL_ID_99") == std::string::npos,
          "compact all restore does not resurrect pre-cut continuation");

    runtime.reset();
    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_in_window_snapshot_is_not_rewritten_from_transcript() {
    const std::string workspace = temp_workspace("exact-snapshot");
    const std::string cached = "UNIQUE_PREFIX_CACHE_BODY_" + std::string(500, 'C');
    {
        agent::AgentSessionStore store;
        check(store.open(workspace).ok(), "exact snapshot fixture open");
        agent::AgentProjectRecord project;
        project.workspace = workspace;
        project.status = "idle";
        project.api = "chat";
        project.protocol = "native";
        check(store.open_project(project).ok(), "exact snapshot fixture project");
        check(store.append_message("user", "LAST_SESSION_GOAL").ok(), "transcript user");
        check(store.append_tool_event(1, 1, "call-last", "read", "{}",
                                      "LAST_SESSION_TOOL_BODY", true)
                  .ok(),
              "transcript tool");
        check(store.append_message("notice", "quit agent mode").ok(), "transcript quit");
        agent::ConversationSnapshotRecord record;
        record.lane = "act";
        record.seeded = true;
        record.protocol = "native";
        record.messages_json =
            R"([{"role":"system","content":"cached-system-prompt"},)"
            R"({"role":"user","content":"cached goal"}])";
        record.continuation_json = std::string("[\"") + cached + "\"]";
        check(store.save_conversation_snapshot(record).ok(), "save in-window snapshot");
    }

    agent::AgentSessionRuntime runtime;
    agent::SessionRuntimeOptions options;
    options.workspace = workspace;
    options.interactive = true;
    options.enable_session_db = true;
    options.enable_agent_log = false;
    options.index_mode = agent::SessionRuntimeOptions::IndexMode::Disabled;
    provider::RequestContext context = offline_context(workspace);
    context.options.context_tokens = 100000;
    check(runtime.prepare(context, {}, {}, options).ok() && runtime.conversation_seeded(),
          "prepare restores the in-window snapshot");
    const provider::ToolConversation first = runtime.model_conversation();
    std::string blob;
    for (const provider::Message& message : first.messages) blob += message.content;
    for (const std::string& item : first.continuation_items_json) blob += item;
    check(first.messages.front().content == "cached-system-prompt",
          "in-window restore does not replace the cached system prompt");
    check(blob.find(cached) != std::string::npos, "in-window restore keeps snapshot continuation");
    check(blob.find("LAST_SESSION_GOAL") == std::string::npos,
          "in-window restore does not reconstruct a different last session");

    runtime.reset();
    check(runtime.prepare(context, {}, {}, options).ok() && runtime.conversation_seeded(),
          "second prepare restores the same snapshot");
    const provider::ToolConversation second = runtime.model_conversation();
    check(second.messages.size() == first.messages.size() &&
              second.continuation_items_json == first.continuation_items_json,
          "reopen keeps the restored prefix byte-identical");
    for (std::size_t i = 0; i < first.messages.size(); ++i) {
        check(second.messages[i].role == first.messages[i].role &&
                  second.messages[i].content == first.messages[i].content,
              "reopen keeps restored seed messages byte-identical");
    }

    runtime.reset();
    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_oversized_snapshot_reconstructs_last_session() {
    const std::string workspace = temp_workspace("oversized-snapshot");
    const std::string last_body = "LAST_LIVE_TOOL_BODY_" + std::string(200, 'L');
    {
        agent::AgentSessionStore store;
        check(store.open(workspace).ok(), "oversized fixture open");
        agent::AgentProjectRecord project;
        project.workspace = workspace;
        project.status = "idle";
        project.api = "chat";
        project.protocol = "native";
        check(store.open_project(project).ok(), "oversized fixture project");
        check(store.append_message("summary", "checkpoint body").ok(), "summary");
        check(store.append_message("user", "OLD_SESSION_GOAL").ok(), "old user");
        check(store.append_tool_event(1, 1, "call-old", "read", "{}",
                                      "OLD_SESSION_TOOL_BODY", true)
                  .ok(),
              "old tool");
        check(store.append_message("notice", "quit agent mode").ok(), "old quit");
        check(store.append_message("user", "LAST_SESSION_GOAL").ok(), "last user");
        check(store.append_tool_event(1, 2, "call-last", "read", "{}", last_body, true)
                  .ok(),
              "last tool");
        check(store.append_message("assistant", "LAST_SESSION_REPLY").ok(),
              "last assistant");
        check(store.append_message("notice", "quit agent mode").ok(), "last quit");
        agent::ConversationSnapshotRecord record;
        record.lane = "act";
        record.seeded = true;
        record.protocol = "native";
        record.messages_json = R"([{"role":"system","content":"placeholder-system"}])";
        record.continuation_json =
            std::string("[\"") + std::string(20000, 'X') + "\"]";
        check(store.save_conversation_snapshot(record).ok(), "save oversized snapshot");
    }

    agent::AgentSessionRuntime runtime;
    agent::SessionRuntimeOptions options;
    options.workspace = workspace;
    options.interactive = true;
    options.enable_session_db = true;
    options.enable_agent_log = false;
    options.index_mode = agent::SessionRuntimeOptions::IndexMode::Disabled;
    provider::RequestContext context = offline_context(workspace);
    context.options.context_tokens = 1000;
    check(runtime.prepare(context, {}, {}, options).ok() && runtime.conversation_seeded(),
          "prepare rejects an over-window snapshot and reconstructs");
    std::string blob;
    for (const provider::Message& message : runtime.model_conversation().messages)
        blob += message.content;
    for (const std::string& item : runtime.model_conversation().continuation_items_json)
        blob += item;
    check(blob.find("checkpoint body") != std::string::npos,
          "oversized snapshot fallback keeps the compact checkpoint");
    check(blob.find("LAST_SESSION_GOAL") != std::string::npos,
          "oversized snapshot fallback keeps the last session");
    check(blob.find(last_body) != std::string::npos,
          "oversized snapshot fallback keeps the last session tool body");
    check(blob.find("OLD_SESSION_GOAL") == std::string::npos,
          "oversized snapshot fallback drops earlier sessions");
    check(blob.find(std::string(20000, 'X')) == std::string::npos,
          "over-window snapshot contents are not restored");
    check(runtime.estimated_request_tokens() <
              agent::estimate_tokens_from_text(std::string(20000, 'X')),
          "idle chrome after bounded reconstruct is smaller than the rejected snapshot");

    runtime.reset();
    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_session_goal_set_pause_resume_complete_and_persist() {
    const std::string workspace = temp_workspace("session-goal");
    agent::AgentSessionRuntime runtime;
    agent::SessionRuntimeOptions options;
    options.workspace = workspace;
    options.interactive = true;
    options.enable_session_db = true;
    options.enable_agent_log = false;
    options.index_mode = agent::SessionRuntimeOptions::IndexMode::Disabled;
    provider::RequestContext context = offline_context(workspace);
    Error error = runtime.prepare(context, {}, {}, options);
    check(error.ok() && runtime.prepared(),
          "prepare for session goal: " + error.message);

    check(runtime.goal().status == agent::GoalStatus::Cleared,
          "fresh session has no goal");
    check(!runtime.set_goal("").ok(), "empty goal condition is rejected");
    error = runtime.set_goal("create file notes.txt containing hello");
    check(error.ok() && agent::goal_is_active(runtime.goal()) &&
              runtime.goal().condition.find("notes.txt") != std::string::npos,
          "set_goal activates the condition");

    error = runtime.pause_goal("waiting for user");
    check(error.ok() && runtime.goal().status == agent::GoalStatus::Paused &&
              !agent::goal_is_active(runtime.goal()),
          "pause_goal strips active status");
    error = runtime.resume_goal();
    check(error.ok() && agent::goal_is_active(runtime.goal()),
          "resume_goal restores Active");

    error = runtime.mark_goal_complete("notes.txt contains hello");
    check(error.ok() && runtime.goal().status == agent::GoalStatus::Complete &&
              runtime.goal().last_reason.find("hello") != std::string::npos &&
              !agent::goal_is_active(runtime.goal()),
          "mark_goal_complete finishes the goal with evidence");

    // Persist across prepare() reload via settings_json.
    runtime.reset();
    error = runtime.prepare(context, {}, {}, options);
    check(error.ok() && runtime.goal().status == agent::GoalStatus::Complete &&
              runtime.goal().condition.find("notes.txt") != std::string::npos,
          "completed goal reloads from project settings");

    error = runtime.clear_goal();
    check(error.ok() && runtime.goal().status == agent::GoalStatus::Cleared,
          "clear_goal resets status");
    runtime.reset();
    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_switch_permission_mode_persists_and_reloads() {
    const std::string workspace = temp_workspace("permissions");
    agent::AgentSessionRuntime runtime;
    agent::SessionRuntimeOptions options;
    options.workspace = workspace;
    options.interactive = true;
    options.enable_session_db = true;
    options.enable_agent_log = false;
    options.index_mode = agent::SessionRuntimeOptions::IndexMode::Disabled;
    provider::RequestContext context = offline_context(workspace);
    Error error = runtime.prepare(context, {}, {}, options);
    check(error.ok() && runtime.prepared(),
          "prepare for permission switch: " + error.message);
    check(runtime.permission_mode() == agent::PermissionMode::Smart,
          "interactive default permission mode is smart");

    error = runtime.switch_permission_mode(agent::PermissionMode::Yolo, context);
    check(error.ok() && runtime.permission_mode() == agent::PermissionMode::Yolo,
          "switch_permission_mode updates the live session: " + error.message);

    {
        agent::AgentSessionStore store;
        check(store.open(workspace).ok(), "open project after permission switch");
        agent::AgentProjectRecord project;
        check(store.open_project(project).ok(), "load project after permission switch");
        check(project.settings_json.find("\"permission_mode\":\"yolo\"") !=
                  std::string::npos,
              "permission switch writes yolo into project settings: " +
                  project.settings_json);
    }

    runtime.reset();
    error = runtime.prepare(context, {}, {}, options);
    check(error.ok() && runtime.permission_mode() == agent::PermissionMode::Yolo,
          "prepare reloads persisted yolo permission mode");

    runtime.reset();
    options.allow_yolo = false;
    options.permission_mode = agent::PermissionMode::Confirm;
    error = runtime.prepare(context, {}, {}, options);
    check(error.ok() && runtime.permission_mode() == agent::PermissionMode::Confirm,
          "remote permission cap replaces persisted yolo with the requested safer mode");

    runtime.reset();
    options.allow_yolo = false;
    options.permission_mode = agent::PermissionMode::Yolo;
    error = runtime.prepare(context, {}, {}, options);
    check(error.ok() && runtime.permission_mode() == agent::PermissionMode::Smart,
          "remote permission cap replaces requested yolo with smart");

    runtime.reset();
    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_agent_session_statistics_format_and_persist() {
    check(agent::format_agent_elapsed_hms(0) == "0 hours, 0 minutes, 0 seconds",
          "zero elapsed uses all three units");
    check(agent::format_agent_elapsed_hms(1000) == "0 hours, 0 minutes, 1 second",
          "one second is singular");
    check(agent::format_agent_elapsed_hms(61000) == "0 hours, 1 minute, 1 second",
          "one minute and one second are singular");
    check(agent::format_agent_elapsed_hms(3725000) ==
              "1 hour, 2 minutes, 5 seconds",
          "HMS wording matches the /statistics display contract");

    agent::AgentSessionStatistics empty;
    const std::string empty_table = agent::format_agent_statistics_table(empty);
    check(empty_table.find(u8"┌") == 0 && empty_table.find(u8"└") != std::string::npos &&
              empty_table.find("| Metric") == std::string::npos,
          "statistics table uses the Unicode box printer, not raw GFM");
    check(empty_table.find("Cached input tokens") != std::string::npos &&
              empty_table.find("Fresh input tokens") != std::string::npos &&
              empty_table.find("Output tokens") != std::string::npos &&
              empty_table.find("Cache hit ratio") != std::string::npos &&
              empty_table.find("n/a") != std::string::npos &&
              empty_table.find("Tool calls") != std::string::npos &&
              empty_table.find("Failed tool calls") != std::string::npos &&
              empty_table.find("Fail percentage") != std::string::npos &&
              empty_table.find("0.0%") != std::string::npos &&
              empty_table.find("Time spent") != std::string::npos &&
              empty_table.find("0 hours, 0 minutes, 0 seconds") != std::string::npos,
          "empty statistics table includes required labels, n/a hit ratio, and 0.0% fail");

    provider::ChatResult first;
    first.usage_json = "{}";
    first.prompt_tokens = 100;
    first.fresh_prompt_tokens = 20;
    first.cache_read_tokens = 80;
    first.completion_tokens = 7;
    provider::ChatResult second = first;
    second.prompt_tokens = 200;
    second.fresh_prompt_tokens = 50;
    second.cache_read_tokens = 150;
    second.completion_tokens = 11;
    agent::AgentTokenUsage usage;
    agent::accumulate_agent_token_usage(first, usage);
    agent::accumulate_agent_token_usage(second, usage);
    agent::AgentSessionStatistics summed;
    summed.cached_input_tokens = usage.cache_read_tokens;
    summed.fresh_input_tokens = usage.fresh_input_tokens;
    summed.output_tokens = usage.output_tokens;
    summed.tool_calls = 4;
    summed.failed_tool_calls = 1;
    summed.elapsed_ms = 3725000;
    check(summed.cached_input_tokens == 230 && summed.fresh_input_tokens == 70 &&
              summed.output_tokens == 18,
          "two fake rounds accumulate cached, fresh, and output tokens");
    const std::string table = agent::format_agent_statistics_table(summed);
    check(table.find("230") != std::string::npos &&
              table.find("70") != std::string::npos &&
              table.find("18") != std::string::npos &&
              table.find("76.7%") != std::string::npos &&
              table.find("25.0%") != std::string::npos &&
              table.find("1 hour, 2 minutes, 5 seconds") != std::string::npos,
          "statistics table shows summed tokens, cache hit ratio, fail percentage, and HMS");

    agent::AgentSessionStatistics estimated;
    estimated.output_tokens = 1234;
    estimated.output_estimated = true;
    const std::string estimated_table = agent::format_agent_statistics_table(estimated);
    check(estimated_table.find("1,234 (estimated)") != std::string::npos,
          "output token estimates use thousands separators and an estimated marker");

    std::string encoded;
    check(agent::settings_json_with_statistics("{}", summed, encoded).ok() &&
              encoded.find("\"cached_input_tokens\":230") != std::string::npos,
          "statistics persist into settings_json");
    agent::AgentSessionStatistics restored;
    check(agent::statistics_from_settings_json(encoded, restored).ok() &&
              restored.cached_input_tokens == 230 &&
              restored.fresh_input_tokens == 70 &&
              restored.output_tokens == 18 && restored.tool_calls == 4 &&
              restored.failed_tool_calls == 1 && restored.elapsed_ms == 3725000,
          "statistics restore from settings_json");

    const std::string json = agent::format_agent_statistics_json(summed);
    check(json.find("\"cache_hit_ratio\":76.7") != std::string::npos &&
              json.find("\"fail_percentage\":25.0") != std::string::npos &&
              json.find("\"elapsed_text\":\"1 hour, 2 minutes, 5 seconds\"") !=
                  std::string::npos &&
              json.find("\"markdown\":") != std::string::npos &&
              json.find("| Metric") != std::string::npos,
          "statistics JSON includes ratio, elapsed text, and GFM markdown for WebUI");

    const std::string workspace = temp_workspace("session-statistics");
    agent::AgentSessionRuntime runtime;
    agent::SessionRuntimeOptions options;
    options.workspace = workspace;
    options.interactive = true;
    options.enable_session_db = true;
    options.enable_agent_log = false;
    options.index_mode = agent::SessionRuntimeOptions::IndexMode::Disabled;
    provider::RequestContext context = offline_context(workspace);
    Error error = runtime.prepare(context, {}, {}, options);
    check(error.ok() && runtime.prepared(),
          "prepare for statistics persist: " + error.message);
    runtime.reset();
    {
        agent::AgentSessionStore store;
        check(store.open(workspace).ok(), "open project to write statistics");
        agent::AgentProjectRecord project;
        check(store.open_project(project).ok(), "load project to write statistics");
        check(agent::settings_json_with_statistics(project.settings_json, summed,
                                                   project.settings_json)
                  .ok(),
              "encode statistics into project settings");
        check(store.update_project_meta(project).ok(),
              "store persisted statistics");
    }
    error = runtime.prepare(context, {}, {}, options);
    check(error.ok() && runtime.prepared(),
          "re-prepare after statistics persist: " + error.message);
    const agent::AgentSessionStatistics live = runtime.session_statistics();
    check(live.cached_input_tokens == 230 && live.fresh_input_tokens == 70 &&
              live.output_tokens == 18 && live.tool_calls == 4 &&
              live.failed_tool_calls == 1 && live.elapsed_ms == 3725000,
          "prepare restores persisted session statistics");
    runtime.reset();
    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_reset_model_context_and_visible_clear() {
    const std::string workspace = temp_workspace("context-reset");
    {
        std::ofstream agents(fs::path(workspace) / "AGENTS.md");
        agents << "# Project\nFollow the local build rules.\n";
    }
    {
        agent::AgentSessionStore store;
        check(store.open(workspace).ok(), "reset fixture open");
        agent::AgentProjectRecord project;
        project.workspace = workspace;
        check(store.open_project(project).ok(), "reset fixture project");
        check(store.append_message("user", "old user prompt").ok(),
              "reset fixture user");
        check(store.append_message("assistant", "old assistant reply").ok(),
              "reset fixture assistant");
    }

    agent::AgentSessionRuntime runtime;
    agent::SessionRuntimeOptions options;
    options.workspace = workspace;
    options.interactive = true;
    options.enable_session_db = true;
    options.enable_agent_log = false;
    options.index_mode = agent::SessionRuntimeOptions::IndexMode::Disabled;
    provider::RequestContext context = offline_context(workspace);
    Error error = runtime.prepare(context, {}, {}, options);
    check(error.ok() && runtime.prepared(),
          "prepare reset fixture: " + error.message);

    std::vector<provider::Message> display;
    check(runtime.load_display_messages(display).ok() && display.size() >= 2,
          "display includes prior transcript before reset");
    error = runtime.set_goal("keep working until tests pass");
    check(error.ok() && agent::goal_is_active(runtime.goal()),
          "goal is active before reset");

    const auto notices = runtime.context_load_notices();
    check(!notices.empty() && notices.front().find("agent_prompt.md loaded ~") == 0,
          "load notice reports agent_prompt.md tokens");
    bool saw_agents = false;
    bool saw_tools = false;
    for (const std::string& line : notices) {
        saw_agents = saw_agents || line.find("AGENTS.md loaded ~") == 0;
        saw_tools = saw_tools || line.find("tools loaded ~") == 0;
    }
    check(saw_agents, "load notice reports AGENTS.md when present");
    check(saw_tools, "load notice reports tool schemas");

    error = runtime.reset_model_context();
    check(error.ok(), "reset_model_context: " + error.message);
    check(runtime.context_reset_after_seq() > 0, "reset persists a cut seq");
    check(!agent::goal_is_active(runtime.goal()) &&
              runtime.goal().status == agent::GoalStatus::Cleared,
          "reset clears the session goal");
    check(!runtime.visible_history_hidden(),
          "reset does not set the session-local /clear watermark");

    display.clear();
    check(runtime.load_display_messages(display).ok(), "load after reset");
    bool saw_old_user = false;
    bool saw_reset_notice = false;
    for (const provider::Message& message : display) {
        if (message.content.find("old user prompt") != std::string::npos)
            saw_old_user = true;
        if (message.content.find("Context reset") != std::string::npos)
            saw_reset_notice = true;
    }
    check(!saw_old_user, "display hides pre-reset transcript");
    check(saw_reset_notice, "display shows the durable reset notice");

    {
        agent::AgentSessionStore store;
        check(store.open(workspace).ok(), "reopen store after reset");
        std::vector<agent::AgentMessageRecord> rows;
        check(store.load_messages(rows).ok() && rows.size() >= 3,
              "sqlite still has pre-reset rows plus the reset notice");
        bool stored_old = false;
        for (const agent::AgentMessageRecord& row : rows) {
            if (row.content.find("old user prompt") != std::string::npos)
                stored_old = true;
        }
        check(stored_old, "full transcript remains on disk after reset");
    }

    const long long after_reset = runtime.estimated_request_tokens();
    check(after_reset > 0, "idle chrome after reset is seed overhead");
    runtime.reset();
    check(runtime.prepare(context, {}, {}, options).ok(),
          "re-prepare after reset");
    check(runtime.context_reset_after_seq() > 0,
          "reset cut reloads from project settings");
    display.clear();
    check(runtime.load_display_messages(display).ok(), "load after re-prepare");
    saw_old_user = false;
    for (const provider::Message& message : display) {
        if (message.content.find("old user prompt") != std::string::npos)
            saw_old_user = true;
    }
    check(!saw_old_user, "reopened session still hides pre-reset transcript");

    const long long reopened_tokens = runtime.estimated_request_tokens();
    check(runtime.hide_visible_history().ok(), "hide visible history");
    check(runtime.visible_history_hidden(), "/clear watermark is session-local");
    display.clear();
    check(runtime.load_display_messages(display).ok() && display.empty(),
          "/clear hides remaining visible rows without deleting sqlite");
    check(runtime.estimated_request_tokens() == reopened_tokens,
          "/clear does not wipe model context");

    runtime.reset();
    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_no_turn_session_log_finalization() {
    const std::string workspace = temp_workspace("no-turn-log");
    auto context = offline_context(workspace);
    context.options.agent_log_enabled = true;
    auto options = agent::make_session_runtime_options(context, workspace, agent::AgentLane::Act, true);
    options.index_mode = agent::SessionRuntimeOptions::IndexMode::Disabled;
    agent::AgentSessionRuntime runtime;
    check(runtime.prepare(context, {}, {}, options).ok(), "prepare a local no-turn session with logging");
    check(runtime.finish_session("success").ok(), "finish a session before its first model turn");
    size_t completed = 0, partial = 0;
    for (const auto& file : fs::directory_iterator(fs::u8path(workspace) / ".ainiux-pr/logs/agent")) {
        if (file.path().extension() == ".jsonl") ++completed;
        if (file.path().extension() == ".partial") ++partial;
    }
    check(completed == 1 && partial == 0,
          "no-turn graceful quit durably finalizes the diagnostic session log");
    runtime.reset();
    std::error_code ec;
    fs::remove_all(workspace, ec);
}

}  // namespace

void run_all() {
    test_no_turn_session_log_finalization();
    test_runtime_options_share_cli_projection();
    test_prepare_opens_session_db_and_tools();
    test_empty_turn_rejected_when_unprepared();
    test_prepare_with_indexing_disabled_never_touches_index_database();
    test_index_code_enables_a_skipped_index_in_place();
    test_index_report_refreshes_and_stays_display_only();
    test_optional_index_modes_create_refresh_and_fallback();
    test_agent_token_usage_aggregation_is_bounded();
    test_active_lane_switch_is_session_scoped_and_failure_safe();
    test_lane_handoffs_are_bounded_and_exclude_tools();
    test_prepare_loads_existing_display_history();
    test_manual_compaction_preserves_transcript_and_noops_until_new_history();
    test_summary_compaction_is_transactional_and_uses_active_api_context();
    test_project_replacement_resets_exact_state_and_switches_workspace();
    test_project_replacement_failure_reopens_prior_project();
    test_display_notices_dedupe_consecutive_duplicates();
    test_prepare_restores_live_conversation_instead_of_recap();
    test_snapshot_round_trip_and_compact_all_clears_restore();
    test_in_window_snapshot_is_not_rewritten_from_transcript();
    test_oversized_snapshot_reconstructs_last_session();
    test_session_goal_set_pause_resume_complete_and_persist();
    test_agent_session_statistics_format_and_persist();
    test_switch_permission_mode_persists_and_reloads();
    test_reset_model_context_and_visible_clear();
}

}  // namespace ainiux::test::agent_session_runtime
