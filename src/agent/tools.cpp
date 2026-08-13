#include "agent/tools.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <thread>
#include <utility>

#include "agent/apply_patch.hpp"
#include "agent/history_backup.hpp"
#include "agent/process.hpp"
#include "agent/project_paths.hpp"
#include "agent/read_only_command.hpp"
#include "agent/text_match.hpp"
#include "agent/tool_args.hpp"
#include "fetch/fetch.hpp"
#include "html/html.hpp"
#include "input/input.hpp"
#include "json/json.hpp"
#include "platform/environment.hpp"
#include "platform/filesystem.hpp"
#include "search/search.hpp"
#include "security/redact.hpp"

namespace ainiux::agent {

struct IndexRefreshState {
    explicit IndexRefreshState(index::Options base_options)
        : options(std::move(base_options)) {
        options.cancellation = stop_source.token();
        options.interrupted = {};
        worker = std::thread([this] { run(); });
    }

    ~IndexRefreshState() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            stop_source.cancel();
        }
        ready.notify_all();
        if (worker.joinable()) worker.join();
    }

    std::size_t enqueue(const std::vector<std::string>& paths,
                        bool full_tree) {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping) return requested_generation;
        if (full_tree) {
            full_refresh = true;
        } else {
            for (const std::string& path : paths)
                if (!path.empty()) pending_paths.insert(path);
        }
        ++requested_generation;
        ready.notify_one();
        return requested_generation;
    }

    std::size_t generation() const {
        std::lock_guard<std::mutex> lock(mutex);
        return requested_generation;
    }

    std::size_t completed() const {
        std::lock_guard<std::mutex> lock(mutex);
        return completed_generation;
    }

    Error wait_for(std::size_t generation,
                   runtime::CancellationToken cancellation) const {
        std::unique_lock<std::mutex> lock(mutex);
        while (completed_generation < generation && !stopping) {
            if (cancellation.cancelled())
                return {ErrorCode::Cancelled,
                        "waiting for code-index refresh was cancelled"};
            ready.wait_for(lock, std::chrono::milliseconds(20));
        }
        if (completed_generation < generation)
            return {ErrorCode::Cancelled, "code-index refresh stopped"};
        return last_error;
    }

    void run() {
        for (;;) {
            std::vector<std::string> paths;
            bool full = false;
            bool completes_generation = true;
            std::size_t generation = 0;
            {
                std::unique_lock<std::mutex> lock(mutex);
                ready.wait(lock, [&] {
                    return stopping || full_refresh || !pending_paths.empty() ||
                           completed_generation < requested_generation;
                });
                if (stopping && completed_generation >= requested_generation)
                    return;
                // A known native mutation must be force-scanned before a broad
                // metadata pass, otherwise a same-size write on a coarse-mtime
                // filesystem could be mistaken for unchanged.
                if (full_refresh && !pending_paths.empty()) {
                    full = false;
                    paths.assign(pending_paths.begin(), pending_paths.end());
                    pending_paths.clear();
                    completes_generation = false;
                } else {
                    full = full_refresh;
                    if (!full)
                        paths.assign(pending_paths.begin(),
                                     pending_paths.end());
                    full_refresh = false;
                    pending_paths.clear();
                }
                generation = requested_generation;
            }
            index::Options refresh_options = options;
            refresh_options.update_paths = full ? std::vector<std::string>{}
                                                : paths;
            // Native mutations are authoritative even on filesystems whose
            // timestamp granularity cannot distinguish a same-size rapid write.
            refresh_options.force_rescan = !full && !paths.empty();
            index::RefreshStats stats;
            const Error error = index::refresh(refresh_options, stats);
            {
                std::lock_guard<std::mutex> lock(mutex);
                last_error = error;
                if (completes_generation)
                    completed_generation = generation;
            }
            ready.notify_all();
        }
    }

    index::Options options;
    runtime::CancellationSource stop_source;
    mutable std::mutex mutex;
    mutable std::condition_variable ready;
    std::set<std::string> pending_paths;
    bool full_refresh = false;
    bool stopping = false;
    std::size_t requested_generation = 0;
    std::size_t completed_generation = 0;
    Error last_error;
    std::thread worker;
};

ReadToolRegistry::ReadToolRegistry() = default;
ReadToolRegistry::~ReadToolRegistry() = default;
ReadToolRegistry::ReadToolRegistry(ReadToolRegistry&&) noexcept = default;
ReadToolRegistry& ReadToolRegistry::operator=(
    ReadToolRegistry&&) noexcept = default;

namespace {
namespace fs = std::filesystem;

json::Value object_value() { json::Value value; value.type = json::Value::Type::Object; return value; }
json::Value array_value() { json::Value value; value.type = json::Value::Type::Array; return value; }
json::Value string_value(const std::string& text) { json::Value value; value.type = json::Value::Type::String; value.string = text; return value; }
json::Value bool_value(bool boolean) { json::Value value; value.type = json::Value::Type::Bool; value.boolean = boolean; return value; }
json::Value number_value(double number) { json::Value value; value.type = json::Value::Type::Number; value.number = number; return value; }

std::string envelope(bool ok,
                     json::Value data,
                     const std::string& error_code,
                     const std::string& error_message,
                     const std::vector<std::string>& warnings,
                     bool truncated,
                     json::Value metadata = object_value()) {
    json::Value root = object_value();
    root.object["ok"] = bool_value(ok);
    root.object["data"] = std::move(data);
    if (error_code.empty()) root.object["error"] = json::Value{};
    else {
        json::Value error = object_value();
        error.object["code"] = string_value(error_code);
        error.object["message"] = string_value(error_message);
        root.object["error"] = std::move(error);
    }
    json::Value warning_array = array_value();
    for (const std::string& warning : warnings) warning_array.array.push_back(string_value(warning));
    root.object["warnings"] = std::move(warning_array);
    root.object["truncated"] = bool_value(truncated);
    root.object["metadata"] = std::move(metadata);
    return json::stringify(root);
}

std::string error_code_string(ErrorCode code) { return error_code_name(code); }

bool get_string(const json::Value& object, const std::string& key, std::string& output, bool required,
                std::string& error) {
    const json::Value* value = object.get(key);
    if (value == nullptr) {
        if (required) error = "missing required string argument: " + key;
        return !required;
    }
    if (!value->is_string()) { error = "argument " + key + " must be a string"; return false; }
    output = value->string;
    if (required && output.empty()) { error = "argument " + key + " must not be empty"; return false; }
    return true;
}

bool get_bool(const json::Value& object, const std::string& key, bool fallback, bool& output,
              std::string& error) {
    const json::Value* value = object.get(key);
    if (value == nullptr) { output = fallback; return true; }
    if (value->type != json::Value::Type::Bool) { error = "argument " + key + " must be a boolean"; return false; }
    output = value->boolean;
    return true;
}

bool get_size(const json::Value& object, const std::string& key, std::size_t fallback,
              std::size_t maximum, std::size_t& output, std::string& error) {
    const json::Value* value = object.get(key);
    if (value == nullptr) { output = fallback; return true; }
    if (value->type != json::Value::Type::Number || value->number < 0 ||
        value->number != static_cast<double>(static_cast<unsigned long long>(value->number)) ||
        value->number > static_cast<double>(maximum)) {
        error = "argument " + key + " must be an integer from 0 through " + std::to_string(maximum);
        return false;
    }
    output = static_cast<std::size_t>(value->number);
    return true;
}

// Map silent legacy aliases to advertised canonical tool names. Old names stay
// executable (zero schema cost) so prior transcripts and habit-trained models work.
std::string canonical_native_tool_name(const std::string& name) {
    if (name == "search_text" || name == "find") return "grep";
    if (name == "list_directory") return "list_dir";
    if (name == "project_overview") return "index_overview";
    if (name == "get_skeleton") return "file_outline";
    if (name == "search_web") return "web_search";
    return name;
}

std::string schema(const std::string& properties, const std::string& required = "") {
    return "{\"type\":\"object\",\"properties\":{" + properties + "},\"required\":[" + required +
           "],\"additionalProperties\":false}";
}

// Path components that must never appear in agent tool paths.
// Note: on POSIX, "~" is a *relative* component (not absolute), so tools must
// reject it explicitly — otherwise "~/code/x" becomes "$workspace/~/code/x".
bool is_forbidden_path_component(const std::string& value) {
    if (value.empty()) return false;  // skip empty parts from duplicate slashes
    if (value == "..") return true;
    if (value == "~" || (!value.empty() && value[0] == '~')) return true;  // ~ or ~user
    if (!value.empty() && value[0] == '$') return true;  // $HOME-style injection
    if (is_protected_state_dir_name(value)) return true;
    return false;
}

bool path_has_home_or_env_prefix(const std::string& path) {
    if (path.empty()) return false;
    if (path[0] == '~' || path[0] == '$') return true;
    return false;
}

bool safe_relative_path(const std::string& path) {
    if (path.empty() || path == ".") return true;
    if (path_has_home_or_env_prefix(path)) return false;
#if defined(_WIN32)
    if (!platform::validate_windows_path_syntax(path).ok()) return false;
#endif
    const fs::path candidate = fs::u8path(path);
    if (candidate.is_absolute()) return false;
    for (const fs::path& component : candidate) {
        if (is_forbidden_path_component(component.u8string())) return false;
    }
    return true;
}

// Clear, user-facing reason when a path is refused for workspace tools.
std::string unsafe_path_message(const std::string& path, const char* action = "manipulate") {
    if (path.empty())
        return std::string("Forbidden to ") + action + " files: path is empty.";
    const fs::path candidate = fs::u8path(path);
    if (candidate.is_absolute() || path_has_home_or_env_prefix(path)) {
        return std::string("Forbidden to ") + action +
               " files outside the project directory. Use a path relative to the "
               "project root only (not absolute, ~/…, or $ENV paths). Refused: " +
               path;
    }
    for (const fs::path& component : candidate) {
        const std::string value = component.u8string();
        if (value == "..") {
            return std::string("Forbidden to ") + action +
                   " files outside the project directory (path escapes with '..': " + path +
                   ").";
        }
        if (value == "~" || (!value.empty() && value[0] == '~')) {
            return std::string("Forbidden to ") + action +
                   " files outside the project directory. \"~\" is not expanded and is not "
                   "allowed as a path component (refused: " +
                   path + "). Use a project-relative path only.";
        }
        if (!value.empty() && value[0] == '$') {
            return std::string("Forbidden to ") + action +
                   " files via environment-style path components (refused: " + path + ").";
        }
        if (is_protected_state_dir_name(value)) {
            return std::string("Forbidden to ") + action +
                   " protected project metadata (" + value + " in " + path + ").";
        }
    }
    return std::string("Forbidden to ") + action +
           " files outside the project directory: " + path;
}

// Defense in depth: resolved absolute path must stay under the workspace root.
Error ensure_under_workspace(const fs::path& workspace,
                             const fs::path& candidate,
                             const std::string& display_path) {
    bool within = false;
    const Error identity_error = platform::path_is_within(
        workspace.u8string(), candidate.u8string(), within);
    if (!identity_error.ok())
        return {ErrorCode::FileWrite,
                "could not verify project path containment for " + display_path + ": " +
                    identity_error.message};
    if (!within) {
        return {ErrorCode::BadArgs,
                "Forbidden to manipulate files outside the project directory (resolved path "
                "escapes workspace). Refused: " +
                    display_path};
    }
    return ok_error();
}

bool resolved_path_is_under(const fs::path& root, const fs::path& candidate) {
    bool within = false;
    if (platform::path_is_within(root.u8string(), candidate.u8string(), within).ok())
        return within;
#if defined(_WIN32)
    // Windows containment must remain identity-based. A lexical fallback would
    // be case-sensitive and could misclassify aliases or reparse-point paths.
    return false;
#else
    const std::string root_s = root.generic_string();
    const std::string path_s = candidate.generic_string();
    return path_s == root_s ||
           (path_s.size() > root_s.size() && path_s.compare(0, root_s.size(), root_s) == 0 &&
            path_s[root_s.size()] == '/');
#endif
}

bool resolved_path_is_under_system_temp(const fs::path& candidate) {
    std::error_code ec;
    const fs::path temp =
        fs::weakly_canonical(fs::temp_directory_path(ec), ec);
    return !ec && !temp.empty() && resolved_path_is_under(temp, candidate);
}

bool contains_protected_metadata_component(const fs::path& path) {
    for (const fs::path& component : path) {
        if (is_protected_state_dir_name(component.u8string())) return true;
    }
    return false;
}

bool contains_symlink_component(const fs::path& path) {
    bool contains = false;
    const Error checked =
        platform::path_contains_link_or_reparse(path.u8string(), contains);
    if (checked.ok()) return contains;
#if defined(_WIN32)
    return true;
#else
    fs::path current = path.root_path();
    for (const fs::path& component : path.relative_path()) {
        current /= component;
        std::error_code ec;
        const fs::file_status status = fs::symlink_status(current, ec);
        if (ec || status.type() == fs::file_type::not_found) break;
        if (fs::is_symlink(status)) return true;
    }
    return false;
#endif
}

Error resolve_native_path(const fs::path& workspace,
                          const std::string& requested,
                          bool must_exist,
                          fs::path& resolved,
                          bool& external) {
    external = false;
    if (requested.empty() || requested[0] == '$')
        return {ErrorCode::BadArgs, "native path must be an exact non-empty path"};
#if defined(_WIN32)
    Error syntax = platform::validate_windows_path_syntax(requested);
    if (!syntax.ok()) return syntax;
#endif
    fs::path supplied;
    if (requested[0] == '~') {
        if (requested.size() < 2 || requested[1] != '/')
            return {ErrorCode::BadArgs, "~user paths are not supported"};
        const std::string home = platform::home_directory();
        if (home.empty())
            return {ErrorCode::BadArgs, "HOME is not set; provide an absolute path"};
        supplied = fs::u8path(home) / fs::u8path(requested.substr(2));
    } else {
        supplied = fs::u8path(requested);
        if (!supplied.is_absolute()) {
            if (!safe_relative_path(requested))
                return {ErrorCode::BadArgs,
                        "outside the project directory access requires an exact absolute or ~/ path"};
            supplied = workspace / supplied;
        }
    }
    std::error_code ec;
    const fs::path root = fs::canonical(workspace, ec);
    if (ec) return {ErrorCode::FileRead, "could not resolve project root"};
    const fs::path supplied_absolute = fs::absolute(supplied, ec).lexically_normal();
    if (!ec && contains_symlink_component(supplied_absolute))
        return {ErrorCode::BadArgs,
                "refusing symlink or reparse-point path: " + supplied_absolute.u8string()};
    resolved = fs::weakly_canonical(supplied_absolute, ec);
    if (ec || resolved.empty())
        return {ErrorCode::FileRead,
                "could not resolve native path " + requested + ": " + ec.message()};
    external = !resolved_path_is_under(root, resolved);
    if (contains_protected_metadata_component(resolved))
        return {ErrorCode::BadArgs,
                "refusing access to protected agent metadata: " +
                    resolved.generic_u8string()};
    if (must_exist) {
        const fs::file_status status = fs::symlink_status(resolved, ec);
        if (ec || status.type() == fs::file_type::not_found)
            return {ErrorCode::FileRead,
                    "path does not exist: " + resolved.generic_u8string()};
        if (fs::is_symlink(status))
            return {ErrorCode::BadArgs,
                    "refusing symlink path: " + resolved.generic_u8string()};
    }
    return ok_error();
}

bool is_broad_removal_root(const fs::path& workspace, const fs::path& target) {
    std::error_code ec;
    const fs::path root = fs::canonical(workspace, ec);
    if (ec) return true;
    const fs::path filesystem_root = target.root_path();
    if (target == filesystem_root || target == root ||
        resolved_path_is_under(target, root))
        return true;
    const std::string home = platform::home_directory();
    if (!home.empty()) {
        const fs::path home_root = fs::weakly_canonical(fs::u8path(home), ec);
        if (!ec && target == home_root) return true;
    }
    const fs::path temp_root =
        fs::weakly_canonical(fs::temp_directory_path(ec), ec);
    return !ec && target == temp_root;
}

Error resolve_external_file_path(const fs::path& workspace,
                                 const std::string& requested,
                                 bool must_exist,
                                 fs::path& resolved) {
    resolved.clear();
    if (requested.empty())
        return {ErrorCode::BadArgs, "outside-project file path must not be empty"};
#if defined(_WIN32)
    Error syntax = platform::validate_windows_path_syntax(requested);
    if (!syntax.ok()) return syntax;
#endif
    if (requested[0] == '$')
        return {ErrorCode::BadArgs,
                "environment-variable paths are not expanded for external file access; "
                "provide the exact absolute path"};

    fs::path supplied;
    if (requested[0] == '~') {
        if (requested.size() < 2 || requested[1] != '/')
            return {ErrorCode::BadArgs,
                    "~user paths are not supported; provide the exact absolute path"};
        const std::string home = platform::home_directory();
        if (home.empty())
            return {ErrorCode::BadArgs,
                    "could not expand ~/ because HOME is not set; provide an absolute path"};
        supplied = fs::u8path(home) / fs::u8path(requested.substr(2));
    } else {
        supplied = fs::u8path(requested);
        if (!supplied.is_absolute()) {
            if (!safe_relative_path(requested))
                return {ErrorCode::BadArgs,
                        "outside the project directory access requires an exact absolute or ~/ path"};
            supplied = workspace / supplied;
        }
    }

    std::error_code ec;
    const fs::path root = fs::weakly_canonical(fs::absolute(workspace, ec), ec);
    if (ec || root.empty())
        return {ErrorCode::FileRead,
                "could not resolve project root before external file approval: " + ec.message()};
    const fs::path supplied_absolute = fs::absolute(supplied, ec).lexically_normal();
    if (!ec && contains_symlink_component(supplied_absolute))
        return {ErrorCode::BadArgs,
                "refusing symlink or reparse-point external path: " +
                    supplied_absolute.u8string()};
    resolved = fs::weakly_canonical(supplied_absolute, ec);
    if (ec || resolved.empty())
        return {ErrorCode::FileRead,
                "could not resolve external file path " + requested + ": " + ec.message()};
    if (resolved_path_is_under(root, resolved))
        return {ErrorCode::BadArgs,
                unsafe_path_message(requested) +
                    " Protected or malformed paths inside the project cannot be approved as "
                    "external access."};
    if (contains_protected_metadata_component(resolved))
        return {ErrorCode::BadArgs,
                "refusing access to protected agent metadata outside the project: " +
                    resolved.generic_u8string()};
    if (must_exist) {
        const fs::file_status status = fs::status(resolved, ec);
        if (ec || !fs::exists(status))
            return {ErrorCode::FileRead,
                    "external file does not exist or cannot be inspected: " +
                        resolved.generic_u8string()};
        if (!fs::is_regular_file(status))
            return {ErrorCode::FileRead,
                    "external read path must be a regular file: " + resolved.generic_u8string()};
    }
    return ok_error();
}

Error resolve_external_directory_path(const fs::path& workspace,
                                      const std::string& requested,
                                      fs::path& resolved) {
    if (requested.empty())
        return {ErrorCode::BadArgs, "outside-project directory path must not be empty"};
#if defined(_WIN32)
    Error syntax = platform::validate_windows_path_syntax(requested);
    if (!syntax.ok()) return syntax;
#endif
    if (requested[0] == '$')
        return {ErrorCode::BadArgs,
                "environment-variable paths are not expanded; provide an exact path"};
    fs::path supplied;
    if (requested[0] == '~') {
        if (requested.size() < 2 || requested[1] != '/')
            return {ErrorCode::BadArgs, "~user paths are not supported"};
        const std::string home = platform::home_directory();
        if (home.empty())
            return {ErrorCode::BadArgs, "HOME is not set; provide an absolute path"};
        supplied = fs::u8path(home) / fs::u8path(requested.substr(2));
    } else {
        supplied = fs::u8path(requested);
        if (!supplied.is_absolute()) {
            if (!safe_relative_path(requested))
                return {ErrorCode::BadArgs,
                        "outside the project directory access requires an exact absolute or ~/ path"};
            supplied = workspace / supplied;
        }
    }
    std::error_code ec;
    const fs::path root = fs::canonical(workspace, ec);
    if (ec) return {ErrorCode::FileRead, "could not resolve project root"};
    if (contains_symlink_component(fs::absolute(supplied, ec).lexically_normal()))
        return {ErrorCode::BadArgs,
                "refusing symlink or reparse-point external directory: " +
                    supplied.u8string()};
    resolved = fs::canonical(supplied, ec);
    if (ec || !fs::is_directory(resolved, ec))
        return {ErrorCode::FileRead,
                "external directory does not exist or cannot be inspected: " +
                    supplied.generic_u8string()};
    if (resolved_path_is_under(root, resolved))
        return {ErrorCode::BadArgs,
                "protected or malformed project paths cannot be approved as external access"};
    if (contains_protected_metadata_component(resolved))
        return {ErrorCode::BadArgs,
                "refusing access to protected agent metadata outside the project"};
    return ok_error();
}

Error ensure_approved_external_path_unchanged(const fs::path& approved,
                                              const char* action,
                                              ErrorCode error_code) {
    std::error_code ec;
    const fs::path current = fs::weakly_canonical(fs::absolute(approved, ec), ec);
    if (ec || current.empty())
        return {error_code,
                std::string("could not re-resolve approved external path before ") + action +
                    ": " + approved.generic_u8string() +
                    (ec ? " (" + ec.message() + ")" : std::string())};
    if (current != approved)
        return {error_code,
                std::string("approved external path changed while waiting; refusing to ") +
                    action + ". Approved: " + approved.generic_u8string() +
                    "; now resolves to: " + current.generic_u8string()};
    return ok_error();
}

std::size_t utf8_prefix(const std::string& text, std::size_t limit) {
    if (text.size() <= limit) return text.size();
    std::size_t end = limit;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) --end;
    return end;
}

std::vector<std::string> split_lines(const std::string& source) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < source.size()) {
        const std::size_t newline = source.find('\n', start);
        const std::size_t end = newline == std::string::npos ? source.size() : newline + 1;
        lines.push_back(source.substr(start, end - start));
        start = end;
    }
    return lines;
}

std::string number_source_lines(const SourceRange& range) {
    std::ostringstream numbered;
    const std::vector<std::string> lines = split_lines(range.content);
    for (std::size_t index = 0; index < lines.size(); ++index)
        numbered << (range.start_line + index) << ": " << lines[index];
    return numbered.str();
}

std::string regex_escape(const std::string& text) {
    std::string output;
    for (char ch : text) {
        if (std::string(R"(.^$|()[]{}+\)").find(ch) != std::string::npos) output.push_back('\\');
        output.push_back(ch);
    }
    return output;
}

void expand_braces(const std::string& pattern, std::vector<std::string>& output) {
    const std::size_t open = pattern.find('{');
    if (open == std::string::npos) { output.push_back(pattern); return; }
    const std::size_t close = pattern.find('}', open + 1);
    if (close == std::string::npos) { output.push_back(pattern); return; }
    const std::string alternatives = pattern.substr(open + 1, close - open - 1);
    std::size_t start = 0;
    while (start <= alternatives.size()) {
        const std::size_t comma = alternatives.find(',', start);
        const std::string alternative = alternatives.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        expand_braces(pattern.substr(0, open) + alternative + pattern.substr(close + 1), output);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
}

// Normalize workspace-relative glob patterns and indexed paths to a common form:
// forward slashes, no leading "./", no trailing slash (except bare ".").
std::string normalize_glob_path(std::string path) {
    for (char& ch : path) {
        if (ch == '\\') ch = '/';
    }
    while (path.size() >= 2 && path[0] == '.' && path[1] == '/') path.erase(0, 2);
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    return path;
}

// Convert a single glob alternative to a std::regex.
// Important: "**/name" must match root-level "name" as well as "dir/name".
// The previous converter mapped "**/" to ".*/", which required a slash and
// therefore missed workspace-root files (DeepSeek/Gemma often emit **/hello.py).
std::regex glob_expression(const std::string& pattern) {
    std::string output = "^";
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char ch = pattern[i];
        if (ch == '*') {
            if (i + 1 < pattern.size() && pattern[i + 1] == '*') {
                ++i;  // consume second '*'
                // "**/..." → zero or more directories (including none at workspace root).
                // Use (?:.*/)? so "**/hello.py" matches "hello.py", "src/hello.py",
                // and "a/b/hello.py". A single (?:.+/)? only covers one directory.
                if (i + 1 < pattern.size() && pattern[i + 1] == '/') {
                    ++i;
                    output += "(?:.*/)?";
                } else if (i + 1 == pattern.size()) {
                    // trailing "**" matches the rest of the path
                    output += ".*";
                } else {
                    // bare "**" mid-pattern without slash (unusual)
                    output += ".*";
                }
            } else {
                output += "[^/]*";
            }
        } else if (ch == '?') {
            output += "[^/]";
        } else {
            output += regex_escape(std::string(1, ch));
        }
    }
    output += "$";
    return std::regex(output, std::regex::optimize);
}

bool glob_matches(const std::string& path, const std::string& pattern) {
    if (pattern.empty()) return true;
    const std::string normalized_path = normalize_glob_path(path);
    std::vector<std::string> alternatives;
    expand_braces(normalize_glob_path(pattern), alternatives);
    for (std::string alternative : alternatives) {
        alternative = normalize_glob_path(alternative);
        if (alternative.empty() || alternative == ".") {
            if (normalized_path.empty() || normalized_path == ".") return true;
            continue;
        }
        try {
            if (std::regex_match(normalized_path, glob_expression(alternative))) return true;
            // Basename-only patterns: "hello.py" should also match "src/hello.py"
            // when the model omits **/. Prefer explicit **/ when possible; this
            // is a soft compatibility path for patterns with no slash.
            if (alternative.find('/') == std::string::npos &&
                alternative.find("**") == std::string::npos) {
                const std::string recursive = "**/" + alternative;
                if (std::regex_match(normalized_path, glob_expression(recursive))) return true;
            }
        } catch (const std::regex_error&) {
            return false;
        }
    }
    return false;
}

// True when file is the search root itself or a contained descendant.
// "src" matches "src" and "src/a.ts"; not "src-extra/a.ts".
bool path_in_search_root(const std::string& file, const std::string& root) {
    if (root.empty() || root == ".") return true;
    const std::string normalized = normalize_glob_path(file);
    if (normalized == root) return true;
    return normalized.size() > root.size() &&
           normalized.compare(0, root.size(), root) == 0 &&
           normalized[root.size()] == '/';
}

bool has_unescaped_alternation(const std::string& query) {
    bool escaped = false;
    for (std::size_t index = 0; index < query.size(); ++index) {
        const char ch = query[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch != '|' || index == 0 || index + 1 >= query.size() ||
            query[index - 1] == '|' || query[index + 1] == '|')
            continue;
        bool left_content = false;
        for (std::size_t left = index; left > 0;) {
            --left;
            if (query[left] == '|') break;
            if (!std::isspace(static_cast<unsigned char>(query[left])))
                left_content = true;
        }
        bool right_content = false;
        for (std::size_t right = index + 1; right < query.size(); ++right) {
            if (query[right] == '|') break;
            if (!std::isspace(static_cast<unsigned char>(query[right])))
                right_content = true;
        }
        if (left_content && right_content) return true;
    }
    return false;
}

// Parse one ripgrep line-oriented hit: "path:line:text" (match) or
// "path-line-text" (context with -C). Paths are expected not to contain ':'.
bool parse_rg_output_line(const std::string& line,
                          std::string& path,
                          std::size_t& line_number,
                          std::string& text,
                          bool& is_match) {
    path.clear();
    text.clear();
    line_number = 0;
    is_match = false;
    if (line.empty() || line == "--") return false;
    // Prefer match form path:line:text (first colon after path, second after line).
    const std::size_t first = line.find(':');
    if (first != std::string::npos && first > 0) {
        const std::size_t second = line.find(':', first + 1);
        if (second != std::string::npos && second > first + 1) {
            bool digits = true;
            for (std::size_t i = first + 1; i < second; ++i) {
                if (line[i] < '0' || line[i] > '9') {
                    digits = false;
                    break;
                }
            }
            if (digits) {
                path = normalize_glob_path(line.substr(0, first));
                try {
                    line_number = static_cast<std::size_t>(std::stoul(line.substr(
                        first + 1, second - first - 1)));
                } catch (...) {
                    return false;
                }
                if (line_number == 0) return false;
                text = line.substr(second + 1);
                is_match = true;
                return true;
            }
        }
    }
    // Context form path-line-text (hyphen separators).
    const std::size_t hyphen = line.find('-');
    if (hyphen == std::string::npos || hyphen == 0) return false;
    const std::size_t hyphen2 = line.find('-', hyphen + 1);
    if (hyphen2 == std::string::npos || hyphen2 <= hyphen + 1) return false;
    bool digits = true;
    for (std::size_t i = hyphen + 1; i < hyphen2; ++i) {
        if (line[i] < '0' || line[i] > '9') {
            digits = false;
            break;
        }
    }
    if (!digits) return false;
    path = normalize_glob_path(line.substr(0, hyphen));
    try {
        line_number = static_cast<std::size_t>(
            std::stoul(line.substr(hyphen + 1, hyphen2 - hyphen - 1)));
    } catch (...) {
        return false;
    }
    if (line_number == 0) return false;
    text = line.substr(hyphen2 + 1);
    is_match = false;
    return true;
}

std::string lowercase(std::string text) {
    for (char& ch : text) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    return text;
}

std::string redact_source_secrets(std::string text,
                                  const std::vector<std::string>& secrets) {
    for (const std::string& secret : secrets) {
        if (secret.empty()) continue;
        std::string replacement(secret.size(), '*');
        static const std::string marker = "[REDACTED]";
        if (replacement.size() >= marker.size())
            replacement.replace(0, marker.size(), marker);
        std::size_t position = 0;
        while ((position = text.find(secret, position)) != std::string::npos) {
            text.replace(position, secret.size(), replacement);
            position += replacement.size();
        }
    }
    return text;
}

std::string normalized_workspace_path(const std::string& cwd, const std::string& path) {
    fs::path combined;
    if (!cwd.empty() && cwd != ".") combined /= fs::u8path(cwd);
    combined /= fs::u8path(path);
    std::string normalized = combined.lexically_normal().generic_u8string();
    if (normalized == ".") normalized.clear();
    while (normalized.rfind("./", 0) == 0) normalized.erase(0, 2);
    return normalized;
}

// Index-only eligibility (code files known to the snapshot).
bool eligible_indexed_path(const index::Snapshot& snapshot,
                           const std::string& path,
                           bool allow_directory) {
    if (path.empty()) return true;
    const std::string prefix = path + "/";
    for (const index::IndexedFile& file : snapshot.files) {
        if (file.status != "indexed") continue;
        if (file.path == path || (allow_directory && file.path.rfind(prefix, 0) == 0)) return true;
    }
    return false;
}

// Real workspace visibility for listing/inspection: any safe non-symlink path that
// exists on disk (including empty directories and non-source files like #backup#).
// Falls back to the index when the path is not on disk yet but is snapshot-known.
bool visible_workspace_path(const index::Snapshot& snapshot,
                            const std::string& path,
                            bool allow_directory) {
    if (path.empty()) return true;
    if (!safe_relative_path(path)) return false;
    std::error_code ec;
    const fs::path absolute = fs::u8path(snapshot.workspace) / fs::u8path(path);
    const fs::file_status status = fs::symlink_status(absolute, ec);
    if (!ec && status.type() != fs::file_type::not_found) {
        if (fs::is_symlink(status)) return false;
        if (fs::is_directory(status)) return allow_directory;
        if (fs::is_regular_file(status)) return true;
        return false;
    }
    return eligible_indexed_path(snapshot, path, allow_directory);
}

bool is_protected_listing_name(const std::string& name) {
    return is_protected_state_dir_name(name);
}

bool path_allowed_for_command(const index::Snapshot& snapshot,
                              const std::string& path,
                              bool allow_directory,
                              bool index_only) {
    if (index_only) return eligible_indexed_path(snapshot, path, allow_directory);
    return visible_workspace_path(snapshot, path, allow_directory);
}

