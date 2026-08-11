#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "agent/agent_loop.hpp"
#include "agent/activity.hpp"
#include "agent/agents_md.hpp"
#include "agent/approval.hpp"
#include "agent/compact.hpp"
#include "agent/goal.hpp"
#include "agent/prompts.hpp"
#include "agent/project_root.hpp"
#include "agent/review_log.hpp"
#include "agent/session_store.hpp"
#include "agent/tools.hpp"
#include "agent/attachment_bag.hpp"
#include "mcp/client.hpp"
#include "mcp/tool_bridge.hpp"
#include "common.hpp"
#include "provider/provider.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::agent {

// Long-lived agent workspace session for interactive TUI (and one-shot --run).
// Prepare once (index + tools + agent.sqlite + AGENTS.md); run multiple user
// turns without re-seeding the system prompt or re-opening the DB each time.

struct AgentTokenUsage {
    long long input_tokens = 0;
    long long fresh_input_tokens = 0;
    long long cache_read_tokens = 0;
    long long cache_write_tokens = 0;
    long long output_tokens = 0;
    // Number of model rounds included in these totals, whether provider-reported
    // or locally estimated.
    std::size_t reported_rounds = 0;
    // True when at least one round contributing to that total was estimated.
    bool input_estimated = false;
    bool output_estimated = false;
};

void accumulate_agent_token_usage(const provider::ChatResult& metrics,
                                  AgentTokenUsage& usage,
                                  long long estimated_input_tokens = 0,
                                  long long estimated_output_tokens = 0);

struct SessionTurnResult {
    Error error;
    std::string final_text;
    std::size_t turns = 0;        // model rounds this turn
    std::size_t tool_calls = 0;   // tool calls this turn
    std::size_t failed_tool_calls = 0;
    std::size_t session_turns = 0;
    std::size_t session_tool_calls = 0;
    std::size_t session_failed_tool_calls = 0;
    AgentTokenUsage token_usage;
    bool needs_user_continue = false;
    bool goal_completed = false;
    bool goal_stalled = false;
    std::string notice;
    std::vector<std::string> compact_tool_lines;  // timed rows, ready to persist/render
    // Wall-clock completion ms for transcript ordering only.
    std::vector<long long> compact_tool_line_ms;
    long long turn_started_ms = 0;
    long long finished_at_ms = 0;
};

enum class CompactionReason { Automatic, Manual };
struct SessionCompactionResult {
    Error error;
    bool compacted = false;
    bool no_op = false;
    CompactionStrategy requested_strategy = CompactionStrategy::Smart;
    CompactionStrategy applied_strategy = CompactionStrategy::Smart;
    long long tokens_before = 0;
    long long tokens_after = 0;
    std::string reason;
    std::string notice;
};

using CompactionSummaryCall = std::function<Error(
    const provider::RequestContext& context,
    const std::vector<provider::Message>& messages,
    int max_output_tokens,
    runtime::CancellationToken cancellation,
    std::string& summary)>;

struct SessionProjectReplaceResult {
    Error error;
    std::string workspace;
    std::string warning;
};

struct SessionIndexReportResult {
    Error error;
    std::string markdown;
    bool indexing_enabled = false;
    bool created = false;
    // Wall time for the whole /index-code or /show-index operation.
    long long elapsed_ms = 0;
};

struct SessionRuntimeOptions {
    std::string workspace = ".";
    AgentTaskMode task_mode = AgentTaskMode::Act;
    bool allow_network = true;  // fetch_url / web_search in agent mode
    bool interactive = false;  // turn-cap can return needs_user_continue
    bool enable_session_db = true;
    bool enable_agent_log = true;
    int security_review_log_keep_runs = 20;
    std::string trusted_prompt_dir;
    std::size_t max_source_code_file_size = 10 * 1024 * 1024;
    HistoryBackupPolicy history_backup;
    bool auto_compact = true;
    CompactionStrategy compact_strategy = CompactionStrategy::Smart;
    int compact_limit = 0;  // 0 = derive from window
    // Auto-continuations while a session goal is Active (tool-less FinalText).
    int max_goal_turns = 20;
    // Injected by tests and embedders. Empty uses the active provider/model.
    CompactionSummaryCall summary_call;
    enum class IndexMode {
        Disabled,
        UseExistingLazy,
        UseExisting = UseExistingLazy,
    };
    IndexMode index_mode = IndexMode::UseExistingLazy;
    std::function<void(const index::Progress&)> on_index_progress;
    bool show_command_output = false;
    // Headless callers always use Smart and retain Ask→Deny through an empty
    // approval callback. Interactive projects restore their persisted value.
    PermissionMode permission_mode = PermissionMode::Smart;
    fetch::Options fetch_options;
    search::Options search_options;
    std::function<void(const std::string& status_line)> on_progress;
    std::function<void(const AgentProgressUpdate&)> on_structured_progress;
    std::function<void(AgentActivityPhase)> on_phase;
    std::function<void(const PreparationProgress&)> on_prepare_progress;
    // Interactive Guard Ask (blocks tool worker until resolved). Empty ⇒ headless Deny.
    GuardApprovalCallback on_guard_ask;
};

