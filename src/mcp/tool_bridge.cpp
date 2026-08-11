#include "mcp/tool_bridge.hpp"
#include "mcp/arg_rewrite.hpp"
#include "mcp/protocol.hpp"

#include <sstream>

#include "json/json.hpp"

namespace ainiux::mcp {
namespace {

// Match native agent tool envelope so agent_loop::tool_result_ok accepts MCP
// successes (requires top-level {"ok": true, ...}).
std::string agent_envelope(bool ok,
                           json::Value data,
                           const std::string& error_code,
                           const std::string& error_message,
                           bool truncated = false) {
    json::Value root = make_json_object();
    root.object["ok"] = make_json_bool(ok);
    root.object["data"] = std::move(data);
    if (error_code.empty()) {
        root.object["error"] = json::Value{};
    } else {
        json::Value error = make_json_object();
        error.object["code"] = make_json_string(error_code);
        error.object["message"] = make_json_string(error_message);
        root.object["error"] = std::move(error);
    }
    root.object["warnings"] = make_json_array();
    root.object["truncated"] = make_json_bool(truncated);
    root.object["metadata"] = make_json_object();
    return json::stringify(root);
}

json::Value data_from_tool_result(const ToolCallResult& result) {
    // Prefer structured JSON when the server returned an object/array payload.
    if (!result.raw_json.empty()) {
        json::ParseResult parsed = json::parse(result.raw_json);
        if (parsed.error.ok() &&
            (parsed.value.is_object() || parsed.value.is_array())) {
            // If MCP result has content[] text already extracted, still keep full
            // structure when raw is the full tools/call result object.
            if (parsed.value.is_object() && parsed.value.get("content") != nullptr) {
                json::Value data = make_json_object();
                if (!result.text.empty())
                    data.object["text"] = make_json_string(result.text);
                data.object["mcp_result"] = std::move(parsed.value);
                return data;
            }
            return std::move(parsed.value);
        }
    }
    if (!result.text.empty()) return make_json_string(result.text);
    return make_json_object();
}

}  // namespace

struct ToolBridge::Impl {
    std::shared_ptr<Manager> manager;
    std::vector<provider::FunctionDefinition> definitions;
    std::vector<std::string> last_errors;
};

ToolBridge::ToolBridge() : impl_(std::make_unique<Impl>()) {}
ToolBridge::~ToolBridge() = default;
ToolBridge::ToolBridge(ToolBridge&&) noexcept = default;
ToolBridge& ToolBridge::operator=(ToolBridge&&) noexcept = default;

void ToolBridge::set_manager(std::shared_ptr<Manager> manager) {
    impl_->manager = std::move(manager);
}
std::shared_ptr<Manager> ToolBridge::manager() const { return impl_->manager; }

Error ToolBridge::refresh(runtime::CancellationToken cancellation) {
    impl_->definitions.clear();
    impl_->last_errors.clear();
    if (!impl_->manager) return ok_error();

    ConnectOptions opts = impl_->manager->connect_options();
    opts.cancellation = cancellation;
    impl_->manager->set_connect_options(opts);

    std::vector<std::pair<std::string, ToolsListResult>> listed;
    Error err = impl_->manager->list_all_tools(listed);
    if (!err.ok()) {
        impl_->last_errors.push_back(err.message);
        return err;
    }

    constexpr std::size_t kMaxTotal = 256;
    constexpr std::size_t kMaxPerServer = 128;
    for (const auto& entry : listed) {
        const std::string& server = entry.first;
        std::size_t count = 0;
        for (const ToolInfo& tool : entry.second.tools) {
            if (count >= kMaxPerServer) break;
            if (impl_->definitions.size() >= kMaxTotal) break;
            provider::FunctionDefinition def;
            def.name = qualified_tool_name(server, tool.name);
            def.description = "[MCP:" + server + "] " +
                              (tool.description.empty() ? tool.name : tool.description);
            def.parameters_json =
                tool.input_schema_json.empty() ? "{}" : tool.input_schema_json;
            // Ensure object schema for providers that require type:object.
            json::ParseResult parsed = json::parse(def.parameters_json);
            if (!parsed.error.ok() || !parsed.value.is_object()) {
                def.parameters_json =
                    "{\"type\":\"object\",\"properties\":{}}";
            } else if (parsed.value.get("type") == nullptr) {
                parsed.value.object["type"] = make_json_string("object");
                def.parameters_json = json::stringify(parsed.value);
            }
            impl_->definitions.push_back(std::move(def));
            ++count;
        }
    }
    return ok_error();
}

std::vector<provider::FunctionDefinition> ToolBridge::definitions() const {
    return impl_->definitions;
}

const std::vector<std::string>& ToolBridge::last_errors() const {
    return impl_->last_errors;
}

bool ToolBridge::is_mcp_tool(const std::string& name) const {
    return name.rfind("mcp__", 0) == 0;
}

std::string ToolBridge::execute(const std::string& qualified_name,
                                const std::string& arguments_json,
                                runtime::CancellationToken cancellation) const {
    if (!impl_->manager) {
        return agent_envelope(false, make_json_object(), "mcp_not_configured",
                              "MCP manager is not configured");
    }
    ConnectOptions opts = impl_->manager->connect_options();
    opts.cancellation = cancellation;
    impl_->manager->set_connect_options(opts);

    std::string wire_args = arguments_json;
    std::string server_name;
    std::string tool_name;
    if (attachment_bag_ != nullptr &&
        parse_qualified_tool_name(qualified_name, server_name, tool_name)) {
        Client* client = nullptr;
        // ensure_connected for dialect/transport; ignore error and still try call.
        (void)impl_->manager->ensure_connected(server_name, client);
        ServerConfig cfg;
        auto it = impl_->manager->registry().servers.find(server_name);
        if (it != impl_->manager->registry().servers.end()) cfg = it->second;
        else if (client != nullptr) cfg = client->config();
        ArgRewriteResult rewritten;
        Error rew = rewrite_mcp_arguments(cfg, tool_name, "", wire_args, *attachment_bag_,
                                          rewrite_caps_, cancellation, rewritten);
        if (!rew.ok()) {
            return agent_envelope(false, make_json_object(), "mcp_attachment_too_large",
                                  rew.message);
        }
        if (rewritten.changed) wire_args = rewritten.arguments_json;
    }

    ToolCallResult result;
    Error err =
        impl_->manager->call_qualified_tool(qualified_name, wire_args, result);
    if (!err.ok()) {
        return agent_envelope(false, make_json_object(), "mcp_error", err.message);
    }
    if (result.is_error) {
        const std::string message =
            result.text.empty() ? "MCP tool reported an error" : result.text;
        return agent_envelope(false, data_from_tool_result(result), "mcp_tool_error",
                              message);
    }

    // Bound very large MCP payloads the same way native tools surface truncation.
    constexpr std::size_t kMaxResultChars = 64U * 1024U;
    bool truncated = false;
    json::Value data = data_from_tool_result(result);
    std::string body = agent_envelope(true, data, "", "");
    if (body.size() > kMaxResultChars) {
        truncated = true;
        json::Value slim = make_json_object();
        slim.object["text"] = make_json_string(
            result.text.empty()
                ? result.raw_json.substr(0, kMaxResultChars / 2)
                : result.text.substr(0, kMaxResultChars / 2));
        slim.object["note"] = make_json_string("MCP tool result truncated");
        body = agent_envelope(true, slim, "", "", truncated);
    }
    return body;
}

}  // namespace ainiux::mcp
