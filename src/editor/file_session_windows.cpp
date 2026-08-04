#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "editor/file_session.hpp"

#include "platform/environment.hpp"
#include "platform/filesystem.hpp"
#include "platform/windows_utf.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <sstream>
#include <utility>

namespace ainiux::editor {
namespace {
namespace fs = std::filesystem;

constexpr const char* kOwnerFileName = "owner";
constexpr std::size_t kMaxMetadataBytes = 16384;
constexpr std::size_t kMaxOwnerFieldBytes = 4096;

std::string hex_encode(const std::string& input) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(input.size() * 2U);
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
    if (input.size() % 2U != 0 || input.size() / 2U > kMaxOwnerFieldBytes) return false;
    output.clear();
    output.reserve(input.size() / 2U);
    for (std::size_t index = 0; index < input.size(); index += 2U) {
        const int high = hex_value(input[index]);
        const int low = hex_value(input[index + 1]);
        if (high < 0 || low < 0) return false;
        output.push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

bool parse_long_long(const std::string& value, long long& output) {
    if (value.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const long long parsed = std::strtoll(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0') return false;
    output = parsed;
    return true;
}

std::string hostname() {
    std::wstring value(256, L'\0');
    DWORD size = static_cast<DWORD>(value.size());
    if (!GetComputerNameW(value.data(), &size)) return {};
    value.resize(size);
    std::string utf8;
    return platform::utf16_to_utf8(value, utf8).ok() ? utf8 : std::string();
}

long long filetime_value(const FILETIME& value) {
    ULARGE_INTEGER integer{};
    integer.LowPart = value.dwLowDateTime;
    integer.HighPart = value.dwHighDateTime;
    return static_cast<long long>(integer.QuadPart);
}

long long current_process_creation_time() {
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    return filetime_value(created);
}

std::string serialize_owner(const EditorLockOwner& owner) {
    std::ostringstream output;
    output << "schema=" << owner.schema_version << '\n'
           << "hostname=" << hex_encode(owner.hostname) << '\n'
           << "pid=" << owner.pid << '\n'
           << "start_time=" << owner.start_time << '\n'
           << "target=" << hex_encode(owner.canonical_target) << '\n'
           << "token=" << hex_encode(owner.token) << '\n';
    return output.str();
}

bool safe_remove_lock(const std::string& directory, const std::string& expected_token) {
    EditorLockOwner current;
    if (!read_editor_lock_owner(directory, current).ok() || current.token != expected_token)
        return false;
    std::error_code error;
    const fs::path owner = fs::u8path(directory) / kOwnerFileName;
    if (!fs::remove(owner, error) || error) return false;
    if (fs::remove(fs::u8path(directory), error) && !error) return true;
    // Preserve an unverifiable lock if another entry appeared concurrently.
    (void)platform::atomic_write_private(owner.u8string(), serialize_owner(current));
    return false;
}

bool process_is_proven_gone(const EditorLockOwner& owner) {
    if (owner.pid <= 0 || owner.pid > std::numeric_limits<DWORD>::max()) return false;
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                 static_cast<DWORD>(owner.pid));
    if (process == nullptr) return GetLastError() == ERROR_INVALID_PARAMETER;
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    const bool different_process =
        GetProcessTimes(process, &created, &exited, &kernel, &user) &&
        filetime_value(created) != owner.start_time;
    const bool exited_process = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
    CloseHandle(process);
    return different_process || exited_process;
}

Error actual_case_leaf(const fs::path& parent,
                       const fs::path& requested_leaf,
                       fs::path& output) {
    output = requested_leaf;
    std::wstring requested = requested_leaf.wstring();
    bool found = false;
    std::error_code error;
    for (fs::directory_iterator iterator(parent, error), end;
         !error && iterator != end; iterator.increment(error)) {
        const std::wstring candidate = iterator->path().filename().wstring();
        if (CompareStringOrdinal(requested.c_str(), static_cast<int>(requested.size()),
                                 candidate.c_str(), static_cast<int>(candidate.size()), TRUE) !=
            CSTR_EQUAL)
            continue;
        if (candidate == requested) {
            output = iterator->path().filename();
            return ok_error();
        }
        if (found)
            return {ErrorCode::FileLock,
                    "editor target has ambiguous case variants in a case-sensitive Windows "
                    "directory: " + requested_leaf.u8string()};
        output = iterator->path().filename();
        found = true;
    }
    if (error)
        return {ErrorCode::FileLock,
                "could not enumerate editor target parent: " + parent.u8string() + ": " +
                    error.message()};
    return ok_error();
}

EditorLockAttempt acquire_canonical(const std::string& canonical_path,
                                    bool allow_stale_retry) {
    EditorLockAttempt result;
    const std::string directory = canonical_path + ".LOCK";
    bool created = false;
    result.error = platform::create_private_directory_exclusive(directory, created);
    if (!result.error.ok()) {
        result.error.code = ErrorCode::FileLock;
        return result;
    }
    if (created) {
        EditorLockOwner owner;
        owner.schema_version = 1;
        owner.hostname = hostname();
        if (owner.hostname.empty()) owner.hostname = "hostname-unavailable";
        owner.pid = static_cast<long long>(platform::current_process_id());
        owner.start_time = current_process_creation_time();
        owner.canonical_target = canonical_path;
        result.error = platform::secure_random_hex(24, owner.token);
        if (!result.error.ok()) {
            std::error_code ignored;
            fs::remove(fs::u8path(directory), ignored);
            result.error.code = ErrorCode::FileLock;
            return result;
        }
        const std::string owner_path =
            (fs::u8path(directory) / kOwnerFileName).u8string();
        result.error =
            platform::atomic_write_private(owner_path, serialize_owner(owner), true);
        if (!result.error.ok()) {
            std::error_code ignored;
            fs::remove(fs::u8path(owner_path), ignored);
            fs::remove(fs::u8path(directory), ignored);
            result.error.code = ErrorCode::FileLock;
            return result;
        }
        result.lock = std::make_shared<EditorFileLock>(
            canonical_path, directory, std::move(owner));
        return result;
    }

    result.error = read_editor_lock_owner(directory, result.conflicting_owner);
    result.owner_metadata_valid = result.error.ok();
    const std::string local_host = hostname();
    if (result.owner_metadata_valid && allow_stale_retry && !local_host.empty() &&
        result.conflicting_owner.hostname == local_host &&
        result.conflicting_owner.canonical_target == canonical_path &&
        process_is_proven_gone(result.conflicting_owner) &&
        safe_remove_lock(directory, result.conflicting_owner.token)) {
        result = acquire_canonical(canonical_path, false);
        result.stale_lock_recovered = result.lock != nullptr;
        return result;
    }
    if (result.owner_metadata_valid) {
        result.error = {ErrorCode::FileLock,
                        "editor file is locked: " + canonical_path + " (" +
                            editor_lock_owner_summary(result) +
                            "). Opened read-only; close the other editor or remove " + directory +
                            " after verifying it is stale"};
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
    return left.exists == right.exists && left.device == right.device &&
           left.inode == right.inode && left.size == right.size &&
           left.modified_seconds == right.modified_seconds &&
           left.modified_nanoseconds == right.modified_nanoseconds;
}

bool operator!=(const FileFingerprint& left, const FileFingerprint& right) {
    return !(left == right);
}

Error fingerprint_file(const std::string& canonical_path, FileFingerprint& output) {
    output = FileFingerprint{};
    std::error_code error;
    const fs::path path = fs::u8path(canonical_path);
    const fs::file_status status = fs::status(path, error);
    if (error == std::errc::no_such_file_or_directory) return ok_error();
    if (error)
        return {ErrorCode::FileRead,
                "could not inspect editor file " + canonical_path + ": " + error.message()};
    if (!fs::exists(status)) return ok_error();
    platform::FileIdentity identity;
    Error identity_error = platform::file_identity(canonical_path, identity);
    if (!identity_error.ok()) return identity_error;
    output.exists = true;
    output.device = identity.volume;
    output.inode = identity.file_low ^ identity.file_high;
    if (fs::is_regular_file(status)) output.size = fs::file_size(path, error);
    if (error) return {ErrorCode::FileRead, "could not size editor file: " + error.message()};
    const auto modified = fs::last_write_time(path, error).time_since_epoch();
    if (error) return {ErrorCode::FileRead, "could not timestamp editor file: " + error.message()};
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(modified);
    const auto nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(modified - seconds);
    output.modified_seconds = seconds.count();
    output.modified_nanoseconds = nanoseconds.count();
    return ok_error();
}

Error canonicalize_editor_target(const std::string& path, std::string& output) {
    const std::string expanded = expand_user_path(path);
    if (expanded.empty()) return {ErrorCode::BadArgs, "editor file path is empty"};
    Error syntax = platform::validate_windows_path_syntax(expanded);
    if (!syntax.ok()) return syntax;
    std::error_code error;
    fs::path absolute = fs::absolute(fs::u8path(expanded), error).lexically_normal();
    if (error)
        return {ErrorCode::FileLock,
                "could not make editor path absolute: " + expanded + ": " + error.message()};
    fs::path parent = fs::canonical(absolute.parent_path(), error);
    if (error)
        return {ErrorCode::FileLock,
                "could not canonicalize editor parent: " + absolute.parent_path().u8string() +
                    ": " + error.message()};
    fs::path leaf;
    Error leaf_error = actual_case_leaf(parent, absolute.filename(), leaf);
    if (!leaf_error.ok()) return leaf_error;
    fs::path resolved = parent / leaf;
    if (fs::exists(resolved, error)) {
        resolved = fs::canonical(resolved, error);
        if (error)
            return {ErrorCode::FileLock,
                    "could not canonicalize editor target: " + resolved.u8string() + ": " +
                        error.message()};
    } else if (error) {
        return {ErrorCode::FileLock,
                "could not inspect editor target: " + resolved.u8string() + ": " +
                    error.message()};
    }
    output = resolved.u8string();
    return ok_error();
}

Error read_editor_lock_owner(const std::string& lock_directory, EditorLockOwner& output) {
    output = EditorLockOwner{};
    const std::string path = (fs::u8path(lock_directory) / kOwnerFileName).u8string();
    std::string metadata;
    Error error = platform::read_file_bounded(path, kMaxMetadataBytes, metadata);
    if (!error.ok()) {
        error.code = ErrorCode::FileLock;
        return error;
    }
    std::istringstream lines(metadata);
    std::string line;
    std::array<std::string, 6> values{};
    const std::array<std::string, 6> keys = {
        "schema=", "hostname=", "pid=", "start_time=", "target=", "token="};
    std::size_t index = 0;
    while (std::getline(lines, line)) {
        if (line.empty() && lines.eof()) break;
        if (index >= keys.size() || line.rfind(keys[index], 0) != 0)
            return {ErrorCode::FileLock,
                    "editor lock metadata has an invalid schema: " + path};
        values[index] = line.substr(keys[index].size());
        ++index;
    }
    long long schema = 0;
    if (index != keys.size() || !parse_long_long(values[0], schema) || schema != 1 ||
        !hex_decode(values[1], output.hostname) ||
        !parse_long_long(values[2], output.pid) ||
        !parse_long_long(values[3], output.start_time) ||
        !hex_decode(values[4], output.canonical_target) ||
        !hex_decode(values[5], output.token) || output.hostname.empty() || output.pid <= 0 ||
        output.start_time <= 0 || output.canonical_target.empty() || output.token.empty())
        return {ErrorCode::FileLock, "editor lock metadata is malformed: " + path};
    output.schema_version = 1;
    return ok_error();
}

EditorFileLock::EditorFileLock(std::string canonical_target,
                               std::string lock_directory,
                               EditorLockOwner owner)
    : canonical_target_(std::move(canonical_target)),
      lock_directory_(std::move(lock_directory)),
      owner_(std::move(owner)) {}

EditorFileLock::~EditorFileLock() { release(); }

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
    std::ostringstream output;
    output << "owner " << attempt.conflicting_owner.hostname << " pid "
           << attempt.conflicting_owner.pid << ", started "
           << attempt.conflicting_owner.start_time;
    return output.str();
}

}  // namespace ainiux::editor

#endif
