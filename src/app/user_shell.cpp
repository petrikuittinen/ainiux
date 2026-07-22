#include "app/user_shell.hpp"

#include "security/redact.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace ainiux::app {
namespace {

class Pipe {
   public:
    ~Pipe() { close_all(); }
    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;
    Pipe() = default;

    Error open() {
        if (::pipe(fds_) != 0) {
            return {ErrorCode::Internal,
                    "could not create shell process pipe: " + std::string(std::strerror(errno))};
        }
        return ok_error();
    }

    int read_fd() const { return fds_[0]; }
    int write_fd() const { return fds_[1]; }

    int release_read() {
        const int fd = fds_[0];
        fds_[0] = -1;
        return fd;
    }

    void close_read() { close_one(0); }
    void close_write() { close_one(1); }

   private:
    int fds_[2] = {-1, -1};

    void close_one(int index) {
        if (fds_[index] >= 0) {
            ::close(fds_[index]);
            fds_[index] = -1;
        }
    }

    void close_all() {
        close_one(0);
        close_one(1);
    }
};

void append_bounded(std::string& output,
                    const char* data,
                    std::size_t count,
                    std::size_t limit,
                    bool& truncated) {
    const std::size_t remaining = output.size() < limit ? limit - output.size() : 0;
    const std::size_t accepted = std::min(remaining, count);
    output.append(data, accepted);
    if (accepted != count) truncated = true;
}

void drain_fd(int fd, std::string& output, std::size_t limit, bool& truncated, bool& open) {
    char buffer[8192];
    while (true) {
        const ssize_t count = ::read(fd, buffer, sizeof(buffer));
        if (count > 0) {
            append_bounded(output, buffer, static_cast<std::size_t>(count), limit, truncated);
            continue;
        }
        if (count == 0) {
            ::close(fd);
            open = false;
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        if (errno == EINTR) continue;
        ::close(fd);
        open = false;
        return;
    }
}

std::string resolve_shell_path() {
    static const char* kCandidates[] = {"/bin/sh", "/usr/bin/sh"};
    for (const char* path : kCandidates) {
        if (::access(path, X_OK) == 0) return path;
    }
    return {};
}

std::string process_cwd() {
    char buffer[4096];
    if (::getcwd(buffer, sizeof(buffer)) != nullptr) return buffer;
    return ".";
}

std::string ascii_trim_copy(std::string text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' ||
                             text.front() == '\n')) {
        text.erase(text.begin());
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' ||
                             text.back() == '\n')) {
        text.pop_back();
    }
    return text;
}

}  // namespace

bool parse_user_shell_invocation(const std::string& trimmed,
                                 std::string& command_out,
                                 std::string& error_out,
                                 UserShellDestination& destination_out) {
    command_out.clear();
    error_out.clear();
    destination_out = UserShellDestination::Notice;
    if (trimmed.empty()) return false;

    // Draft forms first so !! is not treated as notice !.
    if (trimmed.rfind("!!", 0) == 0) {
        destination_out = UserShellDestination::Draft;
        command_out = ascii_trim_copy(trimmed.substr(2));
        if (command_out.empty()) {
            error_out = "Usage: !!COMMAND  or  /shell-stdout COMMAND";
        }
        return true;
    }
    if (trimmed == "/shell-stdout") {
        destination_out = UserShellDestination::Draft;
        error_out = "Usage: /shell-stdout COMMAND  or  !!COMMAND";
        return true;
    }
    if (trimmed.rfind("/shell-stdout ", 0) == 0) {
        destination_out = UserShellDestination::Draft;
        command_out = ascii_trim_copy(trimmed.substr(14));
        if (command_out.empty()) {
            error_out = "Usage: /shell-stdout COMMAND  or  !!COMMAND";
        }
        return true;
    }

    if (trimmed.front() == '!') {
        destination_out = UserShellDestination::Notice;
        command_out = ascii_trim_copy(trimmed.substr(1));
        if (command_out.empty()) {
            error_out = "Usage: !COMMAND  or  /shell COMMAND";
        }
        return true;
    }

    if (trimmed == "/shell") {
        destination_out = UserShellDestination::Notice;
        error_out = "Usage: /shell COMMAND  or  !COMMAND";
        return true;
    }
    if (trimmed.rfind("/shell ", 0) == 0) {
        destination_out = UserShellDestination::Notice;
        command_out = ascii_trim_copy(trimmed.substr(7));
        if (command_out.empty()) {
            error_out = "Usage: /shell COMMAND  or  !COMMAND";
        }
        return true;
    }
    return false;
}

