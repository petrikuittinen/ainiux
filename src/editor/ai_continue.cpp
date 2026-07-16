#include "editor/ai_continue.hpp"

#include "cli/args.hpp"
#include "editor/editor_ai_setup.hpp"
#include "editor/editor_assist.hpp"
#include "tui/tui.hpp"

namespace pkchat::editor {

AiContinueSettings ai_continue_settings(const cli::Options& options) {
    AiContinueSettings settings;
    settings.max_prefix_chars = options.editor_ai_continue_prefix_max_chars;
    settings.max_postfix_chars = options.editor_ai_continue_postfix_max_chars;
    settings.max_prose_prefix_chars = options.editor_ai_continue_prose_prefix_max_chars;
    settings.max_prose_postfix_chars = options.editor_ai_continue_prose_postfix_max_chars;
    settings.max_output_tokens = options.editor_ai_continue_max_tokens;
    settings.max_prefix_chars =
        nonnegative_size_from_env("MAX_CONTINUE_PREFIX", settings.max_prefix_chars);
    settings.max_postfix_chars =
        nonnegative_size_from_env("MAX_CONTINUE_POSTFIX", settings.max_postfix_chars);
    settings.max_prose_prefix_chars = nonnegative_size_from_env(
        "MAX_CONTINUE_PROSE_PREFIX", settings.max_prose_prefix_chars);
    settings.max_prose_postfix_chars = nonnegative_size_from_env(
        "MAX_CONTINUE_PROSE_POSTFIX", settings.max_prose_postfix_chars);
    settings.max_output_tokens =
        positive_int_from_env("MAX_AI_CONTINUE_TOKENS", settings.max_output_tokens);
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

std::string continue_completion_status_message(const std::string& provider_name,
                                               const std::string& model_name,
                                               const provider::ChatResult& result,
                                               bool stream,
                                               const std::vector<provider::Message>& messages,
                                               long long context_tokens) {
    return tui::generation_ready_status(provider_name, model_name, result, stream, messages,
                                        context_tokens);
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
    start_assist_job(context,
                     execution.messages,
                     true,
                     execution.code_completion,
                     execution.prose_completion,
                     execution.completion_language,
                     events,
                     job);
}

}  // namespace pkchat::editor