Error validate_command_workspace_paths(const index::Snapshot& snapshot,
                                       const std::vector<std::string>& arguments,
                                       const std::string& cwd,
                                       bool index_only,
                                       bool allow_external,
                                       bool& uses_external) {
    uses_external = false;
    std::error_code ec;
    const fs::path root = fs::canonical(snapshot.workspace, ec);
    if (ec) return {ErrorCode::FileRead, "could not resolve run_command workspace"};
    fs::path requested_cwd =
        cwd.empty() ? root
                    : (fs::u8path(cwd).is_absolute() ? fs::u8path(cwd) : root / fs::u8path(cwd));
    const fs::path canonical_cwd = fs::canonical(requested_cwd, ec);
    if (ec || !fs::is_directory(canonical_cwd, ec) ||
        contains_symlink_component(fs::absolute(requested_cwd).lexically_normal()) ||
        contains_protected_metadata_component(canonical_cwd))
        return {ErrorCode::BadArgs,
                "run_command cwd must be an existing safe canonical directory"};
    uses_external = !resolved_path_is_under(root, canonical_cwd);
    if (uses_external && !allow_external)
        return {ErrorCode::BadArgs, "run_command cwd is outside the workspace"};
    const std::string normalized_cwd =
        uses_external ? canonical_cwd.generic_u8string()
                      : fs::relative(canonical_cwd, root, ec).generic_u8string();
    if (ec)
        return {ErrorCode::BadArgs, "could not normalize run_command cwd"};
    const std::string workspace_cwd =
        normalized_cwd == "." ? std::string() : normalized_cwd;
    if (!uses_external && (!safe_relative_path(workspace_cwd) ||
        !path_allowed_for_command(snapshot, workspace_cwd, true, index_only))
        )
        return {ErrorCode::BadArgs,
                index_only ? "run_command cwd has no eligible indexed content"
                           : "run_command cwd is outside the workspace or not visible"};
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const std::string& argument = arguments[index];
        if (argument.empty() || argument.front() == '-') continue;
        if (allow_external && fs::u8path(argument).is_absolute()) {
            const fs::path argument_path = fs::u8path(argument);
            const fs::path canonical = fs::weakly_canonical(argument_path, ec);
            const fs::file_status literal_status = fs::symlink_status(argument_path, ec);
            if (ec || canonical.empty() || !fs::exists(canonical, ec) ||
                fs::is_symlink(literal_status) ||
                contains_symlink_component(fs::absolute(argument_path).lexically_normal()) ||
                contains_protected_metadata_component(canonical))
                return {ErrorCode::BadArgs,
                        "run_command external path operand is not an existing safe path: " +
                            argument};
            if (!resolved_path_is_under(root, canonical)) uses_external = true;
            continue;
        }
        if (uses_external) continue;
        const std::string relative = normalized_workspace_path(workspace_cwd, argument);
        if (!safe_relative_path(relative))
            return {ErrorCode::BadArgs, unsafe_path_message(relative, "access via run_command")};
        std::error_code ec;
        const fs::file_status status = fs::symlink_status(
            fs::u8path(snapshot.workspace) / fs::u8path(relative), ec);
        if (!ec && status.type() != fs::file_type::not_found &&
            (contains_symlink_component(
                 fs::absolute(fs::u8path(snapshot.workspace) / fs::u8path(relative))
                     .lexically_normal()) ||
             !path_allowed_for_command(snapshot, relative, fs::is_directory(status), index_only)))
            return {ErrorCode::BadArgs,
                    index_only
                        ? ("run_command path is not eligible in the completed index snapshot: " +
                           relative)
                        : ("run_command path is not a visible workspace entry: " + relative)};
    }
    return ok_error();
}

Error validate_vetted_read_only_paths(const index::Snapshot& snapshot,
                                      const ReadOnlyCommandAssessment& assessment,
                                      const std::string& cwd,
                                      bool& outside_project) {
    outside_project = false;
    std::error_code ec;
    const fs::path root = fs::canonical(snapshot.workspace, ec);
    if (ec || root.empty())
        return {ErrorCode::FileRead,
                "could not resolve project root for read-only command validation"};
    const fs::path requested_cwd =
        cwd.empty() ? root
                    : (fs::u8path(cwd).is_absolute() ? fs::u8path(cwd) : root / fs::u8path(cwd));
    const fs::path canonical_cwd = fs::canonical(requested_cwd, ec);
    if (ec || !fs::is_directory(canonical_cwd, ec) ||
        contains_symlink_component(fs::absolute(requested_cwd).lexically_normal()) ||
        contains_protected_metadata_component(requested_cwd.lexically_normal()) ||
        contains_protected_metadata_component(canonical_cwd))
        return {ErrorCode::BadArgs,
                "vetted read-only command cwd must be an existing safe directory"};
    if (!resolved_path_is_under(root, canonical_cwd)) outside_project = true;

    for (const std::string& operand : assessment.path_operands) {
        if (operand.empty() || operand == "-") continue;
        const fs::path supplied = fs::u8path(operand);
        const fs::path requested =
            supplied.is_absolute() ? supplied : canonical_cwd / supplied;
        if (contains_protected_metadata_component(requested.lexically_normal()))
            return {ErrorCode::BadArgs,
                    "vetted read-only command refused protected metadata path: " +
                        operand};
        const fs::file_status literal_status = fs::symlink_status(requested, ec);
        if (ec || literal_status.type() == fs::file_type::not_found)
            return {ErrorCode::BadArgs,
                    "vetted read-only command path does not exist: " + operand};
        if (fs::is_symlink(literal_status))
            return {ErrorCode::BadArgs,
                    "vetted read-only command refused symlink path: " + operand};
        if (contains_symlink_component(fs::absolute(requested).lexically_normal()))
            return {ErrorCode::BadArgs,
                    "vetted read-only command refused symlink path component: " +
                        operand};
        const fs::path canonical = fs::canonical(requested, ec);
        if (ec || canonical.empty() ||
            contains_protected_metadata_component(canonical))
            return {ErrorCode::BadArgs,
                    "could not safely resolve vetted read-only command path: " +
                        operand};
        if (!resolved_path_is_under(root, canonical)) outside_project = true;
    }
    return ok_error();
}

std::string filter_path_lines(const index::Snapshot& snapshot,
                              const std::string& cwd,
                              const std::string& text,
                              const std::string& command,
                              const std::vector<std::string>& arguments,
                              bool& filtered,
                              bool index_only) {
    std::string listing_base = cwd;
    if (command == "ls") {
        for (std::size_t index = 1; index < arguments.size(); ++index) {
            if (arguments[index].empty() || arguments[index].front() == '-') continue;
            const std::string operand = normalized_workspace_path(cwd, arguments[index]);
            std::error_code ec;
            if (fs::is_directory(fs::u8path(snapshot.workspace) / fs::u8path(operand), ec))
                listing_base = operand;
            break;
        }
    }
    std::ostringstream output;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t newline = text.find('\n', start);
        const std::size_t end = newline == std::string::npos ? text.size() : newline;
        const std::string line = text.substr(start, end - start);
        std::string candidate;
        bool allow_directory = command == "find" || command == "ls";
        if (command == "rg" || command == "grep") {
            const std::size_t colon = line.find(':');
            if (colon != std::string::npos) candidate = line.substr(0, colon);
        } else {
            candidate = line;
        }
        const std::string relative = normalized_workspace_path(
            command == "ls" ? listing_base : cwd, candidate);
        if (!candidate.empty() && safe_relative_path(relative) &&
            path_allowed_for_command(snapshot, relative, allow_directory, index_only)) {
            output << line;
            if (newline != std::string::npos) output << '\n';
        } else if (!line.empty()) {
            filtered = true;
        }
        if (newline == std::string::npos) break;
        start = newline + 1;
    }
    return output.str();
}

}  // namespace

std::string tool_error_result(const std::string& code, const std::string& message) {
    return envelope(false, json::Value{}, code, message, {}, false);
}

Error ReadToolRegistry::create(index::Options index_options,
                               index::Snapshot snapshot,
                               std::vector<std::string> secrets,
                               ReadToolRegistry& registry,
                               ToolRegistryOptions options) {
    if (snapshot.workspace.empty()) return {ErrorCode::Internal, "tool registry requires a completed index snapshot"};
    ReadToolRegistry loaded;
    loaded.index_options_ = std::move(index_options);
    loaded.snapshot_ = std::move(snapshot);
    loaded.secrets_ = std::move(secrets);
    loaded.mutation_policy_ = options.mutation_policy;
    loaded.allow_network_ = options.allow_network;
    loaded.history_backup_ = options.history_backup;
    loaded.fetch_options_ = options.fetch_options;
    loaded.search_options_ = options.search_options;
    loaded.on_guard_ask_ = std::move(options.on_guard_ask);
    loaded.goal_hooks_ = std::move(options.goal_hooks);
    loaded.vision_hooks_ = std::move(options.vision_hooks);
    loaded.permission_mode_ = options.permission_mode;
    loaded.permission_controls_ = options.permission_controls;
    loaded.indexing_enabled_ = options.indexing_enabled;
    loaded.index_access_mode_ = options.index_access_mode;
    if (loaded.mutation_policy_ != MutationPolicy::Disabled &&
        loaded.indexing_enabled_) {
        // Session preparation runs as its own cancellable job. That job's token
        // is valid only while the initial refresh/snapshot load is running; a
        // long-lived agent registry must use the cancellation token supplied by
        // each later tool call instead of retaining the completed job token.
        loaded.index_options_.cancellation = runtime::CancellationToken();
        loaded.index_options_.interrupted = {};
        try {
            loaded.index_refresh_ =
                std::make_unique<IndexRefreshState>(loaded.index_options_);
        } catch (const std::exception& exception) {
            return {ErrorCode::Internal,
                    "could not start code-index refresh worker: " +
                        std::string(exception.what())};
        }
    }
    registry = std::move(loaded);
    registry.rebuild_file_map();
    (void)registry.purge_expired_history_backups();
    return ok_error();
}

Error ReadToolRegistry::create_without_index(
    std::string workspace,
    std::vector<std::string> secrets,
    ReadToolRegistry& registry,
    ToolRegistryOptions options) {
    index::Options index_options;
    index_options.workspace = std::move(workspace);
    return create_without_index(std::move(index_options), std::move(secrets),
                                registry, std::move(options));
}

Error ReadToolRegistry::create_without_index(
    index::Options index_options,
    std::vector<std::string> secrets,
    ReadToolRegistry& registry,
    ToolRegistryOptions options) {
    if (index_options.workspace.empty())
        return {ErrorCode::Internal,
                "tool registry requires a canonical workspace"};
    index::Snapshot empty;
    empty.workspace = index_options.workspace;
    options.indexing_enabled = false;
    options.index_access_mode = IndexAccessMode::Disabled;
    return create(std::move(index_options), std::move(empty),
                  std::move(secrets), registry, std::move(options));
}

Error ReadToolRegistry::create_lazy(
    index::Options index_options,
    std::vector<std::string> secrets,
    ReadToolRegistry& registry,
    ToolRegistryOptions options) {
    if (index_options.workspace.empty())
        return {ErrorCode::Internal,
                "lazy index registry requires a canonical workspace"};
    index::Snapshot hints;
    hints.workspace = index_options.workspace;
    options.indexing_enabled = true;
    options.index_access_mode = IndexAccessMode::LazyHints;
    return create(std::move(index_options), std::move(hints),
                  std::move(secrets), registry, std::move(options));
}

Error ReadToolRegistry::enable_persistent_index(
    index::Options index_options,
    index::Snapshot snapshot) {
    if (indexing_enabled_) return ok_error();
    if (mutation_policy_ == MutationPolicy::Disabled)
        return {ErrorCode::UnsupportedFeature,
                "enabling code indexing requires an Agent session"};
    if (index_options.workspace.empty() || snapshot.workspace.empty() ||
        index_options.workspace != snapshot.workspace ||
        snapshot.workspace != snapshot_.workspace)
        return {ErrorCode::Internal,
                "completed code index does not match the active Agent workspace"};

    // The foreground slash-command token is valid only while creation runs.
    // Later mutation/task refreshes receive their own cancellation tokens.
    index_options.cancellation = runtime::CancellationToken();
    index_options.interrupted = {};
    std::unique_ptr<IndexRefreshState> refresh;
    try {
        refresh = std::make_unique<IndexRefreshState>(index_options);
    } catch (const std::exception& exception) {
        return {ErrorCode::Internal,
                "could not start code-index refresh worker: " +
                    std::string(exception.what())};
    }

    index_options_ = std::move(index_options);
    snapshot_ = std::move(snapshot);
    index_refresh_ = std::move(refresh);
    loaded_index_generation_ = 0;
    indexing_enabled_ = true;
    rebuild_file_map();
    return ok_error();
}

Error ReadToolRegistry::enable_lazy_index(index::Options index_options) {
    if (indexing_enabled_) return ok_error();
    if (mutation_policy_ == MutationPolicy::Disabled)
        return {ErrorCode::UnsupportedFeature,
                "enabling code indexing requires an Agent session"};
    if (index_options.workspace.empty() ||
        index_options.workspace != snapshot_.workspace)
        return {ErrorCode::Internal,
                "completed code index does not match the active Agent workspace"};
    index_options.cancellation = runtime::CancellationToken();
    index_options.interrupted = {};
    std::unique_ptr<IndexRefreshState> refresh;
    try {
        refresh = std::make_unique<IndexRefreshState>(index_options);
    } catch (const std::exception& exception) {
        return {ErrorCode::Internal,
                "could not start code-index refresh worker: " +
                    std::string(exception.what())};
    }
    index_options_ = std::move(index_options);
    index_refresh_ = std::move(refresh);
    loaded_index_generation_ = 0;
    indexing_enabled_ = true;
    index_access_mode_ = IndexAccessMode::LazyHints;
    snapshot_.files.clear();
    snapshot_.symbols.clear();
    snapshot_.language_totals.clear();
    rebuild_file_map();
    return ok_error();
}

void ReadToolRegistry::enqueue_background_freshness() const {
    if (index_refresh_) (void)index_refresh_->enqueue({}, true);
}

void ReadToolRegistry::rebuild_file_map() const {
    files_.clear();
    for (const index::IndexedFile& file : snapshot_.files) files_[file.path] = &file;
}

std::size_t ReadToolRegistry::queue_index_paths(
    const std::vector<std::string>& paths,
    bool full_tree) const {
    if (index_refresh_)
        return index_refresh_->enqueue(paths, full_tree);
    return 0;
}

void ReadToolRegistry::merge_index_overlay() const {
    if (index_access_mode_ != IndexAccessMode::LazyHints ||
        index_overlay_.empty())
        return;
    auto covered = [](const std::string& path, const std::string& root) {
        return path == root ||
               (path.size() > root.size() &&
                path.compare(0, root.size(), root) == 0 &&
                path[root.size()] == '/');
    };
    for (const auto& item : index_overlay_) {
        const std::string& path = item.first;
        const IndexOverlayEntry& overlay = item.second;
        snapshot_.files.erase(
            std::remove_if(snapshot_.files.begin(), snapshot_.files.end(),
                           [&](const index::IndexedFile& file) {
                               return covered(file.path, path);
                           }),
            snapshot_.files.end());
        snapshot_.symbols.erase(
            std::remove_if(snapshot_.symbols.begin(), snapshot_.symbols.end(),
                           [&](const index::IndexedSymbol& symbol) {
                               return covered(symbol.path, path);
                           }),
            snapshot_.symbols.end());
        if (!overlay.removed) {
            snapshot_.files.push_back(overlay.file);
            snapshot_.symbols.insert(snapshot_.symbols.end(),
                                     overlay.symbols.begin(),
                                     overlay.symbols.end());
        }
    }
    std::sort(snapshot_.files.begin(), snapshot_.files.end(),
              [](const index::IndexedFile& left,
                 const index::IndexedFile& right) {
                  return left.path < right.path;
              });
}

Error ReadToolRegistry::refresh_persistent_index(
    bool full_tree,
    runtime::CancellationToken cancellation) const {
    if (!index_refresh_) {
        return !indexing_enabled_
                   ? ok_error()
                   : full_tree
                   ? Error{ErrorCode::UnsupportedFeature,
                           "code-index persistence is disabled for this read-only tool session"}
                   : ok_error();
    }
    const std::size_t generation =
        full_tree ? index_refresh_->enqueue({}, true)
                  : index_refresh_->generation();
    Error error = index_refresh_->wait_for(generation, cancellation);
    if (!error.ok()) return error;
    if (generation <= loaded_index_generation_) return ok_error();
    if (index_access_mode_ == IndexAccessMode::LazyHints) {
        loaded_index_generation_ = generation;
        for (auto item = index_overlay_.begin();
             item != index_overlay_.end();) {
            if (item->second.revision <= generation)
                item = index_overlay_.erase(item);
            else
                ++item;
        }
        return ok_error();
    }
    index::Options options = index_options_;
    options.cancellation = cancellation;
    index::Snapshot next;
    error = index::load_snapshot(options, next);
    if (!error.ok()) return error;
    snapshot_ = std::move(next);
    loaded_index_generation_ = generation;
    rebuild_file_map();
    return ok_error();
}

GuardApprovalDecision ReadToolRegistry::request_guard_approval(
    const GuardApprovalRequest& request,
    runtime::CancellationToken cancellation) const {
    if (permission_mode_ == PermissionMode::Yolo)
        return cancellation.cancelled() ? GuardApprovalDecision::Cancelled
                                        : GuardApprovalDecision::Allow;
    if (!on_guard_ask_) return GuardApprovalDecision::Deny;
    return on_guard_ask_(request, cancellation);
}

GuardApprovalDecision ReadToolRegistry::request_permission(
    const std::string& tool_name,
    const std::string& preview,
    const std::vector<std::string>& arguments,
    bool outside_project,
    bool under_system_temp,
    bool write,
    bool destructive,
    const std::string& specific_rule,
    const std::string& specific_message,
    runtime::CancellationToken cancellation) const {
    if (cancellation.cancelled()) return GuardApprovalDecision::Cancelled;
    if (!permission_controls_) {
        if (!outside_project) return GuardApprovalDecision::Allow;
        GuardApprovalRequest legacy;
        legacy.tool_name = tool_name;
        legacy.command_preview = preview;
        legacy.rule_id = specific_rule.empty() ? "ask_on_external_native_access"
                                               : specific_rule;
        legacy.message = specific_message.empty()
                             ? "Access this exact path outside the active project?"
                             : specific_message;
        legacy.arguments = arguments;
        return request_guard_approval(legacy, cancellation);
    }
    bool ask = false;
    if (permission_mode_ == PermissionMode::Confirm)
        ask = write || outside_project || destructive || tool_name == "run_command";
    else if (permission_mode_ == PermissionMode::Smart)
        ask = destructive || tool_name == "run_command" ||
              (outside_project && !under_system_temp);
    if (!ask) return GuardApprovalDecision::Allow;

    GuardApprovalRequest request;
    request.tool_name = tool_name;
    request.command_preview = preview;
    request.rule_id =
        !specific_rule.empty()
            ? specific_rule
            : (tool_name == "run_command"
                   ? "ask_on_model_command"
                   : (outside_project ? "ask_on_external_native_access"
                                      : "ask_on_confirm_mode_write"));
    if (!specific_message.empty()) {
        request.message = specific_message;
    } else if (tool_name == "run_command") {
        request.message =
            "Run this model-issued command? Commands always require approval in " +
            std::string(permission_mode_name(permission_mode_)) + " mode.";
    } else {
        request.message =
            std::string(write ? "Modify" : "Read") +
            (outside_project ? " this exact path outside the active project?"
                             : " this path in the active project?");
    }
    request.arguments = arguments;
    return request_guard_approval(request, cancellation);
}

Error ReadToolRegistry::resolve_writable_path(const std::string& relative_path,
                                              fs::path& absolute) const {
    if (relative_path.empty() || !safe_relative_path(relative_path))
        return {ErrorCode::BadArgs, unsafe_path_message(relative_path, "create or modify")};
    const std::string generic = fs::u8path(relative_path).generic_u8string();
    if (generic.empty() || generic == ".")
        return {ErrorCode::BadArgs, "path must name a file, not the workspace root"};
    fs::path current(snapshot_.workspace);
    fs::path remaining = fs::u8path(generic);
    // Walk existing parents and refuse symlink components before creating anything.
    for (const fs::path& component : remaining) {
        current /= component;
        std::error_code ec;
        if (!fs::exists(current, ec) || ec) break;
        const fs::file_status status = fs::symlink_status(current, ec);
        if (ec) return {ErrorCode::FileWrite, "could not inspect path " + generic + ": " + ec.message()};
        bool linked = false;
        const Error link_error =
            platform::path_is_link_or_reparse(current.u8string(), linked);
        if (!link_error.ok())
            return {ErrorCode::FileWrite,
                    "could not inspect workspace write path " + generic + ": " +
                        link_error.message};
        if (linked)
            return {ErrorCode::FileWrite,
                    "refusing symlink or reparse-point path in workspace write: " + generic};
    }
    absolute = fs::u8path(snapshot_.workspace) / remaining;
    // Final containment: never trust string join alone (symlink races, odd components).
    Error contained = ensure_under_workspace(snapshot_.workspace, absolute, generic);
    if (!contained.ok()) return contained;
    return ok_error();
}

Error ReadToolRegistry::normalize_mutation_path(const std::string& input,
                                                std::string& relative) const {
    relative.clear();
    if (input.empty())
        return {ErrorCode::BadArgs, "path must not be empty"};
#if defined(_WIN32)
    Error syntax = platform::validate_windows_path_syntax(input);
    if (!syntax.ok()) return syntax;
#endif
    const fs::path supplied = fs::u8path(input);
    if (!supplied.is_absolute()) {
        relative = supplied.generic_u8string();
        if (!safe_relative_path(relative))
            return {ErrorCode::BadArgs, unsafe_path_message(input, "create or modify")};
        return ok_error();
    }

    std::error_code ec;
    const fs::path workspace = fs::absolute(fs::u8path(snapshot_.workspace), ec).lexically_normal();
    if (ec)
        return {ErrorCode::FileWrite,
                "could not resolve project workspace for path validation: " + ec.message()};
    const fs::path normalized = supplied.lexically_normal();
    const fs::path within = normalized.lexically_relative(workspace);
    relative = within.generic_u8string();
    if (relative.empty() || relative == "." || !safe_relative_path(relative)) {
        relative.clear();
        return {ErrorCode::BadArgs,
                "absolute path is outside the project directory: " + input +
                    " (use a project-relative path)"};
    }
    return ok_error();
}

Error ReadToolRegistry::validate_mutation_path(const std::string& relative_path,
                                               bool create_dirs,
                                               bool deleting) const {
    if (mutation_policy_ == MutationPolicy::Disabled)
        return {ErrorCode::UnsupportedFeature,
                "workspace writes are disabled for this tool session"};
    if (mutation_policy_ == MutationPolicy::Full) return ok_error();
    if (deleting)
        return {ErrorCode::UnsupportedFeature,
                "Plan mode cannot delete planning documents"};
    if (create_dirs)
        return {ErrorCode::UnsupportedFeature,
                "Plan mode cannot create directories"};
    if (relative_path.empty() || !safe_relative_path(relative_path))
        return {ErrorCode::BadArgs, unsafe_path_message(relative_path, "modify in Plan mode")};
    const std::string path = fs::u8path(relative_path).generic_u8string();
    const bool root_allowed =
        path == "PLANS.md" || path == "PLAN.md" || path == "TODO.md" || path == "AGENTS.md";
    const bool plans_markdown =
        path.rfind("docs/plans/", 0) == 0 && path.size() > std::string("docs/plans/").size() &&
        path.size() >= 3 && path.compare(path.size() - 3, 3, ".md") == 0;
    if (!root_allowed && !plans_markdown)
        return {ErrorCode::UnsupportedFeature,
                "Plan mode may write only root PLANS.md, PLAN.md, TODO.md, AGENTS.md, "
                "or case-sensitive *.md files below docs/plans/"};

    fs::path absolute;
    Error error = resolve_writable_path(path, absolute);
    if (!error.ok()) return error;
    std::error_code ec;
    const fs::path parent = absolute.parent_path();
    if (!fs::is_directory(parent, ec) || ec)
        return {ErrorCode::FileWrite,
                "Plan mode requires the destination parent directory to already exist: " +
                    parent.u8string()};
    const fs::file_status parent_status = fs::symlink_status(parent, ec);
    if (ec || fs::is_symlink(parent_status))
        return {ErrorCode::FileWrite,
                "Plan mode refuses symlink destination parents: " + path};
    return ok_error();
}

Error ReadToolRegistry::purge_expired_history_backups() const {
    if (!history_backup_.enabled || history_backup_.ttl_days <= 0) return ok_error();
    const fs::path history_dir =
        fs::u8path(snapshot_.workspace) / kProjectStateDirName / "history";
    std::error_code ec;
    if (!fs::is_directory(history_dir, ec) || ec) return ok_error();
    const std::time_t now_tt = std::time(nullptr);
    const std::time_t ttl_sec = static_cast<std::time_t>(history_backup_.ttl_days) * 86400;
    for (fs::directory_iterator it(history_dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec) || ec) continue;
        const std::string name = it->path().filename().u8string();
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".bak") != 0) continue;
        const fs::file_time_type modified = it->last_write_time(ec);
        if (ec) { ec.clear(); continue; }
        const auto system_modified = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            modified - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        const std::time_t modified_tt = std::chrono::system_clock::to_time_t(system_modified);
        if (now_tt >= modified_tt && (now_tt - modified_tt) > ttl_sec) {
            std::error_code rm_ec;
            fs::remove(it->path(), rm_ec);
        }
    }
    return ok_error();
}

Error ReadToolRegistry::save_history_copy(const std::string& relative_path,
                                          const std::string& previous_content,
                                          std::string& history_path) const {
    history_path.clear();
    if (!history_backup_.enabled) return ok_error();
    if (history_backup_.max_bytes > 0 && previous_content.size() > history_backup_.max_bytes) {
        return ok_error();  // too large — skip without error
    }

    const std::string generic = fs::u8path(relative_path).generic_u8string();
    const fs::path history_file =
        fs::u8path(history_backup_path(snapshot_.workspace, generic));
    const fs::path history_dir = history_file.parent_path();
    Error directory_error =
        platform::ensure_private_directory(history_dir.u8string(), true, true);
    if (!directory_error.ok()) return directory_error;

    // One stable slot per workspace path (hash of generic relative path).
    Error history_error =
        platform::atomic_write_private(history_file.u8string(), previous_content, true);
    if (!history_error.ok()) return history_error;
    history_path =
        (fs::path(kProjectStateDirName) / "history" / history_file.filename())
            .generic_u8string();
    (void)purge_expired_history_backups();
    return ok_error();
}

void ReadToolRegistry::note_written_file(const std::string& relative_path,
                                         const std::string& content) const {
    if (!indexing_enabled_) return;
    const std::string generic = fs::u8path(relative_path).generic_u8string();
    const std::string hash = index::content_hash(content);
    const std::size_t line_count = content.empty()
                                       ? 0
                                       : static_cast<std::size_t>(
                                             std::count(content.begin(), content.end(), '\n') +
                                             (content.back() == '\n' ? 0 : 1));
    index::Language language = index::Language::Markdown;
    index::language_for_path(generic, language);
    long long file_id = 0;
    bool found = false;
    for (index::IndexedFile& file : snapshot_.files) {
        if (file.path != generic) continue;
        file.language = language;
        file.size = content.size();
        file.content_hash = hash;
        file.line_count = line_count;
        file.status = "indexed";
        file.error.clear();
        file_id = file.id;
        found = true;
        break;
    }
    if (!found) {
        long long next_file_id = 1;
        for (const index::IndexedFile& file : snapshot_.files)
            next_file_id = std::max(next_file_id, file.id + 1);
        index::IndexedFile file;
        file.id = next_file_id;
        file.path = generic;
        file.language = language;
        file.size = content.size();
        file.content_hash = hash;
        file.line_count = line_count;
        file.status = "indexed";
        file_id = file.id;
        snapshot_.files.push_back(std::move(file));
    }

    // Drop stale symbols for this path and re-scan so replace_symbol / read_symbol
    // stay consistent within the same agent run after mutations.
    snapshot_.symbols.erase(
        std::remove_if(snapshot_.symbols.begin(), snapshot_.symbols.end(),
                       [&](const index::IndexedSymbol& symbol) { return symbol.path == generic; }),
        snapshot_.symbols.end());
    const index::ScanResult scan = index::scan_source(generic, content, language);
    if (scan.language != language) {
        for (index::IndexedFile& file : snapshot_.files) {
            if (file.path == generic) {
                file.language = scan.language;
                break;
            }
        }
    }
    long long next_symbol_id = 1;
    for (const index::IndexedSymbol& symbol : snapshot_.symbols)
        next_symbol_id = std::max(next_symbol_id, symbol.id + 1);
    for (const index::Symbol& symbol : scan.symbols) {
        index::IndexedSymbol entry;
        entry.id = next_symbol_id++;
        entry.file_id = file_id;
        entry.path = generic;
        entry.symbol = symbol;
        snapshot_.symbols.push_back(std::move(entry));
    }
    rebuild_file_map();
    IndexOverlayEntry overlay;
    for (const index::IndexedFile& file : snapshot_.files) {
        if (file.path == generic) {
            overlay.file = file;
            break;
        }
    }
    for (const index::IndexedSymbol& symbol : snapshot_.symbols)
        if (symbol.path == generic) overlay.symbols.push_back(symbol);
    overlay.revision = queue_index_paths({generic});
    index_overlay_[generic] = std::move(overlay);
}

void ReadToolRegistry::note_removed_path(const std::string& relative_path) const {
    if (!indexing_enabled_) return;
    const std::string generic = fs::u8path(relative_path).generic_u8string();
    snapshot_.files.erase(
        std::remove_if(snapshot_.files.begin(), snapshot_.files.end(),
                       [&](const index::IndexedFile& file) {
                           return file.path == generic ||
                                  (file.path.size() > generic.size() &&
                                   file.path.compare(0, generic.size(), generic) == 0 &&
                                   file.path[generic.size()] == '/');
                       }),
        snapshot_.files.end());
    snapshot_.symbols.erase(
        std::remove_if(snapshot_.symbols.begin(), snapshot_.symbols.end(),
                       [&](const index::IndexedSymbol& symbol) {
                           return symbol.path == generic ||
                                  (symbol.path.size() > generic.size() &&
                                   symbol.path.compare(0, generic.size(), generic) == 0 &&
                                   symbol.path[generic.size()] == '/');
                       }),
        snapshot_.symbols.end());
    rebuild_file_map();
    IndexOverlayEntry overlay;
    overlay.removed = true;
    overlay.revision = queue_index_paths({generic});
    index_overlay_[generic] = std::move(overlay);
}

namespace {

Error write_bytes_atomic(const fs::path& absolute, const std::string& content) {
    // Project/workspace source is not secret: use shared modes (umask / preserve).
    // History under .ainiux-pr still goes through atomic_write_private separately.
    return platform::atomic_write_shared(absolute.u8string(), content, true);
}

std::string read_all_bytes(const fs::path& absolute, Error& error) {
    std::ifstream input(absolute, std::ios::binary);
    if (!input) {
        error = {ErrorCode::FileRead, "could not open file: " + absolute.u8string()};
        return {};
    }
    std::string content{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad()) {
        error = {ErrorCode::FileRead, "could not read file: " + absolute.u8string()};
        return {};
    }
    error = ok_error();
    return content;
}

}  // namespace

