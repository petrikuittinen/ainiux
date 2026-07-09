#include "tui/session_load.hpp"

#include "chat/settings.hpp"
#include "provider/provider.hpp"

#include <cctype>
#include <sstream>

namespace pkchat::tui {

namespace {

std::string normalize_provider_key(std::string name) {
    for (char& ch : name) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        } else if (ch == '-') {
            ch = '_';
        }
    }
    return name;
}

std::string canonical_provider_key(const std::string& name) {
    const std::string lookup = normalize_provider_key(name.empty() ? "openai" : name);
    for (const provider::Profile& profile : provider::built_in_profiles()) {
        if (normalize_provider_key(profile.name) == lookup) {
            return profile.name;
        }
        for (const std::string& alias : profile.aliases) {
            if (normalize_provider_key(alias) == lookup) {
                return profile.name;
            }
        }
    }
    return lookup;
}

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

bool loaded_session_differs_from_context(const provider::RequestContext& active,
                                         const chat::Session& loaded) {
    if (!active_context_has_provider_selection(active)) {
        return false;
    }
    const bool provider_differs = !loaded.provider.empty() &&
                                  canonical_provider_key(loaded.provider) !=
                                      canonical_provider_key(active.profile.name);
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
    if (!loaded.provider.empty()) {
        next.provider = loaded.provider;
    }
    if (!loaded.base_url.empty()) {
        next.base_url = loaded.base_url;
        next.positional_url.clear();
    }
    if (!loaded.model.empty()) {
        next.model = loaded.model;
    }
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

}  // namespace pkchat::tui