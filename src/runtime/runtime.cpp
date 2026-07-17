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

}  // namespace ainiux::runtime
