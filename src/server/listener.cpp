#include "server/listener.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "server/http_parser.hpp"
#include "server/chat_service.hpp"
#include "server/job_service.hpp"
#include "server/mcp_adapter.hpp"
#include "server/router.hpp"
#include "server/session_hub.hpp"
#include "server/wire.hpp"
#include "server/workspace_service.hpp"

namespace ainiux::server {
namespace {

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
void close_socket(Socket socket) { if (socket != kInvalidSocket) closesocket(socket); }
void shutdown_socket(Socket socket) { if (socket != kInvalidSocket) shutdown(socket, SD_BOTH); }
bool interrupted_error() { return WSAGetLastError() == WSAEINTR; }
bool timeout_error() {
    const int error = WSAGetLastError();
    return error == WSAETIMEDOUT || error == WSAEWOULDBLOCK;
}
class NetworkGuard {
   public:
    NetworkGuard() { WSADATA data{}; ok_ = WSAStartup(MAKEWORD(2, 2), &data) == 0; }
    ~NetworkGuard() { if (ok_) WSACleanup(); }
    bool ok() const { return ok_; }
   private:
    bool ok_ = false;
};
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
void close_socket(Socket socket) { if (socket != kInvalidSocket) ::close(socket); }
void shutdown_socket(Socket socket) { if (socket != kInvalidSocket) ::shutdown(socket, SHUT_RDWR); }
bool interrupted_error() { return errno == EINTR; }
bool timeout_error() { return errno == EAGAIN || errno == EWOULDBLOCK; }
class NetworkGuard { public: bool ok() const { return true; } };
#endif

class OwnedSocket {
   public:
    explicit OwnedSocket(Socket socket = kInvalidSocket) : socket_(socket) {}
    ~OwnedSocket() { close_socket(socket_); }
    OwnedSocket(const OwnedSocket&) = delete;
    OwnedSocket& operator=(const OwnedSocket&) = delete;
    Socket get() const { return socket_; }
    Socket release() { const Socket out = socket_; socket_ = kInvalidSocket; return out; }
    void reset(Socket socket = kInvalidSocket) { close_socket(socket_); socket_ = socket; }
   private:
    Socket socket_;
};

bool send_all(Socket socket, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const std::size_t remaining = data.size() - sent;
        const int chunk = remaining > static_cast<std::size_t>(0x7fffffff)
                              ? 0x7fffffff
                              : static_cast<int>(remaining);
#if defined(MSG_NOSIGNAL)
        const int written = ::send(socket, data.data() + sent, chunk, MSG_NOSIGNAL);
#else
        const int written = ::send(socket, data.data() + sent, chunk, 0);
#endif
        if (written <= 0) {
            if (written < 0 && interrupted_error()) continue;
            return false;
        }
        sent += static_cast<std::size_t>(written);
    }
    return true;
}

void set_socket_timeouts(Socket socket) {
#if defined(_WIN32)
    const DWORD timeout_ms = 1000;
    (void)setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    (void)setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
                     reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    timeval timeout{1, 0};
    (void)setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#if defined(SO_NOSIGPIPE)
    int no_sigpipe = 1;
    (void)setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
#endif
}

std::atomic<unsigned long long> listener_request_counter{0};

std::string listener_request_id(const char* prefix) {
    return std::string(prefix) +
           std::to_string(listener_request_counter.fetch_add(1, std::memory_order_relaxed) + 1U);
}

Response parser_error_response(const http::ParseError& error) {
    Response response;
    response.status = error.status;
    response.close = true;
    const std::string code = error.status == 413 ? "content_too_large" :
                             error.status == 414 ? "uri_too_long" :
                             error.status == 431 ? "headers_too_large" :
                             error.status == 501 ? "unsupported_framing" :
                             error.status == 505 ? "http_version_unsupported" : "invalid_request";
    wire::ErrorEnvelope envelope;
    envelope.http_status = error.status;
    envelope.code = code;
    envelope.message = error.message;
    envelope.request_id = listener_request_id("req_parse_");
    response.body = wire::error_json(envelope);
    return response;
}

struct Worker {
    std::thread thread;
    std::atomic<bool> running{true};
    std::atomic<Socket> socket{kInvalidSocket};
};

}  // namespace

struct Listener::Impl {
    NetworkGuard network;
    OwnedSocket listener;
    ListenerConfig config;
    std::atomic<bool> stopping{false};
    std::atomic<std::size_t> active{0};
    unsigned short bound_port = 0;
    std::mutex workers_mutex;
    std::vector<std::shared_ptr<Worker>> workers;
    std::unique_ptr<JobService> jobs;
    std::unique_ptr<McpAdapter> mcp;
    std::unique_ptr<SessionHub> sessions;
    std::unique_ptr<WorkspaceService> workspace;
    std::unique_ptr<ChatService> chat_threads;

