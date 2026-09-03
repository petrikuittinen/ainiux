#include "server/metrics.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "context/context.hpp"
#include "json/json.hpp"

namespace ainiux::server {
namespace {

long long saturating_add(long long left, long long right) {
    if (left < 0 || right < 0) return -1;
    if (right > std::numeric_limits<long long>::max() - left)
        return std::numeric_limits<long long>::max();
    return left + right;
}

std::string nullable_integer(long long value) {
    return value < 0 ? "null" : std::to_string(value);
}

std::string nullable_number(double value) {
    if (!std::isfinite(value) || value < 0.0) return "null";
    json::Value number;
    number.type = json::Value::Type::Number;
    number.number = value;
    return json::stringify(number);
}

}  // namespace

GenerationMetrics chat_generation_metrics(
    const provider::RequestContext& context,
    const std::vector<provider::Message>& messages,
    const provider::ChatResult& result) {
    GenerationMetrics metrics;
    metrics.input_tokens = result.prompt_tokens;
    if (metrics.input_tokens < 0) {
        metrics.input_tokens = context::estimated_text_tokens(messages);
        metrics.input_tokens_estimated = true;
    }
    metrics.fresh_input_tokens = result.fresh_prompt_tokens;
    metrics.cache_read_tokens = result.cache_read_tokens;
    metrics.cache_write_tokens = result.cache_write_tokens;
    metrics.output_tokens = std::max(0LL, result.completion_tokens);
    metrics.output_tokens_estimated = result.completion_tokens_estimated;
    metrics.total_tokens = provider::reported_total_tokens(result);
    if (metrics.total_tokens < 0)
        metrics.total_tokens = saturating_add(metrics.input_tokens, metrics.output_tokens);
    metrics.context_used_tokens = context::estimated_usage_tokens(messages, result);
    metrics.context_window_tokens = context.options.context_tokens > 0
                                        ? context.options.context_tokens : -1;
    metrics.elapsed_ms = std::max(0LL, result.total_ms);
    metrics.ttft_ms = result.ttft_ms;
    metrics.output_tokens_per_second =
        provider::tokens_per_second(result, context.options.stream);
    return metrics;
}

GenerationMetrics agent_generation_metrics(
    long long context_used_tokens,
    long long context_window_tokens,
    const agent::AgentTokenUsage& usage,
    long long elapsed_ms,
    double output_tokens_per_second) {
    GenerationMetrics metrics;
    metrics.context_used_tokens = context_used_tokens >= 0 ? context_used_tokens : -1;
    metrics.context_window_tokens = context_window_tokens > 0 ? context_window_tokens : -1;
    metrics.input_tokens = std::max(0LL, usage.input_tokens);
    metrics.fresh_input_tokens = std::max(0LL, usage.fresh_input_tokens);
    metrics.cache_read_tokens = std::max(0LL, usage.cache_read_tokens);
    metrics.cache_write_tokens = std::max(0LL, usage.cache_write_tokens);
    metrics.output_tokens = std::max(0LL, usage.output_tokens);
    metrics.total_tokens = saturating_add(metrics.input_tokens, metrics.output_tokens);
    metrics.input_tokens_estimated = usage.input_estimated;
    metrics.output_tokens_estimated = usage.output_estimated;
    metrics.elapsed_ms = elapsed_ms >= 0 ? elapsed_ms : -1;
    metrics.output_tokens_per_second = output_tokens_per_second;
    return metrics;
}

std::string generation_metrics_json(const GenerationMetrics& metrics) {
    return "{\"context_used_tokens\":" + nullable_integer(metrics.context_used_tokens) +
           ",\"context_window_tokens\":" + nullable_integer(metrics.context_window_tokens) +
           ",\"input_tokens\":" + nullable_integer(metrics.input_tokens) +
           ",\"fresh_input_tokens\":" + nullable_integer(metrics.fresh_input_tokens) +
           ",\"cache_read_tokens\":" + nullable_integer(metrics.cache_read_tokens) +
           ",\"cache_write_tokens\":" + nullable_integer(metrics.cache_write_tokens) +
           ",\"output_tokens\":" + nullable_integer(metrics.output_tokens) +
           ",\"total_tokens\":" + nullable_integer(metrics.total_tokens) +
           ",\"input_tokens_estimated\":" +
           std::string(metrics.input_tokens_estimated ? "true" : "false") +
           ",\"output_tokens_estimated\":" +
           std::string(metrics.output_tokens_estimated ? "true" : "false") +
           ",\"elapsed_ms\":" + nullable_integer(metrics.elapsed_ms) +
           ",\"ttft_ms\":" + nullable_integer(metrics.ttft_ms) +
           ",\"output_tokens_per_second\":" +
           nullable_number(metrics.output_tokens_per_second) + "}";
}

}  // namespace ainiux::server
