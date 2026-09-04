#include "server/router.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include "ainiux/version.hpp"
#include "encoding/encoding.hpp"
#include "json/json.hpp"
#include "provider/provider.hpp"
#include "server/chat_service.hpp"
#include "server/embedded_assets.hpp"
#include "server/limits.hpp"
#include "server/mcp_adapter.hpp"
#include "server/session_hub.hpp"
#include "server/wire.hpp"
#include "server/workspace_service.hpp"

namespace ainiux::server {
namespace {

std::atomic<unsigned long long> request_counter{0};

std::string request_id() {
    return "req_" + std::to_string(request_counter.fetch_add(1, std::memory_order_relaxed) + 1U);
}

bool decimal_port(const std::string& value, unsigned short expected) {
    if (value.empty() || (value.size() > 1U && value.front() == '0')) return false;
    unsigned long port = 0;
    for (unsigned char c : value) {
        if (c < '0' || c > '9') return false;
        port = port * 10U + static_cast<unsigned long>(c - '0');
        if (port > 65535U) return false;
    }
    return port == expected;
}

bool ipv4_literal(const std::string& value) {
    int components = 0;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t dot = value.find('.', start);
        const std::string part = value.substr(
            start, dot == std::string::npos ? std::string::npos : dot - start);
        if (part.empty() || part.size() > 3U || (part.size() > 1U && part.front() == '0')) return false;
        unsigned int number = 0;
        for (unsigned char c : part) {
            if (c < '0' || c > '9') return false;
            number = number * 10U + static_cast<unsigned int>(c - '0');
        }
        if (number > 255U) return false;
        ++components;
        if (dot == std::string::npos) break;
        start = dot + 1U;
    }
    return components == 4;
}

bool allowed_host(const std::string& host, const PublicStatus& status) {
    const std::size_t colon = host.rfind(':');
    std::string name = host;
    if (colon != std::string::npos) {
        if (host.find(':') != colon || !decimal_port(host.substr(colon + 1U), status.port)) return false;
        name = host.substr(0, colon);
    }
    const std::string bind = status.bind_address.empty() ? "127.0.0.1"
                                                         : ascii_lower(status.bind_address);
    if (!status.remote) return name == "localhost" || name == bind || name == "127.0.0.1";
    if (colon == std::string::npos && status.port != (status.tls ? 443U : 80U)) return false;
    if (bind == "0.0.0.0") return ipv4_literal(name) && name != "0.0.0.0";
    return name == bind;
}

Response error_response(int status,
                        const std::string& code,
                        const std::string& message,
                        const std::string& details = "{}") {
    wire::ErrorEnvelope envelope;
    envelope.http_status = status;
    envelope.code = code;
    envelope.message = message;
    envelope.details_json = details;
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
        case 201: return "Created";
        case 202: return "Accepted";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 410: return "Gone";
        case 415: return "Unsupported Media Type";
        case 422: return "Unprocessable Content";
        case 413: return "Content Too Large";
        case 414: return "URI Too Long";
        case 421: return "Misdirected Request";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        case 501: return "Not Implemented";
        case 505: return "HTTP Version Not Supported";
        default: return "Error";
    }
}

bool parse_event_id(const std::string& value, std::uint64_t& output) {
    if (value.empty()) return false;
    std::uint64_t number = 0;
    for (unsigned char c : value) {
        if (c < '0' || c > '9') return false;
        const unsigned int digit = c - '0';
        if (number > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) return false;
        number = number * 10U + digit;
    }
    output = number;
    return true;
}

bool json_content_type(const http::Request& request) {
    const auto found = request.headers.find("content-type");
    if (found == request.headers.end()) return false;
    const std::string value = ascii_lower(found->second);
    return value == "application/json" || value.rfind("application/json;", 0) == 0;
}

Response job_not_found() {
    return error_response(404, "job_not_found", "no retained job has this identifier");
}

Response session_error(const Error& error) {
    int status = 500;
    std::string code = "internal";
    switch (error.code) {
        case ErrorCode::BadArgs:
        case ErrorCode::JsonParse: status = 400; code = "invalid_request"; break;
        case ErrorCode::UnsupportedFeature: status = 422; code = "unsupported_feature"; break;
        case ErrorCode::RateLimit: status = 429; code = "session_limit"; break;
        case ErrorCode::FileRead: status = 404; code = "not_found"; break;
        case ErrorCode::FileLock: status = 409; code = "session_conflict"; break;
        case ErrorCode::Cancelled: status = 409; code = "cancelled"; break;
        default: break;
    }
    return error_response(status, code, error.message);
}

Response session_not_found() {
    return error_response(404, "session_not_found", "no retained interactive session has this identifier");
}

Response workspace_error(const Error& error, const std::string& current_revision = {}) {
    int status = 500;
    std::string code = "internal";
    switch (error.code) {
        case ErrorCode::BadArgs:
        case ErrorCode::JsonParse: status = 400; code = "invalid_request"; break;
        case ErrorCode::FileRead: status = 404; code = "not_found"; break;
        case ErrorCode::FileLock: status = 409; code = "revision_conflict"; break;
        case ErrorCode::FileWrite: status = 409; code = "mutation_failed"; break;
        default: break;
    }
    return error_response(status, code, error.message,
                          error.code != ErrorCode::FileLock || current_revision.empty()
                              ? std::string()
                              : "{\"current_revision\":" +
                                    json::quote(current_revision) + "}");
}

