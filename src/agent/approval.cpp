#include "agent/approval.hpp"

#include <chrono>
#include <sstream>

namespace ainiux::agent {

const char* permission_mode_name(PermissionMode mode) {
    switch (mode) {
        case PermissionMode::Confirm:
            return "confirm";
        case PermissionMode::Smart:
            return "smart";
        case PermissionMode::Yolo:
            return "yolo";
    }
    return "smart";
}

bool parse_permission_mode(const std::string& text, PermissionMode& mode) {
    if (text == "confirm") {
        mode = PermissionMode::Confirm;
        return true;
    }
    if (text == "smart") {
        mode = PermissionMode::Smart;
        return true;
    }
    if (text == "yolo") {
        mode = PermissionMode::Yolo;
        return true;
    }
    return false;
}

std::string guard_approval_decision_name(GuardApprovalDecision decision) {
    switch (decision) {
        case GuardApprovalDecision::Allow:
            return "allow";
        case GuardApprovalDecision::Deny:
            return "deny";
        case GuardApprovalDecision::Cancelled:
            return "cancelled";
    }
    return "deny";
}

std::string format_command_preview(const std::vector<std::string>& arguments) {
    if (arguments.empty()) return {};
    std::ostringstream out;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        if (i) out << ' ';
        const std::string& arg = arguments[i];
        bool need_quote = arg.empty();
        for (char ch : arg) {
            if (ch == ' ' || ch == '\t' || ch == '"' || ch == '\'') {
                need_quote = true;
                break;
            }
        }
        if (!need_quote) {
            out << arg;
            continue;
        }
        out << '"';
        for (char ch : arg) {
            if (ch == '"' || ch == '\\') out << '\\';
            out << ch;
        }
        out << '"';
    }
    return out.str();
}

std::string format_command_preview(const std::string& command_line) {
    return command_line;
}

void ApprovalGate::set_notify(NotifyFn notify) {
    std::lock_guard<std::mutex> lock(mutex_);
    notify_ = std::move(notify);
}

GuardApprovalDecision ApprovalGate::request(const GuardApprovalRequest& req,
                                            runtime::CancellationToken cancellation) {
    NotifyFn notify_copy;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        // Serialize concurrent Ask prompts (tools run one-at-a-time today; still safe).
        cv_.wait(lock, [&] {
            return !pending_ || cancellation.cancelled();
        });
        if (cancellation.cancelled()) return GuardApprovalDecision::Cancelled;

        pending_ = true;
        answered_ = false;
        request_ = req;
        decision_ = GuardApprovalDecision::Deny;
        notify_copy = notify_;
    }
    if (notify_copy) notify_copy(req);

    std::unique_lock<std::mutex> lock(mutex_);
    while (!answered_ && !cancellation.cancelled()) {
        cv_.wait_for(lock, std::chrono::milliseconds(50));
    }
    if (!answered_) {
        // Cancellation while waiting for the user.
        pending_ = false;
        answered_ = true;
        decision_ = GuardApprovalDecision::Cancelled;
        cv_.notify_all();
        return GuardApprovalDecision::Cancelled;
    }
    const GuardApprovalDecision result = decision_;
    pending_ = false;
    answered_ = false;
    cv_.notify_all();
    return result;
}

bool ApprovalGate::has_pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_ && !answered_;
}

bool ApprovalGate::try_get_pending(GuardApprovalRequest& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pending_ || answered_) return false;
    out = request_;
    return true;
}

void ApprovalGate::resolve(GuardApprovalDecision decision) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pending_ || answered_) return;
    decision_ = decision;
    answered_ = true;
    cv_.notify_all();
}

void ApprovalGate::cancel_pending() {
    resolve(GuardApprovalDecision::Cancelled);
}

}  // namespace ainiux::agent
