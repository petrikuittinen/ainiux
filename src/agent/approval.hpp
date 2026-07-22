#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "runtime/runtime.hpp"

namespace ainiux::agent {

// Interactive Guard Ask request (PLANS.md §15.8). The model/tool never approves
// itself; only the user (or headless Deny) resolves Ask decisions.

struct GuardApprovalRequest {
    std::string tool_name;       // run_command | remove | …
    std::string command_preview; // human-readable command/path summary
    std::string rule_id;         // e.g. ask_on_destructive_git
    std::string message;         // why the guard asked
    std::vector<std::string> arguments;
};

enum class GuardApprovalDecision {
    Allow,
    Deny,
    Cancelled,  // job cancel / Esc while waiting without explicit n
};

// Synchronous approval callback (may block the tool worker). Cancellation should
// abort the wait without treating the decision as a user Allow.
using GuardApprovalCallback =
    std::function<GuardApprovalDecision(const GuardApprovalRequest&,
                                        runtime::CancellationToken)>;

std::string guard_approval_decision_name(GuardApprovalDecision decision);
std::string format_command_preview(const std::vector<std::string>& arguments);
std::string format_command_preview(const std::string& command_line);

// Thread-safe gate: tool worker blocks in request(); UI thread resolve()s.
// notify is invoked once when a request becomes pending (before the wait).
class ApprovalGate {
   public:
    using NotifyFn = std::function<void(const GuardApprovalRequest&)>;

    ApprovalGate() = default;
    ApprovalGate(const ApprovalGate&) = delete;
    ApprovalGate& operator=(const ApprovalGate&) = delete;

    void set_notify(NotifyFn notify);

    // Blocks until resolve/cancel_pending or cancellation token fires.
    GuardApprovalDecision request(const GuardApprovalRequest& req,
                                  runtime::CancellationToken cancellation =
                                      runtime::CancellationToken());

    bool has_pending() const;
    bool try_get_pending(GuardApprovalRequest& out) const;

    void resolve(GuardApprovalDecision decision);
    void cancel_pending();

   private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool pending_ = false;
    bool answered_ = false;
    GuardApprovalRequest request_;
    GuardApprovalDecision decision_ = GuardApprovalDecision::Deny;
    NotifyFn notify_;
};

}  // namespace ainiux::agent
