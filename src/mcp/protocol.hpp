#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common.hpp"
#include "json/json.hpp"
#include "mcp/registry.hpp"

namespace ainiux::mcp {

struct ClientInfo {
    std::string name = "ainiux";
    std::string version;
};

struct ToolInfo {
    std::string name;
    std::string description;
    std::string input_schema_json;
};

struct ToolsListResult {
    std::vector<ToolInfo> tools;
    long long ttl_ms = -1;
    std::string cache_scope;
};

struct ToolCallResult {
    std::string text;
    bool is_error = false;
    std::string raw_json;
};

struct JsonRpcResponse {
    bool is_notification_ack = false;
    json::Value id;
    bool has_result = false;
    json::Value result;
    bool has_error = false;
    int error_code = 0;
    std::string error_message;
    std::string raw_body;
};

json::Value make_json_string(const std::string& value);
json::Value make_json_number(double value);
json::Value make_json_bool(bool value);
json::Value make_json_object();
json::Value make_json_array();

std::string client_version_string();
ClientInfo default_client_info();

json::Value build_request(std::int64_t id,
                          const std::string& method,
                          const json::Value& params);

json::Value build_initialize_params(const std::string& protocol_version,
                                    const ClientInfo& client);
json::Value build_stateless_meta(const ClientInfo& client,
                                 const std::string& protocol_version);
json::Value build_tools_call_params(const std::string& tool_name,
                                    const std::string& arguments_json,
                                    const json::Value* meta = nullptr);

Error parse_jsonrpc_response(const std::string& body, JsonRpcResponse& out);
Error extract_tools_list(const json::Value& result, ToolsListResult& out);
Error extract_tool_call_result(const json::Value& result, ToolCallResult& out);

std::string qualified_tool_name(const std::string& server, const std::string& tool);
bool parse_qualified_tool_name(const std::string& qualified,
                               std::string& server,
                               std::string& tool);

std::string sanitize_name_component(const std::string& value);

}  // namespace ainiux::mcp
