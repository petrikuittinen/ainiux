#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "agent/approval.hpp"
#include "common.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::agent {

struct ProcessOptions {
    std::string workspace;
    std::string cwd;
    bool allow_external_cwd = false;
    // Set only after the tool layer has validated and authorized every absolute
    // operand. Direct/headless process callers retain the deny-by-default policy.
    bool allow_external_paths = false;
    // Interactive Yolo: skip hard Guard denials (shells, sudo, …) at the user's
    // risk. Confirm/Smart keep the denylist. Unquoted shell control operators
    // still fail closed because run_command never spawns a real shell.
    bool unrestricted = false;
    // Agent: resolve ./scripts and bare workspace executables under cwd/root.
    bool allow_workspace_executables = false;
    long timeout_ms = 10000;
    std::size_t stdout_limit = 65536;
    std::size_t stderr_limit = 65536;
    runtime::CancellationToken cancellation;
    // When set, GuardDecision::Ask prompts the user (one-shot). When empty,
    // Ask is denied (headless). Deny is never elevatable (unless unrestricted).
    GuardApprovalCallback on_guard_ask;
};

struct ProcessResult {
    std::vector<std::string> arguments;
    std::string resolved_executable;
    std::string cwd;
    std::string stdout_text;
    std::string stderr_text;
    std::int64_t exit_status = -1;
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

// InspectionOnly: security-review's historical narrow snapshot allowlist.
// PlanReadOnly: expanded, conservatively classified read-only argv forms.
// Agent: default-allow any basename found on a fixed PATH, shell-free execve,
// structural argument safety, and a hard denylist / Guard for dangerous forms.
enum class CommandPolicy {
    InspectionOnly,
    PlanReadOnly,
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
                    runtime::CancellationToken cancellation = runtime::CancellationToken(),
                    bool allow_absolute_paths = false,
                    bool unrestricted = false);
Error run_inspection_command(const std::string& command,
                             const ProcessOptions& options,
                             ProcessResult& result);
Error run_command(const std::string& command,
                  const ProcessOptions& options,
                  ProcessResult& result,
                  CommandPolicy policy = CommandPolicy::InspectionOnly);
// Direct argv execution without shell tokenization. Same fixed PATH, Guard, and
// policy checks as run_command. Prefer this when arguments may contain spaces
// or quotes (e.g. grep ripgrep backend).
Error run_argv(std::vector<std::string> arguments,
               const ProcessOptions& options,
               ProcessResult& result,
               CommandPolicy policy = CommandPolicy::InspectionOnly);
// True when a trusted system `rg` binary is on the fixed process PATH.
bool ripgrep_available();

}  // namespace ainiux::agent
