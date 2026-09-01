#include "server/job_registry.hpp"

#include <chrono>
#include <utility>
#include <vector>

#include "json/json.hpp"
#include "server/wire.hpp"

namespace ainiux::server {
namespace {

bool terminal_state(wire::JobState state) {
    return state == wire::JobState::Succeeded || state == wire::JobState::Failed ||
           state == wire::JobState::Cancelled;
}

std::string public_error_json(const Error& error) {
    const wire::ErrorEnvelope envelope = wire::error_envelope(error, "");
    return "{\"code\":" + json::quote(envelope.code) +
           ",\"message\":" + json::quote(envelope.message) +
           ",\"details\":" + envelope.details_json + "}";
}

}  // namespace

Job::Job(std::string job_id, std::string operation_name, JobClass kind)
    : id(std::move(job_id)),
      operation(std::move(operation_name)),
      job_class(kind),
      events(id),
      created_at_(server_timestamp()),
      updated_at_(created_at_) {}

wire::JobState Job::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

bool Job::terminal() const { return terminal_state(state()); }

void Job::cancel() {
    cancellation_.cancel();
}

std::string Job::snapshot_json() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return "{\"id\":" + json::quote(id) +
           ",\"operation\":" + json::quote(operation) +
           ",\"state\":" + json::quote(wire::job_state_name(state_)) +
           ",\"created_at\":" + json::quote(created_at_) +
           ",\"updated_at\":" + json::quote(updated_at_) +
           ",\"result\":" + result_json_ +
           ",\"error\":" + (error_.ok() ? std::string("null") : public_error_json(error_)) + "}";
}

JobRegistry::JobRegistry(std::size_t max_jobs, std::size_t provider_concurrency)
    : max_jobs_(max_jobs == 0 ? 1U : max_jobs),
      provider_concurrency_(provider_concurrency == 0 ? 1U : provider_concurrency) {}

JobRegistry::~JobRegistry() { shutdown(); }

void JobRegistry::evict_terminal_locked() {
    while (jobs_.size() >= max_jobs_) {
        auto oldest_order = retention_order_.end();
        for (auto it = retention_order_.begin(); it != retention_order_.end(); ++it) {
            const auto found = jobs_.find(*it);
            if (found != jobs_.end() && found->second->terminal()) {
                oldest_order = it;
                break;
            }
        }
        if (oldest_order == retention_order_.end()) return;
        const std::string removed_id = *oldest_order;
        retention_order_.erase(oldest_order);
        jobs_.erase(removed_id);
        for (auto it = idempotency_.begin(); it != idempotency_.end();) {
            if (it->second.job_id == removed_id) it = idempotency_.erase(it);
            else ++it;
        }
    }
}

SubmitResult JobRegistry::submit(std::string operation,
                                 std::string canonical_payload,
                                 std::string idempotency_key,
                                 JobClass job_class,
                                 JobWork work) {
    std::shared_ptr<Job> job;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return {SubmitStatus::Stopping, {}, {}};
        for (auto it = workers_.begin(); it != workers_.end();) {
            if (!(*it)->running()) {
                (*it)->join();
                it = workers_.erase(it);
            } else {
                ++it;
            }
        }
        if (!idempotency_key.empty()) {
            const auto existing = idempotency_.find(idempotency_key);
            if (existing != idempotency_.end()) {
                const auto found = jobs_.find(existing->second.job_id);
                if (existing->second.operation == operation &&
                    existing->second.payload == canonical_payload && found != jobs_.end()) {
                    return {SubmitStatus::Existing, found->second, {}};
                }
                return {SubmitStatus::IdempotencyConflict,
                        found == jobs_.end() ? std::shared_ptr<Job>{} : found->second,
                        existing->second.job_id};
            }
        }
        if (job_class == JobClass::Agent && !active_agent_job_id_.empty()) {
            return {SubmitStatus::AgentConflict, {}, active_agent_job_id_};
        }
        evict_terminal_locked();
        if (jobs_.size() >= max_jobs_) return {SubmitStatus::Capacity, {}, {}};
        const std::string id = "job_" + std::to_string(next_job_id_++);
        job = std::shared_ptr<Job>(new Job(id, operation, job_class));
        jobs_[id] = job;
        retention_order_.push_back(id);
        if (!idempotency_key.empty()) {
            idempotency_[idempotency_key] = {operation, canonical_payload, id};
        }
        if (job_class == JobClass::Agent) active_agent_job_id_ = id;
        job->events.publish("queued", "{\"state\":\"queued\"}");
        auto worker = std::make_shared<runtime::JobHandle>();
        workers_.push_back(worker);
        try {
            worker->start([this, job, work = std::move(work)](runtime::CancellationToken) mutable {
                run_job(job, std::move(work));
            });
        } catch (...) {
            workers_.pop_back();
            jobs_.erase(id);
            retention_order_.pop_back();
            if (!idempotency_key.empty()) idempotency_.erase(idempotency_key);
            if (active_agent_job_id_ == id) active_agent_job_id_.clear();
            return {SubmitStatus::InternalFailure, {}, {}};
        }
    }
    return {SubmitStatus::Created, std::move(job), {}};
}

