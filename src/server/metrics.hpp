#pragma once

#include <string>
#include <vector>

#include "agent/session_runtime.hpp"
#include "provider/provider.hpp"

namespace ainiux::server {

// Provider-neutral generation measurements exposed by the control API. A
// negative value means the measurement is unavailable and is serialized as
// null. Token counts use signed values so that convention remains explicit.
struct GenerationMetrics {
    long long context_used_tokens = -1;
    long long context_window_tokens = -1;
    long long input_tokens = -1;
    long long fresh_input_tokens = -1;
    long long cache_read_tokens = -1;
    long long cache_write_tokens = -1;
    long long output_tokens = -1;
    long long total_tokens = -1;
    bool input_tokens_estimated = false;
    bool output_tokens_estimated = false;
    long long elapsed_ms = -1;
    long long ttft_ms = -1;
    double output_tokens_per_second = -1.0;
};

GenerationMetrics chat_generation_metrics(
    const provider::RequestContext& context,
    const std::vector<provider::Message>& messages,
    const provider::ChatResult& result);

GenerationMetrics agent_generation_metrics(
    long long context_used_tokens,
    long long context_window_tokens,
    const agent::AgentTokenUsage& usage,
    long long elapsed_ms,
    double output_tokens_per_second = -1.0);

std::string generation_metrics_json(const GenerationMetrics& metrics);

}  // namespace ainiux::server
