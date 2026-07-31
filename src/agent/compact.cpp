#include "agent/compact.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <utility>

#include "config/model_catalog.hpp"
#include "json/json.hpp"

namespace ainiux::agent {
namespace {

constexpr long long kSubstantiveItemTokens = 2000;
constexpr std::size_t kMinimumTailItems = 3;
constexpr std::size_t kMaximumTailItems = 20;
// Fraction of the context window retained as an unsummarized recent tail.
// Kept deliberately smaller than earlier 15% so large recent tool batches do
// not dominate post-compact request size once tool results are reduced.
constexpr int kTailBudgetPercent = 8;
constexpr std::size_t kFailedToolErrorBytes = 400;
constexpr std::size_t kStubSizeThresholdBytes = 1024;
constexpr std::size_t kStubExcerptBytes = 200;
constexpr std::size_t kSemanticOutputBytes = 1200;
constexpr std::size_t kReadMergeMaxItems = 100;
constexpr long long kSummaryOutputMax = 2000;
constexpr long long kSummaryOutputMin = 512;
constexpr long long kSummaryModelTimeoutMs = 30000;
constexpr std::size_t kKeepListMaxLines = 48;
constexpr std::size_t kKeepListLineBytes = 240;

bool model_projection_role(const std::string& role) {
    return role != "system" && role != "notice" && role != "thinking" &&
           role != "index";
}

long long item_tokens(const CompactionLogicalItem& item) {
    return estimate_tokens_from_text(item.role) +
           estimate_tokens_from_text(item.content) +
           estimate_tokens_from_text(item.tool_name) + 4;
}

std::string utf8_prefix(const std::string& text, std::size_t bytes) {
    if (text.size() <= bytes) return text;
    std::size_t end = std::min(bytes, text.size());
    while (end > 0 &&
           (static_cast<unsigned char>(text[end]) & 0xc0U) == 0x80U)
        --end;
    return text.substr(0, end);
}

std::string utf8_suffix(const std::string& text, std::size_t bytes) {
    if (text.size() <= bytes) return text;
    std::size_t start = text.size() - bytes;
    while (start < text.size() &&
           (static_cast<unsigned char>(text[start]) & 0xc0U) == 0x80U)
        ++start;
    return text.substr(start);
}

std::string bounded_extract(const std::string& text, std::size_t bytes,
                            bool& truncated) {
    if (text.size() <= bytes) return text;
    truncated = true;
    if (bytes < 8) return utf8_prefix(text, bytes);
    return utf8_prefix(text, bytes - 4) + " ...";
}

std::string single_line(std::string text) {
    for (char& c : text) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    }
    return ascii_trim(std::move(text));
}

std::string item_label(const CompactionLogicalItem& item) {
    if (item.role == "tool") {
        return "tool " + (item.tool_name.empty() ? std::string("activity")
                                                  : item.tool_name) +
               (item.tool_ok ? " [ok]" : " [failed]");
    }
    return item.role.empty() ? "message" : item.role;
}

void append_exact_item(std::ostringstream& out, const CompactionLogicalItem& item,
                       const char* section) {
    out << section << " [" << item_label(item) << "]\n" << item.content << "\n";
}

std::string bounded_error_from_result(const std::string& result_json) {
    std::string detail;
    json::ParseResult parsed = json::parse(result_json);
    if (parsed.error.ok() && parsed.value.is_object()) {
        if (const json::Value* err = parsed.value.get("error")) {
            if (err->is_object()) {
                if (const json::Value* msg = err->get("message");
                    msg != nullptr && msg->is_string())
                    detail = msg->string;
                else if (const json::Value* code = err->get("code");
                         code != nullptr && code->is_string())
                    detail = code->string;
            } else if (err->is_string()) {
                detail = err->string;
            }
        }
        if (detail.empty()) {
            if (const json::Value* message = parsed.value.get("message");
                message != nullptr && message->is_string())
                detail = message->string;
        }
    }
    if (detail.empty()) detail = single_line(result_json);
    bool truncated = false;
    return bounded_extract(single_line(std::move(detail)), kFailedToolErrorBytes,
                           truncated);
}

const json::Value* result_data_object(const json::Value& root) {
    if (!root.is_object()) return nullptr;
    const json::Value* data = root.get("data");
    if (data != nullptr && data->is_object()) return data;
    return &root;
}

std::string json_string_field(const json::Value& object, const char* key) {
    if (!object.is_object()) return {};
    const json::Value* value = object.get(key);
    if (value == nullptr || !value->is_string()) return {};
    return value->string;
}

bool json_number_field(const json::Value& object, const char* key, double& out) {
    if (!object.is_object()) return false;
    const json::Value* value = object.get(key);
    if (value == nullptr || value->type != json::Value::Type::Number) return false;
    out = value->number;
    return true;
}

std::string args_preview(const std::string& arguments_json, std::size_t max_bytes) {
    bool truncated = false;
    return bounded_extract(
        arguments_json.empty() ? std::string("{}") : single_line(arguments_json),
        max_bytes, truncated);
}

std::string extract_path_from_args(const std::string& arguments_json) {
    json::ParseResult parsed = json::parse(arguments_json);
    if (!parsed.error.ok() || !parsed.value.is_object()) return {};
    std::string path = json_string_field(parsed.value, "path");
    if (!path.empty()) return path;
    path = json_string_field(parsed.value, "source");
    if (!path.empty()) return path;
    path = json_string_field(parsed.value, "destination");
    return path;
}

std::string extract_hash_from_json(const json::Value& root) {
    const json::Value* data = result_data_object(root);
    if (data == nullptr) return {};
    std::string hash = json_string_field(*data, "file_hash");
    if (!hash.empty()) return hash;
    hash = json_string_field(*data, "new_file_hash");
    if (!hash.empty()) return hash;
    hash = json_string_field(*data, "content_hash");
    if (!hash.empty()) return hash;
    hash = json_string_field(*data, "range_hash");
    return hash;
}

void fill_path_and_hash_meta(const std::string& tool_name,
                             const std::string& arguments_json,
                             const std::string& result_json,
                             CompactionLogicalItem* meta) {
    if (meta == nullptr) return;
    meta->primary_path = extract_path_from_args(arguments_json);
    json::ParseResult parsed = json::parse(result_json);
    if (parsed.error.ok()) {
        if (meta->primary_path.empty()) {
            const json::Value* data = result_data_object(parsed.value);
            if (data != nullptr) {
                meta->primary_path = json_string_field(*data, "path");
                if (meta->primary_path.empty() && data->is_object()) {
                    // read_symbol style
                    meta->primary_path = json_string_field(*data, "path");
                }
            }
        }
        meta->content_hash = extract_hash_from_json(parsed.value);
        const json::Value* data = result_data_object(parsed.value);
        if (data != nullptr) {
            double exit = 0;
            if (json_number_field(*data, "exit_status", exit) ||
                json_number_field(*data, "exit_code", exit)) {
                meta->has_exit_status = true;
                meta->exit_status = static_cast<int>(exit);
            }
        }
    }
    (void)tool_name;
}

std::string format_path_range_from_args(const std::string& arguments_json) {
    json::ParseResult parsed = json::parse(arguments_json);
    if (!parsed.error.ok() || !parsed.value.is_object())
        return args_preview(arguments_json, 160);
    std::string path = json_string_field(parsed.value, "path");
    if (path.empty()) return args_preview(arguments_json, 160);
    double start = 0;
    double end = 0;
    const bool has_start = json_number_field(parsed.value, "start_line", start);
    const bool has_end = json_number_field(parsed.value, "end_line", end);
    if (has_start || has_end) {
        std::ostringstream out;
        out << path;
        if (has_start && has_end && end > 0)
            out << ":" << static_cast<long long>(start) << "-"
                << static_cast<long long>(end);
        else if (has_start)
            out << ":" << static_cast<long long>(start);
        return out.str();
    }
    return path;
}

std::string search_hit_paths(const std::string& result_json, std::size_t max_hits) {
    json::ParseResult parsed = json::parse(result_json);
    if (!parsed.error.ok()) return {};
    const json::Value* data = parsed.value.is_object() ? parsed.value.get("data")
                                                       : nullptr;
    const json::Value* hits = data;
    if (data != nullptr && data->is_object()) {
        if (const json::Value* results = data->get("results")) hits = results;
        else if (const json::Value* matches = data->get("matches")) hits = matches;
        else if (const json::Value* items = data->get("items")) hits = items;
    }
    if (hits == nullptr || !hits->is_array()) return {};
    std::ostringstream out;
    std::size_t count = 0;
    for (const json::Value& hit : hits->array) {
        if (count >= max_hits) break;
        std::string path;
        long long line = -1;
        if (hit.is_object()) {
            path = json_string_field(hit, "path");
            if (path.empty()) path = json_string_field(hit, "file");
            double line_num = 0;
            if (json_number_field(hit, "line", line_num) ||
                json_number_field(hit, "line_number", line_num) ||
                json_number_field(hit, "start_line", line_num))
                line = static_cast<long long>(line_num);
        } else if (hit.is_string()) {
            path = hit.string;
        }
        if (path.empty()) continue;
        if (count > 0) out << ", ";
        out << path;
        if (line > 0) out << ":" << line;
        ++count;
    }
    if (count == 0) return {};
    if (hits->array.size() > count)
        out << " (+" << (hits->array.size() - count) << " more)";
    return out.str();
}

std::string symbol_location_hint(const std::string& arguments_json,
                                 const std::string& result_json) {
    json::ParseResult args = json::parse(arguments_json);
    json::ParseResult result = json::parse(result_json);
    std::ostringstream out;
    if (args.error.ok() && args.value.is_object()) {
        double symbol_id = 0;
        if (json_number_field(args.value, "symbol_id", symbol_id))
            out << "symbol_id=" << static_cast<long long>(symbol_id);
        std::string path = json_string_field(args.value, "path");
        if (!path.empty()) {
            if (out.tellp() > 0) out << " ";
            out << path;
        }
        std::string query = json_string_field(args.value, "query");
        if (!query.empty()) {
            if (out.tellp() > 0) out << " ";
            bool truncated = false;
            out << "q=" << bounded_extract(single_line(query), 80, truncated);
        }
    }
    if (result.error.ok()) {
        const json::Value* data = result_data_object(result.value);
        if (data != nullptr) {
            std::string path = json_string_field(*data, "path");
            double start = 0;
            double end = 0;
            const bool has_start = json_number_field(*data, "line_start", start) ||
                                   json_number_field(*data, "start_line", start);
            const bool has_end = json_number_field(*data, "line_end", end) ||
                                 json_number_field(*data, "end_line", end);
            if (!path.empty()) {
                if (out.tellp() > 0) out << " ";
                out << path;
                if (has_start) {
                    out << ":" << static_cast<long long>(start);
                    if (has_end && end > 0)
                        out << "-" << static_cast<long long>(end);
                }
            }
        }
    }
    std::string text = out.str();
    if (text.empty()) text = args_preview(arguments_json, 160);
    return text;
}

std::string head_tail_excerpt(const std::string& text) {
    if (text.size() <= kStubExcerptBytes * 2 + 32) return single_line(text);
    return single_line(utf8_prefix(text, kStubExcerptBytes)) + " … " +
           single_line(utf8_suffix(text, kStubExcerptBytes));
}

bool looks_like_failure_line(const std::string& line) {
    const std::string lower = ascii_lower(line);
    return lower.find("error:") != std::string::npos ||
           lower.find("error ") != std::string::npos ||
           lower.find("failed") != std::string::npos ||
           lower.find("failure") != std::string::npos ||
           lower.find("undefined reference") != std::string::npos ||
           lower.find("fatal:") != std::string::npos ||
           lower.find("assert") != std::string::npos ||
           lower.find("failing") != std::string::npos ||
           lower.find("not found") != std::string::npos ||
           lower.find("no such file") != std::string::npos ||
           lower.find("segmentation fault") != std::string::npos ||
           lower.find("tracebacks") != std::string::npos ||
           lower.find("traceback") != std::string::npos ||
           lower.find("exception") != std::string::npos ||
           (lower.find("FAIL") != std::string::npos);
}

std::string extract_semantic_output(const std::string& result_json, bool ok) {
    json::ParseResult parsed = json::parse(result_json);
    std::string stdout_text;
    std::string stderr_text;
    int exit_status = ok ? 0 : 1;
    bool has_exit = false;
    if (parsed.error.ok()) {
        const json::Value* data = result_data_object(parsed.value);
        if (data != nullptr) {
            stdout_text = json_string_field(*data, "stdout");
            stderr_text = json_string_field(*data, "stderr");
            double exit = 0;
            if (json_number_field(*data, "exit_status", exit) ||
                json_number_field(*data, "exit_code", exit)) {
                has_exit = true;
                exit_status = static_cast<int>(exit);
            }
        }
    }
    if (stdout_text.empty() && stderr_text.empty()) {
        if (!ok) return bounded_error_from_result(result_json);
        bool truncated = false;
        return bounded_extract(single_line(result_json), kSemanticOutputBytes,
                               truncated);
    }

    std::ostringstream kept;
    auto absorb = [&](const std::string& blob) {
        std::istringstream stream(blob);
        std::string line;
        while (std::getline(stream, line)) {
            if (line.empty()) continue;
            if (looks_like_failure_line(line) || !ok ||
                (has_exit && exit_status != 0)) {
                if (kept.tellp() > 0) kept << " | ";
                bool truncated = false;
                kept << bounded_extract(single_line(line), 160, truncated);
                if (static_cast<std::size_t>(kept.tellp()) >= kSemanticOutputBytes)
                    break;
            }
        }
    };
    absorb(stderr_text);
    if (static_cast<std::size_t>(kept.tellp()) < kSemanticOutputBytes / 2)
        absorb(stdout_text);
    if (kept.tellp() == 0) {
        // Passing command: keep a short head of stdout only.
        bool truncated = false;
        return bounded_extract(single_line(stdout_text.empty() ? stderr_text
                                                               : stdout_text),
                               240, truncated);
    }
    bool truncated = false;
    return bounded_extract(kept.str(), kSemanticOutputBytes, truncated);
}

bool command_looks_irreversible_git(const std::string& arguments_json) {
    const std::string lower = ascii_lower(arguments_json);
    return lower.find("commit") != std::string::npos ||
           lower.find("merge") != std::string::npos ||
           lower.find("push") != std::string::npos ||
           lower.find("rebase") != std::string::npos ||
           lower.find("\"tag\"") != std::string::npos ||
           lower.find(" reset") != std::string::npos ||
           lower.find("checkout") != std::string::npos ||
           lower.find("switch") != std::string::npos ||
           lower.find("branch") != std::string::npos;
}

std::string reduce_prune(const std::string& tool_name,
                         const std::string& arguments_json, bool ok) {
    std::ostringstream content;
    content << tool_name << "(" << args_preview(arguments_json, 120) << ") -> "
            << (ok ? "ok" : "fail");
    return content.str();
}

std::string reduce_stub(const std::string& tool_name,
                        const std::string& arguments_json,
                        const std::string& result_json, bool ok) {
    std::ostringstream content;
    content << "Tool: " << (tool_name.empty() ? "unknown" : tool_name) << "\n";
    if (tool_name == "read_file" || tool_name == "read_many") {
        content << "Arguments: "
                << (arguments_json.empty() ? std::string("{}") : arguments_json)
                << "\n";
    } else if (tool_name == "read_symbol" || tool_name == "get_skeleton" ||
               tool_name == "search_symbol") {
        content << "Target: " << symbol_location_hint(arguments_json, result_json)
                << "\n";
    } else if (tool_name == "search_text" || tool_name == "find_tests" ||
               tool_name == "inspect_code_task") {
        content << "Arguments: " << args_preview(arguments_json, 200) << "\n";
        const std::string hits = search_hit_paths(result_json, 12);
        if (!hits.empty()) content << "Hits: " << hits << "\n";
    } else {
        content << "Arguments: " << args_preview(arguments_json, 200) << "\n";
    }
    if (ok) {
        content << "Result: omitted (reloadable; re-run to reload)\n"
                   "Status: ok";
    } else {
        content << "Result: omitted\n"
                   "Status: failed\n"
                   "Error: "
                << bounded_error_from_result(result_json);
    }
    return content.str();
}

std::string reduce_digest(const std::string& tool_name,
                          const std::string& arguments_json,
                          const std::string& result_json, bool ok) {
    std::string path = extract_path_from_args(arguments_json);
    json::ParseResult args = json::parse(arguments_json);
    json::ParseResult result = json::parse(result_json);
    if (path.empty() && result.error.ok()) {
        const json::Value* data = result_data_object(result.value);
        if (data != nullptr) path = json_string_field(*data, "path");
    }
    std::string dest;
    if (args.error.ok() && args.value.is_object()) {
        if (path.empty()) path = json_string_field(args.value, "source");
        dest = json_string_field(args.value, "destination");
    }
    std::ostringstream content;
    content << "Tool: " << tool_name << "\n";
    if (!path.empty() && !dest.empty())
        content << "Path: " << path << " -> " << dest << "\n";
    else if (!path.empty())
        content << "Path: " << path << "\n";
    else
        content << "Arguments: " << args_preview(arguments_json, 160) << "\n";
    if (tool_name == "edit_file" && args.error.ok() && args.value.is_object()) {
        if (const json::Value* ops = args.value.get("ops");
            ops != nullptr && ops->is_array())
            content << "Ops: " << ops->array.size() << "\n";
    }
    if (tool_name == "str_replace") content << "Op: str_replace\n";
    if (tool_name == "write_file") content << "Op: write\n";
    if (tool_name == "remove") content << "Op: remove\n";
    if (tool_name == "create_directory") content << "Op: mkdir\n";
    if (tool_name == "rename_path") content << "Op: rename\n";
    if (tool_name == "apply_patch") content << "Op: apply_patch\n";
    if (result.error.ok()) {
        const json::Value* data = result_data_object(result.value);
        if (data != nullptr) {
            std::string hash = json_string_field(*data, "new_file_hash");
            if (hash.empty()) hash = json_string_field(*data, "file_hash");
            if (!hash.empty()) {
                bool truncated = false;
                content << "Hash: "
                        << bounded_extract(hash, 20, truncated) << "\n";
            }
        }
    }
    content << "Status: " << (ok ? "ok" : "failed");
    if (!ok) content << "\nError: " << bounded_error_from_result(result_json);
    return content.str();
}

std::string reduce_semantic(const std::string& tool_name,
                            const std::string& arguments_json,
                            const std::string& result_json, bool ok) {
    std::ostringstream content;
    content << "Tool: " << tool_name << "\nArguments: "
            << args_preview(arguments_json, 200) << "\n";
    json::ParseResult parsed = json::parse(result_json);
    if (parsed.error.ok()) {
        const json::Value* data = result_data_object(parsed.value);
        if (data != nullptr) {
            double exit = 0;
            if (json_number_field(*data, "exit_status", exit) ||
                json_number_field(*data, "exit_code", exit))
                content << "Exit: " << static_cast<int>(exit) << "\n";
        }
    }
    content << "Result: " << extract_semantic_output(result_json, ok) << "\n";
    content << "Status: " << (ok ? "ok" : "failed");
    if (!ok && content.str().find("Error:") == std::string::npos)
        content << "\nError: " << bounded_error_from_result(result_json);
    if (tool_name == "run_command" && command_looks_irreversible_git(arguments_json))
        content << "\nNote: git-like action (retain in checkpoint)";
    return content.str();
}

std::string reduce_full_or_size(const std::string& tool_name,
                                const std::string& arguments_json,
                                const std::string& result_json, bool ok) {
    if (result_json.size() <= kStubSizeThresholdBytes) {
        std::ostringstream content;
        content << "Tool: " << (tool_name.empty() ? "unknown" : tool_name)
                << "\nArguments: "
                << (arguments_json.empty() ? std::string("{}") : arguments_json)
                << "\nResult: " << result_json;
        if (!ok) content << "\nStatus: failed";
        return content.str();
    }
    std::ostringstream content;
    content << "Tool: " << (tool_name.empty() ? "unknown" : tool_name)
            << "\nArguments: " << args_preview(arguments_json, 200)
            << "\nResult: " << head_tail_excerpt(result_json)
            << "\n(re-run to reload full output)\nStatus: "
            << (ok ? "ok" : "failed");
    if (!ok) content << "\nError: " << bounded_error_from_result(result_json);
    return content.str();
}

bool is_read_tool(const std::string& name) {
    return name == "read_file" || name == "read_many";
}

bool is_explore_tool(const std::string& name) {
    return name == "search_text" || name == "list_directory" || name == "glob" ||
           name == "project_overview" || name == "index_status";
}

bool is_digest_tool(const std::string& name) {
    return tool_compaction_tier(name) == ToolCompactionTier::Digest;
}

void recompute_item_tokens(CompactionLogicalItem& item) {
    item.estimated_tokens = item_tokens(item);
}

std::string keep_line_from_item(const CompactionLogicalItem& item) {
    bool truncated = false;
    if (item.role == "user") {
        return "user: " +
               bounded_extract(single_line(item.content), kKeepListLineBytes,
                               truncated);
    }
    if (item.role == "tool") {
        if (is_digest_tool(item.tool_name)) {
            std::string path = item.primary_path;
            if (path.empty()) path = "path?";
            return item.tool_name + " " + path +
                   (item.tool_ok ? " ok" : " failed");
        }
        if (item.tool_name == "run_command" || item.tool_name == "git_status" ||
            item.tool_name == "git_diff") {
            std::ostringstream line;
            line << item.tool_name;
            if (item.has_exit_status) line << " exit=" << item.exit_status;
            line << (item.tool_ok ? " ok" : " failed");
            // Pull a short failure excerpt from content when present.
            const std::string lowered = ascii_lower(item.content);
            if (!item.tool_ok ||
                (item.has_exit_status && item.exit_status != 0) ||
                lowered.find("error") != std::string::npos ||
                lowered.find("fail") != std::string::npos) {
                bool t = false;
                line << " | "
                     << bounded_extract(single_line(item.content), 160, t);
            } else if (item.content.find("git-like action") != std::string::npos) {
                bool t = false;
                line << " | "
                     << bounded_extract(single_line(item.content), 160, t);
            }
            return line.str();
        }
        if (!item.tool_ok) {
            return item.tool_name + " failed: " +
                   bounded_extract(single_line(item.content), 160, truncated);
        }
    }
    return {};
}

}  // namespace

