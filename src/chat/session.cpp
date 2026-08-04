#include "chat/session.hpp"

#include "common.hpp"
#include "platform/filesystem.hpp"

#include <chrono>
#include <ctime>
#include <sstream>

#include "chat/settings.hpp"
#include "json/json.hpp"

namespace ainiux::chat {

namespace {

std::string required_string(const json::Value& root, const std::string& key, Error& error) {
    const json::Value* value = root.get(key);
    if (value == nullptr || !value->is_string()) {
        error = {ErrorCode::ProviderSchema, "chat file field is missing or not a string: " + key};
        return "";
    }
    return value->string;
}

std::string optional_raw_json(const json::Value& root, const std::string& key) {
    const json::Value* value = root.get(key);
    return value == nullptr ? "{}" : json::stringify(*value);
}

}  // namespace

std::string current_timestamp_utc() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now);
#elif defined(_POSIX_THREAD_SAFE_FUNCTIONS)
    gmtime_r(&now, &tm);
#else
    std::tm* tmp = std::gmtime(&now);
    if (tmp != nullptr) {
        tm = *tmp;
    }
#endif
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

bool session_has_chat_messages(const Session& session) {
    for (const provider::Message& message : session.messages) {
        if (message.role == "user" || message.role == "assistant") {
            return true;
        }
    }
    return false;
}

Session new_session(const provider::RequestContext& context) {
    Session session;
    session.created_at = current_timestamp_utc();
    session.updated_at = session.created_at;
    session.provider = context.profile.name;
    session.base_url = context.base_url;
    session.model = context.options.model;
    session.settings_json = settings_json_from_options(context.options);
    return session;
}

std::string session_to_json(const Session& session) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema_version\": " << session.schema_version << ",\n";
    out << "  \"created_at\": " << json::quote(session.created_at) << ",\n";
    out << "  \"updated_at\": " << json::quote(session.updated_at) << ",\n";
    out << "  \"provider\": " << json::quote(session.provider) << ",\n";
    out << "  \"base_url\": " << json::quote(session.base_url) << ",\n";
    out << "  \"model\": " << json::quote(session.model) << ",\n";
    out << "  \"read_only\": " << (session.read_only ? "true" : "false") << ",\n";
    out << "  \"read_only_reason\": " << json::quote(session.read_only_reason) << ",\n";
    out << "  \"settings\": " << (session.settings_json.empty() ? "{}" : session.settings_json) << ",\n";
    out << "  \"messages\": [\n";
    for (size_t i = 0; i < session.messages.size(); ++i) {
        const provider::Message& message = session.messages[i];
        out << "    {\"role\": " << json::quote(message.role) << ", \"content\": " << json::quote(message.content) << "}";
        if (i + 1 != session.messages.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ],\n";
    out << "  \"attachments\": [],\n";
    out << "  \"usage\": " << (session.usage_json.empty() ? "{}" : session.usage_json) << ",\n";
    out << "  \"compaction_events\": [";
    for (size_t i = 0; i < session.compaction_events.size(); ++i) {
        const context::CompactionEvent& event = session.compaction_events[i];
        if (i != 0) {
            out << ",";
        }
        out << "{\"timestamp\":" << json::quote(event.timestamp)
            << ",\"policy\":" << json::quote(event.policy)
            << ",\"messages_compacted\":" << event.messages_compacted
            << ",\"original_bytes\":" << event.original_bytes
            << ",\"request_bytes\":" << event.request_bytes
            << ",\"notice\":" << json::quote(event.notice) << "}";
    }
    out << "]\n";
    out << "}\n";
    return out.str();
}

