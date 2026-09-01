#include "server/mcp_adapter.hpp"

#include <cstdint>
#include <chrono>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <utility>

#include "ainiux/version.hpp"
#include "json/json.hpp"
#include "server/job_registry.hpp"
#include "server/job_service.hpp"

namespace ainiux::server {
namespace {

constexpr char kProtocolVersion[] = "2026-07-28";
constexpr char kTasksExtension[] = "io.modelcontextprotocol/tasks";
constexpr std::size_t kMaxToolNameBytes = 128U;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kParseError = -32700;
constexpr int kHeaderMismatch = -32020;
constexpr int kUnsupportedVersion = -32022;
constexpr int kTaskNotFound = -32001;

struct TaskEntry {
    std::string id;
    std::shared_ptr<Job> job;
};

bool json_content_type(const http::Request& request) {
    const auto found = request.headers.find("content-type");
    if (found == request.headers.end()) return false;
    std::string value = ascii_lower(found->second);
    const std::size_t semicolon = value.find(';');
    if (semicolon != std::string::npos) value.resize(semicolon);
    return ascii_trim(std::move(value)) == "application/json";
}

bool accepts(const std::string& header, const char* wanted) {
    std::size_t start = 0;
    while (start <= header.size()) {
        const std::size_t comma = header.find(',', start);
        std::string item = header.substr(start, comma == std::string::npos
                                                  ? std::string::npos : comma - start);
        const std::size_t semicolon = item.find(';');
        if (semicolon != std::string::npos) item.resize(semicolon);
        if (ascii_trim(ascii_lower(std::move(item))) == wanted) return true;
        if (comma == std::string::npos) break;
        start = comma + 1U;
    }
    return false;
}

bool decode_base64(const std::string& input, std::string& output) {
    if (input.empty() || input.size() % 4U != 0) return false;
    auto value = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    output.clear();
    output.reserve((input.size() / 4U) * 3U);
    for (std::size_t i = 0; i < input.size(); i += 4U) {
        const bool third_padding = input[i + 2U] == '=';
        const bool fourth_padding = input[i + 3U] == '=';
        const int a = value(static_cast<unsigned char>(input[i]));
        const int b = value(static_cast<unsigned char>(input[i + 1U]));
        const int c = third_padding ? 0 : value(static_cast<unsigned char>(input[i + 2U]));
        const int d = fourth_padding ? 0 : value(static_cast<unsigned char>(input[i + 3U]));
        if (a < 0 || b < 0 || c < 0 || d < 0 ||
            (third_padding && !fourth_padding) ||
            (third_padding && i + 4U != input.size()) ||
            (fourth_padding && i + 4U != input.size())) return false;
        output.push_back(static_cast<char>((a << 2) | (b >> 4)));
        if (!third_padding) output.push_back(static_cast<char>((b << 4) | (c >> 2)));
        if (!fourth_padding) output.push_back(static_cast<char>((c << 6) | d));
    }
    return true;
}

bool decode_header_value(const std::string& value, std::string& decoded) {
    constexpr char prefix[] = "=?base64?";
    constexpr char suffix[] = "?=";
    if (value.rfind(prefix, 0) != 0 || value.size() < sizeof(prefix) - 1U + sizeof(suffix) - 1U ||
        value.compare(value.size() - (sizeof(suffix) - 1U), sizeof(suffix) - 1U, suffix) != 0) {
        decoded = value;
        return true;
    }
    const std::size_t start = sizeof(prefix) - 1U;
    const std::size_t count = value.size() - start - (sizeof(suffix) - 1U);
    return decode_base64(value.substr(start, count), decoded);
}

std::string id_json(const json::Value* id) {
    return id == nullptr ? "null" : json::stringify(*id);
}

McpResponse rpc_response(const std::string& id,
                         const std::string& result_json) {
    return {200, "application/json; charset=utf-8",
            "{\"jsonrpc\":\"2.0\",\"id\":" + id +
                ",\"result\":" + result_json + "}", false, {}};
}

McpResponse rpc_error(const std::string& id,
                      int code,
                      const std::string& message,
                      const std::string& data_json = {}) {
    std::string error = "{\"code\":" + std::to_string(code) +
                        ",\"message\":" + json::quote(message);
    if (!data_json.empty()) error += ",\"data\":" + data_json;
    error += "}";
    const int status = code == kMethodNotFound ? 404 :
                       code == kInvalidRequest || code == kHeaderMismatch ||
                               code == kUnsupportedVersion ? 400 : 200;
    return {status, "application/json; charset=utf-8",
            "{\"jsonrpc\":\"2.0\",\"id\":" + id +
                ",\"error\":" + error + "}", false, {}};
}

McpResponse invalid_transport(int code, const std::string& message) {
    return {400, "application/json; charset=utf-8",
            "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":" +
                std::to_string(code) + ",\"message\":" + json::quote(message) + "}}", false, {}};
}

std::string error_result(const std::string& message) {
    return "{\"resultType\":\"complete\",\"content\":[{\"type\":\"text\",\"text\":" +
           json::quote(message) + "}],\"isError\":true}";
}

std::string json_object_or_empty(const json::Value* value) {
    return value != nullptr && value->is_object() ? json::stringify(*value) : "{}";
}

std::string text_result(const std::string& text, const std::string& structured_json = {}) {
    std::string result = "{\"resultType\":\"complete\",\"content\":[{\"type\":\"text\",\"text\":" +
                         json::quote(text) + "}],\"isError\":false";
    if (!structured_json.empty()) result += ",\"structuredContent\":" + structured_json;
    return result + "}";
}

std::string job_submission_message(const ServiceSubmitResult& submitted) {
    if (!submitted.validation_error.ok()) return submitted.validation_error.message;
    switch (submitted.submission.status) {
        case SubmitStatus::AgentConflict: return "an agent operation is already active for this workspace";
        case SubmitStatus::Capacity: return "the bounded job registry is full";
        case SubmitStatus::Stopping: return "the server is stopping and accepts no new jobs";
        case SubmitStatus::InternalFailure: return "the server could not start the job worker";
        case SubmitStatus::IdempotencyConflict: return "the job idempotency key conflicts with an existing job";
        case SubmitStatus::Created:
        case SubmitStatus::Existing: return {};
    }
    return "the server could not submit the job";
}

bool task_extension_requested(const json::Value& meta) {
    const json::Value* clients = meta.get("io.modelcontextprotocol/clientCapabilities");
    const json::Value* extensions = clients == nullptr ? nullptr : clients->get("extensions");
    const json::Value* task = extensions == nullptr ? nullptr : extensions->get(kTasksExtension);
    return extensions != nullptr && extensions->is_object() && task != nullptr && task->is_object();
}

bool valid_jsonrpc_id(const json::Value& value) {
    return value.is_null() || value.is_string() || value.type == json::Value::Type::Number;
}

const char* tool_schema(const std::string& name) {
    if (name == "ainiux_chat") {
        return R"({"type":"object","properties":{"provider":{"type":"string"},"model":{"type":"string"},"api":{"type":"string","enum":["chat","responses"]},"messages":{"type":"array","items":{"type":"object","properties":{"role":{"type":"string"},"content":{"type":"string"}},"required":["role","content"],"additionalProperties":false}}},"required":["messages"],"additionalProperties":false})";
    }
    if (name == "ainiux_run" || name == "ainiux_plan") {
        return R"({"type":"object","properties":{"provider":{"type":"string"},"model":{"type":"string"},"api":{"type":"string","enum":["chat","responses"]},"goal":{"type":"string"}},"required":["goal"],"additionalProperties":false})";
    }
    if (name == "ainiux_image") {
        return R"({"type":"object","properties":{"provider":{"type":"string"},"model":{"type":"string"},"api":{"type":"string","enum":["chat","responses"]},"prompt":{"type":"string"},"size":{"type":"string"},"aspect":{"type":"string"},"quality":{"type":"string"},"format":{"type":"string"}},"required":["prompt"],"additionalProperties":false})";
    }
    if (name == "ainiux_job_get" || name == "ainiux_job_cancel") {
        return R"({"type":"object","properties":{"job_id":{"type":"string","description":"Opaque job handle returned by Ainiux"}},"required":["job_id"],"additionalProperties":false})";
    }
    return nullptr;
}

}  // namespace

struct McpAdapter::Impl {
    JobService* jobs = nullptr;
    const std::size_t max_tasks;
    mutable std::mutex mutex;
    std::map<std::string, TaskEntry> tasks;
    std::uint64_t sequence = 0;