Response chat_thread_error(const Error& error, long long current_revision = 0) {
    switch (error.code) {
        case ErrorCode::BadArgs:
        case ErrorCode::JsonParse:
            return error_response(400, "invalid_request", error.message);
        case ErrorCode::FileRead:
            return error_response(404, "thread_not_found", error.message);
        case ErrorCode::FileLock:
            return error_response(409, "revision_conflict", error.message,
                                  "{\"current_revision\":" +
                                      std::to_string(current_revision) + "}");
        case ErrorCode::FileWrite:
            return error_response(409, "thread_read_only", error.message);
        default:
            return error_response(500, "chat_store_error", error.message);
    }
}

bool positive_decimal_id(const std::string& text, long long& value) {
    if (text.empty()) return false;
    long long parsed = 0;
    for (unsigned char c : text) {
        if (c < '0' || c > '9') return false;
        const int digit = c - '0';
        if (parsed > (std::numeric_limits<long long>::max() - digit) / 10LL) return false;
        parsed = parsed * 10LL + digit;
    }
    if (parsed <= 0) return false;
    value = parsed;
    return true;
}

int hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

Error decode_query_component(const std::string& encoded, std::string& decoded) {
    decoded.clear();
    decoded.reserve(encoded.size());
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        const unsigned char c = static_cast<unsigned char>(encoded[index]);
        if (c != '%') {
            decoded.push_back(static_cast<char>(c));
            continue;
        }
        if (index + 2U >= encoded.size()) {
            return {ErrorCode::BadArgs, "query path contains an incomplete percent escape"};
        }
        const int high = hex_digit(static_cast<unsigned char>(encoded[index + 1U]));
        const int low = hex_digit(static_cast<unsigned char>(encoded[index + 2U]));
        if (high < 0 || low < 0) {
            return {ErrorCode::BadArgs, "query path contains an invalid percent escape"};
        }
        const unsigned char value = static_cast<unsigned char>((high << 4) | low);
        if (value == 0 || value == '\r' || value == '\n') {
            return {ErrorCode::BadArgs, "query path contains an invalid encoded byte"};
        }
        decoded.push_back(static_cast<char>(value));
        index += 2U;
    }
    if (!encoding::is_valid_utf8(decoded)) {
        return {ErrorCode::BadArgs, "query path must decode to valid UTF-8"};
    }
    return ok_error();
}

Error query_path(const http::Request& request, bool required, std::string& path) {
    path.clear();
    if (request.query.empty()) {
        if (required) return {ErrorCode::BadArgs, "query parameter path is required"};
        return ok_error();
    }
    std::size_t start = 0;
    bool found = false;
    while (start <= request.query.size()) {
        const std::size_t ampersand = request.query.find('&', start);
        const std::string item = request.query.substr(
            start, ampersand == std::string::npos ? std::string::npos : ampersand - start);
        const std::size_t equals = item.find('=');
        if (equals == std::string::npos || item.substr(0, equals) != "path" || found) {
            return {ErrorCode::BadArgs, "query accepts only one path parameter"};
        }
        Error decoded = decode_query_component(item.substr(equals + 1U), path);
        if (!decoded.ok()) return decoded;
        found = true;
        if (ampersand == std::string::npos) break;
        start = ampersand + 1U;
    }
    if (!found || (required && path.empty())) {
        return {ErrorCode::BadArgs, "query parameter path is required"};
    }
    return ok_error();
}

Response job_submission_response(const ServiceSubmitResult& submitted) {
    if (!submitted.validation_error.ok()) {
        const wire::ErrorEnvelope envelope =
            wire::error_envelope(submitted.validation_error, request_id());
        Response response;
        response.status = envelope.http_status;
        response.body = wire::error_json(envelope);
        return response;
    }
    const SubmitResult& result = submitted.submission;
    if (result.status == SubmitStatus::Created || result.status == SubmitStatus::Existing) {
        Response response;
        response.status = result.status == SubmitStatus::Created ? 202 : 200;
        response.body = "{\"existing\":" +
                        std::string(result.status == SubmitStatus::Existing ? "true" : "false") +
                        ",\"job\":" + result.job->snapshot_json() + "}";
        return response;
    }
    if (result.status == SubmitStatus::IdempotencyConflict) {
        return error_response(409, "idempotency_conflict",
                              "the idempotency key was already used with different input",
                              "{\"job_id\":" + json::quote(result.conflicting_job_id) + "}");
    }
    if (result.status == SubmitStatus::AgentConflict) {
        return error_response(409, "agent_lane_busy",
                              "an agent operation is already active for this workspace",
                              "{\"job_id\":" + json::quote(result.conflicting_job_id) + "}");
    }
    if (result.status == SubmitStatus::Capacity) {
        return error_response(429, "job_limit", "the bounded in-memory job registry is full");
    }
    if (result.status == SubmitStatus::InternalFailure) {
        return error_response(500, "job_start_failed", "the server could not start the job worker");
    }
    return error_response(503, "server_stopping", "the server is stopping and accepts no new jobs");
}

}  // namespace

