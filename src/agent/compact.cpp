#include "agent/compact.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <set>
#include <sstream>

#include "config/model_catalog.hpp"
#include "json/json.hpp"

namespace ainiux::agent {
namespace {

constexpr long long kSubstantiveItemTokens = 2000;
constexpr std::size_t kMinimumTailItems = 3;
constexpr std::size_t kMaximumTailItems = 20;
// Fraction of the context window retained as an unsummarized recent tail.
// Kept deliberately smaller than earlier 15% so large recent tool batches do
// not dominate post-compact request size once reloadable reads are stubbed.
constexpr int kTailBudgetPercent = 8;
constexpr std::size_t kFailedToolErrorBytes = 400;

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

bool is_reloadable_file_read_tool(const std::string& tool_name) {
    return tool_name == "read_file" || tool_name == "read_many";
}

std::string stub_reloadable_tool_item_content(const std::string& tool_name,
                                              const std::string& arguments_json,
                                              const std::string& result_json,
                                              bool ok) {
    std::ostringstream content;
    content << "Tool: " << (tool_name.empty() ? "unknown" : tool_name)
            << "\nArguments: "
            << (arguments_json.empty() ? std::string("{}") : arguments_json)
            << "\n";
    if (ok) {
        content << "Result: omitted (reloadable from workspace; re-read if needed)\n"
                   "Status: ok";
    } else {
        content << "Result: omitted (file body not retained on failure)\n"
                   "Status: failed\n"
                   "Error: "
                << bounded_error_from_result(result_json);
    }
    return content.str();
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
        // Display tool rows are already one-line previews. If a reloadable
        // read somehow lands here without a matching tool_event, still avoid
        // carrying a large body into the compact projection.
        if (message.role == "tool" && is_reloadable_file_read_tool(message.tool_name) &&
            message.content.size() > 512) {
            item.content = stub_reloadable_tool_item_content(
                message.tool_name,
                message.args_preview.empty() ? "{}" : message.args_preview,
                message.content, message.tool_ok);
        } else {
            item.content = message.content;
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
        if (is_reloadable_file_read_tool(event.tool_name)) {
            item.content = stub_reloadable_tool_item_content(
                event.tool_name, event.arguments, event.result, event.ok);
        } else {
            std::ostringstream content;
            content << "Tool: "
                    << (event.tool_name.empty() ? "unknown" : event.tool_name)
                    << "\nArguments: " << event.arguments
                    << "\nResult: " << event.result;
            item.content = content.str();
        }
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

FastCompactionCandidate build_fast_compaction_candidate(
    const CompactionPartition& partition,
    long long max_checkpoint_tokens) {
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
            out << "- " << extract
                << "\n";
            truncated = truncated || item_truncated;
            if (item_truncated)
                omitted += std::max<long long>(
                    0, item.estimated_tokens - estimate_tokens_from_text(extract));
            ++user_count;
        } else if (item.role == "tool") {
            ++tools;
            if (!item.tool_ok) ++failed;
            const std::string text = item.content;
            std::size_t pos = 0;
            while ((pos = text.find('/', pos)) != std::string::npos) {
                std::size_t end = text.find_first_of(" \t\r\n\"')},]", pos);
                paths.insert(text.substr(pos, end == std::string::npos ? 160
                                                                      : end - pos));
                pos += 1;
                if (paths.size() >= 24) break;
            }
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
    out << "Decisions / Completed Work / Blockers / Remaining Work\n"
           "- Deterministic checkpoint: verify current source and tool state before acting.\n";

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
    // Cap the model-written checkpoint tightly: file bodies are omitted from
    // the source via stubs, so a multi-kB summary is rarely useful and often
    // reintroduces quoted source into remaining context.
    const long long upper =
        context_window_tokens > 0
            ? std::min<long long>(4000, context_window_tokens / 20)
            : 1000;
    return std::max<long long>(512,
                               std::min<long long>(upper,
                                                   std::max<long long>(1, source_tokens / 8)));
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
           "checkpoint. Preserve the user's language and never reproduce "
           "credentials or secrets.\n"
           "Reloadable tool policy: do not treat read_file or read_many results as "
           "vital durable content. File bodies can be reloaded from the workspace. "
           "At most note that a path was read (and why), or list the path under "
           "Relevant Files/Evidence. Never paste source code, file dumps, or long "
           "tool result bodies into any section.\n"
           "Prefer short bullets. Omit filler. Return only a concise checkpoint "
           "with these headings:\nActive Task\nGoal\nConstraints\n"
           "Decisions\nCompleted Work\nActive State\nRelevant Files/Evidence\nBlockers\n"
           "Remaining Work";
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
