#if !defined(_WIN32)

#include "runtime/subprocess.hpp"

#include "runtime/subprocess_detail.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace ainiux::runtime {
namespace {

class Fd {
   public:
    explicit Fd(int value = -1) : value_(value) {}
    ~Fd() { reset(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : value_(other.release()) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    int get() const { return value_; }
    int release() { const int value = value_; value_ = -1; return value; }
    void reset(int value = -1) {
        if (value_ >= 0) {
            while (::close(value_) != 0 && errno == EINTR) {}
        }
        value_ = value;
    }
   private:
    int value_ = -1;
};

class Pipe {
   public:
    Error open(const char* purpose) {
        int pair[2] = {-1, -1};
        if (::pipe(pair) != 0)
            return {ErrorCode::Internal,
                    std::string("could not create ") + purpose + " pipe: " +
                        std::strerror(errno)};
        read_.reset(pair[0]);
        write_.reset(pair[1]);
        return ok_error();
    }
    Fd& read_end() { return read_; }
    Fd& write_end() { return write_; }
   private:
    Fd read_;
    Fd write_;
};

// A child may close stdin before the parent has finished writing. Keep that
// ordinary pipe condition local to this call instead of allowing SIGPIPE to
// terminate the whole CLI process.
class PipeSignalGuard {
   public:
    PipeSignalGuard() {
        sigemptyset(&set_);
        sigaddset(&set_, SIGPIPE);
        sigset_t pending{};
        if (sigpending(&pending) == 0) pending_before_ = sigismember(&pending, SIGPIPE) == 1;
        active_ = pthread_sigmask(SIG_BLOCK, &set_, &previous_) == 0;
    }
    ~PipeSignalGuard() {
        if (!active_) return;
        if (!pending_before_) {
            sigset_t pending{};
            if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1) {
                // This thread generated the newly pending signal while it was
                // blocked, so sigwait returns immediately and is portable to macOS.
                int signal_number = 0;
                (void)sigwait(&set_, &signal_number);
            }
        }
        (void)pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
    }
    PipeSignalGuard(const PipeSignalGuard&) = delete;
    PipeSignalGuard& operator=(const PipeSignalGuard&) = delete;

   private:
    sigset_t set_{};
    sigset_t previous_{};
    bool active_ = false;
    bool pending_before_ = false;
};

void make_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void drain_fd(Fd& fd,
              std::string& output,
              std::size_t limit,
              bool& truncated,
              bool& open) {
    char buffer[8192];
    while (open) {
        const ssize_t count = ::read(fd.get(), buffer, sizeof(buffer));
        if (count > 0) {
            detail::append_bounded(output, buffer, static_cast<std::size_t>(count), limit,
                                   truncated);
            continue;
        }
        if (count == 0) {
            fd.reset();
            open = false;
            return;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        fd.reset();
        open = false;
    }
}

Error validate_options(const SubprocessOptions& options) {
    if (options.executable.empty())
        return {ErrorCode::BadArgs, "subprocess executable is empty"};
    if (options.executable.find('\0') != std::string::npos ||
        options.cwd.find('\0') != std::string::npos)
        return {ErrorCode::BadArgs, "subprocess path contains a NUL byte"};
    for (const std::string& argument : options.arguments) {
        if (argument.find('\0') != std::string::npos)
            return {ErrorCode::BadArgs, "subprocess argument contains a NUL byte"};
    }
    for (const std::string& entry : options.environment) {
        const std::size_t equals = entry.find('=');
        if (equals == std::string::npos || equals == 0 ||
            entry.find('\0') != std::string::npos)
            return {ErrorCode::BadArgs,
                    "subprocess environment entries must use non-empty NAME=VALUE form"};
    }
    return ok_error();
}

struct PosixBackgroundJob {
    pid_t pid = -1;
    Fd stdout_fd;
    Fd stderr_fd;
    std::size_t stdout_limit = 0;
    std::size_t stderr_limit = 0;
    long timeout_ms = 0;
    std::chrono::steady_clock::time_point started;
    std::thread worker;
};

std::mutex g_background_mutex;
std::map<std::int64_t, std::shared_ptr<PosixBackgroundJob>> g_background_jobs;

void posix_background_reaper(std::shared_ptr<PosixBackgroundJob> job) {
    std::string discarded_out;
    std::string discarded_err;
    bool out_trunc = false;
    bool err_trunc = false;
    bool stdout_open = job->stdout_fd.get() >= 0;
    bool stderr_open = job->stderr_fd.get() >= 0;
    if (stdout_open) make_nonblocking(job->stdout_fd.get());
    if (stderr_open) make_nonblocking(job->stderr_fd.get());
    bool reaped = false;
    bool terminating = false;
    long long terminate_started_ms = 0;
    int wait_status = 0;
    while (!reaped || stdout_open || stderr_open) {
        const auto now = std::chrono::steady_clock::now();
        const long long elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - job->started)
                .count();
        if (!terminating && job->timeout_ms > 0 && elapsed >= job->timeout_ms) {
            (void)::kill(-job->pid, SIGTERM);
            terminating = true;
            terminate_started_ms = elapsed;
        }
        if (terminating && elapsed >= terminate_started_ms + 250)
            (void)::kill(-job->pid, SIGKILL);
        pollfd descriptors[2]{};
        nfds_t count = 0;
        if (stdout_open)
            descriptors[count++] = {job->stdout_fd.get(), POLLIN | POLLHUP, 0};
        if (stderr_open)
            descriptors[count++] = {job->stderr_fd.get(), POLLIN | POLLHUP, 0};
        if (count > 0) (void)::poll(descriptors, count, 25);
        if (stdout_open)
            drain_fd(job->stdout_fd, discarded_out, job->stdout_limit, out_trunc,
                     stdout_open);
        if (stderr_open)
            drain_fd(job->stderr_fd, discarded_err, job->stderr_limit, err_trunc,
                     stderr_open);
        if (!reaped) {
            const pid_t waited = ::waitpid(job->pid, &wait_status, WNOHANG);
            if (waited == job->pid) reaped = true;
            else if (waited < 0 && errno != EINTR) reaped = true;
        }
    }
    std::lock_guard<std::mutex> lock(g_background_mutex);
    g_background_jobs.erase(static_cast<std::int64_t>(job->pid));
}

void start_posix_background_reaper(pid_t pid,
                                   Fd&& stdout_fd,
                                   Fd&& stderr_fd,
                                   const SubprocessOptions& options,
                                   std::chrono::steady_clock::time_point started) {
    auto job = std::make_shared<PosixBackgroundJob>();
    job->pid = pid;
    job->stdout_fd = std::move(stdout_fd);
    job->stderr_fd = std::move(stderr_fd);
    job->stdout_limit = options.stdout_limit;
    job->stderr_limit = options.stderr_limit;
    job->timeout_ms = options.timeout_ms;
    job->started = started;
    {
        std::lock_guard<std::mutex> lock(g_background_mutex);
        g_background_jobs[static_cast<std::int64_t>(pid)] = job;
    }
    job->worker = std::thread(posix_background_reaper, job);
    job->worker.detach();
}

}  // namespace

Error kill_background_process(std::int64_t pid) {
    if (pid <= 0) return ok_error();
    std::shared_ptr<PosixBackgroundJob> job;
    {
        std::lock_guard<std::mutex> lock(g_background_mutex);
        const auto found = g_background_jobs.find(pid);
        if (found != g_background_jobs.end()) job = found->second;
    }
    (void)::kill(static_cast<pid_t>(-pid), SIGTERM);
    (void)::kill(static_cast<pid_t>(pid), SIGTERM);
    (void)job;
    return ok_error();
}

void kill_all_background_processes() {
    std::vector<std::int64_t> pids;
    {
        std::lock_guard<std::mutex> lock(g_background_mutex);
        for (const auto& entry : g_background_jobs) pids.push_back(entry.first);
    }
    for (const std::int64_t pid : pids) (void)kill_background_process(pid);
}

Error run_subprocess(const SubprocessOptions& options, SubprocessResult& result) {
    result = SubprocessResult{};
    Error error = validate_options(options);
    if (!error.ok()) return error;
    PipeSignalGuard pipe_signal_guard;

    Pipe stdout_pipe;
    Pipe stderr_pipe;
    Pipe stdin_pipe;
    if (!(error = stdout_pipe.open("stdout")).ok() ||
        !(error = stderr_pipe.open("stderr")).ok() ||
        (options.provide_stdin && !(error = stdin_pipe.open("stdin")).ok()))
        return error;

    std::vector<std::string> argument_storage;
    argument_storage.reserve(options.arguments.size() + 1);
    argument_storage.push_back(options.executable);
    argument_storage.insert(argument_storage.end(), options.arguments.begin(),
                            options.arguments.end());
    std::vector<char*> argv;
    argv.reserve(argument_storage.size() + 1);
    for (std::string& value : argument_storage) argv.push_back(value.data());
    argv.push_back(nullptr);
    std::vector<std::string> environment_storage = options.environment;
    std::vector<char*> environment;
    environment.reserve(environment_storage.size() + 1);
    for (std::string& value : environment_storage) environment.push_back(value.data());
    environment.push_back(nullptr);

    const auto started = std::chrono::steady_clock::now();
    const pid_t pid = ::fork();
    if (pid < 0) {
        result.termination = SubprocessTerminationReason::SpawnFailed;
        return {ErrorCode::Internal,
                "could not fork subprocess " + options.executable + ": " +
                    std::strerror(errno)};
    }
    if (pid == 0) {
        (void)::setpgid(0, 0);
        stdout_pipe.read_end().reset();
        stderr_pipe.read_end().reset();
        if (options.provide_stdin) stdin_pipe.write_end().reset();

        int null_fd = -1;
        if (!options.provide_stdin) null_fd = ::open("/dev/null", O_RDONLY);
        const int source_stdin = options.provide_stdin ? stdin_pipe.read_end().get() : null_fd;
        if (source_stdin >= 0) (void)::dup2(source_stdin, STDIN_FILENO);
        (void)::dup2(stdout_pipe.write_end().get(), STDOUT_FILENO);
        (void)::dup2(stderr_pipe.write_end().get(), STDERR_FILENO);
        if (null_fd >= 0) ::close(null_fd);
        if (options.provide_stdin) stdin_pipe.read_end().reset();
        stdout_pipe.write_end().reset();
        stderr_pipe.write_end().reset();
        if (!options.cwd.empty() && ::chdir(options.cwd.c_str()) != 0) _exit(126);
        ::execve(options.executable.c_str(), argv.data(), environment.data());
        _exit(errno == ENOENT ? 127 : 126);
    }

    (void)::setpgid(pid, pid);
    stdout_pipe.write_end().reset();
    stderr_pipe.write_end().reset();
    if (options.provide_stdin) stdin_pipe.read_end().reset();
    make_nonblocking(stdout_pipe.read_end().get());
    make_nonblocking(stderr_pipe.read_end().get());
    if (options.provide_stdin) make_nonblocking(stdin_pipe.write_end().get());

    bool stdout_open = true;
    bool stderr_open = true;
    bool stdin_open = options.provide_stdin;
    std::size_t stdin_offset = 0;
    bool reaped = false;
    bool terminating = false;
    bool detached = false;
    long long terminate_started_ms = 0;
    int wait_status = 0;
    const long startup_ms = options.startup_ms > 0 ? options.startup_ms : 400;
    while (!reaped || stdout_open || stderr_open) {
        const auto now = std::chrono::steady_clock::now();
        const long long elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count();
        if (!terminating && options.cancellation.cancelled()) {
            result.termination = SubprocessTerminationReason::Cancelled;
            (void)::kill(-pid, SIGTERM);
            terminating = true;
            terminate_started_ms = elapsed;
        } else if (!terminating && !options.background && options.timeout_ms > 0 &&
                   elapsed >= options.timeout_ms) {
            result.termination = SubprocessTerminationReason::TimedOut;
            (void)::kill(-pid, SIGTERM);
            terminating = true;
            terminate_started_ms = elapsed;
        } else if (options.background && !reaped && !terminating && elapsed >= startup_ms) {
            detached = true;
            break;
        }
        if (terminating && elapsed >= terminate_started_ms + 250)
            (void)::kill(-pid, SIGKILL);

        if (stdin_open && stdin_offset == options.stdin_text.size()) {
            stdin_pipe.write_end().reset();
            stdin_open = false;
        }
        pollfd descriptors[3]{};
        nfds_t count = 0;
        if (stdout_open)
            descriptors[count++] = {stdout_pipe.read_end().get(), POLLIN | POLLHUP, 0};
        if (stderr_open)
            descriptors[count++] = {stderr_pipe.read_end().get(), POLLIN | POLLHUP, 0};
        if (stdin_open)
            descriptors[count++] = {stdin_pipe.write_end().get(), POLLOUT | POLLHUP, 0};
        if (count > 0) (void)::poll(descriptors, count, 25);

        if (stdin_open) {
            const ssize_t written = ::write(stdin_pipe.write_end().get(),
                                            options.stdin_text.data() + stdin_offset,
                                            options.stdin_text.size() - stdin_offset);
            if (written > 0) {
                stdin_offset += static_cast<std::size_t>(written);
            } else if (written < 0 && errno != EINTR && errno != EAGAIN &&
                       errno != EWOULDBLOCK) {
                result.stdin_incomplete = stdin_offset < options.stdin_text.size();
                stdin_pipe.write_end().reset();
                stdin_open = false;
            }
        }
        if (stdout_open)
            drain_fd(stdout_pipe.read_end(), result.stdout_text, options.stdout_limit,
                     result.stdout_truncated, stdout_open);
        if (stderr_open)
            drain_fd(stderr_pipe.read_end(), result.stderr_text, options.stderr_limit,
                     result.stderr_truncated, stderr_open);
        if (!reaped) {
            const pid_t waited = ::waitpid(pid, &wait_status, WNOHANG);
            if (waited == pid) reaped = true;
            else if (waited < 0 && errno != EINTR) reaped = true;
        }
    }
    stdin_pipe.write_end().reset();
    result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    result.pid = static_cast<std::int64_t>(pid);
    if (detached) {
        start_posix_background_reaper(pid, std::move(stdout_pipe.read_end()),
                                      std::move(stderr_pipe.read_end()), options,
                                      started);
        result.background = true;
        result.termination = SubprocessTerminationReason::Running;
        detail::normalize_output(result.stdout_text, options.stdout_limit,
                                 result.stdout_truncated, result.stdout_repaired_utf8);
        detail::normalize_output(result.stderr_text, options.stderr_limit,
                                 result.stderr_truncated, result.stderr_repaired_utf8);
        return ok_error();
    }
    if (result.termination != SubprocessTerminationReason::Cancelled &&
        result.termination != SubprocessTerminationReason::TimedOut) {
        if (WIFEXITED(wait_status)) {
            result.exit_code = WEXITSTATUS(wait_status);
            result.termination = SubprocessTerminationReason::Exited;
        } else if (WIFSIGNALED(wait_status)) {
            result.signal = WTERMSIG(wait_status);
            result.termination = SubprocessTerminationReason::Signalled;
        } else {
            result.termination = SubprocessTerminationReason::IoFailed;
        }
    }
    detail::normalize_output(result.stdout_text, options.stdout_limit,
                             result.stdout_truncated, result.stdout_repaired_utf8);
    detail::normalize_output(result.stderr_text, options.stderr_limit,
                             result.stderr_truncated, result.stderr_repaired_utf8);
    if (result.termination == SubprocessTerminationReason::Cancelled)
        return {ErrorCode::Cancelled, "subprocess cancelled: " + options.executable};
    if (result.termination == SubprocessTerminationReason::TimedOut)
        return {ErrorCode::Timeout, "subprocess exceeded its timeout: " + options.executable};
    if (result.termination == SubprocessTerminationReason::IoFailed)
        return {ErrorCode::Internal, "could not obtain subprocess status: " + options.executable};
    return ok_error();
}

}  // namespace ainiux::runtime

#endif
