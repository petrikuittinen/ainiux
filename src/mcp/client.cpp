#include "mcp/client.hpp"

#include <atomic>
#include <map>
#include <utility>

#include "json/json.hpp"
#include "mcp/transport_http.hpp"
#include "mcp/transport_stdio.hpp"

namespace ainiux::mcp {
namespace {
std::atomic<std::int64_t> g_next_id{2};
std::int64_t next_id() { return g_next_id.fetch_add(1); }
}  // namespace

struct Client::Impl {
    ServerConfig config;
    ConnectOptions options;
    Dialect dialect = Dialect::Unknown;
    bool connected = false;
    HttpSession http;
    StdioSession stdio;
    std::int64_t next_rpc_id = 2;

    Error rpc(const std::string& method,
              const std::string& mcp_name,
              const json::Value& params,
              JsonRpcResponse& response) {
        json::Value req = build_request(next_id(), method, params);
        const std::string body = json::stringify(req);
        if (config.transport == TransportKind::Http) {
            HttpTransportOptions http_opts;
            http_opts.connect_timeout_seconds = options.connect_timeout_seconds;
            http_opts.timeout_seconds = options.tool_timeout_seconds;
            http_opts.block_private_addresses =
                options.block_private_addresses && !config.allow_private;
            http_opts.insecure_tls = options.insecure_tls;
            http_opts.trace = options.trace;
            http_opts.secrets_to_redact = options.secrets_to_redact;
            http_opts.cancellation = options.cancellation;
            std::string new_session;
            Error err = http_post_jsonrpc(http, method, mcp_name, body, http_opts, response,
                                          &new_session);
            if (!new_session.empty()) http.session_id = new_session;
            return err;
        }
        StdioTransportOptions stdio_opts;
        stdio_opts.call_timeout_ms = config.tool_timeout_ms;
        stdio_opts.cancellation = options.cancellation;
        return stdio.request(body, config.tool_timeout_ms, options.cancellation, response);
    }
};

Client::Client() : impl_(std::make_unique<Impl>()) {}
Client::~Client() { close(); }
Client::Client(Client&&) noexcept = default;
Client& Client::operator=(Client&&) noexcept = default;

Error Client::connect(const ServerConfig& config, ConnectOptions options) {
    close();
    impl_->config = config;
    impl_->options = std::move(options);
    if (config.transport == TransportKind::Http) {
        HttpTransportOptions http_opts;
        http_opts.connect_timeout_seconds =
            config.startup_timeout_ms > 0 ? (config.startup_timeout_ms + 999) / 1000
                                          : impl_->options.connect_timeout_seconds;
        http_opts.timeout_seconds = http_opts.connect_timeout_seconds;
        http_opts.block_private_addresses =
            impl_->options.block_private_addresses && !config.allow_private;
        http_opts.insecure_tls = impl_->options.insecure_tls;
        http_opts.trace = impl_->options.trace;
        http_opts.secrets_to_redact = impl_->options.secrets_to_redact;
        http_opts.cancellation = impl_->options.cancellation;
        Error err = http_connect_negotiate(config, http_opts, impl_->http);
        if (!err.ok()) return err;
        impl_->dialect = impl_->http.dialect;
        impl_->connected = true;
        return ok_error();
    }
    StdioTransportOptions stdio_opts;
    stdio_opts.startup_timeout_ms = config.startup_timeout_ms;
    stdio_opts.call_timeout_ms = config.tool_timeout_ms;
    stdio_opts.cancellation = impl_->options.cancellation;
    Error err = stdio_connect_negotiate(config, stdio_opts, impl_->stdio);
    if (!err.ok()) return err;
    impl_->dialect = impl_->stdio.dialect();
    impl_->connected = true;
    return ok_error();
}

void Client::close() {
    if (!impl_) return;
    if (impl_->config.transport == TransportKind::Stdio) impl_->stdio.close();
    impl_->http = HttpSession{};
    impl_->connected = false;
    impl_->dialect = Dialect::Unknown;
}

bool Client::connected() const { return impl_ && impl_->connected; }
Dialect Client::dialect() const { return impl_ ? impl_->dialect : Dialect::Unknown; }
const ServerConfig& Client::config() const { return impl_->config; }

void Client::set_call_options(ConnectOptions options) {
    if (!impl_) return;
    // Keep transport identity; replace call-time options (cancellation/timeouts).
    impl_->options = std::move(options);
}

Error Client::list_tools(ToolsListResult& out,
                         runtime::CancellationToken cancellation) {
    out = ToolsListResult{};
    if (!connected()) return {ErrorCode::Internal, "MCP client is not connected"};
    if (cancellation.cancelled())
        return {ErrorCode::Cancelled, "MCP tools/list cancelled"};
    impl_->options.cancellation = cancellation;
    json::Value params = make_json_object();
    if (impl_->dialect == Dialect::Stateless20260728) {
        params.object["_meta"] =
            build_stateless_meta(default_client_info(), "2026-07-28");
    }
    JsonRpcResponse response;
    Error err = impl_->rpc("tools/list", "", params, response);
    if (!err.ok()) return err;
    if (response.has_error)
        return {ErrorCode::ProviderSchema, "tools/list failed: " + response.error_message};
    if (!response.has_result)
        return {ErrorCode::ProviderSchema, "tools/list missing result"};
    return extract_tools_list(response.result, out);
}

Error Client::call_tool(const std::string& tool_name,
                        const std::string& arguments_json,
                        ToolCallResult& out,
                        runtime::CancellationToken cancellation) {
    out = ToolCallResult{};
    if (!connected()) return {ErrorCode::Internal, "MCP client is not connected"};
    if (cancellation.cancelled())
        return {ErrorCode::Cancelled, "MCP tools/call cancelled"};
    impl_->options.cancellation = cancellation;
    json::Value meta;
    const json::Value* meta_ptr = nullptr;
    if (impl_->dialect == Dialect::Stateless20260728) {
        meta = build_stateless_meta(default_client_info(), "2026-07-28");
        meta_ptr = &meta;
    }
    json::Value params = build_tools_call_params(tool_name, arguments_json, meta_ptr);
    JsonRpcResponse response;
    Error err = impl_->rpc("tools/call", tool_name, params, response);
    if (!err.ok()) return err;
    if (response.has_error)
        return {ErrorCode::ProviderSchema, "tools/call failed: " + response.error_message};
    if (!response.has_result)
        return {ErrorCode::ProviderSchema, "tools/call missing result"};
    return extract_tool_call_result(response.result, out);
}

struct Manager::Impl {
    Registry registry;
    std::string registry_path;
    ConnectOptions options;
    std::map<std::string, std::unique_ptr<Client>> clients;
};

Manager::Manager() : impl_(std::make_unique<Impl>()) {}
Manager::~Manager() { close_all(); }

void Manager::set_connect_options(ConnectOptions options) {
    impl_->options = std::move(options);
}
ConnectOptions Manager::connect_options() const { return impl_->options; }

void Manager::set_registry_path(const std::string& path) {
    impl_->registry_path = path;
}
const std::string& Manager::registry_path() const { return impl_->registry_path; }

Error Manager::reload_from_registry(const std::string& path) {
    Registry loaded;
    if (!path.empty()) impl_->registry_path = path;
    Error err = load_registry(loaded, path.empty() ? impl_->registry_path : path);
    if (!err.ok()) return err;
    // Drop clients that disappeared or were disabled.
    std::vector<std::string> drop;
    for (const auto& entry : impl_->clients) {
        auto it = loaded.servers.find(entry.first);
        if (it == loaded.servers.end() || !it->second.enabled) drop.push_back(entry.first);
    }
    for (const std::string& name : drop) {
        if (auto it = impl_->clients.find(name); it != impl_->clients.end()) {
            it->second->close();
            impl_->clients.erase(it);
        }
    }
    impl_->registry = std::move(loaded);
    return ok_error();
}

const Registry& Manager::registry() const { return impl_->registry; }

Error Manager::ensure_connected(const std::string& server_name, Client*& client) {
    client = nullptr;
    auto cfg_it = impl_->registry.servers.find(server_name);
    if (cfg_it == impl_->registry.servers.end())
        return {ErrorCode::BadArgs, "MCP server not found: " + server_name};
    if (!cfg_it->second.enabled)
        return {ErrorCode::BadArgs, "MCP server is disabled: " + server_name};

    auto client_it = impl_->clients.find(server_name);
    if (client_it != impl_->clients.end() && client_it->second->connected()) {
        client = client_it->second.get();
        return ok_error();
    }

    auto fresh = std::make_unique<Client>();
    ConnectOptions connect_opts = impl_->options;
    // Do not pin the prepare/job cancellation token onto the live client: JobHandle
    // cancels its token when the prepare job ends, which would make every later
    // MCP HTTP call fail with "HTTP request cancelled".
    connect_opts.cancellation = runtime::CancellationToken();
    Error err = fresh->connect(cfg_it->second, connect_opts);
    if (!err.ok()) return err;

    // Persist negotiated dialect.
    if (fresh->dialect() != Dialect::Unknown &&
        cfg_it->second.last_dialect != fresh->dialect()) {
        cfg_it->second.last_dialect = fresh->dialect();
        Registry to_save = impl_->registry;
        (void)save_registry(to_save, impl_->registry_path);
        impl_->registry = std::move(to_save);
    }

    client = fresh.get();
    impl_->clients[server_name] = std::move(fresh);
    return ok_error();
}

Error Manager::list_all_tools(
    std::vector<std::pair<std::string, ToolsListResult>>& out) {
    out.clear();
    // Copy names first: ensure_connected may rewrite registry (last_dialect).
    std::vector<std::string> names;
    names.reserve(impl_->registry.servers.size());
    for (const auto& entry : impl_->registry.servers) {
        if (entry.second.enabled) names.push_back(entry.first);
    }
    for (const std::string& name : names) {
        Client* client = nullptr;
        Error err = ensure_connected(name, client);
        if (!err.ok() || client == nullptr) continue;
        ToolsListResult tools;
        err = client->list_tools(tools, impl_->options.cancellation);
        if (!err.ok()) continue;
        out.push_back(std::make_pair(name, std::move(tools)));
    }
    return ok_error();
}

Error Manager::call_qualified_tool(const std::string& qualified_name,
                                   const std::string& arguments_json,
                                   ToolCallResult& out) {
    std::string server;
    std::string tool;
    if (!parse_qualified_tool_name(qualified_name, server, tool))
        return {ErrorCode::BadArgs, "not an MCP tool name: " + qualified_name};
    Client* client = nullptr;
    Error err = ensure_connected(server, client);
    if (!err.ok()) return err;
    return client->call_tool(tool, arguments_json, out, impl_->options.cancellation);
}

void Manager::close_all() {
    if (!impl_) return;
    for (auto& entry : impl_->clients) entry.second->close();
    impl_->clients.clear();
}

}  // namespace ainiux::mcp
