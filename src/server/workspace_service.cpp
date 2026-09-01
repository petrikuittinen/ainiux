#include "server/workspace_service.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "editor/dired.hpp"
#include "json/json.hpp"
#include "platform/filesystem.hpp"
#include "server/limits.hpp"

namespace ainiux::server {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kMaxEntries = 2048U;
constexpr std::size_t kMaxFileBytes = 1024U * 1024U;
constexpr std::size_t kMaxReviewDirectories = 2048U;

struct RemoteEntry {
    std::string name;
    std::string path;
    std::string type;
    std::uint64_t size = 0;
    std::int64_t modified_at = 0;
};

bool is_sensitive_name(const std::string& name) {
    if (name == "config.conf" || name == "themes.conf" ||
        name == "editor-commands.conf" || name == "benchmarks.conf" ||
        name == "models.conf" || name == "images.conf" || name == ".env") {
        return true;
    }
    if (name.rfind(".env.", 0) == 0 || name.rfind("secret", 0) == 0 ||
        name.rfind("credential", 0) == 0) {
        return true;
    }
    const std::size_t dot = name.rfind('.');
    return dot != std::string::npos &&
           (name.substr(dot) == ".key" || name.substr(dot) == ".pem");
}

bool excluded_component(const std::string& name) {
    return name == ".ainiux-pr" || name == ".ainiux" || name == ".git" ||
           is_sensitive_name(name);
}

bool excluded_path(const std::string& relative) {
    std::size_t start = 0;
    while (start < relative.size()) {
        const std::size_t slash = relative.find('/', start);
        const std::string component = relative.substr(
            start, slash == std::string::npos ? std::string::npos : slash - start);
        if (excluded_component(component)) return true;
        if (slash == std::string::npos) break;
        start = slash + 1U;
    }
    return false;
}

Error invalid_path(const std::string& message) {
    return {ErrorCode::BadArgs, message};
}

std::string safe_path_error(const char* action, const std::string& relative) {
    return std::string(action) + (relative.empty() ? std::string() : ": " + relative);
}

std::string normalized_wire_path(const std::string& relative) {
    return relative.empty() || relative == "." ? "." : relative;
}

Error validate_wire_path(const std::string& relative, bool allow_root) {
    if (relative.size() > Limits::request_line_bytes) {
        return invalid_path("workspace path is too long");
    }
    if (relative.empty()) return allow_root ? ok_error() : invalid_path("file path is required");
    if (relative == ".") return allow_root ? ok_error() : invalid_path("file path is required");
    if (relative.front() == '/' || relative.find('\\') != std::string::npos ||
        relative.find('\0') != std::string::npos) {
        return invalid_path("workspace path must be relative and use '/' separators");
    }
    const bool drive_prefix = relative.size() >= 2U &&
                              ((relative[0] >= 'A' && relative[0] <= 'Z') ||
                               (relative[0] >= 'a' && relative[0] <= 'z')) &&
                              relative[1] == ':';
    if (drive_prefix) return invalid_path("workspace path must not name a drive");
    // Apply the shared Windows lexical checks on every platform so a client
    // cannot send a path that changes meaning when the server is moved to a
    // Windows workspace.
    const Error platform_path = platform::validate_windows_path_syntax(relative);
    if (!platform_path.ok()) {
        return invalid_path("workspace path contains an unsupported filename component");
    }
    std::size_t start = 0;
    while (start <= relative.size()) {
        const std::size_t slash = relative.find('/', start);
        const std::string component = relative.substr(
            start, slash == std::string::npos ? std::string::npos : slash - start);
        if (component.empty() || component == "." || component == "..") {
            return invalid_path("workspace path contains an invalid component");
        }
        if (excluded_component(component)) {
            return invalid_path("workspace path is not available through the control API");
        }
        if (slash == std::string::npos) break;
        start = slash + 1U;
    }
    return ok_error();
}

std::int64_t modified_seconds(const fs::path& path) {
    std::error_code ec;
    const auto value = fs::last_write_time(path, ec);
    if (ec) return 0;
    using namespace std::chrono;
    const auto system_time = time_point_cast<system_clock::duration>(
        value - fs::file_time_type::clock::now() + system_clock::now());
    return static_cast<std::int64_t>(duration_cast<seconds>(system_time.time_since_epoch()).count());
}

Error resolve_existing(const std::string& root,
                       const std::string& relative,
                       bool allow_root,
                       fs::path& resolved) {
    Error validation = validate_wire_path(relative, allow_root);
    if (!validation.ok()) return validation;
    if (excluded_path(relative)) {
        return invalid_path("workspace path is not available through the control API");
    }

    std::error_code ec;
    const fs::path candidate = (fs::u8path(root) / fs::u8path(relative)).lexically_normal();
    const fs::file_status status = fs::symlink_status(candidate, ec);
    if (ec || !fs::exists(status)) {
        return {ErrorCode::FileRead, safe_path_error("workspace path was not found", relative)};
    }

    bool crosses_link = false;
    Error link_error = platform::path_contains_link_or_reparse(candidate.u8string(), crosses_link);
    if (!link_error.ok()) return {ErrorCode::FileRead, "could not inspect workspace path"};
    if (crosses_link) {
        return invalid_path("workspace path crosses a symlink or reparse point");
    }

    const fs::path canonical = fs::canonical(candidate, ec);
    if (ec) return {ErrorCode::FileRead, "could not access workspace path"};
    bool within = false;
    Error containment = platform::path_is_within(root, canonical.u8string(), within);
    if (!containment.ok() || !within) {
        return invalid_path("workspace path is outside the served workspace");
    }
    resolved = canonical;
    return ok_error();
}

bool entry_is_supported(const fs::path& path, bool& directory, std::uint64_t& size) {
    std::error_code ec;
    const fs::file_status link_status = fs::symlink_status(path, ec);
    if (ec || fs::is_symlink(link_status)) return false;
    directory = fs::is_directory(link_status);
    if (directory) {
        size = 0;
        return true;
    }
    if (!fs::is_regular_file(link_status)) return false;
    const auto value = fs::file_size(path, ec);
    if (ec) return false;
    size = static_cast<std::uint64_t>(value);
    return true;
}

Error collect_directory(const std::string& root,
                        const std::string& relative,
                        std::vector<RemoteEntry>& entries,
                        bool& truncated) {
    fs::path directory;
    Error resolved = resolve_existing(root, relative, true, directory);
    if (!resolved.ok()) return resolved;
    std::error_code ec;
    if (!fs::is_directory(directory, ec) || ec) {
        return {ErrorCode::FileRead, safe_path_error("workspace path is not a directory", relative)};
    }

    std::vector<editor::DiredEntry> source_entries;
    const Error dired_error = editor::dired_list_read_only(directory.u8string(), source_entries);
    if (!dired_error.ok()) return {ErrorCode::FileRead, "could not list workspace directory"};
    for (const editor::DiredEntry& source : source_entries) {
        if (source.name.empty()) continue;
        const std::string child = relative.empty() || relative == "."
                                      ? source.name
                                      : relative + "/" + source.name;
        if (excluded_path(child) || source.is_symlink) continue;
        fs::path child_path;
        Error child_error = resolve_existing(root, child, false, child_path);
        if (!child_error.ok()) {
            // A concurrent removal or a denied entry should not disclose the
            // native path or make an otherwise safe directory listing fail.
            continue;
        }
        bool directory_child = false;
        std::uint64_t size = 0;
        if (!entry_is_supported(child_path, directory_child, size)) continue;
        if (entries.size() >= kMaxEntries) {
            truncated = true;
            break;
        }
        RemoteEntry entry;
        entry.name = source.name;
        entry.path = child;
        entry.type = directory_child ? "directory" : "file";
        entry.size = size;
        entry.modified_at = modified_seconds(child_path);
        entries.push_back(std::move(entry));
    }
    return ok_error();
}

std::string entry_json(const RemoteEntry& entry) {
    return "{\"name\":" + json::quote(entry.name) +
           ",\"path\":" + json::quote(entry.path) +
           ",\"type\":" + json::quote(entry.type) +
           ",\"size\":" + std::to_string(entry.size) +
           ",\"modified_at\":" + std::to_string(entry.modified_at) + "}";
}

std::string entries_json(const std::vector<RemoteEntry>& entries) {
    std::string output = "[";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i != 0) output += ',';
        output += entry_json(entries[i]);
    }
    output += ']';
    return output;
}

}  // namespace

