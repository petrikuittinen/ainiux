#include "server/test_server.hpp"

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

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "cli/args.hpp"
#include "server/auth.hpp"
#include "server/http_parser.hpp"
#include "server/listener.hpp"
#include "server/router.hpp"
#include "server/server.hpp"
#include "support/test_support.hpp"

namespace ainiux::test::server_control {
namespace {

using ainiux::test::check;
using namespace ainiux::server;
namespace http = ainiux::server::http;

std::string request_text(const std::string& path = "/ainiux/v1/health",
                         const std::string& token = "controller") {
    return "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " +
           token + "\r\n\r\n";
}

http::Request parsed_request(const std::string& text) {
    http::Parser parser;
    check(parser.feed(text) == http::ParseState::Complete, "server test request parses");
    return parser.request();
}

void test_fragmented_parser_and_pipeline() {
    const std::string first = request_text();
    const std::string second = request_text("/ainiux/v1/status");
    http::Parser parser;
    for (std::size_t i = 0; i + 1 < first.size(); ++i) {
        check(parser.feed(std::string_view(first.data() + i, 1)) == http::ParseState::NeedMore,
              "fragmented HTTP request waits for all bytes");
    }
    check(parser.feed(std::string_view(first.data() + first.size() - 1, 1)) ==
              http::ParseState::Complete,
          "fragmented HTTP request completes on final byte");
    check(parser.request().path == "/ainiux/v1/health" && parser.request().body.empty(),
          "strict parser preserves normalized path and empty body");

    http::Parser pipelined;
    check(pipelined.feed(first + second) == http::ParseState::Complete,
          "parser completes the first pipelined request");
    check(pipelined.take_remaining() == second,
          "parser returns unconsumed pipelined bytes exactly");
}

void expect_parse_failure(const std::string& text, int status, const char* label) {
    http::Parser parser;
    check(parser.feed(text) == http::ParseState::Failed && parser.error().status == status, label);
}

void test_strict_framing_and_limits() {
    expect_parse_failure("GET /ainiux/v1/health HTTP/1.1\nHost: localhost\n\n", 400,
                         "bare-LF HTTP is rejected");
    expect_parse_failure("GET /ainiux/v1/%2e%2e/x HTTP/1.1\r\nHost: localhost\r\n\r\n", 400,
                         "encoded ambiguous path is rejected");
    expect_parse_failure("GET /../x HTTP/1.1\r\nHost: localhost\r\n\r\n", 400,
                         "path traversal is rejected");
    expect_parse_failure("POST /ainiux/v1/x HTTP/1.1\r\nHost: localhost\r\n"
                         "Content-Length: 0\r\nContent-Length: 0\r\n\r\n", 400,
                         "duplicate Content-Length is rejected");
    expect_parse_failure("POST /ainiux/v1/x HTTP/1.1\r\nHost: localhost\r\n"
                         "Transfer-Encoding: chunked\r\n\r\n", 501,
                         "transfer encoding is rejected");
    expect_parse_failure("GET / HTTP/1.1\r\nHost: localhost\r\n folded\r\n\r\n", 400,
                         "obsolete folded headers are rejected");
    expect_parse_failure("GET / HTTP/1.1\r\n\r\n", 400,
                         "missing HTTP/1.1 Host is rejected");

    http::Parser small(3);
    check(small.feed("POST /x HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\n\r\n") ==
              http::ParseState::Failed && small.error().status == 413,
          "route body limit is enforced before buffering the body");

    std::string headers = "GET / HTTP/1.1\r\nHost: localhost\r\n";
    for (int i = 0; i < 100; ++i) headers += "X-" + std::to_string(i) + ": y\r\n";
    headers += "\r\n";
    expect_parse_failure(headers, 431, "header count includes Host and is bounded at 100");
}

void test_auth_and_routes() {
    check(constant_time_equal("same", "same") && !constant_time_equal("same", "samf") &&
              !constant_time_equal("same", "same-longer"),
          "constant-time credential comparison handles equality and unequal lengths");
    AuthConfig config{"controller", "mcp-token"};
    http::Request full = parsed_request(request_text());
    check(authenticate(full, config) == AuthScope::FullControl,
          "full-control credential authenticates control route");
    http::Request wrong_scope = parsed_request(request_text("/ainiux/v1/status", "mcp-token"));
    check(authenticate(wrong_scope, config) == AuthScope::None,
          "MCP-only credential cannot authenticate control API");
    http::Request mcp = parsed_request(request_text("/mcp", "mcp-token"));
    check(authenticate(mcp, config) == AuthScope::McpOnly,
          "MCP-only credential authenticates only the MCP path");
    http::Request full_on_mcp = parsed_request(request_text("/mcp", "controller"));
    check(authenticate(full_on_mcp, config) == AuthScope::None,
          "full-control credential is not reused as an MCP credential");

    std::atomic<std::size_t> active{2};
    PublicStatus status{8766, 64, 128, &active};
    Response health = route_request(full, config, status);
    check(health.status == 200 && health.body == "{\"status\":\"ok\"}",
          "authenticated health is minimal");
    Response denied = route_request(wrong_scope, config, status);
    check(denied.status == 401 && denied.body.find("mcp-token") == std::string::npos,
          "wrong-scope response is unauthorized and does not expose credentials");
    http::Request status_request = parsed_request(request_text("/ainiux/v1/status"));
    Response status_response = route_request(status_request, config, status);
    check(status_response.status == 200 &&
              status_response.body.find("\"active\":2") != std::string::npos &&
              status_response.body.find("127.0.0.1") != std::string::npos,
          "status reports bounded public listener state");
    http::Request capabilities = parsed_request(request_text("/ainiux/v1/capabilities"));
    Response capability_response = route_request(capabilities, config, status);
    check(capability_response.status == 200 &&
              capability_response.body.find("\"mcp\":false") != std::string::npos &&
              capability_response.body.find("controller") == std::string::npos,
          "capabilities advertise only landed adapters and no secret");

    http::Request bad_host = full;
    bad_host.headers["host"] = "example.com";
    check(route_request(bad_host, config, status).status == 421,
          "non-loopback Host is rejected");
    http::Request bad_origin = full;
    bad_origin.headers["origin"] = "https://example.com";
    check(route_request(bad_origin, config, status).status == 403,
          "cross-origin request is rejected");
}

void test_server_cli_contract() {
    const char* argv[] = {"ainiux", "server", "--workspace", ".", "--port", "9001",
                          "--max-connections", "7", "--max-jobs", "9"};
    cli::ParseResult parsed = cli::parse_args(10, const_cast<char**>(argv));
    check(parsed.error.ok() && parsed.options.server && parsed.options.port == 9001 &&
              parsed.options.max_connections == 7 && parsed.options.max_jobs == 9,
          "server subcommand and bounded options parse");
    check(validate_server_options(parsed.options).ok(), "standalone server options validate");
    const char* combined[] = {"ainiux", "server", "-p", "hello"};
    parsed = cli::parse_args(4, const_cast<char**>(combined));
    check(parsed.error.ok() && !validate_server_options(parsed.options).ok(),
          "server mode rejects prompt combinations");
    const char* misplaced[] = {"ainiux", "-p", "hello", "--port", "9001"};
    parsed = cli::parse_args(5, const_cast<char**>(misplaced));
    check(parsed.error.ok() && !validate_server_options(parsed.options).ok(),
          "server-only options are rejected outside server mode");
}

#if !defined(_WIN32)
int open_loopback(unsigned short port) {
    const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) return -1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(socket_fd);
        return -1;
    }
    return socket_fd;
}

