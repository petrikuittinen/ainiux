#include "agent/agent_controller.hpp"

#include <utility>

namespace ainiux::agent {

AgentController::AgentController()
    : runtime_(std::make_shared<AgentSessionRuntime>()),
      gate_(std::make_shared<ApprovalGate>()) {
    arm_guard_notify();
}

AgentController::~AgentController() {
    shutdown(true, "agent controller destroyed");
    clear_guard_notify();
    AgentSurfaceEvent discarded;
    while (events_.try_pop(discarded)) {
    }
    events_.close();
}

bool AgentController::prepared() const {
    return runtime_ && runtime_->prepared();
}

bool AgentController::turn_running() const {
    return turn_running_.load(std::memory_order_acquire);
}

bool AgentController::job_joinable() const {
    return turn_job_.joinable();
}

bool AgentController::waiting_guard() const {
    return waiting_guard_.load(std::memory_order_acquire) ||
           (gate_ && gate_->has_pending());
}

std::string AgentController::status_label() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_label_;
}

void AgentController::set_status_label(std::string label) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_label_ = std::move(label);
}

std::chrono::steady_clock::time_point AgentController::turn_started() const {
    return turn_started_;
}

void AgentController::arm_guard_notify() {
    if (!gate_) return;
    std::weak_ptr<ApprovalGate> weak_gate = gate_;
    gate_->set_notify([this, weak_gate](const GuardApprovalRequest& request) {
        if (weak_gate.expired()) return;
        waiting_guard_.store(true, std::memory_order_release);
        set_status_label("Agent waiting for Guard approval");
        AgentSurfaceEvent event;
        event.type = AgentSurfaceEvent::Type::GuardApproval;
        event.guard_tool_name = request.tool_name;
        event.guard_command_preview = request.command_preview;
        event.guard_rule_id = request.rule_id;
        event.guard_message = request.message;
        events_.push(std::move(event));
    });
}

void AgentController::clear_guard_notify() {
    if (gate_) gate_->set_notify({});
}

void AgentController::cancel_turn() {
    if (gate_) gate_->cancel_pending();
    waiting_guard_.store(false, std::memory_order_release);
    turn_job_.cancel();
}

void AgentController::join_turn() {
    turn_job_.join();
    turn_running_.store(false, std::memory_order_release);
    waiting_guard_.store(false, std::memory_order_release);
}

void AgentController::shutdown(bool finish_open_session, const std::string& reason) {
    cancel_turn();
    join_turn();
    if (finish_open_session && runtime_ && runtime_->prepared() &&
        runtime_->session_id() > 0) {
        const std::string note =
            reason.empty() ? std::string("agent controller shutdown") : reason;
        (void)runtime_->finish_session("cancelled", "", "Cancelled", note);
    }
    // Keep events_ open so a long-lived controller can be reused after a
    // temporary editor hop. The destructor drains and closes.
}

bool AgentController::start_turn(TurnWork work) {
    if (!work) return false;
    if (turn_running_.load(std::memory_order_acquire)) return false;
    turn_running_.store(true, std::memory_order_release);
    turn_started_ = std::chrono::steady_clock::now();
    set_status_label("Agent running");
    turn_job_.start([this, work = std::move(work)](runtime::CancellationToken token) mutable {
        AgentSurfaceEvent event;
        try {
            event = work(token);
        } catch (...) {
            event.type = AgentSurfaceEvent::Type::TurnError;
            event.error = {ErrorCode::Internal, "agent turn failed with an unexpected exception"};
            event.agent_turn = true;
        }
        if (event.type != AgentSurfaceEvent::Type::TurnDone &&
            event.type != AgentSurfaceEvent::Type::TurnError) {
            // Defensive: treat unknown completion as error so the UI clears busy.
            if (event.error.ok()) {
                event.type = AgentSurfaceEvent::Type::TurnDone;
            } else {
                event.type = AgentSurfaceEvent::Type::TurnError;
            }
        }
        waiting_guard_.store(false, std::memory_order_release);
        turn_running_.store(false, std::memory_order_release);
        if (event.type == AgentSurfaceEvent::Type::TurnDone) {
            set_status_label("Agent done");
        } else if (event.error.code == ErrorCode::Cancelled) {
            set_status_label("Agent cancelled");
        } else {
            set_status_label(event.error.message.empty() ? "Agent error"
                                                         : event.error.message);
        }
        events_.push(std::move(event));
    });
    return true;
}

bool AgentController::preserve_across_editor() const {
    return prepared() || turn_running() || job_joinable();
}

std::shared_ptr<AgentController> ensure_agent_controller(
    std::shared_ptr<AgentController>& slot) {
    if (!slot) slot = std::make_shared<AgentController>();
    return slot;
}

}  // namespace ainiux::agent
