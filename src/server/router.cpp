#include "server/router.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>

#include "ainiux/version.hpp"
#include "json/json.hpp"
#include "provider/provider.hpp"
#include "server/limits.hpp"
#include "server/mcp_adapter.hpp"
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
                        ",\"bind\":{\"address\":\"127.0.0.1\",\"port\":" +
                        std::to_string(status.port) +
                        ",\"transport\":\"loopback_plain_http\"}" +
                        ",\"connections\":{\"active\":" + std::to_string(active) +
                        ",\"maximum\":" + std::to_string(status.max_connections) + "}" +
                        ",\"jobs\":{\"active\":" +
                        std::to_string(status.jobs == nullptr ? 0U : status.jobs->registry().active_count()) +
                        ",\"retained\":" +
                        std::to_string(status.jobs == nullptr ? 0U : status.jobs->registry().size()) +
                        ",\"maximum\":" + std::to_string(status.max_jobs) + "}}";
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
                        ",\"operations\":[\"health\",\"status\",\"capabilities\",\"chat\",\"run\",\"plan\",\"image\"]" +
                        ",\"authentication\":{\"scope\":\"full_control\",\"mcp_configured\":" +
                        std::string(auth.mcp_secret.empty() ? "false" : "true") + "}" +
                        ",\"adapters\":{\"mcp\":true,\"openai_v1\":false}" +
                        ",\"providers\":" + providers + "}";
        return response;
    }

    const std::string jobs_prefix = "/ainiux/v1/jobs/";
    if (request.path.rfind(jobs_prefix, 0) != 0) {
        return error_response(404, "not_found", "no control API route matches this path");
    }
    if (status.jobs == nullptr) return error_response(503, "jobs_unavailable", "job service is unavailable");
    std::string suffix = request.path.substr(jobs_prefix.size());
    if (suffix == "chat" || suffix == "run" || suffix == "plan" || suffix == "image") {
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
    headers +=
           "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff" +
           std::string("\r\nContent-Security-Policy: default-src 'none'; frame-ancestors 'none'");
    if (response.status == 401) headers += "\r\nWWW-Authenticate: Bearer realm=\"ainiux-control\"";
    if (response.status == 405 && !response.allow.empty()) headers += "\r\nAllow: " + response.allow;
    headers += std::string("\r\nConnection: ") + (persistent ? "keep-alive" : "close") +
               "\r\n\r\n";
    return headers + (response.streaming ? std::string() : response.body);
}

}  // namespace ainiux::server
