#pragma once

#include <map>
#include <string>
#include <vector>

#include "common.hpp"

namespace ainiux::mcp {

enum class TransportKind { Http, Stdio };

enum class ProtocolHint { Auto, Stateless20260728, Legacy20251125, Legacy20250326 };

enum class Dialect {
    Unknown,
    Stateless20260728,
    Streamable20251125,
    Streamable20250326,
    HttpSse20241105,
};

struct ServerConfig {
    std::string name;
    bool enabled = true;
    TransportKind transport = TransportKind::Http;
    std::string url;
    std::vector<std::string> headers;  // "Name: value"
    std::string command;
    std::vector<std::string> args;
    std::vector<std::string> env;  // NAME=VALUE
    std::string cwd;
    ProtocolHint protocol_hint = ProtocolHint::Auto;
    Dialect last_dialect = Dialect::Unknown;
    long startup_timeout_ms = 30000;
    long tool_timeout_ms = 120000;
    bool allow_private = false;
    std::string created_at;
    std::string updated_at;
};

struct Registry {
    int schema_version = 1;
    std::map<std::string, ServerConfig> servers;
};

// User private registry: ~/.ainiux/mcp/registry.json
std::string default_registry_path();
std::string default_mcp_dir();

Error ensure_mcp_dir();
Error load_registry(Registry& registry, const std::string& path = {});
Error save_registry(const Registry& registry, const std::string& path = {});

bool valid_server_name(const std::string& name);
Error add_or_update_server(Registry& registry, ServerConfig config);
Error remove_server(Registry& registry, const std::string& name);
Error set_server_enabled(Registry& registry, const std::string& name, bool enabled);

const char* transport_kind_name(TransportKind kind);
const char* dialect_name(Dialect dialect);
const char* protocol_hint_name(ProtocolHint hint);

bool parse_transport_kind(const std::string& text, TransportKind& out);
bool parse_protocol_hint(const std::string& text, ProtocolHint& out);
bool parse_dialect(const std::string& text, Dialect& out);

// Expand ${VAR} and ${VAR:-default} in a string using the process environment.
std::string expand_env_refs(const std::string& input);

}  // namespace ainiux::mcp