Error ReadToolRegistry::write_workspace_file(const std::string& relative_path,
                                             const std::string& content,
                                             bool create_dirs,
                                             const std::string& mode,
                                             const std::string& expected_file_hash,
                                             std::string& history_path,
                                             bool& created,
                                             std::string& old_hash,
                                             std::string& new_hash) const {
    history_path.clear();
    created = false;
    old_hash.clear();
    new_hash.clear();
    Error policy_error = validate_mutation_path(relative_path, create_dirs, false);
    if (!policy_error.ok()) return policy_error;
    if (content.find('\0') != std::string::npos)
        return {ErrorCode::BadArgs, "write_file content must be UTF-8 text without NUL bytes"};
    if (!html::is_valid_utf8(content))
        return {ErrorCode::BadArgs, "write_file content must be valid UTF-8"};
    if (content.size() > index_options_.max_source_code_file_size)
        return {ErrorCode::BadArgs,
                "write_file content exceeds max_source_code_file_size (" +
                    std::to_string(index_options_.max_source_code_file_size) + " bytes)"};

    std::string write_mode = mode.empty() ? "overwrite" : mode;
    if (write_mode != "overwrite" && write_mode != "create_new")
        return {ErrorCode::BadArgs, "mode must be overwrite or create_new"};

    fs::path absolute;
    Error error = resolve_writable_path(relative_path, absolute);
    if (!error.ok()) return error;

    std::error_code ec;
    const bool exists = fs::exists(absolute, ec) && !ec;
    if (ec) return {ErrorCode::FileWrite, "could not inspect destination: " + ec.message()};
    if (exists && fs::is_directory(absolute, ec))
        return {ErrorCode::FileWrite, "destination path is a directory: " + relative_path};
    if (write_mode == "create_new" && exists)
        return {ErrorCode::FileWrite, "file already exists (mode=create_new): " + relative_path};

    std::string previous;
    if (exists) {
        previous = read_all_bytes(absolute, error);
        if (!error.ok()) return error;
        old_hash = index::content_hash(previous);
        if (!expected_file_hash.empty() && expected_file_hash != old_hash)
            return {ErrorCode::FileWrite,
                    "stale_file: expected_file_hash does not match current file content"};
    } else if (!expected_file_hash.empty()) {
        return {ErrorCode::FileWrite, "stale_file: expected_file_hash set but file does not exist"};
    }

    const fs::path parent = absolute.parent_path();
    if (!parent.empty()) {
        const bool parent_exists = fs::exists(parent, ec) && !ec;
        if (ec) return {ErrorCode::FileWrite, "could not inspect parent directory: " + ec.message()};
        if (parent_exists) {
            // Never replace a file with a directory tree.
            if (fs::is_regular_file(parent, ec) || fs::is_symlink(fs::symlink_status(parent, ec)))
                return {ErrorCode::FileWrite,
                        "parent path exists and is not a directory: " +
                            fs::u8path(relative_path).parent_path().generic_u8string()};
            if (!fs::is_directory(parent, ec))
                return {ErrorCode::FileWrite,
                        "parent path is not a usable directory: " +
                            fs::u8path(relative_path).parent_path().generic_u8string()};
        } else {
            if (!create_dirs)
                return {ErrorCode::FileWrite,
                        "parent directory does not exist; pass create_dirs=true and obtain user "
                        "approval (interactive agent y/n)"};
            // Creating new directories is high-impact: require Guard Ask (y/n). Never
            // silently mkdir -p, and never treat missing parents as free license to wipe.
            if (!permission_controls_) {
                const std::string parent_rel =
                    fs::u8path(relative_path).parent_path().generic_u8string();
                GuardApprovalRequest ask;
                ask.tool_name = "write_file";
                ask.command_preview = "create_directories " + parent_rel;
                ask.rule_id = "ask_on_create_dirs";
                ask.message =
                    "Create missing project directories for write: " + parent_rel +
                    " (will not touch paths outside the project; press y to allow, n to deny)";
                ask.arguments = {parent_rel, relative_path};
                const GuardApprovalDecision decision = request_guard_approval(ask, {});
                if (decision != GuardApprovalDecision::Allow) {
                    std::string message =
                        "refusing to create directories without approval: " + parent_rel;
                    if (decision == GuardApprovalDecision::Cancelled)
                        message += " (approval cancelled)";
                    else if (!on_guard_ask_)
                        message +=
                            " (headless agent denies create_dirs; create the directory first or "
                            "use interactive agent and approve y/n)";
                    else
                        message += " (user denied approval)";
                    return {decision == GuardApprovalDecision::Cancelled ? ErrorCode::Cancelled
                                                                         : ErrorCode::FileWrite,
                            message};
                }
            }
            // Re-check containment of parent before mkdir.
            error = ensure_under_workspace(snapshot_.workspace, parent,
                                           fs::u8path(relative_path)
                                               .parent_path()
                                               .generic_u8string());
            if (!error.ok()) return error;
            fs::create_directories(parent, ec);
            if (ec)
                return {ErrorCode::FileWrite, "could not create parent directories: " + ec.message()};
            // create_directories never deletes existing trees; refuse if parent still missing.
            if (!fs::is_directory(parent, ec))
                return {ErrorCode::FileWrite,
                        "parent directory was not created as a directory: " +
                            fs::u8path(relative_path).parent_path().generic_u8string()};
        }
    }

    // Containment again immediately before write (TOCTOU hardening).
    error = ensure_under_workspace(snapshot_.workspace, absolute,
                                   fs::u8path(relative_path).generic_u8string());
    if (!error.ok()) return error;

    if (exists) {
        error = save_history_copy(relative_path, previous, history_path);
        if (!error.ok()) return error;
    }
    error = write_bytes_atomic(absolute, content);
    if (!error.ok()) return error;
    created = !exists;
    new_hash = index::content_hash(content);
    note_written_file(relative_path, content);
    return ok_error();
}

namespace {

// Map optional 1-based line_range_hint into a byte region inside content.
// When both lines are 0, the whole file is used.
Error region_from_line_hint(const std::string& content,
                            std::size_t start_line,
                            std::size_t end_line,
                            std::size_t& region_start,
                            std::size_t& region_end) {
    region_start = 0;
    region_end = content.size();
    if (start_line == 0 && end_line == 0) return ok_error();
    if (start_line == 0 || end_line == 0 || end_line < start_line)
        return {ErrorCode::BadArgs, "line_range_hint requires start_line and end_line >= 1"};
    const std::vector<std::string> lines = split_lines(content);
    if (start_line > lines.size() || end_line > lines.size())
        return {ErrorCode::BadArgs, "line_range_hint is outside file"};
    std::size_t start_byte = 0;
    for (std::size_t i = 1; i < start_line; ++i) start_byte += lines[i - 1].size();
    std::size_t end_byte = start_byte;
    for (std::size_t i = start_line; i <= end_line; ++i) end_byte += lines[i - 1].size();
    region_start = start_byte;
    region_end = end_byte;
    return ok_error();
}

// When multiple matches exist and replace_all is false, pick the match whose start
// line is closest to the midpoint of the hint (or fail if no hint).
Error disambiguate_matches(const TextMatchResult& found,
                           bool replace_all,
                           std::size_t hint_start_line,
                           std::size_t hint_end_line,
                           std::vector<TextSpan>& chosen,
                           std::vector<std::string>& candidate_lines,
                           std::size_t& matches_found) {
    chosen.clear();
    candidate_lines.clear();
    matches_found = found.matches.size();
    if (found.matches.empty())
        return {ErrorCode::FileWrite, "old_text not found in file"};
    if (replace_all) {
        chosen = found.matches;
        return ok_error();
    }
    if (found.matches.size() == 1) {
        chosen.push_back(found.matches.front());
        return ok_error();
    }
    if (hint_start_line > 0 && hint_end_line >= hint_start_line) {
        const std::size_t mid = hint_start_line + (hint_end_line - hint_start_line) / 2;
        const TextSpan* best = nullptr;
        std::size_t best_dist = std::numeric_limits<std::size_t>::max();
        std::size_t best_count = 0;
        for (const TextSpan& span : found.matches) {
            // Prefer spans that overlap the hint range.
            const bool overlaps =
                !(span.end_line < hint_start_line || span.start_line > hint_end_line);
            const std::size_t dist =
                span.start_line >= mid ? span.start_line - mid : mid - span.start_line;
            if (overlaps) {
                if (best == nullptr || dist < best_dist) {
                    best = &span;
                    best_dist = dist;
                    best_count = 1;
                } else if (dist == best_dist) {
                    ++best_count;
                }
            }
        }
        if (best != nullptr && best_count == 1) {
            chosen.push_back(*best);
            return ok_error();
        }
    }
    for (const TextSpan& span : found.matches) {
        candidate_lines.push_back(std::to_string(span.start_line) +
                                  (span.end_line != span.start_line
                                       ? "-" + std::to_string(span.end_line)
                                       : ""));
    }
    return {ErrorCode::FileWrite,
            "old_text matches " + std::to_string(found.matches.size()) +
                " times; pass replace_all=true, narrow with line_range_hint, or provide a more "
                "specific old_text"};
}

bool is_database_path(const std::string& relative_path) {
    const std::string lower = [&] {
        std::string out = relative_path;
        for (char& ch : out) {
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        }
        return out;
    }();
    static const char* kSuffixes[] = {".sqlite", ".sqlite3", ".db", ".db3", ".duckdb"};
    for (const char* suffix : kSuffixes) {
        const std::size_t n = std::char_traits<char>::length(suffix);
        if (lower.size() >= n && lower.compare(lower.size() - n, n, suffix) == 0) return true;
    }
    return false;
}

}  // namespace

Error ReadToolRegistry::str_replace_workspace_file(const std::string& relative_path,
                                                   const std::string& old_text,
                                                   const std::string& new_text,
                                                   bool replace_all,
                                                   bool allow_fuzzy,
                                                   std::size_t hint_start_line,
                                                   std::size_t hint_end_line,
                                                   const std::string& expected_file_hash,
                                                   std::string& history_path,
                                                   std::size_t& matches_found,
                                                   std::size_t& replacements_made,
                                                   std::string& match_mode,
                                                   std::string& old_hash,
                                                   std::string& new_hash,
                                                   std::vector<std::string>& candidate_lines) const {
    history_path.clear();
    matches_found = 0;
    replacements_made = 0;
    match_mode.clear();
    old_hash.clear();
    new_hash.clear();
    candidate_lines.clear();
    Error policy_error = validate_mutation_path(relative_path, false, false);
    if (!policy_error.ok()) return policy_error;
    if (old_text.empty()) return {ErrorCode::BadArgs, "old_text must not be empty"};
    if (old_text.find('\0') != std::string::npos || new_text.find('\0') != std::string::npos)
        return {ErrorCode::BadArgs, "str_replace text must not contain NUL bytes"};
    if (!html::is_valid_utf8(old_text) || !html::is_valid_utf8(new_text))
        return {ErrorCode::BadArgs, "str_replace text must be valid UTF-8"};

    fs::path absolute;
    Error error = resolve_writable_path(relative_path, absolute);
    if (!error.ok()) return error;

    std::error_code ec;
    if (!fs::exists(absolute, ec) || ec)
        return {ErrorCode::FileRead, "file does not exist: " + relative_path};
    if (fs::is_directory(absolute, ec))
        return {ErrorCode::FileWrite, "path is a directory: " + relative_path};

    const std::string previous = read_all_bytes(absolute, error);
    if (!error.ok()) return error;
    if (previous.find('\0') != std::string::npos || !html::is_valid_utf8(previous))
        return {ErrorCode::FileRead, "file is not valid UTF-8 text: " + relative_path};
    old_hash = index::content_hash(previous);
    if (!expected_file_hash.empty() && expected_file_hash != old_hash)
        return {ErrorCode::FileWrite, "stale_file: expected_file_hash does not match current file content"};

    std::size_t region_start = 0;
    std::size_t region_end = previous.size();
    error = region_from_line_hint(previous, hint_start_line, hint_end_line, region_start, region_end);
    if (!error.ok()) return error;

    const TextMatchResult found =
        find_text_matches(previous, old_text, allow_fuzzy, region_start, region_end);
    std::vector<TextSpan> chosen;
    error = disambiguate_matches(found, replace_all, hint_start_line, hint_end_line, chosen,
                                 candidate_lines, matches_found);
    if (!error.ok()) {
        if (matches_found == 0)
            return {ErrorCode::FileWrite, "old_text not found in file: " + relative_path};
        return error;
    }
    match_mode = found.mode;

    const std::string updated =
        apply_text_replacements(previous, chosen, new_text, replace_all, replacements_made);

    if (updated.size() > index_options_.max_source_code_file_size)
        return {ErrorCode::BadArgs,
                "str_replace result exceeds max_source_code_file_size (" +
                    std::to_string(index_options_.max_source_code_file_size) + " bytes)"};

    error = save_history_copy(relative_path, previous, history_path);
    if (!error.ok()) return error;
    error = write_bytes_atomic(absolute, updated);
    if (!error.ok()) return error;
    new_hash = index::content_hash(updated);
    note_written_file(relative_path, updated);
    return ok_error();
}