Error run_user_shell(const std::string& command,
                     const UserShellOptions& options,
                     UserShellResult& result) {
    result = UserShellResult{};
    result.command = command;
    if (ascii_trim_copy(command).empty()) {
        return {ErrorCode::BadArgs, "Usage: /shell COMMAND  or  !COMMAND"};
    }

    const std::string shell = resolve_shell_path();
    if (shell.empty()) {
        return {ErrorCode::FileRead, "could not find executable /bin/sh or /usr/bin/sh"};
    }

    std::string cwd = options.cwd.empty() ? process_cwd() : options.cwd;
    result.cwd = cwd;

    const long timeout_ms = options.timeout_ms > 0 ? options.timeout_ms : 60000;
    const std::size_t stdout_limit =
        options.stdout_limit > 0 ? options.stdout_limit : 256 * 1024;
    const std::size_t stderr_limit =
        options.stderr_limit > 0 ? options.stderr_limit : 256 * 1024;

    // Prepare argv/env storage before fork (async-signal-safe child path).
    std::string shell_storage = shell;
    std::string dash_c = "-c";
    std::string command_storage = command;
    std::vector<char*> argv = {shell_storage.data(), dash_c.data(), command_storage.data(),
                               nullptr};

    std::vector<std::string> environment_storage = {
        "PATH=/usr/local/bin:/usr/bin:/bin",
        "LC_ALL=C.UTF-8",
        "LANG=C.UTF-8",
        "PAGER=cat",
    };
    // Preserve a few non-secret user env vars that interactive shells often need.
    if (const char* home = std::getenv("HOME")) {
        if (home[0] != '\0') environment_storage.push_back(std::string("HOME=") + home);
    }
    if (const char* term = std::getenv("TERM")) {
        if (term[0] != '\0') environment_storage.push_back(std::string("TERM=") + term);
    }
    if (const char* user = std::getenv("USER")) {
        if (user[0] != '\0') environment_storage.push_back(std::string("USER=") + user);
    }
    std::vector<char*> environment;
    environment.reserve(environment_storage.size() + 1);
    for (std::string& item : environment_storage) environment.push_back(item.data());
    environment.push_back(nullptr);

    Pipe stdout_pipe;
    Pipe stderr_pipe;
    Error error = stdout_pipe.open();
    if (!error.ok()) return error;
    error = stderr_pipe.open();
    if (!error.ok()) return error;

    const auto started = std::chrono::steady_clock::now();
    const pid_t pid = ::fork();
    if (pid < 0) {
        return {ErrorCode::Internal,
                "could not fork shell command: " + std::string(std::strerror(errno))};
    }
    if (pid == 0) {
        ::setpgid(0, 0);
        stdout_pipe.close_read();
        stderr_pipe.close_read();
        const int null_fd = ::open("/dev/null", O_RDONLY);
        if (null_fd >= 0) {
            ::dup2(null_fd, STDIN_FILENO);
            ::close(null_fd);
        }
        ::dup2(stdout_pipe.write_fd(), STDOUT_FILENO);
        ::dup2(stderr_pipe.write_fd(), STDERR_FILENO);
        stdout_pipe.close_write();
        stderr_pipe.close_write();
        if (::chdir(cwd.c_str()) != 0) _exit(126);
        ::execve(shell_storage.c_str(), argv.data(), environment.data());
        _exit(errno == ENOENT ? 127 : 126);
    }

    ::setpgid(pid, pid);
    stdout_pipe.close_write();
    stderr_pipe.close_write();
    ::fcntl(stdout_pipe.read_fd(), F_SETFL, ::fcntl(stdout_pipe.read_fd(), F_GETFL) | O_NONBLOCK);
    ::fcntl(stderr_pipe.read_fd(), F_SETFL, ::fcntl(stderr_pipe.read_fd(), F_GETFL) | O_NONBLOCK);
    int stdout_fd = stdout_pipe.release_read();
    int stderr_fd = stderr_pipe.release_read();
    bool stdout_open = true;
    bool stderr_open = true;
    int wait_status = 0;
    bool reaped = false;
    bool terminated = false;
    long long terminated_at = 0;

    while (!reaped || stdout_open || stderr_open) {
        const auto now = std::chrono::steady_clock::now();
        const long long elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count();
        if (!terminated && (options.cancellation.cancelled() || elapsed > timeout_ms)) {
            result.cancelled = options.cancellation.cancelled();
            result.timed_out = !result.cancelled;
            ::kill(-pid, SIGTERM);
            terminated = true;
            terminated_at = elapsed;
        }
        if (terminated && elapsed > terminated_at + 250) ::kill(-pid, SIGKILL);
        pollfd fds[2] = {{stdout_fd, static_cast<short>(stdout_open ? POLLIN | POLLHUP : 0), 0},
                         {stderr_fd, static_cast<short>(stderr_open ? POLLIN | POLLHUP : 0), 0}};
        ::poll(fds, 2, 25);
        if (stdout_open) {
            drain_fd(stdout_fd, result.stdout_text, stdout_limit, result.stdout_truncated,
                     stdout_open);
        }
        if (stderr_open) {
            drain_fd(stderr_fd, result.stderr_text, stderr_limit, result.stderr_truncated,
                     stderr_open);
        }
        if (!reaped) {
            const pid_t waited = ::waitpid(pid, &wait_status, WNOHANG);
            if (waited == pid) reaped = true;
            else if (waited < 0 && errno != EINTR) reaped = true;
        }
    }

    result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    if (WIFEXITED(wait_status)) result.exit_status = WEXITSTATUS(wait_status);
    if (WIFSIGNALED(wait_status)) result.signal = WTERMSIG(wait_status);

    if (result.cancelled) return {ErrorCode::Cancelled, "shell command cancelled"};
    if (result.timed_out) return {ErrorCode::Timeout, "shell command exceeded its timeout"};
    return ok_error();
}

