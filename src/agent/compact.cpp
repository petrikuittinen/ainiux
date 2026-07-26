#include "agent/compact.hpp"

#include <sstream>

namespace ainiux::agent {
namespace {

bool model_projection_role(const std::string& role) {
    return role != "notice" && role != "thinking";
}

}  // namespace

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
        if (!model_projection_role(message.role)) continue;
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
        if (!model_projection_role(messages[i].role)) continue;
        if (messages[i].role == "summary") {
            std::string prior = messages[i].content;
            if (prior.size() > 1200) prior = prior.substr(0, 1197) + "...";
            out << "Prior compacted context:\n" << prior << "\n";
        } else if (messages[i].role == "user") {
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

std::string build_prior_session_context(const std::vector<AgentMessageRecord>& messages,
                                        std::size_t max_chars) {
    if (messages.empty() || max_chars == 0) return {};

    // Prefer the newest messages when the transcript exceeds the budget.
    std::size_t start = 0;
    if (messages.size() > 80) start = messages.size() - 80;

    std::ostringstream body;
    for (std::size_t i = start; i < messages.size(); ++i) {
        const AgentMessageRecord& message = messages[i];
        if (!model_projection_role(message.role)) continue;
        std::string content = message.content;
        if (content.size() > 1500) content = content.substr(0, 1497) + "...";
        body << "[" << (message.role.empty() ? "message" : message.role) << "] " << content
             << "\n";
    }
    std::string text = body.str();
    if (text.empty()) return {};
    if (text.size() > max_chars) {
        // Keep the tail (most recent activity).
        text = text.substr(text.size() - max_chars);
        const std::size_t nl = text.find('\n');
        if (nl != std::string::npos && nl + 1 < text.size()) text = text.substr(nl + 1);
    }

    std::ostringstream out;
    out << "Prior agent work on this project (read-only context from earlier turns; "
           "use tools for the current request; do not restate this block unless asked):\n"
        << text;
    return out.str();
}

}  // namespace ainiux::agent