namespace {

// Collect sibling basenames in a directory (best-effort; ignores protected names).
std::vector<std::string> sibling_names(const fs::path& directory) {
    std::vector<std::string> names;
    std::error_code ec;
    fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec);
    if (ec) return names;
    for (const fs::directory_entry& entry : it) {
        const std::string name = entry.path().filename().u8string();
        if (name.empty() || name == "." || name == ".." || is_protected_listing_name(name)) continue;
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

// Suggest nearby names when a remove path is missing (literal # backups, close stems).
std::vector<std::string> suggest_remove_paths(const fs::path& parent,
                                              const std::string& basename) {
    std::vector<std::string> suggestions;
    const std::vector<std::string> names = sibling_names(parent);
    const std::string wrapped = "#" + basename + "#";
    const std::string hash_prefix = "#" + basename;
    for (const std::string& name : names) {
        if (name == wrapped || name == hash_prefix || name == basename + "~" ||
            name == basename + ".bak" || name == "#" + basename + "#") {
            suggestions.push_back(name);
            continue;
        }
        // Containment: basename inside sibling or vice versa (bounded).
        if ((!basename.empty() && name.find(basename) != std::string::npos) ||
            (!name.empty() && basename.find(name) != std::string::npos && name.size() >= 3)) {
            if (std::find(suggestions.begin(), suggestions.end(), name) == suggestions.end())
                suggestions.push_back(name);
        }
    }
    if (suggestions.size() > 8) suggestions.resize(8);
    return suggestions;
}

// True when both plain basename and a #basename# (or #basename) sibling exist.
bool has_hash_wrapped_sibling(const fs::path& parent, const std::string& basename) {
    if (basename.empty()) return false;
    // Only apply when the requested name itself is not already hash-wrapped.
    if (!basename.empty() && basename.front() == '#' && basename.back() == '#' && basename.size() > 2)
        return false;
    std::error_code ec;
    if (fs::exists(parent / ("#" + basename + "#"), ec) && !ec) return true;
    if (fs::exists(parent / ("#" + basename), ec) && !ec) return true;
    return false;
}

}  // namespace

Error ReadToolRegistry::remove_workspace_path(const std::string& relative_path,
                                              bool recursive,
                                              bool confirm,
                                              const std::string& expected_file_hash,
                                              std::string& history_path,
                                              bool& was_directory,
                                              std::string& guard_decision,
                                              std::string& guard_rule_id,
                                              std::string& old_hash,
                                              std::vector<std::string>& suggestions,
                                              std::vector<std::string>& warnings) const {
    history_path.clear();
    was_directory = false;
    guard_decision = "deny";
    guard_rule_id.clear();
    old_hash.clear();
    suggestions.clear();
    warnings.clear();
    Error policy_error = validate_mutation_path(relative_path, false, true);
    if (!policy_error.ok()) return policy_error;

    fs::path absolute;
    Error error = resolve_writable_path(relative_path, absolute);
    if (!error.ok()) return error;

    const std::string generic = fs::u8path(relative_path).generic_u8string();
    const std::string basename = absolute.filename().u8string();
    const fs::path parent = absolute.parent_path();
    bool permission_approved = false;

    std::error_code ec;
    if (!fs::exists(absolute, ec) || ec) {
        suggestions = suggest_remove_paths(parent, basename);
        std::string message = "path does not exist: " + relative_path;
        if (!suggestions.empty()) {
            message += "; did you mean one of: ";
            for (std::size_t i = 0; i < suggestions.size(); ++i) {
                if (i) message += ", ";
                message += suggestions[i];
            }
            message += "? Filenames may include literal # characters—use list_dir and the exact name.";
        }
        return {ErrorCode::FileRead, message};
    }
    if (fs::is_symlink(fs::symlink_status(absolute, ec)) || ec) {
        guard_rule_id = "refuse_symlink";
        return {ErrorCode::FileWrite, "refusing to remove symlink: " + relative_path};
    }

    if (is_database_path(generic)) {
        guard_rule_id = "ask_on_database_delete";
        GuardApprovalRequest ask;
        ask.tool_name = "remove";
        ask.command_preview = "remove " + generic;
        ask.rule_id = guard_rule_id;
        ask.message = "delete database file: " + generic;
        ask.arguments = {generic};
        const GuardApprovalDecision decision = request_guard_approval(ask, {});
        if (decision != GuardApprovalDecision::Allow) {
            guard_decision = decision == GuardApprovalDecision::Cancelled ? "cancelled" : "deny";
            std::string message =
                "refusing to delete database file without approval: " + generic +
                " (*.sqlite/*.db/*.duckdb)";
            if (decision == GuardApprovalDecision::Cancelled)
                message += " (approval cancelled)";
            else if (!on_guard_ask_)
                message += " (headless agent denies Ask; use interactive agent)";
            else
                message += " (user denied approval)";
            return {decision == GuardApprovalDecision::Cancelled ? ErrorCode::Cancelled
                                                                 : ErrorCode::FileWrite,
                    message};
        }
        // User approved one-shot; continue.
        permission_approved = true;
    }

    // Models often strip Markdown-like # from "#file#" and delete the plain file instead.
    // When both exist, require an explicit confirm=true for the plain name.
    if (!confirm && has_hash_wrapped_sibling(parent, basename)) {
        guard_rule_id = "ambiguous_hash_sibling";
        suggestions = suggest_remove_paths(parent, basename);
        if (std::find(suggestions.begin(), suggestions.end(), basename) == suggestions.end())
            suggestions.insert(suggestions.begin(), basename);
        return {ErrorCode::FileWrite,
                "ambiguous remove: both \"" + basename +
                    "\" and a #…# sibling exist. If you intend the plain name, re-call remove with "
                    "confirm=true. If the user named a #wrapped# file, use that exact path "
                    "(list_dir first). Do not strip # from filenames."};
    }
    if (has_hash_wrapped_sibling(parent, basename)) {
        warnings.push_back(
            "sibling #…# name also exists; confirm=true accepted for plain path \"" + basename +
            "\"");
    }

    was_directory = fs::is_directory(absolute, ec);
    if (ec) return {ErrorCode::FileWrite, "could not inspect path: " + ec.message()};

    if (was_directory) {
        if (!recursive) {
            // Empty directory only — but nested empty dirs mean "not empty".
            const bool empty = fs::is_empty(absolute, ec);
            if (ec) return {ErrorCode::FileWrite, "could not inspect directory: " + ec.message()};
            if (!empty) {
                guard_rule_id = "recursive_required";
                // Hint whether children are only empty subdirs.
                std::vector<std::string> children;
                for (const std::string& name : sibling_names(absolute)) children.push_back(name);
                std::string message =
                    "directory is not empty; pass recursive=true to remove it (guarded)";
                if (!children.empty()) {
                    message += "; children: ";
                    for (std::size_t i = 0; i < children.size() && i < 12; ++i) {
                        if (i) message += ", ";
                        message += children[i];
                    }
                }
                return {ErrorCode::FileWrite, message};
            }
        } else {
            // Recursive: Ask once if any nested path looks like a database file.
            bool db_delete_approved = false;
            for (fs::recursive_directory_iterator it(absolute, ec), end; !ec && it != end;
                 it.increment(ec)) {
                const std::string name = it->path().filename().u8string();
                std::string rel = generic;
                const fs::path nested = it->path().lexically_relative(absolute);
                if (!nested.empty() && nested != ".")
                    rel = (fs::u8path(generic) / nested).generic_u8string();
                if (!db_delete_approved &&
                    (is_database_path(name) || is_database_path(rel))) {
                    guard_rule_id = "ask_on_database_delete";
                    GuardApprovalRequest ask;
                    ask.tool_name = "remove";
                    ask.command_preview = "remove -r " + generic + " (includes " + rel + ")";
                    ask.rule_id = guard_rule_id;
                    ask.message = "recursive remove would delete database file: " + rel;
                    ask.arguments = {generic, rel};
                    const GuardApprovalDecision decision = request_guard_approval(ask, {});
                    if (decision != GuardApprovalDecision::Allow) {
                        guard_decision =
                            decision == GuardApprovalDecision::Cancelled ? "cancelled" : "deny";
                        std::string message =
                            "refusing recursive remove because it would delete database file: " +
                            rel;
                        if (decision == GuardApprovalDecision::Cancelled)
                            message += " (approval cancelled)";
                        else if (!on_guard_ask_)
                            message += " (headless agent denies Ask; use interactive agent)";
                        else
                            message += " (user denied approval)";
                        return {decision == GuardApprovalDecision::Cancelled
                                    ? ErrorCode::Cancelled
                                    : ErrorCode::FileWrite,
                                message};
                    }
                    db_delete_approved = true;
                }
                if (fs::is_symlink(it->symlink_status(ec)) || ec) {
                    guard_rule_id = "refuse_symlink";
                    return {ErrorCode::FileWrite,
                            "refusing recursive remove of tree containing symlink: " + rel};
                }
            }
            if (ec) return {ErrorCode::FileWrite, "could not walk directory: " + ec.message()};
            if (permission_controls_ && !db_delete_approved) {
                guard_rule_id = "ask_on_recursive_delete";
                GuardApprovalRequest ask;
                ask.tool_name = "remove";
                ask.command_preview = "remove -r " + generic;
                ask.rule_id = guard_rule_id;
                ask.message = "recursively delete directory tree: " + generic;
                ask.arguments = {generic, "recursive=true"};
                const GuardApprovalDecision decision =
                    request_guard_approval(ask, {});
                if (decision != GuardApprovalDecision::Allow) {
                    guard_decision =
                        decision == GuardApprovalDecision::Cancelled ? "cancelled"
                                                                     : "deny";
                    return {decision == GuardApprovalDecision::Cancelled
                                ? ErrorCode::Cancelled
                                : ErrorCode::FileWrite,
                            decision == GuardApprovalDecision::Cancelled
                                ? "recursive remove approval cancelled"
                                : "refusing recursive remove without user approval: " +
                                      generic};
                }
            }
        }
        if (!expected_file_hash.empty())
            return {ErrorCode::BadArgs, "expected_file_hash is only valid for file removals"};
        if (!recursive && permission_mode_ == PermissionMode::Confirm &&
            permission_controls_ && !permission_approved) {
            GuardApprovalRequest ask;
            ask.tool_name = "remove";
            ask.command_preview = "remove " + generic;
            ask.rule_id = "ask_on_confirm_mode_write";
            ask.message = "Delete this empty project directory?";
            ask.arguments = {generic};
            const GuardApprovalDecision decision = request_guard_approval(ask, {});
            if (decision != GuardApprovalDecision::Allow)
                return {decision == GuardApprovalDecision::Cancelled
                            ? ErrorCode::Cancelled
                            : ErrorCode::FileWrite,
                        decision == GuardApprovalDecision::Cancelled
                            ? "remove approval cancelled"
                            : "remove requires user approval"};
        }
        fs::remove_all(absolute, ec);
        if (ec) return {ErrorCode::FileWrite, "could not remove directory: " + ec.message()};
        guard_decision = "allow";
        note_removed_path(generic);
        return ok_error();
    }

    // Regular file.
    if (!fs::is_regular_file(absolute, ec) || ec)
        return {ErrorCode::FileWrite, "path is not a removable regular file or directory: " + generic};

    std::string previous = read_all_bytes(absolute, error);
    if (!error.ok()) return error;
    // History only for text-ish content (no NULs); binary still removable without history.
    const bool text_ok = previous.find('\0') == std::string::npos && html::is_valid_utf8(previous);
    if (text_ok) {
        old_hash = index::content_hash(previous);
        if (!expected_file_hash.empty() && expected_file_hash != old_hash)
            return {ErrorCode::FileWrite,
                    "stale_file: expected_file_hash does not match current file content"};
    } else if (!expected_file_hash.empty()) {
        return {ErrorCode::BadArgs, "expected_file_hash is only supported for UTF-8 text files"};
    }

    if (permission_mode_ == PermissionMode::Confirm && permission_controls_ &&
        !permission_approved) {
        GuardApprovalRequest ask;
        ask.tool_name = "remove";
        ask.command_preview = "remove " + generic;
        ask.rule_id = "ask_on_confirm_mode_write";
        ask.message = "Delete this project file?";
        ask.arguments = {generic};
        const GuardApprovalDecision decision = request_guard_approval(ask, {});
        if (decision != GuardApprovalDecision::Allow)
            return {decision == GuardApprovalDecision::Cancelled
                        ? ErrorCode::Cancelled
                        : ErrorCode::FileWrite,
                    decision == GuardApprovalDecision::Cancelled
                        ? "remove approval cancelled"
                        : "remove requires user approval"};
    }

    if (text_ok) {
        error = save_history_copy(relative_path, previous, history_path);
        if (!error.ok()) return error;
    }
    fs::remove(absolute, ec);
    if (ec) return {ErrorCode::FileWrite, "could not remove file: " + ec.message()};
    guard_decision = "allow";
    note_removed_path(generic);
    return ok_error();
}

Error ReadToolRegistry::apply_workspace_patch(const std::string& patch_text,
                                              bool atomic,
                                              bool allow_fuzzy,
                                              std::vector<std::string>& files_changed,
                                              std::size_t& operations_applied,
                                              std::map<std::string, std::string>& new_hashes,
                                              std::string& reverse_patch_path,
                                              std::vector<std::string>& summary,
                                              std::vector<std::string>& warnings) const {
    files_changed.clear();
    operations_applied = 0;
    new_hashes.clear();
    reverse_patch_path.clear();
    summary.clear();
    warnings.clear();
    if (!allow_mutations())
        return {ErrorCode::UnsupportedFeature, "workspace writes are disabled for this tool session"};

    ParsedPatch parsed;
    Error error = parse_apply_patch(patch_text, parsed);
    if (!error.ok()) return error;
    for (PatchFileOp& op : parsed.ops) {
        std::string relative;
        error = normalize_mutation_path(op.path, relative);
        if (!error.ok()) return error;
        op.path = std::move(relative);
        error = validate_mutation_path(op.path, false, op.kind == PatchOpKind::DeleteFile);
        if (!error.ok()) return error;
    }

    struct Planned {
        PatchOpKind kind = PatchOpKind::UpdateFile;
        std::string path;
        std::string previous;   // empty if new
        std::string next;       // empty if delete
        bool existed = false;
        std::vector<std::string> match_modes;
    };
    std::vector<Planned> plan;
    plan.reserve(parsed.ops.size());

    // Models (notably Kimi / GPT-5.x) often emit multiple "*** Update File: path"
    // sections for the same path. Planning each against the original disk content
    // and writing them in order makes later sections overwrite earlier ones while
    // still reporting applied=true. Coalesce same-path updates in memory.
    auto find_open_plan = [&](const std::string& path) -> Planned* {
        for (std::size_t i = plan.size(); i > 0; --i) {
            Planned& candidate = plan[i - 1];
            if (candidate.path != path) continue;
            if (candidate.kind == PatchOpKind::DeleteFile) return nullptr;
            return &candidate;
        }
        return nullptr;
    };

    for (const PatchFileOp& op : parsed.ops) {
        Planned item;
        item.kind = op.kind;
        item.path = fs::u8path(op.path).generic_u8string();
        if (item.path.empty() || !safe_relative_path(item.path))
            return {ErrorCode::BadArgs, unsafe_path_message(item.path.empty() ? op.path : item.path,
                                                            "patch")};
        if (is_database_path(item.path))
            return {ErrorCode::FileWrite,
                    "refusing apply_patch delete/write on database file: " + item.path};

        fs::path absolute;
        error = resolve_writable_path(item.path, absolute);
        if (!error.ok()) return error;

        std::error_code ec;
        const bool exists = fs::exists(absolute, ec) && !ec;
        if (ec) return {ErrorCode::FileWrite, "could not inspect path: " + ec.message()};
        if (exists && fs::is_symlink(fs::symlink_status(absolute, ec)))
            return {ErrorCode::FileWrite, "refusing symlink path in apply_patch: " + item.path};
        if (exists && fs::is_directory(absolute, ec))
            return {ErrorCode::FileWrite, "apply_patch path is a directory: " + item.path};

        if (op.kind == PatchOpKind::AddFile) {
            if (find_open_plan(item.path) != nullptr)
                return {ErrorCode::FileWrite,
                        "Add File after prior patch ops on the same path: " + item.path};
            if (exists)
                return {ErrorCode::FileWrite,
                        "Add File target already exists (use Update File): " + item.path};
            if (op.add_content.find('\0') != std::string::npos)
                return {ErrorCode::BadArgs, "Add File content must not contain NUL bytes"};
            if (!html::is_valid_utf8(op.add_content))
                return {ErrorCode::BadArgs, "Add File content must be valid UTF-8"};
            if (op.add_content.size() > index_options_.max_source_code_file_size)
                return {ErrorCode::BadArgs, "Add File content exceeds max_source_code_file_size"};
            item.existed = false;
            item.next = op.add_content;
            plan.push_back(std::move(item));
            continue;
        }

        if (op.kind == PatchOpKind::DeleteFile) {
            Planned* open = find_open_plan(item.path);
            if (open != nullptr) {
                // Drop the pending add/update; delete the path if it existed on disk
                // before this patch, or cancel a pure in-patch add.
                if (!open->existed) {
                    // Added then deleted in the same patch: no filesystem change.
                    for (auto it = plan.begin(); it != plan.end(); ++it) {
                        if (&*it == open) {
                            plan.erase(it);
                            break;
                        }
                    }
                    continue;
                }
                item.existed = true;
                item.previous = open->previous;
                for (auto it = plan.begin(); it != plan.end(); ++it) {
                    if (&*it == open) {
                        plan.erase(it);
                        break;
                    }
                }
                plan.push_back(std::move(item));
                continue;
            }
            if (!exists)
                return {ErrorCode::FileRead, "Delete File target does not exist: " + item.path};
            item.existed = true;
            item.previous = read_all_bytes(absolute, error);
            if (!error.ok()) return error;
            plan.push_back(std::move(item));
            continue;
        }

        // UpdateFile — chain onto any in-plan content for this path.
        Planned* open = find_open_plan(item.path);
        if (open != nullptr) {
            std::string chained;
            std::vector<std::string> modes;
            error = apply_patch_hunks(open->next, op.hunks, allow_fuzzy, chained, modes);
            if (!error.ok()) {
                error.message = item.path + ": " + error.message;
                return error;
            }
            if (chained.size() > index_options_.max_source_code_file_size)
                return {ErrorCode::BadArgs,
                        "Update File result exceeds max_source_code_file_size for " + item.path};
            if (!html::is_valid_utf8(chained))
                return {ErrorCode::FileWrite,
                        "Update File result is not valid UTF-8 for " + item.path};
            open->next = std::move(chained);
            open->match_modes.insert(open->match_modes.end(), modes.begin(), modes.end());
            continue;
        }

        if (!exists)
            return {ErrorCode::FileRead, "Update File target does not exist: " + item.path};
        item.existed = true;
        item.previous = read_all_bytes(absolute, error);
        if (!error.ok()) return error;
        if (item.previous.find('\0') != std::string::npos || !html::is_valid_utf8(item.previous))
            return {ErrorCode::FileRead, "Update File target is not valid UTF-8 text: " + item.path};
        error = apply_patch_hunks(item.previous, op.hunks, allow_fuzzy, item.next, item.match_modes);
        if (!error.ok()) {
            error.message = item.path + ": " + error.message;
            return error;
        }
        if (item.next.size() > index_options_.max_source_code_file_size)
            return {ErrorCode::BadArgs,
                    "Update File result exceeds max_source_code_file_size for " + item.path};
        if (!html::is_valid_utf8(item.next))
            return {ErrorCode::FileWrite, "Update File result is not valid UTF-8 for " + item.path};
        plan.push_back(std::move(item));
    }

    // Atomic by default: all plan entries validated before any mutation.
    // Non-atomic still applies in order but stops on first I/O failure after partial writes.
    (void)atomic;

    for (const Planned& item : plan) {
        fs::path absolute;
        error = resolve_writable_path(item.path, absolute);
        if (!error.ok()) return error;

        if (item.kind == PatchOpKind::DeleteFile) {
            std::string history_path;
            if (item.previous.find('\0') == std::string::npos &&
                html::is_valid_utf8(item.previous)) {
                error = save_history_copy(item.path, item.previous, history_path);
                if (!error.ok()) return error;
                if (reverse_patch_path.empty()) reverse_patch_path = history_path;
            }
            std::error_code ec;
            fs::remove(absolute, ec);
            if (ec)
                return {ErrorCode::FileWrite,
                        "could not delete " + item.path + ": " + ec.message()};
            note_removed_path(item.path);
            files_changed.push_back(item.path);
            summary.push_back("deleted " + item.path);
            ++operations_applied;
            continue;
        }

        // Add or update: write content.
        if (item.existed) {
            std::string history_path;
            error = save_history_copy(item.path, item.previous, history_path);
            if (!error.ok()) return error;
            if (reverse_patch_path.empty()) reverse_patch_path = history_path;
        } else {
            const fs::path parent = absolute.parent_path();
            std::error_code ec;
            if (!parent.empty() && (!fs::exists(parent, ec) || ec)) {
                fs::create_directories(parent, ec);
                if (ec)
                    return {ErrorCode::FileWrite,
                            "could not create parent directories for " + item.path + ": " +
                                ec.message()};
            }
        }
        error = write_bytes_atomic(absolute, item.next);
        if (!error.ok()) return error;
        note_written_file(item.path, item.next);
        new_hashes[item.path] = index::content_hash(item.next);
        files_changed.push_back(item.path);
        if (item.kind == PatchOpKind::AddFile) {
            summary.push_back("added " + item.path);
        } else {
            std::string modes;
            for (std::size_t i = 0; i < item.match_modes.size(); ++i) {
                if (i) modes += ",";
                modes += item.match_modes[i];
            }
            summary.push_back("updated " + item.path +
                              (modes.empty() ? std::string() : " (" + modes + ")"));
            if (allow_fuzzy) {
                for (const std::string& mode : item.match_modes) {
                    if (mode != "exact" && mode != "eof_insert") {
                        warnings.push_back("fuzzy hunk match on " + item.path + ": " + mode);
                        break;
                    }
                }
            }
        }
        ++operations_applied;
    }
    return ok_error();
}

namespace {

std::string join_lines(const std::vector<std::string>& lines) {
    std::string output;
    for (const std::string& line : lines) output += line;
    return output;
}

// Weak models often omit trailing newlines in replacement text. When the replaced
// range ended with '\n', preserve that terminator so "# comment" does not glue to
// the next line ("# commentprint(...)").
std::string preserve_range_newline(const std::string& previous_range,
                                   const std::string& replacement) {
    if (replacement.empty()) return replacement;
    if (!previous_range.empty() && previous_range.back() == '\n' && replacement.back() != '\n')
        return replacement + "\n";
    return replacement;
}

// GPT-5.x / some OpenRouter tool encoders materialize every nested op object with
// default empty fields (symbol_id=1, empty strings, line=1). Treat those shells as
// absent so a real nested op (or top-level type) can still win.
bool nested_edit_op_is_empty_shell(const json::Value& nested) {
    if (!nested.is_object()) return true;
    auto non_empty_string = [&](const char* key) -> bool {
        const json::Value* value = nested.get(key);
        return value != nullptr && value->is_string() && !value->string.empty();
    };
    if (non_empty_string("new_text") || non_empty_string("replacement") ||
        non_empty_string("text") || non_empty_string("old_text") ||
        non_empty_string("path") || non_empty_string("expected_hash"))
        return false;

    auto number_at = [&](const char* key, double& out) -> bool {
        const json::Value* value = nested.get(key);
        if (value == nullptr || value->type != json::Value::Type::Number) return false;
        out = value->number;
        return true;
    };
    double start = 0, end = 0, line = 0, symbol = 0;
    const bool has_start = number_at("start_line", start);
    const bool has_end = number_at("end_line", end);
    const bool has_line = number_at("line", line);
    const bool has_symbol = number_at("symbol_id", symbol);

    // Full default template from schema-filling models (~10+ keys, all defaults).
    const bool looks_like_filler_template =
        nested.object.size() >= 8 && has_start && has_end && has_line && has_symbol &&
        start == 1.0 && end == 1.0 && line == 1.0 && symbol == 1.0 &&
        !non_empty_string("new_text") && !non_empty_string("old_text") &&
        !non_empty_string("replacement") && !non_empty_string("text");
    if (looks_like_filler_template) return true;

    // Compact real payloads: delete_range/replace_range line spans, insert line,
    // or replace_symbol id.
    if (has_start && has_end && end >= start && start >= 1.0) return false;
    if (has_line && line >= 1.0) return false;
    if (has_symbol && symbol >= 1.0) return false;
    return true;
}

// Some models (e.g. DeepSeek via OpenRouter) nest the operation:
//   {"replace_range":{"start_line":1,"end_line":1,"new_text":"..."}}
// or split text outside the nested object:
//   {"replace_range":{"start_line":1,"end_line":1},"text":"..."}
// GPT-5.x often fills ALL nested op keys with empty defaults plus a real top-level
// type/fields — drop empty shells and flatten the remaining payload.
// Flatten that into the flat schema before type inference / parsing.
json::Value normalize_edit_op_shape(const json::Value& op) {
    if (!op.is_object()) return op;
    static const char* kNestedTypes[] = {"replace_range", "insert_at", "delete_range",
                                         "replace_text",  "replace_symbol", "create_file"};

    // Explicit type/op wins: ignore nested shells entirely (GPT-5 pollution).
    const json::Value* type_value = op.get("type");
    if (type_value == nullptr) type_value = op.get("op");
    const bool has_explicit_type =
        type_value != nullptr && type_value->is_string() && !type_value->string.empty();

    const json::Value* nested = nullptr;
    std::string nested_type;
    int payload_count = 0;
    for (const char* name : kNestedTypes) {
        const json::Value* candidate = op.get(name);
        if (candidate == nullptr || !candidate->is_object()) continue;
        if (nested_edit_op_is_empty_shell(*candidate)) continue;
        ++payload_count;
        if (nested == nullptr) {
            nested = candidate;
            nested_type = name;
        }
    }

    // When the model set type/op, keep flat fields and drop nested noise.
    if (has_explicit_type) {
        json::Value flat = object_value();
        for (const auto& entry : op.object) {
            bool is_nested_type = false;
            for (const char* name : kNestedTypes) {
                if (entry.first == name) {
                    is_nested_type = true;
                    break;
                }
            }
            if (is_nested_type) continue;
            flat.object[entry.first] = entry.second;
        }
        // If top-level lacks text fields but the matching nested type had payload,
        // merge that nested payload (models put replacement only under nested key).
        if (nested != nullptr && payload_count == 1 &&
            (type_value->string == nested_type || type_value->string.empty())) {
            for (const auto& entry : nested->object) {
                if (flat.object.find(entry.first) == flat.object.end())
                    flat.object[entry.first] = entry.second;
                else {
                    // Prefer non-empty nested strings over empty top-level defaults.
                    const json::Value* existing = flat.get(entry.first);
                    if (existing != nullptr && existing->is_string() && existing->string.empty() &&
                        entry.second.is_string() && !entry.second.string.empty())
                        flat.object[entry.first] = entry.second;
                }
            }
        } else if (nested != nullptr && type_value->string == nested_type) {
            for (const auto& entry : nested->object) {
                const json::Value* existing = flat.get(entry.first);
                if (existing == nullptr)
                    flat.object[entry.first] = entry.second;
                else if (existing->is_string() && existing->string.empty() &&
                         entry.second.is_string() && !entry.second.string.empty())
                    flat.object[entry.first] = entry.second;
            }
        }
        if (flat.get("new_text") == nullptr && flat.get("replacement") == nullptr) {
            const json::Value* text = flat.get("text");
            if (text != nullptr && text->is_string()) flat.object["new_text"] = *text;
        }
        return flat;
    }

    if (nested == nullptr) return op;
    // Multiple real nested payloads without type → leave unchanged (ambiguous).
    if (payload_count > 1) return op;

    json::Value flat = object_value();
    flat.object["type"] = string_value(nested_type);
    for (const auto& entry : nested->object) flat.object[entry.first] = entry.second;
    // Preserve top-level siblings (text, expected_hash, replace_all, …).
    for (const auto& entry : op.object) {
        bool is_nested_type = false;
        for (const char* name : kNestedTypes) {
            if (entry.first == name) {
                is_nested_type = true;
                break;
            }
        }
        if (is_nested_type) continue;
        if (flat.object.find(entry.first) == flat.object.end())
            flat.object[entry.first] = entry.second;
    }
    // "text" is a common alias for replacement/new_text.
    if (flat.get("new_text") == nullptr && flat.get("replacement") == nullptr) {
        const json::Value* text = flat.get("text");
        if (text != nullptr && text->is_string()) flat.object["new_text"] = *text;
    }
    return flat;
}

// Infer edit op type when models omit "type"/"op" but send enough fields.
// Returns empty string when ambiguous.
std::string infer_edit_op_type(const json::Value& op) {
    const json::Value* type_value = op.get("type");
    if (type_value == nullptr) type_value = op.get("op");
    if (type_value != nullptr && type_value->is_string() && !type_value->string.empty())
        return type_value->string;

    const bool has_start = op.get("start_line") != nullptr;
    const bool has_end = op.get("end_line") != nullptr;
    const bool has_line = op.get("line") != nullptr;
    const bool has_old = op.get("old_text") != nullptr;
    const bool has_new = op.get("new_text") != nullptr || op.get("replacement") != nullptr ||
                         op.get("text") != nullptr;
    const bool has_symbol = op.get("symbol_id") != nullptr;

    if (has_old && has_new && !has_start && !has_end) return "replace_text";
    if (has_line && has_new && !has_start && !has_end) return "insert_at";
    if (has_start && has_end && has_new) return "replace_range";
    if (has_start && has_end && !has_new) return "delete_range";
    if (has_symbol && has_new) return "replace_symbol";
    if (has_new && !has_start && !has_end && !has_line && !has_old) return "create_file";
    return {};
}

struct LineEditOp {
    enum class Type { ReplaceRange, InsertAt, DeleteRange, ReplaceText, ReplaceSymbol };
    Type type = Type::ReplaceRange;
    std::size_t start_line = 0;  // 1-based; insert uses line; replace_text uses as hint
    std::size_t end_line = 0;
    std::string text;            // replacement / new_text
    std::string old_text;        // replace_text only
    bool replace_all = false;
    bool allow_fuzzy = true;     // replace_text / str_replace fallback
    std::string expected_hash;
    std::size_t symbol_id = 0;   // replace_symbol only
    std::size_t original_index = 0;
};

bool is_line_op(LineEditOp::Type type) {
    return type == LineEditOp::Type::ReplaceRange || type == LineEditOp::Type::InsertAt ||
           type == LineEditOp::Type::DeleteRange;
}

}  // namespace

Error ReadToolRegistry::edit_workspace_file(const std::string& relative_path,
                                            const std::string& expected_file_hash,
                                            const json::Value& ops,
                                            bool create_dirs,
                                            std::string& history_path,
                                            std::string& old_hash,
                                            std::string& new_hash,
                                            std::size_t& operations_applied,
                                            std::vector<std::string>& summary,
                                            std::vector<std::string>& warnings) const {
    history_path.clear();
    old_hash.clear();
    new_hash.clear();
    operations_applied = 0;
    summary.clear();
    warnings.clear();
    const bool external = fs::u8path(relative_path).is_absolute();
    Error policy_error;
    if (external) {
        if (mutation_policy_ != MutationPolicy::Full)
            return {ErrorCode::UnsupportedFeature,
                    "outside-project edits require interactive Act mode"};
    } else {
        policy_error = validate_mutation_path(relative_path, create_dirs, false);
        if (!policy_error.ok()) return policy_error;
    }
    if (!ops.is_array() || ops.array.empty())
        return {ErrorCode::BadArgs, "edit_file requires a non-empty ops array"};
    if (ops.array.size() > 100)
        return {ErrorCode::BadArgs, "edit_file supports at most 100 operations per call"};

    // Normalize nested/aliased op shapes once so create-file detection and parsing agree.
    std::vector<json::Value> normalized_ops;
    normalized_ops.reserve(ops.array.size());
    for (const json::Value& raw_op : ops.array) {
        if (!raw_op.is_object()) return {ErrorCode::BadArgs, "each edit_file op must be an object"};
        json::Value normalized = normalize_edit_op_shape(raw_op);
        const json::Value* type = normalized.get("type");
        if (type == nullptr) type = normalized.get("op");
        if (type != nullptr && type->is_string() && type->string == "insert_at" &&
            normalized.get("line") == nullptr) {
            // Several OpenAI-compatible local models select start_line because
            // the permissive multi-op schema exposes both fields. For insert_at
            // they are unambiguously equivalent.
            const json::Value* start_line = normalized.get("start_line");
            if (start_line != nullptr && start_line->type == json::Value::Type::Number)
                normalized.object["line"] = *start_line;
        }
        if (type != nullptr && type->is_string() && type->string == "replace_range") {
            // glm-5.2 and similar sometimes send replace_range with only "line"
            // (single-line rewrite) instead of start_line/end_line.
            const json::Value* line = normalized.get("line");
            if (line != nullptr && line->type == json::Value::Type::Number) {
                if (normalized.get("start_line") == nullptr)
                    normalized.object["start_line"] = *line;
                if (normalized.get("end_line") == nullptr)
                    normalized.object["end_line"] = *line;
            }
        }
        normalized_ops.push_back(std::move(normalized));
    }

    // create_file may stand alone (or with only create_file ops) to make a new file.
    bool create_file_only = true;
    std::string create_content;
    bool saw_create = false;
    for (const json::Value& op : normalized_ops) {
        const std::string type = infer_edit_op_type(op);
        if (type.empty())
            return {ErrorCode::BadArgs,
                    "each edit_file op requires type/op or enough fields to infer the operation "
                    "(replace_range needs start_line+end_line+new_text; replace_text needs "
                    "old_text+new_text; insert_at needs line+new_text; delete_range needs "
                    "start_line+end_line). Nested shapes like "
                    "{\"replace_range\":{\"start_line\":1,\"end_line\":1,\"new_text\":\"...\"}} "
                    "are also accepted."};
        if (type == "create_file") {
            saw_create = true;
            const json::Value* text = op.get("new_text");
            if (text == nullptr) text = op.get("replacement");
            if (text == nullptr) text = op.get("text");
            if (text == nullptr || !text->is_string())
                return {ErrorCode::BadArgs, "create_file requires new_text"};
            if (saw_create && !create_content.empty() && create_content != text->string)
                return {ErrorCode::BadArgs, "edit_file allows only one create_file content"};
            create_content = text->string;
        } else {
            create_file_only = false;
        }
    }
    if (saw_create && !create_file_only)
        return {ErrorCode::BadArgs, "create_file cannot be combined with other edit_file ops"};
    if (saw_create && create_file_only) {
        if (external)
            return {ErrorCode::UnsupportedFeature,
                    "external edit_file create_file is unsupported; use write_file"};
        bool created = false;
        const Error create_error =
            write_workspace_file(relative_path, create_content, create_dirs, "create_new",
                                 expected_file_hash, history_path, created, old_hash, new_hash);
        if (create_error.ok()) {
            operations_applied = 1;
            summary.push_back("created file");
        }
        return create_error;
    }

    std::vector<LineEditOp> parsed;
    parsed.reserve(normalized_ops.size());
    for (std::size_t index = 0; index < normalized_ops.size(); ++index) {
        const json::Value& op = normalized_ops[index];
        const std::string type = infer_edit_op_type(op);
        if (type.empty())
            return {ErrorCode::BadArgs,
                    "each edit_file op requires type/op or enough fields to infer the operation"};
        LineEditOp item;
        item.original_index = index;
        const json::Value* expected = op.get("expected_hash");
        if (expected != nullptr) {
            if (!expected->is_string())
                return {ErrorCode::BadArgs, "op expected_hash must be a string"};
            item.expected_hash = expected->string;
        }
        if (type == "replace_range") {
            item.type = LineEditOp::Type::ReplaceRange;
            std::string validation_error;
            if (!get_size(op, "start_line", 0, 100000000, item.start_line, validation_error) ||
                item.start_line == 0 ||
                !get_size(op, "end_line", 0, 100000000, item.end_line, validation_error) ||
                item.end_line == 0)
                return {ErrorCode::BadArgs,
                        validation_error.empty() ? "replace_range requires start_line and end_line"
                                                 : validation_error};
            if (item.end_line < item.start_line)
                return {ErrorCode::BadArgs, "replace_range end_line must be >= start_line"};
            const json::Value* text = op.get("replacement");
            if (text == nullptr) text = op.get("new_text");
            if (text == nullptr) text = op.get("text");
            if (text == nullptr || !text->is_string())
                return {ErrorCode::BadArgs, "replace_range requires replacement"};
            item.text = text->string;
        } else if (type == "insert_at") {
            item.type = LineEditOp::Type::InsertAt;
            std::string validation_error;
            if (!get_size(op, "line", 0, 100000000, item.start_line, validation_error) ||
                item.start_line == 0)
                return {ErrorCode::BadArgs,
                        validation_error.empty()
                            ? "insert_at requires line (1-based; start_line is also accepted)"
                            : validation_error};
            const json::Value* text = op.get("new_text");
            if (text == nullptr) text = op.get("replacement");
            if (text == nullptr) text = op.get("text");
            if (text == nullptr || !text->is_string())
                return {ErrorCode::BadArgs, "insert_at requires new_text"};
            item.text = text->string;
        } else if (type == "delete_range") {
            item.type = LineEditOp::Type::DeleteRange;
            std::string validation_error;
            if (!get_size(op, "start_line", 0, 100000000, item.start_line, validation_error) ||
                item.start_line == 0 ||
                !get_size(op, "end_line", 0, 100000000, item.end_line, validation_error) ||
                item.end_line == 0)
                return {ErrorCode::BadArgs,
                        validation_error.empty() ? "delete_range requires start_line and end_line"
                                                 : validation_error};
            if (item.end_line < item.start_line)
                return {ErrorCode::BadArgs, "delete_range end_line must be >= start_line"};
        } else if (type == "replace_text") {
            item.type = LineEditOp::Type::ReplaceText;
            const json::Value* old_text = op.get("old_text");
            const json::Value* new_text = op.get("new_text");
            if (new_text == nullptr) new_text = op.get("text");
            if (old_text == nullptr || !old_text->is_string() || old_text->string.empty())
                return {ErrorCode::BadArgs, "replace_text requires non-empty old_text"};
            if (new_text == nullptr || !new_text->is_string())
                return {ErrorCode::BadArgs, "replace_text requires new_text"};
            item.old_text = old_text->string;
            item.text = new_text->string;
            std::string validation_error;
            if (!get_bool(op, "replace_all", false, item.replace_all, validation_error))
                return {ErrorCode::BadArgs, validation_error};
            if (!get_bool(op, "fuzzy", true, item.allow_fuzzy, validation_error))
                return {ErrorCode::BadArgs, validation_error};
            // Optional line_range_hint narrows the search window.
            const json::Value* hint = op.get("line_range_hint");
            if (hint != nullptr) {
                if (!hint->is_object())
                    return {ErrorCode::BadArgs, "line_range_hint must be an object"};
                if (!get_size(*hint, "start_line", 0, 100000000, item.start_line, validation_error) ||
                    !get_size(*hint, "end_line", 0, 100000000, item.end_line, validation_error))
                    return {ErrorCode::BadArgs, validation_error};
            }
        } else if (type == "replace_symbol") {
            if (!indexing_enabled_)
                return {ErrorCode::UnsupportedFeature,
                        "replace_symbol is unavailable because indexing is disabled"};
            item.type = LineEditOp::Type::ReplaceSymbol;
            std::string validation_error;
            if (!get_size(op, "symbol_id", 0, static_cast<std::size_t>(std::numeric_limits<int>::max()),
                          item.symbol_id, validation_error) ||
                item.symbol_id == 0)
                return {ErrorCode::BadArgs,
                        validation_error.empty() ? "replace_symbol requires positive symbol_id"
                                                 : validation_error};
            const json::Value* text = op.get("replacement");
            if (text == nullptr) text = op.get("new_text");
            if (text == nullptr) text = op.get("text");
            if (text == nullptr || !text->is_string())
                return {ErrorCode::BadArgs, "replace_symbol requires replacement"};
            item.text = text->string;
            // expected_hash already parsed into item.expected_hash above.
        } else if (type == "create_file") {
            // Handled above.
            continue;
        } else {
            return {ErrorCode::BadArgs, "unknown edit_file op type: " + type};
        }
        if (item.text.find('\0') != std::string::npos || item.old_text.find('\0') != std::string::npos)
            return {ErrorCode::BadArgs, "edit_file text must not contain NUL bytes"};
        if ((!item.text.empty() && !html::is_valid_utf8(item.text)) ||
            (!item.old_text.empty() && !html::is_valid_utf8(item.old_text)))
            return {ErrorCode::BadArgs, "edit_file text must be valid UTF-8"};
        parsed.push_back(std::move(item));
    }
    if (parsed.empty()) return {ErrorCode::BadArgs, "edit_file ops produced no applicable operations"};

    fs::path absolute;
    Error error;
    if (external) {
        bool outside = false;
        error = resolve_native_path(snapshot_.workspace, relative_path, true, absolute,
                                    outside);
        if (error.ok() && !outside)
            error = {ErrorCode::BadArgs,
                     "external edit path resolved inside the project"};
    } else {
        error = resolve_writable_path(relative_path, absolute);
    }
    if (!error.ok()) return error;
    std::error_code ec;
    if (!fs::exists(absolute, ec) || ec)
        return {ErrorCode::FileRead, "file does not exist: " + relative_path};
    if (fs::is_directory(absolute, ec))
        return {ErrorCode::FileWrite, "path is a directory: " + relative_path};

    const std::string previous = read_all_bytes(absolute, error);
    if (!error.ok()) return error;
    if (previous.find('\0') != std::string::npos || !html::is_valid_utf8(previous))
        return {ErrorCode::FileRead, "file is not valid UTF-8 text: " + relative_path};
    old_hash = index::content_hash(previous);
    if (!expected_file_hash.empty() && expected_file_hash != old_hash)
        return {ErrorCode::FileWrite, "stale_file: expected_file_hash does not match current file content"};

    std::vector<std::string> lines = split_lines(previous);

    // Resolve replace_symbol into replace_range against the current snapshot index.
    for (LineEditOp& op : parsed) {
        if (op.type != LineEditOp::Type::ReplaceSymbol) continue;
        if (external)
            return {ErrorCode::UnsupportedFeature,
                    "replace_symbol is index-dependent and unavailable outside the project"};
        const index::IndexedSymbol* found = nullptr;
        for (const index::IndexedSymbol& symbol : snapshot_.symbols) {
            if (symbol.id == static_cast<long long>(op.symbol_id)) {
                found = &symbol;
                break;
            }
        }
        if (found == nullptr)
            return {ErrorCode::FileRead,
                    "replace_symbol: indexed symbol id was not found: " +
                        std::to_string(op.symbol_id)};
        if (found->path != fs::u8path(relative_path).generic_u8string())
            return {ErrorCode::BadArgs,
                    "replace_symbol: symbol_id " + std::to_string(op.symbol_id) + " is in " +
                        found->path + ", not " +
                            fs::u8path(relative_path).generic_u8string()};
        if (found->symbol.line_start <= 0 || found->symbol.line_end < found->symbol.line_start)
            return {ErrorCode::Internal, "replace_symbol: symbol has invalid line range"};
        op.type = LineEditOp::Type::ReplaceRange;
        op.start_line = static_cast<std::size_t>(found->symbol.line_start);
        op.end_line = static_cast<std::size_t>(found->symbol.line_end);
        // Prefer body hash from the index when the model did not supply expected_hash.
        if (op.expected_hash.empty() && found->symbol.body_hash != 0) {
            // body_hash is stored as raw uint64 fingerprint; range check uses content_hash
            // of the source slice, so leave expected_hash empty unless the model set it.
        }
        summary.push_back("replace_symbol id=" + std::to_string(op.symbol_id) + " -> lines " +
                          std::to_string(op.start_line) + "-" + std::to_string(op.end_line));
    }

    // Apply line ops bottom-to-top so earlier higher-line edits do not shift later ones.
    std::vector<LineEditOp> line_ops;
    std::vector<LineEditOp> text_ops;
    for (const LineEditOp& op : parsed) {
        if (is_line_op(op.type)) line_ops.push_back(op);
        else text_ops.push_back(op);
    }
    std::sort(line_ops.begin(), line_ops.end(), [](const LineEditOp& a, const LineEditOp& b) {
        if (a.start_line != b.start_line) return a.start_line > b.start_line;
        return a.original_index > b.original_index;
    });

    auto range_slice = [&](std::size_t start, std::size_t end) -> std::string {
        std::string out;
        if (lines.empty() || start == 0) return out;
        const std::size_t last = std::min(end, lines.size());
        for (std::size_t i = start; i <= last; ++i) out += lines[i - 1];
        return out;
    };

    for (const LineEditOp& op : line_ops) {
        if (op.type == LineEditOp::Type::InsertAt) {
            if (op.start_line > lines.size() + 1)
                return {ErrorCode::BadArgs,
                        "insert_at line is outside file (1.." + std::to_string(lines.size() + 1) +
                            ")"};
            if (!op.expected_hash.empty()) {
                // expected_hash on insert is optional and unused for content checks.
            }
            std::vector<std::string> inserted = split_lines(op.text);
            const std::size_t at = op.start_line - 1;
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at), inserted.begin(),
                         inserted.end());
            summary.push_back("inserted before line " + std::to_string(op.start_line));
            ++operations_applied;
            continue;
        }
        if (op.start_line == 0 || op.start_line > lines.size() || op.end_line < op.start_line ||
            op.end_line > lines.size())
            return {ErrorCode::BadArgs,
                    "line range is outside file for op starting at line " +
                        std::to_string(op.start_line)};
        const std::string current_range = range_slice(op.start_line, op.end_line);
        const std::string current_range_hash = index::content_hash(current_range);
        if (!op.expected_hash.empty() && op.expected_hash != current_range_hash) {
            // Models often invent range hashes or paste file_hash into expected_hash.
            // When the whole-file fingerprint already matched (or the model supplied the
            // whole-file hash as the range hash), treat expected_hash as advisory.
            const bool file_fingerprint_ok =
                (!expected_file_hash.empty() && expected_file_hash == old_hash) ||
                op.expected_hash == old_hash;
            if (file_fingerprint_ok) {
                warnings.push_back(
                    "ignored mismatched expected_hash for lines " +
                    std::to_string(op.start_line) + "-" + std::to_string(op.end_line) +
                    " (current_range_hash=" + current_range_hash +
                    "); file fingerprint matched so the edit proceeded");
            } else {
                std::string preview = current_range;
                if (preview.size() > 120) {
                    preview.resize(utf8_prefix(preview, 120));
                    preview += "…";
                }
                for (char& ch : preview) {
                    if (ch == '\n' || ch == '\r') ch = ' ';
                }
                return {ErrorCode::FileWrite,
                        "stale_range: expected_hash does not match lines " +
                            std::to_string(op.start_line) + "-" + std::to_string(op.end_line) +
                            " (current_range_hash=" + current_range_hash +
                            ", preview=\"" + preview +
                            "\"); re-read that line range and use its range_hash, or omit "
                            "expected_hash when expected_file_hash already matches"};
            }
        }
        if (op.type == LineEditOp::Type::DeleteRange) {
            lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(op.start_line - 1),
                        lines.begin() + static_cast<std::ptrdiff_t>(op.end_line));
            summary.push_back("deleted lines " + std::to_string(op.start_line) + "-" +
                              std::to_string(op.end_line));
            ++operations_applied;
        } else if (op.type == LineEditOp::Type::ReplaceRange) {
            const std::string normalized = preserve_range_newline(current_range, op.text);
            std::vector<std::string> replacement = split_lines(normalized);
            lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(op.start_line - 1),
                        lines.begin() + static_cast<std::ptrdiff_t>(op.end_line));
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(op.start_line - 1),
                         replacement.begin(), replacement.end());
            summary.push_back("replaced lines " + std::to_string(op.start_line) + "-" +
                              std::to_string(op.end_line));
            ++operations_applied;
        }
    }

    // Text ops run after line ops on the resulting content, in original order.
    std::sort(text_ops.begin(), text_ops.end(),
              [](const LineEditOp& a, const LineEditOp& b) { return a.original_index < b.original_index; });
    std::string content = join_lines(lines);
    for (const LineEditOp& op : text_ops) {
        std::size_t region_start = 0;
        std::size_t region_end = content.size();
        Error region_error =
            region_from_line_hint(content, op.start_line, op.end_line, region_start, region_end);
        if (!region_error.ok()) return region_error;

        const TextMatchResult found =
            find_text_matches(content, op.old_text, op.allow_fuzzy, region_start, region_end);
        std::vector<TextSpan> chosen;
        std::vector<std::string> candidate_lines;
        std::size_t matches_found = 0;
        Error match_error =
            disambiguate_matches(found, op.replace_all, op.start_line, op.end_line, chosen,
                                 candidate_lines, matches_found);
        if (!match_error.ok()) {
            if (matches_found == 0)
                return {ErrorCode::FileWrite, "old_text not found for replace_text"};
            return match_error;
        }
        std::size_t replacements_made = 0;
        content = apply_text_replacements(content, chosen, op.text, op.replace_all, replacements_made);
        summary.push_back(std::string("replace_text (") + found.mode + ", " +
                          (op.replace_all ? "all" : "one") + ")");
        ++operations_applied;
    }

    if (content.size() > index_options_.max_source_code_file_size)
        return {ErrorCode::BadArgs,
                "edit_file result exceeds max_source_code_file_size (" +
                    std::to_string(index_options_.max_source_code_file_size) + " bytes)"};

    // Re-apply create_dirs only if needed for... not for existing files.
    if (!external) {
        error = save_history_copy(relative_path, previous, history_path);
        if (!error.ok()) return error;
    }
    error = write_bytes_atomic(absolute, content);
    if (!error.ok()) return error;
    new_hash = index::content_hash(content);
    if (!external) note_written_file(relative_path, content);
    return ok_error();
}

