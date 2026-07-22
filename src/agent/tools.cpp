#include "agent/tools.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <regex>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

#include "agent/apply_patch.hpp"
#include "agent/process.hpp"
#include "agent/project_paths.hpp"
#include "agent/text_match.hpp"
#include "agent/tool_args.hpp"
#include "fetch/fetch.hpp"
#include "html/html.hpp"
#include "json/json.hpp"
#include "search/search.hpp"
#include "security/redact.hpp"

namespace ainiux::agent {
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

std::string schema(const std::string& properties, const std::string& required = "") {
    return "{\"type\":\"object\",\"properties\":{" + properties + "},\"required\":[" + required +
           "],\"additionalProperties\":false}";
}

bool safe_relative_path(const std::string& path) {
    if (path.empty() || path == ".") return true;
    const fs::path candidate(path);
    if (candidate.is_absolute()) return false;
    for (const fs::path& component : candidate) {
        const std::string value = component.string();
        if (value == ".." || is_protected_state_dir_name(value))
            return false;
    }
    return true;
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
    if (!cwd.empty() && cwd != ".") combined /= fs::path(cwd);
    combined /= fs::path(path);
    std::string normalized = combined.lexically_normal().generic_string();
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
    const fs::path absolute = fs::path(snapshot.workspace) / path;
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
                                       bool index_only) {
    const std::string normalized_cwd = normalized_workspace_path("", cwd.empty() ? "." : cwd);
    if (!safe_relative_path(normalized_cwd) ||
        !path_allowed_for_command(snapshot, normalized_cwd, true, index_only))
        return {ErrorCode::BadArgs,
                index_only ? "run_command cwd has no eligible indexed content"
                           : "run_command cwd is outside the workspace or not visible"};
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const std::string& argument = arguments[index];
        if (argument.empty() || argument.front() == '-') continue;
        const std::string relative = normalized_workspace_path(normalized_cwd, argument);
        if (!safe_relative_path(relative))
            return {ErrorCode::BadArgs, "run_command path escapes or targets protected metadata"};
        std::error_code ec;
        const fs::file_status status = fs::symlink_status(fs::path(snapshot.workspace) / relative, ec);
        if (!ec && status.type() != fs::file_type::not_found &&
            !path_allowed_for_command(snapshot, relative, fs::is_directory(status), index_only))
            return {ErrorCode::BadArgs,
                    index_only
                        ? ("run_command path is not eligible in the completed index snapshot: " +
                           relative)
                        : ("run_command path is not a visible workspace entry: " + relative)};
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
            if (fs::is_directory(fs::path(snapshot.workspace) / operand, ec)) listing_base = operand;
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
    loaded.allow_mutations_ = options.allow_mutations;
    loaded.allow_network_ = options.allow_network;
    loaded.history_backup_ = options.history_backup;
    loaded.fetch_options_ = options.fetch_options;
    loaded.search_options_ = options.search_options;
    registry = std::move(loaded);
    registry.rebuild_file_map();
    (void)registry.purge_expired_history_backups();
    return ok_error();
}

void ReadToolRegistry::rebuild_file_map() const {
    files_.clear();
    for (const index::IndexedFile& file : snapshot_.files) files_[file.path] = &file;
}

Error ReadToolRegistry::resolve_writable_path(const std::string& relative_path,
                                              fs::path& absolute) const {
    if (relative_path.empty() || !safe_relative_path(relative_path))
        return {ErrorCode::BadArgs, "path must be a safe workspace-relative file path"};
    const std::string generic = fs::path(relative_path).generic_string();
    if (generic.empty() || generic == ".")
        return {ErrorCode::BadArgs, "path must name a file, not the workspace root"};
    fs::path current(snapshot_.workspace);
    fs::path remaining = fs::path(generic);
    // Walk existing parents and refuse symlink components before creating anything.
    for (const fs::path& component : remaining) {
        current /= component;
        std::error_code ec;
        if (!fs::exists(current, ec) || ec) break;
        const fs::file_status status = fs::symlink_status(current, ec);
        if (ec) return {ErrorCode::FileWrite, "could not inspect path " + generic + ": " + ec.message()};
        if (fs::is_symlink(status))
            return {ErrorCode::FileWrite, "refusing symlink path in workspace write: " + generic};
    }
    absolute = fs::path(snapshot_.workspace) / remaining;
    return ok_error();
}

Error ReadToolRegistry::purge_expired_history_backups() const {
    if (!history_backup_.enabled || history_backup_.ttl_days <= 0) return ok_error();
    const fs::path history_dir =
        fs::path(snapshot_.workspace) / kProjectStateDirName / "history";
    std::error_code ec;
    if (!fs::is_directory(history_dir, ec) || ec) return ok_error();
    const std::time_t now_tt = std::time(nullptr);
    const std::time_t ttl_sec = static_cast<std::time_t>(history_backup_.ttl_days) * 86400;
    for (fs::directory_iterator it(history_dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec) || ec) continue;
        const std::string name = it->path().filename().string();
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".bak") != 0) continue;
#if defined(_WIN32)
        (void)now_tt;
        (void)ttl_sec;
#else
        struct stat st {};
        if (::stat(it->path().c_str(), &st) != 0) continue;
        if (now_tt >= st.st_mtime && (now_tt - st.st_mtime) > ttl_sec) {
            std::error_code rm_ec;
            fs::remove(it->path(), rm_ec);
        }
#endif
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

    const fs::path history_dir =
        fs::path(snapshot_.workspace) / kProjectStateDirName / "history";
    std::error_code ec;
    fs::create_directories(history_dir, ec);
    if (ec) return {ErrorCode::FileWrite, "could not create history directory: " + ec.message()};

    // One stable slot per workspace path (hash of generic relative path).
    const std::string generic = fs::path(relative_path).generic_string();
    const std::string digest = index::content_hash(generic);
    std::string short_hash = digest.size() > 16 ? digest.substr(0, 16) : digest;
    std::string safe_tail = generic;
    for (char& ch : safe_tail) {
        if (ch == '/' || ch == '\\' || ch == ':' || ch == ' ') ch = '_';
    }
    if (safe_tail.size() > 48) safe_tail = safe_tail.substr(safe_tail.size() - 48);
    const fs::path history_file = history_dir / (short_hash + "-" + safe_tail + ".bak");
    {
        std::ofstream out(history_file, std::ios::binary | std::ios::trunc);
        if (!out) return {ErrorCode::FileWrite, "could not open history file: " + history_file.string()};
        out.write(previous_content.data(), static_cast<std::streamsize>(previous_content.size()));
        if (!out) return {ErrorCode::FileWrite, "could not write history file: " + history_file.string()};
    }
    history_path =
        (fs::path(kProjectStateDirName) / "history" / history_file.filename()).generic_string();
    (void)purge_expired_history_backups();
    return ok_error();
}

void ReadToolRegistry::note_written_file(const std::string& relative_path,
                                         const std::string& content) const {
    const std::string generic = fs::path(relative_path).generic_string();
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
}

void ReadToolRegistry::note_removed_path(const std::string& relative_path) const {
    const std::string generic = fs::path(relative_path).generic_string();
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
}

namespace {

Error write_bytes_atomic(const fs::path& absolute, const std::string& content) {
    const fs::path temporary =
        absolute.string() + ".ainiux-tmp." + std::to_string(static_cast<long long>(::getpid()));
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return {ErrorCode::FileWrite, "could not open temporary file: " + temporary.string()};
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!out) {
            std::error_code ignore;
            fs::remove(temporary, ignore);
            return {ErrorCode::FileWrite, "could not write temporary file: " + temporary.string()};
        }
    }
    std::error_code ec;
    fs::rename(temporary, absolute, ec);
    if (ec) {
        std::error_code ignore;
        fs::remove(temporary, ignore);
        return {ErrorCode::FileWrite, "could not replace file " + absolute.string() + ": " + ec.message()};
    }
    return ok_error();
}

