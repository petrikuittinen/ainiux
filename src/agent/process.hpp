#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "agent/approval.hpp"
#include "common.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::agent {

struct ProcessOptions {
    std::string workspace;
    std::string cwd;
    long timeout_ms = 10000;
    std::size_t stdout_limit = 65536;
    std::size_t stderr_limit = 65536;
    runtime::CancellationToken cancellation;
    // When set, GuardDecision::Ask prompts the user (one-shot). When empty,
    // Ask is denied (headless). Deny is never elevatable.
    GuardApprovalCallback on_guard_ask;
};

struct ProcessResult {
    std::vector<std::string> arguments;
    std::string cwd;
    std::string stdout_text;
    std::string stderr_text;
    int exit_status = -1;
    int signal = 0;
    long long duration_ms = 0;
    bool stdout_truncated = false;
    bool stderr_truncated = false;
    bool timed_out = false;
    bool cancelled = false;
    std::string policy = "denied";
    std::string guard_rule_id;
    std::string guard_decision;  // allow | deny | ask→resolved
};

// InspectionOnly: security-review read-only allowlist (pwd/ls/rg/find/git/…).
// Agent: default-allow any basename found on a fixed PATH, shell-free execve,
// structural argument safety, and a hard denylist / Guard for dangerous forms.
// Do not grow per-command option allowlists for agent mode.
enum class CommandPolicy {
    InspectionOnly,
    Agent,
};

// How GuardDecision::Ask is handled during parse/enforce.
enum class GuardAskHandling {
    DenyAsk,   // headless default: Ask becomes Deny
    DeferAsk,  // parse succeeds; caller must re-check on execute (path validation)
    PromptAsk, // call ProcessOptions.on_guard_ask (or deny if unset)
};

Error parse_inspection_command(const std::string& command, std::vector<std::string>& arguments);
Error parse_command(const std::string& command,
                    std::vector<std::string>& arguments,
                    CommandPolicy policy,
                    std::string& guard_rule_id,
                    GuardAskHandling ask_handling = GuardAskHandling::DenyAsk,
                    const GuardApprovalCallback* on_guard_ask = nullptr,
                    runtime::CancellationToken cancellation = runtime::CancellationToken());
Error run_inspection_command(const std::string& command,
                             const ProcessOptions& options,
                             ProcessResult& result);
Error run_command(const std::string& command,
                  const ProcessOptions& options,
                  ProcessResult& result,
                  CommandPolicy policy = CommandPolicy::InspectionOnly);

}  // namespace ainiux::agent

