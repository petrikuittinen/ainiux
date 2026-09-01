#include "server/auth.hpp"

#include <cstddef>

#include "common.hpp"

namespace ainiux::server {

bool constant_time_equal(std::string_view left, std::string_view right) {
    const std::size_t count = left.size() > right.size() ? left.size() : right.size();
    unsigned int difference = static_cast<unsigned int>(left.size() ^ right.size());
    for (std::size_t i = 0; i < count; ++i) {
        const unsigned char a = i < left.size() ? static_cast<unsigned char>(left[i]) : 0U;
        const unsigned char b = i < right.size() ? static_cast<unsigned char>(right[i]) : 0U;
        difference |= static_cast<unsigned int>(a ^ b);
    }
    return difference == 0U;
}

AuthScope authenticate(const http::Request& request, const AuthConfig& config) {
    const auto found = request.headers.find("authorization");
    if (found == request.headers.end()) return AuthScope::None;
    constexpr std::string_view prefix = "Bearer ";
    if (found->second.size() <= prefix.size() ||
        ascii_lower(found->second.substr(0, prefix.size())) != "bearer ") {
        return AuthScope::None;
    }
    const std::string_view token(found->second.data() + prefix.size(),
                                 found->second.size() - prefix.size());
    // Scope is path-bound: an MCP-only credential never authenticates the
    // controller API, and a full-control credential is not an MCP credential.
    if (request.path == "/mcp") {
        return !config.mcp_secret.empty() && constant_time_equal(token, config.mcp_secret)
                   ? AuthScope::McpOnly
                   : AuthScope::None;
    }
    return !config.full_control_secret.empty() &&
                   constant_time_equal(token, config.full_control_secret)
               ? AuthScope::FullControl
               : AuthScope::None;
}

const char* auth_scope_name(AuthScope scope) {
    switch (scope) {
        case AuthScope::None: return "none";
        case AuthScope::FullControl: return "full_control";
        case AuthScope::McpOnly: return "mcp_only";
    }
    return "none";
}

}  // namespace ainiux::server
