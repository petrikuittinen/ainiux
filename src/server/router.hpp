#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

#include "server/auth.hpp"
#include "server/http_parser.hpp"
#include "server/job_service.hpp"

namespace ainiux::server {

class McpAdapter;
class SessionHub;
class WorkspaceService;
class ChatService;

struct PublicStatus {
    unsigned short port = 0;
    std::size_t max_connections = 0;
    std::size_t max_jobs = 0;
    const std::atomic<std::size_t>* active_connections = nullptr;
    JobService* jobs = nullptr;
    McpAdapter* mcp = nullptr;
    SessionHub* sessions = nullptr;
    std::size_t max_sessions = 0;
    WorkspaceService* workspace = nullptr;
    ChatService* chat_threads = nullptr;
    std::string bind_address = "127.0.0.1";
    bool tls = false;
    bool remote = false;
};

struct Response {
    int status = 200;
    std::string content_type = "application/json; charset=utf-8";
    std::string body;
    bool close = false;
    bool streaming = false;
    std::string allow;
    std::string cache_control = "no-store";
    std::string content_security_policy = "default-src 'none'; frame-ancestors 'none'";
    bool browser_asset = false;
    std::function<void(const std::function<bool(std::string_view)>&)> stream_body;
};

Response route_request(const http::Request& request,
                       const AuthConfig& auth,
                       const PublicStatus& status);
std::string serialize_response(const Response& response, bool keep_alive);

}  // namespace ainiux::server
