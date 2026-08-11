#include "app/app.hpp"

#include <iostream>
#include <sstream>

#include "cli/args.hpp"
#include "json/json.hpp"
#include "mcp/client.hpp"
#include "mcp/protocol.hpp"
#include "mcp/registry.hpp"

namespace ainiux::app {
namespace {

bool is_mcp_manage(const cli::Options& options) {
    return options.list_mcp || options.add_mcp || options.remove_mcp || options.enable_mcp ||
           options.disable_mcp;
}

Error validate_mcp_manage_exclusive(const cli::Options& options) {
    int actions = 0;
    if (options.list_mcp) ++actions;
    if (options.add_mcp) ++actions;
    if (options.remove_mcp) ++actions;
    if (options.enable_mcp) ++actions;
    if (options.disable_mcp) ++actions;
    if (actions > 1)
        return {ErrorCode::BadArgs, "use only one of --list-mcp/--add-mcp/--remove-mcp/--enable-mcp/--disable-mcp"};
    if (options.agent || options.agent_run || options.tui || options.repl || options.editor ||
        options.list_models || options.benchmark || options.grade || options.security_review ||
        options.index_code || options.print_index || options.clear_index) {
        return {ErrorCode::BadArgs, "MCP management flags cannot be combined with other modes"};
    }
    return ok_error();
}

std::string registry_path(const cli::Options& options) {
    return options.mcp_registry_path;
}

Error load(mcp::Registry& registry, const cli::Options& options) {
    return mcp::load_registry(registry, registry_path(options));
}

Error save(const mcp::Registry& registry, const cli::Options& options) {
    return mcp::save_registry(registry, registry_path(options));
}

}  // namespace

bool mcp_manage_requested(const cli::Options& options) { return is_mcp_manage(options); }

Error run_mcp_manage(const cli::Options& options, std::ostream& out, std::ostream& /*err*/) {
    Error v = validate_mcp_manage_exclusive(options);
    if (!v.ok()) return v;

    mcp::Registry registry;
    Error load_err = load(registry, options);
    if (!load_err.ok()) return load_err;

    if (options.list_mcp) {
        if (options.format == cli::OutputFormat::Json) {
            json::Value root;
            root.type = json::Value::Type::Object;
            json::Value servers;
            servers.type = json::Value::Type::Array;
            for (const auto& entry : registry.servers) {
                const mcp::ServerConfig& cfg = entry.second;
                json::Value obj;
                obj.type = json::Value::Type::Object;
                obj.object["name"] = mcp::make_json_string(cfg.name);
                obj.object["enabled"] = mcp::make_json_bool(cfg.enabled);
                obj.object["transport"] =
                    mcp::make_json_string(mcp::transport_kind_name(cfg.transport));
                if (!cfg.url.empty()) obj.object["url"] = mcp::make_json_string(cfg.url);
                if (!cfg.command.empty())
                    obj.object["command"] = mcp::make_json_string(cfg.command);
                if (cfg.last_dialect != mcp::Dialect::Unknown)
                    obj.object["last_dialect"] =
                        mcp::make_json_string(mcp::dialect_name(cfg.last_dialect));
                servers.array.push_back(std::move(obj));
            }
            root.object["servers"] = std::move(servers);
            out << json::stringify(root) << "\n";
        } else {
            if (registry.servers.empty()) {
                out << "No MCP servers installed.\n";
                out << "Install with: ainiux --add-mcp NAME --mcp-url URL\n";
                out << "           or: ainiux --add-mcp NAME --mcp-transport stdio -- command args\n";
            } else {
                out << "Installed MCP servers (" << registry.servers.size() << "):\n";
                for (const auto& entry : registry.servers) {
                    const mcp::ServerConfig& cfg = entry.second;
                    out << "  " << cfg.name << (cfg.enabled ? "" : " (disabled)")
                        << "  [" << mcp::transport_kind_name(cfg.transport) << "]";
                    if (cfg.transport == mcp::TransportKind::Http)
                        out << "  " << cfg.url;
                    else {
                        out << "  " << cfg.command;
                        for (const std::string& a : cfg.args) out << " " << a;
                    }
                    if (cfg.last_dialect != mcp::Dialect::Unknown)
                        out << "  dialect=" << mcp::dialect_name(cfg.last_dialect);
                    out << "\n";
                }
            }
        }
        return ok_error();
    }

    if (options.remove_mcp) {
        Error err = mcp::remove_server(registry, options.mcp_name);
        if (!err.ok()) return err;
        err = save(registry, options);
        if (!err.ok()) return err;
        if (!options.quiet) std::cerr << "Removed MCP server: " << options.mcp_name << "\n";
        return ok_error();
    }

    if (options.enable_mcp || options.disable_mcp) {
        Error err =
            mcp::set_server_enabled(registry, options.mcp_name, options.enable_mcp);
        if (!err.ok()) return err;
        err = save(registry, options);
        if (!err.ok()) return err;
        if (!options.quiet) {
            std::cerr << (options.enable_mcp ? "Enabled" : "Disabled")
                      << " MCP server: " << options.mcp_name << "\n";
        }
        return ok_error();
    }

    if (options.add_mcp) {
        mcp::ServerConfig cfg;
        cfg.name = options.mcp_name;
        cfg.allow_private = options.mcp_allow_private;
        cfg.headers = options.mcp_headers;
        cfg.env = options.mcp_env;
        if (!options.mcp_protocol_hint.empty()) {
            if (!mcp::parse_protocol_hint(options.mcp_protocol_hint, cfg.protocol_hint))
                return {ErrorCode::BadArgs, "invalid --mcp-protocol value"};
        }

        mcp::TransportKind transport = mcp::TransportKind::Http;
        if (!options.mcp_transport.empty()) {
            if (!mcp::parse_transport_kind(options.mcp_transport, transport))
                return {ErrorCode::BadArgs, "invalid --mcp-transport (use http or stdio)"};
        } else if (!options.mcp_url.empty()) {
            transport = mcp::TransportKind::Http;
        } else if (!options.mcp_command_args.empty()) {
            transport = mcp::TransportKind::Stdio;
        } else {
            return {ErrorCode::BadArgs,
                    "--add-mcp requires --mcp-url URL or --mcp-transport stdio -- command..."};
        }
        cfg.transport = transport;

        if (transport == mcp::TransportKind::Http) {
            cfg.url = options.mcp_url;
            if (cfg.url.empty())
                return {ErrorCode::BadArgs, "HTTP MCP install requires --mcp-url"};
        } else {
            if (options.mcp_command_args.empty())
                return {ErrorCode::BadArgs,
                        "stdio MCP install requires: --mcp-transport stdio -- command [args...]"};
            cfg.command = options.mcp_command_args.front();
            cfg.args.assign(options.mcp_command_args.begin() + 1, options.mcp_command_args.end());
        }

        Error err = mcp::add_or_update_server(registry, cfg);
        if (!err.ok()) return err;
        err = save(registry, options);
        if (!err.ok()) return err;

        // Optional connectivity probe (non-fatal warning on failure).
        if (!options.quiet) {
            mcp::Client client;
            mcp::ConnectOptions copts;
            copts.block_private_addresses = !cfg.allow_private && !options.allow_private_url_fetch;
            copts.insecure_tls = options.insecure_tls;
            Error probe = client.connect(registry.servers[cfg.name], copts);
            if (probe.ok()) {
                registry.servers[cfg.name].last_dialect = client.dialect();
                (void)save(registry, options);
                mcp::ToolsListResult tools;
                if (client.list_tools(tools).ok()) {
                    std::cerr << "Installed MCP server '" << cfg.name << "' ("
                              << mcp::dialect_name(client.dialect()) << ", "
                              << tools.tools.size() << " tools)\n";
                } else {
                    std::cerr << "Installed MCP server '" << cfg.name
                              << "' (connected; tools/list failed)\n";
                }
                client.close();
            } else {
                std::cerr << "Installed MCP server '" << cfg.name
                          << "' (saved; connect probe failed: " << probe.message << ")\n";
            }
        }
        return ok_error();
    }

    return {ErrorCode::Internal, "no MCP manage action"};
}

}  // namespace ainiux::app