    Impl(JobService* service, std::size_t limit)
        : jobs(service), max_tasks(limit == 0 ? 1U : limit) {}

    std::string new_task_id_locked() {
        std::string entropy;
        entropy.reserve(32U);
        try {
            std::random_device random;
            for (int i = 0; i < 8; ++i) {
                std::ostringstream part;
                part << std::hex << random();
                entropy += part.str();
            }
        } catch (...) {
            // Keep request handling alive on platforms without a usable
            // random-device backend. The normal path remains high entropy;
            // the fallback is still unique within this adapter instance.
            entropy = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                      "_" + std::to_string(reinterpret_cast<std::uintptr_t>(this));
        }
        do {
            ++sequence;
            const std::string candidate = "mcp_task_" + entropy + "_" + std::to_string(sequence);
            if (tasks.count(candidate) == 0) return candidate;
        } while (true);
    }

    void prune_locked() {
        while (tasks.size() >= max_tasks) {
            auto found = tasks.end();
            for (auto it = tasks.begin(); it != tasks.end(); ++it) {
                if (it->second.job->terminal()) {
                    found = it;
                    break;
                }
            }
            if (found == tasks.end()) return;
            tasks.erase(found);
        }
    }

    std::string add_task(const std::shared_ptr<Job>& job) {
        std::lock_guard<std::mutex> lock(mutex);
        prune_locked();
        if (tasks.size() >= max_tasks) return {};
        const std::string id = new_task_id_locked();
        tasks.emplace(id, TaskEntry{id, job});
        return id;
    }