WorkspaceService::WorkspaceService(std::string workspace) {
    std::error_code ec;
    const fs::path candidate = fs::canonical(fs::u8path(workspace.empty() ? "." : workspace), ec);
    if (!ec && fs::is_directory(candidate, ec) && !ec) {
        workspace_ = candidate.u8string();
    }
}

Error WorkspaceService::list(const std::string& relative_path, std::string& body) const {
    body.clear();
    if (workspace_.empty()) return {ErrorCode::Internal, "workspace service is unavailable"};
    std::vector<RemoteEntry> entries;
    bool truncated = false;
    Error error = collect_directory(workspace_, relative_path, entries, truncated);
    if (!error.ok()) return error;
    body = "{\"path\":" + json::quote(normalized_wire_path(relative_path)) +
           ",\"entries\":" + entries_json(entries) +
           ",\"truncated\":" + std::string(truncated ? "true" : "false") + "}";
    return ok_error();
}

Error WorkspaceService::read(const std::string& relative_path, std::string& body) const {
    body.clear();
    if (workspace_.empty()) return {ErrorCode::Internal, "workspace service is unavailable"};
    fs::path path;
    Error error = resolve_existing(workspace_, relative_path, false, path);
    if (!error.ok()) return error;
    bool directory = false;
    std::uint64_t ignored_size = 0;
    if (!entry_is_supported(path, directory, ignored_size) || directory) {
        return {ErrorCode::FileRead, safe_path_error("workspace path is not a regular file", relative_path)};
    }
    std::string content;
    error = platform::read_file_bounded(path.u8string(), kMaxFileBytes, content);
    if (!error.ok()) {
        return {ErrorCode::FileRead, safe_path_error("workspace file cannot be read", relative_path)};
    }
    body = "{\"path\":" + json::quote(normalized_wire_path(relative_path)) +
           ",\"content\":" + json::quote(content) +
           ",\"size\":" + std::to_string(content.size()) +
           ",\"truncated\":false}";
    return ok_error();
}