std::string read_all_bytes(const fs::path& absolute, Error& error) {
    std::ifstream input(absolute, std::ios::binary);
    if (!input) {
        error = {ErrorCode::FileRead, "could not open file: " + absolute.string()};
        return {};
    }
    std::string content{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad()) {
        error = {ErrorCode::FileRead, "could not read file: " + absolute.string()};
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
    if (!allow_mutations_) return {ErrorCode::UnsupportedFeature, "workspace writes are disabled for this tool session"};
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
        error = save_history_copy(relative_path, previous, history_path);
        if (!error.ok()) return error;
    } else if (!expected_file_hash.empty()) {
        return {ErrorCode::FileWrite, "stale_file: expected_file_hash set but file does not exist"};
    }

    const fs::path parent = absolute.parent_path();
    if (!parent.empty()) {
        const bool parent_exists = fs::exists(parent, ec) && !ec;
        if (!parent_exists) {
            if (!create_dirs)
                return {ErrorCode::FileWrite,
                        "parent directory does not exist; pass create_dirs=true"};
            fs::create_directories(parent, ec);
            if (ec)
                return {ErrorCode::FileWrite, "could not create parent directories: " + ec.message()};
        }
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
    if (!allow_mutations_) return {ErrorCode::UnsupportedFeature, "workspace writes are disabled for this tool session"};
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
        const std::string name = entry.path().filename().string();
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
    if (!allow_mutations_)
        return {ErrorCode::UnsupportedFeature, "workspace writes are disabled for this tool session"};

    fs::path absolute;
    Error error = resolve_writable_path(relative_path, absolute);
    if (!error.ok()) return error;

    const std::string generic = fs::path(relative_path).generic_string();
    const std::string basename = absolute.filename().string();
    const fs::path parent = absolute.parent_path();

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
            message += "? Filenames may include literal # characters—use list_directory and the exact name.";
        }
        return {ErrorCode::FileRead, message};
    }
    if (fs::is_symlink(fs::symlink_status(absolute, ec)) || ec) {
        guard_rule_id = "refuse_symlink";
        return {ErrorCode::FileWrite, "refusing to remove symlink: " + relative_path};
    }

    if (is_database_path(generic)) {
        // No interactive approval UI in one-shot agent yet — deny high-risk DB deletes.
        guard_rule_id = "ask_on_database_delete";
        return {ErrorCode::FileWrite,
                "refusing to delete database file without interactive approval: " + generic +
                    " (remove is blocked for *.sqlite/*.db/*.duckdb in headless agent mode)"};
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
                    "(list_directory first). Do not strip # from filenames."};
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
            // Recursive: still refuse if any nested path looks like a database file.
            for (fs::recursive_directory_iterator it(absolute, ec), end; !ec && it != end;
                 it.increment(ec)) {
                const std::string name = it->path().filename().string();
                std::string rel = generic;
                const fs::path nested = it->path().lexically_relative(absolute);
                if (!nested.empty() && nested != ".")
                    rel = (fs::path(generic) / nested).generic_string();
                if (is_database_path(name) || is_database_path(rel)) {
                    guard_rule_id = "ask_on_database_delete";
                    return {ErrorCode::FileWrite,
                            "refusing recursive remove because it would delete database file: " +
                                rel};
                }
                if (fs::is_symlink(it->symlink_status(ec)) || ec) {
                    guard_rule_id = "refuse_symlink";
                    return {ErrorCode::FileWrite,
                            "refusing recursive remove of tree containing symlink: " + rel};
                }
            }
            if (ec) return {ErrorCode::FileWrite, "could not walk directory: " + ec.message()};
        }
        if (!expected_file_hash.empty())
            return {ErrorCode::BadArgs, "expected_file_hash is only valid for file removals"};
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
        error = save_history_copy(relative_path, previous, history_path);
        if (!error.ok()) return error;
    } else if (!expected_file_hash.empty()) {
        return {ErrorCode::BadArgs, "expected_file_hash is only supported for UTF-8 text files"};
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
    if (!allow_mutations_)
        return {ErrorCode::UnsupportedFeature, "workspace writes are disabled for this tool session"};

    ParsedPatch parsed;
    Error error = parse_apply_patch(patch_text, parsed);
    if (!error.ok()) return error;

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

    for (const PatchFileOp& op : parsed.ops) {
        Planned item;
        item.kind = op.kind;
        item.path = fs::path(op.path).generic_string();
        if (item.path.empty() || !safe_relative_path(item.path))
            return {ErrorCode::BadArgs, "apply_patch path must be a safe workspace-relative path: " +
                                            op.path};
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
            if (!exists)
                return {ErrorCode::FileRead, "Delete File target does not exist: " + item.path};
            item.existed = true;
            item.previous = read_all_bytes(absolute, error);
            if (!error.ok()) return error;
            plan.push_back(std::move(item));
            continue;
        }

        // UpdateFile
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

// Some models (e.g. DeepSeek via OpenRouter) nest the operation:
//   {"replace_range":{"start_line":1,"end_line":1,"new_text":"..."}}
// or split text outside the nested object:
//   {"replace_range":{"start_line":1,"end_line":1},"text":"..."}
// Flatten that into the flat schema before type inference / parsing.
json::Value normalize_edit_op_shape(const json::Value& op) {
    if (!op.is_object()) return op;
    static const char* kNestedTypes[] = {"replace_range", "insert_at", "delete_range",
                                         "replace_text",  "replace_symbol", "create_file"};
    const json::Value* nested = nullptr;
    std::string nested_type;
    for (const char* name : kNestedTypes) {
        const json::Value* candidate = op.get(name);
        if (candidate != nullptr && candidate->is_object()) {
            // Prefer the first nested op-type key; ignore if multiple (ambiguous).
            if (nested != nullptr) return op;
            nested = candidate;
            nested_type = name;
        }
    }
    if (nested == nullptr) return op;

    json::Value flat = object_value();
    flat.object["type"] = string_value(nested_type);
    for (const auto& entry : nested->object) flat.object[entry.first] = entry.second;
    // Preserve top-level siblings (text, expected_hash, replace_all, …).
    for (const auto& entry : op.object) {
        if (entry.first == nested_type) continue;
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
    if (!allow_mutations_)
        return {ErrorCode::UnsupportedFeature, "workspace writes are disabled for this tool session"};
    if (!ops.is_array() || ops.array.empty())
        return {ErrorCode::BadArgs, "edit_file requires a non-empty ops array"};
    if (ops.array.size() > 100)
        return {ErrorCode::BadArgs, "edit_file supports at most 100 operations per call"};

    // Normalize nested/aliased op shapes once so create-file detection and parsing agree.
    std::vector<json::Value> normalized_ops;
    normalized_ops.reserve(ops.array.size());
    for (const json::Value& raw_op : ops.array) {
        if (!raw_op.is_object()) return {ErrorCode::BadArgs, "each edit_file op must be an object"};
        normalized_ops.push_back(normalize_edit_op_shape(raw_op));
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
                        validation_error.empty() ? "insert_at requires line" : validation_error};
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

    std::vector<std::string> lines = split_lines(previous);

    // Resolve replace_symbol into replace_range against the current snapshot index.
    for (LineEditOp& op : parsed) {
        if (op.type != LineEditOp::Type::ReplaceSymbol) continue;
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
        if (found->path != fs::path(relative_path).generic_string())
            return {ErrorCode::BadArgs,
                    "replace_symbol: symbol_id " + std::to_string(op.symbol_id) + " is in " +
                        found->path + ", not " + fs::path(relative_path).generic_string()};
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
    error = save_history_copy(relative_path, previous, history_path);
    if (!error.ok()) return error;
    error = write_bytes_atomic(absolute, content);
    if (!error.ok()) return error;
    new_hash = index::content_hash(content);
    note_written_file(relative_path, content);
    return ok_error();
}

std::vector<provider::FunctionDefinition> ReadToolRegistry::definitions() const {
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
        "\"expected_hash\":{\"type\":\"string\"},"
        "\"symbol_id\":{\"type\":\"integer\",\"minimum\":1}";
    const std::string edit_nested_object =
        "{\"type\":\"object\",\"properties\":{" + edit_line_fields + "}}";
    const std::string edit_op_item =
        "{\"type\":\"object\",\"properties\":{"
        "\"type\":{\"type\":\"string\"},"
        "\"op\":{\"type\":\"string\"}," +
        edit_line_fields +
        ",\"line_range_hint\":{\"type\":\"object\",\"properties\":{"
        "\"start_line\":{\"type\":\"integer\",\"minimum\":1},"
        "\"end_line\":{\"type\":\"integer\",\"minimum\":1}}},"
        "\"replace_range\":" +
        edit_nested_object +
        ",\"insert_at\":" + edit_nested_object + ",\"delete_range\":" + edit_nested_object +
        ",\"replace_text\":" + edit_nested_object + ",\"replace_symbol\":" + edit_nested_object +
        ",\"create_file\":" + edit_nested_object +
        "}}";
    std::vector<provider::FunctionDefinition> tools = {
        {"project_overview",
         "Summarize the code index (languages, indexed files, symbols hints, freshness). "
         "This is NOT a full filesystem listing—empty directories and non-source files are omitted. "
         "Use list_directory for the real workspace tree.",
         schema("")},
        {"list_directory",
         "List real filesystem entries in a workspace-relative directory (files, empty dirs, "
         "non-source names). Names are literal (may include #, spaces). Index-only tools miss "
         "empty directories and non-code files—prefer this for layout questions and before remove.",
         schema(path + ",\"max_entries\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":500}")},
        {"glob", "Match indexed relative file paths using *, ?, **, and brace alternatives.", schema("\"pattern\":{\"type\":\"string\"},\"max_results\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":1000}", "\"pattern\"")},
        {"search_text", "Search indexed UTF-8 files using bounded literal or line-oriented regex matching.", schema("\"query\":{\"type\":\"string\"},\"regex\":{\"type\":\"boolean\"},\"case_sensitive\":{\"type\":\"boolean\"},\"word\":{\"type\":\"boolean\"},\"glob\":{\"type\":\"string\"},\"context\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":10},\"max_results\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":500}", "\"query\"")},
        {"grep", "Alias for search_text.", schema("\"query\":{\"type\":\"string\"},\"regex\":{\"type\":\"boolean\"},\"case_sensitive\":{\"type\":\"boolean\"},\"word\":{\"type\":\"boolean\"},\"glob\":{\"type\":\"string\"},\"context\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":10},\"max_results\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":500}", "\"query\"")},
        {"find", "Validated alias for search_text.", schema("\"query\":{\"type\":\"string\"},\"regex\":{\"type\":\"boolean\"},\"case_sensitive\":{\"type\":\"boolean\"},\"word\":{\"type\":\"boolean\"},\"glob\":{\"type\":\"string\"},\"context\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":10},\"max_results\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":500}", "\"query\"")},
        {"search_symbol", "Rank indexed symbol names by case-insensitive exact, prefix, then substring match.", schema("\"query\":{\"type\":\"string\"},\"max_results\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":200}", "\"query\"")},
        {"get_skeleton", "Return ordered indexed declarations, signatures, ranges, and documentation for one file.", schema(path, "\"path\"")},
        {"read_symbol", "Fingerprint-verify and read the actual indexed source range for a symbol id.", schema("\"symbol_id\":{\"type\":\"integer\",\"minimum\":1}", "\"symbol_id\"")},
        {"read_file", "Fingerprint-verify and read a bounded UTF-8 line range with hashes and line numbers.", schema(range, "\"path\"")},
        {"read_many", "Read multiple bounded line ranges under one aggregate byte cap.", schema("\"items\":{\"type\":\"array\",\"items\":" + schema(range, "\"path\"") + ",\"maxItems\":100},\"max_bytes\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":262144}", "\"items\"")},
        {"run_command",
         allow_mutations_
             ? "Run an allowlisted workspace command without a shell (python3/make/ctest/node/go/"
               "cargo/g++ plus read-only ls/rg/find/git). Destructive commands (rm -rf, git "
               "reset --hard, shells, sudo) are denied. Prefer native tools for file edits."
             : "Run one allowlisted read-only inspection command without a shell "
               "(pwd/ls/rg/grep/find/git).",
         schema("\"command\":{\"type\":\"string\"},\"cwd\":{\"type\":\"string\"},"
                "\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":" +
                    std::string(allow_mutations_ ? "120000" : "10000") + "}",
                "\"command\"")},
        {"git_status",
         "Compact git status for the workspace (short form with branch by default). "
         "Uses the git CLI; not libgit2. Prefer this over run_command for status.",
         schema("\"short\":{\"type\":\"boolean\"},\"include_branch\":{\"type\":\"boolean\"}")},
        {"git_diff",
         "Bounded git diff for the workspace (optional path, --cached, --stat). "
         "Uses the git CLI with pager/external-diff disabled.",
         schema("\"path\":{\"type\":\"string\"},\"cached\":{\"type\":\"boolean\"},"
                "\"stat\":{\"type\":\"boolean\"},"
                "\"max_bytes\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":524288}")},
        {"index_status",
         "Report .ainiux-pr/index.sqlite state (file/symbol counts, freshness, optional "
         "changed-path sample).",
         schema("\"check_filesystem\":{\"type\":\"boolean\"},"
                "\"max_changed_files\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":500}")},
        {"index_update",
         "Incrementally refresh the code index for changed files (or only listed paths). "
         "force=true rescans even when size/mtime look unchanged.",
         schema("\"paths\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"maxItems\":200},"
                "\"force\":{\"type\":\"boolean\"}")},
        {"find_tests",
         "Heuristically find likely tests for a path or symbol (naming/path conventions).",
         schema("\"path\":{\"type\":\"string\"},\"symbol_id\":{\"type\":\"integer\",\"minimum\":1},"
                "\"max_results\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":100}")},
        {"inspect_code_task",
         "Macro-tool: rank likely files/symbols/tests for a natural-language coding task "
         "from the index (no network).",
         schema("\"query\":{\"type\":\"string\"},"
                "\"max_symbols\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":100},"
                "\"max_files\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":100},"
                "\"include_skeletons\":{\"type\":\"boolean\"},"
                "\"include_tests\":{\"type\":\"boolean\"},"
                "\"max_bytes\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":262144}",
                "\"query\"")},
    };
    if (allow_mutations_) {
        tools.push_back(
            {"index_rebuild",
             "Full rebuild of .ainiux-pr/index.sqlite (recovery/debugging). Requires confirm=true.",
             schema("\"confirm\":{\"type\":\"boolean\"}", "\"confirm\"")});
    }
    if (allow_network_) {
        tools.push_back(
            {"fetch_url",
             "Fetch one http(s) URL and return UTF-8 Markdown (or plain text). Never returns raw "
             "HTML/CSS/JS—scripts, styles, and comments are stripped to reduce tokens and "
             "prompt-injection risk. max_bytes caps the returned Markdown size (not raw HTML). "
             "Prefer the top 1–3 search hits only. Private/loopback blocked unless configured.",
             schema("\"url\":{\"type\":\"string\"},"
                    "\"max_bytes\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":8388608},"
                    "\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":120000}",
                    "\"url\"")});
        tools.push_back(
            {"search_web",
             "Web search (API providers when configured, else free DuckDuckGo HTML). Returns "
             "title/URL/snippet for at most 3 results by default—use those top hits only; do not "
             "fetch every URL. Returns web_search_unavailable when no provider can run.",
             schema("\"term\":{\"type\":\"string\"},"
                    "\"max_results\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":3},"
                    "\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":120000},"
                    "\"site\":{\"type\":\"string\"}",
                    "\"term\"")});
    }
    if (allow_mutations_) {
        tools.push_back(
            {"edit_file",
             "Preferred in-file edit (not for deleting whole files—use remove). Ops: "
             "insert_at (add lines), replace_range (rewrite line spans; include full old text in "
             "replacement when substituting), delete_range, replace_text (exact then fuzzy), "
             "replace_symbol, create_file (alone). Line ops apply bottom-to-top.",
             schema(path + ",\"expected_file_hash\":{\"type\":\"string\"},"
                           "\"create_dirs\":{\"type\":\"boolean\"},"
                           "\"ops\":{\"type\":\"array\",\"minItems\":1,\"maxItems\":100,"
                           "\"items\":" +
                           edit_op_item + "}",
                    "\"path\",\"ops\"")});
        tools.push_back(
            {"write_file",
             "Create or overwrite a workspace-relative UTF-8 file. Prefer edit_file.replace_range for edits.",
             schema(path + ",\"content\":{\"type\":\"string\"},"
                           "\"create_dirs\":{\"type\":\"boolean\"},"
                           "\"expected_file_hash\":{\"type\":\"string\"},"
                           "\"mode\":{\"type\":\"string\",\"enum\":[\"overwrite\",\"create_new\"]}",
                    "\"path\",\"content\"")});
        tools.push_back(
            {"str_replace",
             "Text replacement in one workspace file (exact, then optional fuzzy whitespace/indent). "
             "Prefer edit_file when possible. Fails on 0 matches or ambiguous multi-match without "
             "replace_all or line_range_hint.",
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
             "Delete a workspace-relative file or empty directory (recursive=true for non-empty "
             "dirs). Use the exact filename from list_directory—do not strip # or other "
             "punctuation. When both name and #name# exist, plain name requires confirm=true. "
             "Database files (*.sqlite/*.db) are refused in headless mode. Prefer remove over "
             "edit_file for deleting files.",
             schema(path + ",\"recursive\":{\"type\":\"boolean\"},"
                           "\"confirm\":{\"type\":\"boolean\"},"
                           "\"expected_file_hash\":{\"type\":\"string\"}",
                    "\"path\"")});
        tools.push_back(
            {"apply_patch",
             "Apply an OpenAI/Codex-style multi-file patch. Prefer edit_file for simple "
             "single-file edits. Preferred form:\n"
             "*** Begin Patch\n"
             "*** Update File: path\n"
             "@@\n"
             " context\n"
             "-old\n"
             "+new\n"
             "*** End Patch\n"
             "Also accepts bare *** Update/Add/Delete File sections without Begin/End "
             "(common with local models). patch/input/diff aliases; fuzzy=true default.",
             schema("\"patch\":{\"type\":\"string\"},"
                    "\"input\":{\"type\":\"string\"},"
                    "\"diff\":{\"type\":\"string\"},"
                    "\"atomic\":{\"type\":\"boolean\"},"
                    "\"fuzzy\":{\"type\":\"boolean\"}",
                    "")});
    }
    return tools;
}

