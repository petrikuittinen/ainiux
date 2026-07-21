#pragma once

#include <cstddef>
#include <string>
#include <vector>

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
};

// InspectionOnly: security-review allowlist (read-only ls/rg/find/git/...).
// Agent: inspection set plus common build/test/run tools (python3, make, …),
// still without a shell, with destructive-command guard applied.
enum class CommandPolicy {
    InspectionOnly,
    Agent,
};

Error parse_inspection_command(const std::string& command, std::vector<std::string>& arguments);
Error parse_command(const std::string& command,
                    std::vector<std::string>& arguments,
                    CommandPolicy policy,
                    std::string& guard_rule_id);
Error run_inspection_command(const std::string& command,
                             const ProcessOptions& options,
                             ProcessResult& result);
Error run_command(const std::string& command,
                  const ProcessOptions& options,
                  ProcessResult& result,
                  CommandPolicy policy = CommandPolicy::InspectionOnly);

}  // namespace ainiux::agent
