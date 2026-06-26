#include "editor/ai_continue.hpp"

#include "editor/editor_assist.hpp"

#include <cctype>
#include <cstdlib>

namespace pkchat::editor {
namespace {

std::string lower_ascii_copy(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

bool is_loopback_base_url(const std::string& base_url) {
    const std::string lower = lower_ascii_copy(base_url);
    return lower.find("://localhost") != std::string::npos ||
           lower.find("://127.0.0.1") != std::string::npos ||
           lower.find("://[::1]") != std::string::npos;
}

int parse_positive_int_env(const char* name, int default_value) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return default_value;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || (end != nullptr && *end != '\0') || parsed <= 0) {
        return default_value;
    }
    return static_cast<int>(parsed);
}

size_t parse_positive_size_env(const char* name, size_t default_value) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return default_value;
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (end == value || (end != nullptr && *end != '\0') || parsed == 0) {
        return default_value;
    }
    return static_cast<size_t>(parsed);
}

}  // namespace

AiContinueSettings ai_continue_settings_from_env() {
    AiContinueSettings settings;
    settings.max_read_chars = parse_positive_size_env("MAX_AI_CONTINUE_READ", kDefaultAiContinueReadChars);
    settings.max_output_tokens =
        parse_positive_int_env("MAX_AI_CONTINUE_TOKENS", kDefaultAiContinueMaxTokens);
    return settings;
}

std::string continue_status_message(const std::string& model_name, const std::string& suffix) {
    if (model_name.empty()) {
        return suffix;
    }
    return "[" + model_name + "] " + suffix;
}

bool editor_auto_selects_model(const provider::RequestContext& context) {
    if (context.profile.offline) {
        return false;
    }
    if (context.profile.name == "lm_studio" || context.profile.name == "ollama" ||
        context.profile.name == "vllm") {
        return true;
    }
    if (context.profile.name == "custom_openai_chat" && is_loopback_base_url(context.base_url)) {
        return true;
    }
    return false;
}

Error resolve_editor_default_model(AiContinueContext& context) {
    if (!context.request.options.model.empty()) {
        return ok_error();
    }
    if (!editor_auto_selects_model(context.request)) {
        return {ErrorCode::BadArgs, "AI continue requires --model for this provider"};
    }

    provider::ModelsResult models;
    Error err = provider::list_models(context.request, models);
    if (!err.ok()) {
        return err;
    }
    if (models.model_ids.empty()) {
        return {ErrorCode::ProviderSchema,
                "model list from " + context.request.models_url + " was empty; pass --model explicitly"};
    }
    context.request.options.model = models.model_ids.front();
    return ok_error();
}

Error validate_continue_request(const AiContinueContext& context) {
    if (context.request.profile.offline) {
        return {ErrorCode::UnsupportedFeature,
                "AI continue requires a provider; use --provider lmstudio or similar"};
    }
    if (context.request.options.model.empty()) {
        return {ErrorCode::BadArgs, "AI continue requires --model for this provider"};
    }
    return ok_error();
}

provider::RequestContext continue_request_context(const AiContinueContext& context) {
    return assist_request_context(context, true);
}

void start_continue_job(const AiContinueContext& context,
                        const std::string& prefix,
                        runtime::EventQueue<ContinueEvent>& events,
                        runtime::JobHandle& job) {
    EditorState continue_state = EditorState::from_text(prefix);
    continue_state.cursor = continue_state.text.size();
    AssistExecution execution = build_assist_execution(
        continue_state, context, AssistCommandKind::Continue, std::nullopt, "", std::nullopt);
    if (!execution.ok) {
        ContinueEvent event;
        event.type = ContinueEventType::Error;
        event.error = {ErrorCode::Internal, execution.error_message};
        events.push(std::move(event));
        return;
    }
    start_assist_job(context, execution.messages, true, events, job);
}

}  // namespace pkchat::editor