const char* compaction_strategy_name(CompactionStrategy strategy) {
    switch (strategy) {
        case CompactionStrategy::Fast:
            return "fast";
        case CompactionStrategy::Smart:
            return "smart";
        case CompactionStrategy::Summary:
            return "summary";
    }
    return "smart";
}

bool parse_compaction_strategy(const std::string& text,
                               CompactionStrategy& strategy) {
    const std::string value = ascii_lower(ascii_trim(text));
    if (value == "fast") strategy = CompactionStrategy::Fast;
    else if (value == "smart") strategy = CompactionStrategy::Smart;
    else if (value == "summary") strategy = CompactionStrategy::Summary;
    else return false;
    return true;
}

int effective_compact_limit_percent(int configured_limit,
                                    long long /*context_window_tokens*/) {
    if (configured_limit >= 1 && configured_limit <= 100) return configured_limit;
    return 75;
}

bool should_auto_compact(bool auto_compact_enabled,
                         int compact_limit_percent,
                         long long context_window_tokens,
                         long long estimated_request_tokens) {
    if (!auto_compact_enabled) return false;
    if (context_window_tokens <= 0 || estimated_request_tokens <= 0) return false;
    const int limit =
        effective_compact_limit_percent(compact_limit_percent, context_window_tokens);
    const long long threshold =
        (context_window_tokens * static_cast<long long>(limit) + 99) / 100;
    return estimated_request_tokens >= threshold;
}

