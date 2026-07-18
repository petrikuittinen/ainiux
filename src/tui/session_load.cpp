#include "tui/session_load.hpp"

#include "chat/settings.hpp"
#include "provider/provider.hpp"

#include <cctype>
#include <sstream>

namespace ainiux::tui {

namespace {

std::string format_provider_model_line(const std::string& provider_name, const std::string& model_name) {
    std::ostringstream out;
    if (!provider_name.empty()) {
        out << provider::display_name_for_profile(provider_name);
    } else {
        out << "unknown provider";
    }
    if (!model_name.empty()) {
        out << " / " << model_name;
    } else {
        out << " / unknown model";
    }
    return out.str();
}

}  // namespace

bool active_context_has_provider_selection(const provider::RequestContext& active) {
    return !active.profile.offline;
}

bool saved_provider_model_complete(const std::string& provider_name,
                                   const std::string& model_name) {
    const bool has_provider = !provider_name.empty() &&
                              provider::canonical_profile_name(provider_name) != "none";
    return has_provider && !model_name.empty();
}

std::string saved_provider_model_missing(const std::string& provider_name,
                                         const std::string& model_name) {
    const bool provider_missing = provider_name.empty() ||
                                  provider::canonical_profile_name(provider_name) == "none";
    const bool model_missing = model_name.empty();
    if (provider_missing && model_missing) {
        return "provider+model missing";
    }
    if (provider_missing) {
        return "provider missing";
    }
    if (model_missing) {
        return "model missing";
    }
    return "";
}

bool session_has_complete_provider_model(const chat::Session& session) {
    return saved_provider_model_complete(session.provider, session.model);
}

bool loaded_session_differs_from_context(const provider::RequestContext& active,
                                         const chat::Session& loaded) {
    if (!active_context_has_provider_selection(active)) {
        return false;
    }
    const bool provider_differs = !loaded.provider.empty() &&
                                  provider::canonical_profile_name(loaded.provider) !=
                                      provider::canonical_profile_name(active.profile.name);
    const bool model_differs = !active.options.model.empty() && !loaded.model.empty() &&
                               loaded.model != active.options.model;
    return provider_differs || model_differs;
}

bool loaded_session_differs_from_cli(const provider::RequestContext& cli_context,
                                     const chat::Session& loaded) {
    return loaded_session_differs_from_context(cli_context, loaded);
}

std::string model_confirm_text(const provider::RequestContext& active,
                               const chat::Session& loaded) {
    std::ostringstream out;
    out << "Thread model: " << format_provider_model_line(loaded.provider, loaded.model) << "\n";
    out << "Current: " << format_provider_model_line(active.profile.name, active.options.model) << "\n";
    out << "Keep current provider and model?\n";
    out << "Press y to keep current · n or Esc to use thread model";
    return out.str();
}

Error apply_loaded_session_to_context(provider::RequestContext& context, const chat::Session& loaded) {
    cli::Options next = context.options;
    const bool has_loaded_provider = !loaded.provider.empty() &&
                                     provider::canonical_profile_name(loaded.provider) != "none";
    const std::string loaded_provider = has_loaded_provider ? loaded.provider : "none";
    provider::apply_provider_target(next, loaded_provider);
    if (has_loaded_provider && !loaded.base_url.empty()) {
        next.base_url = loaded.base_url;
        next.positional_url.clear();
    }
    // An empty persisted model is meaningful. Never inherit a model (or an
    // endpoint override cleared above) from the previously active thread.
    next.model = loaded.model;
    Error settings_error = chat::apply_settings_json(next, loaded.settings_json);
    if (!settings_error.ok()) {
        return settings_error;
    }
    provider::ContextResult rebuilt = provider::build_context(next);
    if (!rebuilt.error.ok()) {
        return rebuilt.error;
    }
    context = std::move(rebuilt.context);
    return ok_error();
}

void restore_cli_context(provider::RequestContext& context, const provider::RequestContext& cli_context) {
    context = cli_context;
}

}  // namespace ainiux::tui
