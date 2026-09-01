#pragma once

#include <cstdint>
#include <string>

#include "app/operations.hpp"
#include "common.hpp"

namespace ainiux::server::wire {

inline constexpr char kApiVersion[] = "ainiux/v1";

enum class JobState { Queued, Running, Succeeded, Failed, Cancelled };

struct ErrorEnvelope {
    int http_status = 500;
    std::string code = "internal";
    std::string message;
    std::string details_json = "{}";
    std::string request_id;
};

struct Event {
    std::uint64_t id = 0;
    std::string type;
    std::string created_at;
    std::string job_id;
    std::string session_id;
    std::string turn_id;
    std::string data_json = "{}";
};

const char* job_state_name(JobState state);
ErrorEnvelope error_envelope(const Error& error,
                             std::string request_id,
                             std::string details_json = "{}");
std::string error_json(const ErrorEnvelope& envelope);
std::string event_json(const Event& event);
Event operation_event(const app::operation::Event& event,
                      std::uint64_t id,
                      std::string created_at,
                      std::string job_id,
                      std::string session_id = {},
                      std::string turn_id = {});

}  // namespace ainiux::server::wire
