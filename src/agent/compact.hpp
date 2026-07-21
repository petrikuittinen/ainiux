#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "agent/session_store.hpp"
#include "common.hpp"

namespace ainiux::agent {

// Resolve compact_limit percentage: explicit 1..100 wins; else 75 if window>64k else 100.
int effective_compact_limit_percent(int configured_limit, long long context_window_tokens);

// Whether estimated request tokens have reached the auto-compact threshold.
bool should_auto_compact(bool auto_compact_enabled,
                         int compact_limit_percent,
                         long long context_window_tokens,
                         long long estimated_request_tokens);

// Rough token estimate from UTF-8 bytes (~4 bytes/token).
long long estimate_tokens_from_text(const std::string& text);

long long estimate_transcript_tokens(const std::vector<AgentMessageRecord>& messages);

// Deterministic local summary of older transcript for compaction.
std::string build_local_compact_summary(const std::vector<AgentMessageRecord>& messages,
                                        std::size_t drop_count);

}  // namespace ainiux::agent
