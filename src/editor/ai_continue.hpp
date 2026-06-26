#pragma once

#include <cstddef>
#include <string>

#include "common.hpp"
#include "provider/provider.hpp"
#include "runtime/runtime.hpp"

namespace pkchat::editor {

constexpr size_t kDefaultAiContinueReadChars = 4096;
constexpr int kDefaultAiContinueMaxTokens = 32768;

struct AiContinueSettings {
    size_t max_read_chars = kDefaultAiContinueReadChars;
    int max_output_tokens = kDefaultAiContinueMaxTokens;
};

struct AiContinueContext {
    provider::RequestContext request;
    AiContinueSettings settings;
};

enum class ContinueEventType {
    Thinking,
    Writing,
    Delta,
    Done,
    Error,
};

struct ContinueEvent {
    ContinueEventType type = ContinueEventType::Thinking;
    std::string text;
    Error error;
    provider::ChatResult chat;
};

AiContinueSettings ai_continue_settings_from_env();
std::string continue_status_message(const std::string& model_name, const std::string& suffix);
Error validate_continue_request(const AiContinueContext& context);
provider::RequestContext continue_request_context(const AiContinueContext& context);
void start_continue_job(const AiContinueContext& context,
                        const std::string& prefix,
                        runtime::EventQueue<ContinueEvent>& events,
                        runtime::JobHandle& job);

}  // namespace pkchat::editor