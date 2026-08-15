#include "mcp/transport_stdio.hpp"

#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include "json/json.hpp"
#include "platform/environment.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ainiux::mcp {
namespace {

#if !defined(_WIN32)
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
    int release() {
        const int v = value_;
        value_ = -1;
        return v;
    }
    void reset(int value = -1) {
        if (value_ >= 0) {
            while (::close(value_) != 0 && errno == EINTR) {
            }
        }
        value_ = value;
    }

   private:
    int value_ = -1;
};
#endif

std::vector<std::string> build_environment(const ServerConfig& config) {
    // Minimal env: PATH + user-specified entries (expanded).
    std::vector<std::string> env;
    const std::string path = platform::environment_value("PATH");
    if (!path.empty()) env.push_back("PATH=" + path);
    const std::string home = platform::environment_value("HOME");
    if (!home.empty()) env.push_back("HOME=" + home);
    const std::string term = platform::environment_value("TERM");
    if (!term.empty()) env.push_back("TERM=" + term);
    const std::string lang = platform::environment_value("LANG");
    if (!lang.empty()) env.push_back("LANG=" + lang);
    for (const std::string& entry : config.env) {
        env.push_back(expand_env_refs(entry));
    }
    return env;
}

}  // namespace

struct StdioSession::Impl {
#if defined(_WIN32)
    HANDLE process = nullptr;
    HANDLE stdin_write = nullptr;
    HANDLE stdout_read = nullptr;
#else
    pid_t pid = -1;
    Fd stdin_write;
    Fd stdout_read;
#endif
    std::string read_buffer;
    bool alive = false;

    void kill_child() {
#if defined(_WIN32)
        if (process != nullptr) {
            TerminateProcess(process, 1);
            WaitForSingleObject(process, 3000);
            CloseHandle(process);
            process = nullptr;
        }
        if (stdin_write != nullptr) {
            CloseHandle(stdin_write);
            stdin_write = nullptr;
        }
        if (stdout_read != nullptr) {
            CloseHandle(stdout_read);
            stdout_read = nullptr;
        }
#else
        if (pid > 0) {
            ::kill(-pid, SIGTERM);
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
            int status = 0;
            while (std::chrono::steady_clock::now() < deadline) {
                const pid_t r = ::waitpid(pid, &status, WNOHANG);
                if (r == pid || (r < 0 && errno == ECHILD)) {
                    pid = -1;
                    break;
                }
                ::usleep(20000);
            }
            if (pid > 0) {
                ::kill(-pid, SIGKILL);
                (void)::waitpid(pid, &status, 0);
                pid = -1;
            }
        }
        stdin_write.reset();
        stdout_read.reset();
#endif
        alive = false;
        read_buffer.clear();
    }
};

StdioSession::StdioSession() : impl_(std::make_unique<Impl>()) {}
StdioSession::~StdioSession() { close(); }
StdioSession::StdioSession(StdioSession&&) noexcept = default;
StdioSession& StdioSession::operator=(StdioSession&&) noexcept = default;

void StdioSession::close() {
    if (impl_) impl_->kill_child();
}

bool StdioSession::running() const { return impl_ && impl_->alive; }

