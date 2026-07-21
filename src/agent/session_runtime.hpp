#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "agent/agent_loop.hpp"
#include "agent/agents_md.hpp"
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
};

struct SessionRuntimeOptions {
    std::string workspace = ".";
    bool allow_mutations = true;
    bool interactive = false;  // turn-cap can return needs_user_continue
    bool enable_session_db = true;
    bool enable_agent_log = true;
    int security_review_log_keep_runs = 20;
    std::string trusted_prompt_dir;
    std::size_t max_source_code_file_size = 10 * 1024 * 1024;
    std::function<void(const std::string& status_line)> on_progress;
};

class AgentSessionRuntime {
   public:
    AgentSessionRuntime() = default;
    AgentSessionRuntime(const AgentSessionRuntime&) = delete;
    AgentSessionRuntime& operator=(const AgentSessionRuntime&) = delete;
    AgentSessionRuntime(AgentSessionRuntime&&) noexcept = default;
    AgentSessionRuntime& operator=(AgentSessionRuntime&&) noexcept = default;

    bool prepared() const { return prepared_; }
    long long session_id() const { return session_id_; }
    const std::string& workspace() const { return options_.workspace; }
    const std::string& session_db_path() const { return session_store_.path(); }
    ToolProtocol protocol() const { return state_.protocol; }
    std::size_t session_turns() const { return session_turns_; }
    std::size_t session_tool_calls() const { return session_tool_calls_; }

    // Refresh index, create tools, open agent.sqlite, load prompts/AGENTS.md.
    // Does not create a session row until the first user turn.
    Error prepare(const provider::RequestContext& context,
                  runtime::CancellationToken cancellation = runtime::CancellationToken(),
                  std::function<bool()> interrupted = {},
                  SessionRuntimeOptions options = {});

    // Run one user goal/follow-up until FinalText, NeedsUserContinue, abort, or error.
    // First turn seeds the tool conversation; later turns append a user message.
    SessionTurnResult run_user_turn(provider::RequestContext& context,
                                    const std::string& user_text,
                                    runtime::CancellationToken cancellation = runtime::CancellationToken(),
                                    std::function<bool()> interrupted = {});

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
};

}  // namespace ainiux::agent
