#include "mcp/registry.hpp"

#include <cctype>
#include <chrono>
#include <filesystem>
#include <sstream>

#include "chat/session.hpp"
#include "json/json.hpp"
#include "platform/environment.hpp"
#include "platform/filesystem.hpp"

namespace ainiux::mcp {
namespace {

json::Value jstring(const std::string& s) {
    json::Value v;
    v.type = json::Value::Type::String;
    v.string = s;
    return v;
}

json::Value jbool(bool b) {
    json::Value v;
    v.type = json::Value::Type::Bool;
    v.boolean = b;
    return v;
}

json::Value jnumber(double n) {
    json::Value v;
    v.type = json::Value::Type::Number;
    v.number = n;
    return v;
}

json::Value jarray_of_strings(const std::vector<std::string>& items) {
    json::Value arr;
    arr.type = json::Value::Type::Array;
    for (const std::string& item : items) arr.array.push_back(jstring(item));
    return arr;
}

std::vector<std::string> read_string_array(const json::Value* value) {
    std::vector<std::string> out;
    if (value == nullptr || !value->is_array()) return out;
    for (const json::Value& item : value->array) {
        if (item.is_string()) out.push_back(item.string);
    }
    return out;
}

std::string require_string(const json::Value* value, const std::string& fallback = {}) {
    if (value != nullptr && value->is_string()) return value->string;
    return fallback;
}

bool require_bool(const json::Value* value, bool fallback) {
    if (value != nullptr && value->type == json::Value::Type::Bool) return value->boolean;
    return fallback;
}

long require_long(const json::Value* value, long fallback) {
    if (value != nullptr && value->type == json::Value::Type::Number)
        return static_cast<long>(value->number);
    return fallback;
}

}  // namespace

std::string default_mcp_dir() {
    const std::string home = platform::home_directory();
    if (home.empty()) return {};
    return (std::filesystem::u8path(home) / ".ainiux" / "mcp").u8string();
}

std::string default_registry_path() {
    const std::string dir = default_mcp_dir();
    if (dir.empty()) return {};
    return (std::filesystem::u8path(dir) / "registry.json").u8string();
}

Error ensure_mcp_dir() {
    const std::string dir = default_mcp_dir();
    if (dir.empty())
        return {ErrorCode::FileWrite, "cannot locate home directory for ~/.ainiux/mcp"};
    return platform::ensure_private_directory(dir, true, true);
}

bool valid_server_name(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    for (char c : name) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'))
            return false;
    }
    return true;
}

const char* transport_kind_name(TransportKind kind) {
    switch (kind) {
        case TransportKind::Http:
            return "http";
        case TransportKind::Stdio:
            return "stdio";
    }
    return "http";
}

const char* dialect_name(Dialect dialect) {
    switch (dialect) {
        case Dialect::Unknown:
            return "unknown";
        case Dialect::Stateless20260728:
            return "2026-07-28";
        case Dialect::Streamable20251125:
            return "2025-11-25";
        case Dialect::Streamable20250326:
            return "2025-03-26";
        case Dialect::HttpSse20241105:
            return "2024-11-05";
    }
    return "unknown";
}

const char* protocol_hint_name(ProtocolHint hint) {
    switch (hint) {
        case ProtocolHint::Auto:
            return "auto";
        case ProtocolHint::Stateless20260728:
            return "2026-07-28";
        case ProtocolHint::Legacy20251125:
            return "2025-11-25";
        case ProtocolHint::Legacy20250326:
            return "2025-03-26";
    }
    return "auto";
}

bool parse_transport_kind(const std::string& text, TransportKind& out) {
    if (text == "http" || text == "HTTP" || text == "streamable" || text == "sse") {
        out = TransportKind::Http;
        return true;
    }
    if (text == "stdio" || text == "STDIO" || text == "std") {
        out = TransportKind::Stdio;
        return true;
    }
    return false;
}

bool parse_protocol_hint(const std::string& text, ProtocolHint& out) {
    if (text.empty() || text == "auto") {
        out = ProtocolHint::Auto;
        return true;
    }
    if (text == "2026-07-28" || text == "stateless") {
        out = ProtocolHint::Stateless20260728;
        return true;
    }
    if (text == "2025-11-25") {
        out = ProtocolHint::Legacy20251125;
        return true;
    }
    if (text == "2025-03-26") {
        out = ProtocolHint::Legacy20250326;
        return true;
    }
    return false;
}