std::string raw_request(unsigned short port, const std::string& request) {
    const int socket_fd = open_loopback(port);
    if (socket_fd < 0) return {};
    std::size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t count = ::send(socket_fd, request.data() + sent, request.size() - sent, 0);
        if (count <= 0) break;
        sent += static_cast<std::size_t>(count);
    }
    (void)::shutdown(socket_fd, SHUT_WR);
    std::string response;
    char buffer[2048];
    for (;;) {
        const ssize_t count = ::recv(socket_fd, buffer, sizeof(buffer), 0);
        if (count <= 0) break;
        response.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(socket_fd);
    return response;
}

void test_loopback_listener_lifecycle() {
    Listener listener;
    ListenerConfig config;
    config.port = 0;
    config.max_connections = 2;
    config.auth.full_control_secret = "controller";
    check(listener.start(config).ok() && listener.port() != 0,
          "listener binds an ephemeral loopback port");
    std::atomic<bool> stop{false};
    Error serve_error;
    std::thread server_thread([&] { serve_error = listener.serve_until([&] { return stop.load(); }); });
    const std::string response = raw_request(listener.port(), request_text());
    check(response.find("HTTP/1.1 200 OK") == 0 &&
              response.find("X-Content-Type-Options: nosniff") != std::string::npos &&
              response.find("Content-Security-Policy: default-src 'none'") != std::string::npos,
          "raw loopback request reaches authenticated health with hardened headers");

    const int idle_client = open_loopback(listener.port());
    for (int i = 0; i < 50 && listener.active_connections() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(idle_client >= 0 && listener.active_connections() == 1,
          "listener tracks an idle active connection");
    stop.store(true);
    server_thread.join();
    if (idle_client >= 0) ::close(idle_client);
    check(serve_error.ok() && listener.active_connections() == 0,
          "listener shutdown joins workers and releases active connections");

    Listener capped;
    config.max_connections = 1;
    check(capped.start(config).ok(), "connection-cap listener starts");
    stop.store(false);
    std::thread capped_thread([&] { serve_error = capped.serve_until([&] { return stop.load(); }); });
    const int held = open_loopback(capped.port());
    for (int i = 0; i < 50 && capped.active_connections() != 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const std::string busy = raw_request(capped.port(), request_text());
    check(held >= 0 && busy.find("HTTP/1.1 429 Too Many Requests") == 0,
          "simultaneous connection cap rejects excess clients");
    if (held >= 0) ::close(held);
    stop.store(true);
    capped_thread.join();
}
#else
void test_loopback_listener_lifecycle() {
    // Native Windows socket lifecycle is compiled into the product target; the
    // platform integration gate exercises it with the packaged executable.
}
#endif

}  // namespace

void run_all() {
    test_fragmented_parser_and_pipeline();
    test_strict_framing_and_limits();
    test_auth_and_routes();
    test_server_cli_contract();
    test_loopback_listener_lifecycle();
}

}  // namespace ainiux::test::server_control
