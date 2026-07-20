#include "agent/review_log.hpp"

#include <atomic>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <filesystem>
#include <iomanip>
#include <regex>
#include <sstream>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "html/html.hpp"
#include "security/redact.hpp"

namespace ainiux::agent {
namespace {
namespace fs = std::filesystem;

json::Value object_value() { json::Value v; v.type = json::Value::Type::Object; return v; }
json::Value array_value() { json::Value v; v.type = json::Value::Type::Array; return v; }
json::Value string_value(const std::string& s) { json::Value v; v.type = json::Value::Type::String; v.string = s; return v; }
json::Value number_value(double n) { json::Value v; v.type = json::Value::Type::Number; v.number = n; return v; }
json::Value bool_value(bool b) { json::Value v; v.type = json::Value::Type::Bool; v.boolean = b; return v; }

std::string errno_text(const std::string& action, const std::string& path) {
    return action + " " + path + ": " + std::strerror(errno);
}

std::string timestamp(bool filename) {
    const auto now = std::chrono::system_clock::now();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch()).count();
    const std::time_t seconds = static_cast<std::time_t>(millis / 1000);
    std::tm utc{};
    if (gmtime_r(&seconds, &utc) == nullptr) return std::to_string(millis);
    char date[32] = {};
    std::strftime(date, sizeof(date), filename ? "%Y%m%dT%H%M%S" : "%Y-%m-%dT%H:%M:%S", &utc);
    std::ostringstream out;
    out << date << '.' << std::setw(3) << std::setfill('0') << (millis % 1000) << 'Z';
    return out.str();
}

class FdHandle {
   public:
    explicit FdHandle(int fd = -1) : fd_(fd) {}
    ~FdHandle() { if (fd_ >= 0) ::close(fd_); }
    FdHandle(const FdHandle&) = delete;
    FdHandle& operator=(const FdHandle&) = delete;
    int get() const { return fd_; }
    int release() { const int fd = fd_; fd_ = -1; return fd; }
    void reset(int fd) { if (fd_ >= 0) ::close(fd_); fd_ = fd; }
   private:
    int fd_;
};

Error open_secure_child_directory(int parent_fd, const std::string& name,
                                  const std::string& display_path, FdHandle& child) {
    struct stat info{};
    if (::fstatat(parent_fd, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno != ENOENT)
            return {ErrorCode::FileWrite, errno_text("could not inspect log directory", display_path)};
        if (::mkdirat(parent_fd, name.c_str(), 0700) != 0)
            return {ErrorCode::FileWrite, errno_text("could not create log directory", display_path)};
        if (::fstatat(parent_fd, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0)
            return {ErrorCode::FileWrite, errno_text("could not verify log directory", display_path)};
    }
    if (S_ISLNK(info.st_mode) || !S_ISDIR(info.st_mode))
        return {ErrorCode::FileWrite,
                "security-review log path is symlinked or not a directory: " + display_path};
    const int fd = ::openat(parent_fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return {ErrorCode::FileWrite, errno_text("could not open log directory", display_path)};
    child.reset(fd);
    if (::fchmod(child.get(), 0700) != 0)
        return {ErrorCode::FileWrite, errno_text("could not secure log directory", display_path)};
    return ok_error();
}

std::string base64_encode(const std::string& input) {
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < input.size(); i += 3) {
        const unsigned int a = static_cast<unsigned char>(input[i]);
        const unsigned int b = i + 1 < input.size() ? static_cast<unsigned char>(input[i + 1]) : 0;
        const unsigned int c = i + 2 < input.size() ? static_cast<unsigned char>(input[i + 2]) : 0;
        const unsigned int value = (a << 16U) | (b << 8U) | c;
        output.push_back(table[(value >> 18U) & 63U]);
        output.push_back(table[(value >> 12U) & 63U]);
        output.push_back(i + 1 < input.size() ? table[(value >> 6U) & 63U] : '=');
        output.push_back(i + 2 < input.size() ? table[value & 63U] : '=');
    }
    return output;
}

std::string sanitized_endpoint(std::string endpoint) {
    const std::size_t scheme = endpoint.find("://");
    if (scheme != std::string::npos) {
        const std::size_t authority = scheme + 3;
        const std::size_t slash = endpoint.find('/', authority);
        const std::size_t at = endpoint.find('@', authority);
        if (at != std::string::npos && (slash == std::string::npos || at < slash))
            endpoint.erase(authority, at - authority + 1);
    }
    const std::size_t suffix = endpoint.find_first_of("?#");
    if (suffix != std::string::npos) endpoint.erase(suffix);
    return endpoint;
}

ReviewLogContext from_provider_context(const provider::ToolRoundContext& source) {
    ReviewLogContext result;
    result.stage = source.stage;
    result.worker_slot = source.worker_slot;
    result.task_number = source.task_number;
    result.segment_number = source.segment_number;
    result.synthesis_group = source.synthesis_group;
    result.round = source.round;
    result.retry_attempt = source.retry_attempt;
    result.cumulative_tool_calls = source.cumulative_tool_calls;
    result.sources = source.sources;
    return result;
}

void add_error(json::Value& fields, const Error& error) {
    if (error.ok()) return;
    fields.object["error_code"] = string_value(error_code_name(error.code));
    fields.object["error_message"] = string_value(error.message);
}

}  // namespace

ReviewLogger::~ReviewLogger() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ >= 0) ::close(fd_);
    if (directory_fd_ >= 0) ::close(directory_fd_);
}

