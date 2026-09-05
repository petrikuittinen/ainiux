#include "server/model_settings.hpp"

#include "chat/settings_fields.hpp"
#include "config/model_catalog.hpp"
#include "provider/provider.hpp"
#include "security/hash.hpp"

namespace ainiux::server {
namespace {
bool exposed(const chat::SettingsFieldSpec& field) {
    return field.group == chat::SettingsGroup::Model ||
           std::string(field.id) == "context_tokens" ||
           std::string(field.id) == "auto_convert_html_to_md";
}
}

std::string public_model_settings(const cli::Options& options) {
    std::string result = "{";
    bool first = true;
    for (const auto& field : chat::all_settings_fields()) {
        if (!exposed(field)) continue;
        if (!first) result += ',';
        first = false;
        result += json::quote(field.id) + ":" +
                  json::quote(chat::settings_field_stored_value(field, options, {}));
    }
    return result + "}";
}

std::string public_model_fields(const cli::Options& options) {
    std::string result = "[";
    bool first = true;
    for (const auto& field : chat::all_settings_fields()) {
        if (!exposed(field)) continue;
        if (!first) result += ',';
        first = false;
        result += "{\"id\":" + json::quote(field.id) +
                  ",\"optional\":" + (field.optional ? std::string("true") : "false") +
                  ",\"hint\":" + json::quote(chat::settings_field_constraint_hint(field, options)) +
                  ",\"choices\":[";
        bool first_choice = true;
        for (const auto& choice : chat::settings_field_choices(field, options)) {
            if (!first_choice) result += ',';
            first_choice = false;
            result += json::quote(choice);
        }
        result += "]}";
    }
    return result + "]";
}

Error apply_public_model_settings(const json::Value& values, cli::Options& options) {
    if (!values.is_object()) return {ErrorCode::BadArgs, "settings must be an object"};
    cli::Options draft = options;
    chat::SettingsEditorLocals locals;
    for (const auto& entry : values.object) {
        const chat::SettingsFieldSpec* spec = nullptr;
        for (const auto& field : chat::all_settings_fields())
            if (exposed(field) && entry.first == field.id) spec = &field;
        if (spec == nullptr) return {ErrorCode::BadArgs, "unsupported model setting: " + entry.first};
        if (!entry.second.is_string() || entry.second.string.size() > 128)
            return {ErrorCode::BadArgs, "setting " + entry.first + " must be a string of at most 128 bytes"};
        Error error = chat::apply_settings_field(*spec, draft, locals, entry.second.string);
        if (!error.ok()) return error;
    }
    options = std::move(draft);
    return ok_error();
}

std::string model_settings_revision(const cli::Options& options) {
    return security::sha256_hex(json::quote(options.provider) + ":" + json::quote(options.model) + ":" + public_model_settings(options));
}

std::string public_model_configuration(const cli::Options& options) {
    return "{\"provider\":" + json::quote(options.provider) +
        ",\"model\":" + json::quote(options.model) +
        ",\"settings\":" + public_model_settings(options) +
        ",\"settings_fields\":" + public_model_fields(options) +
        ",\"revision\":" + json::quote(model_settings_revision(options)) + "}";
}

Error apply_public_model_target(const json::Value& root, cli::Options& options) {
    if (const json::Value* value = root.get("provider")) {
        if (!value->is_string() || value->string.size() > 128)
            return {ErrorCode::BadArgs, "provider must be a configured profile name"};
        if (!value->string.empty() && value->string != options.provider) {
            const std::string canonical = provider::canonical_profile_name(value->string);
            bool known = false;
            for (const auto& profile : provider::built_in_profiles()) if (profile.name == canonical) known = true;
            if (!known) return {ErrorCode::BadArgs, "unknown provider profile"};
            provider::apply_provider_target(options, value->string);
            options.provider_explicit = true;
        }
    }
    if (const json::Value* value = root.get("model")) {
        if (!value->is_string() || value->string.size() > 512)
            return {ErrorCode::BadArgs, "model must be a string of at most 512 bytes"};
        if (options.model != value->string) {
            options.model = value->string;
            options.model_explicit = true;
            options.reasoning = ReasoningSelection::automatic();
            options.reasoning_explicit = true;
        }
    }
    if (const auto* settings = root.get("settings")) return apply_public_model_settings(*settings, options);
    return ok_error();
}
}  // namespace ainiux::server
