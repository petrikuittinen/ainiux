#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace ainiux::runtime {

class CancellationToken {
   public:
    CancellationToken();
    explicit CancellationToken(std::shared_ptr<std::atomic<bool>> flag);

    bool cancelled() const;

   private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

class CancellationSource {
   public:
    CancellationSource();

    CancellationToken token() const;
    void cancel() const;

   private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

template <typename Event>
class EventQueue {
   public:
    EventQueue() = default;
    EventQueue(const EventQueue&) = delete;
    EventQueue& operator=(const EventQueue&) = delete;

    void push(Event event) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) {
                return;
            }
            events_.push_back(std::move(event));
        }
        cv_.notify_one();
    }

    bool try_pop(Event& event) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (events_.empty()) {
            return false;
        }
        event = std::move(events_.front());
        events_.pop_front();
        return true;
    }

    bool wait_pop_for(Event& event, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, timeout, [&] { return closed_ || !events_.empty(); });
        if (events_.empty()) {
            return false;
        }
        event = std::move(events_.front());
        events_.pop_front();
        return true;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();
    }

   private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Event> events_;
    bool closed_ = false;
};

class JobHandle {
   public:
    using Job = std::function<void(CancellationToken)>;

    JobHandle() = default;
    ~JobHandle();
    JobHandle(const JobHandle&) = delete;
    JobHandle& operator=(const JobHandle&) = delete;

    void start(Job job);
    void cancel();
    void join();
    bool running() const;
    bool joinable() const;

   private:
    CancellationSource source_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

// Owns a latest-wins background job without ever joining superseded work from
// start(). Call reap_finished() from the owning event loop and shutdown() before
// destroying resources captured by jobs.
class BackgroundJobs {
   public:
    using Job = JobHandle::Job;

    BackgroundJobs() = default;
    ~BackgroundJobs();
    BackgroundJobs(const BackgroundJobs&) = delete;
    BackgroundJobs& operator=(const BackgroundJobs&) = delete;

    std::uint64_t start(Job job);
    void reap_finished();
    void cancel_all();
    void shutdown();
    std::uint64_t generation() const { return generation_; }
    std::size_t job_count() const;

   private:
    std::uint64_t generation_ = 0;
    std::shared_ptr<JobHandle> current_;
    std::vector<std::shared_ptr<JobHandle>> retired_;
};

}  // namespace ainiux::runtime