bool valid_run_kind(const std::string& run_kind) {
    if (run_kind.empty() || run_kind.size() > 64) return false;
    for (char ch : run_kind) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
        if (!ok) return false;
    }
    return true;
}

std::unique_ptr<ReviewLogger> ReviewLogger::create(const std::string& workspace,
                                                   int keep_runs,
                                                   std::vector<std::string> secrets,
                                                   WarningCallback warning,
                                                   Error& error,
                                                   const std::string& run_kind) {
    std::unique_ptr<ReviewLogger> logger(new ReviewLogger());
    error = logger->initialize(workspace, keep_runs, std::move(secrets), std::move(warning),
                               run_kind);
    if (!error.ok()) return nullptr;
    return logger;
}

Error ReviewLogger::initialize(const std::string& workspace, int keep_runs,
                               std::vector<std::string> secrets, WarningCallback warning,
                               const std::string& run_kind) {
    if (!valid_run_kind(run_kind))
        return {ErrorCode::BadArgs, "diagnostic log run kind must be [a-z0-9-]{1,64}"};
    run_kind_ = run_kind;
    keep_runs_ = keep_runs;
    warning_ = std::move(warning);
    secrets_ = std::move(secrets);
    std::vector<std::string> escaped;
    for (const std::string& secret : secrets_) {
        const std::string encoded = json::escape_string(secret);
        if (encoded != secret) escaped.push_back(encoded);
    }
    secrets_.insert(secrets_.end(), escaped.begin(), escaped.end());

    fs::path root = workspace.empty() ? fs::path(".") : fs::path(workspace);
    FdHandle parent(::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (parent.get() < 0)
        return {ErrorCode::FileWrite, errno_text("could not open security-review workspace", root.string())};
    FdHandle child;
    Error directory_error = open_secure_child_directory(parent.get(), ".ainiux",
                                                         (root / ".ainiux").string(), child);
    if (!directory_error.ok()) return directory_error;
    parent.reset(child.release());
    directory_error = open_secure_child_directory(parent.get(), "logs",
                                                    (root / ".ainiux/logs").string(), child);
    if (!directory_error.ok()) return directory_error;
    parent.reset(child.release());
    directory_error = open_secure_child_directory(parent.get(), run_kind_,
                                                    (root / ".ainiux/logs" / run_kind_).string(), child);
    if (!directory_error.ok()) return directory_error;
    directory_fd_ = child.release();
    directory_ = (root / ".ainiux/logs" / run_kind_).string();
    static std::atomic<unsigned long long> counter{0};
    const std::string prefix = run_kind_ + "-" + timestamp(true) + "-" +
                               std::to_string(static_cast<long long>(::getpid())) + "-";
    for (int attempts = 0; attempts < 1000; ++attempts) {
        const unsigned long long suffix = counter.fetch_add(1) + 1;
        run_id_ = prefix + std::to_string(suffix);
        final_name_ = run_id_ + ".jsonl";
        partial_name_ = final_name_ + ".partial";
        final_path_ = (fs::path(directory_) / final_name_).string();
        partial_path_ = (fs::path(directory_) / partial_name_).string();
        fd_ = ::openat(directory_fd_, partial_name_.c_str(),
                       O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd_ >= 0) break;
        if (errno != EEXIST) return {ErrorCode::FileWrite, errno_text("could not create security-review log", partial_path_)};
    }
    if (fd_ < 0) return {ErrorCode::FileWrite, "could not choose a unique security-review log name"};
    active_ = true;
    return ok_error();
}

bool ReviewLogger::enabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_;
}

