#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "agent/activity.hpp"
#include "agent/approval.hpp"
#include "agent/index/index.hpp"
#include "agent/session_runtime.hpp"
#include "common.hpp"
#include "provider/provider.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::agent {

// Surface-neutral events from a background agent prepare/turn. The TUI maps
// these into its own event type; the editor may poll status/Guard without
// owning the alternate-screen agent shell.
struct AgentSurfaceEvent {
    enum class Type {
        Progress,
        Phase,
        PrepareProgress,
        IndexProgress,
        PrepareDone,
        TurnDone,
        TurnError,
        GuardApproval,
    };

    Type type = Type::Progress;
    Error error;
    std::string text;
    bool agent_turn = false;
    std::vector<std::string> agent_tool_lines;
    std::vector<long long> agent_tool_line_ms;
    std::string agent_final_text;
    std::size_t agent_turns = 0;
    std::size_t agent_tool_calls = 0;
    std::size_t agent_failed_tool_calls = 0;
    bool agent_needs_user_continue = false;
    long long agent_turn_started_ms = 0;
    long long agent_finished_at_ms = 0;
    long long agent_stream_output_tokens = 0;
    long long agent_stream_decode_ms = 0;
    bool agent_stream_tokens_estimated = false;
    long long agent_elapsed_ms = -1;
    AgentTokenUsage agent_token_usage;
    long long agent_context_used_tokens = -1;
    long long agent_context_window_tokens = -1;
    provider::ChatResult chat;
    std::vector<provider::Message> agent_history;
    bool agent_history_loaded = false;
    bool agent_index_enabled = false;
    AgentActivityPhase agent_phase = AgentActivityPhase::Thinking;
    AgentProgressUpdate agent_progress;
    PreparationProgress agent_prepare_progress;
    index::Progress agent_index_progress;
    std::string guard_tool_name;
    std::string guard_command_preview;
    std::string guard_rule_id;
    std::string guard_message;
    std::string guard_review_path;
};

// Owns the multi-turn agent workspace session and (optionally) an in-flight
// user-turn job so the interactive product can visit the editor without
// tearing down a running agent.
class AgentController {
   public:
    AgentController();
    ~AgentController();

    AgentController(const AgentController&) = delete;
    AgentController& operator=(const AgentController&) = delete;

    std::shared_ptr<AgentSessionRuntime> runtime() const { return runtime_; }
    std::shared_ptr<ApprovalGate> approval_gate() const { return gate_; }
    runtime::EventQueue<AgentSurfaceEvent>& events() { return events_; }

    bool prepared() const;
    bool turn_running() const;
    bool job_joinable() const;
    bool waiting_guard() const;

    // Last published status line for non-agent surfaces (editor badge).
    std::string status_label() const;
    void set_status_label(std::string label);

    std::chrono::steady_clock::time_point turn_started() const;

    // Install notify so Guard Ask pushes GuardApproval into events_.
    void arm_guard_notify();
    void clear_guard_notify();

    // Cancel the in-flight turn (if any) and join. Does not finish the DB session.
    void cancel_turn();
    void join_turn();

    // Cancel + join turn; optionally finish the open project session.
    void shutdown(bool finish_open_session, const std::string& reason = {});

    // Start a turn job. Caller must ensure !turn_running(). The worker posts
    // progress/Done/Error to events_ and clears turn_running_ on completion.
    // `work` receives the cancellation token and should return a TurnDone or
    // TurnError event (without posting it); the controller posts and clears.
    using TurnWork = std::function<AgentSurfaceEvent(runtime::CancellationToken)>;
    bool start_turn(TurnWork work);

    // True when a temporary editor hop should keep the controller alive.
    // Always true for this controller while prepared or running; chat leave
    // still calls shutdown(true).
    bool preserve_across_editor() const;

   private:
    std::shared_ptr<AgentSessionRuntime> runtime_;
    std::shared_ptr<ApprovalGate> gate_;
    runtime::JobHandle turn_job_;
    runtime::EventQueue<AgentSurfaceEvent> events_;
    std::atomic<bool> turn_running_{false};
    std::atomic<bool> waiting_guard_{false};
    mutable std::mutex status_mutex_;
    std::string status_label_;
    std::chrono::steady_clock::time_point turn_started_{};
};

// Ensure InteractiveSession (or caller) has a controller instance.
std::shared_ptr<AgentController> ensure_agent_controller(
    std::shared_ptr<AgentController>& slot);

}  // namespace ainiux::agent