class AgentSessionRuntime {
   public:
    AgentSessionRuntime() = default;
    AgentSessionRuntime(const AgentSessionRuntime&) = delete;
    AgentSessionRuntime& operator=(const AgentSessionRuntime&) = delete;
    AgentSessionRuntime(AgentSessionRuntime&&) = delete;
    AgentSessionRuntime& operator=(AgentSessionRuntime&&) = delete;

    bool prepared() const { return prepared_; }
    long long session_id() const { return session_id_; }
    const std::string& workspace() const { return options_.workspace; }
    const std::string& session_db_path() const { return session_store_.path(); }
    ToolProtocol protocol() const { return state_.protocol; }
    AgentTaskMode task_mode() const { return task_mode_; }
    MutationPolicy mutation_policy() const { return tools_.mutation_policy(); }
    PermissionMode permission_mode() const { return permission_mode_; }
    const SessionGoal& goal() const { return goal_; }
    // Set/replace Active goal (turns reset). Persists when the session DB is open.
    Error set_goal(const std::string& condition);
    Error clear_goal(const std::string& reason = {});
    Error pause_goal(const std::string& reason = {});
    Error resume_goal();
    Error mark_goal_complete(const std::string& evidence);
    bool indexing_enabled() const { return tools_.indexing_enabled(); }
    // Cheap, non-blocking handoff invoked by surfaces only after they have
    // published Agent readiness.
    void begin_background_index_freshness();
    std::size_t session_turns() const { return session_turns_; }
    std::size_t session_tool_calls() const { return session_tool_calls_; }
    std::size_t session_failed_tool_calls() const {
        return session_failed_tool_calls_;
    }

    // Thread-safe: returns last published estimate (worker updates only).
    // Never walks conversation_ from the UI thread — that races with run_user_turn.
    // Pure model-visible request size; does not include in-flight generation.
    long long estimated_request_tokens() const;
    // Last non-zero published request estimate. Used by the TUI when the live
    // cache is temporarily 0 (e.g. between prepare and first seed) so the
    // status line does not claim an empty context after compact/resume.
    long long last_nonzero_request_tokens() const;
    // Local estimate of streamed reasoning/output not yet committed into the
    // request conversation. TUI chrome may add this to estimated_request_tokens
    // for a live meter; compaction must not.
    long long in_flight_generation_tokens() const;

    // Refresh index, create tools, open agent.sqlite, load prompts/AGENTS.md.
    // Does not create a session row until the first user turn.
    Error prepare(const provider::RequestContext& context,
                  runtime::CancellationToken cancellation = runtime::CancellationToken(),
                  std::function<bool()> interrupted = {},
                  SessionRuntimeOptions options = {});

    // After prepare: load the project transcript for TUI display. Internal
    // summary/checkpoint rows stay durable but are omitted from visible history.
    Error load_display_messages(std::vector<provider::Message>& out) const;

    // Append a display-only notice to the project agent transcript (e.g. user /shell).
    // No-op when the session DB is not open.
    Error append_display_notice(const std::string& content);

    // Persist the active provider/model/API endpoint and request settings for
    // restoration the next time this project opens in interactive agent mode.
    Error update_project_settings(const provider::RequestContext& context);

    // Switch the trusted task prompt and tool policy without resetting session
    // history. Only valid while no turn/compaction operation is active.
    Error switch_task_mode(AgentTaskMode mode);
    // Persist first, then publish to the live registry. On failure the active
    // mode is unchanged.
    Error switch_permission_mode(PermissionMode mode,
                                 const provider::RequestContext& context);

    // One user submit for the agent tool loop. Images are request-local: included
    // on every model round of this turn, then stripped from the live conversation.
    // They are never written to agent.sqlite or project media.
    struct UserTurnPayload {
        std::string text;
        std::vector<provider::ImageInput> images;
    };

    // Run one user goal/follow-up until FinalText, NeedsUserContinue, abort, or error.
    // First turn seeds the tool conversation; later turns append a user message.
    // Optional on_progress receives compact tool lines (and brief notices) as they
    // happen so interactive UIs can stream them without waiting for the final answer.
    SessionTurnResult run_user_turn(
        provider::RequestContext& context,
        UserTurnPayload payload,
        runtime::CancellationToken cancellation = runtime::CancellationToken(),
        std::function<bool()> interrupted = {},
        std::function<void(const std::string& status_line)> on_progress = {},
        std::function<void(const AgentProgressUpdate&)> on_structured_progress = {});

