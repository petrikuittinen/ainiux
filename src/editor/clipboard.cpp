#include "editor/clipboard.hpp"

#include "html/html.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace ainiux::editor {
namespace {

std::string getenv_copy(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

bool executable_in_path(const std::string& path, const std::string& name, std::string& resolved) {
    size_t start = 0;
    while (start <= path.size()) {
        const size_t end = path.find(':', start);
        const std::string directory =
            path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!directory.empty() && directory.front() == '/') {
            const std::string candidate = directory + "/" + name;
            if (access(candidate.c_str(), X_OK) == 0) {
                resolved = candidate;
                return true;
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

bool make_pipe(int fds[2]) {
    if (pipe(fds) != 0) return false;
    (void)fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return true;
}

void close_fd(int& fd) {
    if (fd >= 0) {
        while (close(fd) != 0 && errno == EINTR) {
        }
        fd = -1;
    }
}

void discard_pending_signal(const sigset_t& blocked, int signal) {
    sigset_t pending{};
    if (sigpending(&pending) != 0 || sigismember(&pending, signal) != 1)
        return;

    int received = 0;
    int wait_error = 0;
    do {
        wait_error = sigwait(&blocked, &received);
    } while (wait_error == EINTR);
}

struct ChildProcess {
    pid_t pid = -1;
    pid_t group = -1;
    int input = -1;
    int output = -1;

    ~ChildProcess() {
        close_fd(input);
        close_fd(output);
        if (group > 0) (void)kill(-group, SIGKILL);
        if (pid > 0) {
            if (group <= 0) (void)kill(pid, SIGKILL);
            while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
            }
        }
    }

    void reaped() {
        pid = -1;
        if (group > 0) {
            (void)kill(-group, SIGKILL);
            group = -1;
        }
    }
};

struct SpawnFileActions {
    posix_spawn_file_actions_t actions{};
    bool initialized = false;

    ~SpawnFileActions() {
        if (initialized) (void)posix_spawn_file_actions_destroy(&actions);
    }
};

struct SpawnAttributes {
    posix_spawnattr_t attributes{};
    bool initialized = false;

    ~SpawnAttributes() {
        if (initialized) (void)posix_spawnattr_destroy(&attributes);
    }
};

struct PipeSignalGuard {
    sigset_t blocked{};
    sigset_t previous{};
    bool active = false;
    bool was_pending = false;

    PipeSignalGuard() {
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGPIPE);
        sigset_t pending{};
        if (sigpending(&pending) == 0)
            was_pending = sigismember(&pending, SIGPIPE) == 1;
        active = pthread_sigmask(SIG_BLOCK, &blocked, &previous) == 0;
    }

    ~PipeSignalGuard() {
        if (!active) return;
        if (!was_pending) discard_pending_signal(blocked, SIGPIPE);
        (void)pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    }
};

SystemClipboardResult run_command(const ClipboardCommand& command,
                                  const std::string* input,
                                  runtime::CancellationToken token) {
    SystemClipboardResult result;
    result.backend = command.backend;
    int input_pipe[2] = {-1, -1};
    int output_pipe[2] = {-1, -1};
    if (!make_pipe(input_pipe) || !make_pipe(output_pipe)) {
        close_fd(input_pipe[0]);
        close_fd(input_pipe[1]);
        close_fd(output_pipe[0]);
        close_fd(output_pipe[1]);
        result.error = SystemClipboardError::Failed;
        result.message = "could not create clipboard helper pipes: " + std::string(std::strerror(errno));
        return result;
    }

    ChildProcess child;
    std::vector<char*> argv;
    argv.reserve(command.arguments.size() + 2);
    argv.push_back(const_cast<char*>(command.executable.c_str()));
    for (const std::string& argument : command.arguments)
        argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);

    SpawnFileActions file_actions;
    int spawn_error = posix_spawn_file_actions_init(&file_actions.actions);
    file_actions.initialized = spawn_error == 0;
    if (spawn_error == 0)
        spawn_error = posix_spawn_file_actions_adddup2(
            &file_actions.actions, input_pipe[0], STDIN_FILENO);
    if (spawn_error == 0)
        spawn_error = posix_spawn_file_actions_adddup2(
            &file_actions.actions, output_pipe[1], STDOUT_FILENO);
    if (spawn_error == 0)
        spawn_error = posix_spawn_file_actions_addopen(
            &file_actions.actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    const int descriptors[] = {
        input_pipe[0], input_pipe[1], output_pipe[0], output_pipe[1]};
    for (int descriptor : descriptors) {
        if (spawn_error == 0 && descriptor != STDIN_FILENO &&
            descriptor != STDOUT_FILENO && descriptor != STDERR_FILENO)
            spawn_error =
                posix_spawn_file_actions_addclose(&file_actions.actions,
                                                  descriptor);
    }
    SpawnAttributes spawn_attributes;
    if (spawn_error == 0) {
        spawn_error = posix_spawnattr_init(&spawn_attributes.attributes);
        spawn_attributes.initialized = spawn_error == 0;
    }
    if (spawn_error == 0)
        spawn_error = posix_spawnattr_setflags(&spawn_attributes.attributes,
                                               POSIX_SPAWN_SETPGROUP);
    if (spawn_error == 0)
        spawn_error =
            posix_spawnattr_setpgroup(&spawn_attributes.attributes, 0);
    if (spawn_error == 0)
        spawn_error = posix_spawn(&child.pid,
                                  command.executable.c_str(),
                                  &file_actions.actions,
                                  &spawn_attributes.attributes,
                                  argv.data(),
                                  environ);
    if (spawn_error == 0) child.group = child.pid;
    if (spawn_error != 0) {
        close_fd(input_pipe[0]);
        close_fd(input_pipe[1]);
        close_fd(output_pipe[0]);
        close_fd(output_pipe[1]);
        result.error = SystemClipboardError::Failed;
        result.message = "could not start " + command.backend + " clipboard helper: " +
                         std::string(std::strerror(spawn_error));
        return result;
    }

    close_fd(input_pipe[0]);
    close_fd(output_pipe[1]);
    child.input = input_pipe[1];
    child.output = output_pipe[0];
    (void)fcntl(child.input, F_SETFL, fcntl(child.input, F_GETFL, 0) | O_NONBLOCK);
    (void)fcntl(child.output, F_SETFL, fcntl(child.output, F_GETFL, 0) | O_NONBLOCK);
    PipeSignalGuard pipe_signal_guard;
    if (input != nullptr && !pipe_signal_guard.active) {
        result.error = SystemClipboardError::Failed;
        result.message = "could not safely prepare the clipboard helper input pipe";
        return result;
    }

    const std::string empty;
    const std::string& bytes = input == nullptr ? empty : *input;
    size_t written = 0;
    bool input_closed = false;
    bool input_failed = false;
    bool output_closed = false;
    bool exited = false;
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kClipboardHelperTimeoutMs);
    while (!(exited && output_closed)) {
        if (token.cancelled()) {
            result.error = SystemClipboardError::Cancelled;
            result.message = "clipboard operation cancelled";
            return result;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            result.error = SystemClipboardError::Timeout;
            result.message = command.backend + " clipboard helper timed out after two seconds";
            return result;
        }

        if (!input_closed && written == bytes.size()) {
            close_fd(child.input);
            input_closed = true;
        }
        pollfd fds[2]{};
        nfds_t count = 0;
        if (!input_closed) {
            fds[count].fd = child.input;
            fds[count].events = POLLOUT;
            ++count;
        }
        if (!output_closed) {
            fds[count].fd = child.output;
            fds[count].events = POLLIN | POLLHUP;
            ++count;
        }
        const int polled = poll(fds, count, 10);
        if (polled < 0 && errno != EINTR) {
            result.error = SystemClipboardError::Failed;
            result.message = "clipboard helper pipe failed: " + std::string(std::strerror(errno));
            return result;
        }
        for (nfds_t index = 0; index < count; ++index) {
            if (fds[index].fd == child.input && (fds[index].revents & POLLOUT)) {
                const ssize_t n = write(child.input, bytes.data() + written, bytes.size() - written);
                if (n > 0) written += static_cast<size_t>(n);
                else if (n < 0 && errno != EAGAIN && errno != EINTR) {
                    input_failed = true;
                    close_fd(child.input);
                    input_closed = true;
                }
            } else if (fds[index].fd == child.output &&
                       (fds[index].revents & (POLLIN | POLLHUP))) {
                char buffer[8192];
                for (;;) {
                    const ssize_t n = read(child.output, buffer, sizeof(buffer));
                    if (n > 0) {
                        if (result.text.size() + static_cast<size_t>(n) >
                            kExternalClipboardReadLimit) {
                            result.error = SystemClipboardError::TooLarge;
                            result.message = "system clipboard exceeds the 16 MiB text limit";
                            return result;
                        }
                        result.text.append(buffer, static_cast<size_t>(n));
                    } else if (n == 0) {
                        close_fd(child.output);
                        output_closed = true;
                        break;
                    } else if (errno == EAGAIN || errno == EINTR) {
                        break;
                    } else {
                        result.error = SystemClipboardError::Failed;
                        result.message = "could not read clipboard helper output: " +
                                         std::string(std::strerror(errno));
                        return result;
                    }
                }
            }
        }
        const pid_t waited = waitpid(child.pid, &status, WNOHANG);
        if (waited == child.pid) {
            child.reaped();
            exited = true;
        } else if (waited < 0 && errno != EINTR) {
            result.error = SystemClipboardError::Failed;
            result.message = "could not reap clipboard helper: " + std::string(std::strerror(errno));
            return result;
        }
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        result.error = SystemClipboardError::Failed;
        result.message = command.backend + " clipboard helper exited unsuccessfully";
        return result;
    }
    if (input_failed || (input != nullptr && written != bytes.size())) {
        result.error = SystemClipboardError::Failed;
        result.message = command.backend +
                         " clipboard helper closed its input before the text was written";
        return result;
    }
    if (input != nullptr) return result;
    if (result.text.empty()) {
        result.error = SystemClipboardError::Empty;
        result.message = "system clipboard contains no text";
    } else if (result.text.find('\0') != std::string::npos) {
        result.error = SystemClipboardError::NonText;
        result.message = "system clipboard is not text";
    } else if (!html::is_valid_utf8(result.text)) {
        result.error = SystemClipboardError::Malformed;
        result.message = "system clipboard text is not valid UTF-8";
    }
    return result;
}

}  // namespace

void Clipboard::set(std::string text) {
    text_ = std::move(text);
}

void Clipboard::clear() {
    text_.clear();
}

Clipboard& shared_clipboard() {
    static Clipboard clipboard;
    return clipboard;
}

ClipboardEnvironment current_clipboard_environment() {
    ClipboardEnvironment environment;
    environment.path = getenv_copy("PATH");
#if defined(__APPLE__)
    environment.macos = true;
#endif
    environment.wayland = !getenv_copy("WAYLAND_DISPLAY").empty();
    environment.x11 = !getenv_copy("DISPLAY").empty();
    environment.termux =
        !getenv_copy("TERMUX_VERSION").empty() ||
        (!getenv_copy("PREFIX").empty() &&
         getenv_copy("PREFIX").find("com.termux") != std::string::npos);
    const std::string release = getenv_copy("WSL_DISTRO_NAME") + getenv_copy("WSL_INTEROP");
    environment.wsl = !release.empty();
    environment.ssh = !getenv_copy("SSH_CONNECTION").empty() ||
                      !getenv_copy("SSH_TTY").empty() ||
                      !getenv_copy("SSH_CLIENT").empty();
    return environment;
}

bool prefer_terminal_clipboard_query(const ClipboardEnvironment& environment) {
    return environment.ssh;
}

bool resolve_clipboard_command(const ClipboardEnvironment& environment,
                               bool write,
                               ClipboardCommand& command) {
    auto choose = [&](const char* backend, const char* name,
                      std::vector<std::string> arguments) {
        std::string executable;
        if (!executable_in_path(environment.path, name, executable)) return false;
        command = {backend, std::move(executable), std::move(arguments)};
        return true;
    };
    if (environment.termux &&
        choose("Termux", write ? "termux-clipboard-set" : "termux-clipboard-get", {}))
        return true;
    if (environment.wsl) {
        if (write && choose("WSL", "clip.exe", {})) return true;
        if (!write &&
            choose("WSL", "powershell.exe",
                   {"-NoProfile", "-NonInteractive", "-Command", "Get-Clipboard -Raw"}))
            return true;
    }
    if (environment.macos && choose("macOS", write ? "pbcopy" : "pbpaste", {})) return true;
    if (environment.wayland &&
        choose("Wayland", write ? "wl-copy" : "wl-paste",
               write ? std::vector<std::string>{"--type", "text/plain;charset=utf-8"}
                     : std::vector<std::string>{}))
        return true;
    if (environment.x11 &&
        choose("X11 xclip", "xclip",
               {"-selection", "clipboard", write ? "-in" : "-out"}))
        return true;
    if (environment.x11 &&
        choose("X11 xsel", "xsel",
               {"--clipboard", write ? "--input" : "--output"}))
        return true;
    return false;
}

SystemClipboardResult read_system_clipboard(const ClipboardEnvironment& environment,
                                            runtime::CancellationToken token) {
    ClipboardCommand command;
    if (!resolve_clipboard_command(environment, false, command)) {
        return {SystemClipboardError::Unavailable, "", "",
                "no supported system clipboard reader was found"};
    }
    return run_command(command, nullptr, token);
}

SystemClipboardResult write_system_clipboard(const ClipboardEnvironment& environment,
                                             const std::string& text,
                                             runtime::CancellationToken token) {
    ClipboardCommand command;
    if (!resolve_clipboard_command(environment, true, command)) {
        return {SystemClipboardError::Unavailable, "", "",
                "no supported system clipboard writer was found"};
    }
    return run_command(command, &text, token);
}

std::string clipboard_failure_help(const ClipboardEnvironment& environment,
                                   const SystemClipboardResult& result,
                                   bool reading) {
    if (result.error != SystemClipboardError::Unavailable)
        return result.message +
               (reading ? "; use your terminal paste shortcut" : "");
    std::string suggestion;
    if (environment.wayland) suggestion = "install wl-clipboard";
    else if (environment.x11) suggestion = "install xclip or xsel";
    else if (environment.termux) suggestion = "install Termux:API";
    else if (environment.wsl) suggestion = "ensure clip.exe and powershell.exe are in PATH";
    else if (environment.macos) suggestion = "ensure /usr/bin is in PATH";
    else suggestion = "install wl-clipboard, xclip, or xsel";
    return result.message + "; " + suggestion +
           (reading ? ", or use your terminal paste shortcut" : "");
}

ClipboardRuntime::~ClipboardRuntime() {
    cancel_all();
}

std::uint64_t ClipboardRuntime::start_read(const ClipboardEnvironment& environment) {
    read_job_.cancel();
    read_job_.join();
    const std::uint64_t generation = ++next_generation_;
    read_job_.start([this, environment, generation](runtime::CancellationToken token) {
        ClipboardRuntimeEvent event;
        event.type = ClipboardRuntimeEventType::ReadFinished;
        event.generation = generation;
        event.result = read_system_clipboard(environment, token);
        events_.push(std::move(event));
    });
    return generation;
}

std::uint64_t ClipboardRuntime::start_write(const ClipboardEnvironment& environment,
                                            std::string text) {
    write_job_.cancel();
    write_job_.join();
    const std::uint64_t generation = ++next_generation_;
    write_job_.start([this, environment, generation, text = std::move(text)](
                         runtime::CancellationToken token) {
        ClipboardRuntimeEvent event;
        event.type = ClipboardRuntimeEventType::WriteFinished;
        event.generation = generation;
        event.result = write_system_clipboard(environment, text, token);
        events_.push(std::move(event));
    });
    return generation;
}

void ClipboardRuntime::cancel_read() {
    read_job_.cancel();
    read_job_.join();
    ++next_generation_;
}

void ClipboardRuntime::cancel_all() {
    read_job_.cancel();
    write_job_.cancel();
    read_job_.join();
    write_job_.join();
    events_.close();
}

bool ClipboardRuntime::try_pop(ClipboardRuntimeEvent& event) {
    return events_.try_pop(event);
}

}  // namespace ainiux::editor
