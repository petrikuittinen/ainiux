#include "runtime/runtime.hpp"

namespace ainiux::runtime {

CancellationToken::CancellationToken() : flag_(std::make_shared<std::atomic<bool>>(false)) {}

CancellationToken::CancellationToken(std::shared_ptr<std::atomic<bool>> flag) : flag_(std::move(flag)) {}

bool CancellationToken::cancelled() const {
    return flag_ != nullptr && flag_->load(std::memory_order_acquire);
}

CancellationSource::CancellationSource() : flag_(std::make_shared<std::atomic<bool>>(false)) {}

CancellationToken CancellationSource::token() const {
    return CancellationToken(flag_);
}

void CancellationSource::cancel() const {
    flag_->store(true, std::memory_order_release);
}

JobHandle::~JobHandle() {
    cancel();
    join();
}

void JobHandle::start(Job job) {
    cancel();
    join();
    source_ = CancellationSource();
    running_.store(true, std::memory_order_release);
    CancellationToken token = source_.token();
    thread_ = std::thread([this, token, job = std::move(job)]() mutable {
        try {
            job(token);
        } catch (...) {
        }
        running_.store(false, std::memory_order_release);
    });
}

void JobHandle::cancel() {
    source_.cancel();
}

void JobHandle::join() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool JobHandle::running() const {
    return running_.load(std::memory_order_acquire);
}

bool JobHandle::joinable() const {
    return thread_.joinable();
}

BackgroundJobs::~BackgroundJobs() {
    shutdown();
}

std::uint64_t BackgroundJobs::start(Job job) {
    if (current_) {
        current_->cancel();
        retired_.push_back(std::move(current_));
    }
    current_ = std::make_shared<JobHandle>();
    ++generation_;
    current_->start(std::move(job));
    return generation_;
}

void BackgroundJobs::reap_finished() {
    auto reap = [](std::vector<std::shared_ptr<JobHandle>>& jobs) {
        auto it = jobs.begin();
        while (it != jobs.end()) {
            if (!(*it)->running()) {
                (*it)->join();
                it = jobs.erase(it);
            } else {
                ++it;
            }
        }
    };
    reap(retired_);
    if (current_ && !current_->running()) {
        current_->join();
        current_.reset();
    }
}

void BackgroundJobs::cancel_all() {
    if (current_) current_->cancel();
    for (const auto& job : retired_) job->cancel();
}

void BackgroundJobs::shutdown() {
    cancel_all();
    if (current_) {
        current_->join();
        current_.reset();
    }
    for (const auto& job : retired_) job->join();
    retired_.clear();
}

std::size_t BackgroundJobs::job_count() const {
    return retired_.size() + (current_ ? 1U : 0U);
}

}  // namespace ainiux::runtime