bool parse_dialect(const std::string& text, Dialect& out) {
    if (text.empty() || text == "unknown") {
        out = Dialect::Unknown;
        return true;
    }
    if (text == "2026-07-28") {
        out = Dialect::Stateless20260728;
        return true;
    }
    if (text == "2025-11-25") {
        out = Dialect::Streamable20251125;
        return true;
    }
    if (text == "2025-03-26") {
        out = Dialect::Streamable20250326;
        return true;
    }
    if (text == "2024-11-05") {
        out = Dialect::HttpSse20241105;
        return true;
    }
    return false;
}

std::string expand_env_refs(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (std::size_t i = 0; i < input.size();) {
        if (input[i] == '$' && i + 1 < input.size() && input[i + 1] == '{') {
            const std::size_t end = input.find('}', i + 2);
            if (end == std::string::npos) {
                out.push_back(input[i]);
                ++i;
                continue;
            }
            const std::string body = input.substr(i + 2, end - (i + 2));
            std::string name = body;
            std::string default_value;
            const std::size_t colon = body.find(":-");
            if (colon != std::string::npos) {
                name = body.substr(0, colon);
                default_value = body.substr(colon + 2);
            }
            const std::string env = platform::environment_value(name.c_str());
            out += env.empty() ? default_value : env;
            i = end + 1;
            continue;
        }
        out.push_back(input[i]);
        ++i;
    }
    return out;
}

Error load_registry(Registry& registry, const std::string& path) {
    registry = Registry{};
    const std::string resolved = path.empty() ? default_registry_path() : path;
    if (resolved.empty())
        return {ErrorCode::FileRead, "MCP registry path is empty"};
    if (!std::filesystem::exists(std::filesystem::u8path(resolved))) {
        registry.schema_version = 1;
        return ok_error();
    }
    std::string data;
    Error err = platform::read_file_bounded(resolved, 4U * 1024U * 1024U, data);
    if (!err.ok()) return err;
    if (data.empty()) return ok_error();
    json::ParseResult parsed = json::parse(data);
    if (!parsed.error.ok())
        return {ErrorCode::JsonParse, "MCP registry is not valid JSON: " + parsed.error.message};
    if (!parsed.value.is_object())
        return {ErrorCode::JsonParse, "MCP registry root must be an object"};
    if (const json::Value* schema = parsed.value.get("schema_version");
        schema != nullptr && schema->type == json::Value::Type::Number) {
        registry.schema_version = static_cast<int>(schema->number);
    }
    const json::Value* servers = parsed.value.get("servers");
    if (servers == nullptr) return ok_error();
    if (!servers->is_object())
        return {ErrorCode::JsonParse, "MCP registry servers must be an object"};
    for (const auto& entry : servers->object) {
        if (!entry.second.is_object()) continue;
        ServerConfig cfg;
        cfg.name = entry.first;
        if (!valid_server_name(cfg.name)) continue;
        const json::Value& obj = entry.second;
        cfg.enabled = require_bool(obj.get("enabled"), true);
        TransportKind transport = TransportKind::Http;
        (void)parse_transport_kind(require_string(obj.get("transport"), "http"), transport);
        cfg.transport = transport;
        cfg.url = require_string(obj.get("url"));
        cfg.headers = read_string_array(obj.get("headers"));
        cfg.command = require_string(obj.get("command"));
        cfg.args = read_string_array(obj.get("args"));
        cfg.env = read_string_array(obj.get("env"));
        cfg.cwd = require_string(obj.get("cwd"));
        ProtocolHint hint = ProtocolHint::Auto;
        (void)parse_protocol_hint(require_string(obj.get("protocol_hint"), "auto"), hint);
        cfg.protocol_hint = hint;
        Dialect dialect = Dialect::Unknown;
        (void)parse_dialect(require_string(obj.get("last_dialect"), "unknown"), dialect);
        cfg.last_dialect = dialect;
        cfg.startup_timeout_ms = require_long(obj.get("startup_timeout_ms"), 30000);
        cfg.tool_timeout_ms = require_long(obj.get("tool_timeout_ms"), 120000);
        cfg.allow_private = require_bool(obj.get("allow_private"), false);
        cfg.created_at = require_string(obj.get("created_at"));
        cfg.updated_at = require_string(obj.get("updated_at"));
        registry.servers[cfg.name] = std::move(cfg);
    }
    return ok_error();
}

