#include "provider/model_selection.hpp"

#include <cmath>
#include <limits>
#include <sstream>

#include "config/model_catalog.hpp"
#include "json/json.hpp"
#include "provider/provider.hpp"

namespace ainiux::provider {

ModelSelection model_selection_from_options(const cli::Options& options) {
    return {options.provider, options.model, options.api, options.reasoning};
}

void apply_model_selection(cli::Options& options, const ModelSelection& selection) {
    options.provider = selection.provider;
    options.model = selection.model;
    options.api = selection.api;
    options.reasoning = selection.reasoning;
    options.reasoning_explicit = !selection.reasoning.is_auto();
}

bool can_restore_model_selection(const cli::Options& configured_options,
                                 const ModelSelection& selection) {
    const std::string canonical = canonical_profile_name(selection.provider);
    Profile profile;
    bool found = false;
    for (const Profile& candidate : built_in_profiles()) {
        if (candidate.name == canonical) {
            profile = candidate;
            found = true;
            break;
        }
    }
    if (!found) return false;
    if (profile.offline) return true;

    const bool has_base = !configured_options.base_url.empty() || !profile.base_url.empty();
    const bool request_endpoint = selection.api == "responses"
                                      ? !configured_options.responses_url.empty() ||
                                            (has_base && profile.capabilities.responses_api)
                                      : !configured_options.chat_url.empty() ||
                                            (has_base && profile.capabilities.chat_completions);
    if (!request_endpoint) return false;

    return !selection.model.empty() || !configured_options.models_url.empty() ||
           (has_base && profile.capabilities.model_listing);
}

void apply_cli_target_change(cli::Options& options,
                             const cli::Options& previous_options,
                             bool positional_target_changed) {
    const bool provider_or_endpoint_changed =
        positional_target_changed ||
        (options.provider_explicit &&
         canonical_profile_name(options.provider) !=
             canonical_profile_name(previous_options.provider));
    const bool model_changed =
        options.model_explicit && options.model != previous_options.model;
    const bool api_changed =
        options.api_explicit && options.api != previous_options.api;

    if (provider_or_endpoint_changed && !options.model_explicit) {
        // Never carry a remembered or configured model id across a provider or
        // endpoint switch. Interactive startup then discovers models the same
        // way chat does (and auto-selects a single result).
        options.model.clear();
    }
    if ((provider_or_endpoint_changed || model_changed || api_changed) &&
        !options.reasoning_cli_explicit) {
        options.reasoning = ReasoningSelection::automatic();
        options.reasoning_explicit = true;
    }
}

std::string serialize_model_selection(const ModelSelection& selection) {
    std::ostringstream out;
    out << "{\"provider\":" << json::quote(selection.provider)
        << ",\"model\":" << json::quote(selection.model)
        << ",\"api\":" << json::quote(selection.api)
        << ",\"reasoning\":";
    if (selection.reasoning.kind == ReasoningSelectionKind::Named) {
        out << json::quote(selection.reasoning.value);
    } else if (selection.reasoning.kind == ReasoningSelectionKind::TokenBudget) {
        out << selection.reasoning.tokens;
    } else {
        out << "null";
    }
    out << '}';
    return out.str();
}

Error parse_model_selection(const std::string& text, ModelSelection& selection) {
    const json::ParseResult parsed = json::parse(text);
    if (!parsed.error.ok()) return parsed.error;
    if (!parsed.value.is_object()) {
        return {ErrorCode::ProviderSchema, "model selection must be a JSON object"};
    }
    const json::Value* provider = parsed.value.get("provider");
    const json::Value* model = parsed.value.get("model");
    const json::Value* api = parsed.value.get("api");
    const json::Value* reasoning = parsed.value.get("reasoning");
    if (provider == nullptr || !provider->is_string() || provider->string.empty()) {
        return {ErrorCode::ProviderSchema, "model selection provider must be a non-empty string"};
    }
    if (model == nullptr || !model->is_string()) {
        return {ErrorCode::ProviderSchema, "model selection model must be a string"};
    }
    if (api == nullptr || !api->is_string() ||
        (api->string != "chat" && api->string != "responses")) {
        return {ErrorCode::ProviderSchema, "model selection api must be chat or responses"};
    }
    ModelSelection candidate{provider->string, model->string, api->string,
                             ReasoningSelection::automatic()};
    if (reasoning != nullptr && !reasoning->is_null()) {
        if (reasoning->is_string()) {
            Error err = config::parse_reasoning_selection(reasoning->string,
                                                          candidate.reasoning,
                                                          false);
            if (!err.ok()) return {ErrorCode::ProviderSchema, err.message};
        } else if (reasoning->type == json::Value::Type::Number &&
                   std::isfinite(reasoning->number) && reasoning->number >= 0.0 &&
                   std::floor(reasoning->number) == reasoning->number &&
                   reasoning->number <= static_cast<double>(std::numeric_limits<long long>::max())) {
            candidate.reasoning = ReasoningSelection::token_budget(
                static_cast<long long>(reasoning->number));
        } else {
            return {ErrorCode::ProviderSchema,
                    "model selection reasoning must be a string, integer, or null"};
        }
    }
    selection = std::move(candidate);
    return ok_error();
}

}  // namespace ainiux::provider
