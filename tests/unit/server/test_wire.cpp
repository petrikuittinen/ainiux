#include "server/test_wire.hpp"

#include <string>

#include "json/json.hpp"
#include "server/wire.hpp"
#include "server/limits.hpp"
#include "server/metrics.hpp"
#include "support/test_support.hpp"

namespace ainiux::test::server_wire {
namespace {

using ainiux::test::check;

void test_job_state_names() {
    check(std::string(server::wire::job_state_name(server::wire::JobState::Queued)) == "queued",
          "wire queued state is stable");
    check(std::string(server::wire::job_state_name(server::wire::JobState::Cancelled)) ==
              "cancelled",
          "wire cancelled state is stable");
}

void test_error_mapping_and_json() {
    const server::wire::ErrorEnvelope bad = server::wire::error_envelope(
        {ErrorCode::BadArgs, "model is required"}, "req-1", "{\"field\":\"model\"}");
    check(bad.http_status == 400 && bad.code == "invalid_request",
          "BadArgs maps to stable 400 invalid_request");
    const json::ParseResult parsed = json::parse(server::wire::error_json(bad));
    check(parsed.error.ok() && parsed.value.is_object(), "wire error envelope is valid JSON");
    const json::Value* error = parsed.value.get("error");
    check(error != nullptr && error->is_object(), "wire error envelope has error object");
    check(error != nullptr && error->get("request_id") != nullptr &&
              error->get("request_id")->string == "req-1",
          "wire error includes request id");

    const server::wire::ErrorEnvelope conflict =
        server::wire::error_envelope({ErrorCode::FileLock, "active"}, "req-2");
    check(conflict.http_status == 409 && conflict.code == "conflict",
          "FileLock maps to conflict without exposing internal enum");
    const server::wire::ErrorEnvelope upstream =
        server::wire::error_envelope({ErrorCode::Connect, "failed"}, "req-3");
    check(upstream.http_status == 502 && upstream.code == "upstream_failure",
          "network errors map to upstream failure");

    const ErrorCode codes[] = {
        ErrorCode::Ok,          ErrorCode::BadArgs,       ErrorCode::BadUrl,
        ErrorCode::Dns,         ErrorCode::Connect,       ErrorCode::Tls,
        ErrorCode::Timeout,     ErrorCode::HttpStatus,    ErrorCode::Auth,
        ErrorCode::RateLimit,   ErrorCode::JsonParse,     ErrorCode::SseParse,
        ErrorCode::ProviderSchema, ErrorCode::UnsupportedFeature,
        ErrorCode::FileRead,    ErrorCode::FileWrite,     ErrorCode::FileLock,
        ErrorCode::Config,      ErrorCode::Cancelled,     ErrorCode::StreamComplete,
        ErrorCode::Internal,
    };
    for (const ErrorCode code : codes) {
        const server::wire::ErrorEnvelope mapped =
            server::wire::error_envelope({code, "message"}, "request");
        check(mapped.http_status >= 200 && mapped.http_status <= 599 && !mapped.code.empty(),
              std::string("wire mapping is complete for ") + error_code_name(code));
        check(json::parse(server::wire::error_json(mapped)).error.ok(),
              std::string("wire mapping serializes for ") + error_code_name(code));
    }
}

void test_invalid_details_are_not_injected() {
    server::wire::ErrorEnvelope envelope =
        server::wire::error_envelope({ErrorCode::Internal, "bad"}, "req", "not-json");
    const std::string encoded = server::wire::error_json(envelope);
    check(encoded.find("\"details\":{}") != std::string::npos,
          "invalid error details are replaced with an empty object");
    check(json::parse(encoded).error.ok(), "sanitized error envelope remains valid JSON");
}

void test_operation_event_conversion() {
    app::operation::Event source;
    source.type = app::operation::EventType::Delta;
    source.text = "hello";
    source.current = 2;
    source.total = 5;
    const server::wire::Event event =
        server::wire::operation_event(source, 7, "2026-09-01T00:00:00Z", "job-1");
    check(event.type == "delta" && event.id == 7 && event.job_id == "job-1",
          "operation event converts explicitly to stable wire fields");
    const std::string encoded = server::wire::event_json(event);
    const json::ParseResult parsed = json::parse(encoded);
    check(parsed.error.ok(), "wire event is valid JSON");
    check(encoded.find("\"session_id\":null") != std::string::npos &&
              encoded.find("\"turn_id\":null") != std::string::npos,
          "absent session and turn ids serialize as null");
}

void test_initial_server_limits_are_bounded() {
    check(server::Limits::request_line_bytes == 8U * 1024U &&
              server::Limits::header_bytes == 32U * 1024U &&
              server::Limits::json_body_bytes == 1U * 1024U * 1024U,
          "initial parser limits are explicit and bounded");
    check(server::Limits::default_provider_concurrency == 4U &&
              server::Limits::agent_lanes == 1U &&
              server::Limits::events_per_job == 256U &&
              server::Limits::event_bytes_per_job == 1024U * 1024U,
          "initial operation concurrency is explicit");
}

void test_generation_metrics_are_normalized() {
    provider::RequestContext context;
    context.options.context_tokens = 8192;
    context.options.stream = true;
    std::vector<provider::Message> messages{{"user", "Please summarize this text."}};
    provider::ChatResult chat;
    chat.completion_tokens = 12;
    chat.completion_tokens_estimated = true;
    chat.total_ms = 2400;
    chat.ttft_ms = 320;
    const server::GenerationMetrics normalized =
        server::chat_generation_metrics(context, messages, chat);
    const std::string encoded = server::generation_metrics_json(normalized);
    const json::ParseResult parsed = json::parse(encoded);
    check(parsed.error.ok() && parsed.value.is_object() &&
              parsed.value.get("context_used_tokens") != nullptr &&
              parsed.value.get("context_window_tokens") != nullptr &&
              parsed.value.get("input_tokens_estimated") != nullptr &&
              parsed.value.get("input_tokens_estimated")->boolean &&
              parsed.value.get("output_tokens_estimated") != nullptr &&
              parsed.value.get("output_tokens_estimated")->boolean,
          "chat metrics expose context, timing, and explicit estimation markers");

    agent::AgentTokenUsage usage;
    usage.input_tokens = 50;
    usage.output_tokens = 10;
    usage.cache_read_tokens = 20;
    const std::string agent_encoded = server::generation_metrics_json(
        server::agent_generation_metrics(300, 4096, usage, 750, 20.0));
    check(agent_encoded.find("\"total_tokens\":60") != std::string::npos &&
              agent_encoded.find("\"elapsed_ms\":750") != std::string::npos &&
              agent_encoded.find("\"output_tokens_per_second\":20") != std::string::npos,
          "agent metrics retain token totals, elapsed milliseconds, and decode rate");
}

}  // namespace

void run_all() {
    test_job_state_names();
    test_error_mapping_and_json();
    test_invalid_details_are_not_injected();
    test_operation_event_conversion();
    test_initial_server_limits_are_bounded();
    test_generation_metrics_are_normalized();
}

}  // namespace ainiux::test::server_wire
