#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "server/http_parser.hpp"

namespace ainiux::server {

class Job;
class JobService;

struct McpResponse {
    int status = 200;
    std::string content_type = "application/json; charset=utf-8";
    std::string body;
    bool empty = false;
    std::string allow;
};

// Stateless MCP 2026-07-28 endpoint. The adapter owns only opaque task
// aliases; execution, retention, cancellation, and provider limits stay in
// JobService/JobRegistry.
class McpAdapter {
   public:
    McpAdapter(JobService* jobs, std::size_t max_tasks);
    ~McpAdapter();
    McpAdapter(const McpAdapter&) = delete;
    McpAdapter& operator=(const McpAdapter&) = delete;

    McpResponse handle(const http::Request& request);

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ainiux::server
