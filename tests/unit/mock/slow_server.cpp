#include "mock/slow_server.hpp"

#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace ainiux::test::mock {

namespace {

std::string repo_root() {
    return ".";
}

}  // namespace

int pick_free_port() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return 0;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(fd);
        return 0;
    }
    socklen_t length = sizeof(address);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        close(fd);
        return 0;
    }
    const int port = ntohs(address.sin_port);
    close(fd);
    return port;
}

SlowHttpServer::SlowHttpServer() = default;

SlowHttpServer::~SlowHttpServer() {
    stop();
}

bool SlowHttpServer::start(double response_delay_seconds,
                           double chunk_delay_seconds,
                           int chunk_count) {
    stop();
    port_ = pick_free_port();
    if (port_ <= 0) {
        return false;
    }
    if (pipe(ready_pipe_) != 0) {
        port_ = 0;
        return false;
    }

    const std::string script = repo_root() + "/tests/mock_server/slow_http_mock.py";
    const std::string port = std::to_string(port_);
    const std::string response_delay = std::to_string(response_delay_seconds);
    const std::string chunk_delay = std::to_string(chunk_delay_seconds);
    const std::string chunks = std::to_string(chunk_count);
    const std::string ready_fd = std::to_string(ready_pipe_[1]);

    const pid_t child = fork();
    if (child < 0) {
        close(ready_pipe_[0]);
        close(ready_pipe_[1]);
        ready_pipe_[0] = ready_pipe_[1] = -1;
        port_ = 0;
        return false;
    }

    if (child == 0) {
        close(ready_pipe_[0]);
        execlp("python3", "python3", script.c_str(), "--host", "127.0.0.1", "--port",
               port.c_str(), "--response-delay", response_delay.c_str(), "--chunk-delay",
               chunk_delay.c_str(), "--chunk-count", chunks.c_str(), "--ready-fd",
               ready_fd.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    close(ready_pipe_[1]);
    ready_pipe_[1] = -1;
    pid_ = static_cast<int>(child);

    char ready_byte = 0;
    const ssize_t read_bytes = read(ready_pipe_[0], &ready_byte, 1);
    close(ready_pipe_[0]);
    ready_pipe_[0] = -1;
    if (read_bytes != 1 || ready_byte != '1') {
        stop();
        return false;
    }
    return true;
}

void SlowHttpServer::stop() {
    if (ready_pipe_[0] >= 0) {
        close(ready_pipe_[0]);
        ready_pipe_[0] = -1;
    }
    if (ready_pipe_[1] >= 0) {
        close(ready_pipe_[1]);
        ready_pipe_[1] = -1;
    }
    if (pid_ > 0) {
        kill(pid_, SIGTERM);
        int status = 0;
        bool reaped = false;
        for (int attempt = 0; attempt < 50; ++attempt) {
            const pid_t waited = waitpid(pid_, &status, WNOHANG);
            if (waited == pid_) {
                reaped = true;
                break;
            }
            if (waited < 0 && errno == ECHILD) {
                reaped = true;
                break;
            }
            usleep(10000);
        }
        if (!reaped) {
            kill(pid_, SIGKILL);
            while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {
            }
        }
        pid_ = -1;
    }
    port_ = 0;
}

std::string SlowHttpServer::base_url() const {
    return "http://127.0.0.1:" + std::to_string(port_);
}

}  // namespace ainiux::test::mock