json::Value ReviewLogger::payload(const std::string& bytes) {
    json::Value value = object_value();
    if (html::is_valid_utf8(bytes)) {
        value.object["encoding"] = string_value("utf-8");
        value.object["data"] = string_value(bytes);
    } else {
        value.object["encoding"] = string_value("base64");
        value.object["data"] = string_value(base64_encode(bytes));
    }
    return value;
}

void ReviewLogger::fail_locked(const std::string& detail) {
    active_ = false;
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    if (!warned_) {
        warned_ = true;
        const std::string label = run_kind_ == "security-review" ? "SECURITY REVIEW"
                                                                : "AGENT";
        const std::string warning = label + " LOGGING DISABLED: " +
                                    redact_secrets(detail, secrets_) +
                                    "; the run will continue and any partial log is preserved";
        if (warning_) warning_(warning);
    }
}

bool ReviewLogger::write_record_locked(const std::string& record) {
    std::string line = redact_secrets(record, secrets_);
    line.push_back('\n');
    std::size_t offset = 0;
    while (offset < line.size()) {
        const ssize_t written = ::write(fd_, line.data() + offset, line.size() - offset);
        if (written < 0) {
            if (errno == EINTR) continue;
            fail_locked(errno_text("could not write security-review log", partial_path_));
            return false;
        }
        if (written == 0) {
            fail_locked("could not write security-review log: zero-byte write");
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    // Flush each record so `tail -f` on the live .partial path sees events as
    // they happen (and so crash partials hold a complete JSONL prefix).
#if defined(_POSIX_SYNCHRONIZED_IO) && _POSIX_SYNCHRONIZED_IO > 0
    if (::fdatasync(fd_) != 0) {
#else
    if (::fsync(fd_) != 0) {
#endif
        fail_locked(errno_text("could not flush security-review log", partial_path_));
        return false;
    }
    return true;
}

void ReviewLogger::event(const std::string& event_type, const ReviewLogContext& context,
                         json::Value fields, const std::string& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_) return;
    if (!fields.is_object()) fields = object_value();
    fields.object["schema_version"] = number_value(1);
    fields.object["timestamp"] = string_value(timestamp(false));
    fields.object["sequence"] = number_value(static_cast<double>(++sequence_));
    fields.object["run_id"] = string_value(run_id_);
    fields.object["event_type"] = string_value(event_type);
    fields.object["stage"] = string_value(context.stage.empty() ? "run" : context.stage);
    if (!status.empty()) fields.object["status"] = string_value(status);
    if (context.worker_slot) fields.object["worker_slot"] = number_value(context.worker_slot);
    if (context.task_number) fields.object["task_number"] = number_value(context.task_number);
    if (context.segment_number) fields.object["segment_number"] = number_value(context.segment_number);
    if (context.synthesis_group) fields.object["synthesis_group"] = number_value(context.synthesis_group);
    if (context.round) fields.object["round"] = number_value(context.round);
    if (context.retry_attempt) fields.object["retry_attempt"] = number_value(context.retry_attempt);
    fields.object["cumulative_tool_calls"] = number_value(context.cumulative_tool_calls);
    if (!context.sources.empty()) {
        json::Value sources = array_value();
        for (const provider::ToolSourceRange& source : context.sources) {
            json::Value item = object_value();
            item.object["path"] = string_value(source.path);
            item.object["byte_start"] = number_value(source.byte_start);
            item.object["byte_end"] = number_value(source.byte_end);
            item.object["line_start"] = number_value(source.line_start);
            item.object["line_end"] = number_value(source.line_end);
            sources.array.push_back(std::move(item));
        }
        fields.object["sources"] = std::move(sources);
    }
    write_record_locked(json::stringify(fields));
}

provider::ToolRoundObserver ReviewLogger::tool_round_observer() {
    provider::ToolRoundObserver observer;
    observer.on_request = [this](const provider::ToolRoundContext& context,
                                 const std::string& endpoint,
                                 const std::vector<std::string>& headers,
                                 const std::string& body,
                                 const Error& serialization_error) {
        json::Value fields = object_value();
        fields.object["endpoint"] = string_value(sanitized_endpoint(endpoint));
        json::Value names = array_value();
        for (const std::string& name : headers) names.array.push_back(string_value(name));
        fields.object["header_names"] = std::move(names);
        fields.object["request_bytes"] = number_value(body.size());
        fields.object["serialized_body"] = payload(body);
        add_error(fields, serialization_error);
        event("llm_request", from_provider_context(context), std::move(fields),
              serialization_error.ok() ? "success" : "failure");
    };
    observer.on_response = [this](const provider::ToolRoundContext& context,
                                  const http::Response& response,
                                  const provider::ToolRoundResult& round,
                                  const Error& outcome) {
        json::Value fields = object_value();
        fields.object["http_status"] = number_value(response.status);
        fields.object["content_type"] = string_value(response.content_type);
        fields.object["raw_response"] = payload(response.body);
        fields.object["response_bytes"] = number_value(response.body.size());
        fields.object["truncated"] = bool_value(round.truncated);
        fields.object["parsed_content_bytes"] = number_value(round.content.size());
        fields.object["parsed_tool_call_count"] = number_value(round.tool_calls.size());
        fields.object["dns_ms"] = number_value(response.dns_ms);
        fields.object["connect_ms"] = number_value(response.connect_ms);
        fields.object["tls_ms"] = number_value(response.tls_ms);
        fields.object["time_to_first_byte_ms"] = number_value(response.time_to_first_byte_ms);
        fields.object["first_body_ms"] = number_value(response.first_body_ms);
        fields.object["duration_ms"] = number_value(response.total_ms);
        json::Value headers = object_value();
        for (const auto& header : response.diagnostic_headers)
            headers.object[header.first] = string_value(header.second);
        fields.object["diagnostic_headers"] = std::move(headers);
        add_error(fields, outcome);
        event("llm_response", from_provider_context(context), std::move(fields),
              outcome.ok() ? "success" : "failure");
    };
    return observer;
}

void ReviewLogger::finish(json::Value fields, const std::string& status) {
    event("run_end", {"run"}, std::move(fields), status);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_) return;
        if (::fsync(fd_) != 0) { fail_locked(errno_text("could not fsync security-review log", partial_path_)); return; }
        if (::close(fd_) != 0) { fd_ = -1; fail_locked(errno_text("could not close security-review log", partial_path_)); return; }
        fd_ = -1;
        if (::renameat(directory_fd_, partial_name_.c_str(), directory_fd_, final_name_.c_str()) != 0) {
            fail_locked(errno_text("could not finalize security-review log", partial_path_));
            return;
        }
        active_ = false;
        if (::fsync(directory_fd_) != 0) {
            const std::string detail = errno_text("could not fsync security-review log directory", directory_);
            if (!warned_ && warning_) warning_("SECURITY REVIEW LOG WARNING: " + redact_secrets(detail, secrets_));
            warned_ = true;
        }
    }
    prune_completed();
}