long long estimate_tokens_from_text(const std::string& text) {
    if (text.empty()) return 0;
    return static_cast<long long>((text.size() + 3) / 4);
}

long long estimate_transcript_tokens(
    const std::vector<AgentMessageRecord>& messages) {
    long long total = 0;
    for (const AgentMessageRecord& message : messages) {
        if (!model_projection_role(message.role)) continue;
        total += estimate_tokens_from_text(message.role);
        total += estimate_tokens_from_text(message.content);
        total += estimate_tokens_from_text(message.tool_name);
        total += estimate_tokens_from_text(message.args_preview);
        total += 4;
    }
    return total;
}

ToolCompactionTier tool_compaction_tier(const std::string& tool_name) {
    if (tool_name == "index_status" || tool_name == "index_update" ||
        tool_name == "index_rebuild" || tool_name == "list_directory" ||
        tool_name == "glob" || tool_name == "project_overview")
        return ToolCompactionTier::Prune;
    if (tool_name == "read_file" || tool_name == "read_many" ||
        tool_name == "read_symbol" || tool_name == "get_skeleton" ||
        tool_name == "search_symbol" || tool_name == "search_text" ||
        tool_name == "grep" || tool_name == "find" ||
        tool_name == "find_tests" || tool_name == "inspect_code_task" ||
        tool_name == "fetch_url" || tool_name == "search_web")
        return ToolCompactionTier::Stub;
    if (tool_name == "edit_file" || tool_name == "write_file" ||
        tool_name == "str_replace" || tool_name == "apply_patch" ||
        tool_name == "rename_path" || tool_name == "remove" ||
        tool_name == "create_directory")
        return ToolCompactionTier::Digest;
    if (tool_name == "git_status" || tool_name == "git_diff" ||
        tool_name == "run_command")
        return ToolCompactionTier::Semantic;
    if (tool_name == "goal_met" || tool_name == "attach_image")
        return ToolCompactionTier::Digest;
    return ToolCompactionTier::Full;
}

