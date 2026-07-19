#include "editor/editor_ai_setup.hpp"

#include "cli/args.hpp"
#include "tui/tui.hpp"

namespace ainiux::editor {

bool editor_ai_has_provider(const std::optional<AiContinueContext>& context) {
    return context.has_value() && !context->request.profile.offline;
}

bool editor_ai_ready(const std::optional<AiContinueContext>& context) {
    return editor_ai_has_provider(context) && !context->request.options.model.empty();
}

std::string editor_no_provider_message() {
    return "No provider chosen. Use /provider to choose one";
}

std::string editor_no_model_message() {
    return "No model chosen. Use /model to choose one";
}

std::string editor_startup_status(const std::optional<AiContinueContext>& context) {
    if (!editor_ai_has_provider(context)) {
        return "Local editor · Use /provider and /model to enable AI features";
    }
    if (context->request.options.model.empty()) {
        return tui::chat_startup_status(context->request);
    }
    return tui::provider_model_status_message(context->request,
                                              "ready · Change provider with /provider or model with /model");
}

Error ensure_editor_ai_context(std::optional<AiContinueContext>& context, const EditorAssistConfig& assist_config) {
    if (context.has_value()) {
        return ok_error();
    }
    AiContinueContext created;
    created.assist_config = assist_config;
    cli::Options options;
    options.editor = true;
    provider::apply_editor_startup_default(options);
    created.settings = ai_continue_settings(options);
    provider::ContextResult built = provider::build_context(options);
    if (!built.error.ok()) {
        return built.error;
    }
    created.request = std::move(built.context);
    context = std::move(created);
    return ok_error();
}

Error apply_editor_provider_target(std::optional<AiContinueContext>& context,
                                  const EditorAssistConfig& assist_config,
                                  const std::string& target) {
    Error ensure_error = ensure_editor_ai_context(context, assist_config);
    if (!ensure_error.ok()) {
        return ensure_error;
    }

    cli::Options next = context->request.options;
    provider::apply_provider_target(next, target);
    provider::ContextResult rebuilt = provider::build_context(next);
    if (!rebuilt.error.ok()) {
        return rebuilt.error;
    }
    const bool changed = rebuilt.context.profile.name != context->request.profile.name ||
                         rebuilt.context.base_url != context->request.base_url;
    if (changed) {
        // A model id belongs to the provider that supplied it. Never carry it
        // across an actual provider change; the shared flow discovers a new one.
        rebuilt.context.options.model.clear();
        rebuilt.context.options.reasoning = ReasoningSelection::automatic();
        rebuilt.context.options.reasoning_explicit = true;
    }
    context->request = std::move(rebuilt.context);
    return ok_error();
}

Error apply_editor_model(std::optional<AiContinueContext>& context, const std::string& model) {
    if (!editor_ai_has_provider(context)) {
        return {ErrorCode::UnsupportedFeature, editor_no_provider_message()};
    }
    const bool changed = context->request.options.model != model;
    context->request.options.model = model;
    if (changed) {
        context->request.options.reasoning = ReasoningSelection::automatic();
        context->request.options.reasoning_explicit = true;
    }
    const Error context_err = provider::resolve_context_window(context->request, model);
    if (!context_err.ok()) {
        return context_err;
    }
    return ok_error();
}

}  // namespace ainiux::editor