bool preflight_request_body(const http::Request& request,
                            std::size_t content_length,
                            const AuthConfig& auth,
                            const PublicStatus& status,
                            Response& denial) {
    const bool upload = request.method == "POST" &&
                        request.path == "/ainiux/v1/images/inputs";
    const std::size_t limit = upload ? Limits::upload_body_bytes : Limits::json_body_bytes;
    if (content_length > limit) {
        denial = error_response(413, "content_too_large",
                                upload ? "image upload exceeds the 20 MiB per-file limit"
                                       : "HTTP request body exceeds the 1 MiB JSON limit");
        return false;
    }
    if (!upload) return true;
    const auto host = request.headers.find("host");
    const std::string normalized_host = host == request.headers.end()
                                            ? std::string() : ascii_lower(host->second);
    if (host == request.headers.end() || !allowed_host(normalized_host, status)) {
        denial = error_response(421, "invalid_host", "Host is not allowed for this listener");
        return false;
    }
    const auto origin = request.headers.find("origin");
    if (origin != request.headers.end()) {
        const std::string expected = std::string(status.tls ? "https://" : "http://") + normalized_host;
        if (ascii_lower(origin->second) != expected) {
            denial = error_response(403, "origin_rejected", "cross-origin requests are disabled");
            return false;
        }
    }
    if (authenticate(request, auth) != AuthScope::FullControl) {
        denial = error_response(401, "authentication_failed",
                                "a valid Bearer controller credential is required");
        return false;
    }
    const auto type = request.headers.find("content-type");
    const std::string mime = type == request.headers.end()
                                 ? std::string() : ascii_lower(ascii_trim(type->second));
    if (mime != "image/png" && mime != "image/jpeg") {
        denial = error_response(415, "unsupported_media_type",
                                "image input uploads require Content-Type: image/png or image/jpeg");
        return false;
    }
    return true;
}

