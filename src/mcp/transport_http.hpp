#pragma once

#include <string>
#include <vector>

#include "common.hpp"
#include "mcp/protocol.hpp"
#include "mcp/registry.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::mcp {

struct HttpTransportOptions {
    long connect_timeout_seconds = 30;
    long timeout_seconds = 120;
    bool block_private_addresses = true;
    bool insecure_tls = false;
    bool trace = false;
    std::vector<std::string> secrets_to_redact;
    runtime::CancellationToken cancellation;
};

struct HttpSession {
    std::string endpoint_url;
    std::string session_id;
    Dialect dialect = Dialect::Unknown;
    std::vector<std::string> extra_headers;
};

// Single JSON-RPC POST. Sets Accept for json + event-stream. Parses JSON body
// or first JSON-RPC payload from an SSE stream.
Error http_post_jsonrpc(const HttpSession& session,
                        const std::string& method,
                        const std::string& mcp_name,
                        const std::string& body_json,
                        const HttpTransportOptions& options,
                        JsonRpcResponse& response,
                        std::string* response_session_id = nullptr);

Error http_connect_negotiate(const ServerConfig& config,
                             const HttpTransportOptions& options,
                             HttpSession& session);

}  // namespace ainiux::mcp
