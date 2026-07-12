#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "common.hpp"
#include "editor/editor.hpp"
#include "editor/editor_prompts.hpp"

namespace pkchat::cli {
struct Options;
}
#include "provider/provider.hpp"
#include "runtime/runtime.hpp"

namespace pkchat::editor {

struct AiContinueSettings {
    size_t max_read_chars = kDefaultAiContinueReadChars;
    int max_output_tokens = kDefaultAiContinueMaxTokens;
};

struct AiContinueContext {
    provider::RequestContext request;
    AiContinueSettings settings;
    EditorAssistConfig assist_config;
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

AiContinueSettings ai_continue_settings(const cli::Options& options);
std::string continue_status_message(const std::string& provider_name,
                                    const std::string& model_name,
                                    const std::string& suffix);

std::string continue_status_label(const std::string& provider_name, const std::string& model_name);
std::string continue_completion_status_message(const std::string& provider_name,
                                               const std::string& model_name,
                                               const provider::ChatResult& result,
                                               bool stream,
                                               const std::vector<provider::Message>& messages = {},
                                               long long context_tokens = 0);
Error validate_continue_request(const AiContinueContext& context);
provider::RequestContext continue_request_context(const AiContinueContext& context);
void start_continue_job(const AiContinueContext& context,
                        const std::string& prefix,
                        runtime::EventQueue<ContinueEvent>& events,
                        runtime::JobHandle& job);

}  // namespace pkchat::editor
