#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "server/limits.hpp"
#include "server/wire.hpp"

namespace ainiux::server {

struct ReplayBatch {
    std::vector<wire::Event> events;
    bool expired = false;
    bool closed = false;
};

// A producer-never-blocks, bounded replay log. Slow subscribers read by event
// ID and receive an explicit expiry signal if retention overtakes them.
class EventBroker {
   public:
    explicit EventBroker(std::string job_id,
                         std::size_t max_events = Limits::events_per_job,
                         std::size_t max_bytes = Limits::event_bytes_per_job);
    EventBroker(const EventBroker&) = delete;
    EventBroker& operator=(const EventBroker&) = delete;

    wire::Event publish(std::string type,
                        std::string data_json = "{}",
                        std::string session_id = {},
                        std::string turn_id = {});
    ReplayBatch replay_after(std::uint64_t event_id) const;
    ReplayBatch wait_after(std::uint64_t event_id, std::chrono::milliseconds timeout) const;
    void close();
    bool closed() const;

   private:
    ReplayBatch replay_locked(std::uint64_t event_id) const;

    std::string job_id_;
    std::size_t max_events_;
    std::size_t max_bytes_;
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    std::deque<wire::Event> events_;
    std::size_t retained_bytes_ = 0;
    std::uint64_t next_id_ = 1;
    bool closed_ = false;
};

std::string server_timestamp();
std::string sse_record(const wire::Event& event);

}  // namespace ainiux::server
