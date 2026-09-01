#include "server/event_broker.hpp"

#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

namespace ainiux::server {

std::string server_timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

EventBroker::EventBroker(std::string job_id, std::size_t max_events, std::size_t max_bytes)
    : job_id_(std::move(job_id)),
      max_events_(max_events == 0 ? 1U : max_events),
      max_bytes_(max_bytes == 0 ? 1U : max_bytes) {}

wire::Event EventBroker::publish(std::string type,
                                 std::string data_json,
                                 std::string session_id,
                                 std::string turn_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    wire::Event event;
    event.id = next_id_++;
    event.type = std::move(type);
    event.created_at = server_timestamp();
    event.job_id = job_id_;
    event.session_id = std::move(session_id);
    event.turn_id = std::move(turn_id);
    event.data_json = std::move(data_json);
    const std::size_t bytes = wire::event_json(event).size();
    if (bytes <= max_bytes_) {
        while (!events_.empty() &&
               (events_.size() >= max_events_ || retained_bytes_ + bytes > max_bytes_)) {
            retained_bytes_ -= wire::event_json(events_.front()).size();
            events_.pop_front();
        }
        events_.push_back(event);
        retained_bytes_ += bytes;
    } else {
        events_.clear();
        retained_bytes_ = 0;
    }
    cv_.notify_all();
    return event;
}

ReplayBatch EventBroker::replay_locked(std::uint64_t event_id) const {
    ReplayBatch out;
    out.closed = closed_;
    if (!events_.empty() && event_id != 0 && event_id < events_.front().id &&
        events_.front().id - event_id > 1U) {
        out.expired = true;
        return out;
    }
    for (const wire::Event& event : events_) {
        if (event.id > event_id) out.events.push_back(event);
    }
    return out;
}

ReplayBatch EventBroker::replay_after(std::uint64_t event_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return replay_locked(event_id);
}

ReplayBatch EventBroker::wait_after(std::uint64_t event_id,
                                    std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, timeout, [&] {
        return closed_ || (!events_.empty() && events_.back().id > event_id);
    });
    return replay_locked(event_id);
}

void EventBroker::close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }
    cv_.notify_all();
}

bool EventBroker::closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
}

std::string sse_record(const wire::Event& event) {
    return "id: " + std::to_string(event.id) + "\nevent: " + event.type +
           "\ndata: " + wire::event_json(event) + "\n\n";
}

}  // namespace ainiux::server