Response route_request(const http::Request& request,
                       const AuthConfig& auth,
                       const PublicStatus& status) {
    const auto host = request.headers.find("host");
    const std::string normalized_host = host == request.headers.end()
                                            ? std::string()
                                            : ascii_lower(host->second);
    if (host == request.headers.end() || !allowed_host(normalized_host, status)) {
        return error_response(421, "invalid_host", "Host is not allowed for this listener");
    }
    const auto origin = request.headers.find("origin");
    if (origin != request.headers.end()) {
        const std::string expected = std::string(status.tls ? "https://" : "http://") + normalized_host;
        if (ascii_lower(origin->second) != expected) {
            return error_response(403, "origin_rejected", "cross-origin requests are disabled");
        }
    }
    if (is_web_ui_path(request.path)) {
        if (request.method != "GET") {
            Response denied = error_response(405, "method_not_allowed", "web UI assets accept GET only");
            denied.allow = "GET";
            return denied;
        }
        if (!request.query.empty() || !request.body.empty()) {
            return error_response(400, "invalid_request", "web UI assets do not accept a query or body");
        }
        EmbeddedAsset asset;
        if (!find_embedded_asset(request.path, asset)) {
            return error_response(404, "asset_not_found", "no embedded web UI asset matches this path");
        }
        Response web;
        web.content_type = std::string(asset.content_type);
        web.body.assign(asset.content.data(), asset.content.size());
        web.cache_control = asset.immutable
                                ? "public, max-age=31536000, immutable"
                                : "no-store";
        web.content_security_policy =
            "default-src 'none'; script-src 'self'; style-src 'self'; "
            "img-src 'self' data:; connect-src 'self'; base-uri 'none'; "
            "form-action 'self'; frame-ancestors 'none'";
        web.browser_asset = true;
        return web;
    }
    const AuthScope scope = authenticate(request, auth);
    if (scope == AuthScope::None) {
        return error_response(401, "authentication_failed",
                              "a valid Bearer controller credential is required");
    }
    if (request.path == "/mcp") {
        if (scope != AuthScope::McpOnly) {
            return error_response(403, "insufficient_scope", "this credential cannot access the MCP adapter");
        }
        if (status.mcp == nullptr) return error_response(503, "mcp_unavailable", "the MCP adapter is unavailable");
        const McpResponse mcp_response = status.mcp->handle(request);
        Response response;
        response.status = mcp_response.status;
        response.content_type = mcp_response.content_type;
        response.body = mcp_response.body;
        response.close = mcp_response.status == 400 || mcp_response.status == 405;
        response.allow = mcp_response.allow;
        return response;
    }
    if (scope != AuthScope::FullControl) {
        return error_response(403, "insufficient_scope", "this credential cannot access the control API");
    }
    Response response;
    if (request.path == "/ainiux/v1/health") {
        if (request.method != "GET") {
            response = error_response(405, "method_not_allowed", "health accepts GET only");
            response.allow = "GET";
            return response;
        }
        if (!request.body.empty()) return error_response(400, "invalid_request", "health does not accept a body");
        response.body = "{\"status\":\"ok\"}";
        return response;
    }
    if (request.path == "/ainiux/v1/status") {
        if (request.method != "GET") {
            response = error_response(405, "method_not_allowed", "status accepts GET only");
            response.allow = "GET";
            return response;
        }
        if (!request.body.empty()) return error_response(400, "invalid_request", "status does not accept a body");
        const std::size_t active = status.active_connections == nullptr
                                       ? 0U
                                       : status.active_connections->load(std::memory_order_acquire);
        response.body = "{\"api_version\":" + json::quote(wire::kApiVersion) +
                        ",\"status\":\"ready\",\"auth_scope\":\"full_control\"" +
                        ",\"bind\":{\"address\":" +
                        json::quote(status.bind_address.empty() ? "127.0.0.1" : status.bind_address) +
                        ",\"port\":" +
                        std::to_string(status.port) +
                        ",\"transport\":" +
                        json::quote(status.tls
                                        ? (status.remote ? "remote_https" : "loopback_https")
                                        : (status.remote ? "remote_plain_http" : "loopback_plain_http")) + "}" +
                        ",\"connections\":{\"active\":" + std::to_string(active) +
                        ",\"maximum\":" + std::to_string(status.max_connections) + "}" +
                        ",\"jobs\":{\"active\":" +
                        std::to_string(status.jobs == nullptr ? 0U : status.jobs->registry().active_count()) +
                        ",\"retained\":" +
                        std::to_string(status.jobs == nullptr ? 0U : status.jobs->registry().size()) +
                        ",\"maximum\":" + std::to_string(status.max_jobs) + "}}";
        if (status.sessions != nullptr) {
            response.body.resize(response.body.size() - 1U);
            response.body += ",\"sessions\":{\"active\":" +
                             std::to_string(status.sessions->size()) +
                             ",\"maximum\":" + std::to_string(status.max_sessions) + "}}";
        }
        return response;
    }
    if (request.path == "/ainiux/v1/capabilities") {
        if (request.method != "GET") {
            response = error_response(405, "method_not_allowed", "capabilities accepts GET only");
            response.allow = "GET";
            return response;
        }
        if (!request.body.empty()) return error_response(400, "invalid_request", "capabilities does not accept a body");
        std::string providers = "[";
        bool first = true;
        for (const provider::Profile& profile : provider::built_in_profiles()) {
            if (!first) providers += ',';
            first = false;
            providers += json::quote(profile.name);
        }
        providers += ']';
        response.body = "{\"api_version\":" + json::quote(wire::kApiVersion) +
                        ",\"operations\":[\"health\",\"status\",\"capabilities\",\"image_catalog\",\"image_inputs\",\"models\",\"chat\",\"run\",\"plan\",\"image\",\"editor_assist\",\"sessions\",\"review\",\"dired\",\"workspace_mutations\",\"files\",\"chat_threads\"]" +
                        ",\"authentication\":{\"scope\":\"full_control\",\"mcp_configured\":" +
                        std::string(auth.mcp_secret.empty() ? "false" : "true") + "}" +
                        ",\"adapters\":{\"mcp\":true,\"openai_v1\":false,\"web_ui\":true}" +
                        ",\"providers\":" + providers +
                        ",\"sessions\":{\"maximum\":" + std::to_string(status.max_sessions) +
                        ",\"active\":" + std::to_string(status.sessions == nullptr ? 0U : status.sessions->size()) + "}}";
        return response;
    }

    if (request.path == "/ainiux/v1/images/catalog") {
        if (request.method != "GET") {
            response = error_response(405, "method_not_allowed", "image catalog accepts GET only");
            response.allow = "GET";
            return response;
        }
        if (!request.query.empty() || !request.body.empty()) {
            return error_response(400, "invalid_request", "image catalog does not accept a query or body");
        }
        if (status.jobs == nullptr) {
            return error_response(503, "images_unavailable", "image service is unavailable");
        }
        response.body = status.jobs->image_catalog_json();
        return response;
    }

    const std::string image_inputs_path = "/ainiux/v1/images/inputs";
    if (request.path == image_inputs_path) {
        if (request.method != "POST") {
            response = error_response(405, "method_not_allowed", "image input uploads accept POST only");
            response.allow = "POST";
            return response;
        }
        if (!request.query.empty()) {
            return error_response(400, "invalid_request", "image input uploads do not accept query parameters");
        }
        const auto content_type = request.headers.find("content-type");
        const std::string mime = content_type == request.headers.end()
                                     ? std::string() : ascii_lower(ascii_trim(content_type->second));
        if (mime != "image/png" && mime != "image/jpeg") {
            return error_response(415, "unsupported_media_type",
                                  "image input uploads require Content-Type: image/png or image/jpeg");
        }
        if (status.jobs == nullptr) {
            return error_response(503, "images_unavailable", "image service is unavailable");
        }
        StoredImageInput stored;
        const Error error = status.jobs->add_image_input(mime, request.body, stored);
        if (!error.ok()) {
            if (error.code == ErrorCode::RateLimit) {
                return error_response(429, "image_input_capacity", error.message);
            }
            return error_response(error.code == ErrorCode::UnsupportedFeature ? 415 : 400,
                                  "invalid_image_input", error.message);
        }
        response.status = 201;
        response.body = "{\"id\":" + json::quote(stored.id) +
                        ",\"mime_type\":" + json::quote(stored.mime_type) +
                        ",\"size\":" + std::to_string(stored.bytes->size()) +
                        ",\"expires_at\":" +
                            json::quote(image_input_expiry_timestamp(stored.expires_at)) + "}";
        return response;
    }
    if (request.path.rfind(image_inputs_path + "/", 0) == 0) {
        if (request.method != "DELETE") {
            response = error_response(405, "method_not_allowed", "image input deletion accepts DELETE only");
            response.allow = "DELETE";
            return response;
        }
        if (!request.query.empty() || !request.body.empty()) {
            return error_response(400, "invalid_request", "image input deletion does not accept a query or body");
        }
        const std::string id = request.path.substr(image_inputs_path.size() + 1U);
        if (id.empty() || id.find('/') != std::string::npos || status.jobs == nullptr ||
            !status.jobs->remove_image_input(id)) {
            return error_response(404, "image_input_not_found",
                                  "uploaded image input is missing or expired");
        }
        response.body = "{\"deleted\":true,\"id\":" + json::quote(id) + "}";
        return response;
    }

    if (request.path == "/ainiux/v1/workspace/review" ||
        request.path == "/ainiux/v1/dired" ||
        request.path == "/ainiux/v1/dired/mutations" ||
        request.path == "/ainiux/v1/files") {
        if (status.workspace == nullptr) {
            return error_response(503, "workspace_unavailable", "the workspace service is unavailable");
        }
        std::string body;
        Error error;
        if (request.path == "/ainiux/v1/workspace/review") {
            if (request.method != "GET") {
                response = error_response(405, "method_not_allowed", "workspace review accepts GET only");
                response.allow = "GET";
                return response;
            }
            if (!request.body.empty()) return error_response(400, "invalid_request", "workspace review does not accept a body");
            if (!request.query.empty()) {
                return error_response(400, "invalid_request", "workspace review does not accept query parameters");
            }
            error = status.workspace->review(body);
        } else if (request.path == "/ainiux/v1/dired") {
            if (request.method != "GET") {
                response = error_response(405, "method_not_allowed", "dired listing accepts GET only");
                response.allow = "GET";
                return response;
            }
            if (!request.body.empty()) return error_response(400, "invalid_request", "dired listing does not accept a body");
            std::string path;
            error = query_path(request, false, path);
            if (error.ok()) error = status.workspace->list(path, body);
        } else if (request.path == "/ainiux/v1/dired/mutations") {
            if (request.method != "POST") {
                response = error_response(405, "method_not_allowed", "workspace mutations accept POST only");
                response.allow = "POST";
                return response;
            }
            if (!request.query.empty()) return error_response(400, "invalid_request", "workspace mutations do not accept query parameters");
            if (!json_content_type(request)) return error_response(415, "unsupported_media_type", "workspace mutations require Content-Type: application/json");
            error = status.workspace->mutate(request.body, body);
        } else if (request.method == "GET" || request.method == "PUT") {
            std::string path;
            error = query_path(request, true, path);
            if (request.method == "GET") {
                if (!request.body.empty()) return error_response(400, "invalid_request", "file reads do not accept a body");
                if (error.ok()) error = status.workspace->read(path, body);
            } else {
                if (!json_content_type(request)) return error_response(415, "unsupported_media_type", "file saves require Content-Type: application/json");
                std::string current_revision;
                if (error.ok()) error = status.workspace->save(path, request.body, body, current_revision);
                if (!error.ok()) return workspace_error(error, current_revision);
            }
        } else if (request.method == "POST") {
            if (!request.query.empty()) return error_response(400, "invalid_request", "file creation does not accept query parameters");
            if (!json_content_type(request)) return error_response(415, "unsupported_media_type", "file creation requires Content-Type: application/json");
            std::string current_revision;
            error = status.workspace->create_file(request.body, body, current_revision);
            if (!error.ok()) return workspace_error(error, current_revision);
            response.status = 201;
        } else {
            response = error_response(405, "method_not_allowed", "files accept GET, PUT, and POST only");
            response.allow = "GET, PUT, POST";
            return response;
        }
        if (!error.ok()) return workspace_error(error);
        response.body = std::move(body);
        return response;
    }

    const std::string chat_threads_prefix = "/ainiux/v1/chat/threads";
    if (request.path == chat_threads_prefix) {
        if (status.chat_threads == nullptr) {
            return error_response(503, "chat_unavailable", "the chat thread service is unavailable");
        }
        if (!request.query.empty()) {
            return error_response(400, "invalid_request", "chat thread routes do not accept query parameters");
        }
        if (request.method == "GET") {
            if (!request.body.empty()) {
                return error_response(400, "invalid_request", "thread listing does not accept a body");
            }
            std::string body;
            const Error error = status.chat_threads->list(body);
            if (!error.ok()) return chat_thread_error(error);
            response.body = std::move(body);
            return response;
        }
        if (request.method != "POST") {
            response = error_response(405, "method_not_allowed",
                                      "chat thread collection accepts GET and POST only");
            response.allow = "GET, POST";
            return response;
        }
        if (!json_content_type(request)) {
            return error_response(415, "unsupported_media_type",
                                  "thread creation requires Content-Type: application/json");
        }
        std::string body;
        const Error error = status.chat_threads->create(request.body, body);
        if (!error.ok()) return chat_thread_error(error);
        response.status = 201;
        response.body = std::move(body);
        return response;
    }
    if (request.path.rfind(chat_threads_prefix + "/", 0) == 0) {
        if (status.chat_threads == nullptr) {
            return error_response(503, "chat_unavailable", "the chat thread service is unavailable");
        }
        if (!request.query.empty()) {
            return error_response(400, "invalid_request", "chat thread routes do not accept query parameters");
        }
        const std::string suffix = request.path.substr(chat_threads_prefix.size() + 1U);
        const std::size_t slash = suffix.find('/');
        const std::string id_text = slash == std::string::npos ? suffix : suffix.substr(0, slash);
        const std::string action = slash == std::string::npos ? std::string() : suffix.substr(slash + 1U);
        long long thread_id = 0;
        if (!positive_decimal_id(id_text, thread_id) ||
            (slash != std::string::npos && action.empty()) ||
            (!action.empty() && action != "messages" && action != "regenerate")) {
            return error_response(404, "thread_route_not_found", "no chat thread route matches this path");
        }
        if (action.empty()) {
            if (request.method != "GET") {
                response = error_response(405, "method_not_allowed", "chat thread loading accepts GET only");
                response.allow = "GET";
                return response;
            }
            if (!request.body.empty()) {
                return error_response(400, "invalid_request", "chat thread loading does not accept a body");
            }
            std::string body;
            const Error error = status.chat_threads->load(thread_id, body);
            if (!error.ok()) return chat_thread_error(error);
            response.body = std::move(body);
            return response;
        }
        if (request.method != "POST") {
            response = error_response(
                405, "method_not_allowed",
                action == "regenerate" ? "chat regeneration accepts POST only"
                                       : "message append accepts POST only");
            response.allow = "POST";
            return response;
        }
        if (!json_content_type(request)) {
            return error_response(415, "unsupported_media_type",
                                  action == "regenerate"
                                      ? "chat regeneration requires Content-Type: application/json"
                                      : "message append requires Content-Type: application/json");
        }
        std::string body;
        long long current_revision = 0;
        const Error error = action == "regenerate"
                                ? status.chat_threads->rewind_last_answer(
                                      thread_id, request.body, body, current_revision)
                                : status.chat_threads->append(
                                      thread_id, request.body, body, current_revision);
        if (!error.ok()) return chat_thread_error(error, current_revision);
        response.body = std::move(body);
        return response;
    }

    const std::string sessions_prefix = "/ainiux/v1/sessions";
    if (request.path == sessions_prefix || request.path == sessions_prefix + "/agent") {
        if (status.sessions == nullptr) return error_response(503, "sessions_unavailable", "session hub is unavailable");
        const bool agent_path = request.path == sessions_prefix + "/agent";
        if (request.method == "GET" && !agent_path) {
            if (!request.body.empty()) return error_response(400, "invalid_request", "session listing does not accept a body");
            response.body = status.sessions->list_json();
            return response;
        }
        if (request.method != "POST") {
            response = error_response(405, "method_not_allowed",
                                       agent_path ? "agent session creation accepts POST only"
                                                  : "session collection accepts GET and POST only");
            response.allow = agent_path ? "POST" : "GET, POST";
            return response;
        }
        if (!json_content_type(request))
            return error_response(415, "unsupported_media_type", "session creation requires Content-Type: application/json");
        const SessionCreateResult created = status.sessions->create(request.body);
        if (!created.error.ok()) return session_error(created.error);
        response.status = 202;
        response.body = "{\"session\":" + created.session->snapshot_json() + "}";
        return response;
    }
    if (request.path.rfind(sessions_prefix + "/", 0) == 0) {
        if (status.sessions == nullptr) return error_response(503, "sessions_unavailable", "session hub is unavailable");
        std::string suffix = request.path.substr(sessions_prefix.size() + 1U);
        const std::size_t slash = suffix.find('/');
        const std::string session_id = slash == std::string::npos ? suffix : suffix.substr(0, slash);
        const std::string action = slash == std::string::npos ? std::string() : suffix.substr(slash + 1U);
        const std::shared_ptr<InteractiveSession> session = status.sessions->find(session_id);
        if (!session) return session_not_found();
        if (action.empty()) {
            if (request.method == "GET") {
                if (!request.body.empty()) return error_response(400, "invalid_request", "session status does not accept a body");
                response.body = session->snapshot_json();
                return response;
            }
            if (request.method != "DELETE") {
                response = error_response(405, "method_not_allowed", "session status accepts GET or DELETE only");
                response.allow = "GET, DELETE";
                return response;
            }
            if (!request.body.empty()) return error_response(400, "invalid_request", "session close does not accept a body");
            if (!status.sessions->erase(session_id)) return session_not_found();
            response.body = "{\"deleted\":true,\"id\":" + json::quote(session_id) + "}";
            return response;
        }
        if (action == "events") {
            if (request.method != "GET") {
                response = error_response(405, "method_not_allowed", "session events accept GET only");
                response.allow = "GET";
                return response;
            }
            if (!request.body.empty()) return error_response(400, "invalid_request", "session events do not accept a body");
            std::uint64_t after = 0;
            const auto last = request.headers.find("last-event-id");
            if (last != request.headers.end() && !parse_event_id(last->second, after))
                return error_response(400, "invalid_last_event_id", "Last-Event-ID must be an unsigned decimal integer");
            const ReplayBatch initial = session->events().replay_after(after);
            if (initial.expired)
                return error_response(410, "replay_expired", "the requested events are no longer retained; fetch the session snapshot");
            response.content_type = "text/event-stream; charset=utf-8";
            response.close = true;
            response.streaming = true;
            response.stream_body = [session, after](const std::function<bool(std::string_view)>& write) {
                std::uint64_t cursor = after;
                auto last_write = std::chrono::steady_clock::now();
                for (;;) {
                    const ReplayBatch batch = session->events().wait_after(cursor, std::chrono::seconds(1));
                    if (batch.expired) {
                        (void)write("event: replay_expired\ndata: {\"code\":\"replay_expired\"}\n\n");
                        return;
                    }
                    for (const wire::Event& event : batch.events) {
                        if (!write(sse_record(event))) return;
                        cursor = event.id;
                        last_write = std::chrono::steady_clock::now();
                    }
                    if (batch.closed && batch.events.empty()) return;
                    if (std::chrono::steady_clock::now() - last_write >=
                        std::chrono::seconds(Limits::sse_heartbeat_seconds)) {
                        if (!write(": heartbeat\n\n")) return;
                        last_write = std::chrono::steady_clock::now();
                    }
                }
            };
            return response;
        }
        if (action == "turns") {
            if (request.method != "POST") {
                response = error_response(405, "method_not_allowed", "turn submission accepts POST only");
                response.allow = "POST";
                return response;
            }
            if (!json_content_type(request))
                return error_response(415, "unsupported_media_type", "turn submission requires Content-Type: application/json");
            std::string turn_id;
            const Error error = session->start_turn(request.body, turn_id);
            if (!error.ok()) return session_error(error);
            response.status = 202;
            response.body = "{\"session_id\":" + json::quote(session_id) +
                            ",\"turn_id\":" + json::quote(turn_id) + "}";
            return response;
        }
        if (action == "reasoning") {
            if (request.method != "POST") {
                response = error_response(405, "method_not_allowed",
                                          "session reasoning accepts POST only");
                response.allow = "POST";
                return response;
            }
            if (!json_content_type(request)) {
                return error_response(415, "unsupported_media_type",
                                      "session reasoning requires Content-Type: application/json");
            }
            const Error error = session->set_reasoning(request.body);
            if (!error.ok()) return session_error(error);
            response.body = session->snapshot_json();
            return response;
        }
        if (action == "settings") {
            if (request.method != "POST") {
                response = error_response(405, "method_not_allowed",
                                          "session settings accept POST only");
                response.allow = "POST";
                return response;
            }
            if (!json_content_type(request)) {
                return error_response(415, "unsupported_media_type",
                                      "session settings require Content-Type: application/json");
            }
            const Error error = session->set_settings(request.body);
            if (!error.ok()) return session_error(error);
            response.body = session->snapshot_json();
            return response;
        }
        if (action.rfind("turns/", 0) == 0 && action.size() > 13U &&
            action.substr(action.size() - 7U) == "/cancel") {
            if (request.method != "POST") {
                response = error_response(405, "method_not_allowed", "turn cancellation accepts POST only");
                response.allow = "POST";
                return response;
            }
            if (!request.body.empty()) return error_response(400, "invalid_request", "turn cancellation does not accept a body");
            const std::string turn_id = action.substr(6U, action.size() - 13U);
            const Error error = session->cancel_turn(turn_id);
            if (!error.ok()) return session_error(error);
            response.body = session->snapshot_json();
            return response;
        }
        if (action.rfind("approvals/", 0) == 0) {
            const std::string approval_tail = action.substr(10U);
            if (approval_tail.size() > 12U && approval_tail.substr(approval_tail.size() - 12U) == "/review-file") {
                if (request.method != "GET") {
                    response = error_response(405, "method_not_allowed", "approval review accepts GET only");
                    response.allow = "GET";
                    return response;
                }
                if (!request.body.empty()) return error_response(400, "invalid_request", "approval review does not accept a body");
                std::string review;
                const Error error = session->review_file(approval_tail.substr(0, approval_tail.size() - 12U), review);
                if (!error.ok()) return session_error(error);
                response.body = std::move(review);
                return response;
            }
            if (request.method != "POST") {
                response = error_response(405, "method_not_allowed", "approval resolution accepts POST only");
                response.allow = "POST";
                return response;
            }
            if (!json_content_type(request))
                return error_response(415, "unsupported_media_type", "approval resolution requires Content-Type: application/json");
            const json::ParseResult parsed = json::parse(request.body);
            if (!parsed.error.ok() || !parsed.value.is_object())
                return error_response(400, "invalid_request", "approval body must be a JSON object");
            const json::Value* decision = parsed.value.get("decision");
            if (decision == nullptr || !decision->is_string())
                return error_response(400, "invalid_request", "approval decision must be a string");
            const Error error = session->resolve_approval(approval_tail, decision->string);
            if (!error.ok()) return session_error(error);
            response.body = session->snapshot_json();
            return response;
        }
        return session_not_found();
    }

    const std::string jobs_prefix = "/ainiux/v1/jobs/";
    if (request.path.rfind(jobs_prefix, 0) != 0) {
        return error_response(404, "not_found", "no control API route matches this path");
    }
    if (status.jobs == nullptr) return error_response(503, "jobs_unavailable", "job service is unavailable");
    std::string suffix = request.path.substr(jobs_prefix.size());
    if (suffix == "models" || suffix == "chat" || suffix == "run" || suffix == "plan" || suffix == "image" ||
        suffix == "editor-assist") {
        if (request.method != "POST") {
            response = error_response(405, "method_not_allowed", "job submission accepts POST only");
            response.allow = "POST";
            return response;
        }
        if (!json_content_type(request)) {
            return error_response(415, "unsupported_media_type", "job submission requires Content-Type: application/json");
        }
        std::string idempotency_key;
        const auto key = request.headers.find("idempotency-key");
        if (key != request.headers.end()) {
            idempotency_key = key->second;
            if (idempotency_key.empty() || idempotency_key.size() > 128U) {
                return error_response(400, "invalid_idempotency_key",
                                      "Idempotency-Key must contain 1 to 128 bytes");
            }
        }
        return job_submission_response(status.jobs->submit(suffix, request.body, idempotency_key));
    }

    std::string action;
    const std::size_t slash = suffix.find('/');
    if (slash != std::string::npos) {
        action = suffix.substr(slash + 1U);
        suffix.resize(slash);
    }
    if (suffix.empty() || suffix.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") !=
                              std::string::npos) {
        return job_not_found();
    }
    std::shared_ptr<Job> job = status.jobs->registry().find(suffix);
    if (!job) return job_not_found();
    if (action.empty()) {
        if (request.method != "GET") {
            response = error_response(405, "method_not_allowed", "job status accepts GET only");
            response.allow = "GET";
            return response;
        }
        if (!request.body.empty()) return error_response(400, "invalid_request", "job status does not accept a body");
        response.body = job->snapshot_json();
        return response;
    }
    if (action == "cancel") {
        if (request.method != "POST") {
            response = error_response(405, "method_not_allowed", "job cancellation accepts POST only");
            response.allow = "POST";
            return response;
        }
        if (!request.body.empty()) return error_response(400, "invalid_request", "job cancellation does not accept a body");
        std::shared_ptr<Job> cancelled;
        if (!status.jobs->registry().cancel(job->id, cancelled)) cancelled = job;
        response.body = cancelled->snapshot_json();
        return response;
    }
    if (action == "events") {
        if (request.method != "GET") {
            response = error_response(405, "method_not_allowed", "job events accept GET only");
            response.allow = "GET";
            return response;
        }
        if (!request.body.empty()) return error_response(400, "invalid_request", "job events do not accept a body");
        std::uint64_t after = 0;
        const auto last = request.headers.find("last-event-id");
        if (last != request.headers.end() && !parse_event_id(last->second, after)) {
            return error_response(400, "invalid_last_event_id", "Last-Event-ID must be an unsigned decimal integer");
        }
        const ReplayBatch initial = job->events.replay_after(after);
        if (initial.expired) {
            return error_response(410, "replay_expired",
                                  "the requested events are no longer retained; fetch the job snapshot");
        }
        response.status = 200;
        response.content_type = "text/event-stream; charset=utf-8";
        response.close = true;
        response.streaming = true;
        response.stream_body = [job, after](const std::function<bool(std::string_view)>& write) {
            std::uint64_t cursor = after;
            auto last_write = std::chrono::steady_clock::now();
            for (;;) {
                const ReplayBatch batch = job->events.wait_after(cursor, std::chrono::seconds(1));
                if (batch.expired) {
                    (void)write("event: replay_expired\ndata: {\"code\":\"replay_expired\"}\n\n");
                    return;
                }
                for (const wire::Event& event : batch.events) {
                    const std::string record = sse_record(event);
                    if (!write(record)) return;
                    cursor = event.id;
                    last_write = std::chrono::steady_clock::now();
                }
                if (batch.closed && batch.events.empty()) return;
                if (std::chrono::steady_clock::now() - last_write >=
                    std::chrono::seconds(Limits::sse_heartbeat_seconds)) {
                    if (!write(": heartbeat\n\n")) return;
                    last_write = std::chrono::steady_clock::now();
                }
            }
        };
        return response;
    }
    return error_response(404, "not_found", "no control API route matches this path");
}

