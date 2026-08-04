#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "runtime/runtime.hpp"

namespace ainiux::test::mock {

class SlowHttpServer {
   public:
    SlowHttpServer();
    ~SlowHttpServer();

    SlowHttpServer(const SlowHttpServer&) = delete;
    SlowHttpServer& operator=(const SlowHttpServer&) = delete;

    bool start(double response_delay_seconds = 0.0,
               double chunk_delay_seconds = 0.0,
               int chunk_count = 8);
    void stop();
    bool running() const { return running_.load(std::memory_order_acquire); }
    int port() const { return port_; }
    std::string base_url() const;

   private:
    int port_ = 0;
    std::atomic<bool> running_{false};
    runtime::CancellationSource cancellation_;
    std::thread worker_;
};

int pick_free_port();

}  // namespace ainiux::test::mock
