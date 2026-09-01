#include "server/router.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include "ainiux/version.hpp"
#include "json/json.hpp"
#include "provider/provider.hpp"
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

Response workspace_error(const Error& error) {
    int status = 500;
    std::string code = "internal";
    switch (error.code) {
        case ErrorCode::BadArgs: status = 400; code = "invalid_request"; break;
        case ErrorCode::FileRead: status = 404; code = "not_found"; break;
        default: break;
    }
    return error_response(status, code, error.message);
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
        path = item.substr(equals + 1U);
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
                        ",\"operations\":[\"health\",\"status\",\"capabilities\",\"chat\",\"run\",\"plan\",\"image\",\"sessions\",\"review\",\"dired\",\"files\"]" +
                        ",\"authentication\":{\"scope\":\"full_control\",\"mcp_configured\":" +
                        std::string(auth.mcp_secret.empty() ? "false" : "true") + "}" +
                        ",\"adapters\":{\"mcp\":true,\"openai_v1\":false}" +
                        ",\"providers\":" + providers +
                        ",\"sessions\":{\"maximum\":" + std::to_string(status.max_sessions) +
                        ",\"active\":" + std::to_string(status.sessions == nullptr ? 0U : status.sessions->size()) + "}}";
        return response;
    }

    if (request.path == "/ainiux/v1/workspace/review" ||
        request.path == "/ainiux/v1/dired" || request.path == "/ainiux/v1/files") {
        if (status.workspace == nullptr) {
            return error_response(503, "workspace_unavailable", "the workspace service is unavailable");
        }
        if (request.method != "GET") {
            response = error_response(405, "method_not_allowed", "workspace routes accept GET only");
            response.allow = "GET";
            return response;
        }
        if (!request.body.empty()) {
            return error_response(400, "invalid_request", "workspace routes do not accept a body");
        }
        std::string body;
        Error error;
        if (request.path == "/ainiux/v1/workspace/review") {
            if (!request.query.empty()) {
                return error_response(400, "invalid_request", "workspace review does not accept query parameters");
            }
            error = status.workspace->review(body);
        } else {
            std::string path;
            error = query_path(request, request.path == "/ainiux/v1/files", path);
            if (error.ok()) {
                if (request.path == "/ainiux/v1/dired") {
                    error = status.workspace->list(path, body);
                } else {
                    error = status.workspace->read(path, body);
                }
            }
        }
        if (!error.ok()) return workspace_error(error);
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
