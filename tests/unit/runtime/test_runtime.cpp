#include "runtime/test_runtime.hpp"
#include "support/test_support.hpp"
#include "runtime/runtime.hpp"
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace ainiux::test::runtime {

namespace {

using ainiux::test::check;
using ainiux::test::read_fixture;

void test_runtime_event_queue_and_job_cancel() {
    ainiux::runtime::EventQueue<int> queue;
    int value = 0;
    check(!queue.try_pop(value), "empty runtime queue has no event");
    queue.push(7);
    check(queue.try_pop(value) && value == 7, "runtime queue preserves event value");

    ainiux::runtime::JobHandle job;
    std::atomic<bool> entered{false};
    job.start([&](ainiux::runtime::CancellationToken token) {
        entered.store(true, std::memory_order_release);
        while (!token.cancelled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        queue.push(42);
    });
    for (int i = 0; i < 100 && !entered.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(job.running() && job.joinable(), "runtime job reports running and joinable");
    job.cancel();
    check(queue.wait_pop_for(value, std::chrono::milliseconds(1000)) && value == 42, "runtime job observes cancellation");
    job.join();
    check(!job.running() && !job.joinable(), "runtime job reports stopped after join");

    job.start([](ainiux::runtime::CancellationToken) {});
    for (int i = 0; i < 100 && job.running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(!job.running() && job.joinable(),
          "completed runtime job stays joinable until its event owner joins it");
    job.join();
}

void test_runtime_queue_timeout() {
    ainiux::runtime::EventQueue<int> queue;
    int value = 0;
    check(!queue.wait_pop_for(value, std::chrono::milliseconds(10)),
          "runtime queue wait_pop_for times out on an empty queue");
}

void test_background_jobs_supersede_without_blocking() {
    ainiux::runtime::BackgroundJobs jobs;
    std::atomic<bool> stale_started{false};
    std::atomic<bool> release_stale{false};
    std::atomic<bool> newest_finished{false};
    check(jobs.start([&](ainiux::runtime::CancellationToken) {
              stale_started.store(true, std::memory_order_release);
              while (!release_stale.load(std::memory_order_acquire))
                  std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }) == 1,
          "first background job receives generation one");
    for (int i = 0; i < 100 && !stale_started.load(std::memory_order_acquire); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    const auto before = std::chrono::steady_clock::now();
    const std::uint64_t generation =
        jobs.start([&](ainiux::runtime::CancellationToken) {
            newest_finished.store(true, std::memory_order_release);
        });
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - before);
    check(generation == 2 && elapsed.count() < 100,
          "superseding background work never joins a stalled worker on the caller");
    for (int i = 0; i < 100 && !newest_finished.load(std::memory_order_acquire); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    jobs.reap_finished();
    check(newest_finished.load(std::memory_order_acquire) && jobs.job_count() == 1,
          "latest background job can finish while stale work remains retired");
    release_stale.store(true, std::memory_order_release);
    jobs.shutdown();
    check(jobs.job_count() == 0, "background shutdown joins every generation");
}

}  // namespace

void run_all() {
    test_runtime_event_queue_and_job_cancel();
    test_runtime_queue_timeout();
    test_background_jobs_supersede_without_blocking();
}

}  // namespace ainiux::test::runtime
