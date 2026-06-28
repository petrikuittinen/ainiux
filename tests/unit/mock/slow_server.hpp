#pragma once

#include <cstdint>
#include <string>

namespace pkchat::test::mock {

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
    bool running() const { return pid_ > 0; }
    int port() const { return port_; }
    std::string base_url() const;

   private:
    int port_ = 0;
    int pid_ = -1;
    int ready_pipe_[2] = {-1, -1};
};

int pick_free_port();

}  // namespace pkchat::test::mock