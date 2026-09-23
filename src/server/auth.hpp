#pragma once

#include <string>
#include <string_view>

#include "server/http_parser.hpp"

namespace ainiux::server {

enum class AuthScope { None, FullControl, McpOnly };

struct AuthConfig {
    std::string full_control_secret;
    std::string mcp_secret;
    // Ephemeral per-listener token used only for browser-originated control
    // mutations. It is never persisted with either bearer credential.
    std::string csrf_token{};
};

bool constant_time_equal(std::string_view left, std::string_view right);
AuthScope authenticate(const http::Request& request, const AuthConfig& config);

}  // namespace ainiux::server
