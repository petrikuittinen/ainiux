#pragma once

#include <memory>
#include <string>
#include <vector>

#include "common.hpp"
#include "mcp/protocol.hpp"
#include "mcp/registry.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::mcp {

struct StdioTransportOptions {
    long startup_timeout_ms = 30000;
    long call_timeout_ms = 120000;
    runtime::CancellationToken cancellation;
};

// Long-lived MCP stdio child process (newline-delimited JSON-RPC).
class StdioSession {
   public:
    StdioSession();
    ~StdioSession();
    StdioSession(const StdioSession&) = delete;
    StdioSession& operator=(const StdioSession&) = delete;
    StdioSession(StdioSession&&) noexcept;
    StdioSession& operator=(StdioSession&&) noexcept;

    Error start(const ServerConfig& config, const StdioTransportOptions& options);
    void close();
    bool running() const;
    Dialect dialect() const { return dialect_; }
    void set_dialect(Dialect d) { dialect_ = d; }

    Error request(const std::string& request_json,
                  long timeout_ms,
                  runtime::CancellationToken cancellation,
                  JsonRpcResponse& response);
    Error notify(const std::string& notification_json,
                 long timeout_ms,
                 runtime::CancellationToken cancellation);

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Dialect dialect_ = Dialect::Unknown;
};

Error stdio_connect_negotiate(const ServerConfig& config,
                              const StdioTransportOptions& options,
                              StdioSession& session);

}  // namespace ainiux::mcp
