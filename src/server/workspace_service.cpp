#include "server/workspace_service.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "editor/dired.hpp"
#include "html/html.hpp"
#include "json/json.hpp"
#include "platform/filesystem.hpp"
#include "security/hash.hpp"
#include "server/limits.hpp"

namespace ainiux::server {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kMaxEntries = 2048U;
constexpr std::size_t kMaxFileBytes = 1024U * 1024U;
constexpr std::size_t kMaxReviewDirectories = 2048U;
constexpr std::size_t kMaxMutationOperations = 32U;
constexpr std::size_t kMaxMutationTreeEntries = 512U;
constexpr std::uintmax_t kMaxMutationTreeBytes = 8U * 1024U * 1024U;

struct RemoteEntry {
    std::string name;
    std::string path;
    std::string type;
    std::string revision;
    std::uint64_t size = 0;
    std::int64_t modified_at = 0;
    bool mutable_target = true;
};

struct TreeItem {
    fs::path source;
    fs::path relative;
    bool directory = false;
    platform::FileIdentity identity;
    std::string revision;
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

Error conflict_error(const std::string& relative) {
    return {ErrorCode::FileLock, "workspace target changed since it was reviewed: " + relative};
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
    if (relative.empty() || relative == ".") {
        return allow_root ? ok_error() : invalid_path("file path is required");
    }
    if (relative.front() == '/' || relative.find('\\') != std::string::npos ||
        relative.find('\0') != std::string::npos) {
        return invalid_path("workspace path must be relative and use '/' separators");
    }
    const bool drive_prefix = relative.size() >= 2U &&
                              ((relative[0] >= 'A' && relative[0] <= 'Z') ||
                               (relative[0] >= 'a' && relative[0] <= 'z')) &&
                              relative[1] == ':';
    if (drive_prefix) return invalid_path("workspace path must not name a drive");
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

long long raw_modified_time(const fs::path& path) {
    std::error_code ec;
    const auto value = fs::last_write_time(path, ec);
    return ec ? 0LL : static_cast<long long>(value.time_since_epoch().count());
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

Error resolve_new_target(const std::string& root,
                         const std::string& relative,
                         fs::path& target,
                         fs::path& parent,
                         std::string& parent_relative) {
    Error validation = validate_wire_path(relative, false);
    if (!validation.ok()) return validation;
    const std::size_t slash = relative.rfind('/');
    parent_relative = slash == std::string::npos ? "." : relative.substr(0, slash);
    Error parent_error = resolve_existing(root, parent_relative, true, parent);
    if (!parent_error.ok()) return parent_error;
    std::error_code ec;
    if (!fs::is_directory(parent, ec) || ec) {
        return {ErrorCode::FileRead, safe_path_error("workspace parent is not a directory", parent_relative)};
    }
    target = parent / fs::u8path(slash == std::string::npos ? relative : relative.substr(slash + 1U));
    ec.clear();
    const fs::file_status status = fs::symlink_status(target, ec);
    if (!ec && fs::exists(status)) {
        return {ErrorCode::FileWrite, safe_path_error("workspace destination already exists", relative)};
    }
    if (ec && ec != std::errc::no_such_file_or_directory) {
        return {ErrorCode::FileWrite, safe_path_error("could not inspect workspace destination", relative)};
    }
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

Error revision_for_path(const fs::path& path,
                        bool directory,
                        std::uint64_t size,
                        const std::string* known_content,
                        std::string& revision) {
    platform::FileIdentity identity;
    Error error = platform::file_identity(path.u8string(), identity, false);
    if (!error.ok() || !identity.valid) {
        return {ErrorCode::FileRead, "could not inspect workspace target identity"};
    }
    std::string material = "ainiux-workspace-revision-v1\n" +
                           std::to_string(identity.volume) + ":" +
                           std::to_string(identity.file_high) + ":" +
                           std::to_string(identity.file_low) + "\n" +
                           (directory ? "directory" : "file") + "\n" +
                           std::to_string(size) + "\n" +
                           std::to_string(raw_modified_time(path));
    if (!directory && size <= kMaxFileBytes) {
        std::string content;
        if (known_content != nullptr) {
            content = *known_content;
        } else {
            error = platform::read_file_bounded(path.u8string(), kMaxFileBytes, content);
            if (!error.ok()) return {ErrorCode::FileRead, "could not fingerprint workspace file"};
        }
        material += "\n" + security::sha256_hex(content);
    }
    revision = security::sha256_hex(material);
    return ok_error();
}

Error directory_revision(const fs::path& directory, std::string& revision) {
    return revision_for_path(directory, true, 0, nullptr, revision);
}

Error snapshot_file(const std::string& root,
                    const std::string& relative,
                    WorkspaceFileSnapshot& snapshot) {
    fs::path path;
    Error error = resolve_existing(root, relative, false, path);
    if (!error.ok()) return error;
    bool directory = false;
    std::uint64_t size = 0;
    if (!entry_is_supported(path, directory, size) || directory) {
        return {ErrorCode::FileRead, safe_path_error("workspace path is not a regular file", relative)};
    }
    if (size > kMaxFileBytes) {
        return {ErrorCode::FileRead, safe_path_error("workspace file exceeds the remote editing limit", relative)};
    }
    platform::FileIdentity before;
    error = platform::file_identity(path.u8string(), before, false);
    if (!error.ok()) return {ErrorCode::FileRead, "could not inspect workspace file"};
    std::string content;
    error = platform::read_file_bounded(path.u8string(), kMaxFileBytes, content);
    if (!error.ok()) {
        return {ErrorCode::FileRead, safe_path_error("workspace file cannot be read", relative)};
    }
    platform::FileIdentity after;
    error = platform::file_identity(path.u8string(), after, false);
    if (!error.ok() || !platform::same_file_identity(before, after)) {
        return conflict_error(relative);
    }
    std::string revision;
    error = revision_for_path(path, false, size, &content, revision);
    if (!error.ok()) return error;
    snapshot.path = normalized_wire_path(relative);
    snapshot.content = std::move(content);
    snapshot.revision = std::move(revision);
    return ok_error();
}

Error remote_entry(const std::string& root,
                   const std::string& relative,
                   const std::string& name,
                   RemoteEntry& entry) {
    fs::path path;
    Error error = resolve_existing(root, relative, false, path);
    if (!error.ok()) return error;
    bool directory = false;
    std::uint64_t size = 0;
    if (!entry_is_supported(path, directory, size)) {
        return {ErrorCode::FileRead, safe_path_error("workspace target type is not supported", relative)};
    }
    std::string revision;
    error = revision_for_path(path, directory, size, nullptr, revision);
    if (!error.ok()) return error;
    entry.name = name;
    entry.path = relative;
    entry.type = directory ? "directory" : "file";
    entry.revision = std::move(revision);
    entry.size = size;
    entry.modified_at = modified_seconds(path);
    entry.mutable_target = directory || size <= kMaxFileBytes;
    return ok_error();
}

Error collect_directory(const std::string& root,
                        const std::string& relative,
                        std::vector<RemoteEntry>& entries,
                        bool& truncated,
                        std::string* revision = nullptr) {
    fs::path directory;
    Error resolved = resolve_existing(root, relative, true, directory);
    if (!resolved.ok()) return resolved;
    std::error_code ec;
    if (!fs::is_directory(directory, ec) || ec) {
        return {ErrorCode::FileRead, safe_path_error("workspace path is not a directory", relative)};
    }
    if (revision != nullptr) {
        Error revision_error = directory_revision(directory, *revision);
        if (!revision_error.ok()) return revision_error;
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
        if (entries.size() >= kMaxEntries) {
            truncated = true;
            break;
        }
        RemoteEntry entry;
        Error child_error = remote_entry(root, child, source.name, entry);
        if (!child_error.ok()) continue;
        entries.push_back(std::move(entry));
    }
    return ok_error();
}

std::string entry_json(const RemoteEntry& entry) {
    return "{\"name\":" + json::quote(entry.name) +
           ",\"path\":" + json::quote(entry.path) +
           ",\"type\":" + json::quote(entry.type) +
           ",\"revision\":" + json::quote(entry.revision) +
           ",\"size\":" + std::to_string(entry.size) +
           ",\"modified_at\":" + std::to_string(entry.modified_at) +
           ",\"mutable\":" + (entry.mutable_target ? "true" : "false") + "}";
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

Error reject_unknown(const json::Value& value, const std::set<std::string>& allowed) {
    if (!value.is_object()) return {ErrorCode::BadArgs, "operation must be a JSON object"};
    for (const auto& member : value.object) {
        if (allowed.count(member.first) == 0) {
            return {ErrorCode::BadArgs, "field '" + member.first + "' is not supported"};
        }
    }
    return ok_error();
}

Error required_string(const json::Value& value,
                      const std::string& field,
                      std::string& output,
                      std::size_t max_bytes = Limits::json_body_bytes) {
    const json::Value* member = value.get(field);
    if (member == nullptr || !member->is_string() || member->string.empty()) {
        return {ErrorCode::BadArgs, "field '" + field + "' must be a non-empty string"};
    }
    if (member->string.size() > max_bytes) {
        return {ErrorCode::BadArgs, "field '" + field + "' is too long"};
    }
    output = member->string;
    return ok_error();
}

Error optional_bool(const json::Value& value,
                    const std::string& field,
                    bool default_value,
                    bool& output) {
    output = default_value;
    const json::Value* member = value.get(field);
    if (member == nullptr) return ok_error();
    if (member->type != json::Value::Type::Bool) {
        return {ErrorCode::BadArgs, "field '" + field + "' must be a boolean"};
    }
    output = member->boolean;
    return ok_error();
}

std::string mutation_error_code(const Error& error) {
    switch (error.code) {
        case ErrorCode::BadArgs: return "invalid_target";
        case ErrorCode::FileRead: return "not_found";
        case ErrorCode::FileLock: return "revision_conflict";
        case ErrorCode::FileWrite: return "mutation_failed";
        default: return "internal";
    }
}

std::string mutation_error_json(std::size_t index,
                                const std::string& operation,
                                const std::string& relative,
                                const Error& error,
                                const std::string& current_revision) {
    std::string output = "{\"index\":" + std::to_string(index) +
                         ",\"operation\":" + json::quote(operation) +
                         ",\"path\":" + json::quote(relative) +
                         ",\"ok\":false,\"error\":{\"code\":" +
                         json::quote(mutation_error_code(error)) +
                         ",\"message\":" + json::quote(error.message);
    if (!current_revision.empty()) {
        output += ",\"current_revision\":" + json::quote(current_revision);
    }
    output += "}}";
    return output;
}

Error verify_revision(const fs::path& path,
                      const std::string& relative,
                      const std::string& expected,
                      std::string& current,
                      bool* directory_out = nullptr) {
    bool directory = false;
    std::uint64_t size = 0;
    if (!entry_is_supported(path, directory, size)) {
        return {ErrorCode::FileRead, safe_path_error("workspace target was not found", relative)};
    }
    Error error = revision_for_path(path, directory, size, nullptr, current);
    if (!error.ok()) return error;
    if (expected != current) return conflict_error(relative);
    if (directory_out != nullptr) *directory_out = directory;
    return ok_error();
}

Error verify_parent_revision(const fs::path& parent,
                             const std::string& parent_relative,
                             const std::string& expected,
                             std::string& current) {
    Error error = directory_revision(parent, current);
    if (!error.ok()) return error;
    return expected == current ? ok_error() : conflict_error(parent_relative);
}

Error collect_mutation_tree(const fs::path& root, std::vector<TreeItem>& items) {
    items.clear();
    std::vector<TreeItem> pending{{root, fs::path(), true, {}, {}}};
    std::uintmax_t bytes = 0;
    while (!pending.empty()) {
        TreeItem item = std::move(pending.back());
        pending.pop_back();
        if (items.size() >= kMaxMutationTreeEntries) {
            return {ErrorCode::BadArgs, "workspace target exceeds the bounded mutation entry limit"};
        }
        std::error_code ec;
        const fs::file_status status = fs::symlink_status(item.source, ec);
        if (ec || fs::is_symlink(status) || (!fs::is_directory(status) && !fs::is_regular_file(status))) {
            return {ErrorCode::BadArgs, "workspace target contains an unsupported entry"};
        }
        item.directory = fs::is_directory(status);
        Error identity_error = platform::file_identity(item.source.u8string(), item.identity, false);
        if (!identity_error.ok() || !item.identity.valid) {
            return {ErrorCode::FileRead, "could not inspect workspace target identity"};
        }
        if (!item.relative.empty() && excluded_component(item.relative.filename().u8string())) {
            return {ErrorCode::BadArgs, "workspace target contains a protected entry"};
        }
        if (!item.directory) {
            const std::uintmax_t size = fs::file_size(item.source, ec);
            if (ec || size > kMaxFileBytes || bytes > kMaxMutationTreeBytes - size) {
                return {ErrorCode::BadArgs, "workspace target exceeds the bounded mutation size limit"};
            }
            bytes += size;
            Error revision_error = revision_for_path(
                item.source, false, static_cast<std::uint64_t>(size), nullptr, item.revision);
            if (!revision_error.ok()) return revision_error;
        } else {
            fs::directory_iterator iterator(item.source, fs::directory_options::none, ec);
            if (ec) return {ErrorCode::FileRead, "could not inspect workspace target contents"};
            for (const fs::directory_entry& child : iterator) {
                if (items.size() + pending.size() >= kMaxMutationTreeEntries) {
                    return {ErrorCode::BadArgs,
                            "workspace target exceeds the bounded mutation entry limit"};
                }
                const fs::path child_relative = item.relative / child.path().filename();
                pending.push_back({child.path(), child_relative, false, {}, {}});
            }
        }
        items.push_back(std::move(item));
    }
    return ok_error();
}

Error verify_tree_item(const TreeItem& item) {
    bool crosses_link = false;
    Error error = platform::path_contains_link_or_reparse(item.source.u8string(), crosses_link);
    if (!error.ok() || crosses_link) {
        return {ErrorCode::FileLock, "workspace target changed during the mutation"};
    }
    platform::FileIdentity current;
    error = platform::file_identity(item.source.u8string(), current, false);
    if (!error.ok() || !platform::same_file_identity(item.identity, current)) {
        return {ErrorCode::FileLock, "workspace target changed during the mutation"};
    }
    if (!item.directory) {
        bool directory = false;
        std::uint64_t size = 0;
        if (!entry_is_supported(item.source, directory, size) || directory) {
            return {ErrorCode::FileLock, "workspace target changed during the mutation"};
        }
        std::string revision;
        error = revision_for_path(item.source, false, size, nullptr, revision);
        if (!error.ok() || revision != item.revision) {
            return {ErrorCode::FileLock, "workspace target changed during the mutation"};
        }
    }
    return ok_error();
}

Error copy_tree(const fs::path& source, const fs::path& destination) {
    std::vector<TreeItem> items;
    Error error = collect_mutation_tree(source, items);
    if (!error.ok()) return error;
    std::sort(items.begin(), items.end(), [](const TreeItem& left, const TreeItem& right) {
        if (left.directory != right.directory) return left.directory > right.directory;
        return left.relative.native().size() < right.relative.native().size();
    });
    std::error_code ec;
    bool created_root = false;
    for (const TreeItem& item : items) {
        error = verify_tree_item(item);
        if (!error.ok()) {
            if (created_root) fs::remove_all(destination, ec);
            return error;
        }
        const fs::path target = item.relative.empty() ? destination : destination / item.relative;
        if (item.directory) {
            if (!fs::create_directory(target, ec) || ec) {
                if (created_root) fs::remove_all(destination, ec);
                return {ErrorCode::FileWrite, "could not create copied workspace directory"};
            }
            created_root = true;
            continue;
        }
        std::string content;
        error = platform::read_file_bounded(item.source.u8string(), kMaxFileBytes, content);
        if (!error.ok()) {
            if (created_root) fs::remove_all(destination, ec);
            return {ErrorCode::FileRead, "could not read workspace copy source"};
        }
        error = platform::atomic_write_shared_create(target.u8string(), content, true);
        if (!error.ok()) {
            if (created_root) fs::remove_all(destination, ec);
            return {ErrorCode::FileWrite, "could not write workspace copy destination"};
        }
        created_root = true;
    }
    return ok_error();
}

Error remove_tree(const fs::path& target, bool recursive) {
    std::error_code ec;
    const bool directory = fs::is_directory(target, ec) && !ec;
    if (directory && recursive) {
        std::vector<TreeItem> items;
        Error error = collect_mutation_tree(target, items);
        if (!error.ok()) return error;
        std::sort(items.begin(), items.end(), [](const TreeItem& left, const TreeItem& right) {
            return left.relative.native().size() > right.relative.native().size();
        });
        for (const TreeItem& item : items) {
            error = verify_tree_item(item);
            if (!error.ok()) return error;
            if (!fs::remove(item.source, ec) || ec) {
                return {ErrorCode::FileWrite, "could not delete workspace target"};
            }
        }
        return ok_error();
    }
    if (!fs::remove(target, ec) || ec) {
        return {ErrorCode::FileWrite,
                directory ? "workspace directory is not empty; set recursive and confirm the target"
                          : "could not delete workspace target"};
    }
    return ok_error();
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
    std::lock_guard<std::mutex> lock(mutex_);
    body.clear();
    if (workspace_.empty()) return {ErrorCode::Internal, "workspace service is unavailable"};
    std::vector<RemoteEntry> entries;
    bool truncated = false;
    std::string revision;
    Error error = collect_directory(workspace_, relative_path, entries, truncated, &revision);
    if (!error.ok()) return error;
    body = "{\"path\":" + json::quote(normalized_wire_path(relative_path)) +
           ",\"revision\":" + json::quote(revision) +
           ",\"entries\":" + entries_json(entries) +
           ",\"truncated\":" + std::string(truncated ? "true" : "false") + "}";
    return ok_error();
}

Error WorkspaceService::load_file(const std::string& relative_path,
                                  const std::string& expected_revision,
                                  WorkspaceFileSnapshot& snapshot,
                                  std::string* current_revision) const {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot = WorkspaceFileSnapshot{};
    if (current_revision != nullptr) current_revision->clear();
    if (workspace_.empty()) return {ErrorCode::Internal, "workspace service is unavailable"};
    Error error = snapshot_file(workspace_, relative_path, snapshot);
    if (!error.ok()) return error;
    if (current_revision != nullptr) *current_revision = snapshot.revision;
    if (!expected_revision.empty() && expected_revision != snapshot.revision) {
        return conflict_error(relative_path);
    }
    return ok_error();
}

Error WorkspaceService::read(const std::string& relative_path, std::string& body) const {
    WorkspaceFileSnapshot snapshot;
    Error error = load_file(relative_path, "", snapshot);
    if (!error.ok()) return error;
    body = "{\"path\":" + json::quote(snapshot.path) +
           ",\"revision\":" + json::quote(snapshot.revision) +
           ",\"content\":" + json::quote(snapshot.content) +
           ",\"size\":" + std::to_string(snapshot.content.size()) +
           ",\"truncated\":false}";
    return ok_error();
}

Error WorkspaceService::review(std::string& body) const {
    std::lock_guard<std::mutex> lock(mutex_);
    body.clear();
    if (workspace_.empty()) return {ErrorCode::Internal, "workspace service is unavailable"};
    std::vector<RemoteEntry> entries;
    std::vector<std::string> directories{"."};
    bool truncated = false;
    std::size_t directory_count = 0;
    std::uint64_t file_bytes = 0;
    std::string root_revision;
    while (!directories.empty() && !truncated && directory_count < kMaxReviewDirectories) {
        const std::string directory = std::move(directories.back());
        directories.pop_back();
        ++directory_count;
        std::vector<RemoteEntry> children;
        bool directory_truncated = false;
        std::string directory_revision_value;
        Error error = collect_directory(workspace_, directory, children, directory_truncated,
                                        &directory_revision_value);
        if (!error.ok()) {
            if (directory == ".") return error;
            continue;
        }
        if (directory == ".") root_revision = std::move(directory_revision_value);
        for (RemoteEntry& entry : children) {
            if (entry.type == "directory") directories.push_back(entry.path);
            else file_bytes += entry.size;
            entries.push_back(std::move(entry));
            if (entries.size() >= kMaxEntries) {
                truncated = true;
                break;
            }
        }
        if (directory_truncated) truncated = true;
    }
    if (!directories.empty()) truncated = true;
    body = "{\"path\":\".\",\"revision\":" + json::quote(root_revision) +
           ",\"entries\":" + entries_json(entries) + ",\"summary\":{\"files\":";
    std::size_t file_count = 0;
    for (const RemoteEntry& entry : entries) if (entry.type == "file") ++file_count;
    body += std::to_string(file_count) + ",\"directories\":" +
            std::to_string(directory_count > 0 ? directory_count - 1U : 0U) +
            ",\"bytes\":" + std::to_string(file_bytes) + "},\"truncated\":" +
            std::string(truncated ? "true" : "false") + "}";
    return ok_error();
}

Error WorkspaceService::save(const std::string& relative_path,
                             const std::string& request_body,
                             std::string& body,
                             std::string& current_revision) {
    std::lock_guard<std::mutex> lock(mutex_);
    body.clear();
    current_revision.clear();
    if (workspace_.empty()) return {ErrorCode::Internal, "workspace service is unavailable"};
    const json::ParseResult parsed = json::parse(request_body);
    if (!parsed.error.ok()) return {ErrorCode::JsonParse, "request body is not valid JSON"};
    Error error = reject_unknown(parsed.value, {"content", "revision"});
    if (!error.ok()) return error;
    std::string content;
    std::string expected;
    error = required_string(parsed.value, "revision", expected, 128U);
    if (!error.ok()) return error;
    const json::Value* content_value = parsed.value.get("content");
    if (content_value == nullptr || !content_value->is_string()) {
        return {ErrorCode::BadArgs, "field 'content' must be a string"};
    }
    content = content_value->string;
    if (content.size() > kMaxFileBytes) {
        return {ErrorCode::BadArgs, "field 'content' exceeds the remote editing limit"};
    }
    if (content.find('\0') != std::string::npos || !html::is_valid_utf8(content)) {
        return {ErrorCode::BadArgs, "field 'content' must be valid UTF-8 text without NUL bytes"};
    }
    WorkspaceFileSnapshot before;
    error = snapshot_file(workspace_, relative_path, before);
    if (!error.ok()) return error;
    current_revision = before.revision;
    if (expected != current_revision) return conflict_error(relative_path);

    fs::path target;
    error = resolve_existing(workspace_, relative_path, false, target);
    if (!error.ok()) return error;
    error = platform::atomic_write_shared(target.u8string(), content, true);
    if (!error.ok()) {
        return {ErrorCode::FileWrite, safe_path_error("could not atomically save workspace file", relative_path)};
    }
    WorkspaceFileSnapshot after;
    error = snapshot_file(workspace_, relative_path, after);
    if (!error.ok()) return error;
    current_revision = after.revision;
    body = "{\"file\":{\"path\":" + json::quote(after.path) +
           ",\"revision\":" + json::quote(after.revision) +
           ",\"size\":" + std::to_string(after.content.size()) + "}}";
    return ok_error();
}

Error WorkspaceService::create_file(const std::string& request_body,
                                    std::string& body,
                                    std::string& current_revision) {
    std::lock_guard<std::mutex> lock(mutex_);
    body.clear();
    current_revision.clear();
    if (workspace_.empty()) return {ErrorCode::Internal, "workspace service is unavailable"};
    const json::ParseResult parsed = json::parse(request_body);
    if (!parsed.error.ok()) return {ErrorCode::JsonParse, "request body is not valid JSON"};
    Error error = reject_unknown(parsed.value, {"path", "content", "parent_revision"});
    if (!error.ok()) return error;
    std::string relative;
    std::string content;
    std::string parent_expected;
    error = required_string(parsed.value, "path", relative, Limits::request_line_bytes);
    if (!error.ok()) return error;
    error = required_string(parsed.value, "parent_revision", parent_expected, 128U);
    if (!error.ok()) return error;
    const json::Value* content_value = parsed.value.get("content");
    if (content_value == nullptr || !content_value->is_string()) {
        return {ErrorCode::BadArgs, "field 'content' must be a string"};
    }
    content = content_value->string;
    if (content.size() > kMaxFileBytes || content.find('\0') != std::string::npos ||
        !html::is_valid_utf8(content)) {
        return {ErrorCode::BadArgs, "field 'content' must be bounded valid UTF-8 text without NUL bytes"};
    }
    fs::path target;
    fs::path parent;
    std::string parent_relative;
    error = resolve_new_target(workspace_, relative, target, parent, parent_relative);
    if (!error.ok()) return error;
    error = verify_parent_revision(parent, parent_relative, parent_expected, current_revision);
    if (!error.ok()) return error;
    error = platform::atomic_write_shared_create(target.u8string(), content, true);
    if (!error.ok()) {
        return {ErrorCode::FileWrite, safe_path_error("could not atomically create workspace file", relative)};
    }
    WorkspaceFileSnapshot after;
    error = snapshot_file(workspace_, relative, after);
    if (!error.ok()) return error;
    current_revision = after.revision;
    body = "{\"file\":{\"path\":" + json::quote(after.path) +
           ",\"revision\":" + json::quote(after.revision) +
           ",\"size\":" + std::to_string(after.content.size()) + "}}";
    return ok_error();
}

Error WorkspaceService::mutate(const std::string& request_body, std::string& body) {
    std::lock_guard<std::mutex> lock(mutex_);
    body.clear();
    if (workspace_.empty()) return {ErrorCode::Internal, "workspace service is unavailable"};
    const json::ParseResult parsed = json::parse(request_body);
    if (!parsed.error.ok()) return {ErrorCode::JsonParse, "request body is not valid JSON"};
    Error top_error = reject_unknown(parsed.value, {"operations"});
    if (!top_error.ok()) return top_error;
    const json::Value* operations = parsed.value.get("operations");
    if (operations == nullptr || !operations->is_array() || operations->array.empty() ||
        operations->array.size() > kMaxMutationOperations) {
        return {ErrorCode::BadArgs, "field 'operations' must contain 1 to 32 operations"};
    }

    std::string results = "[";
    for (std::size_t index = 0; index < operations->array.size(); ++index) {
        if (index != 0) results += ',';
        const json::Value& operation_value = operations->array[index];
        std::string operation;
        std::string relative;
        std::string current;
        Error error = required_string(operation_value, "operation", operation, 16U);
        if (error.ok()) error = required_string(operation_value, "path", relative, Limits::request_line_bytes);
        if (!error.ok()) {
            results += mutation_error_json(index, operation, relative, error, current);
            continue;
        }

        if (operation == "mkdir") {
            error = reject_unknown(operation_value, {"operation", "path", "parent_revision"});
            std::string expected;
            if (error.ok()) error = required_string(operation_value, "parent_revision", expected, 128U);
            fs::path target;
            fs::path parent;
            std::string parent_relative;
            if (error.ok()) error = resolve_new_target(workspace_, relative, target, parent, parent_relative);
            if (error.ok()) error = verify_parent_revision(parent, parent_relative, expected, current);
            std::error_code ec;
            if (error.ok() && (!fs::create_directory(target, ec) || ec)) {
                error = {ErrorCode::FileWrite, safe_path_error("could not create workspace directory", relative)};
            }
        } else if (operation == "move" || operation == "rename" || operation == "copy") {
            error = reject_unknown(operation_value,
                                   {"operation", "path", "revision", "destination",
                                    "destination_parent_revision"});
            std::string expected;
            std::string destination;
            std::string parent_expected;
            if (error.ok()) error = required_string(operation_value, "revision", expected, 128U);
            if (error.ok()) error = required_string(operation_value, "destination", destination,
                                                     Limits::request_line_bytes);
            if (error.ok()) error = required_string(operation_value, "destination_parent_revision",
                                                     parent_expected, 128U);
            fs::path source;
            if (error.ok()) error = resolve_existing(workspace_, relative, false, source);
            if (error.ok()) error = verify_revision(source, relative, expected, current);
            fs::path target;
            fs::path parent;
            std::string parent_relative;
            if (error.ok()) error = resolve_new_target(workspace_, destination, target, parent, parent_relative);
            std::string parent_current;
            if (error.ok()) {
                Error parent_error = verify_parent_revision(
                    parent, parent_relative, parent_expected, parent_current);
                if (!parent_error.ok()) {
                    error = std::move(parent_error);
                    if (error.code == ErrorCode::FileLock) current = parent_current;
                }
            }
            if (error.ok()) {
                if (operation == "copy") {
                    error = copy_tree(source, target);
                } else {
                    error = verify_revision(source, relative, expected, current);
                    if (error.ok()) {
                        error = platform::atomic_move(source.u8string(), target.u8string(), false);
                        if (!error.ok()) {
                            error = {ErrorCode::FileWrite, "could not move workspace target"};
                        }
                    }
                }
            }
            if (error.ok()) relative = destination;
        } else if (operation == "delete") {
            error = reject_unknown(operation_value,
                                   {"operation", "path", "revision", "recursive", "confirmation"});
            std::string expected;
            std::string confirmation;
            bool recursive = false;
            if (error.ok()) error = required_string(operation_value, "revision", expected, 128U);
            if (error.ok()) error = required_string(operation_value, "confirmation", confirmation,
                                                     Limits::request_line_bytes + 16U);
            if (error.ok()) error = optional_bool(operation_value, "recursive", false, recursive);
            if (error.ok() && confirmation != "delete " + relative) {
                error = {ErrorCode::BadArgs,
                         "field 'confirmation' must exactly match 'delete " + relative + "'"};
            }
            fs::path target;
            bool directory = false;
            if (error.ok()) error = resolve_existing(workspace_, relative, false, target);
            if (error.ok()) error = verify_revision(target, relative, expected, current, &directory);
            if (error.ok() && directory && !recursive) {
                std::error_code ec;
                if (!fs::is_empty(target, ec) || ec) {
                    error = {ErrorCode::BadArgs,
                             "recursive must be true to delete a non-empty directory"};
                }
            }
            if (error.ok()) error = verify_revision(target, relative, expected, current, &directory);
            if (error.ok()) error = remove_tree(target, recursive);
        } else {
            error = {ErrorCode::BadArgs, "field 'operation' must be mkdir, move, rename, copy, or delete"};
        }

        if (!error.ok()) {
            results += mutation_error_json(index, operation, relative, error, current);
            continue;
        }
        std::string entry;
        fs::path changed;
        Error changed_error = resolve_existing(workspace_, relative, false, changed);
        if (operation != "delete" && changed_error.ok()) {
            RemoteEntry remote;
            const std::size_t slash = relative.rfind('/');
            const std::string name = slash == std::string::npos ? relative : relative.substr(slash + 1U);
            if (remote_entry(workspace_, relative, name, remote).ok()) entry = entry_json(remote);
        }
        results += "{\"index\":" + std::to_string(index) +
                   ",\"operation\":" + json::quote(operation) +
                   ",\"path\":" + json::quote(relative) + ",\"ok\":true";
        if (!entry.empty()) results += ",\"entry\":" + entry;
        results += "}";
    }
    results += ']';
    body = "{\"results\":" + results + "}";
    return ok_error();
}

}  // namespace ainiux::server
