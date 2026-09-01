#include "server/router.hpp"

#include <atomic>
#include <string>

#include "ainiux/version.hpp"
#include "json/json.hpp"
#include "provider/provider.hpp"
#include "server/limits.hpp"
#include "server/wire.hpp"

namespace ainiux::server {
namespace {

std::atomic<unsigned long long> request_counter{0};

std::string request_id() {
    return "req_" + std::to_string(request_counter.fetch_add(1, std::memory_order_relaxed) + 1U);
}

bool decimal_port(const std::string& value) {
    if (value.empty()) return false;
    for (unsigned char c : value) if (c < '0' || c > '9') return false;
    return true;
}

bool loopback_host(const std::string& host) {
    if (host == "localhost" || host == "127.0.0.1" || host == "[::1]") return true;
    if (host.rfind("localhost:", 0) == 0) return decimal_port(host.substr(10));
    if (host.rfind("127.0.0.1:", 0) == 0) return decimal_port(host.substr(10));
    if (host.rfind("[::1]:", 0) == 0) return decimal_port(host.substr(6));
    return false;
}

Response error_response(int status, const std::string& code, const std::string& message) {
    wire::ErrorEnvelope envelope;
    envelope.http_status = status;
    envelope.code = code;
    envelope.message = message;
    envelope.request_id = request_id();
    Response response;
    response.status = status;
    response.body = wire::error_json(envelope);
    response.close = status == 400 || status == 401 || status == 403 || status == 421;
    return response;
}

const char* reason_phrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Content Too Large";
        case 414: return "URI Too Long";
        case 421: return "Misdirected Request";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 505: return "HTTP Version Not Supported";
        default: return "Error";
    }
}

}  // namespace

Response route_request(const http::Request& request,
                       const AuthConfig& auth,
                       const PublicStatus& status) {
    const auto host = request.headers.find("host");
    const std::string normalized_host = host == request.headers.end()
                                            ? std::string()
                                            : ascii_lower(host->second);
    if (host == request.headers.end() || !loopback_host(normalized_host)) {
        return error_response(421, "invalid_host", "Host must name the loopback listener");
    }
    const auto origin = request.headers.find("origin");
    if (origin != request.headers.end()) {
        const std::string expected = "http://" + normalized_host;
        if (ascii_lower(origin->second) != expected) {
            return error_response(403, "origin_rejected", "cross-origin requests are disabled");
        }
    }
    if (!request.body.empty()) {
        return error_response(400, "invalid_request", "this route does not accept a request body");
    }

    const AuthScope scope = authenticate(request, auth);
    if (scope == AuthScope::None) {
        return error_response(401, "authentication_failed",
                              "a valid Bearer controller credential is required");
    }
    if (request.path == "/mcp") {
        return error_response(404, "not_found", "the MCP adapter is not enabled in this build");
    }
    if (scope != AuthScope::FullControl) {
        return error_response(403, "insufficient_scope", "this credential cannot access the control API");
    }
    if (request.method != "GET") {
        return error_response(405, "method_not_allowed", "this PR exposes GET discovery routes only");
    }

    Response response;
    if (request.path == "/ainiux/v1/health") {
        response.body = "{\"status\":\"ok\"}";
        return response;
    }
    if (request.path == "/ainiux/v1/status") {
        const std::size_t active = status.active_connections == nullptr
                                       ? 0U
                                       : status.active_connections->load(std::memory_order_acquire);
        response.body = "{\"api_version\":" + json::quote(wire::kApiVersion) +
                        ",\"status\":\"ready\",\"auth_scope\":\"full_control\"" +
                        ",\"bind\":{\"address\":\"127.0.0.1\",\"port\":" +
                        std::to_string(status.port) +
                        ",\"transport\":\"loopback_plain_http\"}" +
                        ",\"connections\":{\"active\":" + std::to_string(active) +
                        ",\"maximum\":" + std::to_string(status.max_connections) + "}" +
                        ",\"jobs\":{\"maximum\":" + std::to_string(status.max_jobs) + "}}";
        return response;
    }
    if (request.path == "/ainiux/v1/capabilities") {
        std::string providers = "[";
        bool first = true;
        for (const provider::Profile& profile : provider::built_in_profiles()) {
            if (!first) providers += ',';
            first = false;
            providers += json::quote(profile.name);
        }
        providers += ']';
        response.body = "{\"api_version\":" + json::quote(wire::kApiVersion) +
                        ",\"operations\":[\"health\",\"status\",\"capabilities\"]" +
                        ",\"authentication\":{\"scope\":\"full_control\",\"mcp_configured\":" +
                        std::string(auth.mcp_secret.empty() ? "false" : "true") + "}" +
                        ",\"adapters\":{\"mcp\":false,\"openai_v1\":false}" +
                        ",\"providers\":" + providers + "}";
        return response;
    }
    return error_response(404, "not_found", "no control API route matches this path");
}

std::string serialize_response(const Response& response, bool keep_alive) {
    const bool persistent = keep_alive && !response.close;
    std::string headers = "HTTP/1.1 " + std::to_string(response.status) + " " +
                          reason_phrase(response.status) + "\r\nContent-Type: " +
                          response.content_type +
           "\r\nContent-Length: " + std::to_string(response.body.size()) +
           "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff" +
           std::string("\r\nContent-Security-Policy: default-src 'none'; frame-ancestors 'none'");
    if (response.status == 401) headers += "\r\nWWW-Authenticate: Bearer realm=\"ainiux-control\"";
    if (response.status == 405) headers += "\r\nAllow: GET";
    headers += std::string("\r\nConnection: ") + (persistent ? "keep-alive" : "close") +
               "\r\n\r\n";
    return headers + response.body;
}

}  // namespace ainiux::server