bool is_reloadable_file_read_tool(const std::string& tool_name) {
    // Broader than the original read_file/read_many pair: any Stub-tier tool
    // is reloadable from workspace, index, or network.
    return tool_compaction_tier(tool_name) == ToolCompactionTier::Stub;
}

std::string reduce_tool_item_content(const std::string& tool_name,
                                     const std::string& arguments_json,
                                     const std::string& result_json,
                                     bool ok,
                                     CompactionLogicalItem* meta) {
    fill_path_and_hash_meta(tool_name, arguments_json, result_json, meta);
    switch (tool_compaction_tier(tool_name)) {
        case ToolCompactionTier::Prune:
            return reduce_prune(tool_name, arguments_json, ok);
        case ToolCompactionTier::Stub:
            return reduce_stub(tool_name, arguments_json, result_json, ok);
        case ToolCompactionTier::Digest:
            return reduce_digest(tool_name, arguments_json, result_json, ok);
        case ToolCompactionTier::Semantic:
            return reduce_semantic(tool_name, arguments_json, result_json, ok);
        case ToolCompactionTier::Full:
            return reduce_full_or_size(tool_name, arguments_json, result_json,
                                       ok);
    }
    return reduce_full_or_size(tool_name, arguments_json, result_json, ok);
}

std::string stub_reloadable_tool_item_content(const std::string& tool_name,
                                              const std::string& arguments_json,
                                              const std::string& result_json,
                                              bool ok) {
    return reduce_tool_item_content(tool_name, arguments_json, result_json, ok,
                                    nullptr);
}

