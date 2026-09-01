#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include "common.hpp"
#include "cli/args.hpp"
#include "server/auth.hpp"
#include "server/limits.hpp"

namespace ainiux::server {

struct ListenerConfig {
    unsigned short port = 8766;
    std::size_t max_connections = Limits::default_max_connections;
    std::size_t max_jobs = Limits::default_max_jobs;
    std::size_t max_sessions = Limits::default_max_sessions;
    AuthConfig auth;
    cli::Options base_options;
    std::string workspace = ".";
};

class Listener {
   public:
    Listener();
    ~Listener();
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    Error start(ListenerConfig config);
    Error serve_until(const std::function<bool()>& should_stop);
    void stop();
    unsigned short port() const;
    std::size_t active_connections() const;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ainiux::server
