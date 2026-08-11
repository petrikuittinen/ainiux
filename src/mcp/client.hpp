#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common.hpp"
#include "mcp/protocol.hpp"
#include "mcp/registry.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::mcp {

struct ConnectOptions {
    long connect_timeout_seconds = 30;
    long tool_timeout_seconds = 120;
    bool block_private_addresses = true;
    bool insecure_tls = false;
    bool trace = false;
    std::vector<std::string> secrets_to_redact;
    runtime::CancellationToken cancellation;
};

class Client {
   public:
    Client();
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&) noexcept;
    Client& operator=(Client&&) noexcept;

    Error connect(const ServerConfig& config, ConnectOptions options = {});
    void close();
    bool connected() const;
    Dialect dialect() const;
    const ServerConfig& config() const;

    // Update cancellation/timeouts for subsequent RPC without reconnecting.
    void set_call_options(ConnectOptions options);

    Error list_tools(ToolsListResult& out,
                     runtime::CancellationToken cancellation = {});
    Error call_tool(const std::string& tool_name,
                    const std::string& arguments_json,
                    ToolCallResult& out,
                    runtime::CancellationToken cancellation = {});

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Manager {
   public:
    Manager();
    ~Manager();
    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;

    void set_connect_options(ConnectOptions options);
    ConnectOptions connect_options() const;

    Error reload_from_registry(const std::string& path = {});
    // Path used by ensure_connected when persisting last_dialect.
    void set_registry_path(const std::string& path);
    const std::string& registry_path() const;
    const Registry& registry() const;

    Error ensure_connected(const std::string& server_name, Client*& client);

    Error list_all_tools(
        std::vector<std::pair<std::string, ToolsListResult>>& out);

    Error call_qualified_tool(const std::string& qualified_name,
                              const std::string& arguments_json,
                              ToolCallResult& out);

    void close_all();

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ainiux::mcp
