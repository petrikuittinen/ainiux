#include "mcp/protocol.hpp"

#include <cctype>
#include <sstream>

#include "ainiux/version.hpp"

namespace ainiux::mcp {

json::Value make_json_string(const std::string& value) {
    json::Value v;
    v.type = json::Value::Type::String;
    v.string = value;
    return v;
}

json::Value make_json_number(double value) {
    json::Value v;
    v.type = json::Value::Type::Number;
    v.number = value;
    return v;
}

json::Value make_json_bool(bool value) {
    json::Value v;
    v.type = json::Value::Type::Bool;
    v.boolean = value;
    return v;
}

json::Value make_json_object() {
    json::Value v;
    v.type = json::Value::Type::Object;
    return v;
}

json::Value make_json_array() {
    json::Value v;
    v.type = json::Value::Type::Array;
    return v;
}

std::string client_version_string() { return versionNumber; }

ClientInfo default_client_info() {
    ClientInfo info;
    info.name = "ainiux";
    info.version = client_version_string();
    return info;
}

json::Value build_request(std::int64_t id,
                          const std::string& method,
                          const json::Value& params) {
    json::Value req = make_json_object();
    req.object["jsonrpc"] = make_json_string("2.0");
    req.object["id"] = make_json_number(static_cast<double>(id));
    req.object["method"] = make_json_string(method);
    if (!params.is_null()) req.object["params"] = params;
    return req;
}

json::Value build_initialize_params(const std::string& protocol_version,
                                    const ClientInfo& client) {
    json::Value params = make_json_object();
    params.object["protocolVersion"] = make_json_string(protocol_version);
    params.object["capabilities"] = make_json_object();
    json::Value info = make_json_object();
    info.object["name"] = make_json_string(client.name);
    info.object["version"] = make_json_string(client.version);
    params.object["clientInfo"] = std::move(info);
    return params;
}

json::Value build_stateless_meta(const ClientInfo& client,
                                 const std::string& protocol_version) {
    json::Value meta = make_json_object();
    json::Value info = make_json_object();
    info.object["name"] = make_json_string(client.name);
    info.object["version"] = make_json_string(client.version);
    meta.object["io.modelcontextprotocol/clientInfo"] = std::move(info);
    meta.object["io.modelcontextprotocol/protocolVersion"] =
        make_json_string(protocol_version);
    meta.object["io.modelcontextprotocol/clientCapabilities"] = make_json_object();
    return meta;
}

json::Value build_tools_call_params(const std::string& tool_name,
                                    const std::string& arguments_json,
                                    const json::Value* meta) {
    json::Value params = make_json_object();
    params.object["name"] = make_json_string(tool_name);
    if (arguments_json.empty()) {
        params.object["arguments"] = make_json_object();
    } else {
        json::ParseResult parsed = json::parse(arguments_json);
        if (parsed.error.ok() && parsed.value.is_object()) {
            params.object["arguments"] = std::move(parsed.value);
        } else {
            params.object["arguments"] = make_json_object();
        }
    }
    if (meta != nullptr && meta->is_object()) params.object["_meta"] = *meta;
    return params;
}

Error parse_jsonrpc_response(const std::string& body, JsonRpcResponse& out) {
    out = JsonRpcResponse{};
    out.raw_body = body;
    if (body.empty()) {
        out.is_notification_ack = true;
        return ok_error();
    }
    // SSE: extract first data: JSON object.
    std::string payload = body;
    if (body.find("event:") != std::string::npos || body.find("data:") != std::string::npos) {
        std::istringstream stream(body);
        std::string line;
        std::string data;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("data:", 0) == 0) {
                std::string piece = line.substr(5);
                if (!piece.empty() && piece[0] == ' ') piece.erase(0, 1);
                if (!data.empty()) data.push_back('\n');
                data += piece;
            } else if (line.empty() && !data.empty()) {
                break;
            }
        }
        if (!data.empty()) payload = data;
    }
    json::ParseResult parsed = json::parse(payload);
    if (!parsed.error.ok())
        return {ErrorCode::JsonParse, "MCP JSON-RPC response parse failed: " + parsed.error.message};
    if (!parsed.value.is_object())
        return {ErrorCode::JsonParse, "MCP JSON-RPC response must be an object"};
    if (const json::Value* id = parsed.value.get("id"); id != nullptr) out.id = *id;
    if (const json::Value* result = parsed.value.get("result"); result != nullptr) {
        out.has_result = true;
        out.result = *result;
    }
    if (const json::Value* error = parsed.value.get("error"); error != nullptr) {
        out.has_error = true;
        if (error->is_object()) {
            if (const json::Value* code = error->get("code");
                code != nullptr && code->type == json::Value::Type::Number)
                out.error_code = static_cast<int>(code->number);
            if (const json::Value* message = error->get("message");
                message != nullptr && message->is_string())
                out.error_message = message->string;
            else
                out.error_message = "MCP error";
        } else {
            out.error_message = "MCP error";
        }
    }
    if (!out.has_result && !out.has_error)
        return {ErrorCode::ProviderSchema, "MCP JSON-RPC response missing result and error"};
    return ok_error();
}