bool JobRegistry::acquire_provider(const runtime::CancellationToken& cancellation) {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_ && !cancellation.cancelled() &&
           provider_active_ >= provider_concurrency_) {
        provider_cv_.wait_for(lock, std::chrono::milliseconds(50));
    }
    if (stopping_ || cancellation.cancelled()) return false;
    ++provider_active_;
    return true;
}

void JobRegistry::release_provider() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (provider_active_ > 0) --provider_active_;
    }
    provider_cv_.notify_all();
}

void JobRegistry::run_job(const std::shared_ptr<Job>& job, JobWork work) {
    const runtime::CancellationToken cancellation = job->cancellation_.token();
    bool provider_slot = false;
    if (job->job_class == JobClass::Provider) {
        provider_slot = acquire_provider(cancellation);
    }
    JobOutcome outcome;
    if (cancellation.cancelled() || (job->job_class == JobClass::Provider && !provider_slot)) {
        outcome.error = {ErrorCode::Cancelled, "job cancelled"};
    } else {
        {
            std::lock_guard<std::mutex> lock(job->mutex_);
            job->state_ = wire::JobState::Running;
            job->updated_at_ = server_timestamp();
        }
        job->events.publish("running", "{\"state\":\"running\"}");
        JobEvents sink = [job](const app::operation::Event& event) {
            wire::Event encoded = wire::operation_event(event, 0, {}, job->id);
            job->events.publish(std::move(encoded.type), std::move(encoded.data_json));
            return ok_error();
        };
        try {
            outcome = work(cancellation, std::move(sink));
        } catch (const std::exception& exception) {
            outcome.error = {ErrorCode::Internal,
                             std::string("job execution failed: ") + exception.what()};
        } catch (...) {
            outcome.error = {ErrorCode::Internal, "job execution failed unexpectedly"};
        }
    }
    if (provider_slot) release_provider();
    const bool cancelled = cancellation.cancelled() || outcome.error.code == ErrorCode::Cancelled;
    {
        std::lock_guard<std::mutex> lock(job->mutex_);
        job->updated_at_ = server_timestamp();
        if (cancelled) {
            job->state_ = wire::JobState::Cancelled;
            job->error_ = {ErrorCode::Cancelled, "job cancelled"};
        } else if (!outcome.error.ok()) {
            job->state_ = wire::JobState::Failed;
            job->error_ = outcome.error;
        } else {
            job->state_ = wire::JobState::Succeeded;
            job->result_json_ = outcome.result_json.empty() ? "{}" : std::move(outcome.result_json);
        }
    }
    job->events.publish(cancelled ? "cancelled" : outcome.error.ok() ? "succeeded" : "failed",
                        "{\"state\":" + json::quote(wire::job_state_name(job->state())) + "}");
    job->events.close();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (job->job_class == JobClass::Agent && active_agent_job_id_ == job->id) {
            active_agent_job_id_.clear();
        }
    }
    provider_cv_.notify_all();
}

std::shared_ptr<Job> JobRegistry::find(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = jobs_.find(id);
    return found == jobs_.end() ? std::shared_ptr<Job>{} : found->second;
}

bool JobRegistry::cancel(const std::string& id, std::shared_ptr<Job>& job) {
    job = find(id);
    if (!job) return false;
    if (!job->terminal()) job->cancel();
    provider_cv_.notify_all();
    return true;
}

std::size_t JobRegistry::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return jobs_.size();
}

std::size_t JobRegistry::active_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t count = 0;
    for (const auto& entry : jobs_) if (!entry.second->terminal()) ++count;
    return count;
}

bool JobRegistry::stopping() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopping_;
}

void JobRegistry::shutdown() {
    std::vector<std::shared_ptr<Job>> jobs;
    std::vector<std::shared_ptr<runtime::JobHandle>> workers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ && jobs_.empty()) return;
        stopping_ = true;
        for (const auto& entry : jobs_) jobs.push_back(entry.second);
        workers = workers_;
    }
    for (const auto& job : jobs) job->cancel();
    provider_cv_.notify_all();
    for (const auto& worker : workers) worker->join();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        workers_.clear();
    }
}

}  // namespace ainiux::server
