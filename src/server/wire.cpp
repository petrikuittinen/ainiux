#include "server/wire.hpp"

#include <utility>

#include "json/json.hpp"

namespace ainiux::server::wire {
namespace {

std::string valid_object_or_empty(const std::string& value) {
    const json::ParseResult parsed = json::parse(value);
    if (!parsed.error.ok() || !parsed.value.is_object()) return "{}";
    return value;
}

}  // namespace

const char* job_state_name(JobState state) {
    switch (state) {
        case JobState::Queued: return "queued";
        case JobState::Running: return "running";
        case JobState::Succeeded: return "succeeded";
        case JobState::Failed: return "failed";
        case JobState::Cancelled: return "cancelled";
    }
    return "failed";
}

ErrorEnvelope error_envelope(const Error& error,
                             std::string request_id,
                             std::string details_json) {
    ErrorEnvelope out;
    out.message = error.message;
    out.request_id = std::move(request_id);
    out.details_json = valid_object_or_empty(details_json);
    switch (error.code) {
        case ErrorCode::Ok:
            out.http_status = 200;
            out.code = "ok";
            break;
        case ErrorCode::BadArgs:
            out.http_status = 400;
            out.code = "invalid_request";
            break;
        case ErrorCode::BadUrl:
            out.http_status = 400;
            out.code = "invalid_url";
            break;
        case ErrorCode::Auth:
            out.http_status = 401;
            out.code = "authentication_failed";
            break;
        case ErrorCode::FileLock:
            out.http_status = 409;
            out.code = "conflict";
            break;
        case ErrorCode::UnsupportedFeature:
            out.http_status = 422;
            out.code = "unsupported_feature";
            break;
        case ErrorCode::RateLimit:
            out.http_status = 429;
            out.code = "rate_limited";
            break;
        case ErrorCode::Cancelled:
            out.http_status = 409;
            out.code = "cancelled";
            break;
        case ErrorCode::Dns:
        case ErrorCode::Connect:
        case ErrorCode::Tls:
        case ErrorCode::HttpStatus:
            out.http_status = 502;
            out.code = "upstream_failure";
            break;
        case ErrorCode::Timeout:
            out.http_status = 504;
            out.code = "timeout";
            break;
        case ErrorCode::JsonParse:
            out.http_status = 400;
            out.code = "invalid_json";
            break;
        case ErrorCode::ProviderSchema:
            out.http_status = 502;
            out.code = "upstream_schema";
            break;
        case ErrorCode::SseParse:
            out.http_status = 502;
            out.code = "upstream_stream";
            break;
        case ErrorCode::FileRead:
            out.http_status = 404;
            out.code = "not_found";
            break;
        case ErrorCode::FileWrite:
            out.http_status = 500;
            out.code = "write_failed";
            break;
        case ErrorCode::Config:
            out.http_status = 500;
            out.code = "configuration_error";
            break;
        case ErrorCode::StreamComplete:
        case ErrorCode::Internal:
            out.http_status = 500;
            out.code = "internal";
            break;
    }
    return out;
}

std::string error_json(const ErrorEnvelope& envelope) {
    return "{\"error\":{\"code\":" + json::quote(envelope.code) +
           ",\"message\":" + json::quote(envelope.message) +
           ",\"details\":" + valid_object_or_empty(envelope.details_json) +
           ",\"request_id\":" + json::quote(envelope.request_id) + "}}";
}

std::string event_json(const Event& event) {
    return "{\"id\":" + std::to_string(event.id) +
           ",\"type\":" + json::quote(event.type) +
           ",\"created_at\":" + json::quote(event.created_at) +
           ",\"job_id\":" + json::quote(event.job_id) +
           ",\"session_id\":" +
           (event.session_id.empty() ? std::string("null") : json::quote(event.session_id)) +
           ",\"turn_id\":" +
           (event.turn_id.empty() ? std::string("null") : json::quote(event.turn_id)) +
           ",\"data\":" + valid_object_or_empty(event.data_json) + "}";
}

Event operation_event(const app::operation::Event& source,
                      std::uint64_t id,
                      std::string created_at,
                      std::string job_id,
                      std::string session_id,
                      std::string turn_id) {
    Event event;
    event.id = id;
    event.created_at = std::move(created_at);
    event.job_id = std::move(job_id);
    event.session_id = std::move(session_id);
    event.turn_id = std::move(turn_id);
    switch (source.type) {
        case app::operation::EventType::Started: event.type = "started"; break;
        case app::operation::EventType::Delta: event.type = "delta"; break;
        case app::operation::EventType::Progress: event.type = "progress"; break;
        case app::operation::EventType::Completed: event.type = "completed"; break;
    }
    event.data_json = "{\"text\":" + json::quote(source.text) +
                      ",\"current\":" + std::to_string(source.current) +
                      ",\"total\":" + std::to_string(source.total) + "}";
    return event;
}

}  // namespace ainiux::server::wire
