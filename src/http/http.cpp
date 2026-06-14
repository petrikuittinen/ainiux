#include "http/http.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <vector>

#include "security/redact.hpp"

namespace pkchat::http {

namespace {

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

long parse_status_line(const std::string& headers) {
    std::istringstream lines(headers);
    std::string line;
    if (!std::getline(lines, line)) {
        return 0;
    }
    if (!line.empty() && line.back() == 13) {
        line.pop_back();
    }
    if (line.rfind("HTTP/", 0) != 0) {
        return 0;
    }
    const size_t first_space = line.find(" ");
    if (first_space == std::string::npos) {
        return 0;
    }
    char* end = nullptr;
    const char* start = line.c_str() + first_space + 1;
    const long status = std::strtol(start, &end, 10);
    if (end == start) {
        return 0;
    }
    return status;
}

bool is_proxy_connect_header(const std::string& headers) {
    std::istringstream lines(headers);
    std::string line;
    if (!std::getline(lines, line)) {
        return false;
    }
    if (!line.empty() && line.back() == 13) {
        line.pop_back();
    }
    return line.find("Connection established") != std::string::npos;
}

class HeaderBodyParser {
   public:
    Error feed(const std::string& chunk, const BodyCallback& on_body) {
        if (headers_done_) {
            return emit_body(chunk, on_body);
        }
        buffer_ += chunk;
        while (!headers_done_) {
            size_t sep = buffer_.find("\r\n\r\n");
            size_t sep_len = 4;
            const size_t lf_sep = buffer_.find("\n\n");
            if (lf_sep != std::string::npos && (sep == std::string::npos || lf_sep < sep)) {
                sep = lf_sep;
                sep_len = 2;
            }
            if (sep == std::string::npos) {
                return ok_error();
            }
            const std::string headers = buffer_.substr(0, sep);
            buffer_.erase(0, sep + sep_len);
            const long parsed_status = parse_status_line(headers);
            if (parsed_status == 0) {
                return {ErrorCode::Internal, "curl response included invalid HTTP headers"};
            }
            status_ = parsed_status;
            if ((status_ >= 100 && status_ < 200) || is_proxy_connect_header(headers)) {
                continue;
            }
            headers_done_ = true;
        }
        if (!buffer_.empty()) {
            const std::string body = buffer_;
            buffer_.clear();
            return emit_body(body, on_body);
        }
        return ok_error();
    }

    Error finish() const {
        if (!headers_done_) {
            return {ErrorCode::Internal, "curl response ended before HTTP headers completed"};
        }
        return ok_error();
    }

    long status() const { return status_; }
    const std::string& body() const { return body_; }

   private:
    std::string buffer_;
    std::string body_;
    long status_ = 0;
    bool headers_done_ = false;

    Error emit_body(const std::string& data, const BodyCallback& on_body) {
        body_ += data;
        if (on_body && status_ >= 200 && status_ < 300 && !data.empty()) {
            return on_body(data);
        }
        return ok_error();
    }
};

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
    args.emplace_back("--include");
    if (request.on_body) {
        args.emplace_back("--no-buffer");
    }
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

    std::string err_text;
    auto wait_child = [&]() -> Error {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0) {
            if (errno != EINTR) {
                return {ErrorCode::Internal, std::string("waitpid failed: ") + std::strerror(errno)};
            }
        }
        if (!WIFEXITED(status)) {
            return {ErrorCode::Internal, "curl transport terminated unexpectedly"};
        }
        const int exit_status = WEXITSTATUS(status);
        if (exit_status != 0) {
            err_text = redact_secrets(err_text, secrets);
            return classify_curl_error(exit_status, err_text, request.url);
        }
        return ok_error();
    };
    stdin_read.reset();
    stdout_write.reset();
    stderr_write.reset();
    if (request.method == "POST") {
        err = write_all(stdin_write.get(), request.body);
        if (!err.ok()) {
            kill(pid, SIGTERM);
            Error wait_err = wait_child();
            if (!wait_err.ok()) {
                return {{}, wait_err};
            }
            return {{}, err};
        }
    }
    stdin_write.reset();

    HeaderBodyParser parser;
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
                err = parser.feed(std::string(buf, static_cast<size_t>(n)), request.on_body);
                if (!err.ok()) {
                    kill(pid, SIGTERM);
                    stdout_read.reset();
                    stderr_read.reset();
                    Error wait_err = wait_child();
                    if (!wait_err.ok() && err.code == ErrorCode::Ok) {
                        return {{parser.status(), parser.body(), err_text}, wait_err};
                    }
                    err_text = redact_secrets(err_text, secrets);
                    return {{parser.status(), parser.body(), err_text}, err};
                }
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

    err_text = redact_secrets(err_text, secrets);
    err = wait_child();
    if (!err.ok()) {
        return {{parser.status(), parser.body(), err_text}, err};
    }
    err = parser.finish();
    if (!err.ok()) {
        return {{parser.status(), parser.body(), err_text}, err};
    }
    return {{parser.status(), parser.body(), err_text}, ok_error()};
}

}  // namespace pkchat::http
