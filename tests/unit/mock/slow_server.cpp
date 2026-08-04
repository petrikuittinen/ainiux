#include "mock/slow_server.hpp"

#include "platform/environment.hpp"
#include "runtime/subprocess.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ainiux::test::mock {
namespace {
namespace fs = std::filesystem;

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
void close_socket(Socket socket) { closesocket(socket); }
class WinsockGuard {
   public:
    WinsockGuard() { WSADATA data{}; ready_ = WSAStartup(MAKEWORD(2, 2), &data) == 0; }
    ~WinsockGuard() { if (ready_) WSACleanup(); }
    bool ready() const { return ready_; }
   private:
    bool ready_ = false;
};
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
void close_socket(Socket socket) { close(socket); }
#endif

std::string python_executable() {
#if defined(_WIN32)
    const std::string path = platform::environment_value("PATH");
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t end = path.find(';', start);
        const std::string directory =
            path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const fs::path candidate = fs::u8path(directory) / "python.exe";
        std::error_code error;
        if (!directory.empty() && fs::u8path(directory).is_absolute() &&
            fs::is_regular_file(candidate, error) && !error)
            return fs::canonical(candidate, error).u8string();
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return {};
#else
    static const char* candidates[] = {"/usr/bin/python3", "/usr/local/bin/python3"};
    for (const char* candidate : candidates) {
        std::error_code error;
        if (fs::is_regular_file(candidate, error) && !error) return candidate;
    }
    return {};
#endif
}

bool connect_once(int port) {
#if defined(_WIN32)
    WinsockGuard winsock;
    if (!winsock.ready()) return false;
#endif
    const Socket socket_value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_value == kInvalidSocket) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<unsigned short>(port));
    const bool connected =
        connect(socket_value, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    close_socket(socket_value);
    return connected;
}

void append_environment(std::vector<std::string>& environment, const char* name) {
    const std::string value = platform::environment_value(name);
    if (!value.empty()) environment.push_back(std::string(name) + "=" + value);
}

}  // namespace

int pick_free_port() {
#if defined(_WIN32)
    WinsockGuard winsock;
    if (!winsock.ready()) return 0;
#endif
    const Socket socket_value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_value == kInvalidSocket) return 0;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(socket_value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close_socket(socket_value);
        return 0;
    }
#if defined(_WIN32)
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    if (getsockname(socket_value, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        close_socket(socket_value);
        return 0;
    }
    const int port = ntohs(address.sin_port);
    close_socket(socket_value);
    return port;
}

SlowHttpServer::SlowHttpServer() = default;
SlowHttpServer::~SlowHttpServer() { stop(); }

bool SlowHttpServer::start(double response_delay_seconds,
                           double chunk_delay_seconds,
                           int chunk_count) {
    stop();
    port_ = pick_free_port();
    const std::string python = python_executable();
    if (port_ <= 0 || python.empty()) {
        port_ = 0;
        return false;
    }
    const fs::path script = fs::absolute("tests/mock_server/slow_http_mock.py");
    cancellation_ = runtime::CancellationSource();
    runtime::SubprocessOptions options;
    options.executable = python;
    options.arguments = {script.u8string(), "--host", "127.0.0.1", "--port",
                         std::to_string(port_), "--response-delay",
                         std::to_string(response_delay_seconds), "--chunk-delay",
                         std::to_string(chunk_delay_seconds), "--chunk-count",
                         std::to_string(chunk_count)};
    options.timeout_ms = 0;
    options.stdout_limit = 4096;
    options.stderr_limit = 4096;
    options.cancellation = cancellation_.token();
    append_environment(options.environment, "PATH");
    append_environment(options.environment, "SystemRoot");
    append_environment(options.environment, "WINDIR");
    append_environment(options.environment, "HOME");
    append_environment(options.environment, "USERPROFILE");
    append_environment(options.environment, "TEMP");
    append_environment(options.environment, "TMP");
    running_.store(true, std::memory_order_release);
    worker_ = std::thread([this, options = std::move(options)]() mutable {
        runtime::SubprocessResult ignored;
        (void)runtime::run_subprocess(options, ignored);
        running_.store(false, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (connect_once(port_)) return true;
        if (!running()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    stop();
    return false;
}

void SlowHttpServer::stop() {
    cancellation_.cancel();
    if (worker_.joinable()) worker_.join();
    running_.store(false, std::memory_order_release);
    port_ = 0;
}

std::string SlowHttpServer::base_url() const {
    return "http://127.0.0.1:" + std::to_string(port_);
}

}  // namespace ainiux::test::mock