std::vector<provider::FunctionDefinition> ReadToolRegistry::definitions() const {
    const bool agent_session = mutation_policy_ != MutationPolicy::Disabled;
    const std::string path = "\"path\":{\"type\":\"string\"}";
    const std::string range = path + ",\"start_line\":{\"type\":\"integer\",\"minimum\":1},"
                                      "\"end_line\":{\"type\":\"integer\",\"minimum\":1},"
                                      "\"max_bytes\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":262144}";
    // Gemini / Google AI Studio reject array properties without an items schema
    // (OpenRouter surfaces: properties[ops].items: missing field). Keep op items
    // permissive so flat, nested, and alias argument shapes remain accepted.
    const std::string edit_line_fields =
        "\"start_line\":{\"type\":\"integer\",\"minimum\":1},"
        "\"end_line\":{\"type\":\"integer\",\"minimum\":1},"
        "\"line\":{\"type\":\"integer\",\"minimum\":1},"
        "\"new_text\":{\"type\":\"string\"},"
        "\"replacement\":{\"type\":\"string\"},"
        "\"text\":{\"type\":\"string\"},"
        "\"old_text\":{\"type\":\"string\"},"
        "\"replace_all\":{\"type\":\"boolean\"},"
        "\"fuzzy\":{\"type\":\"boolean\"},"
        "\"expected_hash\":{\"type\":\"string\"}" +
        (indexing_enabled_
             ? std::string(",\"symbol_id\":{\"type\":\"integer\",\"minimum\":1}")
             : std::string());
    // Flat op schema only. Nested op objects (replace_range:{...}) are still
    // accepted on the wire via normalize_edit_op_shape, but advertising them in
    // the tool schema causes GPT-5.x / OpenRouter to fill every nested key with
    // empty defaults (symbol_id=1, blank strings), which breaks type inference.
    // Enumerate op names (including replace_symbol when indexing) so enablement
    // tests and models still see the capability without nested shells.
    const std::string edit_type_enum =
        indexing_enabled_
            ? "\"enum\":[\"insert_at\",\"replace_range\",\"delete_range\","
              "\"replace_text\",\"replace_symbol\",\"create_file\"]"
            : "\"enum\":[\"insert_at\",\"replace_range\",\"delete_range\","
              "\"replace_text\",\"create_file\"]";
    const std::string edit_op_item =
        "{\"type\":\"object\",\"properties\":{"
        "\"type\":{\"type\":\"string\"," +
        edit_type_enum +
        "},"
        "\"op\":{\"type\":\"string\"," +
        edit_type_enum + "}," + edit_line_fields +
        ",\"line_range_hint\":{\"type\":\"object\",\"properties\":{"
        "\"start_line\":{\"type\":\"integer\",\"minimum\":1},"
        "\"end_line\":{\"type\":\"integer\",\"minimum\":1}}}"
        "}}";
    const std::string search_fields =
        "\"query\":{\"type\":\"string\"},"
        "\"pattern\":{\"type\":\"string\"},"
        "\"regex\":{\"type\":\"boolean\"},"
        "\"case_sensitive\":{\"type\":\"boolean\"},"
        "\"word\":{\"type\":\"boolean\"},"
        "\"path\":{\"type\":\"string\"},"
        "\"glob\":{\"type\":\"string\"},"
        "\"context\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":10},"
        "\"max_results\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":500}";
    std::vector<provider::FunctionDefinition> tools = {
        {"index_overview",
         "Summarize the code index (languages, file counts, freshness). Not a full "
         "filesystem listing—use list_dir for on-disk layout.",
         schema("")},
        {"list_dir",
         "List real filesystem entries in a workspace-relative directory (literal names, "
         "including empty dirs and non-source files). Prefer before remove.",
         schema(path + ",\"max_entries\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":500}")},
        {"glob",
         "Match eligible workspace source paths (*, ?, **, braces).",
         schema("\"pattern\":{\"type\":\"string\"},\"max_results\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":1000}", "\"pattern\"")},
        {"grep",
         "Search workspace UTF-8 sources (rg when available, else index/live scan). "
         "query is literal unless regex=true; unescaped | infers regex only when regex is "
         "omitted. path=one file or directory root; glob=name/type filter (*.ts, "
         "**/*.{cpp,hpp}); combine them to search a subtree. pattern aliases query.",
         schema(search_fields, "\"query\"")},
        {"search_symbol",
         "Rank indexed symbols by lexical match, then static importance.",
         schema("\"query\":{\"type\":\"string\"},\"max_results\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":200}", "\"query\"")},
        {"file_outline",
         "Indexed declarations, signatures, ranges, and docs for one file.",
         schema(path, "\"path\"")},
        {"read_symbol",
         "Verify and read the indexed source range for a symbol_id.",
         schema("\"symbol_id\":{\"type\":\"integer\",\"minimum\":1}", "\"symbol_id\"")},
        {"read_many",
         !agent_session
             ? "Preferred file reader for two or more known paths/ranges (including when "
               "native parallel tool calls are available). Batch-read 1–100 indexed ranges "
               "with line numbers and hashes under one byte cap."
             : "Preferred file reader for two or more known paths/ranges (including when "
               "native parallel tool calls are available). Batch-read 1–100 live exact-path "
               "files with line numbers and hashes under one byte cap.",
         schema("\"items\":{\"type\":\"array\",\"minItems\":1,\"maxItems\":100,\"items\":" + schema(range, "\"path\"") + "},\"max_bytes\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":262144}", "\"items\"")},
        {"read_file",
         !agent_session
             ? "Single-target fallback: read one bounded indexed UTF-8 range with hashes/"
               "line numbers. Do not issue multiple parallel read_file calls when read_many "
               "can batch the known reads."
             : "Single-target fallback: read one exact-path live UTF-8 file (need not be "
               "indexed). Do not issue multiple parallel read_file calls when read_many can "
               "batch. PNG/JPEG/GIF are not text—use attach_image for pixels.",
         schema(range, "\"path\"")},
        {"run_command",
         agent_session
             ? "Run one workspace command without a real shell (argv exec). Bare PATH names "
               "(`make`, `python3`) or project scripts (`./script.sh`, `bash script.sh`). "
               "No unquoted pipes/redirects/chaining. Act uses Guard; Plan allows vetted "
               "read-only forms. Prefer native filesystem/Git tools when available."
             : "Run one read-only inspection command without a shell "
               "(pwd/ls/rg/grep/find/git allowlist).",
         schema("\"command\":{\"type\":\"string\"},\"cwd\":{\"type\":\"string\"},"
                "\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":" +
                    std::string(agent_session ? "120000" : "10000") + "}",
                "\"command\"")},
        {"git_status",
         "Compact git status (short + branch by default). Prefer over run_command.",
         schema("\"short\":{\"type\":\"boolean\"},\"include_branch\":{\"type\":\"boolean\"}")},
        {"git_diff",
         "Bounded git diff (optional path, --cached, --stat).",
         schema("\"path\":{\"type\":\"string\"},\"cached\":{\"type\":\"boolean\"},"
                "\"stat\":{\"type\":\"boolean\"},"
                "\"max_bytes\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":524288}")},
    };
    if (!indexing_enabled_) {
        static const std::set<std::string> hidden = {
            "index_overview", "search_symbol", "file_outline", "read_symbol"};
        tools.erase(
            std::remove_if(
                tools.begin(), tools.end(),
                [&](const provider::FunctionDefinition& definition) {
                    return hidden.find(definition.name) != hidden.end();
                }),
            tools.end());
    }
    if (allow_network_) {
        tools.push_back(
            {"fetch_url",
             "Fetch one http(s) URL as UTF-8 Markdown/text (never raw HTML). Private/"
             "loopback blocked unless configured. Prefer top search hits only.",
             schema("\"url\":{\"type\":\"string\"},"
                    "\"max_bytes\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":8388608},"
                    "\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":120000}",
                    "\"url\"")});
        tools.push_back(
            {"web_search",
             "Web search (configured API providers, else DuckDuckGo). At most 3 results "
             "(title/URL/snippet). Returns web_search_unavailable when none can run.",
             schema("\"term\":{\"type\":\"string\"},"
                    "\"max_results\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":3},"
                    "\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":120000},"
                    "\"site\":{\"type\":\"string\"}",
                    "\"term\"")});
    }
    if (agent_session) {
        // Always advertised in Act/Plan so tool-definition cache stays stable
        // whether or not a session goal is currently active.
        tools.push_back(
            {"goal_met",
             "Call only when the active session goal is verifiably satisfied. Requires "
             "non-empty evidence. Rejected when no goal is active.",
             schema("\"evidence\":{\"type\":\"string\"}", "\"evidence\"")});
        tools.push_back(
            {"attach_image",
             "Attach one local PNG/JPEG/GIF for vision on the next model round of this turn "
             "(request-local, not stored). Vision-capable Chat Completions model required; "
             "per-turn limits apply.",
             schema(path, "\"path\"")});
    }
    if (allow_mutations()) {
        tools.push_back(
            {"edit_file",
             "Preferred in-file edit (not whole-file delete—use remove). Flat ops only—"
             "set type/op on the op object (do not nest empty replace_range/… shells). "
             "Ops: insert_at (e.g. {\"type\":\"insert_at\",\"line\":2,\"new_text\":\"...\"}), "
             "replace_range, delete_range, replace_text"
             + std::string(indexing_enabled_ ? ", replace_symbol" : "") +
             ", create_file (alone). Omit expected_hash unless from a fresh read. "
             "Line ops apply bottom-to-top.",
             schema(path + ",\"expected_file_hash\":{\"type\":\"string\"},"
                           "\"create_dirs\":{\"type\":\"boolean\"},"
                           "\"ops\":{\"type\":\"array\",\"minItems\":1,\"maxItems\":100,"
                           "\"items\":" +
                           edit_op_item + "}",
                    "\"path\",\"ops\"")});
        tools.push_back(
            {"write_file",
             "Create or overwrite a UTF-8 file. Prefer edit_file for project edits.",
             schema(path + ",\"content\":{\"type\":\"string\"},"
                           "\"create_dirs\":{\"type\":\"boolean\"},"
                           "\"expected_file_hash\":{\"type\":\"string\"},"
                           "\"mode\":{\"type\":\"string\",\"enum\":[\"overwrite\",\"create_new\"]}",
                    "\"path\",\"content\"")});
        tools.push_back(
            {"create_directory",
             "Act-only mkdir; parents=true creates missing parents. Plan: policy_denied.",
             schema(path + ",\"parents\":{\"type\":\"boolean\"}", "\"path\"")});
        tools.push_back(
            {"rename_path",
             "Act-only rename; destination must not exist. Plan: policy_denied.",
             schema("\"source\":{\"type\":\"string\"},"
                    "\"destination\":{\"type\":\"string\"}",
                    "\"source\",\"destination\"")});
        tools.push_back(
            {"str_replace",
             "Exact (then optional fuzzy) text replace in one file. Prefer edit_file. "
             "Fails on 0 or ambiguous multi-match without replace_all/line_range_hint.",
             schema(path + ",\"old_text\":{\"type\":\"string\"},\"new_text\":{\"type\":\"string\"},"
                           "\"replace_all\":{\"type\":\"boolean\"},"
                           "\"fuzzy\":{\"type\":\"boolean\"},"
                           "\"line_range_hint\":{\"type\":\"object\",\"properties\":{"
                           "\"start_line\":{\"type\":\"integer\",\"minimum\":1},"
                           "\"end_line\":{\"type\":\"integer\",\"minimum\":1}}},"
                           "\"expected_file_hash\":{\"type\":\"string\"}",
                    "\"path\",\"old_text\",\"new_text\"")});
        tools.push_back(
            {"remove",
             "Act-only delete file or empty dir (recursive=true for non-empty). Use exact "
             "name from list_dir. Plan: policy_denied. Prefer over edit_file for deletes.",
             schema(path + ",\"recursive\":{\"type\":\"boolean\"},"
                           "\"confirm\":{\"type\":\"boolean\"},"
                           "\"expected_file_hash\":{\"type\":\"string\"}",
                    "\"path\"")});
        tools.push_back(
            {"apply_patch",
             "OpenAI/Codex multi-file patch (prefer edit_file/str_replace for simple "
             "single-file edits). Hunk context must match; @@ -line,count helps. "
             "Args: patch|diff|input. fuzzy=true default. Preferred form:\n"
             "*** Begin Patch\n"
             "*** Update File: path\n"
             "@@\n"
             " context\n"
             "-old\n"
             "+new\n"
             "*** End Patch\n"
             "Bare *** Update/Add/Delete File sections without Begin/End are also accepted.",
             schema("\"patch\":{\"type\":\"string\"},"
                    "\"input\":{\"type\":\"string\"},"
                    "\"diff\":{\"type\":\"string\"},"
                    "\"atomic\":{\"type\":\"boolean\"},"
                    "\"fuzzy\":{\"type\":\"boolean\"}",
                    "")});
    }
        if (mcp_bridge_ != nullptr) {
        for (const provider::FunctionDefinition& mcp_tool : mcp_bridge_->definitions()) {
            tools.push_back(mcp_tool);
        }
    }
    return tools;
}

Error ReadToolRegistry::read_external_source(const fs::path& absolute_path,
                                             std::size_t start_line,
                                             std::size_t end_line,
                                             std::size_t max_bytes,
                                             SourceRange& range,
                                             bool approved_external) const {
    if (approved_external) {
        Error stable = ensure_approved_external_path_unchanged(
            absolute_path, "read", ErrorCode::FileRead);
        if (!stable.ok()) return stable;
    }
    const std::string description =
        approved_external ? "approved external file" : "workspace file";
    std::error_code ec;
    const std::uintmax_t file_size = fs::file_size(absolute_path, ec);
    if (ec)
        return {ErrorCode::FileRead,
                "could not inspect " + description + " " + absolute_path.generic_u8string() +
                    ": " + ec.message()};
    if (file_size > index_options_.max_source_code_file_size)
        return {ErrorCode::FileRead,
                description + " exceeds max_source_code_file_size (" +
                    std::to_string(index_options_.max_source_code_file_size) + " bytes): " +
                    absolute_path.generic_u8string()};

    Error read_error;
    const std::string source = read_all_bytes(absolute_path, read_error);
    if (!read_error.ok()) return read_error;
    if (source.find('\0') != std::string::npos || !html::is_valid_utf8(source)) {
        // Steer the model away from Python/PIL when the path is a vision image.
        if (input::path_has_supported_image_extension(absolute_path.generic_u8string())) {
            return {ErrorCode::UnsupportedFeature,
                    description + " is a PNG/JPEG/GIF image, not UTF-8 text: " +
                        absolute_path.generic_u8string() +
                        ". Do not use Python/PIL or shell tools to open it. "
                        "Call attach_image with this path when you need pixel content "
                        "(vision model required), or ask the user to /attach the file."};
        }
        return {ErrorCode::FileRead,
                description + " is not UTF-8 text: " +
                    absolute_path.generic_u8string()};
    }
    if (max_bytes == 0)
        return {ErrorCode::BadArgs, "source byte cap must be positive"};

    const std::vector<std::string> lines = split_lines(source);
    if (start_line == 0) start_line = 1;
    const std::string display_path = absolute_path.generic_u8string();
    const std::string file_hash = index::content_hash(source);
    if (lines.empty()) {
        if (start_line != 1 || end_line > 1)
            return {ErrorCode::BadArgs,
                    "requested line range is outside " + description + ": " + display_path};
        range.path = display_path;
        range.file_hash = file_hash;
        range.range_hash = index::content_hash("");
        range.start_line = 1;
        range.end_line = 0;
        return ok_error();
    }
    if (end_line == 0) end_line = lines.size();
    if (start_line > lines.size() || end_line < start_line)
        return {ErrorCode::BadArgs,
                "requested line range is outside " + description + ": " + display_path};
    end_line = std::min(end_line, lines.size());

    std::string selected;
    for (std::size_t line = start_line; line <= end_line && line != 0; ++line)
        selected += lines[line - 1];
    const bool truncated = selected.size() > max_bytes;
    if (truncated) selected.resize(utf8_prefix(selected, max_bytes));
    const std::string raw_hash = index::content_hash(selected);
    const std::string redacted = redact_source_secrets(selected, secrets_);
    std::size_t returned_end_line =
        start_line + static_cast<std::size_t>(std::count(selected.begin(), selected.end(), '\n'));
    if (!selected.empty() && selected.back() == '\n' && returned_end_line > start_line)
        --returned_end_line;
    range.path = display_path;
    range.content = redacted;
    range.file_hash = file_hash;
    range.range_hash = raw_hash;
    range.start_line = start_line;
    range.end_line = truncated ? returned_end_line : end_line;
    range.bytes = redacted.size();
    range.truncated = truncated;
    range.redacted = redacted != selected;
    return ok_error();
}

Error ReadToolRegistry::read_workspace_source(const std::string& relative_path,
                                              std::size_t start_line,
                                              std::size_t end_line,
                                              std::size_t max_bytes,
                                              SourceRange& range) const {
    if (relative_path.empty() || !safe_relative_path(relative_path))
        return {ErrorCode::BadArgs, unsafe_path_message(relative_path, "read")};

    fs::path current(snapshot_.workspace);
    std::error_code ec;
    for (const fs::path& component : fs::u8path(relative_path)) {
        current /= component;
        const fs::file_status status = fs::symlink_status(current, ec);
        if (ec || status.type() == fs::file_type::not_found)
            return {ErrorCode::FileRead,
                    "file does not exist or cannot be inspected: " + relative_path +
                        (ec ? " (" + ec.message() + ")" : std::string())};
        bool linked = false;
        const Error link_error =
            platform::path_is_link_or_reparse(current.u8string(), linked);
        if (!link_error.ok())
            return {ErrorCode::FileRead,
                    "could not inspect workspace path " + relative_path + ": " +
                        link_error.message};
        if (linked)
            return {ErrorCode::FileRead,
                    "refusing symlink or reparse-point path in workspace read: " +
                        relative_path};
    }
    if (!fs::is_regular_file(current, ec) || ec)
        return {ErrorCode::FileRead,
                "workspace read path must be a regular file: " + relative_path};

    const fs::path canonical = fs::canonical(current, ec);
    if (ec || canonical.empty())
        return {ErrorCode::FileRead,
                "could not resolve workspace file " + relative_path +
                    (ec ? ": " + ec.message() : std::string())};
    const fs::path root = fs::canonical(snapshot_.workspace, ec);
    if (ec || !resolved_path_is_under(root, canonical))
        return {ErrorCode::FileRead,
                "workspace file escaped the active project during validation: " +
                    relative_path};
    Error stable = ensure_approved_external_path_unchanged(
        canonical, "read workspace file", ErrorCode::FileRead);
    if (!stable.ok()) return stable;

    Error error =
        read_external_source(canonical, start_line, end_line, max_bytes, range, false);
    if (error.ok()) range.path = fs::u8path(relative_path).generic_u8string();
    return error;
}

Error ReadToolRegistry::write_external_file(const fs::path& absolute_path,
                                            const std::string& content,
                                            bool create_dirs,
                                            const std::string& mode,
                                            const std::string& expected_file_hash,
                                            bool& created,
                                            std::string& old_hash,
                                            std::string& new_hash) const {
    created = false;
    old_hash.clear();
    new_hash.clear();
    if (mutation_policy_ != MutationPolicy::Full)
        return {ErrorCode::UnsupportedFeature,
                "outside-project writes require interactive Act mode"};
    if (content.find('\0') != std::string::npos || !html::is_valid_utf8(content))
        return {ErrorCode::BadArgs,
                "write_file content must be valid UTF-8 text without NUL bytes"};
    if (content.size() > index_options_.max_source_code_file_size)
        return {ErrorCode::BadArgs,
                "write_file content exceeds max_source_code_file_size (" +
                    std::to_string(index_options_.max_source_code_file_size) + " bytes)"};
    const std::string write_mode = mode.empty() ? "overwrite" : mode;
    if (write_mode != "overwrite" && write_mode != "create_new")
        return {ErrorCode::BadArgs, "mode must be overwrite or create_new"};

    std::error_code ec;
    const bool exists = fs::exists(absolute_path, ec) && !ec;
    if (ec)
        return {ErrorCode::FileWrite,
                "could not inspect approved external destination: " + ec.message()};
    if (exists && !fs::is_regular_file(absolute_path, ec))
        return {ErrorCode::FileWrite,
                "approved external destination is not a regular file: " +
                    absolute_path.generic_u8string()};
    if (write_mode == "create_new" && exists)
        return {ErrorCode::FileWrite,
                "external file already exists (mode=create_new): " +
                    absolute_path.generic_u8string()};
    if (exists) {
        Error read_error;
        const std::string previous = read_all_bytes(absolute_path, read_error);
        if (!read_error.ok()) return read_error;
        old_hash = index::content_hash(previous);
        if (!expected_file_hash.empty() && expected_file_hash != old_hash)
            return {ErrorCode::FileWrite,
                    "stale_file: expected_file_hash does not match external file content"};
    } else if (!expected_file_hash.empty()) {
        return {ErrorCode::FileWrite,
                "stale_file: expected_file_hash set but external file does not exist"};
    }

    const fs::path parent = absolute_path.parent_path();
    const bool parent_exists = fs::is_directory(parent, ec) && !ec;
    if (ec)
        return {ErrorCode::FileWrite,
                "could not inspect approved external parent: " + ec.message()};
    if (!parent_exists) {
        if (!create_dirs)
            return {ErrorCode::FileWrite,
                    "external parent directory does not exist; pass create_dirs=true so the "
                    "approval prompt includes directory creation"};
        fs::create_directories(parent, ec);
        if (ec || !fs::is_directory(parent))
            return {ErrorCode::FileWrite,
                    "could not create approved external parent " + parent.generic_u8string() +
                        (ec ? ": " + ec.message() : std::string())};
    }

    Error stable = ensure_approved_external_path_unchanged(
        absolute_path, "write", ErrorCode::FileWrite);
    if (!stable.ok()) return stable;
    Error write_error = write_bytes_atomic(absolute_path, content);
    if (!write_error.ok()) return write_error;
    created = !exists;
    new_hash = index::content_hash(content);
    return ok_error();
}

Error ReadToolRegistry::read_source(const std::string& path,
                                    std::size_t start_line,
                                    std::size_t end_line,
                                    std::size_t max_bytes,
                                    SourceRange& range) const {
    if (!safe_relative_path(path) || path.empty())
        return {ErrorCode::BadArgs, unsafe_path_message(path, "read")};
    const auto found = files_.find(fs::u8path(path).generic_u8string());
    if (found == files_.end() || found->second->status != "indexed")
        return {ErrorCode::FileRead, "path is not an eligible indexed file: " + path};
    const index::IndexedFile& record = *found->second;
    fs::path current(snapshot_.workspace);
    for (const fs::path& component : fs::u8path(record.path)) {
        current /= component;
        std::error_code ec;
        const fs::file_status status = fs::symlink_status(current, ec);
        if (ec) return {ErrorCode::FileRead, "could not inspect indexed path " + record.path + ": " + ec.message()};
        if (fs::is_symlink(status)) return {ErrorCode::FileRead, "refusing symlink path in indexed workspace: " + record.path};
    }
    std::ifstream input(current, std::ios::binary);
    if (!input) return {ErrorCode::FileRead, "could not open indexed file: " + record.path};
    std::string source{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad()) return {ErrorCode::FileRead, "could not read indexed file: " + record.path};
    const std::string file_hash = index::content_hash(source);
    if (file_hash != record.content_hash)
        return {ErrorCode::FileRead, "indexed file changed after the snapshot: " + record.path};
    if (source.find('\0') != std::string::npos || !html::is_valid_utf8(source)) {
        if (input::path_has_supported_image_extension(record.path)) {
            return {ErrorCode::UnsupportedFeature,
                    "indexed path is a PNG/JPEG/GIF image, not UTF-8 text: " + record.path +
                        ". Do not use Python/PIL; call attach_image when pixel content is "
                        "required, or ask the user to /attach the file."};
        }
        return {ErrorCode::FileRead, "indexed file is no longer valid UTF-8 text: " + record.path};
    }
    if (max_bytes == 0) return {ErrorCode::BadArgs, "source byte cap must be positive"};
    const std::vector<std::string> lines = split_lines(source);
    if (start_line == 0) start_line = 1;
    if (lines.empty()) {
        if (start_line != 1 || end_line > 1)
            return {ErrorCode::BadArgs, "requested line range is outside indexed file: " + record.path};
        range.path = record.path;
        range.file_hash = file_hash;
        range.range_hash = index::content_hash("");
        range.start_line = 1;
        range.end_line = 0;
        return ok_error();
    }
    if (end_line == 0) end_line = lines.empty() ? 0 : lines.size();
    if ((!lines.empty() && start_line > lines.size()) || end_line < start_line)
        return {ErrorCode::BadArgs, "requested line range is outside indexed file: " + record.path};
    end_line = std::min(end_line, lines.size());
    std::string selected;
    for (std::size_t line = start_line; line <= end_line && line != 0; ++line) selected += lines[line - 1];
    bool truncated = selected.size() > max_bytes;
    if (truncated) selected.resize(utf8_prefix(selected, max_bytes));
    const std::string raw_hash = index::content_hash(selected);
    const std::string redacted = redact_source_secrets(selected, secrets_);
    std::size_t returned_end_line = start_line;
    const std::size_t returned_newlines = static_cast<std::size_t>(
        std::count(selected.begin(), selected.end(), '\n'));
    returned_end_line += returned_newlines;
    if (!selected.empty() && selected.back() == '\n' && returned_end_line > start_line)
        --returned_end_line;
    range.path = record.path;
    range.content = redacted;
    range.file_hash = file_hash;
    range.range_hash = raw_hash;
    range.start_line = start_line;
    range.end_line = truncated ? returned_end_line : end_line;
    range.bytes = redacted.size();
    range.truncated = truncated;
    range.redacted = redacted != selected;
    return ok_error();
}

