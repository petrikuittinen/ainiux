#include "server/chat_service.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "json/json.hpp"
#include "provider/provider.hpp"
#include "chat/settings.hpp"
#include "server/model_settings.hpp"

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

std::string session_json(const chat::Session& session, cli::Options options = {}) {
    (void)chat::apply_settings_json(options, session.settings_json);
    options.provider = session.provider;
    options.model = session.model;
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
           ",\"settings\":" + public_model_settings(options) +
           ",\"settings_fields\":" + public_model_fields(options) +
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
    if (!known_fields(parsed.value, {"revision", "name", "provider", "model", "settings"}, unknown)) {
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
                   std::vector<provider::Message>& messages,
                   std::optional<std::string>& provider_name,
                   std::optional<std::string>& model) {
    const json::ParseResult parsed = json::parse(input);
    if (!parsed.error.ok() || !parsed.value.is_object()) {
        return invalid("message append body must be one JSON object");
    }
    std::string unknown;
    if (!known_fields(parsed.value, {"revision", "messages", "provider", "model"}, unknown)) {
        return invalid("unknown message append field: " + unknown);
    }
    if (!integer_value(parsed.value.get("revision"), revision) || revision <= 0) {
        return invalid("revision must be a positive integer");
    }
    for (const auto& field : {std::pair<const char*, std::optional<std::string>*>(
                                  "provider", &provider_name),
                              std::pair<const char*, std::optional<std::string>*>(
                                  "model", &model)}) {
        const json::Value* value = parsed.value.get(field.first);
        if (value == nullptr) continue;
        if (!value->is_string()) return invalid(std::string(field.first) + " must be a string");
        if (value->string.size() > kMaxMetadataBytes)
            return invalid(std::string(field.first) + " is too long");
        *field.second = value->string;
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

Error parse_revision(const std::string& input, long long& revision,
                     const char* operation = "rewind") {
    const json::ParseResult parsed = json::parse(input);
    if (!parsed.error.ok() || !parsed.value.is_object()) {
        return invalid(std::string("chat ") + operation + " body must be one JSON object");
    }
    std::string unknown;
    if (!known_fields(parsed.value, {"revision"}, unknown)) {
        return invalid(std::string("unknown chat ") + operation + " field: " + unknown);
    }
    if (!integer_value(parsed.value.get("revision"), revision) || revision <= 0) {
        return invalid("revision must be a positive integer");
    }
    return ok_error();
}

Error parse_keep_id(const std::string& input, long long& keep_id) {
    keep_id = 0;
    const json::ParseResult parsed = json::parse(input);
    if (!parsed.error.ok() || !parsed.value.is_object()) {
        return invalid("empty-thread cleanup body must be one JSON object");
    }
    std::string unknown;
    if (!known_fields(parsed.value, {"keep_id"}, unknown)) {
        return invalid("unknown empty-thread cleanup field: " + unknown);
    }
    if (parsed.value.get("keep_id") == nullptr) return ok_error();
    if (!integer_value(parsed.value.get("keep_id"), keep_id) || keep_id < 0) {
        return invalid("keep_id must be a non-negative integer");
    }
    return ok_error();
}

Error parse_message_mutation(const std::string& input,
                             long long& revision,
                             long long& ordinal,
                             std::string* content,
                             const char* operation) {
    const json::ParseResult parsed = json::parse(input);
    if (!parsed.error.ok() || !parsed.value.is_object()) {
        return invalid(std::string("chat ") + operation + " body must be one JSON object");
    }
    std::string unknown;
    std::vector<std::string> allowed{"revision", "ordinal"};
    if (content != nullptr) allowed.emplace_back("content");
    if (!known_fields(parsed.value, allowed, unknown)) {
        return invalid(std::string("unknown chat ") + operation + " field: " + unknown);
    }
    if (!integer_value(parsed.value.get("revision"), revision) || revision <= 0) {
        return invalid("revision must be a positive integer");
    }
    if (!integer_value(parsed.value.get("ordinal"), ordinal) || ordinal < 0) {
        return invalid("ordinal must be a non-negative integer");
    }
    if (content == nullptr) return ok_error();
    const json::Value* value = parsed.value.get("content");
    if (value == nullptr || !value->is_string() || value->string.size() > kMaxMessageBytes) {
        return invalid("message content must be a string no larger than 1 MiB");
    }
    *content = value->string;
    return ok_error();
}

Error load_unbounded_session(chat::SqliteStore& store,
                             long long thread_id,
                             long long expected_revision,
                             chat::Session& session,
                             long long& current_revision) {
    chat::LoadSessionOptions options;
    options.update_last_thread = false;
    Error error = store.load_session(thread_id, session, options);
    if (!error.ok()) return error;
    current_revision = session.revision;
    if (session.revision != expected_revision) {
        return {ErrorCode::FileLock, "chat thread revision is stale"};
    }
    if (session.read_only) {
        return {ErrorCode::FileWrite, "chat thread is read-only"};
    }
    if (session.messages_truncated) {
        return invalid("chat thread transcript is truncated");
    }
    return ok_error();
}

std::size_t message_index_for_ordinal(const chat::Session& session, long long ordinal) {
    const long long message_count = session.persisted_message_count > 0
                                        ? session.persisted_message_count
                                        : static_cast<long long>(session.messages.size());
    const long long first_ordinal =
        std::max(0LL, message_count - static_cast<long long>(session.messages.size()));
    const long long index = ordinal - first_ordinal;
    if (index < 0 || static_cast<std::size_t>(index) >= session.messages.size()) {
        return session.messages.size();
    }
    return static_cast<std::size_t>(index);
}

}  // namespace

ChatService::ChatService(std::string database_path, cli::Options defaults)
    : database_path_(std::move(database_path)), defaults_(std::move(defaults)) {}

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
    body = "{\"thread\":" + session_json(session, defaults_) + "}";
    return ok_error();
}

Error ChatService::create(const std::string& request_body, std::string& body) {
    chat::Session session;
    Error error = parse_create(request_body, session);
    if (!error.ok()) return error;
    cli::Options options = defaults_;
    error = apply_public_model_target(json::parse(request_body).value, options);
    if (!error.ok()) return error;
    session.provider = options.provider;
    session.model = options.model;
    session.base_url = options.base_url;
    session.settings_json = chat::settings_json_from_options(options);
    std::lock_guard<std::mutex> lock(mutex_);
    body.clear();
    error = ensure_open();
    if (!error.ok()) return error;
    error = store_.save_session(session);
    if (!error.ok()) return safe_store_error(error, "create the chat thread");
    body = "{\"thread\":" + session_json(session, defaults_) + "}";
    return ok_error();
}

Error ChatService::settings(long long thread_id, const std::string& request_body,
                            std::string& body, long long& current_revision) {
    const auto parsed = json::parse(request_body);
    if (!parsed.error.ok() || !parsed.value.is_object()) return invalid("settings body must be an object");
    std::string unknown;
    if (!known_fields(parsed.value, {"revision", "provider", "model", "settings"}, unknown))
        return invalid("unknown settings field: " + unknown);
    long long expected = 0;
    if (!integer_value(parsed.value.get("revision"), expected) || expected <= 0)
        return invalid("revision must be a positive integer");
    std::lock_guard<std::mutex> lock(mutex_);
    Error error = ensure_open();
    if (!error.ok()) return error;
    chat::Session session;
    chat::LoadSessionOptions load;
    load.max_messages = 1; load.max_content_bytes = 1;
    load.metadata_only_attachments = true; load.load_compactions = false; load.update_last_thread = false;
    error = store_.load_session(thread_id, session, load);
    if (!error.ok()) return safe_store_error(error, "load settings");
    current_revision = session.revision;
    if (expected != current_revision) return {ErrorCode::FileLock, "chat thread revision is stale"};
    cli::Options options = defaults_;
    error = chat::apply_settings_json(options, session.settings_json);
    if (!error.ok()) return invalid("stored model settings could not be loaded");
    options.provider = session.provider; options.model = session.model; options.base_url = session.base_url;
    error = apply_public_model_target(parsed.value, options);
    if (!error.ok()) return error;
    session.provider = options.provider; session.model = options.model; session.base_url = options.base_url;
    // Merge known options into the existing object, preserving future/local fields.
    auto stored = json::parse(session.settings_json);
    const auto updated = json::parse(chat::settings_json_from_options(options));
    if (!stored.error.ok() || !stored.value.is_object()) return invalid("stored settings are invalid");
    for (const auto& entry : updated.value.object) stored.value.object[entry.first] = entry.second;
    session.settings_json = json::stringify(stored.value);
    error = store_.update_settings(session, expected, current_revision);
    if (!error.ok()) return safe_store_error(error, "save settings");
    body = "{\"thread\":{\"id\":" + std::to_string(thread_id) +
           ",\"revision\":" + std::to_string(current_revision) +
           ",\"provider\":" + json::quote(session.provider) +
           ",\"model\":" + json::quote(session.model) +
           ",\"settings\":" + public_model_settings(options) +
           ",\"settings_fields\":" + public_model_fields(options) + "}}";
    return ok_error();
}

Error ChatService::append(long long thread_id,
                          const std::string& request_body,
                          std::string& body,
                          long long& current_revision) {
    std::vector<provider::Message> messages;
    std::optional<std::string> provider_name;
    std::optional<std::string> model;
    long long expected_revision = 0;
    Error error = parse_append(request_body, expected_revision, messages,
                               provider_name, model);
    if (!error.ok()) return error;
    std::lock_guard<std::mutex> lock(mutex_);
    body.clear();
    current_revision = 0;
    error = ensure_open();
    if (!error.ok()) return error;
    long long message_count = 0;
    error = store_.append_messages(thread_id, expected_revision, messages,
                                   provider_name, model,
                                   current_revision, message_count);
    if (!error.ok()) return safe_store_error(error, "append chat messages");
    body = "{\"thread\":{\"id\":" + std::to_string(thread_id) +
           ",\"revision\":" + std::to_string(current_revision) +
           ",\"message_count\":" + std::to_string(message_count);
    if (provider_name.has_value()) body += ",\"provider\":" + json::quote(*provider_name);
    if (model.has_value()) body += ",\"model\":" + json::quote(*model);
    body += "}}";
    return ok_error();
}

Error ChatService::abandon(long long thread_id,
                           const std::string& request_body,
                           std::string& body,
                           long long& current_revision) {
    long long expected_revision = 0;
    Error error = parse_revision(request_body, expected_revision, "abandon");
    if (!error.ok()) return error;
    std::lock_guard<std::mutex> lock(mutex_);
    body.clear();
    current_revision = 0;
    error = ensure_open();
    if (!error.ok()) return error;
    bool deleted = false;
    error = store_.abandon_empty_thread(thread_id, expected_revision, deleted,
                                        current_revision);
    if (!error.ok()) return safe_store_error(error, "abandon the chat thread");
    body = "{\"id\":" + std::to_string(thread_id) +
           ",\"deleted\":" + std::string(deleted ? "true" : "false");
    if (!deleted) body += ",\"reason\":\"not_empty\"";
    body += "}";
    return ok_error();
}

Error ChatService::cleanup_empty(const std::string& request_body, std::string& body) {
    long long keep_id = 0;
    Error error = parse_keep_id(request_body, keep_id);
    if (!error.ok()) return error;
    std::lock_guard<std::mutex> lock(mutex_);
    body.clear();
    error = ensure_open();
    if (!error.ok()) return error;
    long long deleted_count = 0;
    bool watch_deleted = false;
    error = store_.soft_delete_empty_threads(deleted_count, 0, watch_deleted, keep_id);
    if (!error.ok()) return safe_store_error(error, "clean up empty chat threads");
    body = "{\"deleted_count\":" + std::to_string(deleted_count) + "}";
    return ok_error();
}

Error ChatService::remove(long long thread_id,
                          const std::string& request_body,
                          std::string& body,
                          long long& current_revision) {
    long long expected_revision = 0;
    Error error = parse_revision(request_body, expected_revision, "delete");
    if (!error.ok()) return error;
    std::lock_guard<std::mutex> lock(mutex_);
    body.clear();
    current_revision = 0;
    error = ensure_open();
    if (!error.ok()) return error;
    error = store_.remove_thread(thread_id, expected_revision, current_revision);
    if (!error.ok()) return safe_store_error(error, "delete the chat thread");
    body = "{\"id\":" + std::to_string(thread_id) + ",\"deleted\":true}";
    return ok_error();
}

Error ChatService::edit_message(long long thread_id,
                                const std::string& request_body,
                                std::string& body,
                                long long& current_revision) {
    long long expected_revision = 0;
    long long ordinal = -1;
    std::string content;
    Error error = parse_message_mutation(request_body, expected_revision, ordinal, &content,
                                         "edit");
    if (!error.ok()) return error;
    std::lock_guard<std::mutex> lock(mutex_);
    body.clear();
    current_revision = 0;
    error = ensure_open();
    if (!error.ok()) return error;
    chat::Session session;
    error = load_unbounded_session(store_, thread_id, expected_revision, session,
                                   current_revision);
    if (!error.ok()) return error.code == ErrorCode::BadArgs
                               ? error
                               : safe_store_error(error, "load the chat thread");
    const std::size_t index = message_index_for_ordinal(session, ordinal);
    if (index >= session.messages.size()) {
        return invalid("chat message was not found");
    }
    if (session.messages[index].role != "assistant") {
        return invalid("only assistant messages can be edited");
    }
    session.messages[index].content = std::move(content);
    error = store_.save_session(session);
    if (!error.ok()) return safe_store_error(error, "edit the chat message");
    current_revision = session.revision;
    body = "{\"thread\":" + session_json(session, defaults_) + "}";
    return ok_error();
}

Error ChatService::delete_message(long long thread_id,
                                  const std::string& request_body,
                                  std::string& body,
                                  long long& current_revision) {
    long long expected_revision = 0;
    long long ordinal = -1;
    Error error = parse_message_mutation(request_body, expected_revision, ordinal, nullptr,
                                         "delete");
    if (!error.ok()) return error;
    std::lock_guard<std::mutex> lock(mutex_);
    body.clear();
    current_revision = 0;
    error = ensure_open();
    if (!error.ok()) return error;
    chat::Session session;
    error = load_unbounded_session(store_, thread_id, expected_revision, session,
                                   current_revision);
    if (!error.ok()) return error.code == ErrorCode::BadArgs
                               ? error
                               : safe_store_error(error, "load the chat thread");
    const std::size_t index = message_index_for_ordinal(session, ordinal);
    if (index >= session.messages.size()) {
        return invalid("chat message was not found");
    }
    if (session.messages[index].role != "user" &&
        session.messages[index].role != "assistant") {
        return invalid("only user and assistant messages can be deleted");
    }
    session.messages.erase(session.messages.begin() + static_cast<std::ptrdiff_t>(index),
                           session.messages.end());
    error = store_.save_session(session);
    if (!error.ok()) return safe_store_error(error, "delete the chat message");
    current_revision = session.revision;
    body = "{\"thread\":" + session_json(session, defaults_) + "}";
    return ok_error();
}

Error ChatService::rewind_last_answer(long long thread_id,
                                      const std::string& request_body,
                                      std::string& body,
                                      long long& current_revision) {
    long long expected_revision = 0;
    Error error = parse_revision(request_body, expected_revision);
    if (!error.ok()) return error;
    std::lock_guard<std::mutex> lock(mutex_);
    body.clear();
    current_revision = 0;
    error = ensure_open();
    if (!error.ok()) return error;

    chat::Session session;
    chat::LoadSessionOptions options;
    options.update_last_thread = false;
    error = store_.load_session(thread_id, session, options);
    if (!error.ok()) return safe_store_error(error, "load the chat thread");
    current_revision = session.revision;
    if (session.revision != expected_revision) {
        return {ErrorCode::FileLock, "chat thread revision is stale"};
    }
    if (session.read_only) {
        return {ErrorCode::FileWrite, "chat thread is read-only"};
    }

    std::size_t user_index = session.messages.size();
    while (user_index > 0) {
        --user_index;
        if (session.messages[user_index].role == "user") break;
    }
    if (session.messages.empty() || session.messages[user_index].role != "user") {
        return invalid("chat thread has no user prompt to regenerate");
    }
    const std::string prompt = session.messages[user_index].content;
    if (user_index + 1U < session.messages.size()) {
        session.messages.erase(session.messages.begin() +
                                   static_cast<std::ptrdiff_t>(user_index + 1U),
                               session.messages.end());
        error = store_.save_session(session);
        if (!error.ok()) return safe_store_error(error, "rewind the chat thread");
        current_revision = session.revision;
    }
    body = "{\"thread\":{\"id\":" + std::to_string(thread_id) +
           ",\"revision\":" + std::to_string(session.revision) +
           ",\"message_count\":" + std::to_string(session.messages.size()) + "},"
           "\"prompt\":" + json::quote(prompt) + "}";
    return ok_error();
}

}  // namespace ainiux::server
