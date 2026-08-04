#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::runtime {

// A shell-free, platform-neutral child-process request. All strings are UTF-8.
// The environment is intentionally explicit: callers must opt in to every value
// made visible to the child, which prevents provider credentials leaking through
// inherited process state.
struct SubprocessOptions {
    std::string executable;
    std::vector<std::string> arguments;
    std::string cwd;
    std::vector<std::string> environment;
    std::string stdin_text;
    bool provide_stdin = false;
#if defined(_WIN32)
    // arguments[0] is an already-resolved .bat/.cmd file. The Windows backend
    // invokes it through the resolved cmd.exe using a deliberately restricted,
    // non-expanding command representation prepared by the caller.
    bool windows_batch = false;
#endif
    std::size_t stdout_limit = 256U * 1024U;
    std::size_t stderr_limit = 256U * 1024U;
    long timeout_ms = 60000;
    CancellationToken cancellation;
};

enum class SubprocessTerminationReason {
    Exited,
    Signalled,
    TimedOut,
    Cancelled,
    SpawnFailed,
    IoFailed,
};

struct SubprocessResult {
    std::string stdout_text;
    std::string stderr_text;
    std::int64_t exit_code = -1;
    int signal = 0;
    long long duration_ms = 0;
    bool stdout_truncated = false;
    bool stderr_truncated = false;
    bool stdout_repaired_utf8 = false;
    bool stderr_repaired_utf8 = false;
    bool stdin_incomplete = false;
    SubprocessTerminationReason termination = SubprocessTerminationReason::SpawnFailed;
};

// Runs the executable without invoking a shell. stdout/stderr are returned as
// UTF-8 byte strings with CRLF/CR normalized to LF. Cancellation and timeout
// terminate the complete process tree on both supported platforms.
Error run_subprocess(const SubprocessOptions& options, SubprocessResult& result);

const char* subprocess_termination_name(SubprocessTerminationReason reason);

}  // namespace ainiux::runtime