std::string serialize_response(const Response& response, bool keep_alive) {
    const bool persistent = keep_alive && !response.close && !response.streaming;
    std::string headers = "HTTP/1.1 " + std::to_string(response.status) + " " +
                          reason_phrase(response.status) + "\r\nContent-Type: " +
                          response.content_type;
    if (!response.streaming) headers += "\r\nContent-Length: " + std::to_string(response.body.size());
    headers += "\r\nCache-Control: " + response.cache_control +
               "\r\nX-Content-Type-Options: nosniff" +
               "\r\nContent-Security-Policy: " + response.content_security_policy;
    if (response.browser_asset) {
        headers += "\r\nReferrer-Policy: no-referrer"
                   "\r\nPermissions-Policy: camera=(), microphone=(), geolocation=(), payment=(), usb=()"
                   "\r\nCross-Origin-Opener-Policy: same-origin"
                   "\r\nCross-Origin-Resource-Policy: same-origin"
                   "\r\nX-Frame-Options: DENY";
    }
    if (response.status == 401) headers += "\r\nWWW-Authenticate: Bearer realm=\"ainiux-control\"";
    if (response.status == 405 && !response.allow.empty()) headers += "\r\nAllow: " + response.allow;
    headers += std::string("\r\nConnection: ") + (persistent ? "keep-alive" : "close") +
               "\r\n\r\n";
    return headers + (response.streaming ? std::string() : response.body);
}

}  // namespace ainiux::server
