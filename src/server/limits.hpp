#pragma once

#include <cstddef>

namespace ainiux::server {

// PR 1 freezes the initial HTTP/concurrency contract. The listener and job
// registry added by later slices consume these defaults rather than inventing
// independent limits.
struct Limits {
    static constexpr std::size_t request_line_bytes = 8U * 1024U;
    static constexpr std::size_t header_bytes = 32U * 1024U;
    static constexpr std::size_t header_count = 100U;
    static constexpr std::size_t json_body_bytes = 1U * 1024U * 1024U;
    static constexpr std::size_t upload_body_bytes = 20U * 1024U * 1024U;
    static constexpr std::size_t requests_per_connection = 100U;
    static constexpr std::size_t default_max_connections = 64U;
    static constexpr std::size_t default_max_jobs = 128U;
    static constexpr std::size_t default_max_sessions = 32U;
    static constexpr std::size_t default_provider_concurrency = 4U;
    static constexpr std::size_t events_per_job = 256U;
    static constexpr std::size_t event_bytes_per_job = 1U * 1024U * 1024U;
    static constexpr std::size_t agent_lanes = 1U;
    static constexpr int header_timeout_seconds = 10;
    static constexpr int body_timeout_seconds = 30;
    static constexpr int idle_timeout_seconds = 60;
    static constexpr int request_timeout_seconds = 120;
    static constexpr int sse_heartbeat_seconds = 15;
};

}  // namespace ainiux::server
