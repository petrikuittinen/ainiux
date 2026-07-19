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
};

Error parse_inspection_command(const std::string& command, std::vector<std::string>& arguments);
Error run_inspection_command(const std::string& command,
                             const ProcessOptions& options,
                             ProcessResult& result);

}  // namespace ainiux::agent
