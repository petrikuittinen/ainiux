#include "editor/file_session.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <sys/stat.h>
#include <signal.h>
#include <unistd.h>

namespace ainiux::editor {
namespace {

constexpr const char* kOwnerFileName = "owner";
constexpr size_t kMaxMetadataBytes = 16384;
constexpr size_t kMaxOwnerFieldBytes = 4096;

std::string errno_message(int value) {
    return std::strerror(value);
}

std::string hex_encode(const std::string& input) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(input.size() * 2);
    for (unsigned char ch : input) {
        output.push_back(digits[ch >> 4U]);
        output.push_back(digits[ch & 0x0FU]);
    }
    return output;
}

int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

bool hex_decode(const std::string& input, std::string& output) {
    if (input.size() % 2 != 0 || input.size() / 2 > kMaxOwnerFieldBytes) {
        return false;
    }
    output.clear();
    output.reserve(input.size() / 2);
    for (size_t i = 0; i < input.size(); i += 2) {
        const int high = hex_value(input[i]);
        const int low = hex_value(input[i + 1]);
        if (high < 0 || low < 0) return false;
        output.push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

bool parse_long_long(const std::string& value, long long& out) {
    if (value.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const long long parsed = std::strtoll(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0') return false;
    out = parsed;
    return true;
}

std::string local_hostname() {
    std::array<char, 256> buffer{};
    if (gethostname(buffer.data(), buffer.size() - 1) != 0) {
        return {};
    }
    buffer.back() = '\0';
    return buffer.data();
}

std::string make_token() {
    std::array<unsigned char, 24> bytes{};
    const int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    size_t used = 0;
    if (fd >= 0) {
        while (used < bytes.size()) {
            const ssize_t count = read(fd, bytes.data() + used, bytes.size() - used);
            if (count > 0) {
                used += static_cast<size_t>(count);
            } else if (count < 0 && errno == EINTR) {
                continue;
            } else {
                break;
            }
        }
        close(fd);
    }
    if (used != bytes.size()) {
        static std::atomic<unsigned long long> sequence{0};
        const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        std::ostringstream fallback;
        fallback << getpid() << '-' << now << '-'
                 << sequence.fetch_add(1, std::memory_order_relaxed);
        return fallback.str();
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (unsigned char byte : bytes) out << std::setw(2) << static_cast<unsigned>(byte);
    return out.str();
}

std::string serialize_owner(const EditorLockOwner& owner) {
    std::ostringstream out;
    out << "schema=" << owner.schema_version << '\n'
        << "hostname=" << hex_encode(owner.hostname) << '\n'
        << "pid=" << owner.pid << '\n'
        << "start_time=" << owner.start_time << '\n'
        << "target=" << hex_encode(owner.canonical_target) << '\n'
        << "token=" << hex_encode(owner.token) << '\n';
    return out.str();
}

Error write_all(int fd, const std::string& data, const std::string& path) {
    size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t count = write(fd, data.data() + offset, data.size() - offset);
        if (count > 0) {
            offset += static_cast<size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return {ErrorCode::FileLock,
                    "could not write editor lock metadata " + path + ": " +
                        errno_message(errno)};
        }
    }
    return ok_error();
}

bool safe_remove_lock(const std::string& directory, const std::string& expected_token) {
    EditorLockOwner current;
    if (!read_editor_lock_owner(directory, current).ok() || current.token != expected_token) {
        return false;
    }
    const std::string owner_path = directory + "/" + kOwnerFileName;
    if (unlink(owner_path.c_str()) != 0) return false;
    if (rmdir(directory.c_str()) == 0) return true;
    // Restore metadata when an unexpected directory entry prevents rmdir. This
    // keeps the unverifiable lock visible instead of silently weakening it.
    const int fd = open(owner_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd >= 0) {
        const std::string metadata = serialize_owner(current);
        (void)write_all(fd, metadata, owner_path);
        close(fd);
    }
    return false;
}

bool process_is_proven_gone(long long pid) {
    if (pid <= 0 || pid > std::numeric_limits<pid_t>::max()) return false;
    errno = 0;
    if (kill(static_cast<pid_t>(pid), 0) == 0) return false;
    return errno == ESRCH;
}

EditorLockAttempt acquire_canonical(const std::string& canonical_path, bool allow_stale_retry) {
    EditorLockAttempt result;
    const std::string directory = canonical_path + ".LOCK";
    if (mkdir(directory.c_str(), 0700) == 0) {
        EditorLockOwner owner;
        owner.schema_version = 1;
        owner.hostname = local_hostname();
        if (owner.hostname.empty()) owner.hostname = "hostname-unavailable";
        owner.pid = static_cast<long long>(getpid());
        owner.start_time = static_cast<long long>(
            std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
        owner.canonical_target = canonical_path;
        owner.token = make_token();
        const std::string owner_path = directory + "/" + kOwnerFileName;
        const int fd = open(owner_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) {
            const int saved_errno = errno;
            (void)rmdir(directory.c_str());
            result.error = {ErrorCode::FileLock,
                            "could not create editor lock metadata " + owner_path + ": " +
                                errno_message(saved_errno)};
            return result;
        }
        const std::string metadata = serialize_owner(owner);
        result.error = write_all(fd, metadata, owner_path);
        if (result.error.ok() && fsync(fd) != 0) {
            result.error = {ErrorCode::FileLock,
                            "could not sync editor lock metadata " + owner_path + ": " +
                                errno_message(errno)};
        }
        if (close(fd) != 0 && result.error.ok()) {
            result.error = {ErrorCode::FileLock,
                            "could not close editor lock metadata " + owner_path + ": " +
                                errno_message(errno)};
        }
        if (!result.error.ok()) {
            (void)unlink(owner_path.c_str());
            (void)rmdir(directory.c_str());
            return result;
        }
        result.lock = std::make_shared<EditorFileLock>(
            canonical_path, directory, std::move(owner));
        return result;
    }

    const int mkdir_errno = errno;
    if (mkdir_errno != EEXIST) {
        result.error = {ErrorCode::FileLock,
                        "could not acquire editor lock " + directory + ": " +
                            errno_message(mkdir_errno)};
        return result;
    }

    result.error = read_editor_lock_owner(directory, result.conflicting_owner);
    result.owner_metadata_valid = result.error.ok();
    const std::string hostname = local_hostname();
    if (result.owner_metadata_valid && allow_stale_retry && !hostname.empty() &&
        result.conflicting_owner.hostname == hostname &&
        result.conflicting_owner.canonical_target == canonical_path &&
        process_is_proven_gone(result.conflicting_owner.pid) &&
        safe_remove_lock(directory, result.conflicting_owner.token)) {
        result = acquire_canonical(canonical_path, false);
        result.stale_lock_recovered = result.lock != nullptr;
        return result;
    }
    if (result.owner_metadata_valid) {
        result.error = {ErrorCode::FileLock,
                        "editor file is locked: " + canonical_path + " (" +
                            editor_lock_owner_summary(result) + "). Opened read-only; close the "
                            "other editor or remove " + directory + " after verifying it is stale"};
    } else {
        result.error = {ErrorCode::FileLock,
                        "editor lock " + directory +
                            " has missing or malformed owner metadata. Opened read-only; verify no "
                            "editor owns it before removing the lock directory manually"};
    }
    return result;
}

}  // namespace

bool operator==(const FileFingerprint& left, const FileFingerprint& right) {
    return left.exists == right.exists && left.device == right.device && left.inode == right.inode &&
           left.size == right.size && left.modified_seconds == right.modified_seconds &&
           left.modified_nanoseconds == right.modified_nanoseconds;
}

bool operator!=(const FileFingerprint& left, const FileFingerprint& right) {
    return !(left == right);
}

Error fingerprint_file(const std::string& canonical_path, FileFingerprint& out) {
    out = {};
    struct stat status {};
    if (stat(canonical_path.c_str(), &status) == 0) {
        out.exists = true;
        out.device = static_cast<std::uintmax_t>(status.st_dev);
        out.inode = static_cast<std::uintmax_t>(status.st_ino);
        out.size = static_cast<std::uintmax_t>(status.st_size);
#if defined(__APPLE__)
        out.modified_seconds = status.st_mtimespec.tv_sec;
        out.modified_nanoseconds = status.st_mtimespec.tv_nsec;
#else
        out.modified_seconds = status.st_mtim.tv_sec;
        out.modified_nanoseconds = status.st_mtim.tv_nsec;
#endif
        return ok_error();
    }
    if (errno == ENOENT || errno == ENOTDIR) return ok_error();
    return {ErrorCode::FileRead,
            "could not inspect editor file " + canonical_path + ": " + errno_message(errno)};
}

Error canonicalize_editor_target(const std::string& path, std::string& out) {
    const std::string expanded = expand_user_path(path);
    if (expanded.empty()) return {ErrorCode::BadArgs, "editor file path is empty"};
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(expanded, error);
    if (error) {
        return {ErrorCode::FileLock,
                "could not make editor path absolute: " + expanded + ": " + error.message()};
    }
    std::filesystem::path canonical = std::filesystem::weakly_canonical(absolute, error);
    if (error) {
        return {ErrorCode::FileLock,
                "could not canonicalize editor path: " + expanded + ": " + error.message()};
    }
    out = canonical.string();
    return ok_error();
}

Error read_editor_lock_owner(const std::string& lock_directory, EditorLockOwner& out) {
    out = {};
    const std::string path = lock_directory + "/" + kOwnerFileName;
    std::ifstream input(path, std::ios::binary);
    if (!input) return {ErrorCode::FileLock, "could not read editor lock metadata: " + path};
    std::string metadata;
    std::array<char, 1024> buffer{};
    while (input && metadata.size() <= kMaxMetadataBytes) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        if (input.gcount() > 0) metadata.append(buffer.data(), static_cast<size_t>(input.gcount()));
    }
    if ((!input.good() && !input.eof()) || metadata.size() > kMaxMetadataBytes) {
        return {ErrorCode::FileLock, "editor lock metadata is unreadable or too large: " + path};
    }
    std::istringstream lines(metadata);
    std::string line;
    std::array<std::string, 6> values{};
    const std::array<std::string, 6> keys = {
        "schema=", "hostname=", "pid=", "start_time=", "target=", "token="};
    size_t index = 0;
    while (std::getline(lines, line)) {
        if (line.empty() && lines.eof()) break;
        if (index >= keys.size() || line.rfind(keys[index], 0) != 0) {
            return {ErrorCode::FileLock, "editor lock metadata has an invalid schema: " + path};
        }
        values[index] = line.substr(keys[index].size());
        ++index;
    }
    long long schema = 0;
    if (index != keys.size() || !parse_long_long(values[0], schema) || schema != 1 ||
        !hex_decode(values[1], out.hostname) || !parse_long_long(values[2], out.pid) ||
        !parse_long_long(values[3], out.start_time) ||
        !hex_decode(values[4], out.canonical_target) || !hex_decode(values[5], out.token) ||
        out.hostname.empty() || out.pid <= 0 || out.start_time <= 0 ||
        out.canonical_target.empty() || out.token.empty()) {
        return {ErrorCode::FileLock, "editor lock metadata is malformed: " + path};
    }
    out.schema_version = 1;
    return ok_error();
}

EditorFileLock::EditorFileLock(std::string canonical_target,
                               std::string lock_directory,
                               EditorLockOwner owner)
    : canonical_target_(std::move(canonical_target)),
      lock_directory_(std::move(lock_directory)),
      owner_(std::move(owner)) {}

EditorFileLock::~EditorFileLock() {
    release();
}

void EditorFileLock::release() {
    if (!owns_lock_) return;
    (void)safe_remove_lock(lock_directory_, owner_.token);
    owns_lock_ = false;
}

EditorLockAttempt acquire_editor_file_lock(const std::string& path) {
    std::string canonical;
    EditorLockAttempt result;
    result.error = canonicalize_editor_target(path, canonical);
    if (!result.error.ok()) return result;
    return acquire_canonical(canonical, true);
}

std::string editor_lock_owner_summary(const EditorLockAttempt& attempt) {
    if (!attempt.owner_metadata_valid) return "owner unknown";
    std::ostringstream out;
    out << "owner " << attempt.conflicting_owner.hostname << " pid "
        << attempt.conflicting_owner.pid << ", started " << attempt.conflicting_owner.start_time;
    return out.str();
}

}  // namespace ainiux::editor