Error StdioSession::start(const ServerConfig& config, const StdioTransportOptions& options) {
    close();
    if (config.command.empty())
        return {ErrorCode::BadArgs, "stdio MCP server requires a command"};

#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE stdin_read = nullptr;
    HANDLE stdout_write = nullptr;
    if (!CreatePipe(&stdin_read, &impl_->stdin_write, &sa, 0) ||
        !CreatePipe(&impl_->stdout_read, &stdout_write, &sa, 0)) {
        return {ErrorCode::Internal, "could not create MCP stdio pipes"};
    }
    SetHandleInformation(impl_->stdin_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(impl_->stdout_read, HANDLE_FLAG_INHERIT, 0);

    std::string cmdline = "\"" + config.command + "\"";
    for (const std::string& arg : config.args) {
        cmdline.push_back(' ');
        cmdline.push_back('"');
        for (char c : arg) {
            if (c == '"') cmdline += "\\\"";
            else cmdline.push_back(c);
        }
        cmdline.push_back('"');
    }
    std::vector<char> cmd_buf(cmdline.begin(), cmdline.end());
    cmd_buf.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdin_read;
    si.hStdOutput = stdout_write;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    std::string cwd = config.cwd;
    BOOL ok = CreateProcessA(nullptr, cmd_buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                             nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    CloseHandle(stdin_read);
    CloseHandle(stdout_write);
    if (!ok) {
        impl_->kill_child();
        return {ErrorCode::Internal, "could not spawn MCP stdio process: " + config.command};
    }
    CloseHandle(pi.hThread);
    impl_->process = pi.hProcess;
    impl_->alive = true;
    (void)options;
    return ok_error();
#else
    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    if (::pipe(stdin_pipe) != 0 || ::pipe(stdout_pipe) != 0)
        return {ErrorCode::Internal, "could not create MCP stdio pipes"};

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(stdin_pipe[0]);
        ::close(stdin_pipe[1]);
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        return {ErrorCode::Internal, "could not fork MCP stdio process"};
    }
    if (pid == 0) {
        (void)::setpgid(0, 0);
        ::dup2(stdin_pipe[0], STDIN_FILENO);
        ::dup2(stdout_pipe[1], STDOUT_FILENO);
        ::close(stdin_pipe[0]);
        ::close(stdin_pipe[1]);
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        if (!config.cwd.empty() && ::chdir(config.cwd.c_str()) != 0) _exit(126);

        std::vector<std::string> storage;
        storage.push_back(config.command);
        storage.insert(storage.end(), config.args.begin(), config.args.end());
        std::vector<char*> argv;
        for (std::string& s : storage) argv.push_back(s.data());
        argv.push_back(nullptr);

        std::vector<std::string> env_storage = build_environment(config);
        std::vector<char*> envp;
        for (std::string& s : env_storage) envp.push_back(s.data());
        envp.push_back(nullptr);
        ::execve(config.command.c_str(), argv.data(), envp.data());
        // Fallback: search PATH via execvp if absolute path failed.
        ::execvp(config.command.c_str(), argv.data());
        _exit(127);
    }
    ::close(stdin_pipe[0]);
    ::close(stdout_pipe[1]);
    (void)::setpgid(pid, pid);
    impl_->pid = pid;
    impl_->stdin_write.reset(stdin_pipe[1]);
    impl_->stdout_read.reset(stdout_pipe[0]);
    impl_->alive = true;
    (void)options;
    return ok_error();
#endif
}

Error StdioSession::notify(const std::string& notification_json,
                           long timeout_ms,
                           runtime::CancellationToken cancellation) {
    JsonRpcResponse ignored;
    // Notifications expect no response; write only.
    if (!running()) return {ErrorCode::Internal, "MCP stdio session is not running"};
    const std::string line = notification_json + "\n";
#if defined(_WIN32)
    DWORD written = 0;
    if (!WriteFile(impl_->stdin_write, line.data(), static_cast<DWORD>(line.size()), &written,
                   nullptr))
        return {ErrorCode::Internal, "MCP stdio write failed"};
#else
    std::size_t offset = 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 5000);
    while (offset < line.size()) {
        if (cancellation.cancelled()) return {ErrorCode::Cancelled, "MCP stdio notify cancelled"};
        if (std::chrono::steady_clock::now() > deadline)
            return {ErrorCode::Timeout, "MCP stdio notify timed out"};
        const ssize_t n = ::write(impl_->stdin_write.get(), line.data() + offset, line.size() - offset);
        if (n > 0) {
            offset += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return {ErrorCode::Internal, "MCP stdio write failed"};
    }
#endif
    (void)ignored;
    return ok_error();
}

Error StdioSession::request(const std::string& request_json,
                            long timeout_ms,
                            runtime::CancellationToken cancellation,
                            JsonRpcResponse& response) {
    response = JsonRpcResponse{};
    if (!running()) return {ErrorCode::Internal, "MCP stdio session is not running"};
    Error err = notify(request_json, timeout_ms, cancellation);
    if (!err.ok()) return err;

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 120000);
    while (true) {
        if (cancellation.cancelled()) return {ErrorCode::Cancelled, "MCP stdio request cancelled"};
        if (std::chrono::steady_clock::now() > deadline)
            return {ErrorCode::Timeout, "MCP stdio request timed out"};

        const std::size_t nl = impl_->read_buffer.find('\n');
        if (nl != std::string::npos) {
            std::string line = impl_->read_buffer.substr(0, nl);
            impl_->read_buffer.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            return parse_jsonrpc_response(line, response);
        }

#if defined(_WIN32)
        char buf[4096];
        DWORD got = 0;
        DWORD avail = 0;
        if (!PeekNamedPipe(impl_->stdout_read, nullptr, 0, nullptr, &avail, nullptr))
            return {ErrorCode::Internal, "MCP stdio read failed"};
        if (avail == 0) {
            Sleep(10);
            DWORD code = 0;
            if (GetExitCodeProcess(impl_->process, &code) && code != STILL_ACTIVE) {
                impl_->alive = false;
                return {ErrorCode::Internal, "MCP stdio process exited"};
            }
            continue;
        }
        if (!ReadFile(impl_->stdout_read, buf, sizeof(buf), &got, nullptr) || got == 0) {
            impl_->alive = false;
            return {ErrorCode::Internal, "MCP stdio read failed"};
        }
        impl_->read_buffer.append(buf, buf + got);
#else
        pollfd pfd{};
        pfd.fd = impl_->stdout_read.get();
        pfd.events = POLLIN;
        const int pr = ::poll(&pfd, 1, 50);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return {ErrorCode::Internal, "MCP stdio poll failed"};
        }
        if (pr == 0) {
            int status = 0;
            const pid_t r = ::waitpid(impl_->pid, &status, WNOHANG);
            if (r == impl_->pid) {
                impl_->alive = false;
                impl_->pid = -1;
                return {ErrorCode::Internal, "MCP stdio process exited"};
            }
            continue;
        }
        char buf[4096];
        const ssize_t n = ::read(impl_->stdout_read.get(), buf, sizeof(buf));
        if (n > 0) {
            impl_->read_buffer.append(buf, buf + n);
            continue;
        }
        if (n == 0) {
            impl_->alive = false;
            return {ErrorCode::Internal, "MCP stdio stdout closed"};
        }
        if (errno == EINTR || errno == EAGAIN) continue;
        return {ErrorCode::Internal, "MCP stdio read failed"};
#endif
    }
}

