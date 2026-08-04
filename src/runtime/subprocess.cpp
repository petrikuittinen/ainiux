#include "runtime/subprocess.hpp"

namespace ainiux::runtime {

const char* subprocess_termination_name(SubprocessTerminationReason reason) {
    switch (reason) {
        case SubprocessTerminationReason::Exited: return "exited";
        case SubprocessTerminationReason::Signalled: return "signalled";
        case SubprocessTerminationReason::TimedOut: return "timed_out";
        case SubprocessTerminationReason::Cancelled: return "cancelled";
        case SubprocessTerminationReason::SpawnFailed: return "spawn_failed";
        case SubprocessTerminationReason::IoFailed: return "io_failed";
    }
    return "unknown";
}

}  // namespace ainiux::runtime