    std::shared_ptr<Job> find_task(const std::string& id) const {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = tasks.find(id);
        return found == tasks.end() ? std::shared_ptr<Job>{} : found->second.job;
    }

    std::string task_json(const std::string& task_id, const std::shared_ptr<Job>& job) const {
        const json::ParseResult parsed = json::parse(job->snapshot_json());
        if (!parsed.error.ok() || !parsed.value.is_object()) return "{}";
        const json::Value* state = parsed.value.get("state");
        const std::string state_name = state != nullptr && state->is_string() ? state->string : "queued";
        const std::string status = state_name == "succeeded" ? "completed" :
                                   state_name == "failed" ? "failed" :
                                   state_name == "cancelled" ? "cancelled" : "working";
        std::string result = "{\"taskId\":" + json::quote(task_id) +
                             ",\"status\":" + json::quote(status) +
                             ",\"createdAt\":" + json::quote(parsed.value.get("created_at")->string) +
                             ",\"lastUpdatedAt\":" + json::quote(parsed.value.get("updated_at")->string) +
                             ",\"ttlMs\":null,\"pollIntervalMs\":250";
        if (status == "completed") {
            result += ",\"result\":" + json_object_or_empty(parsed.value.get("result"));
        } else if (status == "failed") {
            result += ",\"error\":" + json_object_or_empty(parsed.value.get("error"));
        }
        return result + "}";
    }
};

McpAdapter::McpAdapter(JobService* jobs, std::size_t max_tasks)
    : impl_(std::make_unique<Impl>(jobs, max_tasks)) {}

McpAdapter::~McpAdapter() = default;

McpResponse McpAdapter::handle(const http::Request& request) {
    if (request.method != "POST") {
        return {405, "application/json; charset=utf-8", "", true, "POST"};
    }
    if (!json_content_type(request)) return invalid_transport(kInvalidRequest, "MCP requests require Content-Type: application/json");
    const auto accept = request.headers.find("accept");
    if (accept == request.headers.end() ||
        !accepts(accept->second, "application/json") ||
        !accepts(accept->second, "text/event-stream")) {
        return invalid_transport(kInvalidRequest, "MCP requests must accept application/json and text/event-stream");
    }
    const json::ParseResult parsed = json::parse(request.body);
    if (!parsed.error.ok()) return invalid_transport(kParseError, "invalid JSON-RPC message");
    if (!parsed.value.is_object()) return invalid_transport(kInvalidRequest, "MCP JSON-RPC message must be an object");
    const json::Value* id = parsed.value.get("id");
    const std::string id_text = id_json(id);
    const json::Value* method_value = parsed.value.get("method");
    if (method_value == nullptr || !method_value->is_string()) {
        return rpc_error(id_text, kInvalidRequest, "JSON-RPC request method is required");
    }
    const std::string method = method_value->string;
    if (id == nullptr) return {202, "application/json; charset=utf-8", "", true, {}};
    if (!valid_jsonrpc_id(*id)) return rpc_error("null", kInvalidRequest, "JSON-RPC id must be a string or number");

    const auto version = request.headers.find("mcp-protocol-version");
    if (version == request.headers.end()) {
        return rpc_error(id_text, kHeaderMismatch, "MCP-Protocol-Version header is required");
    }
    if (version->second != kProtocolVersion) {
        return rpc_error(id_text, kUnsupportedVersion, "unsupported MCP protocol version",
                         "{\"supported\":[\"2026-07-28\"],\"requested\":" + json::quote(version->second) + "}");
    }
    const auto mcp_method = request.headers.find("mcp-method");
    if (mcp_method == request.headers.end() || mcp_method->second != method) {
        return rpc_error(id_text, kHeaderMismatch, "Mcp-Method does not match the JSON-RPC method");
    }
    const json::Value* params = parsed.value.get("params");
    if (params == nullptr || !params->is_object()) return rpc_error(id_text, kInvalidParams, "params must be an object");
    const json::Value* meta = params->get("_meta");
    const json::Value* protocol = meta == nullptr ? nullptr : meta->get("io.modelcontextprotocol/protocolVersion");
    const json::Value* client_info = meta == nullptr ? nullptr : meta->get("io.modelcontextprotocol/clientInfo");
    const json::Value* client_caps = meta == nullptr ? nullptr : meta->get("io.modelcontextprotocol/clientCapabilities");
    const json::Value* client_name = client_info == nullptr ? nullptr : client_info->get("name");
    const json::Value* client_version = client_info == nullptr ? nullptr : client_info->get("version");
    if (meta == nullptr || !meta->is_object() || protocol == nullptr || !protocol->is_string() ||
        client_info == nullptr || !client_info->is_object() ||
        client_name == nullptr || !client_name->is_string() || client_version == nullptr ||
        !client_version->is_string() || client_caps == nullptr || !client_caps->is_object()) {
        return rpc_error(id_text, kInvalidRequest, "MCP request metadata is incomplete");
    }
    if (protocol->string != kProtocolVersion) {
        return rpc_error(id_text, kHeaderMismatch, "MCP protocol version header does not match request metadata");
    }
    const bool needs_name = method == "tools/call" || method == "tasks/get" ||
                            method == "tasks/update" || method == "tasks/cancel";
    if (needs_name) {
        const auto name_header = request.headers.find("mcp-name");
        const char* field = method == "tools/call" ? "name" : "taskId";
        const json::Value* name = params->get(field);
        std::string decoded;
        if (name == nullptr || !name->is_string() || name_header == request.headers.end() ||
            !decode_header_value(name_header->second, decoded) || decoded != name->string) {
            return rpc_error(id_text, kHeaderMismatch, "Mcp-Name does not match the request parameter");
        }
    }

    if (method == "server/discover") {
        return rpc_response(id_text,
            "{\"resultType\":\"complete\",\"supportedVersions\":[\"2026-07-28\"],\"capabilities\":{\"tools\":{},\"extensions\":{\"io.modelcontextprotocol/tasks\":{}}},\"_meta\":{\"io.modelcontextprotocol/serverInfo\":{\"name\":\"ainiux\",\"version\":\"" + std::string(versionNumber) + "\"}},\"instructions\":\"Use Ainiux tools for bounded asynchronous chat, agent, plan, image, and job control operations.\",\"ttlMs\":3600000,\"cacheScope\":\"public\"}");
    }
    if (method == "tools/list") {
        const std::string names[] = {"ainiux_chat", "ainiux_image", "ainiux_job_cancel", "ainiux_job_get", "ainiux_plan", "ainiux_run"};
        std::string tools = "[";
        for (const std::string& name : names) {
            if (tools.size() > 1U) tools += ',';
            const std::string title = name == "ainiux_chat" ? "Ainiux chat job" :
                                      name == "ainiux_image" ? "Ainiux image job" :
                                      name == "ainiux_job_cancel" ? "Cancel an Ainiux job" :
                                      name == "ainiux_job_get" ? "Inspect an Ainiux job" :
                                      name == "ainiux_plan" ? "Ainiux planning job" : "Ainiux agent job";
            tools += "{\"name\":" + json::quote(name) + ",\"title\":" + json::quote(title) +
                     ",\"description\":" + json::quote("Submit or inspect a bounded Ainiux control job") +
                     ",\"inputSchema\":" + tool_schema(name);
            if (name == "ainiux_run" || name == "ainiux_plan") tools += ",\"annotations\":{\"destructiveHint\":true}";
            tools += '}';
        }
        tools += "]";
        return rpc_response(id_text, "{\"resultType\":\"complete\",\"tools\":" + tools +
                            ",\"ttlMs\":300000,\"cacheScope\":\"public\"}");
    }
    if (method == "tools/call") {
        const json::Value* name = params->get("name");
        const json::Value* args = params->get("arguments");
        if (name == nullptr || !name->is_string() || name->string.size() > kMaxToolNameBytes ||
            (args != nullptr && !args->is_object())) return rpc_error(id_text, kInvalidParams, "tools/call requires an object arguments value");
        const std::string tool = name->string;
        const char* schema = tool_schema(tool);
        if (schema == nullptr) return rpc_response(id_text, error_result("unknown Ainiux tool: " + tool));
        const json::Value empty_args = json::Value{};
        const json::Value& arguments = args == nullptr ? empty_args : *args;
        if (tool == "ainiux_job_get" || tool == "ainiux_job_cancel") {
            const json::Value* job_id = arguments.get("job_id");
            if (job_id == nullptr || !job_id->is_string() || job_id->string.empty()) {
                return rpc_response(id_text, error_result("job_id must be a non-empty opaque job handle"));
            }
            const std::shared_ptr<Job> job = impl_->find_task(job_id->string);
            if (!job) return rpc_response(id_text, error_result("the job handle is unknown or expired"));
            if (tool == "ainiux_job_cancel") {
                std::shared_ptr<Job> cancelled;
                (void)impl_->jobs->registry().cancel(job->id, cancelled);
            }
            const std::string snapshot = job->snapshot_json();
            return rpc_response(id_text, text_result(snapshot, snapshot));
        }
        const std::string operation = tool == "ainiux_chat" ? "chat" :
                                      tool == "ainiux_run" ? "run" :
                                      tool == "ainiux_plan" ? "plan" : "image";
        const ServiceSubmitResult submitted = impl_->jobs->submit(operation, json::stringify(arguments), "");
        const std::string submit_error = job_submission_message(submitted);
        if (!submit_error.empty()) return rpc_response(id_text, error_result(submit_error));
        const std::shared_ptr<Job> job = submitted.submission.job;
        if (task_extension_requested(*meta)) {
            const std::string task_id = impl_->add_task(job);
            if (task_id.empty()) {
                std::shared_ptr<Job> ignored;
                (void)impl_->jobs->registry().cancel(job->id, ignored);
                return rpc_response(id_text, error_result("MCP task retention is full; retry after a task completes"));
            }
            const json::ParseResult snapshot = json::parse(job->snapshot_json());
            if (!snapshot.error.ok() || !snapshot.value.is_object()) {
                return rpc_response(id_text, error_result("the submitted job snapshot could not be encoded"));
            }
            return rpc_response(id_text, "{\"resultType\":\"task\",\"taskId\":" + json::quote(task_id) +
                                ",\"status\":\"working\",\"createdAt\":" +
                                json::quote(snapshot.value.get("created_at")->string) +
                                ",\"lastUpdatedAt\":" +
                                json::quote(snapshot.value.get("updated_at")->string) +
                                ",\"ttlMs\":null,\"pollIntervalMs\":250}");
        }
        return rpc_response(id_text, text_result(job->snapshot_json(), job->snapshot_json()));
    }
    if (method == "tasks/get" || method == "tasks/update" || method == "tasks/cancel") {
        const json::Value* task_id = params->get("taskId");
        const std::shared_ptr<Job> job = task_id == nullptr || !task_id->is_string()
                                             ? std::shared_ptr<Job>{} : impl_->find_task(task_id->string);
        if (!job) return rpc_error(id_text, kTaskNotFound, "the MCP task is unknown or expired");
        if (method == "tasks/cancel") {
            std::shared_ptr<Job> ignored;
            (void)impl_->jobs->registry().cancel(job->id, ignored);
            return rpc_response(id_text, "{\"resultType\":\"complete\"}");
        }
        if (method == "tasks/update") return rpc_response(id_text, "{\"resultType\":\"complete\"}");
        return rpc_response(id_text, impl_->task_json(task_id->string, job));
    }
    return rpc_error(id_text, kMethodNotFound, "MCP method is not implemented");
}

}  // namespace ainiux::server
