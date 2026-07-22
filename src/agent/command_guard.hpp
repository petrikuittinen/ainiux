#pragma once

#include <string>
#include <vector>

namespace ainiux::agent {

// Cheap destructive-command guard (PLANS.md §14 / Milestone 6).
// Headless agent maps Ask → Deny. Interactive agent may prompt via ApprovalGate.

enum class GuardDecision {
    Allow,
    Deny,
    Ask,  // requires one-shot user approval in interactive agent
};

struct GuardResult {
    GuardDecision decision = GuardDecision::Allow;
    std::string rule_id;
    std::string message;
};

// Evaluate argv after shell-tokenization (no shell metacharacters expected).
GuardResult evaluate_command_guard(const std::vector<std::string>& arguments);

// Convenience for a single command line string (tokenizes lightly on spaces;
// prefer the argv form when available).
GuardResult evaluate_command_guard_line(const std::string& command_line);

// Map Ask → Deny for headless one-shot agent (no approval UI).
GuardResult finalize_guard_for_headless(GuardResult result);

}  // namespace ainiux::agent