Error ReadToolRegistry::read_source(const std::string& path,
                                    std::size_t start_line,
                                    std::size_t end_line,
                                    std::size_t max_bytes,
                                    SourceRange& range) const {
    if (!safe_relative_path(path) || path.empty()) return {ErrorCode::BadArgs, "path must be a safe workspace-relative indexed file"};
    const auto found = files_.find(fs::path(path).generic_string());
    if (found == files_.end() || found->second->status != "indexed")
        return {ErrorCode::FileRead, "path is not an eligible indexed file: " + path};
    const index::IndexedFile& record = *found->second;
    fs::path current(snapshot_.workspace);
    for (const fs::path& component : fs::path(record.path)) {
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
    if (source.find('\0') != std::string::npos || !html::is_valid_utf8(source))
        return {ErrorCode::FileRead, "indexed file is no longer valid UTF-8 text: " + record.path};
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
    if (cancellation.cancelled()) return tool_error_result("cancelled", "tool call cancelled");

    // Stage 7: exact alias mapping first, then case/snake-camel repair against
    // the registry. grep/find remain explicit aliases of search_text.
    std::string name = requested_name;
    if (name == "grep" || name == "find") name = "search_text";
    else {
        std::vector<std::string> known;
        known.reserve(16);
        for (const provider::FunctionDefinition& definition : definitions())
            known.push_back(definition.name);
        const std::string repaired = repair_tool_name(requested_name, known);
        if (!repaired.empty()) {
            name = repaired;
            if (name == "grep" || name == "find") name = "search_text";
        }
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

    if (name == "project_overview") {
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

    if (name == "list_directory") {
        std::string path;
        std::size_t maximum = 200;
        if (!get_string(args, "path", path, false, validation_error) ||
            !get_size(args, "max_entries", 200, 500, maximum, validation_error))
            return tool_error_result("invalid_arguments", validation_error);
        if (!safe_relative_path(path))
            return tool_error_result("policy_denied",
                                    "directory path escapes or targets protected metadata");
        path = path == "." ? "" : fs::path(path).generic_string();
        if (!path.empty() && path.back() == '/') path.pop_back();

        const fs::path absolute =
            path.empty() ? fs::path(snapshot_.workspace) : fs::path(snapshot_.workspace) / path;
        std::error_code ec;
        if (!path.empty()) {
            // Walk components and refuse symlinks / missing parents.
            fs::path current(snapshot_.workspace);
            for (const fs::path& component : fs::path(path)) {
                current /= component;
                const fs::file_status status = fs::symlink_status(current, ec);
                if (ec || status.type() == fs::file_type::not_found)
                    return tool_error_result("not_found", "directory does not exist: " + path);
                if (fs::is_symlink(status))
                    return tool_error_result("policy_denied",
                                            "refusing symlink path in list_directory: " + path);
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
            const std::string name = entry.path().filename().string();
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
                        const std::string nested_name = nested.path().filename().string();
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
                path.empty() ? name : (fs::path(path) / name).generic_string();
            item.indexed = eligible_indexed_path(snapshot_, relative, false) ||
                           (item.type == "directory" &&
                            eligible_indexed_path(snapshot_, relative, true));
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
        if (path.empty())
            warnings.push_back(
                "filesystem listing (not index-only); empty directories and non-source files included");
        return envelope(true, std::move(data), "", "", warnings, truncated);
    }

    if (name == "glob") {
        std::string pattern;
        std::size_t maximum = 200;
        if (!get_string(args, "pattern", pattern, true, validation_error) ||
            !get_size(args, "max_results", 200, 1000, maximum, validation_error))
            return tool_error_result("invalid_arguments", validation_error);
        if (!safe_relative_path(pattern)) return tool_error_result("policy_denied", "glob targets protected metadata or traversal");
        json::Value data = array_value(); bool truncated = false;
        try {
            for (const index::IndexedFile& file : snapshot_.files) if (glob_matches(file.path, pattern)) {
                if (data.array.size() >= maximum) { truncated = true; break; }
                data.array.push_back(string_value(file.path));
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
        const std::string needle = lowercase(query);
        struct Ranked { int rank; const index::IndexedSymbol* symbol; };
        std::vector<Ranked> ranked;
        for (const index::IndexedSymbol& symbol : snapshot_.symbols) {
            const std::string simple = lowercase(symbol.symbol.name);
            const std::string qualified = lowercase(symbol.symbol.qualified_name);
            int rank = simple == needle || qualified == needle ? 0 :
                       simple.rfind(needle, 0) == 0 || qualified.rfind(needle, 0) == 0 ? 1 :
                       simple.find(needle) != std::string::npos || qualified.find(needle) != std::string::npos ? 2 : 3;
            if (rank < 3) ranked.push_back({rank, &symbol});
        }
        std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
            if (a.rank != b.rank) return a.rank < b.rank;
            if (a.symbol->path != b.symbol->path) return a.symbol->path < b.symbol->path;
            if (a.symbol->symbol.line_start != b.symbol->symbol.line_start) return a.symbol->symbol.line_start < b.symbol->symbol.line_start;
            return a.symbol->id < b.symbol->id;
        });
        json::Value data = array_value(); bool truncated = ranked.size() > maximum;
        for (std::size_t i = 0; i < std::min(maximum, ranked.size()); ++i) {
            const index::IndexedSymbol& symbol = *ranked[i].symbol;
            json::Value item = object_value(); item.object["id"] = number_value(symbol.id);
            item.object["path"] = string_value(symbol.path); item.object["kind"] = string_value(symbol.symbol.kind);
            item.object["name"] = string_value(symbol.symbol.qualified_name); item.object["signature"] = string_value(symbol.symbol.signature);
            item.object["line_start"] = number_value(symbol.symbol.line_start); item.object["line_end"] = number_value(symbol.symbol.line_end);
            data.array.push_back(std::move(item));
        }
        return envelope(true, std::move(data), "", "", {}, truncated);
    }

    if (name == "get_skeleton") {
        std::string path;
        if (!get_string(args, "path", path, true, validation_error)) return tool_error_result("invalid_arguments", validation_error);
        const auto file = files_.find(fs::path(path).generic_string());
        if (file == files_.end() || file->second->status != "indexed") return tool_error_result("not_found", "file is not indexed: " + path);
        json::Value data = array_value();
        for (const index::IndexedSymbol& symbol : snapshot_.symbols) if (symbol.path == file->second->path) {
            json::Value item = object_value(); item.object["id"] = number_value(symbol.id);
            item.object["kind"] = string_value(symbol.symbol.kind); item.object["name"] = string_value(symbol.symbol.qualified_name);
            item.object["signature"] = string_value(symbol.symbol.signature); item.object["line_start"] = number_value(symbol.symbol.line_start);
            item.object["line_end"] = number_value(symbol.symbol.line_end); item.object["documentation"] = string_value(symbol.symbol.documentation);
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
        const Error error = read_source(path, start, end, maximum, range);
        if (!error.ok()) return tool_error_result(error_code_string(error.code), error.message);
        std::ostringstream numbered;
        const std::vector<std::string> lines = split_lines(range.content);
        for (std::size_t index = 0; index < lines.size(); ++index) numbered << (range.start_line + index) << ": " << lines[index];
        json::Value data = object_value(); data.object["path"] = string_value(range.path);
        data.object["line_start"] = number_value(range.start_line); data.object["line_end"] = number_value(range.end_line);
        data.object["content"] = string_value(numbered.str()); data.object["file_hash"] = string_value(range.file_hash);
        data.object["range_hash"] = string_value(range.range_hash); data.object["bytes"] = number_value(range.bytes);
        std::vector<std::string> warnings; if (range.redacted) warnings.push_back("configured credential value was redacted");
        return envelope(true, std::move(data), "", "", warnings, range.truncated);
    }

    if (name == "read_many") {
        const json::Value* items = args.get("items");
        std::size_t maximum = 262144;
        if (items == nullptr || !items->is_array() || items->array.size() > 100 ||
            !get_size(args, "max_bytes", 262144, 262144, maximum, validation_error) || maximum == 0)
            return tool_error_result("invalid_arguments", validation_error.empty() ? "items must be an array of at most 100 ranges" : validation_error);
        json::Value data = array_value(); std::vector<std::string> warnings; std::size_t remaining = maximum;
        bool truncated = false;
        for (std::size_t index = 0; index < items->array.size(); ++index) {
            if (remaining == 0) { warnings.push_back("omitted item " + std::to_string(index) + " because the aggregate cap was reached"); truncated = true; continue; }
            const json::Value& item = items->array[index];
            if (!item.is_object()) { warnings.push_back("omitted item " + std::to_string(index) + ": range must be an object"); truncated = true; continue; }
            std::string path; std::size_t start = 1, end = 0;
            if (!get_string(item, "path", path, true, validation_error) ||
                !get_size(item, "start_line", 1, 100000000, start, validation_error) ||
                !get_size(item, "end_line", 0, 100000000, end, validation_error)) {
                warnings.push_back("omitted item " + std::to_string(index) + ": " + validation_error); truncated = true; continue;
            }
            SourceRange range; const Error error = read_source(path, start, end, remaining, range);
            if (!error.ok()) { warnings.push_back("omitted " + path + ": " + error.message); truncated = true; continue; }
            json::Value output = object_value(); output.object["path"] = string_value(range.path);
            output.object["line_start"] = number_value(range.start_line); output.object["line_end"] = number_value(range.end_line);
            output.object["content"] = string_value(range.content); output.object["file_hash"] = string_value(range.file_hash);
            output.object["range_hash"] = string_value(range.range_hash); output.object["truncated"] = bool_value(range.truncated);
            data.array.push_back(std::move(output)); remaining -= std::min(remaining, range.bytes);
            truncated = truncated || range.truncated; if (range.redacted) warnings.push_back(path + ": configured credential value was redacted");
        }
        json::Value metadata = object_value(); metadata.object["byte_cap"] = number_value(maximum); metadata.object["bytes_remaining"] = number_value(remaining);
        return envelope(true, std::move(data), "", "", warnings, truncated, std::move(metadata));
    }

    if (name == "search_text") {
        std::string query, glob;
        bool regex_mode = false, case_sensitive = false, word = false;
        std::size_t context = 0, maximum = 50;
        if (!get_string(args, "query", query, true, validation_error) || !get_string(args, "glob", glob, false, validation_error) ||
            !get_bool(args, "regex", false, regex_mode, validation_error) || !get_bool(args, "case_sensitive", false, case_sensitive, validation_error) ||
            !get_bool(args, "word", false, word, validation_error) || !get_size(args, "context", 0, 10, context, validation_error) ||
            !get_size(args, "max_results", 50, 500, maximum, validation_error))
            return tool_error_result("invalid_arguments", validation_error);
        std::regex expression;
        try {
            std::string pattern = regex_mode ? query : regex_escape(query);
            if (word) pattern = "\\b(?:" + pattern + ")\\b";
            expression = std::regex(pattern, std::regex::ECMAScript | (case_sensitive ? std::regex::flag_type{} : std::regex::icase));
        } catch (const std::regex_error& exception) { return tool_error_result("invalid_regex", exception.what()); }
        json::Value data = array_value(); std::vector<std::string> warnings; bool truncated = false;
        for (const index::IndexedFile& file : snapshot_.files) {
            if (file.status != "indexed" || (!glob.empty() && !glob_matches(file.path, glob))) continue;
            SourceRange source; const Error read_error = read_source(file.path, 1, 0, static_cast<std::size_t>(file.size) + 1, source);
            if (!read_error.ok()) { warnings.push_back(read_error.message); continue; }
            const std::vector<std::string> lines = split_lines(source.content);
            for (std::size_t line = 0; line < lines.size(); ++line) if (std::regex_search(lines[line], expression)) {
                if (data.array.size() >= maximum) { truncated = true; break; }
                json::Value match = object_value(); match.object["path"] = string_value(file.path); match.object["line"] = number_value(line + 1);
                match.object["text"] = string_value(lines[line]);
                if (context > 0) {
                    json::Value nearby = array_value(); const std::size_t begin = line > context ? line - context : 0;
                    const std::size_t finish = std::min(lines.size(), line + context + 1);
                    for (std::size_t adjacent = begin; adjacent < finish; ++adjacent) if (adjacent != line) {
                        json::Value item = object_value(); item.object["line"] = number_value(adjacent + 1); item.object["text"] = string_value(lines[adjacent]); nearby.array.push_back(std::move(item));
                    }
                    match.object["context"] = std::move(nearby);
                }
                data.array.push_back(std::move(match));
            }
            if (truncated) break;
        }
        return envelope(true, std::move(data), "", "", warnings, truncated);
    }

    if (name == "run_command") {
        std::string command, cwd;
        const std::size_t timeout_cap = allow_mutations_ ? 120000 : 10000;
        std::size_t timeout = allow_mutations_ ? 30000 : 10000;
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
            allow_mutations_ ? CommandPolicy::Agent : CommandPolicy::InspectionOnly;
        Error policy_error = parse_command(command, parsed_arguments, policy, guard_rule_id);
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
        const bool index_only_commands = !allow_mutations_;
        policy_error =
            validate_command_workspace_paths(snapshot_, parsed_arguments, cwd, index_only_commands);
        if (!policy_error.ok()) return tool_error_result("policy_denied", policy_error.message);
        ProcessOptions options;
        options.workspace = snapshot_.workspace;
        options.cwd = cwd;
        options.timeout_ms = static_cast<long>(timeout);
        options.cancellation = cancellation;
        ProcessResult process;
        const Error error = run_command(command, options, process, policy);
        bool output_filtered = false;
        const std::string command_name = parsed_arguments.empty() ? std::string() : parsed_arguments.front();
        // parse_inspection_command has already inserted the fixed Git -c
        // hardening pairs, leaving the validated subcommand at index 9.
        const bool git_file_listing = command_name == "git" && parsed_arguments.size() > 9 &&
                                      parsed_arguments[9] == "ls-files";
        if (error.ok() && (command_name == "find" || command_name == "ls" ||
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
        data.object["exit_status"] = number_value(process.exit_status);
        data.object["signal"] = number_value(process.signal);
        data.object["duration_ms"] = number_value(process.duration_ms);
        data.object["stdout"] = string_value(redact_secrets(process.stdout_text, secrets_));
        data.object["stderr"] = string_value(redact_secrets(process.stderr_text, secrets_));
        data.object["stdout_truncated"] = bool_value(process.stdout_truncated);
        data.object["stderr_truncated"] = bool_value(process.stderr_truncated);
        data.object["policy"] = string_value(process.policy);
        json::Value guard = object_value();
        guard.object["decision"] = string_value(error.ok() ? "allow" : "deny");
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
        if (!allow_mutations_)
            return tool_error_result("policy_denied", "edit_file is not enabled in this session");
        // Models (esp. under natural-language goals) often nest path inside ops[i]
        // instead of the top-level edit_file.path required by the schema. Promote
        // common top-level fields from ops when missing so the call still works.
        json::Value edit_args = args;
        if (!edit_args.is_object())
            return tool_error_result("invalid_arguments", "edit_file arguments must be an object");
        json::Value* ops_value = nullptr;
        {
            auto it = edit_args.object.find("ops");
            if (it != edit_args.object.end()) ops_value = &it->second;
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
        const json::Value& ops = *ops_value;
        std::string history_path, old_hash, new_hash;
        std::size_t operations_applied = 0;
        std::vector<std::string> summary;
        std::vector<std::string> warnings;
        const Error error =
            edit_workspace_file(path, expected_hash, ops, create_dirs, history_path, old_hash,
                                new_hash, operations_applied, summary, warnings);
        json::Value data = object_value();
        data.object["path"] = string_value(fs::path(path).generic_string());
        data.object["applied"] = bool_value(error.ok());
        data.object["operations_applied"] = number_value(static_cast<double>(operations_applied));
        data.object["old_file_hash"] = string_value(old_hash);
        data.object["new_file_hash"] = string_value(new_hash);
        data.object["history_path"] = string_value(history_path);
        data.object["indexed_snapshot_updated"] = bool_value(error.ok());
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
        return envelope(true, std::move(data), "", "", warnings, false);
    }

    if (name == "write_file") {
        if (!allow_mutations_)
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
        const Error error = write_workspace_file(path, content, create_dirs, mode, expected_hash,
                                                 history_path, created, old_hash, new_hash);
        json::Value data = object_value();
        data.object["path"] = string_value(fs::path(path).generic_string());
        data.object["bytes_written"] = number_value(static_cast<double>(content.size()));
        data.object["created"] = bool_value(created);
        data.object["old_file_hash"] = string_value(old_hash);
        data.object["new_file_hash"] = string_value(new_hash);
        data.object["history_path"] = string_value(history_path);
        data.object["indexed_snapshot_updated"] = bool_value(error.ok());
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
        return envelope(true, std::move(data), "", "", {}, false);
    }

    if (name == "str_replace") {
        if (!allow_mutations_)
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
        const Error error = str_replace_workspace_file(
            path, old_text, new_text, replace_all, allow_fuzzy, hint_start, hint_end, expected_hash,
            history_path, matches_found, replacements_made, match_mode, old_hash, new_hash,
            candidate_lines);
        json::Value data = object_value();
        data.object["path"] = string_value(fs::path(path).generic_string());
        data.object["matches_found"] = number_value(static_cast<double>(matches_found));
        data.object["replacements_made"] = number_value(static_cast<double>(replacements_made));
        data.object["match_mode"] = string_value(match_mode.empty() ? "exact" : match_mode);
        data.object["old_file_hash"] = string_value(old_hash);
        data.object["new_file_hash"] = string_value(new_hash);
        data.object["history_path"] = string_value(history_path);
        data.object["indexed_snapshot_updated"] = bool_value(error.ok());
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
        return envelope(true, std::move(data), "", "", {}, false);
    }

    if (name == "remove") {
        if (!allow_mutations_)
            return tool_error_result("policy_denied", "remove is not enabled in this session");
        std::string path, expected_hash;
        bool recursive = false;
        bool confirm = false;
        if (!get_string(args, "path", path, true, validation_error) ||
            !get_bool(args, "recursive", false, recursive, validation_error) ||
            !get_bool(args, "confirm", false, confirm, validation_error) ||
            !get_string(args, "expected_file_hash", expected_hash, false, validation_error))
            return tool_error_result("invalid_arguments", validation_error);
        std::string history_path, old_hash, guard_decision, guard_rule_id;
        bool was_directory = false;
        std::vector<std::string> suggestions;
        std::vector<std::string> warnings;
        const Error error = remove_workspace_path(path, recursive, confirm, expected_hash,
                                                  history_path, was_directory, guard_decision,
                                                  guard_rule_id, old_hash, suggestions, warnings);
        json::Value data = object_value();
        data.object["path"] = string_value(fs::path(path).generic_string());
        data.object["removed"] = bool_value(error.ok());
        data.object["was_directory"] = bool_value(was_directory);
        data.object["history_path"] = string_value(history_path);
        data.object["old_file_hash"] = string_value(old_hash);
        data.object["index_updated"] = bool_value(error.ok());
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
        if (!allow_mutations_)
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
        options.timeout_ms = allow_mutations_ ? 30000 : 10000;
        options.cancellation = cancellation;
        options.stdout_limit = 65536;
        options.stderr_limit = 16384;
        ProcessResult process;
        const CommandPolicy policy =
            allow_mutations_ ? CommandPolicy::Agent : CommandPolicy::InspectionOnly;
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
            return tool_error_result("policy_denied",
                                    "git_diff path escapes or targets protected metadata");
        std::string command = "git diff --no-color --no-ext-diff";
        if (cached) command += " --cached";
        if (stat_only) command += " --stat";
        if (!path.empty()) command += " -- " + path;
        ProcessOptions options;
        options.workspace = snapshot_.workspace;
        options.timeout_ms = allow_mutations_ ? 30000 : 10000;
        options.cancellation = cancellation;
        options.stdout_limit = max_bytes;
        options.stderr_limit = 16384;
        ProcessResult process;
        const CommandPolicy policy =
            allow_mutations_ ? CommandPolicy::Agent : CommandPolicy::InspectionOnly;
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

    if (name == "index_status") {
        bool check_fs = true;
        std::size_t max_changed = 50;
        if (!get_bool(args, "check_filesystem", true, check_fs, validation_error) ||
            !get_size(args, "max_changed_files", 50, 500, max_changed, validation_error))
            return tool_error_result("invalid_arguments", validation_error);
        json::Value data = object_value();
        const std::string db_path = index::database_path(snapshot_.workspace);
        std::error_code ec;
        const bool exists = fs::exists(db_path, ec) && !ec;
        data.object["index_exists"] = bool_value(exists);
        data.object["path"] = string_value(db_path);
        data.object["files_indexed"] = number_value(static_cast<double>(snapshot_.files.size()));
        data.object["symbols_indexed"] =
            number_value(static_cast<double>(snapshot_.symbols.size()));
        data.object["refs_indexed"] = number_value(0);  // call-graph refs not stored yet
        data.object["last_updated"] = number_value(static_cast<double>(snapshot_.updated_at));
        data.object["workspace"] = string_value(snapshot_.workspace);
        bool fresh = true;
        json::Value changed = array_value();
        std::vector<std::string> warnings;
        if (check_fs) {
            index::Freshness freshness;
            index::Options opts = index_options_;
            opts.cancellation = cancellation;
            const Error fresh_error = index::check_freshness(opts, freshness);
            if (!fresh_error.ok()) {
                warnings.push_back(fresh_error.message);
                fresh = false;
            } else {
                fresh = freshness.fresh;
                std::size_t count = 0;
                auto append_paths = [&](const std::vector<std::string>& paths,
                                        const char* kind) {
                    for (const std::string& path : paths) {
                        if (count >= max_changed) return;
                        json::Value item = object_value();
                        item.object["path"] = string_value(path);
                        item.object["change"] = string_value(kind);
                        changed.array.push_back(std::move(item));
                        ++count;
                    }
                };
                append_paths(freshness.added, "added");
                append_paths(freshness.changed, "changed");
                append_paths(freshness.removed, "removed");
                if (!freshness.reason.empty() && !fresh)
                    warnings.push_back(freshness.reason);
            }
        }
        data.object["fresh"] = bool_value(fresh);
        data.object["changed_files"] = std::move(changed);
        return envelope(true, std::move(data), "", "", warnings, false);
    }

    if (name == "index_update") {
        bool force = false;
        if (!get_bool(args, "force", false, force, validation_error))
            return tool_error_result("invalid_arguments", validation_error);
        std::vector<std::string> paths;
        const json::Value* paths_value = args.get("paths");
        if (paths_value != nullptr) {
            if (!paths_value->is_array())
                return tool_error_result("invalid_arguments", "paths must be an array of strings");
            if (paths_value->array.size() > 200)
                return tool_error_result("invalid_arguments", "paths accepts at most 200 entries");
            for (const json::Value& item : paths_value->array) {
                if (!item.is_string() || item.string.empty())
                    return tool_error_result("invalid_arguments",
                                            "paths entries must be non-empty strings");
                if (!safe_relative_path(item.string))
                    return tool_error_result("policy_denied",
                                            "index_update path escapes or targets protected "
                                            "metadata: " +
                                                item.string);
                paths.push_back(fs::path(item.string).generic_string());
            }
        }
        index::Options opts = index_options_;
        opts.cancellation = cancellation;
        opts.force_rescan = force;
        opts.update_paths = paths;
        index::RefreshStats stats;
        const Error error = index::refresh(opts, stats);
        if (!error.ok())
            return tool_error_result(error_code_string(error.code), error.message);
        index::Snapshot next;
        const Error load_error = index::load_snapshot(opts, next);
        if (!load_error.ok())
            return tool_error_result(error_code_string(load_error.code), load_error.message);
        snapshot_ = std::move(next);
        rebuild_file_map();
        json::Value data = object_value();
        data.object["discovered"] = number_value(static_cast<double>(stats.discovered));
        data.object["indexed"] = number_value(static_cast<double>(stats.indexed));
        data.object["unchanged"] = number_value(static_cast<double>(stats.unchanged));
        data.object["skipped"] = number_value(static_cast<double>(stats.skipped));
        data.object["removed"] = number_value(static_cast<double>(stats.removed));
        data.object["symbols"] = number_value(static_cast<double>(stats.symbols));
        data.object["elapsed_ms"] = number_value(static_cast<double>(stats.elapsed_ms));
        data.object["files_indexed"] = number_value(static_cast<double>(snapshot_.files.size()));
        data.object["symbols_indexed"] =
            number_value(static_cast<double>(snapshot_.symbols.size()));
        data.object["last_updated"] = number_value(static_cast<double>(snapshot_.updated_at));
        data.object["force"] = bool_value(force);
        json::Value path_array = array_value();
        for (const std::string& path : paths) path_array.array.push_back(string_value(path));
        data.object["paths"] = std::move(path_array);
        std::vector<std::string> warnings = stats.diagnostics;
        return envelope(true, std::move(data), "", "", warnings, false);
    }

    if (name == "index_rebuild") {
        if (!allow_mutations_)
            return tool_error_result("policy_denied", "index_rebuild is not enabled in this session");
        bool confirm = false;
        if (!get_bool(args, "confirm", false, confirm, validation_error))
            return tool_error_result("invalid_arguments", validation_error);
        if (!confirm)
            return tool_error_result("invalid_arguments",
                                    "index_rebuild requires confirm=true (full index rebuild)");
        index::Options opts = index_options_;
        opts.cancellation = cancellation;
        index::ClearStats clear_stats;
        Error error = index::clear_database(opts, clear_stats);
        if (!error.ok())
            return tool_error_result(error_code_string(error.code), error.message);
        opts.force_rescan = true;
        index::RefreshStats stats;
        error = index::refresh(opts, stats);
        if (!error.ok())
            return tool_error_result(error_code_string(error.code), error.message);
        index::Snapshot next;
        error = index::load_snapshot(opts, next);
        if (!error.ok())
            return tool_error_result(error_code_string(error.code), error.message);
        snapshot_ = std::move(next);
        rebuild_file_map();
        json::Value data = object_value();
        data.object["cleared_files"] = number_value(static_cast<double>(clear_stats.removed_files));
        data.object["discovered"] = number_value(static_cast<double>(stats.discovered));
        data.object["indexed"] = number_value(static_cast<double>(stats.indexed));
        data.object["symbols"] = number_value(static_cast<double>(stats.symbols));
        data.object["elapsed_ms"] = number_value(static_cast<double>(stats.elapsed_ms));
        data.object["files_indexed"] = number_value(static_cast<double>(snapshot_.files.size()));
        data.object["symbols_indexed"] =
            number_value(static_cast<double>(snapshot_.symbols.size()));
        data.object["last_updated"] = number_value(static_cast<double>(snapshot_.updated_at));
        return envelope(true, std::move(data), "", "", stats.diagnostics, false);
    }

    if (name == "find_tests") {
        std::string path;
        std::size_t symbol_id = 0;
        std::size_t max_results = 20;
        if (!get_string(args, "path", path, false, validation_error) ||
            !get_size(args, "symbol_id", 0, 1000000000, symbol_id, validation_error) ||
            !get_size(args, "max_results", 20, 100, max_results, validation_error) ||
            max_results == 0)
            return tool_error_result("invalid_arguments",
                                    validation_error.empty() ? "max_results must be positive"
                                                             : validation_error);
        if (path.empty() && symbol_id == 0)
            return tool_error_result("invalid_arguments",
                                    "find_tests requires path and/or symbol_id");
        if (!path.empty() && !safe_relative_path(path))
            return tool_error_result("policy_denied",
                                    "find_tests path escapes or targets protected metadata");

        std::string focus_name;
        std::string focus_path = path.empty() ? std::string() : fs::path(path).generic_string();
        if (symbol_id != 0) {
            bool found = false;
            for (const index::IndexedSymbol& symbol : snapshot_.symbols) {
                if (static_cast<std::size_t>(symbol.id) != symbol_id) continue;
                focus_name = symbol.symbol.name;
                if (focus_path.empty()) focus_path = symbol.path;
                found = true;
                break;
            }
            if (!found)
                return tool_error_result("not_found",
                                        "symbol_id not in index: " + std::to_string(symbol_id));
        }

        auto basename_stem = [](const std::string& file_path) {
            const fs::path p(file_path);
            std::string stem = p.stem().string();
            // Strip common source suffixes like .test already handled by stem.
            return stem;
        };
        const std::string stem = focus_path.empty() ? std::string() : basename_stem(focus_path);
        const std::string stem_lower = lowercase(stem);
        const std::string name_lower = lowercase(focus_name);

        struct Ranked {
            double score = 0;
            std::string path;
            long long symbol_id = 0;
            std::string qualified_name;
        };
        std::vector<Ranked> ranked;

        auto consider_file = [&](const std::string& candidate_path, double base) {
            if (candidate_path == focus_path) return;
            const std::string lower = lowercase(candidate_path);
            double score = base;
            if (!stem_lower.empty()) {
                if (lower.find("/test_") != std::string::npos ||
                    lower.find("/tests/") != std::string::npos ||
                    lower.find("/test/") != std::string::npos ||
                    lower.find("_test.") != std::string::npos ||
                    lower.find(".test.") != std::string::npos ||
                    lower.find("_spec.") != std::string::npos ||
                    lower.rfind("test_", 0) == 0)
                    score += 0.2;
                if (lower.find(stem_lower) != std::string::npos) score += 0.5;
            }
            if (score < 0.3) return;
            ranked.push_back({score, candidate_path, 0, {}});
        };

        for (const index::IndexedFile& file : snapshot_.files) {
            if (file.status != "indexed") continue;
            const std::string lower = lowercase(file.path);
            const bool looks_test =
                lower.find("/test") != std::string::npos ||
                lower.find("_test.") != std::string::npos ||
                lower.find(".test.") != std::string::npos ||
                lower.find("_spec.") != std::string::npos ||
                lower.find("/spec/") != std::string::npos ||
                lower.rfind("test_", 0) == 0 ||
                fs::path(file.path).filename().string().rfind("test_", 0) == 0;
            if (!looks_test && focus_path.empty()) continue;
            if (looks_test) consider_file(file.path, 0.4);
            else if (!stem_lower.empty() && lower.find(stem_lower) != std::string::npos)
                consider_file(file.path, 0.25);
        }

        for (const index::IndexedSymbol& symbol : snapshot_.symbols) {
            const std::string q = lowercase(symbol.symbol.qualified_name);
            const std::string n = lowercase(symbol.symbol.name);
            double score = 0;
            if (!name_lower.empty() &&
                (n.find(name_lower) != std::string::npos ||
                 q.find(name_lower) != std::string::npos))
                score += 0.6;
            if (!stem_lower.empty() &&
                (lowercase(symbol.path).find(stem_lower) != std::string::npos))
                score += 0.3;
            const bool looks_test =
                n.rfind("test_", 0) == 0 || n.rfind("test", 0) == 0 ||
                q.find("test") != std::string::npos ||
                lowercase(symbol.path).find("/test") != std::string::npos;
            if (!looks_test) continue;
            if (score < 0.3 && name_lower.empty()) score = 0.35;
            if (score < 0.3) continue;
            ranked.push_back({score, symbol.path, symbol.id, symbol.symbol.qualified_name});
        }

        std::sort(ranked.begin(), ranked.end(),
                  [](const Ranked& a, const Ranked& b) {
                      if (a.score != b.score) return a.score > b.score;
                      return a.path < b.path;
                  });
        // De-dupe by path+symbol.
        std::set<std::string> seen;
        json::Value tests = array_value();
        bool truncated = false;
        for (const Ranked& item : ranked) {
            const std::string key =
                item.path + "#" + std::to_string(item.symbol_id) + "#" + item.qualified_name;
            if (!seen.insert(key).second) continue;
            if (tests.array.size() >= max_results) {
                truncated = true;
                break;
            }
            json::Value entry = object_value();
            entry.object["path"] = string_value(item.path);
            if (item.symbol_id > 0)
                entry.object["symbol_id"] = number_value(static_cast<double>(item.symbol_id));
            if (!item.qualified_name.empty())
                entry.object["qualified_name"] = string_value(item.qualified_name);
            entry.object["confidence"] = number_value(item.score > 1.0 ? 1.0 : item.score);
            tests.array.push_back(std::move(entry));
        }

        json::Value commands = array_value();
        if (files_.find("Makefile") != files_.end()) {
            commands.array.push_back(string_value("make test"));
            commands.array.push_back(string_value("make test-unit"));
        }
        if (files_.find("CMakeLists.txt") != files_.end())
            commands.array.push_back(string_value("ctest --output-on-failure"));
        if (files_.find("package.json") != files_.end())
            commands.array.push_back(string_value("npm test"));
        if (files_.find("Cargo.toml") != files_.end())
            commands.array.push_back(string_value("cargo test"));
        if (files_.find("go.mod") != files_.end())
            commands.array.push_back(string_value("go test ./..."));
        if (files_.find("pyproject.toml") != files_.end() ||
            files_.find("pytest.ini") != files_.end())
            commands.array.push_back(string_value("python3 -m pytest"));

        json::Value data = object_value();
        data.object["tests"] = std::move(tests);
        data.object["commands"] = std::move(commands);
        if (!focus_path.empty()) data.object["path"] = string_value(focus_path);
        if (symbol_id != 0) data.object["symbol_id"] = number_value(static_cast<double>(symbol_id));
        return envelope(true, std::move(data), "", "", {}, truncated);
    }

    if (name == "inspect_code_task") {
        std::string query;
        std::size_t max_symbols = 20;
        std::size_t max_files = 20;
        bool include_skeletons = false;
        bool include_tests = true;
        std::size_t max_bytes = 32768;
        if (!get_string(args, "query", query, true, validation_error) ||
            !get_size(args, "max_symbols", 20, 100, max_symbols, validation_error) ||
            !get_size(args, "max_files", 20, 100, max_files, validation_error) ||
            !get_bool(args, "include_skeletons", false, include_skeletons, validation_error) ||
            !get_bool(args, "include_tests", true, include_tests, validation_error) ||
            !get_size(args, "max_bytes", 32768, 262144, max_bytes, validation_error) ||
            max_symbols == 0 || max_files == 0 || max_bytes == 0)
            return tool_error_result("invalid_arguments",
                                    validation_error.empty() ? "limits must be positive"
                                                             : validation_error);
        const std::string query_lower = lowercase(query);
        // Tokenize on non-alnum for lightweight ranking.
        std::vector<std::string> tokens;
        std::string token;
        for (unsigned char ch : query_lower) {
            if (std::isalnum(ch) || ch == '_' || ch == '-') {
                token.push_back(static_cast<char>(ch));
            } else if (!token.empty()) {
                if (token.size() >= 2) tokens.push_back(token);
                token.clear();
            }
        }
        if (token.size() >= 2) tokens.push_back(token);
        if (tokens.empty() && !query_lower.empty()) tokens.push_back(query_lower);

        auto token_score = [&](const std::string& text_lower) {
            double score = 0;
            for (const std::string& t : tokens) {
                if (text_lower == t)
                    score += 1.0;
                else if (text_lower.rfind(t, 0) == 0)
                    score += 0.7;
                else if (text_lower.find(t) != std::string::npos)
                    score += 0.4;
            }
            return score;
        };

        struct RankedSymbol {
            double score = 0;
            const index::IndexedSymbol* symbol = nullptr;
        };
        std::vector<RankedSymbol> symbol_hits;
        for (const index::IndexedSymbol& symbol : snapshot_.symbols) {
            const std::string n = lowercase(symbol.symbol.name);
            const std::string qn = lowercase(symbol.symbol.qualified_name);
            const std::string path_l = lowercase(symbol.path);
            double score = token_score(n) * 1.2 + token_score(qn) + token_score(path_l) * 0.5;
            if (score <= 0) continue;
            symbol_hits.push_back({score, &symbol});
        }
        std::sort(symbol_hits.begin(), symbol_hits.end(),
                  [](const RankedSymbol& a, const RankedSymbol& b) {
                      if (a.score != b.score) return a.score > b.score;
                      return a.symbol->id < b.symbol->id;
                  });

        struct RankedFile {
            double score = 0;
            std::string path;
        };
        std::map<std::string, double> file_scores;
        for (const RankedSymbol& hit : symbol_hits) {
            file_scores[hit.symbol->path] += hit.score;
        }
        for (const index::IndexedFile& file : snapshot_.files) {
            if (file.status != "indexed") continue;
            const double score = token_score(lowercase(file.path));
            if (score > 0) file_scores[file.path] += score * 0.8;
        }
        // Boost important root files lightly when query mentions them.
        static const char* kImportant[] = {"README.md", "AGENTS.md", "Makefile", "CMakeLists.txt",
                                           "package.json", "pyproject.toml", "Cargo.toml", "go.mod"};
        for (const char* name : kImportant) {
            if (files_.find(name) == files_.end()) continue;
            if (query_lower.find(lowercase(name)) != std::string::npos)
                file_scores[name] += 2.0;
        }
        std::vector<RankedFile> file_hits;
        for (const auto& entry : file_scores)
            file_hits.push_back({entry.second, entry.first});
        std::sort(file_hits.begin(), file_hits.end(),
                  [](const RankedFile& a, const RankedFile& b) {
                      if (a.score != b.score) return a.score > b.score;
                      return a.path < b.path;
                  });

        json::Value likely_symbols = array_value();
        json::Value suggested_reads = array_value();
        bool truncated = false;
        for (std::size_t i = 0; i < symbol_hits.size(); ++i) {
            if (i >= max_symbols) {
                truncated = true;
                break;
            }
            const index::IndexedSymbol& symbol = *symbol_hits[i].symbol;
            json::Value item = object_value();
            item.object["symbol_id"] = number_value(static_cast<double>(symbol.id));
            item.object["qualified_name"] = string_value(symbol.symbol.qualified_name);
            item.object["path"] = string_value(symbol.path);
            item.object["start_line"] = number_value(symbol.symbol.line_start);
            item.object["end_line"] = number_value(symbol.symbol.line_end);
            item.object["score"] = number_value(symbol_hits[i].score);
            item.object["reason"] = string_value("name/path token match");
            likely_symbols.array.push_back(std::move(item));
            if (suggested_reads.array.size() < max_files) {
                json::Value read = object_value();
                read.object["path"] = string_value(symbol.path);
                read.object["start_line"] = number_value(symbol.symbol.line_start);
                read.object["end_line"] = number_value(symbol.symbol.line_end);
                suggested_reads.array.push_back(std::move(read));
            }
        }

        json::Value likely_files = array_value();
        for (std::size_t i = 0; i < file_hits.size(); ++i) {
            if (i >= max_files) {
                truncated = true;
                break;
            }
            likely_files.array.push_back(string_value(file_hits[i].path));
        }

        json::Value skeletons = array_value();
        if (include_skeletons) {
            std::size_t budget = max_bytes;
            for (std::size_t i = 0; i < file_hits.size() && i < max_files && budget > 0; ++i) {
                const std::string& file_path = file_hits[i].path;
                json::Value skeleton = object_value();
                skeleton.object["path"] = string_value(file_path);
                json::Value decls = array_value();
                for (const index::IndexedSymbol& symbol : snapshot_.symbols) {
                    if (symbol.path != file_path) continue;
                    json::Value d = object_value();
                    d.object["symbol_id"] = number_value(static_cast<double>(symbol.id));
                    d.object["name"] = string_value(symbol.symbol.qualified_name);
                    d.object["kind"] = string_value(symbol.symbol.kind);
                    d.object["start_line"] = number_value(symbol.symbol.line_start);
                    d.object["end_line"] = number_value(symbol.symbol.line_end);
                    const std::string piece = symbol.symbol.qualified_name + symbol.symbol.kind;
                    if (piece.size() > budget) {
                        truncated = true;
                        break;
                    }
                    budget -= piece.size();
                    decls.array.push_back(std::move(d));
                }
                skeleton.object["symbols"] = std::move(decls);
                skeletons.array.push_back(std::move(skeleton));
            }
        }

        json::Value likely_tests = array_value();
        if (include_tests && !file_hits.empty()) {
            // Reuse find_tests heuristics for the top file.
            const std::string top = file_hits.front().path;
            const std::string stem = lowercase(fs::path(top).stem().string());
            for (const index::IndexedFile& file : snapshot_.files) {
                if (file.status != "indexed") continue;
                const std::string lower = lowercase(file.path);
                const bool looks_test =
                    lower.find("/test") != std::string::npos ||
                    lower.find("_test.") != std::string::npos ||
                    lower.find(".test.") != std::string::npos;
                if (!looks_test) continue;
                if (!stem.empty() && lower.find(stem) == std::string::npos) continue;
                if (likely_tests.array.size() >= 10) {
                    truncated = true;
                    break;
                }
                likely_tests.array.push_back(string_value(file.path));
            }
        }

        json::Value data = object_value();
        data.object["query"] = string_value(query);
        data.object["likely_symbols"] = std::move(likely_symbols);
        data.object["likely_files"] = std::move(likely_files);
        data.object["suggested_reads"] = std::move(suggested_reads);
        data.object["skeletons"] = std::move(skeletons);
        data.object["likely_tests"] = std::move(likely_tests);
        return envelope(true, std::move(data), "", "", {}, truncated);
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

    if (name == "search_web") {
        if (!allow_network_)
            return tool_error_result("policy_denied", "search_web is not enabled in this session");
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

    return tool_error_result("unknown_tool", "unknown native tool: " + requested_name);
}

}  // namespace ainiux::agent