Error load_session(const std::string& path, Session& session) {
    const std::string resolved = expand_user_path(path);
    std::string data;
    Error read_error = platform::read_file_bounded(resolved, 512U * 1024U * 1024U, data);
    if (!read_error.ok()) {
        return {ErrorCode::FileRead,
                "could not open chat file for reading: " + resolved + ": " +
                    read_error.message};
    }
    json::ParseResult parsed = json::parse(data);
    if (!parsed.error.ok()) {
        return parsed.error;
    }
    if (!parsed.value.is_object()) {
        return {ErrorCode::ProviderSchema, "chat file root is not an object: " + resolved};
    }

    Session loaded;
    if (const json::Value* version = parsed.value.get("schema_version")) {
        if (version->type != json::Value::Type::Number || version->number < 1.0) {
            return {ErrorCode::ProviderSchema, "chat file schema_version is invalid: " + resolved};
        }
        loaded.schema_version = static_cast<int>(version->number);
    } else {
        return {ErrorCode::ProviderSchema, "chat file field is missing: schema_version"};
    }

    Error err;
    loaded.created_at = required_string(parsed.value, "created_at", err);
    if (!err.ok()) return err;
    loaded.updated_at = required_string(parsed.value, "updated_at", err);
    if (!err.ok()) return err;
    loaded.provider = required_string(parsed.value, "provider", err);
    if (!err.ok()) return err;
    loaded.base_url = required_string(parsed.value, "base_url", err);
    if (!err.ok()) return err;
    loaded.model = required_string(parsed.value, "model", err);
    if (!err.ok()) return err;
    if (const json::Value* read_only = parsed.value.get("read_only")) {
        if (read_only->type != json::Value::Type::Bool) {
            return {ErrorCode::ProviderSchema,
                    "chat file field is not a boolean: read_only"};
        }
        loaded.read_only = read_only->boolean;
    }
    if (const json::Value* reason = parsed.value.get("read_only_reason")) {
        if (!reason->is_string()) {
            return {ErrorCode::ProviderSchema,
                    "chat file field is not a string: read_only_reason"};
        }
        loaded.read_only_reason = reason->string;
    }
    loaded.settings_json = optional_raw_json(parsed.value, "settings");
    loaded.usage_json = optional_raw_json(parsed.value, "usage");

    const json::Value* messages = parsed.value.get("messages");
    if (messages == nullptr || !messages->is_array()) {
        return {ErrorCode::ProviderSchema, "chat file field is missing or not an array: messages"};
    }
    for (const json::Value& item : messages->array) {
        if (!item.is_object()) {
            return {ErrorCode::ProviderSchema, "chat file message is not an object: " + resolved};
        }
        const json::Value* role = item.get("role");
        const json::Value* content = item.get("content");
        if (role == nullptr || !role->is_string() || content == nullptr || !content->is_string()) {
            return {ErrorCode::ProviderSchema, "chat file message requires string role and content: " + resolved};
        }
        if (role->string != "system" && role->string != "user" && role->string != "assistant") {
            return {ErrorCode::ProviderSchema, "chat file message has unsupported role: " + role->string};
        }
        loaded.messages.push_back({role->string, content->string});
    }

    if (const json::Value* events = parsed.value.get("compaction_events")) {
        if (!events->is_array()) {
            return {ErrorCode::ProviderSchema, "chat file compaction_events is not an array: " + resolved};
        }
        for (const json::Value& item : events->array) {
            if (!item.is_object()) {
                return {ErrorCode::ProviderSchema, "chat file compaction event is not an object: " + resolved};
            }
            context::CompactionEvent event;
            const json::Value* timestamp = item.get("timestamp");
            const json::Value* policy = item.get("policy");
            const json::Value* count = item.get("messages_compacted");
            const json::Value* original = item.get("original_bytes");
            const json::Value* request = item.get("request_bytes");
            const json::Value* notice = item.get("notice");
            if (timestamp == nullptr || !timestamp->is_string() || policy == nullptr || !policy->is_string() ||
                count == nullptr || count->type != json::Value::Type::Number ||
                original == nullptr || original->type != json::Value::Type::Number ||
                request == nullptr || request->type != json::Value::Type::Number ||
                notice == nullptr || !notice->is_string() || count->number < 0.0 ||
                original->number < 0.0 || request->number < 0.0) {
                return {ErrorCode::ProviderSchema, "chat file compaction event fields are invalid: " + resolved};
            }
            event.timestamp = timestamp->string;
            event.policy = policy->string;
            event.messages_compacted = static_cast<size_t>(count->number);
            event.original_bytes = static_cast<size_t>(original->number);
            event.request_bytes = static_cast<size_t>(request->number);
            event.notice = notice->string;
            loaded.compaction_events.push_back(std::move(event));
        }
    }
    session = std::move(loaded);
    return ok_error();
}

Error save_session_atomic(const std::string& path, Session session) {
    const std::string resolved = expand_user_path(path);
    session.updated_at = current_timestamp_utc();
    const std::string data = session_to_json(session);
    return platform::atomic_write_private(resolved, data, true);
}

}  // namespace ainiux::chat
