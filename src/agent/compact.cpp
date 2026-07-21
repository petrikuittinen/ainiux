#include "agent/compact.hpp"

#include <sstream>

namespace ainiux::agent {

int effective_compact_limit_percent(int configured_limit, long long context_window_tokens) {
    if (configured_limit >= 1 && configured_limit <= 100) return configured_limit;
    if (context_window_tokens > 0 && context_window_tokens <= 65536) return 100;
    return 75;
}

bool should_auto_compact(bool auto_compact_enabled,
                         int compact_limit_percent,
                         long long context_window_tokens,
                         long long estimated_request_tokens) {
    if (!auto_compact_enabled) return false;
    if (context_window_tokens <= 0 || estimated_request_tokens <= 0) return false;
    const int limit = effective_compact_limit_percent(compact_limit_percent, context_window_tokens);
    // 100% means only when full (or over).
    const long long threshold =
        (context_window_tokens * static_cast<long long>(limit) + 99) / 100;
    return estimated_request_tokens >= threshold;
}

long long estimate_tokens_from_text(const std::string& text) {
    if (text.empty()) return 0;
    return static_cast<long long>((text.size() + 3) / 4);
}

long long estimate_transcript_tokens(const std::vector<AgentMessageRecord>& messages) {
    long long total = 0;
    for (const AgentMessageRecord& message : messages) {
        total += estimate_tokens_from_text(message.role);
        total += estimate_tokens_from_text(message.content);
        total += estimate_tokens_from_text(message.tool_name);
        total += estimate_tokens_from_text(message.args_preview);
        total += 4;  // framing overhead
    }
    return total;
}

std::string build_local_compact_summary(const std::vector<AgentMessageRecord>& messages,
                                        std::size_t drop_count) {
    std::ostringstream out;
    out << "Compacted earlier agent context (" << drop_count << " messages).\n";
    std::size_t user_n = 0;
    std::size_t tool_n = 0;
    std::size_t assistant_n = 0;
    for (std::size_t i = 0; i < drop_count && i < messages.size(); ++i) {
        if (messages[i].role == "user") {
            ++user_n;
            if (user_n <= 3) {
                std::string snippet = messages[i].content;
                if (snippet.size() > 120) snippet = snippet.substr(0, 117) + "...";
                out << "- user: " << snippet << "\n";
            }
        } else if (messages[i].role == "tool") {
            ++tool_n;
        } else if (messages[i].role == "assistant") {
            ++assistant_n;
        }
    }
    out << "Totals in dropped span: users=" << user_n << " tools=" << tool_n
        << " assistants=" << assistant_n << ".\n";
    out << "Continue with the remaining recent transcript and project tools.";
    return out.str();
}

}  // namespace ainiux::agent
