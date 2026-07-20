#include "agent/tools.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <regex>
#include <set>
#include <sstream>

#include "agent/process.hpp"
#include "agent/tool_args.hpp"
#include "html/html.hpp"
#include "json/json.hpp"
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
        if (value == ".." || value == ".ainiux" || value == ".git" || value == ".hg" || value == ".svn")
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

std::regex glob_expression(const std::string& pattern) {
    std::string output = "^";
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char ch = pattern[i];
        if (ch == '*') {
            if (i + 1 < pattern.size() && pattern[i + 1] == '*') { ++i; output += ".*"; }
            else output += "[^/]*";
        } else if (ch == '?') output += "[^/]";
        else output += regex_escape(std::string(1, ch));
    }
    output += "$";
    return std::regex(output, std::regex::optimize);
}

bool glob_matches(const std::string& path, const std::string& pattern) {
    if (pattern.empty()) return true;
    std::vector<std::string> alternatives;
    expand_braces(pattern, alternatives);
    for (const std::string& alternative : alternatives) {
        try { if (std::regex_match(path, glob_expression(alternative))) return true; }
        catch (const std::regex_error&) { return false; }
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

bool eligible_virtual_path(const index::Snapshot& snapshot,
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

Error validate_command_workspace_paths(const index::Snapshot& snapshot,
                                       const std::vector<std::string>& arguments,
                                       const std::string& cwd) {
    const std::string normalized_cwd = normalized_workspace_path("", cwd.empty() ? "." : cwd);
    if (!safe_relative_path(normalized_cwd) ||
        !eligible_virtual_path(snapshot, normalized_cwd, true))
        return {ErrorCode::BadArgs, "run_command cwd has no eligible indexed content"};
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const std::string& argument = arguments[index];
        if (argument.empty() || argument.front() == '-') continue;
        const std::string relative = normalized_workspace_path(normalized_cwd, argument);
        if (!safe_relative_path(relative))
            return {ErrorCode::BadArgs, "run_command path escapes or targets protected metadata"};
        std::error_code ec;
        const fs::file_status status = fs::symlink_status(fs::path(snapshot.workspace) / relative, ec);
        if (!ec && status.type() != fs::file_type::not_found &&
            !eligible_virtual_path(snapshot, relative, fs::is_directory(status)))
            return {ErrorCode::BadArgs,
                    "run_command path is not eligible in the completed index snapshot: " + relative};
    }
    return ok_error();
}

std::string filter_path_lines(const index::Snapshot& snapshot,
                              const std::string& cwd,
                              const std::string& text,
                              const std::string& command,
                              const std::vector<std::string>& arguments,
                              bool& filtered) {
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
            eligible_virtual_path(snapshot, relative, allow_directory)) {
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
                               ReadToolRegistry& registry) {
    if (snapshot.workspace.empty()) return {ErrorCode::Internal, "tool registry requires a completed index snapshot"};
    ReadToolRegistry loaded;
    loaded.index_options_ = std::move(index_options);
    loaded.snapshot_ = std::move(snapshot);
    loaded.secrets_ = std::move(secrets);
    registry = std::move(loaded);
    registry.files_.clear();
    for (const index::IndexedFile& file : registry.snapshot_.files)
        registry.files_[file.path] = &file;
    return ok_error();
}

std::vector<provider::FunctionDefinition> ReadToolRegistry::definitions() const {
    const std::string path = "\"path\":{\"type\":\"string\"}";
    const std::string range = path + ",\"start_line\":{\"type\":\"integer\",\"minimum\":1},"
                                      "\"end_line\":{\"type\":\"integer\",\"minimum\":1},"
                                      "\"max_bytes\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":262144}";
    return {
        {"project_overview", "Summarize indexed languages, files, lines, likely entry points/tests, and freshness.", schema("")},
        {"list_directory", "List bounded indexed entries in a workspace-relative directory.", schema(path + ",\"max_entries\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":500}")},
        {"glob", "Match indexed relative file paths using *, ?, **, and brace alternatives.", schema("\"pattern\":{\"type\":\"string\"},\"max_results\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":1000}", "\"pattern\"")},
        {"search_text", "Search indexed UTF-8 files using bounded literal or line-oriented regex matching.", schema("\"query\":{\"type\":\"string\"},\"regex\":{\"type\":\"boolean\"},\"case_sensitive\":{\"type\":\"boolean\"},\"word\":{\"type\":\"boolean\"},\"glob\":{\"type\":\"string\"},\"context\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":10},\"max_results\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":500}", "\"query\"")},
        {"grep", "Alias for search_text.", schema("\"query\":{\"type\":\"string\"},\"regex\":{\"type\":\"boolean\"},\"case_sensitive\":{\"type\":\"boolean\"},\"word\":{\"type\":\"boolean\"},\"glob\":{\"type\":\"string\"},\"context\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":10},\"max_results\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":500}", "\"query\"")},
        {"find", "Validated alias for search_text.", schema("\"query\":{\"type\":\"string\"},\"regex\":{\"type\":\"boolean\"},\"case_sensitive\":{\"type\":\"boolean\"},\"word\":{\"type\":\"boolean\"},\"glob\":{\"type\":\"string\"},\"context\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":10},\"max_results\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":500}", "\"query\"")},
        {"search_symbol", "Rank indexed symbol names by case-insensitive exact, prefix, then substring match.", schema("\"query\":{\"type\":\"string\"},\"max_results\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":200}", "\"query\"")},
        {"get_skeleton", "Return ordered indexed declarations, signatures, ranges, and documentation for one file.", schema(path, "\"path\"")},
        {"read_symbol", "Fingerprint-verify and read the actual indexed source range for a symbol id.", schema("\"symbol_id\":{\"type\":\"integer\",\"minimum\":1}", "\"symbol_id\"")},
        {"read_file", "Fingerprint-verify and read a bounded UTF-8 line range with hashes and line numbers.", schema(range, "\"path\"")},
        {"read_many", "Read multiple bounded line ranges under one aggregate byte cap.", schema("\"items\":{\"type\":\"array\",\"items\":" + schema(range, "\"path\"") + ",\"maxItems\":100},\"max_bytes\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":262144}", "\"items\"")},
        {"run_command", "Run one allowlisted read-only inspection command without a shell.", schema("\"command\":{\"type\":\"string\"},\"cwd\":{\"type\":\"string\"},\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":10000}", "\"command\"")},
    };
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
        if (!safe_relative_path(path)) return tool_error_result("policy_denied", "directory path escapes or targets protected metadata");
        path = path == "." ? "" : fs::path(path).generic_string();
        if (!path.empty() && path.back() == '/') path.pop_back();
        const std::string prefix = path.empty() ? "" : path + "/";
        std::map<std::string, std::pair<std::string, std::uintmax_t>> entries;
        for (const index::IndexedFile& file : snapshot_.files) {
            if (file.path.rfind(prefix, 0) != 0) continue;
            const std::string tail = file.path.substr(prefix.size());
            const std::size_t slash = tail.find('/');
            const std::string child = slash == std::string::npos ? tail : tail.substr(0, slash);
            if (child.empty()) continue;
            entries[child] = slash == std::string::npos
                                 ? std::make_pair(std::string("file"), file.size)
                                 : std::make_pair(std::string("directory"), std::uintmax_t{0});
        }
        if (entries.empty() && !path.empty()) return tool_error_result("not_found", "directory has no eligible indexed entries: " + path);
        json::Value data = array_value();
        bool truncated = false;
        for (const auto& entry : entries) {
            if (data.array.size() >= maximum) { truncated = true; break; }
            json::Value item = object_value(); item.object["name"] = string_value(entry.first);
            item.object["type"] = string_value(entry.second.first); item.object["size"] = number_value(static_cast<double>(entry.second.second));
            data.array.push_back(std::move(item));
        }
        return envelope(true, std::move(data), "", "", {}, truncated);
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
        std::size_t timeout = 10000;
        if (!get_string(args, "command", command, true, validation_error) || !get_string(args, "cwd", cwd, false, validation_error) ||
            !get_size(args, "timeout_ms", 10000, 10000, timeout, validation_error) || timeout == 0)
            return tool_error_result("invalid_arguments", validation_error.empty() ? "timeout_ms must be positive" : validation_error);
        std::vector<std::string> parsed_arguments;
        Error policy_error = parse_inspection_command(command, parsed_arguments);
        if (!policy_error.ok()) return tool_error_result(error_code_string(policy_error.code), policy_error.message);
        policy_error = validate_command_workspace_paths(snapshot_, parsed_arguments, cwd);
        if (!policy_error.ok()) return tool_error_result("policy_denied", policy_error.message);
        ProcessOptions options; options.workspace = snapshot_.workspace; options.cwd = cwd; options.timeout_ms = static_cast<long>(timeout); options.cancellation = cancellation;
        ProcessResult process; const Error error = run_inspection_command(command, options, process);
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
                                                    parsed_arguments, output_filtered);
        }
        json::Value data = object_value(); json::Value arguments = array_value();
        for (const std::string& argument : process.arguments)
            arguments.array.push_back(string_value(redact_secrets(argument, secrets_)));
        data.object["arguments"] = std::move(arguments); data.object["cwd"] = string_value(process.cwd);
        data.object["exit_status"] = number_value(process.exit_status); data.object["signal"] = number_value(process.signal);
        data.object["duration_ms"] = number_value(process.duration_ms); data.object["stdout"] = string_value(redact_secrets(process.stdout_text, secrets_));
        data.object["stderr"] = string_value(redact_secrets(process.stderr_text, secrets_)); data.object["stdout_truncated"] = bool_value(process.stdout_truncated);
        data.object["stderr_truncated"] = bool_value(process.stderr_truncated); data.object["policy"] = string_value(process.policy);
        std::vector<std::string> warnings;
        if (output_filtered) warnings.push_back("output referring to non-indexed paths was omitted");
        if (!error.ok()) return envelope(false, std::move(data), error_code_string(error.code), error.message, warnings, process.stdout_truncated || process.stderr_truncated);
        return envelope(true, std::move(data), "", "", warnings, process.stdout_truncated || process.stderr_truncated);
    }

    return tool_error_result("unknown_tool", "unknown native read tool: " + requested_name);
}

}  // namespace ainiux::agent
