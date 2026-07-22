#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "common.hpp"
#include "provider/provider.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::app {

struct UserShellOptions {
    std::string cwd;  // empty → process cwd
    long timeout_ms = 60000;
    std::size_t stdout_limit = 256 * 1024;
    std::size_t stderr_limit = 256 * 1024;
    runtime::CancellationToken cancellation;
};

struct UserShellResult {
    std::string command;
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
};

// Where user shell output is delivered after the job completes.
enum class UserShellDestination {
    Notice,  // /shell, !cmd — display-only history notice
    Draft,   // /shell-stdout, !!cmd — pure stdout into TUI input draft
};

// True when trimmed input is a user shell invocation.
// On true: command_out holds the shell command body (may be empty → use error_out).
// On false: not a shell invocation (ordinary chat / other slash command).
bool parse_user_shell_invocation(const std::string& trimmed,
                                 std::string& command_out,
                                 std::string& error_out,
                                 UserShellDestination& destination_out);

// Run command via /bin/sh -c (or /usr/bin/sh). Non-zero exit is still ok_error()
// with result.exit_status set. Cancel/timeout return Cancelled/Timeout.
Error run_user_shell(const std::string& command,
                     const UserShellOptions& options,
                     UserShellResult& result);

// Format a display-only notice (optionally redacting secrets).
std::string format_user_shell_notice(const UserShellResult& result,
                                     const std::vector<std::string>& secrets = {});

// Pure redacted stdout for the TUI input draft (no metadata, no stderr).
std::string format_user_shell_draft_stdout(const UserShellResult& result,
                                           const std::vector<std::string>& secrets = {});

// True when the shell job should be reported as a failure to the user
// (transport error, cancel, timeout, non-zero exit, or signal).
bool user_shell_failed(const Error& error, const UserShellResult& result);

// One-line status for draft-mode completion (redacted; may include stderr snippet).
std::string format_user_shell_draft_status(const Error& error,
                                           const UserShellResult& result,
                                           const std::vector<std::string>& secrets = {});

// Multi-line diagnostic notice for draft-mode failures (display-only; not for success).
std::string format_user_shell_failure_notice(const Error& error,
                                             const UserShellResult& result,
                                             const std::vector<std::string>& secrets = {});

// Drop roles that must never go to the chat provider / chat SQLite library.
std::vector<provider::Message> provider_chat_messages(
    const std::vector<provider::Message>& messages);

// Lightweight role filter used by provider_chat_messages and SQLite save paths.
bool is_provider_chat_role(const std::string& role);

}  // namespace ainiux::app