void ReviewLogger::prune_completed() {
    if (keep_runs_ == 0) return;
    const std::string pattern =
        "^" + run_kind_ + R"(-[0-9]{8}T[0-9]{6}\.[0-9]{3}Z-[0-9]+-[0-9]+\.jsonl$)";
    const std::regex recognized(pattern);
    const std::string warn_label =
        run_kind_ == "security-review" ? "SECURITY REVIEW LOG WARNING: " : "AGENT LOG WARNING: ";
    const int duplicate = ::dup(directory_fd_);
    DIR* raw_directory = duplicate < 0 ? nullptr : ::fdopendir(duplicate);
    if (raw_directory == nullptr) {
        if (duplicate >= 0) ::close(duplicate);
        std::lock_guard<std::mutex> lock(mutex_);
        if (!warned_ && warning_)
            warning_(warn_label + errno_text("could not enumerate completed logs", directory_));
        warned_ = true;
        return;
    }
    std::unique_ptr<DIR, int (*)(DIR*)> directory(raw_directory, ::closedir);
    std::vector<std::string> files;
    int read_error = 0;
    for (;;) {
        errno = 0;
        dirent* entry = ::readdir(directory.get());
        if (entry == nullptr) { read_error = errno; break; }
        const std::string name = entry->d_name;
        if (!std::regex_match(name, recognized)) continue;
        struct stat info{};
        if (::fstatat(directory_fd_, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISREG(info.st_mode)) files.push_back(name);
    }
    if (read_error != 0) {
        errno = read_error;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!warned_ && warning_)
            warning_(warn_label + errno_text("could not enumerate completed logs", directory_));
        warned_ = true;
        return;
    }
    std::sort(files.begin(), files.end());
    while (files.size() > static_cast<std::size_t>(keep_runs_)) {
        const std::string victim = files.front();
        files.erase(files.begin());
        struct stat info{};
        if (::fstatat(directory_fd_, victim.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(info.st_mode) || ::unlinkat(directory_fd_, victim.c_str(), 0) != 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!warned_ && warning_)
                warning_(warn_label +
                         errno_text("could not prune completed log",
                                    (fs::path(directory_) / victim).string()));
            warned_ = true;
            return;
        }
    }
}

}  // namespace ainiux::agent
