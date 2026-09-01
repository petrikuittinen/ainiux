#include "server/chat_service.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "json/json.hpp"
#include "provider/provider.hpp"

namespace ainiux::server {
namespace {

constexpr int kListLimit = 200;
constexpr std::size_t kMaxMessagesPerAppend = 64U;
constexpr std::size_t kMaxMessageBytes = 1024U * 1024U;
constexpr std::size_t kMaxThreadNameBytes = 200U;
constexpr std::size_t kMaxMetadataBytes = 256U;
constexpr std::size_t kMaxLoadedMessages = 512U;
constexpr std::size_t kMaxLoadedContentBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaxAttachmentsPerMessage = 64U;

Error invalid(const std::string& message) {
    return {ErrorCode::BadArgs, message};
}

Error safe_store_error(const Error& error, const char* action) {
    if (error.code == ErrorCode::FileRead &&
        error.message.find("thread not found") != std::string::npos) {
        return {ErrorCode::FileRead, "chat thread was not found"};
    }
    if (error.code == ErrorCode::FileLock) {
        return {ErrorCode::FileLock, "chat thread revision is stale"};
    }
    if (error.code == ErrorCode::FileWrite &&
        error.message.find("read-only") != std::string::npos) {
        return {ErrorCode::FileWrite, "chat thread is read-only"};
    }
    return {ErrorCode::Internal, std::string("chat store could not ") + action};
}

bool integer_value(const json::Value* value, long long& output) {
    constexpr double kLargestExactJsonInteger = 9007199254740991.0;
    if (value == nullptr || value->type != json::Value::Type::Number ||
        !std::isfinite(value->number) || std::floor(value->number) != value->number ||
        value->number < -kLargestExactJsonInteger || value->number > kLargestExactJsonInteger) {
        return false;
    }
    output = static_cast<long long>(value->number);
    return true;
}

bool known_fields(const json::Value& object,
                  const std::vector<std::string>& allowed,
                  std::string& unknown) {
    for (const auto& entry : object.object) {
        bool found = false;
        for (const std::string& name : allowed) {
            if (entry.first == name) {
                found = true;
                break;
            }
        }
        if (!found) {
            unknown = entry.first;
            return false;
        }
    }
    return true;
}

Error optional_string(const json::Value& object,
                      const char* name,
                      std::size_t maximum,
                      std::string& output) {
    output.clear();
    const json::Value* value = object.get(name);
    if (value == nullptr) return ok_error();
    if (!value->is_string()) return invalid(std::string(name) + " must be a string");
    if (value->string.size() > maximum) return invalid(std::string(name) + " is too long");
    output = value->string;
    return ok_error();
}

std::string attachment_json(const provider::ImageInput& image) {
    return "{\"kind\":\"image\",\"mime_type\":" + json::quote(image.mime_type) +
           ",\"display_name\":" + json::quote(image.display_name) +
           ",\"byte_size\":" + std::to_string(image.byte_size) + "}";
}

std::string attachment_json(const provider::TextAttachment& attachment) {
    return "{\"kind\":\"text\",\"mime_type\":\"text/markdown\",\"display_name\":" +
           json::quote(attachment.display_name) + ",\"byte_size\":" +
           std::to_string(attachment.byte_size) + "}";
}

std::string messages_json(const std::vector<provider::Message>& messages,
                          long long first_ordinal) {
    std::string output = "[";
    for (std::size_t index = 0; index < messages.size(); ++index) {
        if (index != 0) output += ',';
        const provider::Message& message = messages[index];
        output += "{\"ordinal\":" +
                  std::to_string(first_ordinal + static_cast<long long>(index)) +
                  ",\"role\":" + json::quote(message.role) +
                  ",\"content\":" + json::quote(message.content) +
                  ",\"attachments\":[";
        bool first = true;
        for (const provider::ImageInput& image : message.images) {
            if (!first) output += ',';
            first = false;
            output += attachment_json(image);
        }
        for (const provider::TextAttachment& attachment : message.text_attachments) {
            if (!first) output += ',';
            first = false;
            output += attachment_json(attachment);
        }
        output += "]}";
    }
    output += ']';
    return output;
}

std::string summary_json(const chat::ThreadSummary& thread) {
    return "{\"id\":" + std::to_string(thread.id) +
           ",\"revision\":" + std::to_string(thread.revision) +
           ",\"name\":" + json::quote(thread.name) +
           ",\"created_at\":" + json::quote(thread.created_at) +
           ",\"modified_at\":" + json::quote(thread.modified_at) +
           ",\"provider\":" + json::quote(thread.last_provider) +
           ",\"model\":" + json::quote(thread.last_model) +
           ",\"message_count\":" + std::to_string(thread.message_count) +
           ",\"read_only\":" + std::string(thread.read_only ? "true" : "false") + "}";
}

std::string session_json(const chat::Session& session) {
    const long long message_count = session.persisted_message_count > 0
                                        ? session.persisted_message_count
                                        : static_cast<long long>(session.messages.size());
    const long long first_ordinal =
        std::max(0LL, message_count - static_cast<long long>(session.messages.size()));
    return "{\"id\":" + std::to_string(session.thread_id) +
           ",\"revision\":" + std::to_string(session.revision) +
           ",\"name\":" + json::quote(session.name) +
           ",\"created_at\":" + json::quote(session.created_at) +
           ",\"modified_at\":" + json::quote(session.updated_at) +
           ",\"provider\":" + json::quote(session.provider) +
           ",\"model\":" + json::quote(session.model) +
           ",\"message_count\":" + std::to_string(message_count) +
           ",\"read_only\":" + std::string(session.read_only ? "true" : "false") +
           ",\"messages\":" + messages_json(session.messages, first_ordinal) +
           ",\"messages_truncated\":" +
           std::string(session.messages_truncated ? "true" : "false") +
           ",\"attachments_truncated\":" +
           std::string(session.attachments_truncated ? "true" : "false") + "}";
}

Error parse_create(const std::string& input, chat::Session& session) {
    const json::ParseResult parsed = json::parse(input);
    if (!parsed.error.ok() || !parsed.value.is_object()) {
        return invalid("thread creation body must be one JSON object");
    }
    std::string unknown;
    if (!known_fields(parsed.value, {"revision", "name", "provider", "model"}, unknown)) {
        return invalid("unknown thread creation field: " + unknown);
    }
    long long revision = -1;
    if (!integer_value(parsed.value.get("revision"), revision) || revision != 0) {
        return invalid("new thread revision must be 0");
    }
    Error error = optional_string(parsed.value, "name", kMaxThreadNameBytes, session.name);
    if (!error.ok()) return error;
    error = optional_string(parsed.value, "provider", kMaxMetadataBytes, session.provider);
    if (!error.ok()) return error;
    return optional_string(parsed.value, "model", kMaxMetadataBytes, session.model);
}

Error parse_append(const std::string& input,
                   long long& revision,
                   std::vector<provider::Message>& messages) {
    const json::ParseResult parsed = json::parse(input);
    if (!parsed.error.ok() || !parsed.value.is_object()) {
        return invalid("message append body must be one JSON object");
    }
    std::string unknown;
    if (!known_fields(parsed.value, {"revision", "messages"}, unknown)) {
        return invalid("unknown message append field: " + unknown);
    }
    if (!integer_value(parsed.value.get("revision"), revision) || revision <= 0) {
        return invalid("revision must be a positive integer");
    }
    const json::Value* array = parsed.value.get("messages");
    if (array == nullptr || !array->is_array() || array->array.empty() ||
        array->array.size() > kMaxMessagesPerAppend) {
        return invalid("messages must contain 1 through 64 items");
    }
    messages.clear();
    messages.reserve(array->array.size());
    for (const json::Value& value : array->array) {
        if (!value.is_object()) return invalid("each message must be an object");
        if (!known_fields(value, {"role", "content"}, unknown)) {
            return invalid("unknown message field: " + unknown);
        }
        const json::Value* role = value.get("role");
        const json::Value* content = value.get("content");
        if (role == nullptr || !role->is_string() ||
            (role->string != "system" && role->string != "user" &&
             role->string != "assistant")) {
            return invalid("message role must be system, user, or assistant");
        }
        if (content == nullptr || !content->is_string() || content->string.size() > kMaxMessageBytes) {
            return invalid("message content must be a string no larger than 1 MiB");
        }
        messages.push_back({role->string, content->string});
    }
    return ok_error();
}

}  // namespace

ChatService::ChatService(std::string database_path)
    : database_path_(std::move(database_path)) {}

Error ChatService::ensure_open() {
    if (store_.is_open()) return ok_error();
    const Error error = database_path_.empty() ? store_.open_default() : store_.open(database_path_);
    return error.ok() ? error : safe_store_error(error, "open the chat library");
}

Error ChatService::list(std::string& body) {
    std::lock_guard<std::mutex> lock(mutex_);
    body.clear();
    Error error = ensure_open();
    if (!error.ok()) return error;
    std::vector<chat::ThreadSummary> threads;
    error = store_.list_threads(threads, kListLimit + 1);
    if (!error.ok()) return safe_store_error(error, "list chat threads");
    const bool truncated = threads.size() > static_cast<std::size_t>(kListLimit);
    if (truncated) threads.resize(kListLimit);
    body = "{\"threads\":[";
    for (std::size_t index = 0; index < threads.size(); ++index) {
        if (index != 0) body += ',';
        body += summary_json(threads[index]);
    }
    body += "],\"truncated\":" + std::string(truncated ? "true" : "false") + "}";
    return ok_error();
}

Error ChatService::load(long long thread_id, std::string& body) {
    std::lock_guard<std::mutex> lock(mutex_);
    body.clear();
    Error error = ensure_open();
    if (!error.ok()) return error;
    chat::Session session;
    chat::LoadSessionOptions options;
    options.max_messages = kMaxLoadedMessages;
    options.max_content_bytes = kMaxLoadedContentBytes;
    options.max_attachments_per_message = kMaxAttachmentsPerMessage;
    options.metadata_only_attachments = true;
    options.load_compactions = false;
    options.update_last_thread = false;
    error = store_.load_session(thread_id, session, options);
    if (!error.ok()) return safe_store_error(error, "load the chat thread");
    body = "{\"thread\":" + session_json(session) + "}";
    return ok_error();
}

Error ChatService::create(const std::string& request_body, std::string& body) {
    chat::Session session;
    Error error = parse_create(request_body, session);
    if (!error.ok()) return error;
    std::lock_guard<std::mutex> lock(mutex_);
    body.clear();
    error = ensure_open();
    if (!error.ok()) return error;
    error = store_.save_session(session);
    if (!error.ok()) return safe_store_error(error, "create the chat thread");
    body = "{\"thread\":" + session_json(session) + "}";
    return ok_error();
}

Error ChatService::append(long long thread_id,
                          const std::string& request_body,
                          std::string& body,
                          long long& current_revision) {
    std::vector<provider::Message> messages;
    long long expected_revision = 0;
    Error error = parse_append(request_body, expected_revision, messages);
    if (!error.ok()) return error;
    std::lock_guard<std::mutex> lock(mutex_);
    body.clear();
    current_revision = 0;
    error = ensure_open();
    if (!error.ok()) return error;
    long long message_count = 0;
    error = store_.append_messages(thread_id, expected_revision, messages,
                                   current_revision, message_count);
    if (!error.ok()) return safe_store_error(error, "append chat messages");
    body = "{\"thread\":{\"id\":" + std::to_string(thread_id) +
           ",\"revision\":" + std::to_string(current_revision) +
           ",\"message_count\":" + std::to_string(message_count) + "}}";
    return ok_error();
}

}  // namespace ainiux::server
