#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "ainiux/compaction_strategy.hpp"
#include "ainiux/model_setting.hpp"
#include "agent/session_store.hpp"
#include "common.hpp"

namespace ainiux::agent {

const char* compaction_strategy_name(CompactionStrategy strategy);
bool parse_compaction_strategy(const std::string& text, CompactionStrategy& strategy);

// Resolve compact_limit percentage: explicit 1..100 wins; otherwise 75.
int effective_compact_limit_percent(int configured_limit, long long context_window_tokens);

// Whether estimated request tokens have reached the auto-compact threshold.
bool should_auto_compact(bool auto_compact_enabled,
                         int compact_limit_percent,
                         long long context_window_tokens,
                         long long estimated_request_tokens);

// Rough token estimate from UTF-8 bytes (~4 bytes/token).
long long estimate_tokens_from_text(const std::string& text);

long long estimate_transcript_tokens(const std::vector<AgentMessageRecord>& messages);

struct CompactionLogicalItem {
    long long seq = 0;
    std::string role;
    std::string content;
    std::string tool_name;
    bool tool_ok = true;
    long long estimated_tokens = 0;
};

struct CompactionPartition {
    std::vector<CompactionLogicalItem> head;
    std::vector<CompactionLogicalItem> middle;
    std::vector<CompactionLogicalItem> tail;
    std::string prior_summary;
    long long source_tokens = 0;
    long long tail_budget_tokens = 0;
    long long omitted_substantive_tokens = 0;
    bool protected_content_truncated = false;
};

struct FastCompactionCandidate {
    std::string checkpoint;
    long long estimated_tokens = 0;
    bool protected_content_truncated = false;
    long long omitted_substantive_tokens = 0;
    bool omitted_item_at_least_2k = false;
};

std::vector<CompactionLogicalItem> build_compaction_timeline(
    const std::vector<AgentMessageRecord>& messages,
    const std::vector<AgentToolEventRecord>& tool_events);

CompactionPartition partition_compaction_timeline(
    const std::vector<CompactionLogicalItem>& timeline,
    long long context_window_tokens);

FastCompactionCandidate build_fast_compaction_candidate(
    const CompactionPartition& partition,
    long long max_checkpoint_tokens);

bool smart_compaction_should_escalate(const FastCompactionCandidate& candidate,
                                      long long context_window_tokens,
                                      long long compact_trigger_tokens,
                                      long long tail_budget_tokens,
                                      std::string& reason);

long long compaction_summary_input_budget(long long context_window_tokens);
long long compaction_summary_output_budget(long long source_tokens,
                                           long long context_window_tokens);

ReasoningSelection compaction_summary_reasoning(
    const std::vector<ReasoningSelection>& catalog_options);

std::string format_compaction_success_notice(long long tokens_before,
                                             long long tokens_after);
std::string format_compaction_no_op_notice(long long remaining_tokens);
std::string format_compaction_failure_notice(const std::string& error_message);
std::string format_compaction_progress(CompactionStrategy strategy,
                                       long long elapsed_seconds);

std::string compaction_checkpoint_wrapper(const std::string& checkpoint);
std::string compaction_summary_schema_prompt(const std::string& user_preamble);
std::string render_compaction_source(const CompactionPartition& partition);

// Build a bounded prior-session context block for model reseed after reopen.
// Returns empty when there is nothing useful to inject.
std::string build_prior_session_context(const std::vector<AgentMessageRecord>& messages,
                                        std::size_t max_chars = 24000);

}  // namespace ainiux::agent
