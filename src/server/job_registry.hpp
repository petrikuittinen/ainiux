#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "common.hpp"
#include "runtime/runtime.hpp"
#include "server/event_broker.hpp"
#include "server/wire.hpp"

namespace ainiux::server {

enum class JobClass { Provider, Agent };

struct JobOutcome {
    Error error;
    std::string result_json = "{}";
};

using JobEvents = std::function<Error(const app::operation::Event&)>;
using JobWork = std::function<JobOutcome(runtime::CancellationToken, JobEvents)>;

class Job {
   public:
    const std::string id;
    const std::string operation;
    const JobClass job_class;
    EventBroker events;

    wire::JobState state() const;
    std::string snapshot_json() const;
    bool terminal() const;
    void cancel();

   private:
    friend class JobRegistry;
    Job(std::string job_id, std::string operation_name, JobClass kind);

    mutable std::mutex mutex_;
    wire::JobState state_ = wire::JobState::Queued;
    std::string created_at_;
    std::string updated_at_;
    std::string result_json_ = "null";
    Error error_;
    runtime::CancellationSource cancellation_;
};

enum class SubmitStatus {
    Created,
    Existing,
    IdempotencyConflict,
    AgentConflict,
    Capacity,
    Stopping,
    InternalFailure
};

struct SubmitResult {
    SubmitStatus status = SubmitStatus::Stopping;
    std::shared_ptr<Job> job;
    std::string conflicting_job_id;
};

class JobRegistry {
   public:
    explicit JobRegistry(std::size_t max_jobs,
                         std::size_t provider_concurrency = Limits::default_provider_concurrency);
    ~JobRegistry();
    JobRegistry(const JobRegistry&) = delete;
    JobRegistry& operator=(const JobRegistry&) = delete;

    SubmitResult submit(std::string operation,
                        std::string canonical_payload,
                        std::string idempotency_key,
                        JobClass job_class,
                        JobWork work);
    std::shared_ptr<Job> find(const std::string& id) const;
    bool cancel(const std::string& id, std::shared_ptr<Job>& job);
    std::size_t size() const;
    std::size_t active_count() const;
    void shutdown();
    bool stopping() const;

   private:
    struct IdempotencyEntry {
        std::string operation;
        std::string payload;
        std::string job_id;
    };

    void run_job(const std::shared_ptr<Job>& job, JobWork work);
    bool acquire_provider(const runtime::CancellationToken& cancellation);
    void release_provider();
    void evict_terminal_locked();

    const std::size_t max_jobs_;
    const std::size_t provider_concurrency_;
    mutable std::mutex mutex_;
    std::condition_variable provider_cv_;
    std::map<std::string, std::shared_ptr<Job>> jobs_;
    std::deque<std::string> retention_order_;
    std::map<std::string, IdempotencyEntry> idempotency_;
    std::vector<std::shared_ptr<runtime::JobHandle>> workers_;
    std::size_t next_job_id_ = 1;
    std::size_t provider_active_ = 0;
    std::string active_agent_job_id_;
    bool stopping_ = false;
};

}  // namespace ainiux::server