std::string ReadToolRegistry::execute(const std::string& requested_name,
                                      const std::string& arguments_json,
                                      runtime::CancellationToken cancellation) const {
    if (mcp_bridge_ != nullptr && mcp_bridge_->is_mcp_tool(requested_name)) {
        // MCP tools are external capability: Confirm always asks; Smart asks once
        // per tool name via request_permission (stdio treated as higher risk).
        if (permission_controls_ || permission_mode_ != PermissionMode::Yolo) {
            const GuardApprovalDecision decision = request_permission(
                requested_name, "MCP tool " + requested_name,
                {arguments_json.substr(0, 200)}, false, false,
                /*write=*/true, /*destructive=*/false, "mcp.tool",
                "Approve MCP tool call " + requested_name + "?",
                cancellation);
            if (decision != GuardApprovalDecision::Allow) {
                return tool_error_result("permission_denied",
                                         "MCP tool call denied: " + requested_name);
            }
        }
        return mcp_bridge_->execute(requested_name, arguments_json, cancellation);
    }
    if (cancellation.cancelled()) return tool_error_result("cancelled", "tool call cancelled");

    // Stage 7: silent legacy aliases first (not advertised), then case/snake-camel
    // repair against the registry (+ legacy names so typo repair still works).
    std::string name = canonical_native_tool_name(requested_name);
    {
        std::vector<std::string> known;
        known.reserve(24);
        for (const provider::FunctionDefinition& definition : definitions())
            known.push_back(definition.name);
        // Legacy names remain repair targets but are not in definitions().
        known.push_back("search_text");
        known.push_back("find");
        known.push_back("list_directory");
        known.push_back("project_overview");
        known.push_back("get_skeleton");
        known.push_back("search_web");
        const std::string repaired = repair_tool_name(requested_name, known);
        if (!repaired.empty()) name = canonical_native_tool_name(repaired);
    }
    if (!indexing_enabled_) {
        static const std::set<std::string> disabled = {
            "index_overview", "search_symbol", "file_outline", "read_symbol"};
        if (disabled.find(name) != disabled.end())
            return tool_error_result(
                "indexing_disabled",
                name + " is unavailable because indexing is disabled for this session");
    }

    // Stages 1-5 of the shared argument pipeline (empty -> {}, fence strip,
    // strict JSON, single-object extraction, one-pass repair).
    const ToolArgParseResult parsed = parse_tool_arguments(arguments_json);
    if (!parsed.error.ok() || !parsed.value.is_object()) {
        const std::string message = parsed.error.ok()
                                        ? "tool arguments must be a JSON object"
                                        : parsed.error.message;
        return invalid_arguments_tool_result(requested_name, message,
                                             parsed.original_arguments);
    }
    json::Value args = parsed.value;
    // Stage 6: schema-aware coercion for the resolved tool, when known.
    {
        const std::string schema_name = name;
        for (const provider::FunctionDefinition& definition : definitions()) {
            if (definition.name != schema_name) continue;
            const json::ParseResult schema = json::parse(definition.parameters_json);
            if (schema.error.ok() && schema.value.is_object()) {
                const json::Value* properties = schema.value.get("properties");
                if (properties != nullptr && properties->is_object())
                    coerce_tool_arguments(args, *properties);
            }
            break;
        }
    }

    std::string validation_error;
    bool lazy_live_fallback = false;
    static const std::set<std::string> snapshot_tools = {
        "index_overview", "glob",         "grep",
        "search_symbol",  "file_outline", "read_symbol",
        "edit_file"};
    if (snapshot_tools.find(name) != snapshot_tools.end()) {
        if (index_access_mode_ == IndexAccessMode::LazyHints &&
            index_refresh_) {
            const std::size_t completed = index_refresh_->completed();
            if (completed > loaded_index_generation_) {
                loaded_index_generation_ = completed;
                for (auto item = index_overlay_.begin();
                     item != index_overlay_.end();) {
                    if (item->second.revision <= completed)
                        item = index_overlay_.erase(item);
                    else
                        ++item;
                }
            }
        } else {
            const Error refresh_error =
                refresh_persistent_index(false, cancellation);
            if (!refresh_error.ok())
                return tool_error_result(
                    error_code_string(refresh_error.code),
                    refresh_error.message);
        }
    }
    if (index_access_mode_ == IndexAccessMode::LazyHints &&
        indexing_enabled_ &&
        snapshot_tools.find(name) != snapshot_tools.end()) {
        index::Options query_options = index_options_;
        query_options.cancellation = cancellation;
        snapshot_.files.clear();
        snapshot_.symbols.clear();
        snapshot_.language_totals.clear();
        snapshot_.updated_at = 0;
        const bool need_files =
            name == "index_overview" || name == "glob" || name == "grep" ||
            name == "file_outline" || name == "edit_file";
        Error query_error = ok_error();
        if (need_files)
            query_error =
                index::query_files(query_options, snapshot_.files);
        if (query_error.ok() &&
            name == "index_overview") {
            index::QueryTotals totals;
            query_error = index::query_totals(query_options, totals);
            if (query_error.ok()) {
                snapshot_.updated_at = totals.updated_at;
                snapshot_.language_totals = std::move(totals.languages);
            }
        }
        if (query_error.ok() && name == "file_outline") {
            std::string path;
            if (get_string(args, "path", path, true, validation_error))
                query_error = index::query_symbols(
                    query_options,
                    {fs::u8path(path).generic_u8string()},
                    snapshot_.symbols);
        } else if (query_error.ok() && name == "read_symbol") {
            std::size_t id = 0;
            if (get_size(args, "symbol_id", 0,
                         static_cast<std::size_t>(
                             std::numeric_limits<int>::max()),
                         id, validation_error) &&
                id > 0) {
                index::IndexedSymbol symbol;
                bool found = false;
                query_error = index::query_symbol(
                    query_options, static_cast<long long>(id), symbol,
                    found);
                if (query_error.ok() && found)
                    snapshot_.symbols.push_back(std::move(symbol));
            }
        } else if (query_error.ok() && name == "search_symbol") {
            std::string query;
            std::size_t maximum = 50;
            (void)get_string(args, "query", query, true,
                             validation_error);
            (void)get_size(args, "max_results", 50, 200, maximum,
                           validation_error);
            std::vector<index::OwnedRankedSymbol> ranked;
            query_error = index::query_ranked_symbols(
                query_options, query, maximum + 1, ranked);
            if (query_error.ok()) {
                snapshot_.symbols.reserve(ranked.size());
                for (index::OwnedRankedSymbol& item : ranked)
                    snapshot_.symbols.push_back(std::move(item.symbol));
            }
        } else if (query_error.ok() && name == "index_overview") {
            query_error = index::query_symbols(
                query_options, {}, snapshot_.symbols, 4096);
        } else if (query_error.ok() && name == "edit_file") {
            const json::Value* ops = args.get("ops");
            if (ops != nullptr && ops->is_array()) {
                for (const json::Value& op : ops->array) {
                    if (!op.is_object()) continue;
                    const json::Value* id = op.get("symbol_id");
                    if (id == nullptr ||
                        id->type != json::Value::Type::Number)
                        continue;
                    index::IndexedSymbol symbol;
                    bool found = false;
                    query_error = index::query_symbol(
                        query_options, static_cast<long long>(id->number),
                        symbol, found);
                    if (!query_error.ok()) break;
                    if (found)
                        snapshot_.symbols.push_back(std::move(symbol));
                }
            }
        }
        if (!query_error.ok() &&
            (name == "glob" || name == "grep")) {
            index::Options discovery_options = query_options;
            discovery_options.on_progress = {};
            std::vector<index::DiscoveredFile> discovered;
            const Error discovery_error = index::discover_source_files(
                discovery_options, discovered);
            if (!discovery_error.ok())
                return tool_error_result(
                    error_code_string(discovery_error.code),
                    discovery_error.message);
            snapshot_.files.clear();
            for (const index::DiscoveredFile& file : discovered) {
                index::IndexedFile item;
                item.path = file.path;
                item.language = file.language;
                item.size = file.size;
                item.status = "indexed";
                snapshot_.files.push_back(std::move(item));
            }
            lazy_live_fallback = true;
            query_error = ok_error();
        }
        if (!query_error.ok())
            return tool_error_result(error_code_string(query_error.code),
                                     query_error.message);
        merge_index_overlay();
        rebuild_file_map();
    }

    if (name == "index_overview") {
        json::Value data = object_value();
        data.object["workspace"] = string_value(snapshot_.workspace);
        data.object["updated_at"] = number_value(static_cast<double>(snapshot_.updated_at));
        json::Value languages = array_value();
        std::size_t total_files = 0, total_lines = 0, total_bytes = 0;
        for (const index::LanguageTotal& total : snapshot_.language_totals) {
            json::Value item = object_value();
            item.object["language"] = string_value(index::language_name(total.language));
            item.object["files"] = number_value(total.files);
            item.object["lines"] = number_value(total.lines);
            item.object["bytes"] = number_value(static_cast<double>(total.bytes));
            languages.array.push_back(std::move(item));
            total_files += total.files; total_lines += total.lines; total_bytes += total.bytes;
        }
        data.object["languages"] = std::move(languages);
        data.object["files"] = number_value(total_files);
        data.object["lines"] = number_value(total_lines);
        data.object["bytes"] = number_value(total_bytes);
        json::Value important = array_value();
        static const std::vector<std::string> names = {"README.md", "AGENTS.md", "Makefile", "CMakeLists.txt", "package.json", "pyproject.toml", "Cargo.toml", "go.mod"};
        for (const std::string& candidate : names) if (files_.find(candidate) != files_.end()) important.array.push_back(string_value(candidate));
        data.object["important_files"] = std::move(important);
        json::Value entries = array_value();
        for (const index::IndexedSymbol& symbol : snapshot_.symbols) {
            const std::string lower = lowercase(symbol.symbol.name);
            if (lower == "main" || lower.rfind("run_", 0) == 0) {
                json::Value item = object_value(); item.object["path"] = string_value(symbol.path);
                item.object["symbol"] = string_value(symbol.symbol.qualified_name);
                item.object["line"] = number_value(symbol.symbol.line_start); entries.array.push_back(std::move(item));
                if (entries.array.size() == 20) break;
            }
        }
        data.object["entry_points"] = std::move(entries);
        json::Value central = array_value();
        std::vector<const index::IndexedSymbol*> anchors;
        anchors.reserve(snapshot_.symbols.size());
        for (const index::IndexedSymbol& symbol : snapshot_.symbols)
            anchors.push_back(&symbol);
        std::sort(anchors.begin(), anchors.end(), [](const auto* left,
                                                     const auto* right) {
            if (left->symbol.importance != right->symbol.importance)
                return left->symbol.importance > right->symbol.importance;
            if (left->path != right->path) return left->path < right->path;
            if (left->symbol.line_start != right->symbol.line_start)
                return left->symbol.line_start < right->symbol.line_start;
            return left->id < right->id;
        });
        if (anchors.size() > 10) anchors.resize(10);
        for (const index::IndexedSymbol* anchor : anchors) {
            json::Value item = object_value();
            item.object["path"] = string_value(anchor->path);
            item.object["symbol_id"] = number_value(anchor->id);
            item.object["symbol"] =
                string_value(anchor->symbol.qualified_name);
            item.object["line"] =
                number_value(anchor->symbol.line_start);
            item.object["importance"] =
                number_value(anchor->symbol.importance);
            central.array.push_back(std::move(item));
        }
        data.object["important_symbols"] = std::move(central);
        json::Value tests = array_value();
        if (files_.find("Makefile") != files_.end()) { tests.array.push_back(string_value("make test")); tests.array.push_back(string_value("make test-sanitize")); }
        if (files_.find("package.json") != files_.end()) tests.array.push_back(string_value("npm test"));
        data.object["likely_test_commands"] = std::move(tests);
        index::Freshness freshness;
        const Error fresh_error = index::check_freshness(index_options_, freshness);
        data.object["index_fresh"] = bool_value(fresh_error.ok() && freshness.fresh);
        std::vector<std::string> warnings;
        if (!fresh_error.ok()) warnings.push_back(fresh_error.message);
        else if (!freshness.fresh) warnings.push_back("workspace paths changed after the completed snapshot");
        return envelope(true, std::move(data), "", "", warnings, false);
    }

    if (name == "list_dir") {
        std::string path;
        std::size_t maximum = 200;
        if (!get_string(args, "path", path, false, validation_error) ||
            !get_size(args, "max_entries", 200, 500, maximum, validation_error))
            return tool_error_result("invalid_arguments", validation_error);
        bool external = false;
        fs::path absolute;
        if (!safe_relative_path(path)) {
            Error resolved =
                resolve_external_directory_path(snapshot_.workspace, path, absolute);
            if (!resolved.ok())
                return tool_error_result("policy_denied", resolved.message);
            external = true;
            const GuardApprovalDecision decision = request_permission(
                "list_dir", "list_dir " + absolute.generic_u8string(),
                {absolute.generic_u8string()}, true,
                resolved_path_is_under_system_temp(absolute), false, false,
                "ask_on_external_directory_read",
                "List this exact directory outside the active project?\nExact resolved path: " +
                    absolute.generic_u8string(),
                cancellation);
            if (decision != GuardApprovalDecision::Allow)
                return tool_error_result(
                    decision == GuardApprovalDecision::Cancelled ? "cancelled"
                                                                 : "policy_denied",
                    decision == GuardApprovalDecision::Cancelled
                        ? "list_dir approval cancelled"
                        : "external directory listing requires user approval");
            Error stable = ensure_approved_external_path_unchanged(
                absolute, "list directory", ErrorCode::FileRead);
            if (!stable.ok())
                return tool_error_result(error_code_string(stable.code),
                                         stable.message);
        } else {
            path = path == "." ? "" : fs::u8path(path).generic_u8string();
            if (!path.empty() && path.back() == '/') path.pop_back();
            absolute =
                path.empty() ? fs::u8path(snapshot_.workspace)
                             : fs::u8path(snapshot_.workspace) / fs::u8path(path);
        }
        std::error_code ec;
        if (!external && !path.empty()) {
            // Walk components and refuse symlinks / missing parents.
            fs::path current(snapshot_.workspace);
            for (const fs::path& component : fs::u8path(path)) {
                current /= component;
                const fs::file_status status = fs::symlink_status(current, ec);
                if (ec || status.type() == fs::file_type::not_found)
                    return tool_error_result("not_found", "directory does not exist: " + path);
                if (fs::is_symlink(status))
                    return tool_error_result("policy_denied",
                                            "refusing symlink path in list_dir: " + path);
            }
            if (!fs::is_directory(absolute, ec) || ec)
                return tool_error_result("not_found", "path is not a directory: " + path);
        }

        // Real readdir so empty directories and non-indexed files (e.g. #backup#) appear.
        struct DirEntry {
            std::string name;
            std::string type;  // file | directory | other
            std::uintmax_t size = 0;
            bool empty = false;
            bool indexed = false;
        };
        std::map<std::string, DirEntry> entries;
        fs::directory_iterator it(absolute, fs::directory_options::skip_permission_denied, ec);
        if (ec)
            return tool_error_result("file_read",
                                    "could not list directory: " + (path.empty() ? std::string(".") : path) +
                                        ": " + ec.message());
        for (const fs::directory_entry& entry : it) {
            const std::string name = entry.path().filename().u8string();
            if (name.empty() || name == "." || name == "..") continue;
            if (is_protected_listing_name(name)) continue;
            // Skip hidden protected-style components only; other dotfiles remain visible.
            const fs::file_status status = entry.symlink_status(ec);
            if (ec) continue;
            if (fs::is_symlink(status)) {
                DirEntry item;
                item.name = name;
                item.type = "symlink";
                entries[name] = std::move(item);
                continue;
            }
            DirEntry item;
            item.name = name;
            if (fs::is_directory(status)) {
                item.type = "directory";
                bool is_empty = true;
                std::error_code empty_ec;
                fs::directory_iterator child(entry.path(),
                                             fs::directory_options::skip_permission_denied,
                                             empty_ec);
                if (!empty_ec) {
                    for (const fs::directory_entry& nested : child) {
                        const std::string nested_name = nested.path().filename().u8string();
                        if (nested_name == "." || nested_name == "..") continue;
                        is_empty = false;
                        break;
                    }
                }
                item.empty = is_empty;
            } else if (fs::is_regular_file(status)) {
                item.type = "file";
                item.size = entry.file_size(ec);
                if (ec) item.size = 0;
            } else {
                item.type = "other";
            }
            const std::string relative =
                path.empty() ? name
                             : (fs::u8path(path) / fs::u8path(name)).generic_u8string();
            item.indexed =
                !external &&
                (eligible_indexed_path(snapshot_, relative, false) ||
                 (item.type == "directory" &&
                  eligible_indexed_path(snapshot_, relative, true)));
            entries[name] = std::move(item);
        }

        json::Value data = array_value();
        bool truncated = false;
        for (const auto& entry : entries) {
            if (data.array.size() >= maximum) {
                truncated = true;
                break;
            }
            const DirEntry& item = entry.second;
            json::Value row = object_value();
            row.object["name"] = string_value(item.name);
            row.object["type"] = string_value(item.type);
            row.object["size"] = number_value(static_cast<double>(item.size));
            row.object["indexed"] = bool_value(item.indexed);
            if (item.type == "directory") row.object["empty"] = bool_value(item.empty);
            data.array.push_back(std::move(row));
        }
        std::vector<std::string> warnings;
        if (!external && path.empty())
            warnings.push_back(
                "filesystem listing (not index-only); empty directories and non-source files included");
        if (external)
            warnings.push_back("outside-project directory listing");
        return envelope(true, std::move(data), "", "", warnings, truncated);
    }

    if (name == "glob") {
        std::string pattern;
        std::size_t maximum = 200;
        if (!get_string(args, "pattern", pattern, true, validation_error) ||
            !get_size(args, "max_results", 200, 1000, maximum, validation_error))
            return tool_error_result("invalid_arguments", validation_error);
        if (!safe_relative_path(pattern))
            return tool_error_result("policy_denied", unsafe_path_message(pattern, "search"));
        json::Value data = array_value(); bool truncated = false;
        try {
            std::vector<std::string> paths;
            if (indexing_enabled_ && !lazy_live_fallback) {
                paths.reserve(snapshot_.files.size());
                for (const index::IndexedFile& file : snapshot_.files)
                    paths.push_back(file.path);
            } else {
                index::Options discovery_options = index_options_;
                discovery_options.cancellation = cancellation;
                discovery_options.interrupted = {};
                discovery_options.on_progress = {};
                std::vector<index::DiscoveredFile> discovered;
                const Error discovery_error =
                    index::discover_source_files(discovery_options, discovered);
                if (!discovery_error.ok())
                    return tool_error_result(
                        error_code_string(discovery_error.code),
                        discovery_error.message);
                paths.reserve(discovered.size());
                for (const index::DiscoveredFile& file : discovered)
                    paths.push_back(file.path);
            }
            for (const std::string& path : paths) {
                if (cancellation.cancelled())
                    return tool_error_result("cancelled",
                                             "glob search cancelled");
                if (!glob_matches(path, pattern)) continue;
                if (data.array.size() >= maximum) { truncated = true; break; }
                data.array.push_back(string_value(path));
            }
        } catch (const std::regex_error& exception) {
            return tool_error_result("invalid_glob", exception.what());
        }
        return envelope(true, std::move(data), "", "", {}, truncated);
    }

    if (name == "search_symbol") {
        std::string query;
        std::size_t maximum = 50;
        if (!get_string(args, "query", query, true, validation_error) ||
            !get_size(args, "max_results", 50, 200, maximum, validation_error))
            return tool_error_result("invalid_arguments", validation_error);
        const std::vector<index::RankedSymbol> ranked =
            index::rank_task_symbols(snapshot_, query, maximum + 1);
        json::Value data = array_value(); bool truncated = ranked.size() > maximum;
        for (std::size_t i = 0; i < std::min(maximum, ranked.size()); ++i) {
            const index::IndexedSymbol& symbol = *ranked[i].symbol;
            json::Value item = object_value(); item.object["id"] = number_value(symbol.id);
            item.object["path"] = string_value(symbol.path); item.object["kind"] = string_value(symbol.symbol.kind);
            item.object["name"] = string_value(symbol.symbol.qualified_name); item.object["signature"] = string_value(symbol.symbol.signature);
            item.object["line_start"] = number_value(symbol.symbol.line_start); item.object["line_end"] = number_value(symbol.symbol.line_end);
            item.object["importance"] =
                number_value(symbol.symbol.importance);
            data.array.push_back(std::move(item));
        }
        return envelope(true, std::move(data), "", "", {}, truncated);
    }

    if (name == "file_outline") {
        std::string path;
        if (!get_string(args, "path", path, true, validation_error)) return tool_error_result("invalid_arguments", validation_error);
        const auto file = files_.find(fs::u8path(path).generic_u8string());
        if (file == files_.end() || file->second->status != "indexed") return tool_error_result("not_found", "file is not indexed: " + path);
        json::Value data = array_value();
        for (const index::IndexedSymbol& symbol : snapshot_.symbols) if (symbol.path == file->second->path) {
            json::Value item = object_value(); item.object["id"] = number_value(symbol.id);
            item.object["kind"] = string_value(symbol.symbol.kind); item.object["name"] = string_value(symbol.symbol.qualified_name);
            item.object["signature"] = string_value(symbol.symbol.signature); item.object["line_start"] = number_value(symbol.symbol.line_start);
            item.object["line_end"] = number_value(symbol.symbol.line_end); item.object["documentation"] = string_value(symbol.symbol.documentation);
            item.object["importance"] = number_value(symbol.symbol.importance);
            data.array.push_back(std::move(item));
        }
        return envelope(true, std::move(data), "", "", {}, false);
    }

    if (name == "read_symbol") {
        std::size_t id = 0;
        if (!get_size(args, "symbol_id", 0, static_cast<std::size_t>(std::numeric_limits<int>::max()), id, validation_error) || id == 0)
            return tool_error_result("invalid_arguments", validation_error.empty() ? "symbol_id must be positive" : validation_error);
        const index::IndexedSymbol* found = nullptr;
        for (const index::IndexedSymbol& symbol : snapshot_.symbols) if (symbol.id == static_cast<long long>(id)) { found = &symbol; break; }
        if (found == nullptr) return tool_error_result("not_found", "indexed symbol id was not found");
        SourceRange range;
        const Error error = read_source(found->path, found->symbol.line_start, found->symbol.line_end, 262144, range);
        if (!error.ok()) return tool_error_result(error_code_string(error.code), error.message);
        json::Value data = object_value(); data.object["symbol_id"] = number_value(found->id); data.object["path"] = string_value(range.path);
        data.object["line_start"] = number_value(range.start_line); data.object["line_end"] = number_value(range.end_line);
        data.object["content"] = string_value(range.content); data.object["file_hash"] = string_value(range.file_hash); data.object["range_hash"] = string_value(range.range_hash);
        data.object["importance"] =
            number_value(found->symbol.importance);
        std::vector<std::string> warnings; if (range.redacted) warnings.push_back("configured credential value was redacted");
        return envelope(true, std::move(data), "", "", warnings, range.truncated);
    }

    if (name == "read_file") {
        std::string path;
        std::size_t start = 1, end = 0, maximum = 65536;
        if (!get_string(args, "path", path, true, validation_error) ||
            !get_size(args, "start_line", 1, 100000000, start, validation_error) ||
            !get_size(args, "end_line", 0, 100000000, end, validation_error) ||
            !get_size(args, "max_bytes", 65536, 262144, maximum, validation_error) || maximum == 0)
            return tool_error_result("invalid_arguments", validation_error.empty() ? "max_bytes must be positive" : validation_error);
        SourceRange range;
        Error error;
        if (safe_relative_path(path) && !path.empty()) {
            error = mutation_policy_ == MutationPolicy::Disabled
                        ? read_source(path, start, end, maximum, range)
                        : read_workspace_source(path, start, end, maximum, range);
        } else {
            fs::path absolute;
            error = resolve_external_file_path(snapshot_.workspace, path, true, absolute);
            if (error.ok()) {
                const GuardApprovalDecision decision =
                    request_permission(
                        "read_file", "read_file " + absolute.generic_u8string(),
                        {absolute.generic_u8string()}, true,
                        resolved_path_is_under_system_temp(absolute), false, false,
                        "ask_on_external_file_read",
                        "Read this file outside the active project?\nExact resolved path: " +
                            absolute.generic_u8string() +
                            "\nApproval applies only to this tool call and returned text remains "
                            "bounded and credential-redacted.",
                        cancellation);
                if (decision == GuardApprovalDecision::Allow) {
                    if (cancellation.cancelled())
                        error = {ErrorCode::Cancelled,
                                 "external file read cancelled after approval"};
                    else
                        error = read_external_source(absolute, start, end, maximum, range);
                } else {
                    std::string message =
                        "refusing to read file outside the project directory without user "
                        "approval: " +
                        absolute.generic_u8string();
                    if (decision == GuardApprovalDecision::Cancelled)
                        message += " (approval cancelled)";
                    else if (!on_guard_ask_)
                        message += " (headless agent denies external file access)";
                    else
                        message += " (user selected No)";
                    error = {decision == GuardApprovalDecision::Cancelled
                                 ? ErrorCode::Cancelled
                                 : ErrorCode::UnsupportedFeature,
                             message};
                }
            }
        }
        if (!error.ok()) {
            const std::string code =
                error.code == ErrorCode::UnsupportedFeature ? "policy_denied"
                                                            : error_code_string(error.code);
            return tool_error_result(code, error.message);
        }
        json::Value data = object_value(); data.object["path"] = string_value(range.path);
        data.object["line_start"] = number_value(range.start_line); data.object["line_end"] = number_value(range.end_line);
        data.object["content"] = string_value(number_source_lines(range)); data.object["file_hash"] = string_value(range.file_hash);
        data.object["range_hash"] = string_value(range.range_hash); data.object["bytes"] = number_value(range.bytes);
        std::vector<std::string> warnings; if (range.redacted) warnings.push_back("configured credential value was redacted");
        return envelope(true, std::move(data), "", "", warnings, range.truncated);
    }

    if (name == "read_many") {
        const json::Value* items = args.get("items");
        std::size_t maximum = 262144;
        if (items == nullptr || !items->is_array() || items->array.empty() ||
            items->array.size() > 100 ||
            !get_size(args, "max_bytes", 262144, 262144, maximum, validation_error) || maximum == 0)
            return tool_error_result(
                "invalid_arguments",
                validation_error.empty()
                    ? "items must be an array containing 1 to 100 ranges"
                    : validation_error);
        std::map<std::size_t, fs::path> external_paths;
        bool all_external_under_temp = true;
        for (std::size_t index = 0; index < items->array.size(); ++index) {
            const json::Value& item = items->array[index];
            if (!item.is_object()) continue;
            std::string item_path;
            std::string item_error;
            if (!get_string(item, "path", item_path, true, item_error)) continue;
            if (safe_relative_path(item_path) && !item_path.empty()) continue;
            fs::path absolute;
            Error resolved =
                resolve_external_file_path(snapshot_.workspace, item_path, true, absolute);
            if (!resolved.ok()) return tool_error_result("policy_denied", resolved.message);
            all_external_under_temp =
                all_external_under_temp && resolved_path_is_under_system_temp(absolute);
            external_paths[index] = std::move(absolute);
        }
        if (!external_paths.empty()) {
            std::vector<std::string> exact_paths;
            exact_paths.reserve(external_paths.size());
            for (const auto& item : external_paths)
                exact_paths.push_back(item.second.generic_u8string());
            const GuardApprovalDecision decision = request_permission(
                "read_many",
                "read_many " + std::to_string(external_paths.size()) +
                    " outside-project file(s)",
                exact_paths, true, all_external_under_temp, false, false,
                "ask_on_external_read_many",
                "Read these exact files outside the active project? One approval covers all " +
                    std::to_string(external_paths.size()) +
                    " validated paths; returned text remains bounded and credential-redacted.",
                cancellation);
            if (decision != GuardApprovalDecision::Allow)
                return tool_error_result(
                    decision == GuardApprovalDecision::Cancelled ? "cancelled"
                                                                 : "policy_denied",
                    decision == GuardApprovalDecision::Cancelled
                        ? "read_many approval cancelled"
                        : "read_many external paths require user approval");
        }
        json::Value data = array_value(); std::vector<std::string> warnings; std::size_t remaining = maximum;
        bool truncated = false;
        for (std::size_t index = 0; index < items->array.size(); ++index) {
            if (cancellation.cancelled())
                return tool_error_result("cancelled", "read_many cancelled");
            if (remaining == 0) { warnings.push_back("omitted item " + std::to_string(index) + " because the aggregate cap was reached"); truncated = true; continue; }
            const json::Value& item = items->array[index];
            if (!item.is_object()) { warnings.push_back("omitted item " + std::to_string(index) + ": range must be an object"); truncated = true; continue; }
            std::string path;
            std::size_t start = 1, end = 0, item_maximum = 65536;
            if (!get_string(item, "path", path, true, validation_error) ||
                !get_size(item, "start_line", 1, 100000000, start, validation_error) ||
                !get_size(item, "end_line", 0, 100000000, end, validation_error) ||
                !get_size(item, "max_bytes", 65536, 262144, item_maximum,
                          validation_error) ||
                item_maximum == 0) {
                warnings.push_back("omitted item " + std::to_string(index) + ": " + validation_error); truncated = true; continue;
            }
            const std::size_t effective_maximum =
                std::min(remaining, item_maximum);
            SourceRange range;
            Error error;
            const auto external = external_paths.find(index);
            if (external == external_paths.end()) {
                error = mutation_policy_ == MutationPolicy::Disabled
                            ? read_source(path, start, end, effective_maximum, range)
                            : read_workspace_source(path, start, end, effective_maximum, range);
            } else {
                error = read_external_source(external->second, start, end,
                                             effective_maximum, range);
            }
            if (!error.ok()) { warnings.push_back("omitted " + path + ": " + error.message); truncated = true; continue; }
            json::Value output = object_value(); output.object["path"] = string_value(range.path);
            output.object["line_start"] = number_value(range.start_line); output.object["line_end"] = number_value(range.end_line);
            output.object["content"] = string_value(number_source_lines(range)); output.object["file_hash"] = string_value(range.file_hash);
            output.object["range_hash"] = string_value(range.range_hash);
            output.object["bytes"] = number_value(range.bytes);
            output.object["truncated"] = bool_value(range.truncated);
            data.array.push_back(std::move(output)); remaining -= std::min(remaining, range.bytes);
            truncated = truncated || range.truncated; if (range.redacted) warnings.push_back(path + ": configured credential value was redacted");
        }
        json::Value metadata = object_value();
        metadata.object["requested_items"] = number_value(items->array.size());
        metadata.object["returned_items"] = number_value(data.array.size());
        metadata.object["byte_cap"] = number_value(maximum);
        metadata.object["bytes_remaining"] = number_value(remaining);
        return envelope(true, std::move(data), "", "", warnings, truncated, std::move(metadata));
    }

    if (name == "grep") {
        std::string query, pattern, path, glob;
        bool regex_mode = false, case_sensitive = false, word = false;
        std::size_t context = 0, maximum = 50;
        if (!get_string(args, "query", query, false, validation_error) ||
            !get_string(args, "pattern", pattern, false, validation_error) ||
            !get_string(args, "path", path, false, validation_error) ||
            !get_string(args, "glob", glob, false, validation_error) ||
            !get_bool(args, "regex", false, regex_mode, validation_error) ||
            !get_bool(args, "case_sensitive", false, case_sensitive,
                      validation_error) ||
            !get_bool(args, "word", false, word, validation_error) ||
            !get_size(args, "context", 0, 10, context, validation_error) ||
            !get_size(args, "max_results", 50, 500, maximum, validation_error))
            return tool_error_result("invalid_arguments", validation_error);
        if (!query.empty() && !pattern.empty() && query != pattern)
            return tool_error_result(
                "invalid_arguments",
                "query and pattern disagree; provide only query or use the same value");
        if (query.empty()) query = pattern;
        if (query.empty())
            return tool_error_result(
                "invalid_arguments",
                "missing required non-empty string argument: query (pattern is accepted as an alias)");
        if (!path.empty() && !safe_relative_path(path))
            return tool_error_result("policy_denied",
                                     unsafe_path_message(path, "search"));
        const std::string search_root = normalize_glob_path(path);
        const bool regex_was_supplied = args.get("regex") != nullptr;
        const bool inferred_regex =
            !regex_was_supplied && has_unescaped_alternation(query);
        if (inferred_regex) regex_mode = true;
        std::regex expression;
        const bool use_regex = regex_mode || word;
        // Validate ECMAScript patterns even when ripgrep will run, so invalid
        // regex still returns a structured error and portable fallback stays ready.
        if (use_regex) {
            try {
                std::string built = regex_mode ? query : regex_escape(query);
                if (word) built = "\\b(?:" + built + ")\\b";
                expression = std::regex(
                    built,
                    std::regex::ECMAScript |
                        (case_sensitive ? std::regex::flag_type{}
                                        : std::regex::icase));
            } catch (const std::regex_error& exception) {
                return tool_error_result("invalid_regex", exception.what());
            }
        }
        const std::string literal_query =
            case_sensitive ? query : lowercase(query);
        auto matches_line = [&](const std::string& line) {
            if (use_regex) return std::regex_search(line, expression);
            if (case_sensitive)
                return line.find(literal_query) != std::string::npos;
            return lowercase(line).find(literal_query) != std::string::npos;
        };
        struct SearchFile {
            std::string path;
            std::uintmax_t size = 0;
        };
        // Candidate universe: indexed files when available, else live discovery.
        // Order: rg (if present) → this candidate list with built-in scan.
        std::vector<SearchFile> candidates;
        const bool use_index_candidates =
            indexing_enabled_ && !lazy_live_fallback;
        if (use_index_candidates) {
            candidates.reserve(snapshot_.files.size());
            for (const index::IndexedFile& file : snapshot_.files) {
                if (file.status == "indexed")
                    candidates.push_back({file.path, file.size});
            }
        } else {
            index::Options discovery_options = index_options_;
            discovery_options.cancellation = cancellation;
            discovery_options.interrupted = {};
            discovery_options.on_progress = {};
            std::vector<index::DiscoveredFile> discovered;
            const Error discovery_error =
                index::discover_source_files(discovery_options, discovered);
            if (!discovery_error.ok())
                return tool_error_result(
                    error_code_string(discovery_error.code),
                    discovery_error.message);
            candidates.reserve(discovered.size());
            for (const index::DiscoveredFile& file : discovered) {
                if (file.size <= index_options_.max_source_code_file_size)
                    candidates.push_back({file.path, file.size});
            }
        }
        std::set<std::string> eligible_paths;
        for (const SearchFile& file : candidates)
            eligible_paths.insert(normalize_glob_path(file.path));

        json::Value data = array_value();
        std::vector<std::string> warnings;
        bool truncated = false;
        if (inferred_regex)
            warnings.push_back(
                "regex=true inferred because query contains an unescaped '|'");
        else if (!regex_mode && has_unescaped_alternation(query))
            warnings.push_back(
                "regex=false was explicit; query was searched literally, including '|'");

        auto finish_search = [&](const char* backend) {
            json::Value metadata = object_value();
            metadata.object["search_backend"] = string_value(backend);
            if (!search_root.empty()) {
                bool any_in_root = false;
                for (const std::string& candidate : eligible_paths) {
                    if (path_in_search_root(candidate, search_root)) {
                        any_in_root = true;
                        break;
                    }
                }
                if (!any_in_root)
                    warnings.push_back(
                        "path is not an eligible source file or directory: " +
                        search_root);
            }
            return envelope(true, std::move(data), "", "", warnings, truncated,
                            std::move(metadata));
        };

        // --- Prefer system ripgrep when present (soft dependency; Windows often
        // lacks it). Failures fall through to the portable built-in scanner. ---
        if (ripgrep_available() && !cancellation.cancelled()) {
            std::vector<std::string> argv = {
                "rg",          "--with-filename", "--line-number",
                "--no-heading", "--color=never",  "--no-config"};
            if (!case_sensitive) argv.push_back("-i");
            if (word) argv.push_back("-w");
            if (!regex_mode) argv.push_back("-F");
            if (context > 0) {
                argv.push_back("-C");
                argv.push_back(std::to_string(context));
            }
            // Per-file cap; we also enforce a global max_results while parsing.
            argv.push_back("--max-count");
            argv.push_back(std::to_string(maximum));
            if (!glob.empty()) {
                std::vector<std::string> alternatives;
                expand_braces(normalize_glob_path(glob), alternatives);
                for (const std::string& alternative : alternatives) {
                    argv.push_back("--glob");
                    argv.push_back(alternative);
                }
            }
            argv.push_back("--");
            argv.push_back(query);
            if (!search_root.empty())
                argv.push_back(search_root);
            else
                argv.push_back(".");

            ProcessOptions process_options;
            process_options.workspace = snapshot_.workspace;
            process_options.timeout_ms = 60000;
            // Bound stdout generously so max_results hits are not truncated mid-line.
            process_options.stdout_limit =
                std::max<std::size_t>(256 * 1024, maximum * 8192);
            process_options.stderr_limit = 65536;
            process_options.cancellation = cancellation;

            ProcessResult process;
            const Error rg_error =
                run_argv(std::move(argv), process_options, process,
                         CommandPolicy::InspectionOnly);
            // exit 0 = matches, 1 = no matches; both are successful searches.
            const bool rg_ok =
                rg_error.ok() &&
                (process.exit_status == 0 || process.exit_status == 1) &&
                !process.cancelled && !process.timed_out;
            if (rg_error.code == ErrorCode::Cancelled || process.cancelled)
                return tool_error_result("cancelled", "text search cancelled");
            if (rg_ok) {
                // Group context lines that precede each match within a -C block.
                struct PendingContext {
                    std::size_t line = 0;
                    std::string text;
                };
                std::vector<PendingContext> pending_context;
                std::string current_path;
                std::istringstream stream(process.stdout_text);
                std::string line;
                while (std::getline(stream, line)) {
                    if (cancellation.cancelled())
                        return tool_error_result("cancelled",
                                                 "text search cancelled");
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line == "--") {
                        pending_context.clear();
                        continue;
                    }
                    std::string hit_path;
                    std::size_t hit_line = 0;
                    std::string hit_text;
                    bool is_match = false;
                    if (!parse_rg_output_line(line, hit_path, hit_line, hit_text,
                                             is_match))
                        continue;
                    if (!path_in_search_root(hit_path, search_root)) continue;
                    if (!glob.empty() && !glob_matches(hit_path, glob)) continue;
                    // Keep results inside the eligible index/discovery universe so
                    // security-review and agent semantics match the built-in path.
                    if (eligible_paths.find(hit_path) == eligible_paths.end())
                        continue;
                    if (!is_match) {
                        if (hit_path != current_path) {
                            pending_context.clear();
                            current_path = hit_path;
                        }
                        pending_context.push_back({hit_line, hit_text});
                        continue;
                    }
                    if (data.array.size() >= maximum) {
                        truncated = true;
                        break;
                    }
                    json::Value match = object_value();
                    match.object["path"] = string_value(hit_path);
                    match.object["line"] = number_value(hit_line);
                    match.object["text"] = string_value(hit_text);
                    if (context > 0) {
                        json::Value nearby = array_value();
                        for (const PendingContext& item : pending_context) {
                            if (item.line == hit_line) continue;
                            json::Value row = object_value();
                            row.object["line"] = number_value(item.line);
                            row.object["text"] = string_value(item.text);
                            nearby.array.push_back(std::move(row));
                        }
                        // Following context lines arrive after the match until `--`.
                        match.object["context"] = std::move(nearby);
                    }
                    data.array.push_back(std::move(match));
                    pending_context.clear();
                    current_path = hit_path;
                }
                // Attach trailing context lines that appear after each match.
                // Re-parse is expensive; for context>0 do a second pass only if needed.
                if (context > 0 && !data.array.empty()) {
                    // Rebuild context with both before and after lines from full parse.
                    std::vector<std::pair<std::string, std::size_t>> match_keys;
                    match_keys.reserve(data.array.size());
                    for (const json::Value& item : data.array) {
                        const json::Value* p = item.get("path");
                        const json::Value* l = item.get("line");
                        if (p != nullptr && p->is_string() && l != nullptr &&
                            l->type == json::Value::Type::Number)
                            match_keys.push_back(
                                {p->string,
                                 static_cast<std::size_t>(l->number)});
                    }
                    std::map<std::pair<std::string, std::size_t>,
                             std::vector<std::pair<std::size_t, std::string>>>
                        context_by_match;
                    std::string block_path;
                    std::vector<std::pair<std::size_t, std::string>> block_lines;
                    std::vector<std::size_t> block_match_lines;
                    auto flush_block = [&]() {
                        for (std::size_t match_line : block_match_lines) {
                            auto& bucket =
                                context_by_match[{block_path, match_line}];
                            for (const auto& row : block_lines) {
                                if (row.first == match_line) continue;
                                bucket.push_back(row);
                            }
                        }
                        block_lines.clear();
                        block_match_lines.clear();
                    };
                    std::istringstream again(process.stdout_text);
                    while (std::getline(again, line)) {
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        if (line == "--") {
                            flush_block();
                            block_path.clear();
                            continue;
                        }
                        std::string hit_path;
                        std::size_t hit_line = 0;
                        std::string hit_text;
                        bool is_match = false;
                        if (!parse_rg_output_line(line, hit_path, hit_line,
                                                 hit_text, is_match))
                            continue;
                        if (block_path.empty()) block_path = hit_path;
                        if (hit_path != block_path) {
                            flush_block();
                            block_path = hit_path;
                        }
                        block_lines.push_back({hit_line, hit_text});
                        if (is_match) block_match_lines.push_back(hit_line);
                    }
                    flush_block();
                    for (json::Value& item : data.array) {
                        const json::Value* p = item.get("path");
                        const json::Value* l = item.get("line");
                        if (p == nullptr || !p->is_string() || l == nullptr ||
                            l->type != json::Value::Type::Number)
                            continue;
                        const auto key = std::make_pair(
                            p->string, static_cast<std::size_t>(l->number));
                        const auto found = context_by_match.find(key);
                        if (found == context_by_match.end()) continue;
                        json::Value nearby = array_value();
                        for (const auto& row : found->second) {
                            json::Value entry = object_value();
                            entry.object["line"] = number_value(row.first);
                            entry.object["text"] = string_value(row.second);
                            nearby.array.push_back(std::move(entry));
                        }
                        item.object["context"] = std::move(nearby);
                    }
                }
                if (process.stdout_truncated)
                    warnings.push_back(
                        "ripgrep output hit the internal capture limit; results "
                        "may be incomplete");
                return finish_search("rg");
            }
            // Soft failure: missing binary race, timeout, or unexpected exit →
            // portable built-in path.
            if (!rg_error.ok() && rg_error.code != ErrorCode::FileRead)
                warnings.push_back("ripgrep unavailable or failed (" +
                                   rg_error.message +
                                   "); used built-in text search");
            else if (process.exit_status > 1)
                warnings.push_back(
                    "ripgrep exited with status " +
                    std::to_string(process.exit_status) +
                    "; used built-in text search");
            data = array_value();
            truncated = false;
        }

        // --- Built-in portable scanner (index candidates, else live discovery). ---
        for (const SearchFile& file : candidates) {
            if (cancellation.cancelled())
                return tool_error_result("cancelled", "text search cancelled");
            if (!path_in_search_root(file.path, search_root)) continue;
            if (!glob.empty() && !glob_matches(file.path, glob)) continue;
            SourceRange source;
            const std::size_t read_cap = std::max<std::size_t>(
                1, index_options_.max_source_code_file_size);
            const Error read_error =
                use_index_candidates
                    ? read_source(file.path, 1, 0, read_cap, source)
                    : read_workspace_source(file.path, 1, 0, read_cap, source);
            if (!read_error.ok()) {
                warnings.push_back(read_error.message);
                continue;
            }
            const std::vector<std::string> lines = split_lines(source.content);
            for (std::size_t line = 0; line < lines.size(); ++line) {
                if (cancellation.cancelled())
                    return tool_error_result("cancelled",
                                             "text search cancelled");
                if (!matches_line(lines[line])) continue;
                if (data.array.size() >= maximum) {
                    truncated = true;
                    break;
                }
                json::Value match = object_value();
                match.object["path"] = string_value(file.path);
                match.object["line"] = number_value(line + 1);
                match.object["text"] = string_value(lines[line]);
                if (context > 0) {
                    json::Value nearby = array_value();
                    const std::size_t begin =
                        line > context ? line - context : 0;
                    const std::size_t finish =
                        std::min(lines.size(), line + context + 1);
                    for (std::size_t adjacent = begin; adjacent < finish;
                         ++adjacent) {
                        if (adjacent == line) continue;
                        json::Value item = object_value();
                        item.object["line"] = number_value(adjacent + 1);
                        item.object["text"] = string_value(lines[adjacent]);
                        nearby.array.push_back(std::move(item));
                    }
                    match.object["context"] = std::move(nearby);
                }
                data.array.push_back(std::move(match));
            }
            if (truncated) break;
        }
        return finish_search(use_index_candidates ? "builtin_index"
                                                  : "builtin_live");
    }

    if (name == "run_command") {
        std::string command, cwd;
        const bool full = mutation_policy_ == MutationPolicy::Full;
        const bool agent_session =
            mutation_policy_ != MutationPolicy::Disabled;
        const std::size_t timeout_cap =
            mutation_policy_ == MutationPolicy::Disabled ? 10000 : 120000;
        std::size_t timeout = full ? 30000 : 10000;
        if (!get_string(args, "command", command, true, validation_error) ||
            !get_string(args, "cwd", cwd, false, validation_error) ||
            !get_size(args, "timeout_ms", timeout, timeout_cap, timeout, validation_error) ||
            timeout == 0)
            return tool_error_result("invalid_arguments",
                                    validation_error.empty() ? "timeout_ms must be positive"
                                                             : validation_error);
        std::vector<std::string> parsed_arguments;
        std::string guard_rule_id;
        const CommandPolicy policy =
            full ? CommandPolicy::Agent
                 : (agent_session ? CommandPolicy::PlanReadOnly
                                  : CommandPolicy::InspectionOnly);
        // Defer Ask so path validation runs before the interactive prompt.
        const GuardAskHandling preview_ask =
            policy == CommandPolicy::Agent ? GuardAskHandling::DeferAsk
                                           : GuardAskHandling::DenyAsk;
        const bool unrestricted_yolo =
            permission_controls_ && permission_mode_ == PermissionMode::Yolo && full;
        Error policy_error =
            parse_command(command, parsed_arguments, policy, guard_rule_id, preview_ask,
                          nullptr, cancellation, agent_session, unrestricted_yolo);
        if (!policy_error.ok()) {
            const std::string code =
                policy_error.message.find("refusing") != std::string::npos ||
                        policy_error.message.find("not allowed") != std::string::npos ||
                        policy_error.message.find("blocked") != std::string::npos
                    ? "policy_denied"
                    : error_code_string(policy_error.code);
            return tool_error_result(code, policy_error.message);
        }
        // Security-review keeps path scope to the completed index. Agent mode allows
        // real workspace paths (empty dirs, #files#, scripts to execute).
        const bool index_only_commands =
            mutation_policy_ == MutationPolicy::Disabled;
        bool uses_external = false;
        policy_error = validate_command_workspace_paths(
            snapshot_, parsed_arguments, cwd, index_only_commands, agent_session,
            uses_external);
        if (!policy_error.ok()) return tool_error_result("policy_denied", policy_error.message);
        if (!full && agent_session && uses_external)
            return tool_error_result(
                "policy_denied",
                "Plan run_command paths and cwd must remain inside the project");
        const ReadOnlyCommandAssessment read_only =
            assess_read_only_command(parsed_arguments);
        bool read_only_uses_external = false;
        if (read_only.vetted) {
            policy_error = validate_vetted_read_only_paths(
                snapshot_, read_only, cwd, read_only_uses_external);
            if (!policy_error.ok())
                return tool_error_result("policy_denied", policy_error.message);
            uses_external = uses_external || read_only_uses_external;
        }
        const bool smart_read_only_exemption =
            permission_controls_ && permission_mode_ == PermissionMode::Smart &&
            read_only.vetted && !uses_external && guard_rule_id.empty();
        const GuardApprovalDecision permission =
            smart_read_only_exemption
                ? GuardApprovalDecision::Allow
                : request_permission(
                      "run_command", format_command_preview(parsed_arguments),
                      parsed_arguments, uses_external, false, !read_only.vetted,
                      !guard_rule_id.empty(), guard_rule_id,
                      guard_rule_id.empty()
                          ? std::string()
                          : "This command also matched Guard rule " +
                                guard_rule_id +
                                ". One approval covers the complete validated command call.",
                      cancellation);
        if (permission != GuardApprovalDecision::Allow) {
            return tool_error_result(
                permission == GuardApprovalDecision::Cancelled ? "cancelled"
                                                               : "policy_denied",
                permission == GuardApprovalDecision::Cancelled
                    ? "run_command approval cancelled"
                    : "run_command requires user approval in " +
                          std::string(permission_mode_name(permission_mode_)) + " mode");
        }
        ProcessOptions options;
        options.workspace = snapshot_.workspace;
        options.cwd = cwd;
        options.allow_external_cwd = uses_external || unrestricted_yolo;
        options.allow_external_paths = agent_session || unrestricted_yolo;
        // Act/Plan: resolve ./script.sh and bare project scripts under cwd/root.
        options.allow_workspace_executables = full;
        options.unrestricted = unrestricted_yolo;
        options.timeout_ms = static_cast<long>(timeout);
        options.cancellation = cancellation;
        // The complete call was approved above (or Yolo allowed it). Confirm/Smart
        // still fail closed on hard Guard denials; Yolo skips those denials via
        // unrestricted.
        if (permission_controls_) {
            options.on_guard_ask =
                [](const GuardApprovalRequest&, runtime::CancellationToken) {
                    return GuardApprovalDecision::Allow;
                };
        } else if (on_guard_ask_) {
            options.on_guard_ask = on_guard_ask_;
        }
        ProcessResult process;
        const Error error = run_command(command, options, process, policy);
        if (mutation_policy_ == MutationPolicy::Full && !read_only.vetted)
            queue_index_paths({}, true);
        bool output_filtered = false;
        const std::string command_name = parsed_arguments.empty() ? std::string() : parsed_arguments.front();
        // parse_inspection_command has already inserted the fixed Git -c
        // hardening pairs, leaving the validated subcommand at index 9.
        const bool git_file_listing = command_name == "git" && parsed_arguments.size() > 9 &&
                                      parsed_arguments[9] == "ls-files";
        // Only security-review treats the index snapshot as an output
        // authorization list. Agent Act/Plan commands operate on the validated
        // live project filesystem, so filtering their stdout would incorrectly
        // hide safe generated, ignored, or otherwise unindexed project paths
        // (and cannot reliably parse formatted output such as `ls -l`).
        if (error.ok() && index_only_commands && !uses_external &&
            (command_name == "find" || command_name == "ls" ||
                           command_name == "rg" || command_name == "grep" || git_file_listing)) {
            process.stdout_text = filter_path_lines(snapshot_, cwd, process.stdout_text,
                                                    git_file_listing ? "find" : command_name,
                                                    parsed_arguments, output_filtered,
                                                    index_only_commands);
        }
        json::Value data = object_value(); json::Value arguments = array_value();
        for (const std::string& argument : process.arguments)
            arguments.array.push_back(string_value(redact_secrets(argument, secrets_)));
        data.object["arguments"] = std::move(arguments); data.object["cwd"] = string_value(process.cwd);
        data.object["resolved_executable"] =
            string_value(redact_secrets(process.resolved_executable, secrets_));
        data.object["exit_status"] = number_value(process.exit_status);
        data.object["signal"] = number_value(process.signal);
        data.object["duration_ms"] = number_value(process.duration_ms);
        data.object["stdout"] = string_value(redact_secrets(process.stdout_text, secrets_));
        data.object["stderr"] = string_value(redact_secrets(process.stderr_text, secrets_));
        data.object["stdout_truncated"] = bool_value(process.stdout_truncated);
        data.object["stderr_truncated"] = bool_value(process.stderr_truncated);
        data.object["policy"] = string_value(process.policy);
        json::Value guard = object_value();
        const std::string guard_decision =
            !process.guard_decision.empty()
                ? process.guard_decision
                : (error.ok() ? std::string("allow") : std::string("deny"));
        guard.object["decision"] = string_value(guard_decision);
        if (!process.guard_rule_id.empty())
            guard.object["rule_id"] = string_value(process.guard_rule_id);
        else if (!guard_rule_id.empty())
            guard.object["rule_id"] = string_value(guard_rule_id);
        else
            guard.object["rule_id"] = json::Value{};
        data.object["guard"] = std::move(guard);
        std::vector<std::string> warnings;
        if (output_filtered) warnings.push_back("output referring to non-indexed paths was omitted");
        if (!error.ok()) {
            const std::string code =
                error.message.find("refusing") != std::string::npos ||
                        error.message.find("not allowed") != std::string::npos
                    ? "policy_denied"
                    : error_code_string(error.code);
            return envelope(false, std::move(data), code, error.message, warnings,
                            process.stdout_truncated || process.stderr_truncated);
        }
        return envelope(true, std::move(data), "", "", warnings,
                        process.stdout_truncated || process.stderr_truncated);
    }

    if (name == "edit_file") {
        if (!allow_mutations())
            return tool_error_result("policy_denied", "edit_file is not enabled in this session");
        // Models (esp. under natural-language goals) often nest path inside ops[i]
        // instead of the top-level edit_file.path required by the schema. Promote
        // common top-level fields from ops when missing so the call still works.
        // Weaker tool callers (observed: glm-5.2 via OpenRouter) also omit the ops
        // array and put a single replace_text/replace_range payload at the top
        // level — wrap that into ops when present.
        json::Value edit_args = args;
        if (!edit_args.is_object())
            return tool_error_result("invalid_arguments", "edit_file arguments must be an object");
        json::Value* ops_value = nullptr;
        {
            auto it = edit_args.object.find("ops");
            if (it != edit_args.object.end()) ops_value = &it->second;
        }
        if (ops_value == nullptr || !ops_value->is_array()) {
            static const char* kTopLevelOnly[] = {"path", "create_dirs", "expected_file_hash",
                                                  "ops"};
            json::Value promoted_op = object_value();
            for (const auto& entry : edit_args.object) {
                bool top_only = false;
                for (const char* key : kTopLevelOnly) {
                    if (entry.first == key) {
                        top_only = true;
                        break;
                    }
                }
                if (!top_only) promoted_op.object[entry.first] = entry.second;
            }
            const json::Value shaped = normalize_edit_op_shape(promoted_op);
            if (!infer_edit_op_type(shaped).empty() ||
                shaped.get("old_text") != nullptr || shaped.get("new_text") != nullptr ||
                shaped.get("replacement") != nullptr || shaped.get("start_line") != nullptr ||
                shaped.get("line") != nullptr) {
                json::Value ops = array_value();
                ops.array.push_back(shaped);
                edit_args.object["ops"] = std::move(ops);
                ops_value = &edit_args.object["ops"];
            }
        }
        if (ops_value == nullptr || !ops_value->is_array())
            return tool_error_result("invalid_arguments", "edit_file requires ops array");

        auto top_string = [&](const char* key) -> std::string {
            const json::Value* value = edit_args.get(key);
            return value != nullptr && value->is_string() ? value->string : std::string{};
        };
        if (top_string("path").empty()) {
            for (json::Value& op : ops_value->array) {
                if (!op.is_object()) continue;
                // Flat op.path or nested op.replace_range.path etc.
                const json::Value* nested_path = op.get("path");
                if (nested_path == nullptr || !nested_path->is_string() || nested_path->string.empty()) {
                    for (const char* nested_key :
                         {"replace_range", "insert_at", "delete_range", "replace_text",
                          "replace_symbol", "create_file"}) {
                        const json::Value* nested = op.get(nested_key);
                        if (nested != nullptr && nested->is_object()) {
                            nested_path = nested->get("path");
                            if (nested_path != nullptr && nested_path->is_string() &&
                                !nested_path->string.empty())
                                break;
                            nested_path = nullptr;
                        }
                    }
                }
                if (nested_path != nullptr && nested_path->is_string() &&
                    !nested_path->string.empty()) {
                    edit_args.object["path"] = string_value(nested_path->string);
                    break;
                }
            }
        }
        if (top_string("expected_file_hash").empty()) {
            for (const json::Value& op : ops_value->array) {
                if (!op.is_object()) continue;
                const json::Value* hash = op.get("expected_file_hash");
                if (hash != nullptr && hash->is_string() && !hash->string.empty()) {
                    edit_args.object["expected_file_hash"] = *hash;
                    break;
                }
            }
        }

        std::string path, expected_hash;
        bool create_dirs = false;
        if (!get_string(edit_args, "path", path, true, validation_error) ||
            !get_bool(edit_args, "create_dirs", false, create_dirs, validation_error) ||
            !get_string(edit_args, "expected_file_hash", expected_hash, false, validation_error))
            return tool_error_result("invalid_arguments", validation_error);
        std::string relative_path;
        Error normalize_error = normalize_mutation_path(path, relative_path);
        bool external = false;
        fs::path external_path;
        if (!normalize_error.ok()) {
            Error external_error = resolve_external_file_path(
                snapshot_.workspace, path, true, external_path);
            if (external_error.ok()) {
                if (mutation_policy_ != MutationPolicy::Full)
                    return tool_error_result(
                        "policy_denied",
                        "Plan mode cannot edit files outside the project");
                for (const json::Value& raw_op : ops_value->array) {
                    const json::Value op = normalize_edit_op_shape(raw_op);
                    if (infer_edit_op_type(op) == "replace_symbol")
                        return tool_error_result(
                            "policy_denied",
                            "replace_symbol is index-dependent and unavailable outside the "
                            "project; use replace_range or replace_text");
                }
                external = true;
                path = external_path.generic_u8string();
            } else {
                return tool_error_result("policy_denied", normalize_error.message);
            }
        } else {
            path = std::move(relative_path);
        }
        const json::Value& ops = *ops_value;
        std::string history_path, old_hash, new_hash;
        std::size_t operations_applied = 0;
        std::vector<std::string> summary;
        std::vector<std::string> warnings;
        const GuardApprovalDecision permission = request_permission(
            "edit_file", "edit_file " + path, {path}, external,
            external && resolved_path_is_under_system_temp(external_path), true, false,
            external ? "ask_on_external_file_write" : std::string(),
            external
                ? "Edit this validated UTF-8 file outside the active project? The edit has no "
                      "project history backup or index entry."
                : std::string(),
            cancellation);
        const Error error =
            permission == GuardApprovalDecision::Allow
                ? edit_workspace_file(path, expected_hash, ops, create_dirs,
                                      history_path, old_hash, new_hash,
                                      operations_applied, summary, warnings)
                : Error{permission == GuardApprovalDecision::Cancelled
                            ? ErrorCode::Cancelled
                            : ErrorCode::UnsupportedFeature,
                        permission == GuardApprovalDecision::Cancelled
                            ? "edit_file approval cancelled"
                            : "edit_file requires user approval"};
        json::Value data = object_value();
        data.object["path"] = string_value(fs::u8path(path).generic_u8string());
        data.object["applied"] = bool_value(error.ok());
        data.object["operations_applied"] = number_value(static_cast<double>(operations_applied));
        data.object["old_file_hash"] = string_value(old_hash);
        data.object["new_file_hash"] = string_value(new_hash);
        data.object["history_path"] = string_value(history_path);
        data.object["indexed_snapshot_updated"] = bool_value(error.ok() && !external);
        data.object["external"] = bool_value(external);
        json::Value summary_array = array_value();
        for (const std::string& item : summary) summary_array.array.push_back(string_value(item));
        data.object["summary"] = std::move(summary_array);
        if (!error.ok()) {
            const std::string code =
                error.message.find("stale_file") != std::string::npos    ? "stale_file"
                : error.message.find("stale_range") != std::string::npos ? "stale_range"
                : error.message.find("not found") != std::string::npos   ? "not_found"
                : error.message.find("matches ") != std::string::npos    ? "ambiguous_match"
                : error.code == ErrorCode::BadArgs                       ? "invalid_arguments"
                : error.code == ErrorCode::UnsupportedFeature            ? "policy_denied"
                                                                         : error_code_string(error.code);
            return envelope(false, std::move(data), code, error.message, warnings, false);
        }
        if (external)
            warnings.push_back(
                "outside-project edit; no project history backup or index update was created");
        return envelope(true, std::move(data), "", "", warnings, false);
    }

    if (name == "write_file") {
        if (!allow_mutations())
            return tool_error_result("policy_denied", "write_file is not enabled in this session");
        std::string path, content, mode, expected_hash;
        bool create_dirs = false;
        if (!get_string(args, "path", path, true, validation_error) ||
            !get_bool(args, "create_dirs", false, create_dirs, validation_error) ||
            !get_string(args, "mode", mode, false, validation_error) ||
            !get_string(args, "expected_file_hash", expected_hash, false, validation_error))
            return tool_error_result("invalid_arguments", validation_error);
        // content may be empty for intentionally blank files; only require the key.
        if (args.get("content") == nullptr || !args.get("content")->is_string())
            return tool_error_result("invalid_arguments", "missing required string argument: content");
        content = args.get("content")->string;
        std::string history_path, old_hash, new_hash;
        bool created = false;
        bool external = false;
        fs::path external_path;
        std::string relative_path;
        Error error = normalize_mutation_path(path, relative_path);
        if (error.ok()) {
            path = std::move(relative_path);
            const GuardApprovalDecision decision = request_permission(
                "write_file",
                "write_file " + path + " (" + std::to_string(content.size()) + " bytes)",
                {path, mode, create_dirs ? "create_dirs=true" : "create_dirs=false"},
                false, false, true, false, {}, {}, cancellation);
            if (decision == GuardApprovalDecision::Allow)
                error = write_workspace_file(path, content, create_dirs, mode, expected_hash,
                                             history_path, created, old_hash, new_hash);
            else
                error = {decision == GuardApprovalDecision::Cancelled
                             ? ErrorCode::Cancelled
                             : ErrorCode::UnsupportedFeature,
                         decision == GuardApprovalDecision::Cancelled
                             ? "write_file approval cancelled"
                             : "write_file requires user approval"};
        } else if (mutation_policy_ == MutationPolicy::Full) {
            error = resolve_external_file_path(snapshot_.workspace, path, false, external_path);
            if (error.ok()) {
                external = true;
                const GuardApprovalDecision decision =
                    request_permission(
                        "write_file",
                        "write_file " + external_path.generic_u8string() + " (" +
                            std::to_string(content.size()) + " bytes)",
                        {external_path.generic_u8string(), mode,
                         create_dirs ? "create_dirs=true" : "create_dirs=false"},
                        true, resolved_path_is_under_system_temp(external_path), true, false,
                        "ask_on_external_file_write",
                        "Write this file outside the active project?\nExact resolved path: " +
                            external_path.generic_u8string() +
                            "\nApproval applies only to this tool call. The write has no project "
                            "history backup and will not update the project index." +
                            (create_dirs
                                 ? std::string("\nMissing parent directories may be created.")
                                 : std::string()),
                        cancellation);
                if (decision == GuardApprovalDecision::Allow) {
                    if (cancellation.cancelled())
                        error = {ErrorCode::Cancelled,
                                 "external file write cancelled after approval"};
                    else
                        error = write_external_file(external_path, content, create_dirs, mode,
                                                    expected_hash, created, old_hash, new_hash);
                } else {
                    std::string message =
                        "refusing to write file outside the project directory without user "
                        "approval: " +
                        external_path.generic_u8string();
                    if (decision == GuardApprovalDecision::Cancelled)
                        message += " (approval cancelled)";
                    else if (!on_guard_ask_)
                        message += " (headless agent denies external file access)";
                    else
                        message += " (user selected No)";
                    error = {decision == GuardApprovalDecision::Cancelled
                                 ? ErrorCode::Cancelled
                                 : ErrorCode::UnsupportedFeature,
                             message};
                }
            }
        }
        json::Value data = object_value();
        data.object["path"] =
            string_value(external ? external_path.generic_u8string()
                                  : fs::u8path(path).generic_u8string());
        data.object["bytes_written"] = number_value(static_cast<double>(content.size()));
        data.object["created"] = bool_value(created);
        data.object["old_file_hash"] = string_value(old_hash);
        data.object["new_file_hash"] = string_value(new_hash);
        data.object["history_path"] = string_value(history_path);
        data.object["indexed_snapshot_updated"] = bool_value(error.ok() && !external);
        data.object["external"] = bool_value(external);
        json::Value guard = object_value();
        guard.object["decision"] = string_value(error.ok() ? "allow" : "deny");
        data.object["guard"] = std::move(guard);
        if (!error.ok()) {
            const std::string code =
                error.message.find("stale_file") != std::string::npos ? "stale_file"
                : error.code == ErrorCode::BadArgs                   ? "invalid_arguments"
                : error.code == ErrorCode::UnsupportedFeature        ? "policy_denied"
                                                                     : error_code_string(error.code);
            return envelope(false, std::move(data), code, error.message, {}, false);
        }
        std::vector<std::string> warnings;
        if (external)
            warnings.push_back(
                "outside-project write explicitly approved; no project history backup or index "
                "update was created");
        return envelope(true, std::move(data), "", "", warnings, false);
    }

    if (name == "create_directory") {
        if (!allow_mutations())
            return tool_error_result("policy_denied",
                                     "create_directory is not enabled in this session");
        std::string path;
        bool parents = false;
        if (!get_string(args, "path", path, true, validation_error) ||
            !get_bool(args, "parents", false, parents, validation_error))
            return tool_error_result("invalid_arguments", validation_error);
        fs::path target;
        bool external = false;
        Error error =
            resolve_native_path(snapshot_.workspace, path, false, target, external);
        if (error.ok() && external && mutation_policy_ != MutationPolicy::Full)
            error = {ErrorCode::UnsupportedFeature,
                     "Plan mode cannot create directories outside the project"};
        if (error.ok() && !external) {
            std::error_code ec;
            const fs::path root = fs::canonical(snapshot_.workspace, ec);
            const std::string relative = target.lexically_relative(root).generic_u8string();
            error = validate_mutation_path(relative, true, false);
        }
        const GuardApprovalDecision decision =
            error.ok()
                ? request_permission(
                      "create_directory", "create_directory " + target.generic_u8string(),
                      {target.generic_u8string(), parents ? "parents=true" : "parents=false"},
                      external, external && resolved_path_is_under_system_temp(target), true,
                      false, {}, {}, cancellation)
                : GuardApprovalDecision::Deny;
        if (error.ok() && decision != GuardApprovalDecision::Allow)
            error = {decision == GuardApprovalDecision::Cancelled
                         ? ErrorCode::Cancelled
                         : ErrorCode::UnsupportedFeature,
                     decision == GuardApprovalDecision::Cancelled
                         ? "create_directory approval cancelled"
                         : "create_directory requires user approval"};
        if (error.ok()) {
            fs::path current_target;
            bool current_external = false;
            Error stable = resolve_native_path(snapshot_.workspace, path, false,
                                               current_target, current_external);
            if (!stable.ok())
                error = stable;
            else if (current_target != target || current_external != external)
                error = {ErrorCode::FileWrite,
                         "directory target changed while awaiting approval"};
        }
        if (error.ok()) {
            std::error_code ec;
            if (fs::exists(target, ec))
                error = {ErrorCode::FileWrite,
                         "create_directory destination already exists: " +
                             target.generic_u8string()};
            else {
                const bool created =
                    parents ? fs::create_directories(target, ec)
                            : fs::create_directory(target, ec);
                if (ec || !created)
                    error = {ErrorCode::FileWrite,
                             "could not create directory " + target.generic_u8string() +
                                 (ec ? ": " + ec.message() : std::string())};
            }
        }
        json::Value data = object_value();
        data.object["path"] = string_value(target.generic_u8string());
        data.object["created"] = bool_value(error.ok());
        data.object["external"] = bool_value(external);
        return error.ok()
                   ? envelope(true, std::move(data), "", "", {}, false)
                   : envelope(false, std::move(data),
                              error.code == ErrorCode::UnsupportedFeature
                                  ? "policy_denied"
                                  : error_code_string(error.code),
                              error.message, {}, false);
    }

    if (name == "rename_path") {
        if (mutation_policy_ != MutationPolicy::Full)
            return tool_error_result("policy_denied",
                                     "rename_path is enabled only in Act mode");
        std::string source_text, destination_text;
        if (!get_string(args, "source", source_text, true, validation_error) ||
            !get_string(args, "destination", destination_text, true, validation_error))
            return tool_error_result("invalid_arguments", validation_error);
        fs::path source, destination;
        bool source_external = false, destination_external = false;
        Error error = resolve_native_path(snapshot_.workspace, source_text, true, source,
                                          source_external);
        if (error.ok())
            error = resolve_native_path(snapshot_.workspace, destination_text, false,
                                        destination, destination_external);
        const bool external = source_external || destination_external;
        const bool under_temp =
            (!source_external || resolved_path_is_under_system_temp(source)) &&
            (!destination_external || resolved_path_is_under_system_temp(destination));
        const GuardApprovalDecision decision =
            error.ok()
                ? request_permission(
                      "rename_path",
                      "rename_path " + source.generic_u8string() + " -> " +
                          destination.generic_u8string(),
                      {source.generic_u8string(), destination.generic_u8string()}, external,
                      under_temp, true, false, {}, {}, cancellation)
                : GuardApprovalDecision::Deny;
        if (error.ok() && decision != GuardApprovalDecision::Allow)
            error = {decision == GuardApprovalDecision::Cancelled
                         ? ErrorCode::Cancelled
                         : ErrorCode::UnsupportedFeature,
                     decision == GuardApprovalDecision::Cancelled
                         ? "rename_path approval cancelled"
                         : "rename_path requires user approval"};
        if (error.ok()) {
            Error stable = ensure_approved_external_path_unchanged(
                source, "rename", ErrorCode::FileWrite);
            if (!stable.ok()) error = stable;
            fs::path current_destination;
            bool current_external = false;
            if (error.ok()) {
                Error destination_stable = resolve_native_path(
                    snapshot_.workspace, destination_text, false,
                    current_destination, current_external);
                if (!destination_stable.ok())
                    error = destination_stable;
                else if (current_destination != destination ||
                         current_external != destination_external)
                    error = {ErrorCode::FileWrite,
                             "rename destination changed while awaiting approval"};
            }
        }
        if (error.ok()) {
            std::error_code ec;
            if (fs::exists(destination, ec))
                error = {ErrorCode::FileWrite,
                         "rename_path destination already exists: " +
                             destination.generic_u8string()};
            else {
                fs::rename(source, destination, ec);
                if (ec)
                    error = {ErrorCode::FileWrite,
                             "could not rename " + source.generic_u8string() + " to " +
                                 destination.generic_u8string() + ": " + ec.message() +
                                 " (cross-filesystem renames are not copied)"};
            }
        }
        if (error.ok()) {
            std::error_code ec;
            const fs::path root = fs::canonical(snapshot_.workspace, ec);
            if (!source_external)
                note_removed_path(source.lexically_relative(root).generic_u8string());
            if (!destination_external && fs::is_regular_file(destination, ec)) {
                Error read_error;
                const std::string content = read_all_bytes(destination, read_error);
                if (read_error.ok())
                    note_written_file(
                        destination.lexically_relative(root).generic_u8string(), content);
            } else if (!destination_external &&
                       fs::is_directory(destination, ec)) {
                queue_index_paths({}, true);
            }
        }
        json::Value data = object_value();
        data.object["source"] = string_value(source.generic_u8string());
        data.object["destination"] = string_value(destination.generic_u8string());
        data.object["renamed"] = bool_value(error.ok());
        return error.ok()
                   ? envelope(true, std::move(data), "", "", {}, false)
                   : envelope(false, std::move(data),
                              error.code == ErrorCode::UnsupportedFeature
                                  ? "policy_denied"
                                  : error_code_string(error.code),
                              error.message, {}, false);
    }

    if (name == "str_replace") {
        if (!allow_mutations())
            return tool_error_result("policy_denied", "str_replace is not enabled in this session");
        std::string path, old_text, new_text, expected_hash;
        bool replace_all = false;
        bool allow_fuzzy = true;
        if (!get_string(args, "path", path, true, validation_error) ||
            !get_string(args, "old_text", old_text, true, validation_error) ||
            !get_string(args, "new_text", new_text, false, validation_error) ||
            !get_bool(args, "replace_all", false, replace_all, validation_error) ||
            !get_bool(args, "fuzzy", true, allow_fuzzy, validation_error) ||
            !get_string(args, "expected_file_hash", expected_hash, false, validation_error))
            return tool_error_result("invalid_arguments", validation_error);
        std::string relative_path;
        Error normalize_error = normalize_mutation_path(path, relative_path);
        // new_text may be empty (delete match); require the key explicitly.
        if (args.get("new_text") == nullptr || !args.get("new_text")->is_string())
            return tool_error_result("invalid_arguments", "missing required string argument: new_text");
        new_text = args.get("new_text")->string;
        std::size_t hint_start = 0;
        std::size_t hint_end = 0;
        const json::Value* hint = args.get("line_range_hint");
        if (hint != nullptr) {
            if (!hint->is_object())
                return tool_error_result("invalid_arguments", "line_range_hint must be an object");
            if (!get_size(*hint, "start_line", 0, 100000000, hint_start, validation_error) ||
                !get_size(*hint, "end_line", 0, 100000000, hint_end, validation_error))
                return tool_error_result("invalid_arguments", validation_error);
        }
        std::string history_path, old_hash, new_hash, match_mode;
        std::size_t matches_found = 0;
        std::size_t replacements_made = 0;
        std::vector<std::string> candidate_lines;
        bool external = false;
        fs::path external_path;
        Error error;
        if (normalize_error.ok()) {
            path = std::move(relative_path);
            const GuardApprovalDecision decision = request_permission(
                "str_replace", "str_replace " + path, {path}, false, false, true,
                false, {}, {}, cancellation);
            if (decision == GuardApprovalDecision::Allow)
                error = str_replace_workspace_file(
                    path, old_text, new_text, replace_all, allow_fuzzy, hint_start,
                    hint_end, expected_hash, history_path, matches_found,
                    replacements_made, match_mode, old_hash, new_hash,
                    candidate_lines);
            else
                error = {decision == GuardApprovalDecision::Cancelled
                             ? ErrorCode::Cancelled
                             : ErrorCode::UnsupportedFeature,
                         decision == GuardApprovalDecision::Cancelled
                             ? "str_replace approval cancelled"
                             : "str_replace requires user approval"};
        } else {
            if (mutation_policy_ != MutationPolicy::Full)
                return tool_error_result(
                    "policy_denied",
                    "Plan mode cannot edit files outside the project");
            error = resolve_external_file_path(snapshot_.workspace, path, true,
                                               external_path);
            external = error.ok();
            std::string previous;
            if (error.ok()) {
                previous = read_all_bytes(external_path, error);
                if (error.ok() &&
                    (previous.find('\0') != std::string::npos ||
                     !html::is_valid_utf8(previous)))
                    error = {ErrorCode::FileRead,
                             "external str_replace requires a UTF-8 text file"};
            }
            old_hash = error.ok() ? index::content_hash(previous) : std::string();
            if (error.ok() && !expected_hash.empty() && expected_hash != old_hash)
                error = {ErrorCode::FileWrite,
                         "stale_file: expected_file_hash does not match current file content"};
            std::size_t region_start = 0, region_end = previous.size();
            if (error.ok())
                error = region_from_line_hint(previous, hint_start, hint_end,
                                              region_start, region_end);
            TextMatchResult found;
            std::vector<TextSpan> chosen;
            if (error.ok()) {
                found = find_text_matches(previous, old_text, allow_fuzzy,
                                          region_start, region_end);
                error = disambiguate_matches(
                    found, replace_all, hint_start, hint_end, chosen,
                    candidate_lines, matches_found);
                if (!error.ok() && matches_found == 0)
                    error = {ErrorCode::FileWrite,
                             "old_text not found in external file: " +
                                 external_path.generic_u8string()};
            }
            std::string updated;
            if (error.ok()) {
                match_mode = found.mode;
                updated = apply_text_replacements(previous, chosen, new_text,
                                                  replace_all, replacements_made);
                if (updated.size() > index_options_.max_source_code_file_size)
                    error = {ErrorCode::BadArgs,
                             "str_replace result exceeds max_source_code_file_size"};
            }
            if (error.ok()) {
                const GuardApprovalDecision decision = request_permission(
                    "str_replace",
                    "str_replace " + external_path.generic_u8string(),
                    {external_path.generic_u8string()}, true,
                    resolved_path_is_under_system_temp(external_path), true, false,
                    "ask_on_external_file_write",
                    "Edit this validated UTF-8 file outside the active project? The edit has "
                    "no project history backup or index entry.",
                    cancellation);
                if (decision != GuardApprovalDecision::Allow)
                    error = {decision == GuardApprovalDecision::Cancelled
                                 ? ErrorCode::Cancelled
                                 : ErrorCode::UnsupportedFeature,
                             decision == GuardApprovalDecision::Cancelled
                                 ? "str_replace approval cancelled"
                                 : "external str_replace requires user approval"};
            }
            if (error.ok()) {
                Error stable = ensure_approved_external_path_unchanged(
                    external_path, "edit", ErrorCode::FileWrite);
                if (!stable.ok())
                    error = stable;
                else {
                    Error reread_error;
                    const std::string current =
                        read_all_bytes(external_path, reread_error);
                    if (!reread_error.ok())
                        error = reread_error;
                    else if (index::content_hash(current) != old_hash)
                        error = {ErrorCode::FileWrite,
                                 "stale_file: external file changed while awaiting approval"};
                }
            }
            if (error.ok()) {
                error = write_bytes_atomic(external_path, updated);
                if (error.ok()) new_hash = index::content_hash(updated);
            }
        }
        json::Value data = object_value();
        data.object["path"] =
            string_value(external ? external_path.generic_u8string()
                                  : fs::u8path(path).generic_u8string());
        data.object["matches_found"] = number_value(static_cast<double>(matches_found));
        data.object["replacements_made"] = number_value(static_cast<double>(replacements_made));
        data.object["match_mode"] = string_value(match_mode.empty() ? "exact" : match_mode);
        data.object["old_file_hash"] = string_value(old_hash);
        data.object["new_file_hash"] = string_value(new_hash);
        data.object["history_path"] = string_value(history_path);
        data.object["indexed_snapshot_updated"] = bool_value(error.ok() && !external);
        data.object["external"] = bool_value(external);
        if (!candidate_lines.empty()) {
            json::Value candidates = array_value();
            for (const std::string& line : candidate_lines)
                candidates.array.push_back(string_value(line));
            data.object["candidate_lines"] = std::move(candidates);
        }
        if (!error.ok()) {
            const std::string code =
                error.message.find("stale_file") != std::string::npos ? "stale_file"
                : error.message.find("not found") != std::string::npos ? "not_found"
                : error.message.find("matches ") != std::string::npos  ? "ambiguous_match"
                : error.code == ErrorCode::BadArgs                     ? "invalid_arguments"
                : error.code == ErrorCode::UnsupportedFeature          ? "policy_denied"
                                                                       : error_code_string(error.code);
            return envelope(false, std::move(data), code, error.message, {}, false);
        }
        std::vector<std::string> success_warnings;
        if (external)
            success_warnings.push_back(
                "outside-project edit; no project history backup or index update was created");
        return envelope(true, std::move(data), "", "", success_warnings, false);
    }

    if (name == "remove") {
        if (mutation_policy_ != MutationPolicy::Full)
            return tool_error_result("policy_denied", "remove is not enabled in this session");
        std::string path, expected_hash;
        bool recursive = false;
        bool confirm = false;
        if (!get_string(args, "path", path, true, validation_error) ||
            !get_bool(args, "recursive", false, recursive, validation_error) ||
            !get_bool(args, "confirm", false, confirm, validation_error) ||
            !get_string(args, "expected_file_hash", expected_hash, false, validation_error))
            return tool_error_result("invalid_arguments", validation_error);
        std::string relative_path;
        Error normalize_error = normalize_mutation_path(path, relative_path);
        std::string history_path, old_hash, guard_decision, guard_rule_id;
        bool was_directory = false;
        std::vector<std::string> suggestions;
        std::vector<std::string> warnings;
        bool external = false;
        fs::path external_path;
        Error error;
        if (normalize_error.ok()) {
            path = std::move(relative_path);
            error = remove_workspace_path(path, recursive, confirm, expected_hash,
                                          history_path, was_directory, guard_decision,
                                          guard_rule_id, old_hash, suggestions, warnings);
        } else {
            error = resolve_native_path(snapshot_.workspace, path, true, external_path,
                                        external);
            if (error.ok() && !external)
                error = {ErrorCode::BadArgs, normalize_error.message};
            std::error_code ec;
            if (error.ok() && is_broad_removal_root(snapshot_.workspace, external_path))
                error = {ErrorCode::BadArgs,
                         "refusing to remove a filesystem, home, temp, workspace root, "
                         "or workspace ancestor: " +
                             external_path.generic_u8string()};
            const fs::file_status status =
                error.ok() ? fs::symlink_status(external_path, ec) : fs::file_status{};
            if (error.ok() && (ec || fs::is_symlink(status)))
                error = {ErrorCode::FileWrite,
                         "refusing to remove symlink path: " +
                             external_path.generic_u8string()};
            was_directory = error.ok() && fs::is_directory(status);
            if (error.ok() && was_directory && !recursive &&
                !fs::is_empty(external_path, ec))
                error = {ErrorCode::FileWrite,
                         "directory is not empty; pass recursive=true to remove it"};
            bool tree_has_database = is_database_path(external_path.generic_u8string());
            if (error.ok() && was_directory && recursive) {
                for (fs::recursive_directory_iterator it(external_path, ec), end;
                     !ec && it != end; it.increment(ec)) {
                    if (fs::is_symlink(it->symlink_status(ec))) {
                        error = {ErrorCode::FileWrite,
                                 "refusing recursive removal of a tree containing symlink: " +
                                     it->path().generic_u8string()};
                        break;
                    }
                    tree_has_database =
                        tree_has_database ||
                        is_database_path(it->path().generic_u8string());
                }
                if (ec && error.ok())
                    error = {ErrorCode::FileWrite,
                             "could not validate recursive removal tree: " + ec.message()};
            }
            if (error.ok()) {
                const bool destructive = recursive || tree_has_database;
                const GuardApprovalDecision decision = request_permission(
                    "remove",
                    std::string(recursive ? "remove -r " : "remove ") +
                        external_path.generic_u8string(),
                    {external_path.generic_u8string(),
                     recursive ? "recursive=true" : "recursive=false"},
                    true, resolved_path_is_under_system_temp(external_path), true,
                    destructive,
                    tree_has_database ? "ask_on_database_delete"
                                      : (recursive ? "ask_on_recursive_delete"
                                                   : "ask_on_external_remove"),
                    "Delete this validated path outside the active project? One approval covers "
                    "the external and destructive aspects of this call.",
                    cancellation);
                if (decision != GuardApprovalDecision::Allow)
                    error = {decision == GuardApprovalDecision::Cancelled
                                 ? ErrorCode::Cancelled
                                 : ErrorCode::UnsupportedFeature,
                             decision == GuardApprovalDecision::Cancelled
                                 ? "remove approval cancelled"
                                 : "external remove requires user approval"};
            }
            if (error.ok()) {
                Error stable = ensure_approved_external_path_unchanged(
                    external_path, "remove", ErrorCode::FileWrite);
                if (!stable.ok()) error = stable;
            }
            if (error.ok()) {
                if (!was_directory && !expected_hash.empty()) {
                    Error read_error;
                    const std::string previous =
                        read_all_bytes(external_path, read_error);
                    if (!read_error.ok())
                        error = read_error;
                    else if (index::content_hash(previous) != expected_hash)
                        error = {ErrorCode::FileWrite,
                                 "stale_file: expected_file_hash does not match current file "
                                 "content"};
                    else
                        old_hash = index::content_hash(previous);
                } else if (was_directory && !expected_hash.empty()) {
                    error = {ErrorCode::BadArgs,
                             "expected_file_hash is only valid for file removals"};
                }
            }
            if (error.ok()) {
                if (was_directory && recursive)
                    fs::remove_all(external_path, ec);
                else
                    fs::remove(external_path, ec);
                if (ec)
                    error = {ErrorCode::FileWrite,
                             "could not remove external path " +
                                 external_path.generic_u8string() + ": " + ec.message()};
                else
                    guard_decision = "allow";
            }
        }
        json::Value data = object_value();
        data.object["path"] =
            string_value(external ? external_path.generic_u8string()
                                  : fs::u8path(path).generic_u8string());
        data.object["removed"] = bool_value(error.ok());
        data.object["was_directory"] = bool_value(was_directory);
        data.object["history_path"] = string_value(history_path);
        data.object["old_file_hash"] = string_value(old_hash);
        data.object["index_updated"] = bool_value(error.ok() && !external);
        data.object["external"] = bool_value(external);
        if (!suggestions.empty()) {
            json::Value list = array_value();
            for (const std::string& item : suggestions) list.array.push_back(string_value(item));
            data.object["suggestions"] = std::move(list);
        }
        json::Value guard = object_value();
        guard.object["decision"] = string_value(guard_decision);
        if (!guard_rule_id.empty()) guard.object["rule_id"] = string_value(guard_rule_id);
        else guard.object["rule_id"] = json::Value{};
        data.object["guard"] = std::move(guard);
        if (!error.ok()) {
            const std::string code =
                error.message.find("stale_file") != std::string::npos       ? "stale_file"
                : error.message.find("does not exist") != std::string::npos ? "not_found"
                : error.message.find("ambiguous") != std::string::npos      ? "ambiguous_match"
                : error.message.find("refusing") != std::string::npos       ? "policy_denied"
                : error.code == ErrorCode::BadArgs                          ? "invalid_arguments"
                : error.code == ErrorCode::UnsupportedFeature               ? "policy_denied"
                                                                            : error_code_string(error.code);
            return envelope(false, std::move(data), code, error.message, warnings, false);
        }
        return envelope(true, std::move(data), "", "", warnings, false);
    }

    if (name == "apply_patch") {
        if (!allow_mutations())
            return tool_error_result("policy_denied", "apply_patch is not enabled in this session");
        // Accept patch / input / diff aliases (OpenAI tool variants).
        std::string patch_text;
        if (!get_string(args, "patch", patch_text, false, validation_error))
            return tool_error_result("invalid_arguments", validation_error);
        if (patch_text.empty()) {
            if (!get_string(args, "input", patch_text, false, validation_error))
                return tool_error_result("invalid_arguments", validation_error);
        }
        if (patch_text.empty()) {
            if (!get_string(args, "diff", patch_text, false, validation_error))
                return tool_error_result("invalid_arguments", validation_error);
        }
        if (patch_text.empty())
            return tool_error_result("invalid_arguments",
                                    "apply_patch requires patch (or input/diff) string");
        bool atomic = true;
        bool allow_fuzzy = true;
        if (!get_bool(args, "atomic", true, atomic, validation_error) ||
            !get_bool(args, "fuzzy", true, allow_fuzzy, validation_error))
            return tool_error_result("invalid_arguments", validation_error);

        std::vector<std::string> files_changed;
        std::size_t operations_applied = 0;
        std::map<std::string, std::string> new_hashes;
        std::string reverse_patch_path;
        std::vector<std::string> summary;
        std::vector<std::string> warnings;
        const Error error =
            apply_workspace_patch(patch_text, atomic, allow_fuzzy, files_changed, operations_applied,
                                  new_hashes, reverse_patch_path, summary, warnings);
        json::Value data = object_value();
        data.object["applied"] = bool_value(error.ok());
        data.object["operations_applied"] = number_value(static_cast<double>(operations_applied));
        data.object["reverse_patch_path"] = string_value(reverse_patch_path);
        data.object["index_updated"] = bool_value(error.ok());
        json::Value files = array_value();
        for (const std::string& file : files_changed) files.array.push_back(string_value(file));
        data.object["files_changed"] = std::move(files);
        json::Value hashes = object_value();
        for (const auto& entry : new_hashes)
            hashes.object[entry.first] = string_value(entry.second);
        data.object["new_hashes"] = std::move(hashes);
        json::Value summary_array = array_value();
        for (const std::string& item : summary) summary_array.array.push_back(string_value(item));
        data.object["summary"] = std::move(summary_array);
        if (!error.ok()) {
            const std::string code =
                error.message.find("not found") != std::string::npos     ? "not_found"
                : error.message.find("matches ") != std::string::npos    ? "ambiguous_match"
                : error.message.find("refusing") != std::string::npos    ? "policy_denied"
                : error.message.find("does not exist") != std::string::npos ? "not_found"
                : error.code == ErrorCode::BadArgs                       ? "invalid_arguments"
                : error.code == ErrorCode::UnsupportedFeature            ? "policy_denied"
                                                                         : error_code_string(error.code);
            return envelope(false, std::move(data), code, error.message, warnings, false);
        }
        return envelope(true, std::move(data), "", "", warnings, false);
    }

    if (name == "git_status") {
        bool short_form = true;
        bool include_branch = true;
        if (!get_bool(args, "short", true, short_form, validation_error) ||
            !get_bool(args, "include_branch", true, include_branch, validation_error))
            return tool_error_result("invalid_arguments", validation_error);
        std::string command = "git status";
        if (short_form) command += " --short";
        if (include_branch) command += " --branch";
        ProcessOptions options;
        options.workspace = snapshot_.workspace;
        options.timeout_ms = allow_mutations() ? 30000 : 10000;
        options.cancellation = cancellation;
        options.stdout_limit = 65536;
        options.stderr_limit = 16384;
        ProcessResult process;
        const CommandPolicy policy =
            allow_mutations() ? CommandPolicy::Agent : CommandPolicy::InspectionOnly;
        const Error error = run_command(command, options, process, policy);
        json::Value data = object_value();
        data.object["command"] = string_value(command);
        data.object["cwd"] = string_value(process.cwd.empty() ? snapshot_.workspace : process.cwd);
        data.object["exit_status"] = number_value(process.exit_status);
        data.object["stdout"] = string_value(redact_secrets(process.stdout_text, secrets_));
        data.object["stderr"] = string_value(redact_secrets(process.stderr_text, secrets_));
        data.object["stdout_truncated"] = bool_value(process.stdout_truncated);
        data.object["stderr_truncated"] = bool_value(process.stderr_truncated);
        data.object["duration_ms"] = number_value(static_cast<double>(process.duration_ms));
        if (!error.ok()) {
            const std::string code =
                error.message.find("not available") != std::string::npos ? "unavailable"
                : error.code == ErrorCode::BadArgs                       ? "policy_denied"
                                                                         : error_code_string(error.code);
            return envelope(false, std::move(data), code, error.message, {},
                            process.stdout_truncated || process.stderr_truncated);
        }
        return envelope(true, std::move(data), "", "", {},
                        process.stdout_truncated || process.stderr_truncated);
    }

    if (name == "git_diff") {
        std::string path;
        bool cached = false;
        bool stat_only = false;
        std::size_t max_bytes = 65536;
        if (!get_string(args, "path", path, false, validation_error) ||
            !get_bool(args, "cached", false, cached, validation_error) ||
            !get_bool(args, "stat", false, stat_only, validation_error) ||
            !get_size(args, "max_bytes", 65536, 524288, max_bytes, validation_error) ||
            max_bytes == 0)
            return tool_error_result("invalid_arguments",
                                    validation_error.empty() ? "max_bytes must be positive"
                                                             : validation_error);
        if (!path.empty() && !safe_relative_path(path))
            return tool_error_result("policy_denied", unsafe_path_message(path, "diff"));
        std::string command = "git diff --no-color --no-ext-diff";
        if (cached) command += " --cached";
        if (stat_only) command += " --stat";
        if (!path.empty()) command += " -- " + path;
        ProcessOptions options;
        options.workspace = snapshot_.workspace;
        options.timeout_ms = allow_mutations() ? 30000 : 10000;
        options.cancellation = cancellation;
        options.stdout_limit = max_bytes;
        options.stderr_limit = 16384;
        ProcessResult process;
        const CommandPolicy policy =
            allow_mutations() ? CommandPolicy::Agent : CommandPolicy::InspectionOnly;
        const Error error = run_command(command, options, process, policy);
        json::Value data = object_value();
        data.object["command"] = string_value(command);
        data.object["cwd"] = string_value(process.cwd.empty() ? snapshot_.workspace : process.cwd);
        data.object["exit_status"] = number_value(process.exit_status);
        data.object["stdout"] = string_value(redact_secrets(process.stdout_text, secrets_));
        data.object["stderr"] = string_value(redact_secrets(process.stderr_text, secrets_));
        data.object["stdout_truncated"] = bool_value(process.stdout_truncated);
        data.object["stderr_truncated"] = bool_value(process.stderr_truncated);
        data.object["duration_ms"] = number_value(static_cast<double>(process.duration_ms));
        data.object["path"] = string_value(path);
        data.object["cached"] = bool_value(cached);
        data.object["stat"] = bool_value(stat_only);
        if (!error.ok()) {
            const std::string code =
                error.message.find("not available") != std::string::npos ? "unavailable"
                : error.code == ErrorCode::BadArgs                       ? "policy_denied"
                                                                         : error_code_string(error.code);
            return envelope(false, std::move(data), code, error.message, {},
                            process.stdout_truncated || process.stderr_truncated);
        }
        return envelope(true, std::move(data), "", "", {},
                        process.stdout_truncated || process.stderr_truncated);
    }

    if (name == "fetch_url") {
        if (!allow_network_)
            return tool_error_result("policy_denied", "fetch_url is not enabled in this session");
        std::string url;
        // max_bytes limits the returned Markdown/text size (what the model sees), not the
        // raw HTML download. Download uses a larger safety ceiling so typical pages convert.
        const std::size_t default_output = static_cast<std::size_t>(
            fetch_options_.max_bytes > 0 ? fetch_options_.max_bytes : 1048576);
        std::size_t max_output_bytes = default_output;
        std::size_t timeout_ms =
            static_cast<std::size_t>(fetch_options_.timeout_seconds > 0
                                         ? fetch_options_.timeout_seconds * 1000
                                         : 30000);
        // extract_text is intentionally not accepted: agent fetch always returns
        // Markdown/plain text, never raw HTML (prompt-injection + token cost).
        if (!get_string(args, "url", url, true, validation_error) ||
            !get_size(args, "max_bytes", max_output_bytes, 8388608, max_output_bytes,
                      validation_error) ||
            !get_size(args, "timeout_ms", timeout_ms, 120000, timeout_ms, validation_error) ||
            max_output_bytes == 0 || timeout_ms == 0)
            return tool_error_result("invalid_arguments",
                                    validation_error.empty() ? "limits must be positive"
                                                             : validation_error);
        fetch::Options opts = fetch_options_;
        // Raw download budget: config default (1 MiB) or enough headroom over the
        // output cap for HTML→MD (HTML is often much larger than the extracted text).
        std::size_t download_cap = default_output;
        if (download_cap < max_output_bytes) download_cap = max_output_bytes;
        const std::size_t headroom = max_output_bytes > (8388608 / 16) ? 8388608 : max_output_bytes * 16;
        if (download_cap < headroom) download_cap = headroom;
        if (download_cap > 8388608) download_cap = 8388608;
        opts.max_bytes = static_cast<long>(download_cap);
        opts.timeout_seconds = static_cast<long>((timeout_ms + 999) / 1000);
        if (opts.timeout_seconds < 1) opts.timeout_seconds = 1;
        opts.follow_redirects = true;
        std::string body;
        // Always convert HTML→Markdown (or keep text/plain). Never fetch_html for agent tools.
        const Error error = fetch::fetch_text(url, opts, body, cancellation);
        json::Value data = object_value();
        data.object["url"] = string_value(url);
        data.object["format"] = string_value("markdown_or_text");
        std::vector<std::string> warnings;
        if (args.get("extract_text") != nullptr)
            warnings.push_back(
                "extract_text is ignored; fetch_url always returns Markdown or plain text");
        bool truncated = false;
        if (error.ok() && body.size() > max_output_bytes) {
            // Truncate at a UTF-8 code-unit boundary so JSON stays well-formed.
            std::size_t cut = max_output_bytes;
            while (cut > 0 &&
                   (static_cast<unsigned char>(body[cut]) & 0xC0) == 0x80)
                --cut;
            body.resize(cut);
            truncated = true;
            warnings.push_back("returned Markdown/text truncated to max_bytes=" +
                               std::to_string(max_output_bytes));
        }
        data.object["bytes_read"] = number_value(static_cast<double>(body.size()));
        data.object["max_output_bytes"] = number_value(static_cast<double>(max_output_bytes));
        if (!error.ok()) {
            data.object["text"] = string_value(redact_secrets(body, secrets_));
            return envelope(false, std::move(data), error_code_string(error.code), error.message,
                            warnings, truncated);
        }
        data.object["text"] = string_value(redact_secrets(body, secrets_));
        return envelope(true, std::move(data), "", "", warnings, truncated);
    }

    if (name == "web_search") {
        if (!allow_network_)
            return tool_error_result("policy_denied", "web_search is not enabled in this session");
        std::string term;
        std::string site;
        // Hard cap at 3 so agents do not pull large SERPs and then fetch every hit.
        constexpr std::size_t kAgentSearchMaxResults = 3;
        std::size_t max_results = kAgentSearchMaxResults;
        if (search_options_.max_results > 0 &&
            static_cast<std::size_t>(search_options_.max_results) < max_results) {
            max_results = static_cast<std::size_t>(search_options_.max_results);
        }
        std::size_t timeout_ms =
            static_cast<std::size_t>(search_options_.timeout_seconds > 0
                                         ? search_options_.timeout_seconds * 1000
                                         : 30000);
        if (!get_string(args, "term", term, true, validation_error) ||
            !get_string(args, "site", site, false, validation_error) ||
            !get_size(args, "max_results", max_results, kAgentSearchMaxResults, max_results,
                      validation_error) ||
            !get_size(args, "timeout_ms", timeout_ms, 120000, timeout_ms, validation_error) ||
            max_results == 0 || timeout_ms == 0)
            return tool_error_result("invalid_arguments",
                                    validation_error.empty() ? "limits must be positive"
                                                             : validation_error);
        std::string query = term;
        if (!site.empty()) query = "site:" + site + " " + term;
        search::Options opts = search_options_;
        opts.max_results = static_cast<int>(max_results);
        opts.timeout_seconds = static_cast<long>((timeout_ms + 999) / 1000);
        if (opts.timeout_seconds < 1) opts.timeout_seconds = 1;
        search::SearchResponse response;
        const Error error = search::search(query, opts, response, cancellation);
        if (!error.ok()) {
            // Map configuration/auth gaps to the planned web_search_unavailable code.
            const bool unavailable =
                error.code == ErrorCode::Auth || error.code == ErrorCode::BadArgs ||
                error.code == ErrorCode::UnsupportedFeature ||
                error.message.find("requires") != std::string::npos ||
                error.message.find("no search") != std::string::npos;
            json::Value data = object_value();
            data.object["term"] = string_value(term);
            data.object["query"] = string_value(query);
            if (!site.empty()) data.object["site"] = string_value(site);
            return envelope(false, std::move(data),
                            unavailable ? "web_search_unavailable" : error_code_string(error.code),
                            error.message, {}, false);
        }
        json::Value data = object_value();
        data.object["term"] = string_value(term);
        data.object["query"] = string_value(query);
        if (!site.empty()) data.object["site"] = string_value(site);
        data.object["provider"] = string_value(response.provider_used);
        json::Value results = array_value();
        for (const search::SearchResult& item : response.results) {
            json::Value entry = object_value();
            entry.object["title"] = string_value(redact_secrets(item.title, secrets_));
            entry.object["url"] = string_value(item.url);
            entry.object["snippet"] = string_value(redact_secrets(item.snippet, secrets_));
            results.array.push_back(std::move(entry));
        }
        data.object["results"] = std::move(results);
        data.object["result_count"] = number_value(static_cast<double>(response.results.size()));
        return envelope(true, std::move(data), "", "", {}, false);
    }

    if (name == "goal_met") {
        if (mutation_policy_ == MutationPolicy::Disabled)
            return tool_error_result("policy_denied",
                                    "goal_met is only available in agent sessions");
        std::string evidence;
        if (!get_string(args, "evidence", evidence, true, validation_error))
            return tool_error_result("invalid_arguments", validation_error);
        evidence = ascii_trim(evidence);
        if (evidence.empty())
            return tool_error_result(
                "invalid_arguments",
                "goal_met requires non-empty evidence that the goal condition holds");
        if (!goal_hooks_.has_active_goal || !goal_hooks_.has_active_goal())
            return tool_error_result(
                "no_active_goal",
                "no active session goal; set one with /goal <condition> before calling goal_met");
        if (!goal_hooks_.mark_complete)
            return tool_error_result("no_active_goal",
                                    "goal completion is not available in this session");
        const Error complete_error = goal_hooks_.mark_complete(evidence);
        if (!complete_error.ok()) {
            const std::string code =
                complete_error.code == ErrorCode::BadArgs ? "invalid_arguments"
                : complete_error.code == ErrorCode::UnsupportedFeature
                    ? "no_active_goal"
                    : error_code_string(complete_error.code);
            return tool_error_result(code, complete_error.message);
        }
        json::Value data = object_value();
        data.object["evidence"] =
            string_value(redact_secrets(evidence, secrets_));
        data.object["status"] = string_value("complete");
        return envelope(true, std::move(data), "", "", {}, false);
    }

    if (name == "attach_image") {
        if (mutation_policy_ == MutationPolicy::Disabled)
            return tool_error_result("policy_denied",
                                    "attach_image is only available in agent sessions");
        std::string path;
        if (!get_string(args, "path", path, true, validation_error))
            return tool_error_result("invalid_arguments", validation_error);
        path = ascii_trim(path);
        if (path.empty())
            return tool_error_result("invalid_arguments", "path must not be empty");
        if (!vision_hooks_.queue_image && vision_hooks_.attachment_bag == nullptr)
            return tool_error_result(
                "unsupported",
                "attach_image is not available in this session");

        input::FileType type;
        Error type_error = input::classify_file_type(path, type);
        if (!type_error.ok() || type.kind != input::Kind::Image)
            return tool_error_result(
                "invalid_arguments",
                type_error.ok()
                    ? ("path is not a supported image type (.png/.jpg/.jpeg/.gif): " + path)
                    : type_error.message);
        // Project-relative paths resolve under the workspace; external paths need approval.
        fs::path absolute;
        if (safe_relative_path(path) && !path.empty()) {
            fs::path current(snapshot_.workspace);
            std::error_code ec;
            for (const fs::path& component : fs::u8path(path)) {
                current /= component;
                const fs::file_status status = fs::symlink_status(current, ec);
                if (ec || status.type() == fs::file_type::not_found)
                    return tool_error_result(
                        "file_read",
                        "image does not exist or cannot be inspected: " + path +
                            (ec ? " (" + ec.message() + ")" : std::string()));
                if (fs::is_symlink(status))
                    return tool_error_result(
                        "policy_denied",
                        "refusing symlink path for attach_image: " + path);
            }
            if (!fs::is_regular_file(current, ec) || ec)
                return tool_error_result(
                    "file_read", "attach_image path must be a regular file: " + path);
            absolute = fs::canonical(current, ec);
            if (ec || absolute.empty())
                return tool_error_result(
                    "file_read",
                    "could not resolve image path " + path +
                        (ec ? ": " + ec.message() : std::string()));
            const fs::path root = fs::canonical(snapshot_.workspace, ec);
            if (ec || !resolved_path_is_under(root, absolute))
                return tool_error_result(
                    "policy_denied",
                    "image path escaped the active project: " + path);
        } else {
            Error resolve_error =
                resolve_external_file_path(snapshot_.workspace, path, true, absolute);
            if (!resolve_error.ok())
                return tool_error_result(error_code_string(resolve_error.code),
                                         resolve_error.message);
            const GuardApprovalDecision decision = request_permission(
                "attach_image", "attach_image " + absolute.generic_u8string(),
                {absolute.generic_u8string()}, true,
                resolved_path_is_under_system_temp(absolute), false, false,
                "ask_on_external_file_read",
                "Attach this image outside the active project for this turn "
                "(vision and/or MCP)?\nExact resolved path: " +
                    absolute.generic_u8string() +
                    "\nImage bytes are request-local for this turn only and are not stored "
                    "in the project.",
                cancellation);
            if (decision != GuardApprovalDecision::Allow) {
                std::string message =
                    "refusing to attach image outside the project without user approval: " +
                    absolute.generic_u8string();
                if (decision == GuardApprovalDecision::Cancelled)
                    message += " (approval cancelled)";
                else if (!on_guard_ask_)
                    message += " (headless agent denies external file access)";
                else
                    message += " (user selected No)";
                return tool_error_result(
                    decision == GuardApprovalDecision::Cancelled ? "cancelled"
                                                                 : "policy_denied",
                    message);
            }
        }
        if (cancellation.cancelled())
            return tool_error_result("cancelled", "attach_image cancelled");
        const std::size_t limit = vision_hooks_.max_image_bytes > 0
                                      ? vision_hooks_.max_image_bytes
                                      : 20U * 1024U * 1024U;
        input::ImageData loaded;
        Error load_error =
            input::load_image_file(absolute.generic_u8string(), type, limit, loaded,
                                   cancellation);
        if (!load_error.ok())
            return tool_error_result(error_code_string(load_error.code), load_error.message);

        bool vision_ok = vision_hooks_.vision_capable && static_cast<bool>(vision_hooks_.queue_image);
        if (vision_ok && vision_hooks_.validate_capability) {
            const Error capability = vision_hooks_.validate_capability();
            if (!capability.ok()) vision_ok = false;
        }

        bool vision_queued = false;
        if (vision_ok) {
            provider::ImageInput image{loaded.mime_type, loaded.base64_data};
            image.display_name = path;
            image.source_ref = absolute.generic_u8string();
            image.byte_size = static_cast<long long>(loaded.byte_size);
            const Error queue_error = vision_hooks_.queue_image(std::move(image));
            if (!queue_error.ok()) {
                const std::string code =
                    queue_error.code == ErrorCode::UnsupportedFeature ? "limit_exceeded"
                    : queue_error.code == ErrorCode::BadArgs           ? "invalid_arguments"
                                                                       : error_code_string(queue_error.code);
                return tool_error_result(code, queue_error.message);
            }
            vision_queued = true;
        }

        if (vision_hooks_.attachment_bag != nullptr) {
            const Error bag_error = vision_hooks_.attachment_bag->add_image(
                absolute.generic_u8string(), path, loaded.mime_type, loaded.base64_data,
                loaded.byte_size, AttachmentSource::AttachImageTool, vision_queued);
            if (!bag_error.ok())
                return tool_error_result(error_code_string(bag_error.code), bag_error.message);
        } else if (!vision_queued) {
            return tool_error_result(
                "unsupported",
                "attach_image requires a vision-capable model or an MCP attachment bag");
        }

        json::Value data = object_value();
        data.object["path"] = string_value(path);
        data.object["resolved_path"] = string_value(absolute.generic_u8string());
        data.object["mime_type"] = string_value(type.mime_type);
        data.object["bytes"] = number_value(static_cast<double>(loaded.byte_size));
        data.object["scope"] = string_value("request_local_turn");
        data.object["vision_queued"] = bool_value(vision_queued);
        data.object["mcp_bag"] = bool_value(vision_hooks_.attachment_bag != nullptr);
        if (vision_queued) {
            data.object["note"] = string_value(
                "Image will be included on subsequent model rounds of this turn only; "
                "it is not stored in agent.sqlite or project media. "
                "Pass this path in mcp__* tool args when calling image MCPs.");
        } else {
            data.object["note"] = string_value(
                "Model is text-only: pixels were not sent to the LLM. Image is registered "
                "for this turn's MCP tools — pass the path (or absolute path) in mcp__* "
                "arguments; the host rewrites to base64 for remote HTTP MCPs.");
        }
        return envelope(true, std::move(data), "", "", {}, false);
    }

    return tool_error_result("unknown_tool", "unknown native tool: " + requested_name);
}

}  // namespace ainiux::agent