std::string format_user_shell_notice(const UserShellResult& result,
                                     const std::vector<std::string>& secrets) {
    std::string text;
    text.reserve(result.stdout_text.size() + result.stderr_text.size() + 128);
    text += "$ ";
    text += result.command;
    text += "\n";
    text += "exit=";
    text += std::to_string(result.exit_status);
    if (result.signal > 0) {
        text += " signal=";
        text += std::to_string(result.signal);
    }
    text += "  ";
    text += std::to_string(result.duration_ms);
    text += "ms  cwd=";
    text += result.cwd;
    if (result.cancelled) text += "  [cancelled]";
    if (result.timed_out) text += "  [timeout]";
    text += "\n";
    if (!result.stdout_text.empty()) {
        text += result.stdout_text;
        if (!result.stdout_text.empty() && result.stdout_text.back() != '\n') text += "\n";
    }
    if (!result.stderr_text.empty()) {
        text += "--- stderr ---\n";
        text += result.stderr_text;
        if (!result.stderr_text.empty() && result.stderr_text.back() != '\n') text += "\n";
    }
    if (result.stdout_truncated) text += "[stdout truncated]\n";
    if (result.stderr_truncated) text += "[stderr truncated]\n";
    if (result.stdout_text.empty() && result.stderr_text.empty() && !result.cancelled &&
        !result.timed_out) {
        text += "(no output)\n";
    }
    return redact_secrets(std::move(text), secrets);
}

std::string format_user_shell_draft_stdout(const UserShellResult& result,
                                           const std::vector<std::string>& secrets) {
    return redact_secrets(result.stdout_text, secrets);
}

bool user_shell_failed(const Error& error, const UserShellResult& result) {
    if (!error.ok()) return true;
    if (result.cancelled || result.timed_out) return true;
    if (result.signal > 0) return true;
    if (result.exit_status != 0) return true;
    return false;
}

namespace {

std::string first_line_snippet(const std::string& text, std::size_t max_bytes) {
    std::string line;
    for (char ch : text) {
        if (ch == '\n' || ch == '\r') break;
        line.push_back(ch);
        if (line.size() >= max_bytes) break;
    }
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
    std::size_t start = 0;
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) ++start;
    if (start > 0) line = line.substr(start);
    if (line.size() > max_bytes) line.resize(max_bytes);
    return line;
}

}  // namespace

