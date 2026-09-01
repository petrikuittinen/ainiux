#pragma once

#include <atomic>
#include <cstddef>
#include <string>

#include "server/auth.hpp"
#include "server/http_parser.hpp"

namespace ainiux::server {

struct PublicStatus {
    unsigned short port = 0;
    std::size_t max_connections = 0;
    std::size_t max_jobs = 0;
    const std::atomic<std::size_t>* active_connections = nullptr;
};

struct Response {
    int status = 200;
    std::string content_type = "application/json; charset=utf-8";
    std::string body;
    bool close = false;
};

Response route_request(const http::Request& request,
                       const AuthConfig& auth,
                       const PublicStatus& status);
std::string serialize_response(const Response& response, bool keep_alive);

}  // namespace ainiux::server
