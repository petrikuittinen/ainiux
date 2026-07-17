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
    check(job.running(), "runtime job reports running");
    job.cancel();
    check(queue.wait_pop_for(value, std::chrono::milliseconds(1000)) && value == 42, "runtime job observes cancellation");
    job.join();
    check(!job.running(), "runtime job reports stopped after join");
}

void test_runtime_queue_timeout() {
    ainiux::runtime::EventQueue<int> queue;
    int value = 0;
    check(!queue.wait_pop_for(value, std::chrono::milliseconds(10)),
          "runtime queue wait_pop_for times out on an empty queue");
}

}  // namespace

void run_all() {
    test_runtime_event_queue_and_job_cancel();
    test_runtime_queue_timeout();
}

}  // namespace ainiux::test::runtime