std::string format_user_shell_draft_status(const Error& error,
                                           const UserShellResult& result,
                                           const std::vector<std::string>& secrets) {
    std::string line;
    if (error.code == ErrorCode::Cancelled || result.cancelled) {
        line = "Shell-stdout cancelled";
        if (!result.stdout_text.empty()) {
            line += " · partial draft " + std::to_string(result.stdout_text.size()) + " bytes";
        }
    } else if (error.code == ErrorCode::Timeout || result.timed_out) {
        line = "Shell-stdout timed out";
        if (result.duration_ms > 0) {
            line += " after " + std::to_string(result.duration_ms) + "ms";
        }
        if (!result.stdout_text.empty()) {
            line += " · partial draft " + std::to_string(result.stdout_text.size()) + " bytes";
        }
    } else if (!error.ok()) {
        line = "Shell-stdout error · ";
        line += error.message.empty() ? error_code_name(error.code) : error.message;
    } else if (result.signal > 0) {
        line = "Shell-stdout failed · signal " + std::to_string(result.signal);
    } else if (result.exit_status != 0) {
        line = "Shell-stdout failed · exit " + std::to_string(result.exit_status);
    } else {
        line = "Shell → draft · exit 0 · " + std::to_string(result.stdout_text.size()) + " bytes";
        if (result.stdout_truncated) line += " · truncated";
        if (result.stdout_text.empty()) line += " · empty stdout";
        return redact_secrets(std::move(line), secrets);
    }

    const std::string snippet = first_line_snippet(result.stderr_text, 160);
    if (!snippet.empty()) {
        line += " · ";
        line += snippet;
    } else if (result.exit_status == 127 && error.ok()) {
        line += " · command not found (exit 127)";
    } else if (result.stdout_text.empty() && result.stderr_text.empty() && error.ok() &&
               result.exit_status != 0) {
        line += " · no output";
    }
    if (result.stdout_truncated) line += " · stdout truncated";
    if (result.stderr_truncated) line += " · stderr truncated";
    return redact_secrets(std::move(line), secrets);
}

std::string format_user_shell_failure_notice(const Error& error,
                                             const UserShellResult& result,
                                             const std::vector<std::string>& secrets) {
    std::string text;
    text.reserve(result.stderr_text.size() + result.command.size() + 160);
    text += "shell-stdout failed\n";
    text += "command: ";
    text += result.command.empty() ? "(empty)" : result.command;
    text += "\n";
    if (!error.ok()) {
        text += "error: ";
        text += error_code_name(error.code);
        if (!error.message.empty()) {
            text += ": ";
            text += error.message;
        }
        text += "\n";
    }
    if (result.cancelled) text += "reason: cancelled\n";
    if (result.timed_out) text += "reason: timeout\n";
    if (result.exit_status >= 0) {
        text += "exit: ";
        text += std::to_string(result.exit_status);
        text += "\n";
    }
    if (result.signal > 0) {
        text += "signal: ";
        text += std::to_string(result.signal);
        text += "\n";
    }
    if (result.duration_ms > 0) {
        text += "duration_ms: ";
        text += std::to_string(result.duration_ms);
        text += "\n";
    }
    if (!result.cwd.empty()) {
        text += "cwd: ";
        text += result.cwd;
        text += "\n";
    }
    if (!result.stderr_text.empty()) {
        text += "stderr:\n";
        text += result.stderr_text;
        if (result.stderr_text.back() != '\n') text += "\n";
    } else if (error.ok() && result.exit_status != 0 && !result.cancelled && !result.timed_out) {
        text += "stderr: (empty)\n";
        if (result.exit_status == 127) {
            text += "hint: exit 127 usually means the command was not found on PATH\n";
        }
    }
    if (result.stdout_truncated) text += "[stdout truncated]\n";
    if (result.stderr_truncated) text += "[stderr truncated]\n";
    if (!result.stdout_text.empty()) {
        text += "note: pure stdout was placed in the input draft (edit/clear before send)\n";
    }
    return redact_secrets(std::move(text), secrets);
}

bool is_provider_chat_role(const std::string& role) {
    return role == "system" || role == "user" || role == "assistant";
}

std::vector<provider::Message> provider_chat_messages(
    const std::vector<provider::Message>& messages) {
    std::vector<provider::Message> out;
    out.reserve(messages.size());
    for (const provider::Message& message : messages) {
        if (is_provider_chat_role(message.role)) out.push_back(message);
    }
    return out;
}

}  // namespace ainiux::app
