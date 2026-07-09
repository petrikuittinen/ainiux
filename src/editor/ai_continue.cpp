#include "editor/ai_continue.hpp"

#include "editor/editor_ai_setup.hpp"
#include "editor/editor_assist.hpp"

#include <iomanip>
#include <sstream>

namespace pkchat::editor {
namespace {

bool is_loopback_base_url(const std::string& base_url) {
    const std::string lower = ascii_lower(base_url);
    return lower.find("://localhost") != std::string::npos ||
           lower.find("://127.0.0.1") != std::string::npos ||
           lower.find("://[::1]") != std::string::npos;
}

}  // namespace

AiContinueSettings ai_continue_settings_from_env() {
    AiContinueSettings settings;
    settings.max_read_chars = positive_size_from_env("MAX_AI_CONTINUE_READ", kDefaultAiContinueReadChars);
    settings.max_output_tokens =
        positive_int_from_env("MAX_AI_CONTINUE_TOKENS", kDefaultAiContinueMaxTokens);
    return settings;
}

std::string continue_status_message(const std::string& provider_name,
                                    const std::string& model_name,
                                    const std::string& suffix) {
    const std::string display_provider =
        provider_name.empty() ? "" : provider::display_name_for_profile(provider_name);
    if (display_provider.empty() && model_name.empty()) {
        return suffix;
    }
    if (display_provider.empty()) {
        return "[" + model_name + "] " + suffix;
    }
    if (model_name.empty()) {
        return "[" + display_provider + " / model unknown] " + suffix;
    }
    return "[" + display_provider + " / " + model_name + "] " + suffix;
}

std::string continue_status_label(const std::string& provider_name, const std::string& model_name) {
    std::string label = continue_status_message(provider_name, model_name, "");
    while (!label.empty() && label.back() == ' ') {
        label.pop_back();
    }
    return label;
}

std::string continue_completion_status_suffix(const provider::ChatResult& result,
                                              bool stream,
                                              const std::string& state) {
    std::ostringstream out;
    out << state;
    if (stream) {
        out << " | TTFT ";
        if (result.ttft_ms >= 0) {
            out << result.ttft_ms << "ms";
        } else {
            out << "unknown";
        }
    } else {
        out << " | Response " << result.total_ms << "ms";
    }
    out << " | ";
    if (result.completion_tokens_estimated) {
        out << "~";
    }
    out << std::fixed << std::setprecision(1) << provider::tokens_per_second(result, stream)
        << " tok/s";
    return out.str();
}

std::string continue_completion_status_message(const std::string& provider_name,
                                               const std::string& model_name,
                                               const provider::ChatResult& result,
                                               bool stream,
                                               const std::string& state) {
    return continue_status_message(provider_name,
                                   model_name,
                                   continue_completion_status_suffix(result, stream, state));
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
        return {ErrorCode::UnsupportedFeature, editor_no_provider_message()};
    }
    if (context.request.options.model.empty()) {
        return {ErrorCode::BadArgs, editor_no_model_message()};
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
    const std::optional<size_t> command_index = assist_command_index(context.assist_config, "/continue");
    if (!command_index.has_value()) {
        ContinueEvent event;
        event.type = ContinueEventType::Error;
        event.error = {ErrorCode::Internal, "configured editor assist commands are missing /continue"};
        events.push(std::move(event));
        return;
    }
    AssistExecution execution = build_assist_execution(continue_state,
                                                       context,
                                                       AssistCommandKind::Configured,
                                                       *command_index,
                                                       AssistScope::Continue,
                                                       "",
                                                       std::nullopt);
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