Error extract_tools_list(const json::Value& result, ToolsListResult& out) {
    out = ToolsListResult{};
    if (!result.is_object())
        return {ErrorCode::ProviderSchema, "tools/list result must be an object"};
    const json::Value* tools = result.get("tools");
    if (tools == nullptr || !tools->is_array())
        return {ErrorCode::ProviderSchema, "tools/list result.tools must be an array"};
    if (const json::Value* ttl = result.get("ttlMs");
        ttl != nullptr && ttl->type == json::Value::Type::Number)
        out.ttl_ms = static_cast<long long>(ttl->number);
    if (const json::Value* scope = result.get("cacheScope");
        scope != nullptr && scope->is_string())
        out.cache_scope = scope->string;
    for (const json::Value& item : tools->array) {
        if (!item.is_object()) continue;
        const json::Value* name = item.get("name");
        if (name == nullptr || !name->is_string() || name->string.empty()) continue;
        ToolInfo info;
        info.name = name->string;
        if (const json::Value* desc = item.get("description");
            desc != nullptr && desc->is_string())
            info.description = desc->string;
        if (const json::Value* schema = item.get("inputSchema"); schema != nullptr)
            info.input_schema_json = json::stringify(*schema);
        else
            info.input_schema_json = "{}";
        out.tools.push_back(std::move(info));
    }
    return ok_error();
}

Error extract_tool_call_result(const json::Value& result, ToolCallResult& out) {
    out = ToolCallResult{};
    out.raw_json = json::stringify(result);
    if (!result.is_object()) {
        out.text = out.raw_json;
        return ok_error();
    }
    if (const json::Value* is_error = result.get("isError");
        is_error != nullptr && is_error->type == json::Value::Type::Bool)
        out.is_error = is_error->boolean;
    const json::Value* content = result.get("content");
    if (content != nullptr && content->is_array()) {
        std::ostringstream text;
        bool first = true;
        for (const json::Value& part : content->array) {
            if (!part.is_object()) continue;
            const json::Value* type = part.get("type");
            const json::Value* t = part.get("text");
            if (type != nullptr && type->is_string() && type->string == "text" &&
                t != nullptr && t->is_string()) {
                if (!first) text << '\n';
                text << t->string;
                first = false;
            }
        }
        out.text = text.str();
    }
    if (out.text.empty()) {
        if (const json::Value* structured = result.get("structuredContent");
            structured != nullptr)
            out.text = json::stringify(*structured);
        else
            out.text = out.raw_json;
    }
    return ok_error();
}

std::string sanitize_name_component(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')
            out.push_back(c);
        else
            out.push_back('_');
    }
    if (out.empty()) out = "unnamed";
    return out;
}

std::string qualified_tool_name(const std::string& server, const std::string& tool) {
    return "mcp__" + sanitize_name_component(server) + "__" + sanitize_name_component(tool);
}

bool parse_qualified_tool_name(const std::string& qualified,
                               std::string& server,
                               std::string& tool) {
    server.clear();
    tool.clear();
    constexpr const char kPrefix[] = "mcp__";
    if (qualified.rfind(kPrefix, 0) != 0) return false;
    const std::string rest = qualified.substr(sizeof(kPrefix) - 1);
    const std::size_t sep = rest.find("__");
    if (sep == std::string::npos || sep == 0) return false;
    server = rest.substr(0, sep);
    tool = rest.substr(sep + 2);
    return !server.empty() && !tool.empty();
}

}  // namespace ainiux::mcp