std::vector<CompactionLogicalItem> build_compaction_timeline(
    const std::vector<AgentMessageRecord>& messages,
    const std::vector<AgentToolEventRecord>& tool_events) {
    std::vector<CompactionLogicalItem> timeline;
    timeline.reserve(messages.size() + tool_events.size());

    std::map<std::string, std::size_t> duplicate_tool_rows;
    for (const AgentToolEventRecord& event : tool_events)
        ++duplicate_tool_rows[event.tool_name];

    std::vector<bool> skip_message(messages.size(), false);
    // Full events precede their compact display rows. Consume matching rows
    // newest-first so an older display-only row is not erased merely because a
    // later event reused the same tool name.
    for (std::size_t i = messages.size(); i > 0; --i) {
        const AgentMessageRecord& message = messages[i - 1];
        if (message.role != "tool" || message.tool_name.empty()) continue;
        auto found = duplicate_tool_rows.find(message.tool_name);
        if (found != duplicate_tool_rows.end() && found->second > 0) {
            skip_message[i - 1] = true;
            --found->second;
        }
    }

    for (std::size_t i = 0; i < messages.size(); ++i) {
        const AgentMessageRecord& message = messages[i];
        if (!model_projection_role(message.role)) continue;
        if (skip_message[i]) continue;
        CompactionLogicalItem item;
        item.seq = message.seq;
        item.role = message.role;
        item.tool_name = message.tool_name;
        item.tool_ok = message.tool_ok;
        // Display tool rows are already one-line previews. If a large tool
        // body somehow lands here without a matching tool_event, still reduce.
        if (message.role == "tool" && !message.tool_name.empty() &&
            message.content.size() > 512) {
            item.content = reduce_tool_item_content(
                message.tool_name,
                message.args_preview.empty() ? "{}" : message.args_preview,
                message.content, message.tool_ok, &item);
        } else {
            item.content = message.content;
            if (message.role == "tool")
                item.primary_path = extract_path_from_args(
                    message.args_preview.empty() ? "{}" : message.args_preview);
        }
        item.estimated_tokens = item_tokens(item);
        timeline.push_back(std::move(item));
    }
    for (const AgentToolEventRecord& event : tool_events) {
        CompactionLogicalItem item;
        item.seq = event.seq;
        item.role = "tool";
        item.tool_name = event.tool_name;
        item.tool_ok = event.ok;
        item.content = reduce_tool_item_content(event.tool_name, event.arguments,
                                                event.result, event.ok, &item);
        item.estimated_tokens = item_tokens(item);
        timeline.push_back(std::move(item));
    }
    std::stable_sort(timeline.begin(), timeline.end(),
                     [](const CompactionLogicalItem& left,
                        const CompactionLogicalItem& right) {
                         return left.seq < right.seq;
                     });
    return timeline;
}

CompactionPartition partition_compaction_timeline(
    const std::vector<CompactionLogicalItem>& timeline,
    long long context_window_tokens) {
    CompactionPartition result;
    std::size_t newest_summary = timeline.size();
    for (std::size_t i = 0; i < timeline.size(); ++i) {
        result.source_tokens += timeline[i].estimated_tokens;
        if (timeline[i].role == "summary") newest_summary = i;
    }

    std::size_t begin = 0;
    if (newest_summary < timeline.size()) {
        result.prior_summary = timeline[newest_summary].content;
        begin = newest_summary + 1;
    } else {
        for (std::size_t i = 0; i < timeline.size() && result.head.size() < 3; ++i) {
            if (timeline[i].role != "summary") result.head.push_back(timeline[i]);
        }
        begin = result.head.size();
    }

    const long long fallback_window =
        std::max<long long>(result.source_tokens, 8000);
    result.tail_budget_tokens =
        std::max<long long>(1, (context_window_tokens > 0 ? context_window_tokens
                                                          : fallback_window) *
                                   kTailBudgetPercent / 100);

    std::size_t tail_begin = timeline.size();
    long long tail_tokens = 0;
    std::size_t tail_items = 0;
    while (tail_begin > begin && tail_items < kMaximumTailItems) {
        const CompactionLogicalItem& candidate = timeline[tail_begin - 1];
        if (candidate.role == "summary") break;
        const bool needs_minimum = tail_items < kMinimumTailItems;
        if (!needs_minimum &&
            tail_tokens + candidate.estimated_tokens > result.tail_budget_tokens)
            break;
        --tail_begin;
        ++tail_items;
        tail_tokens += candidate.estimated_tokens;
    }
    for (std::size_t i = begin; i < tail_begin; ++i) {
        if (timeline[i].role != "summary") result.middle.push_back(timeline[i]);
    }
    for (std::size_t i = tail_begin; i < timeline.size(); ++i) {
        if (timeline[i].role != "summary") result.tail.push_back(timeline[i]);
    }
    return result;
}