    // Text-only convenience overload (headless --run / tests).
    SessionTurnResult run_user_turn(
        provider::RequestContext& context,
        const std::string& user_text,
        runtime::CancellationToken cancellation = runtime::CancellationToken(),
        std::function<bool()> interrupted = {},
        std::function<void(const std::string& status_line)> on_progress = {},
        std::function<void(const AgentProgressUpdate&)> on_structured_progress = {}) {
        return run_user_turn(context, UserTurnPayload{user_text, {}}, cancellation,
                             std::move(interrupted), std::move(on_progress),
                             std::move(on_structured_progress));
    }

    // Compact only the model-visible request projection. The durable transcript
    // remains complete. Manual compaction bypasses the configured threshold.
    SessionCompactionResult compact(
        const provider::RequestContext& context,
        CompactionReason reason,
        runtime::CancellationToken cancellation = runtime::CancellationToken(),
        std::optional<CompactionStrategy> strategy_override = std::nullopt);

    // Optionally refresh the live project index, persist a display-only totals
    // table, and return it for immediate Agent-history rendering.
    SessionIndexReportResult show_index(
        bool refresh,
        runtime::CancellationToken cancellation = runtime::CancellationToken());
    // Create and enable a skipped/missing index in the live Agent session.
    // If indexing is already enabled, this behaves as an incremental refresh.
    SessionIndexReportResult index_code(
        runtime::CancellationToken cancellation = runtime::CancellationToken());

    // Close the current project, initialize a fresh target, and restore the old
    // project on failure. target.state_dir_exists means the caller already
    // obtained explicit deletion confirmation.
    SessionProjectReplaceResult replace_project(
        const provider::RequestContext& context,
        const NewProjectTarget& target,
        runtime::CancellationToken cancellation = runtime::CancellationToken(),
        std::optional<bool> indexing_enabled = std::nullopt);

    // Mark session finished in agent.sqlite (success/error/cancelled/aborted).
    Error finish_session(const std::string& status,
                         const std::string& final_text = {},
                         const std::string& error_code = {},
                         const std::string& error_message = {});

    // Release tools/DB; safe to call multiple times.
    void reset();

   private:
    bool is_interrupted(runtime::CancellationToken cancellation,
                        const std::function<bool()>& interrupted) const;
    // Worker-only: recompute from conversation_ and publish for UI chrome.
    void publish_request_token_estimate();
    // Worker-only: publish or clear the throttled in-flight generation estimate.
    void publish_in_flight_generation_tokens(long long tokens);
    void clear_in_flight_generation_tokens();
    // Fixed per-request overhead always present after seed: system prompt,
    // optional AGENTS.md, Act/Plan control, and native tool schemas.
    long long estimate_seed_overhead_tokens() const;
    // Compaction "before" size: live conversation when seeded; otherwise seed
    // overhead + full durable model-projection transcript (not the bounded
    // reopen prior block used by idle chrome).
    long long estimate_compact_tokens_before(
        const std::vector<AgentMessageRecord>& stored) const;
    void rebuild_compacted_conversation(const CompactionPartition& partition,
                                        const std::string& checkpoint);
    SessionCompactionResult compact_impl(
        const provider::RequestContext& context,
        CompactionReason reason,
        runtime::CancellationToken cancellation,
        std::optional<CompactionStrategy> strategy_override = std::nullopt,
        bool forced_summary = false);

    Error persist_goal_settings();
    void inject_active_goal_control(bool continue_nudge);

    SessionRuntimeOptions options_;
    AgentTaskMode task_mode_ = AgentTaskMode::Act;
    PermissionMode permission_mode_ = PermissionMode::Smart;
    SessionGoal goal_;
    bool prepared_ = false;
    bool conversation_seeded_ = false;
    std::vector<std::string> secrets_;
    std::unique_ptr<ReviewLogger> logger_;
    AgentSessionStore session_store_;
    long long session_id_ = 0;
    ReadToolRegistry tools_;
    std::shared_ptr<mcp::Manager> mcp_manager_;
    std::unique_ptr<mcp::ToolBridge> mcp_bridge_;
    // Per user-turn bag for MCP image rewrite and optional vision.
    AttachmentBag attachment_bag_;
    TrustedPrompts prompts_;
    AgentsMdBundle agents_md_;
    provider::ToolConversation conversation_;
    AgentLoopState state_;
    AgentLoopLimits limits_;
    std::vector<std::string> known_tools_;
    std::size_t session_turns_ = 0;
    std::size_t session_tool_calls_ = 0;
    std::size_t session_failed_tool_calls_ = 0;
    // Published by the agent worker; read by the TUI render path without locking conversation_.
    mutable std::atomic<long long> cached_request_tokens_{0};
    mutable std::atomic<long long> last_nonzero_request_tokens_{0};
    mutable std::atomic<long long> in_flight_generation_tokens_{0};
    // Total steady-clock time spent waiting for interactive Guard decisions.
    // The tool executor snapshots this counter to exclude approval waits.
    std::atomic<long long> guard_approval_wait_ms_{0};
    std::atomic<bool> operation_active_{false};
    long long last_auto_compact_failure_ms_ = 0;
    long long last_auto_compact_failure_seq_ = 0;
};

}  // namespace ainiux::agent