Error stdio_connect_negotiate(const ServerConfig& config,
                              const StdioTransportOptions& options,
                              StdioSession& session) {
    Error err = session.start(config, options);
    if (!err.ok()) return err;

    const ClientInfo client = default_client_info();
    auto try_stateless = [&]() -> Error {
        json::Value params = make_json_object();
        params.object["_meta"] = build_stateless_meta(client, "2026-07-28");
        json::Value req = build_request(1, "server/discover", params);
        JsonRpcResponse response;
        Error e = session.request(json::stringify(req), options.startup_timeout_ms,
                                  options.cancellation, response);
        if (!e.ok()) return e;
        if (response.has_error)
            return {ErrorCode::ProviderSchema, "server/discover failed: " + response.error_message};
        session.set_dialect(Dialect::Stateless20260728);
        return ok_error();
    };

    auto try_legacy = [&](const std::string& version, Dialect dialect) -> Error {
        json::Value req =
            build_request(1, "initialize", build_initialize_params(version, client));
        JsonRpcResponse response;
        Error e = session.request(json::stringify(req), options.startup_timeout_ms,
                                  options.cancellation, response);
        if (!e.ok()) return e;
        if (response.has_error)
            return {ErrorCode::ProviderSchema, "initialize failed: " + response.error_message};
        json::Value note = make_json_object();
        note.object["jsonrpc"] = make_json_string("2.0");
        note.object["method"] = make_json_string("notifications/initialized");
        (void)session.notify(json::stringify(note), options.startup_timeout_ms, options.cancellation);
        session.set_dialect(dialect);
        return ok_error();
    };

    if (config.protocol_hint == ProtocolHint::Stateless20260728) return try_stateless();
    if (config.protocol_hint == ProtocolHint::Legacy20251125)
        return try_legacy("2025-11-25", Dialect::Streamable20251125);
    if (config.protocol_hint == ProtocolHint::Legacy20250326)
        return try_legacy("2025-03-26", Dialect::Streamable20250326);

    if (config.last_dialect == Dialect::Stateless20260728) {
        err = try_stateless();
        if (err.ok()) return err;
        // Restart process after failed dialect probe.
        session.close();
        err = session.start(config, options);
        if (!err.ok()) return err;
    }

    err = try_stateless();
    if (err.ok()) return err;

    session.close();
    err = session.start(config, options);
    if (!err.ok()) return err;
    err = try_legacy("2025-11-25", Dialect::Streamable20251125);
    if (err.ok()) return err;

    session.close();
    err = session.start(config, options);
    if (!err.ok()) return err;
    return try_legacy("2025-03-26", Dialect::Streamable20250326);
}

}  // namespace ainiux::mcp
