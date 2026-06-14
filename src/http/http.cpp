#include "http/http.hpp"

#include <fcntl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <sstream>
#include <vector>

#include "security/redact.hpp"

namespace pkchat::http {

namespace {

constexpr const char* kStatusMarker = "__PKCHAT_HTTP_STATUS__:";

class Fd {
   public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() { reset(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }
    int get() const { return fd_; }
    int release() {
        int tmp = fd_;
        fd_ = -1;
        return tmp;
    }
    void reset(int fd = -1) {
        if (fd_ >= 0) {
            close(fd_);
        }
        fd_ = fd;
    }

   private:
    int fd_ = -1;
};

Error pipe_pair(Fd& read_end, Fd& write_end) {
    int fds[2];
    if (pipe(fds) != 0) {
        return {ErrorCode::Internal, std::string("pipe failed: ") + std::strerror(errno)};
    }
    read_end.reset(fds[0]);
    write_end.reset(fds[1]);
    return ok_error();
}

Error write_all(int fd, const std::string& data) {
    size_t written = 0;
    while (written < data.size()) {
        const ssize_t n = write(fd, data.data() + written, data.size() - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return {ErrorCode::Internal, std::string("write to curl stdin failed: ") + std::strerror(errno)};
        }
        written += static_cast<size_t>(n);
    }
    return ok_error();
}

Error classify_curl_error(int exit_status, const std::string& stderr_text, const std::string& url) {
    std::string msg = "curl transport failed for " + url + ": " + stderr_text;
    if (exit_status == 6 || stderr_text.find("Could not resolve host") != std::string::npos) {
        return {ErrorCode::Dns, msg};
    }
    if (exit_status == 7 || stderr_text.find("Failed to connect") != std::string::npos ||
        stderr_text.find("Couldn't connect") != std::string::npos) {
        return {ErrorCode::Connect, msg};
    }
    if (exit_status == 28 || stderr_text.find("timed out") != std::string::npos) {
        return {ErrorCode::Timeout, msg};
    }
    if (exit_status == 35 || exit_status == 60 || stderr_text.find("SSL") != std::string::npos ||
        stderr_text.find("TLS") != std::string::npos) {
        return {ErrorCode::Tls, msg};
    }
    return {ErrorCode::Internal, msg};
}

}  // namespace

Result perform(const Request& request, const std::vector<std::string>& secrets) {
    Fd stdin_read;
    Fd stdin_write;
    Fd stdout_read;
    Fd stdout_write;
    Fd stderr_read;
    Fd stderr_write;
    Error err = pipe_pair(stdin_read, stdin_write);
    if (!err.ok()) {
        return {{}, err};
    }
    err = pipe_pair(stdout_read, stdout_write);
    if (!err.ok()) {
        return {{}, err};
    }
    err = pipe_pair(stderr_read, stderr_write);
    if (!err.ok()) {
        return {{}, err};
    }

    std::vector<std::string> args;
    args.emplace_back("curl");
    args.emplace_back("-sS");
    args.emplace_back("-X");
    args.emplace_back(request.method);
    args.emplace_back("--connect-timeout");
    args.emplace_back(std::to_string(request.connect_timeout_seconds));
    if (request.timeout_seconds > 0) {
        args.emplace_back("--max-time");
        args.emplace_back(std::to_string(request.timeout_seconds));
    }
    if (!request.proxy.empty()) {
        args.emplace_back("--proxy");
        args.emplace_back(request.proxy);
    }
    if (request.insecure_tls) {
        args.emplace_back("--insecure");
    }
    if (request.trace) {
        args.emplace_back("--verbose");
    }
    for (const std::string& header : request.headers) {
        args.emplace_back("-H");
        args.emplace_back(header);
    }
    if (request.method == "POST") {
        args.emplace_back("--data-binary");
        args.emplace_back("@-");
    }
    args.emplace_back("--write-out");
    args.emplace_back(std::string("\n") + kStatusMarker + "%{http_code}\n");
    args.emplace_back(request.url);

    const pid_t pid = fork();
    if (pid < 0) {
        return {{}, {ErrorCode::Internal, std::string("fork failed: ") + std::strerror(errno)}};
    }
    if (pid == 0) {
        dup2(stdin_read.get(), STDIN_FILENO);
        dup2(stdout_write.get(), STDOUT_FILENO);
        dup2(stderr_write.get(), STDERR_FILENO);
        stdin_read.reset();
        stdin_write.reset();
        stdout_read.reset();
        stdout_write.reset();
        stderr_read.reset();
        stderr_write.reset();
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (std::string& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        execvp("curl", argv.data());
        _exit(127);
    }

    stdin_read.reset();
    stdout_write.reset();
    stderr_write.reset();
    if (request.method == "POST") {
        err = write_all(stdin_write.get(), request.body);
        if (!err.ok()) {
            return {{}, err};
        }
    }
    stdin_write.reset();

    std::string out;
    std::string err_text;
    bool stdout_open = true;
    bool stderr_open = true;
    while (stdout_open || stderr_open) {
        fd_set readfds;
        FD_ZERO(&readfds);
        int maxfd = -1;
        if (stdout_open) {
            FD_SET(stdout_read.get(), &readfds);
            maxfd = std::max(maxfd, stdout_read.get());
        }
        if (stderr_open) {
            FD_SET(stderr_read.get(), &readfds);
            maxfd = std::max(maxfd, stderr_read.get());
        }
        if (select(maxfd + 1, &readfds, nullptr, nullptr, nullptr) < 0) {
            if (errno == EINTR) {
                continue;
            }
            return {{}, {ErrorCode::Internal, std::string("select failed: ") + std::strerror(errno)}};
        }
        char buf[8192];
        if (stdout_open && FD_ISSET(stdout_read.get(), &readfds)) {
            const ssize_t n = read(stdout_read.get(), buf, sizeof(buf));
            if (n > 0) {
                out.append(buf, static_cast<size_t>(n));
            } else if (n == 0) {
                stdout_open = false;
                stdout_read.reset();
            } else if (errno != EINTR) {
                return {{}, {ErrorCode::Internal, std::string("read curl stdout failed: ") + std::strerror(errno)}};
            }
        }
        if (stderr_open && FD_ISSET(stderr_read.get(), &readfds)) {
            const ssize_t n = read(stderr_read.get(), buf, sizeof(buf));
            if (n > 0) {
                err_text.append(buf, static_cast<size_t>(n));
            } else if (n == 0) {
                stderr_open = false;
                stderr_read.reset();
            } else if (errno != EINTR) {
                return {{}, {ErrorCode::Internal, std::string("read curl stderr failed: ") + std::strerror(errno)}};
            }
        }
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return {{}, {ErrorCode::Internal, std::string("waitpid failed: ") + std::strerror(errno)}};
        }
    }
    int exit_status = 0;
    if (WIFEXITED(status)) {
        exit_status = WEXITSTATUS(status);
    } else {
        return {{}, {ErrorCode::Internal, "curl transport terminated unexpectedly"}};
    }
    err_text = redact_secrets(err_text, secrets);
    if (exit_status != 0) {
        return {{}, classify_curl_error(exit_status, err_text, request.url)};
    }

    const size_t marker = out.rfind(kStatusMarker);
    if (marker == std::string::npos) {
        return {{}, {ErrorCode::Internal, "curl response did not include an HTTP status marker"}};
    }
    std::string body = out.substr(0, marker);
    if (!body.empty() && body.back() == '\n') {
        body.pop_back();
    }
    const std::string status_text = out.substr(marker + std::strlen(kStatusMarker));
    char* end = nullptr;
    const long http_status = std::strtol(status_text.c_str(), &end, 10);
    if (end == status_text.c_str()) {
        return {{}, {ErrorCode::Internal, "curl response included an invalid HTTP status marker"}};
    }
    return {{http_status, body, err_text}, ok_error()};
}

}  // namespace pkchat::http
