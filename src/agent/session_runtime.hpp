#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "agent/agent_loop.hpp"
#include "agent/agents_md.hpp"
#include "agent/approval.hpp"
#include "agent/prompts.hpp"
#include "agent/review_log.hpp"
#include "agent/session_store.hpp"
#include "agent/tools.hpp"
#include "common.hpp"
#include "provider/provider.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::agent {

// Long-lived agent workspace session for interactive TUI (and one-shot --run).
// Prepare once (index + tools + agent.sqlite + AGENTS.md); run multiple user
// turns without re-seeding the system prompt or re-opening the DB each time.

struct SessionTurnResult {
    Error error;
    std::string final_text;
    std::size_t turns = 0;        // model rounds this turn
    std::size_t tool_calls = 0;   // tool calls this turn
    std::size_t session_turns = 0;
    std::size_t session_tool_calls = 0;
    bool needs_user_continue = false;
    std::string notice;
    std::vector<std::string> compact_tool_lines;  // this turn (no elapsed suffix)
    // Wall-clock ms for each compact_tool_lines entry / final answer / turn start.
    // Used by the agent TUI to show tool timing ("N ms") and final "Task complete…".
    std::vector<long long> compact_tool_line_ms;
    long long turn_started_ms = 0;
    long long finished_at_ms = 0;
};

struct SessionRuntimeOptions {
    std::string workspace = ".";
    bool allow_mutations = true;
    bool allow_network = true;  // fetch_url / search_web in agent mode
    bool interactive = false;  // turn-cap can return needs_user_continue
    bool enable_session_db = true;
    bool enable_agent_log = true;
    int security_review_log_keep_runs = 20;
    std::string trusted_prompt_dir;
    std::size_t max_source_code_file_size = 10 * 1024 * 1024;
    HistoryBackupPolicy history_backup;
    bool auto_compact = true;
    int compact_limit = 0;  // 0 = derive from window
    bool show_command_output = false;
    fetch::Options fetch_options;
    search::Options search_options;
    std::function<void(const std::string& status_line)> on_progress;
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
    std::size_t session_turns() const { return session_turns_; }
    std::size_t session_tool_calls() const { return session_tool_calls_; }

    // Thread-safe: returns last published estimate (worker updates only).
    // Never walks conversation_ from the UI thread — that races with run_user_turn.
    long long estimated_request_tokens() const;

    // Refresh index, create tools, open agent.sqlite, load prompts/AGENTS.md.
    // Does not create a session row until the first user turn.
    Error prepare(const provider::RequestContext& context,
                  runtime::CancellationToken cancellation = runtime::CancellationToken(),
                  std::function<bool()> interrupted = {},
                  SessionRuntimeOptions options = {});

    // After prepare: load the project transcript for TUI display (roles
    // user/assistant/tool/notice/summary). Empty when the DB has no messages.
    Error load_display_messages(std::vector<provider::Message>& out) const;

    // Append a display-only notice to the project agent transcript (e.g. user /shell).
    // No-op when the session DB is not open.
    Error append_display_notice(const std::string& content);

    // Run one user goal/follow-up until FinalText, NeedsUserContinue, abort, or error.
    // First turn seeds the tool conversation; later turns append a user message.
    // Optional on_progress receives compact tool lines (and brief notices) as they
    // happen so interactive UIs can stream them without waiting for the final answer.
    SessionTurnResult run_user_turn(
        provider::RequestContext& context,
        const std::string& user_text,
        runtime::CancellationToken cancellation = runtime::CancellationToken(),
        std::function<bool()> interrupted = {},
        std::function<void(const std::string& status_line)> on_progress = {});

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

    SessionRuntimeOptions options_;
    bool prepared_ = false;
    bool conversation_seeded_ = false;
    std::vector<std::string> secrets_;
    std::unique_ptr<ReviewLogger> logger_;
    AgentSessionStore session_store_;
    long long session_id_ = 0;
    ReadToolRegistry tools_;
    TrustedPrompts prompts_;
    AgentsMdBundle agents_md_;
    provider::ToolConversation conversation_;
    AgentLoopState state_;
    AgentLoopLimits limits_;
    std::vector<std::string> known_tools_;
    std::size_t session_turns_ = 0;
    std::size_t session_tool_calls_ = 0;
    // Published by the agent worker; read by the TUI render path without locking conversation_.
    mutable std::atomic<long long> cached_request_tokens_{0};
};

}  // namespace ainiux::agent