Error WorkspaceService::review(std::string& body) const {
    body.clear();
    if (workspace_.empty()) return {ErrorCode::Internal, "workspace service is unavailable"};
    std::vector<RemoteEntry> entries;
    std::vector<std::string> directories{"."};
    bool truncated = false;
    std::size_t directory_count = 0;
    std::uint64_t file_bytes = 0;
    while (!directories.empty() && !truncated && directory_count < kMaxReviewDirectories) {
        const std::string directory = std::move(directories.back());
        directories.pop_back();
        ++directory_count;
        std::vector<RemoteEntry> children;
        bool directory_truncated = false;
        Error error = collect_directory(workspace_, directory, children, directory_truncated);
        if (!error.ok()) {
            if (directory == ".") return error;
            continue;
        }
        for (RemoteEntry& entry : children) {
            if (entry.type == "directory") {
                directories.push_back(entry.path);
            } else {
                file_bytes += entry.size;
            }
            entries.push_back(std::move(entry));
            if (entries.size() >= kMaxEntries) {
                truncated = true;
                break;
            }
        }
        if (directory_truncated) truncated = true;
    }
    if (!directories.empty()) truncated = true;
    body = "{\"path\":\".\",\"entries\":" + entries_json(entries) +
           ",\"summary\":{\"files\":";
    std::size_t file_count = 0;
    for (const RemoteEntry& entry : entries) if (entry.type == "file") ++file_count;
    body += std::to_string(file_count) + ",\"directories\":" +
            std::to_string(directory_count > 0 ? directory_count - 1U : 0U) +
            ",\"bytes\":" + std::to_string(file_bytes) + "},\"truncated\":" +
            std::string(truncated ? "true" : "false") + "}";
    return ok_error();
}

}  // namespace ainiux::server