void pre_shrink_compaction_middle(std::vector<CompactionLogicalItem>& middle) {
    if (middle.size() < 2) return;

    // 1) Merge consecutive read_file into a synthetic read_many stub.
    {
        std::vector<CompactionLogicalItem> merged;
        merged.reserve(middle.size());
        for (std::size_t i = 0; i < middle.size();) {
            if (middle[i].role == "tool" && middle[i].tool_name == "read_file") {
                std::vector<std::string> paths;
                long long first_seq = middle[i].seq;
                bool all_ok = true;
                std::size_t j = i;
                while (j < middle.size() && middle[j].role == "tool" &&
                       middle[j].tool_name == "read_file" &&
                       paths.size() < kReadMergeMaxItems) {
                    std::string key = middle[j].primary_path;
                    if (key.empty())
                        key = format_path_range_from_args(
                            // recover from content Arguments if needed
                            [&]() -> std::string {
                                const std::string& c = middle[j].content;
                                const std::string marker = "Arguments: ";
                                const auto pos = c.find(marker);
                                if (pos == std::string::npos) return "{}";
                                const auto end = c.find('\n', pos);
                                return c.substr(
                                    pos + marker.size(),
                                    end == std::string::npos
                                        ? std::string::npos
                                        : end - (pos + marker.size()));
                            }());
                    if (key.empty()) key = "path?";
                    paths.push_back(std::move(key));
                    all_ok = all_ok && middle[j].tool_ok;
                    ++j;
                }
                if (paths.size() >= 2) {
                    CompactionLogicalItem item;
                    item.seq = first_seq;
                    item.role = "tool";
                    item.tool_name = "read_many";
                    item.tool_ok = all_ok;
                    std::ostringstream content;
                    content << "Tool: read_many\nArguments: [merged "
                            << paths.size() << " reads]\nPaths: ";
                    for (std::size_t p = 0; p < paths.size(); ++p) {
                        if (p) content << ", ";
                        content << paths[p];
                    }
                    content << "\nResult: omitted (reloadable; re-run to reload)\n"
                               "Status: "
                            << (all_ok ? "ok" : "failed");
                    item.content = content.str();
                    if (!paths.empty()) item.primary_path = paths.front();
                    recompute_item_tokens(item);
                    merged.push_back(std::move(item));
                    i = j;
                    continue;
                }
            }
            merged.push_back(std::move(middle[i]));
            ++i;
        }
        middle = std::move(merged);
    }

    // 2) Collapse consecutive explore tools into one exploration line.
    {
        std::vector<CompactionLogicalItem> merged;
        merged.reserve(middle.size());
        for (std::size_t i = 0; i < middle.size();) {
            if (middle[i].role == "tool" && is_explore_tool(middle[i].tool_name)) {
                std::set<std::string> labels;
                long long first_seq = middle[i].seq;
                bool all_ok = true;
                std::size_t j = i;
                while (j < middle.size() && middle[j].role == "tool" &&
                       is_explore_tool(middle[j].tool_name)) {
                    std::string label = middle[j].tool_name;
                    if (!middle[j].primary_path.empty())
                        label += ":" + middle[j].primary_path;
                    else {
                        bool truncated = false;
                        label +=
                            "(" +
                            bounded_extract(single_line(middle[j].content), 40,
                                            truncated) +
                            ")";
                    }
                    labels.insert(std::move(label));
                    all_ok = all_ok && middle[j].tool_ok;
                    ++j;
                }
                if (j - i >= 2) {
                    CompactionLogicalItem item;
                    item.seq = first_seq;
                    item.role = "tool";
                    item.tool_name = "explored";
                    item.tool_ok = all_ok;
                    std::ostringstream content;
                    content << "explored: ";
                    bool first = true;
                    for (const std::string& label : labels) {
                        if (!first) content << ", ";
                        first = false;
                        content << label;
                    }
                    content << " -> " << (all_ok ? "ok" : "fail");
                    item.content = content.str();
                    recompute_item_tokens(item);
                    merged.push_back(std::move(item));
                    i = j;
                    continue;
                }
            }
            merged.push_back(std::move(middle[i]));
            ++i;
        }
        middle = std::move(merged);
    }

    // 3) Drop a single read_file immediately followed by a digest mutation on
    // the same path (the edit digest already names the file). Do not drop
    // read_many batches — other paths in the batch remain useful.
    {
        std::vector<CompactionLogicalItem> filtered;
        filtered.reserve(middle.size());
        for (std::size_t i = 0; i < middle.size(); ++i) {
            if (i + 1 < middle.size() && middle[i].role == "tool" &&
                middle[i].tool_name == "read_file" &&
                middle[i + 1].role == "tool" &&
                is_digest_tool(middle[i + 1].tool_name)) {
                const std::string& read_path = middle[i].primary_path;
                const std::string& edit_path = middle[i + 1].primary_path;
                if (!read_path.empty() && read_path == edit_path) continue;
            }
            filtered.push_back(std::move(middle[i]));
        }
        middle = std::move(filtered);
    }

    // 4) Hash-based dedupe of identical re-reads (keep newest, annotate ×N).
    {
        std::map<std::string, std::size_t> last_index;
        std::vector<int> counts(middle.size(), 1);
        std::vector<bool> drop(middle.size(), false);
        for (std::size_t i = 0; i < middle.size(); ++i) {
            if (middle[i].role != "tool" || middle[i].content_hash.empty())
                continue;
            if (!is_read_tool(middle[i].tool_name) &&
                tool_compaction_tier(middle[i].tool_name) !=
                    ToolCompactionTier::Stub)
                continue;
            const std::string key =
                middle[i].tool_name + "|" + middle[i].primary_path + "|" +
                middle[i].content_hash;
            auto found = last_index.find(key);
            if (found != last_index.end()) {
                drop[found->second] = true;
                counts[i] += counts[found->second];
            }
            last_index[key] = i;
        }
        std::vector<CompactionLogicalItem> deduped;
        deduped.reserve(middle.size());
        for (std::size_t i = 0; i < middle.size(); ++i) {
            if (drop[i]) continue;
            if (counts[i] > 1) {
                middle[i].content += "\n(re-read ×" + std::to_string(counts[i]) + ")";
                recompute_item_tokens(middle[i]);
            }
            deduped.push_back(std::move(middle[i]));
        }
        middle = std::move(deduped);
    }

    // 5) Collapse empty/short tool-only assistant chatter.
    for (CompactionLogicalItem& item : middle) {
        if (item.role != "assistant") continue;
        const std::string trimmed = ascii_trim(item.content);
        if (trimmed.empty() || trimmed.size() < 8) {
            item.content = "assistant -> (tool-only turn)";
            recompute_item_tokens(item);
        }
    }
}