Error save_registry(const Registry& registry, const std::string& path) {
    const std::string resolved = path.empty() ? default_registry_path() : path;
    if (resolved.empty())
        return {ErrorCode::FileWrite, "MCP registry path is empty"};
    Error err = ensure_mcp_dir();
    if (!err.ok() && path.empty()) return err;
    if (!path.empty()) {
        const auto parent = std::filesystem::u8path(resolved).parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
        }
    }
    json::Value root;
    root.type = json::Value::Type::Object;
    root.object["schema_version"] = jnumber(registry.schema_version <= 0 ? 1 : registry.schema_version);
    json::Value servers;
    servers.type = json::Value::Type::Object;
    for (const auto& entry : registry.servers) {
        const ServerConfig& cfg = entry.second;
        json::Value obj;
        obj.type = json::Value::Type::Object;
        obj.object["enabled"] = jbool(cfg.enabled);
        obj.object["transport"] = jstring(transport_kind_name(cfg.transport));
        if (!cfg.url.empty()) obj.object["url"] = jstring(cfg.url);
        if (!cfg.headers.empty()) obj.object["headers"] = jarray_of_strings(cfg.headers);
        if (!cfg.command.empty()) obj.object["command"] = jstring(cfg.command);
        if (!cfg.args.empty()) obj.object["args"] = jarray_of_strings(cfg.args);
        if (!cfg.env.empty()) obj.object["env"] = jarray_of_strings(cfg.env);
        if (!cfg.cwd.empty()) obj.object["cwd"] = jstring(cfg.cwd);
        obj.object["protocol_hint"] = jstring(protocol_hint_name(cfg.protocol_hint));
        if (cfg.last_dialect != Dialect::Unknown)
            obj.object["last_dialect"] = jstring(dialect_name(cfg.last_dialect));
        obj.object["startup_timeout_ms"] = jnumber(static_cast<double>(cfg.startup_timeout_ms));
        obj.object["tool_timeout_ms"] = jnumber(static_cast<double>(cfg.tool_timeout_ms));
        obj.object["allow_private"] = jbool(cfg.allow_private);
        if (!cfg.created_at.empty()) obj.object["created_at"] = jstring(cfg.created_at);
        if (!cfg.updated_at.empty()) obj.object["updated_at"] = jstring(cfg.updated_at);
        servers.object[cfg.name] = std::move(obj);
    }
    root.object["servers"] = std::move(servers);
    const std::string data = json::stringify(root) + "\n";
    return platform::atomic_write_private(resolved, data, true);
}

Error add_or_update_server(Registry& registry, ServerConfig config) {
    if (!valid_server_name(config.name))
        return {ErrorCode::BadArgs,
                "MCP server name must be 1-64 characters of [A-Za-z0-9_-]"};
    if (config.transport == TransportKind::Http && config.url.empty())
        return {ErrorCode::BadArgs, "HTTP MCP server requires --url"};
    if (config.transport == TransportKind::Stdio && config.command.empty())
        return {ErrorCode::BadArgs, "stdio MCP server requires a command"};
    const std::string now = chat::current_timestamp_utc();
    auto existing = registry.servers.find(config.name);
    if (existing == registry.servers.end()) {
        config.created_at = now;
        config.updated_at = now;
    } else {
        if (config.created_at.empty()) config.created_at = existing->second.created_at;
        if (config.created_at.empty()) config.created_at = now;
        config.updated_at = now;
        if (config.last_dialect == Dialect::Unknown)
            config.last_dialect = existing->second.last_dialect;
    }
    registry.servers[config.name] = std::move(config);
    return ok_error();
}

Error remove_server(Registry& registry, const std::string& name) {
    if (registry.servers.erase(name) == 0)
        return {ErrorCode::BadArgs, "MCP server not found: " + name};
    return ok_error();
}

Error set_server_enabled(Registry& registry, const std::string& name, bool enabled) {
    auto it = registry.servers.find(name);
    if (it == registry.servers.end())
        return {ErrorCode::BadArgs, "MCP server not found: " + name};
    it->second.enabled = enabled;
    it->second.updated_at = chat::current_timestamp_utc();
    return ok_error();
}

}  // namespace ainiux::mcp