    void reap_workers() {
        std::lock_guard<std::mutex> lock(workers_mutex);
        auto it = workers.begin();
        while (it != workers.end()) {
            if (!(*it)->running.load(std::memory_order_acquire)) {
                if ((*it)->thread.joinable()) (*it)->thread.join();
                it = workers.erase(it);
            } else {
                ++it;
            }
        }
    }

    void serve_connection(const std::shared_ptr<Worker>& worker) {
        struct ActiveGuard {
            std::atomic<std::size_t>& active;
            std::shared_ptr<Worker> worker;
            ~ActiveGuard() {
                close_socket(worker->socket.exchange(kInvalidSocket, std::memory_order_acq_rel));
                active.fetch_sub(1, std::memory_order_acq_rel);
                worker->running.store(false, std::memory_order_release);
            }
        } guard{active, worker};

        const Socket socket = worker->socket.load(std::memory_order_acquire);
        set_socket_timeouts(socket);
        std::string pending;
        for (std::size_t count = 0; count < Limits::requests_per_connection && !stopping; ++count) {
            http::Parser parser;
            auto phase_started = std::chrono::steady_clock::now();
            bool received_any = !pending.empty();
            http::ParseState state = pending.empty() ? http::ParseState::NeedMore
                                                     : parser.feed(pending);
            if (parser.headers_complete()) phase_started = std::chrono::steady_clock::now();
            pending.clear();
            while (state == http::ParseState::NeedMore && !stopping) {
                char buffer[8192];
                const int received = ::recv(socket, buffer, static_cast<int>(sizeof(buffer)), 0);
                if (received > 0) {
                    if (!received_any) {
                        received_any = true;
                        phase_started = std::chrono::steady_clock::now();
                    }
                    const bool had_headers = parser.headers_complete();
                    state = parser.feed(std::string_view(buffer, static_cast<std::size_t>(received)));
                    if (!had_headers && parser.headers_complete()) {
                        phase_started = std::chrono::steady_clock::now();
                    }
                    continue;
                }
                if (received == 0) {
                    state = parser.finish();
                    break;
                }
                if (interrupted_error()) continue;
                if (!timeout_error()) return;
                const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - phase_started).count();
                const int limit = parser.headers_complete() ? Limits::body_timeout_seconds
                                  : (!received_any && count > 0 ? Limits::idle_timeout_seconds
                                                               : Limits::header_timeout_seconds);
                if (elapsed >= limit) {
                    http::ParseError timeout;
                    timeout.status = 400;
                    timeout.message = parser.headers_complete() ? "HTTP request body timed out"
                                                                : "HTTP request headers timed out";
                    (void)send_all(socket, serialize_response(parser_error_response(timeout), false));
                    return;
                }
            }
            if (stopping) return;
            if (state == http::ParseState::Failed) {
                (void)send_all(socket, serialize_response(parser_error_response(parser.error()), false));
                return;
            }
            pending = parser.take_remaining();
            PublicStatus public_status;
            public_status.port = bound_port;
            public_status.max_connections = config.max_connections;
            public_status.max_jobs = config.max_jobs;
            public_status.max_sessions = config.max_sessions;
            public_status.active_connections = &active;
            public_status.jobs = jobs.get();
            public_status.mcp = mcp.get();
            public_status.sessions = sessions.get();
            public_status.workspace = workspace.get();
            public_status.chat_threads = chat_threads.get();
            const Response response = route_request(parser.request(), config.auth, public_status);
            const bool keep_alive = parser.request().keep_alive &&
                                    count + 1U < Limits::requests_per_connection && !response.close;
            if (!send_all(socket, serialize_response(response, keep_alive))) return;
            if (response.streaming) {
                if (response.stream_body) {
                    response.stream_body([socket](std::string_view data) {
                        return send_all(socket, std::string(data));
                    });
                }
                return;
            }
            if (!keep_alive) return;
        }
    }
};

Listener::Listener() : impl_(std::make_unique<Impl>()) {}
Listener::~Listener() { stop(); }