CompactionKeepList harvest_compaction_keep_list(
    const std::vector<CompactionLogicalItem>& middle) {
    CompactionKeepList keep;
    std::size_t user_count = 0;
    int last_command_exit = 0;
    bool have_last_command = false;
    for (const CompactionLogicalItem& item : middle) {
        if (keep.lines.size() >= kKeepListMaxLines) break;
        if (item.role == "user" && user_count < 6) {
            std::string line = keep_line_from_item(item);
            if (!line.empty()) {
                keep.lines.push_back(std::move(line));
                ++user_count;
            }
            continue;
        }
        if (item.role != "tool") continue;
        if (is_digest_tool(item.tool_name)) {
            std::string line = keep_line_from_item(item);
            if (!line.empty()) keep.lines.push_back(std::move(line));
            continue;
        }
        if (item.tool_name == "run_command" || item.tool_name == "git_status" ||
            item.tool_name == "git_diff") {
            const bool failed =
                !item.tool_ok ||
                (item.has_exit_status && item.exit_status != 0);
            const bool git_action =
                item.content.find("git-like action") != std::string::npos ||
                item.tool_name == "git_status" || item.tool_name == "git_diff";
            if (have_last_command && item.tool_name == "run_command" &&
                item.has_exit_status && last_command_exit != 0 &&
                item.exit_status == 0) {
                keep.lines.push_back("run_command transition: fail -> pass");
            }
            if (failed || git_action) {
                std::string line = keep_line_from_item(item);
                if (!line.empty()) keep.lines.push_back(std::move(line));
            }
            if (item.tool_name == "run_command" && item.has_exit_status) {
                last_command_exit = item.exit_status;
                have_last_command = true;
            }
            continue;
        }
        if (!item.tool_ok) {
            std::string line = keep_line_from_item(item);
            if (!line.empty()) keep.lines.push_back(std::move(line));
        }
    }
    return keep;
}

FastCompactionCandidate build_fast_compaction_candidate(
    const CompactionPartition& partition,
    long long max_checkpoint_tokens,
    const CompactionKeepList& keep_list) {
    FastCompactionCandidate result;
    const std::size_t max_bytes = static_cast<std::size_t>(
        std::max<long long>(256, max_checkpoint_tokens > 0
                                      ? max_checkpoint_tokens * 4
                                      : 4096));
    std::ostringstream out;
    out << "Active Task\n";
    bool truncated = false;
    if (!partition.prior_summary.empty()) {
        out << bounded_extract(partition.prior_summary,
                               std::min<std::size_t>(max_bytes / 3, 6000),
                               truncated)
            << "\n";
    }
    out << "Goal\n";
    std::size_t user_count = 0;
    std::set<std::string> paths;
    std::size_t tools = 0;
    std::size_t failed = 0;
    long long omitted = 0;
    for (const CompactionLogicalItem& item : partition.middle) {
        if (item.role == "user" && user_count < 4) {
            bool item_truncated = false;
            const std::string extract =
                bounded_extract(single_line(item.content), 480, item_truncated);
            out << "- " << extract << "\n";
            truncated = truncated || item_truncated;
            if (item_truncated)
                omitted += std::max<long long>(
                    0, item.estimated_tokens - estimate_tokens_from_text(extract));
            ++user_count;
        } else if (item.role == "tool") {
            ++tools;
            if (!item.tool_ok) ++failed;
            if (!item.primary_path.empty() && paths.size() < 24)
                paths.insert(item.primary_path);
            const std::string text = item.content;
            std::size_t pos = 0;
            while ((pos = text.find('/', pos)) != std::string::npos) {
                std::size_t end = text.find_first_of(" \t\r\n\"')},]", pos);
                paths.insert(text.substr(pos, end == std::string::npos ? 160
                                                                      : end - pos));
                pos += 1;
                if (paths.size() >= 24) break;
            }
            // Tools are already reduced; count residual size beyond a one-liner.
            omitted += std::max<long long>(0, item.estimated_tokens - 12);
        } else {
            omitted += item.estimated_tokens;
        }
        if (item.estimated_tokens >= kSubstantiveItemTokens)
            result.omitted_item_at_least_2k = true;
    }
    out << "Active State\n- Middle activity: " << partition.middle.size()
        << " logical items; tools=" << tools << ", failed=" << failed << "\n";
    if (!paths.empty()) {
        out << "Relevant Files/Evidence\n";
        for (const std::string& path : paths) out << "- " << path << "\n";
    }
    out << "Decisions / Completed Work / Blockers / Remaining Work\n";
    if (!keep_list.lines.empty()) {
        std::size_t emitted = 0;
        for (const std::string& line : keep_list.lines) {
            if (emitted >= 24) break;
            out << "- " << line << "\n";
            ++emitted;
        }
    } else {
        out << "- Deterministic checkpoint: verify current source and tool state "
               "before acting.\n";
    }

    std::string checkpoint = out.str();
    if (checkpoint.size() > max_bytes) {
        checkpoint = bounded_extract(checkpoint, max_bytes, truncated);
    }
    result.checkpoint = std::move(checkpoint);
    result.estimated_tokens = estimate_tokens_from_text(result.checkpoint);
    for (const CompactionLogicalItem& item : partition.head)
        result.estimated_tokens += item.estimated_tokens;
    for (const CompactionLogicalItem& item : partition.tail)
        result.estimated_tokens += item.estimated_tokens;
    result.protected_content_truncated = truncated;
    result.omitted_substantive_tokens = omitted;
    return result;
}

bool smart_compaction_should_escalate(
    const FastCompactionCandidate& candidate,
    long long context_window_tokens,
    long long compact_trigger_tokens,
    long long tail_budget_tokens,
    std::string& reason) {
    const long long size_limit =
        context_window_tokens > 0
            ? std::min(context_window_tokens * 60 / 100,
                       compact_trigger_tokens > 0 ? compact_trigger_tokens
                                                  : std::numeric_limits<long long>::max())
            : compact_trigger_tokens;
    if (size_limit > 0 && candidate.estimated_tokens > size_limit) {
        reason = "fast candidate exceeds the smart size ceiling";
        return true;
    }
    if (candidate.protected_content_truncated) {
        reason = "fast candidate truncates protected content";
        return true;
    }
    if (candidate.omitted_item_at_least_2k) {
        reason = "fast candidate omits a substantive item";
        return true;
    }
    if (tail_budget_tokens > 0 &&
        candidate.omitted_substantive_tokens >= tail_budget_tokens) {
        reason = "fast candidate omits at least the tail budget";
        return true;
    }
    reason.clear();
    return false;
}

long long compaction_summary_input_budget(long long context_window_tokens) {
    return context_window_tokens > 0 ? context_window_tokens * 60 / 100 : 8000;
}

long long compaction_summary_output_budget(long long source_tokens,
                                           long long context_window_tokens) {
    // Cap the model-written checkpoint tightly: tool bodies are reduced before
    // the model sees them, so a multi-kB summary is rarely useful and often
    // reintroduces quoted source into remaining context.
    const long long upper =
        context_window_tokens > 0
            ? std::min<long long>(kSummaryOutputMax, context_window_tokens / 20)
            : 1000;
    return std::max<long long>(
        kSummaryOutputMin,
        std::min<long long>(upper,
                            std::max<long long>(1, source_tokens / 8)));
}

