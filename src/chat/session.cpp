#include "chat/session.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "json/json.hpp"

namespace pkchat::chat {

namespace {

class Fd {
   public:
    explicit Fd(int fd = -1) : fd_(fd) {}
    ~Fd() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return fd_; }
    int release() {
        int out = fd_;
        fd_ = -1;
        return out;
    }

   private:
    int fd_;
};

std::string errno_message(const std::string& action, const std::string& path) {
    return action + ": " + path + ": " + std::strerror(errno);
}

std::string dirname_of(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return ".";
    }
    if (slash == 0) {
        return "/";
    }
    return path.substr(0, slash);
}

Error write_all(int fd, const std::string& data, const std::string& path) {
    const char* ptr = data.data();
    size_t remaining = data.size();
    while (remaining > 0) {
        const ssize_t written = write(fd, ptr, remaining);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return {ErrorCode::FileWrite, errno_message("could not write chat file", path)};
        }
        if (written == 0) {
            return {ErrorCode::FileWrite, "could not write chat file: " + path + ": short write"};
        }
        ptr += written;
        remaining -= static_cast<size_t>(written);
    }
    return ok_error();
}

std::string required_string(const json::Value& root, const std::string& key, Error& error) {
    const json::Value* value = root.get(key);
    if (value == nullptr || !value->is_string()) {
        error = {ErrorCode::ProviderSchema, "chat file field is missing or not a string: " + key};
        return "";
    }
    return value->string;
}

std::string value_to_json(const json::Value& value) {
    std::ostringstream out;
    switch (value.type) {
        case json::Value::Type::Null:
            return "null";
        case json::Value::Type::Bool:
            return value.boolean ? "true" : "false";
        case json::Value::Type::Number:
            out << std::setprecision(17) << value.number;
            return out.str();
        case json::Value::Type::String:
            return json::quote(value.string);
        case json::Value::Type::Array:
            out << "[";
            for (size_t i = 0; i < value.array.size(); ++i) {
                if (i != 0) {
                    out << ",";
                }
                out << value_to_json(value.array[i]);
            }
            out << "]";
            return out.str();
        case json::Value::Type::Object:
            out << "{";
            for (auto it = value.object.begin(); it != value.object.end(); ++it) {
                if (it != value.object.begin()) {
                    out << ",";
                }
                out << json::quote(it->first) << ":" << value_to_json(it->second);
            }
            out << "}";
            return out.str();
    }
    return "null";
}

std::string optional_raw_json(const json::Value& root, const std::string& key) {
    const json::Value* value = root.get(key);
    return value == nullptr ? "{}" : value_to_json(*value);
}

}  // namespace

std::string current_timestamp_utc() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#if defined(_POSIX_THREAD_SAFE_FUNCTIONS)
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

Session new_session(const provider::RequestContext& context) {
    Session session;
    session.created_at = current_timestamp_utc();
    session.updated_at = session.created_at;
    session.provider = context.profile.name;
    session.base_url = context.base_url;
    session.model = context.options.model;
    std::ostringstream settings;
    settings << "{";
    settings << "\"stream\":" << (context.options.stream ? "true" : "false");
    if (context.options.has_temperature) {
        settings << ",\"temperature\":" << context.options.temperature;
    }
    if (context.options.has_top_p) {
        settings << ",\"top_p\":" << context.options.top_p;
    }
    if (context.options.has_max_output_tokens) {
        settings << ",\"max_output_tokens\":" << context.options.max_output_tokens;
    }
    settings << "}";
    session.settings_json = settings.str();
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
    out << "  \"compaction_events\": []\n";
    out << "}\n";
    return out.str();
}

Error load_session(const std::string& path, Session& session) {
    Fd fd(open(path.c_str(), O_RDONLY));
    if (fd.get() < 0) {
        return {ErrorCode::FileRead, errno_message("could not open chat file for reading", path)};
    }
    std::string data;
    char buffer[8192];
    while (true) {
        const ssize_t n = read(fd.get(), buffer, sizeof(buffer));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return {ErrorCode::FileRead, errno_message("could not read chat file", path)};
        }
        if (n == 0) {
            break;
        }
        data.append(buffer, static_cast<size_t>(n));
    }
    json::ParseResult parsed = json::parse(data);
    if (!parsed.error.ok()) {
        return parsed.error;
    }
    if (!parsed.value.is_object()) {
        return {ErrorCode::ProviderSchema, "chat file root is not an object: " + path};
    }

    Session loaded;
    if (const json::Value* version = parsed.value.get("schema_version")) {
        if (version->type != json::Value::Type::Number || version->number < 1.0) {
            return {ErrorCode::ProviderSchema, "chat file schema_version is invalid: " + path};
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
    loaded.settings_json = optional_raw_json(parsed.value, "settings");
    loaded.usage_json = optional_raw_json(parsed.value, "usage");

    const json::Value* messages = parsed.value.get("messages");
    if (messages == nullptr || !messages->is_array()) {
        return {ErrorCode::ProviderSchema, "chat file field is missing or not an array: messages"};
    }
    for (const json::Value& item : messages->array) {
        if (!item.is_object()) {
            return {ErrorCode::ProviderSchema, "chat file message is not an object: " + path};
        }
        const json::Value* role = item.get("role");
        const json::Value* content = item.get("content");
        if (role == nullptr || !role->is_string() || content == nullptr || !content->is_string()) {
            return {ErrorCode::ProviderSchema, "chat file message requires string role and content: " + path};
        }
        if (role->string != "system" && role->string != "user" && role->string != "assistant") {
            return {ErrorCode::ProviderSchema, "chat file message has unsupported role: " + role->string};
        }
        loaded.messages.push_back({role->string, content->string});
    }
    session = std::move(loaded);
    return ok_error();
}

Error save_session_atomic(const std::string& path, Session session) {
    session.updated_at = current_timestamp_utc();
    const std::string data = session_to_json(session);
    const std::string tmp = path + ".tmp." + std::to_string(static_cast<long long>(getpid()));
    Fd fd(open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_TRUNC, 0600));
    if (fd.get() < 0) {
        return {ErrorCode::FileWrite, errno_message("could not open temporary chat file", tmp)};
    }
    Error err = write_all(fd.get(), data, tmp);
    if (!err.ok()) {
        unlink(tmp.c_str());
        return err;
    }
    if (fsync(fd.get()) != 0) {
        err = {ErrorCode::FileWrite, errno_message("could not fsync temporary chat file", tmp)};
        unlink(tmp.c_str());
        return err;
    }
    if (close(fd.release()) != 0) {
        err = {ErrorCode::FileWrite, errno_message("could not close temporary chat file", tmp)};
        unlink(tmp.c_str());
        return err;
    }
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        err = {ErrorCode::FileWrite, errno_message("could not replace chat file", path)};
        unlink(tmp.c_str());
        return err;
    }
    Fd dir(open(dirname_of(path).c_str(), O_RDONLY));
    if (dir.get() >= 0) {
        fsync(dir.get());
    }
    return ok_error();
}

}  // namespace pkchat::chat
