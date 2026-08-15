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

// How aggressively a tool result may be reduced before any model summarizer call.
enum class ToolCompactionTier {
    Prune,     // one-line status only (volatile snapshots)
    Stub,      // args + status + tiny excerpt (reloadable)
    Digest,    // mutation fact: path + op (not replayable)
    Semantic,  // keep failure/exit lines (build/test/git)
    Full,      // keep bounded full text unless oversized
};

struct CompactionLogicalItem {
    long long seq = 0;
    std::string role;
    std::string content;
    std::string tool_name;
    bool tool_ok = true;
    long long estimated_tokens = 0;
    // Optional fields filled when reducing tool events (merge / keep-list).
    std::string primary_path;
    std::string content_hash;
    int exit_status = 0;
    bool has_exit_status = false;
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

struct CompactionKeepList {
    std::vector<std::string> lines;
};

struct FastCompactionCandidate {
    std::string checkpoint;
    long long estimated_tokens = 0;
    bool protected_content_truncated = false;
    long long omitted_substantive_tokens = 0;
    bool omitted_item_at_least_2k = false;
};

ToolCompactionTier tool_compaction_tier(const std::string& tool_name);

// True for tools whose successful payloads are cheap to re-fetch (workspace /
// index / network). Bodies must not bloat compacted model context.
bool is_reloadable_file_read_tool(const std::string& tool_name);

// Deterministic reduction of one tool event for the compaction timeline.
std::string reduce_tool_item_content(const std::string& tool_name,
                                     const std::string& arguments_json,
                                     const std::string& result_json,
                                     bool ok,
                                     CompactionLogicalItem* meta = nullptr);

// Compatibility wrapper around reduce_tool_item_content for read stubs.
std::string stub_reloadable_tool_item_content(const std::string& tool_name,
                                              const std::string& arguments_json,
                                              const std::string& result_json,
                                              bool ok);

std::vector<CompactionLogicalItem> build_compaction_timeline(
    const std::vector<AgentMessageRecord>& messages,
    const std::vector<AgentToolEventRecord>& tool_events);

CompactionPartition partition_compaction_timeline(
    const std::vector<CompactionLogicalItem>& timeline,
    long long context_window_tokens);

// Middle-only merge/dedupe after partition (consecutive reads, exploration,
// read-then-edit, hash dedupe). Mutates middle items in place.
void pre_shrink_compaction_middle(std::vector<CompactionLogicalItem>& middle);

// Deterministic facts that must survive into the fast checkpoint and the
// summarizer prompt (mutations, failures, user decisions, git actions).
CompactionKeepList harvest_compaction_keep_list(
    const std::vector<CompactionLogicalItem>& middle);

FastCompactionCandidate build_fast_compaction_candidate(
    const CompactionPartition& partition,
    long long max_checkpoint_tokens,
    const CompactionKeepList& keep_list = {});

bool smart_compaction_should_escalate(const FastCompactionCandidate& candidate,
                                      long long context_window_tokens,
                                      long long compact_trigger_tokens,
                                      long long tail_budget_tokens,
                                      std::string& reason);

long long compaction_summary_input_budget(long long context_window_tokens);
long long compaction_summary_output_budget(long long source_tokens,
                                           long long context_window_tokens);

// Wall-clock budget for the model compact path before falling back to fast.
long long compaction_summary_model_timeout_ms();

ReasoningSelection compaction_summary_reasoning(
    const std::vector<ReasoningSelection>& catalog_options);

std::string format_compaction_success_notice(long long elapsed_seconds,
                                             long long tokens_before,
                                             long long tokens_after);
std::string format_compaction_no_op_notice(long long remaining_tokens);
std::string format_compaction_failure_notice(const std::string& error_message);
std::string format_compaction_progress(CompactionStrategy strategy,
                                       long long elapsed_seconds);

std::string compaction_checkpoint_wrapper(const std::string& checkpoint);
std::string compaction_summary_schema_prompt(const std::string& user_preamble);
// Extra user-message block: verified keep-list + required heading skeleton.
std::string compaction_summary_user_guidance(const CompactionKeepList& keep_list);
std::string render_compaction_source(const CompactionPartition& partition);

// Build a bounded prior-session context block for model reseed after reopen.
// Returns empty when there is nothing useful to inject.
std::string build_prior_session_context(const std::vector<AgentMessageRecord>& messages,
                                        std::size_t max_chars = 24000);

// Drop rows at or before a /compact all cut. seq 0 means no cut.
std::vector<AgentMessageRecord> messages_after_seq(
    const std::vector<AgentMessageRecord>& messages, long long seq);
std::vector<AgentToolEventRecord> tool_events_after_seq(
    const std::vector<AgentToolEventRecord>& events, long long seq);

}  // namespace ainiux::agent