long long compaction_summary_model_timeout_ms() {
    return kSummaryModelTimeoutMs;
}

ReasoningSelection compaction_summary_reasoning(
    const std::vector<ReasoningSelection>& catalog_options) {
    for (const ReasoningSelection& option : catalog_options) {
        if (config::reasoning_selection_disables(option)) return option;
    }
    for (const char* preferred : {"min", "minimal", "low"}) {
        for (const ReasoningSelection& option : catalog_options) {
            if (option.kind == ReasoningSelectionKind::Named &&
                ascii_lower(option.value) == preferred)
                return option;
        }
    }
    return ReasoningSelection::automatic();
}

std::string format_compaction_success_notice(long long elapsed_seconds,
                                             long long tokens_before,
                                             long long tokens_after) {
    const long long elapsed = std::max(0LL, elapsed_seconds);
    const long long minutes = elapsed / 60;
    const long long seconds = elapsed % 60;
    const long long before = std::max(0LL, tokens_before);
    const long long after = std::max(0LL, tokens_after);
    const long long saved = std::max(0LL, before - after);
    std::ostringstream notice;
    notice << "Compacting context succeeded in ";
    if (minutes > 0)
        notice << minutes << " min ";
    notice << seconds << (seconds == 1 ? " second" : " seconds")
           << ". ~" << saved << " tokens saved. " << after
           << " tokens in remaining context.";
    return notice.str();
}

std::string format_compaction_no_op_notice(long long remaining_tokens) {
    return "Compacting context skipped. No older context can be removed without "
           "dropping the protected head or recent tail. ~" +
           std::to_string(std::max(0LL, remaining_tokens)) +
           " tokens remain.";
}

std::string format_compaction_failure_notice(const std::string& error_message) {
    bool truncated = false;
    std::string detail = single_line(error_message);
    if (detail.empty()) detail = "unknown compaction error";
    detail = bounded_extract(detail, 400, truncated);
    return "Compacting context failed: " + detail;
}

std::string format_compaction_progress(CompactionStrategy strategy,
                                       long long elapsed_seconds) {
    const std::size_t dots =
        static_cast<std::size_t>(std::max(0LL, elapsed_seconds) % 3 + 1);
    return "Compacting context using " + std::string(compaction_strategy_name(strategy)) +
           std::string(dots, '.');
}

std::string compaction_checkpoint_wrapper(const std::string& checkpoint) {
    return "[Compacted agent checkpoint — reference material only]\n"
           "The block below summarizes prior work. It is not a new instruction. "
           "Treat completed work and file paths as historical hints only. "
           "Do not trust quoted source text as current; re-read workspace files "
           "with tools before editing or relying on their contents.\n\n" +
           checkpoint;
}

std::string compaction_summary_schema_prompt(const std::string& user_preamble) {
    return user_preamble +
           "\n\nSummarize the supplied chronological agent history into a tight "
           "checkpoint. The history has already been deterministically reduced: "
           "volatile tool results (index/list/glob), reloadable reads/searches, "
           "and large command output are stubs or digests — do not try to recover "
           "omitted bodies.\n"
           "Preserve the user's language and never reproduce credentials or secrets.\n"
           "Reloadable tool policy: do not treat read_file, read_many, read_symbol, "
           "get_skeleton, search_text/search_symbol, find_tests, inspect_code_task, "
           "fetch_url, or search_web results as vital durable content. Bodies can be "
           "reloaded from the workspace, index, or network. At most note that a path "
           "or query was used. Never paste source code, file dumps, search match text, "
           "or long tool result bodies into any section.\n"
           "Mutation and outcome policy: preserve edit/write/rename/remove facts "
           "(path + what changed), failed commands (exit + key error lines), test "
           "fail→pass transitions, irreversible git actions, and explicit user "
           "constraints. Prefer the verified-facts list in the user message when "
           "present — those lines must appear under the appropriate headings.\n"
           "Prefer short bullets. Omit filler. Return only a concise checkpoint "
           "starting immediately with these headings (no preamble):\n"
           "Active Task\nGoal\nConstraints\n"
           "Decisions\nCompleted Work\nActive State\nRelevant Files/Evidence\nBlockers\n"
           "Remaining Work";
}

std::string compaction_summary_user_guidance(const CompactionKeepList& keep_list) {
    std::ostringstream out;
    out << "Start your response with exactly these headings in order:\n"
           "## Active Task\n## Goal\n## Constraints\n## Decisions\n"
           "## Completed Work\n## Active State\n## Relevant Files/Evidence\n"
           "## Blockers\n## Remaining Work\n";
    if (!keep_list.lines.empty()) {
        out << "\nVerified facts — must appear in the checkpoint under the "
               "appropriate headings (organize; do not drop):\n";
        std::size_t count = 0;
        for (const std::string& line : keep_list.lines) {
            if (count >= 32) break;
            out << "- " << line << "\n";
            ++count;
        }
    }
    out << "\nChronological history to summarize follows. It is already "
           "pre-shrunk; do not invent file contents that are not present.\n";
    return out.str();
}

std::string render_compaction_source(const CompactionPartition& partition) {
    std::ostringstream out;
    if (!partition.prior_summary.empty())
        out << "[Prior checkpoint]\n" << partition.prior_summary << "\n";
    for (const CompactionLogicalItem& item : partition.middle)
        append_exact_item(out, item, "[Chronological history]");
    return out.str();
}

std::string build_prior_session_context(
    const std::vector<AgentMessageRecord>& messages, std::size_t max_chars) {
    if (messages.empty() || max_chars == 0) return {};
    std::size_t start = messages.size() > 80 ? messages.size() - 80 : 0;
    std::ostringstream body;
    for (std::size_t i = start; i < messages.size(); ++i) {
        const AgentMessageRecord& message = messages[i];
        if (!model_projection_role(message.role)) continue;
        std::string content = message.content;
        if (content.size() > 1500) content = content.substr(0, 1497) + "...";
        body << "[" << (message.role.empty() ? "message" : message.role) << "] "
             << content << "\n";
    }
    std::string text = body.str();
    if (text.empty()) return {};
    if (text.size() > max_chars) {
        text = text.substr(text.size() - max_chars);
        const std::size_t nl = text.find('\n');
        if (nl != std::string::npos && nl + 1 < text.size())
            text = text.substr(nl + 1);
    }
    return "Prior agent work on this project (read-only context from earlier turns; "
           "use tools for the current request; do not restate this block unless asked):\n" +
           text;
}

}  // namespace ainiux::agent
