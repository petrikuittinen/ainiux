#include "mock/slow_server.hpp"

#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

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
        std::ostringstream command;
        command << "python3 " << repo_root() << "/tests/mock_server/slow_http_mock.py"
                << " --host 127.0.0.1"
                << " --port " << port_
                << " --response-delay " << response_delay_seconds
                << " --chunk-delay " << chunk_delay_seconds
                << " --chunk-count " << chunk_count
                << " --ready-fd " << ready_pipe_[1];
        execl("/bin/sh", "sh", "-c", command.str().c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    close(ready_pipe_[1]);
    ready_pipe_[1] = -1;
    pid_ = static_cast<int>(child);

    char ready_byte = 0;
    const ssize_t read_bytes = read(ready_pipe_[0], &ready_byte, 1);
    close(ready_pipe_[0]);
    ready_pipe_[0] = -1;
    return read_bytes == 1 && ready_byte == '1';
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
        for (int attempt = 0; attempt < 50; ++attempt) {
            const pid_t waited = waitpid(pid_, &status, WNOHANG);
            if (waited == pid_) {
                break;
            }
            usleep(10000);
        }
        kill(pid_, SIGKILL);
        waitpid(pid_, nullptr, 0);
        pid_ = -1;
    }
    port_ = 0;
}

std::string SlowHttpServer::base_url() const {
    return "http://127.0.0.1:" + std::to_string(port_);
}

}  // namespace ainiux::test::mock