Error Listener::start(ListenerConfig config) {
    if (!impl_->network.ok()) return {ErrorCode::Connect, "could not initialize the socket runtime"};
    if (config.max_connections == 0) return {ErrorCode::BadArgs, "--max-connections must be positive"};
    impl_->config = std::move(config);
    impl_->jobs = std::make_unique<JobService>(impl_->config.base_options,
                                                impl_->config.workspace,
                                                impl_->config.max_jobs);
    impl_->mcp = std::make_unique<McpAdapter>(impl_->jobs.get(), impl_->config.max_jobs);
    impl_->sessions = std::make_unique<SessionHub>(impl_->config.base_options,
                                                   impl_->config.workspace,
                                                   impl_->config.max_sessions);
    impl_->workspace = std::make_unique<WorkspaceService>(impl_->config.workspace);
    impl_->chat_threads = std::make_unique<ChatService>();
    impl_->stopping.store(false, std::memory_order_release);
    OwnedSocket socket(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (socket.get() == kInvalidSocket) return {ErrorCode::Connect, "could not create the loopback listener socket"};
    int reuse = 1;
#if defined(_WIN32)
    (void)setsockopt(socket.get(), SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
    (void)setsockopt(socket.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(impl_->config.port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        return {ErrorCode::Connect, "could not bind the control server to 127.0.0.1:" +
                                        std::to_string(impl_->config.port)};
    }
    if (::listen(socket.get(), static_cast<int>(std::min<std::size_t>(impl_->config.max_connections, 128U))) != 0) {
        return {ErrorCode::Connect, "could not listen on the loopback control socket"};
    }
    sockaddr_in actual{};
#if defined(_WIN32)
    int actual_size = sizeof(actual);
#else
    socklen_t actual_size = sizeof(actual);
#endif
    if (::getsockname(socket.get(), reinterpret_cast<sockaddr*>(&actual), &actual_size) != 0) {
        return {ErrorCode::Connect, "could not inspect the bound control socket"};
    }
    impl_->bound_port = ntohs(actual.sin_port);
    impl_->listener.reset(socket.release());
    return ok_error();
}

Error Listener::serve_until(const std::function<bool()>& should_stop) {
    if (impl_->listener.get() == kInvalidSocket) return {ErrorCode::Internal, "control listener is not started"};
    while (!impl_->stopping.load(std::memory_order_acquire) && !should_stop()) {
        impl_->reap_workers();
        fd_set reads;
        FD_ZERO(&reads);
        FD_SET(impl_->listener.get(), &reads);
        timeval timeout{0, 200000};
        const int ready = ::select(static_cast<int>(impl_->listener.get() + 1), &reads, nullptr, nullptr, &timeout);
        if (ready < 0) {
            if (interrupted_error()) continue;
            if (impl_->stopping) break;
            return {ErrorCode::Connect, "control listener wait failed"};
        }
        if (ready == 0) continue;
        sockaddr_in peer{};
#if defined(_WIN32)
        int peer_size = sizeof(peer);
#else
        socklen_t peer_size = sizeof(peer);
#endif
        OwnedSocket client(::accept(impl_->listener.get(), reinterpret_cast<sockaddr*>(&peer), &peer_size));
        if (client.get() == kInvalidSocket) {
            if (interrupted_error()) continue;
            if (impl_->stopping) break;
            continue;
        }
        if (ntohl(peer.sin_addr.s_addr) != INADDR_LOOPBACK) continue;
        if (impl_->active.load(std::memory_order_acquire) >= impl_->config.max_connections) {
            Response busy;
            busy.status = 429;
            busy.close = true;
            wire::ErrorEnvelope envelope;
            envelope.http_status = 429;
            envelope.code = "connection_limit";
            envelope.message = "too many simultaneous connections";
            envelope.request_id = listener_request_id("req_limit_");
            busy.body = wire::error_json(envelope);
            (void)send_all(client.get(), serialize_response(busy, false));
            continue;
        }
        auto worker = std::make_shared<Worker>();
        worker->socket.store(client.release(), std::memory_order_release);
        impl_->active.fetch_add(1, std::memory_order_acq_rel);
        worker->thread = std::thread([this, worker] { impl_->serve_connection(worker); });
        std::lock_guard<std::mutex> lock(impl_->workers_mutex);
        impl_->workers.push_back(std::move(worker));
    }
    stop();
    return ok_error();
}

void Listener::stop() {
    if (!impl_) return;
    impl_->stopping.store(true, std::memory_order_release);
    shutdown_socket(impl_->listener.get());
    impl_->listener.reset();
    std::vector<std::shared_ptr<Worker>> workers;
    {
        std::lock_guard<std::mutex> lock(impl_->workers_mutex);
        workers.swap(impl_->workers);
    }
    for (const auto& worker : workers) shutdown_socket(worker->socket.load(std::memory_order_acquire));
    if (impl_->sessions) impl_->sessions->shutdown();
    if (impl_->jobs) impl_->jobs->shutdown();
    for (const auto& worker : workers) if (worker->thread.joinable()) worker->thread.join();
}

unsigned short Listener::port() const { return impl_->bound_port; }
std::size_t Listener::active_connections() const { return impl_->active.load(std::memory_order_acquire); }

}  // namespace ainiux::server
