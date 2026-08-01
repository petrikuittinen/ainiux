#include "chat/settings.hpp"
#include "chat/generation_settings.hpp"
#include "config/model_catalog.hpp"

#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

#include "json/json.hpp"

namespace ainiux::chat {
namespace {

bool parse_bool_setting(const std::string& value, bool& output) {
    if (value == "on") {
        output = true;
        return true;
    }
    if (value == "off") {
        output = false;
        return true;
    }
    return false;
}

Error invalid_setting_value(const std::string& name, const std::string& detail) {
    return {ErrorCode::BadArgs, "invalid " + name + " setting: " + detail};
}

void append_json_number(std::ostringstream& out, bool& first, const char* key, double value) {
    if (!first) {
        out << ',';
    }
    first = false;
    out << json::quote(key) << ':' << std::setprecision(17) << value;
}

void append_json_int(std::ostringstream& out, bool& first, const char* key, long long value) {
    if (!first) {
        out << ',';
    }
    first = false;
    out << json::quote(key) << ':' << value;
}

void append_json_bool(std::ostringstream& out, bool& first, const char* key, bool value) {
    if (!first) {
        out << ',';
    }
    first = false;
    out << json::quote(key) << ':' << (value ? "true" : "false");
}

void append_json_string(std::ostringstream& out, bool& first, const char* key, const std::string& value) {
    if (!first) {
        out << ',';
    }
    first = false;
    out << json::quote(key) << ':' << json::quote(value);
}

void append_json_null(std::ostringstream& out, bool& first, const char* key) {
    if (!first) {
        out << ',';
    }
    first = false;
    out << json::quote(key) << ":null";
}

void append_optional_bool(std::ostringstream& out, bool& first, const char* key, bool has_value, bool value) {
    if (has_value) {
        append_json_bool(out, first, key, value);
    } else {
        append_json_null(out, first, key);
    }
}

void append_optional_number(std::ostringstream& out, bool& first, const char* key, bool has_value, double value) {
    if (has_value) {
        append_json_number(out, first, key, value);
    } else {
        append_json_null(out, first, key);
    }
}

void append_optional_int(std::ostringstream& out, bool& first, const char* key, bool has_value, long long value) {
    if (has_value) {
        append_json_int(out, first, key, value);
    } else {
        append_json_null(out, first, key);
    }
}

void append_optional_string(std::ostringstream& out,
                            bool& first,
                            const char* key,
                            bool has_value,
                            const std::string& value) {
    if (has_value) {
        append_json_string(out, first, key, value);
    } else {
        append_json_null(out, first, key);
    }
}

bool is_null_setting_value(const std::string& value) {
    if (value.size() != 4) {
        return false;
    }
    for (size_t i = 0; i < value.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(value[i]);
        if (ch >= 'A' && ch <= 'Z') {
            if (ch + ('a' - 'A') != static_cast<unsigned char>("null"[i])) {
                return false;
            }
        } else if (ch != static_cast<unsigned char>("null"[i])) {
            return false;
        }
    }
    return true;
}

void reconcile_preset_temperature(cli::Options& options) {
    if (!options.temperature_preset_applied) return;
    const ModelCapability* capability = config::resolve_model_capability(
        options.model_catalog, options.provider, options.api, options.model);
    if (capability == nullptr || !options.has_chat_purpose) return;
    const ModelSetting* preset = config::find_model_preset(
        options.model_catalog, *capability, options.chat_purpose);
    if (preset == nullptr || !preset->temperature.has_value()) return;
    if (config::temperature_supported_for(*capability, options.reasoning)) {
        options.temperature = *preset->temperature;
        options.has_temperature = true;
    } else {
        options.has_temperature = false;
    }
}

}  // namespace

void reset_thread_setting_overrides(cli::Options& options) {
    options.stream_explicit = false;
    options.has_temperature = false;
    options.temperature_preset_applied = false;
    options.has_top_p = false;
    options.has_top_k = false;
    options.has_min_p = false;
    options.has_repeat_penalty = false;
    options.has_presence_penalty = false;
    options.has_max_output_tokens = false;
    options.reasoning = ReasoningSelection::automatic();
    options.reasoning_explicit = false;
    options.has_chat_purpose = false;
    options.chat_purpose.clear();
    options.has_context_tokens = false;
    options.context_tokens = 0;
    options.has_show_thinking_traces = false;
    options.has_agent_thinking_preview_max_chars = false;
}

Error apply_model_setting_preset(cli::Options& options,
                                 const ModelSetting& preset,
                                 const ModelCapability* capability) {
    options.chat_purpose = preset.purpose;
    options.has_chat_purpose = true;
    if (preset.reasoning.has_value() && !options.reasoning_explicit) {
        options.reasoning = *preset.reasoning;
    }
    const bool temperature_allowed = capability == nullptr ||
        config::temperature_supported_for(*capability, options.reasoning);
    if (!options.has_temperature || options.temperature_preset_applied) {
        if (preset.temperature.has_value() && temperature_allowed) {
            options.temperature = *preset.temperature;
            options.has_temperature = true;
            options.temperature_preset_applied = true;
        } else {
            options.has_temperature = false;
            // Keep ownership while a catalog preset is temporarily suppressed
            // so a later reasoning=none can restore its exact temperature.
            options.temperature_preset_applied = preset.temperature.has_value();
        }
    }
    if (preset.top_k.has_value()) { options.top_k = *preset.top_k; options.has_top_k = true; }
    if (preset.top_p.has_value()) { options.top_p = *preset.top_p; options.has_top_p = true; }
    if (preset.min_p.has_value()) { options.min_p = *preset.min_p; options.has_min_p = true; }
    if (preset.repeat_penalty.has_value()) {
        options.repeat_penalty = *preset.repeat_penalty;
        options.has_repeat_penalty = true;
    }
    if (preset.presence_penalty.has_value()) {
        options.presence_penalty = *preset.presence_penalty;
        options.has_presence_penalty = true;
    }
    if (preset.default_system_prompt.has_value() && !preset.default_system_prompt->empty()) {
        options.system = *preset.default_system_prompt;
    }
    return ok_error();
}

std::string trim_setting_ascii(std::string text) {
    auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    while (!text.empty() && is_space(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
    }
    while (!text.empty() && is_space(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    return text;
}

Error apply_chat_setting(cli::Options& options, const std::string& raw_name, const std::string& raw_value) {
    std::string name = raw_name;
    std::string value = trim_setting_ascii(raw_value);
    for (char& ch : name) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (name == "auto-convert-html-to-md") {
        bool enabled = false;
        if (!parse_bool_setting(value, enabled)) {
            return invalid_setting_value(name, "expected on or off");
        }
        options.auto_convert_html_to_markdown = enabled;
        return ok_error();
    }
    if (name == "stream" || name == "show_thinking_traces" || name == "highlight" ||
        name == "cmd-out") {
        bool enabled = false;
        if (!parse_bool_setting(value, enabled)) {
            return invalid_setting_value(name, "expected on or off");
        }
        if (name == "stream") {
            options.stream = enabled;
            options.stream_explicit = true;
        } else if (name == "show_thinking_traces") {
            options.show_thinking_traces = enabled;
            options.has_show_thinking_traces = true;
        } else if (name == "highlight") {
            options.tui_highlight = enabled;
        } else {
            options.agent_show_command_output = enabled;
        }
        return ok_error();
    }
    if (name == "thinking_preview_max_chars") {
        long long parsed = 0;
        if (value.empty()) {
            return invalid_setting_value(name, "expected an integer from 0 through 1000");
        }
        for (char ch : value) {
            if (ch < '0' || ch > '9') {
                return invalid_setting_value(name, "expected an integer from 0 through 1000");
            }
            parsed = parsed * 10 + (ch - '0');
            if (parsed > 1000)
                return invalid_setting_value(name, "expected an integer from 0 through 1000");
        }
        options.agent_thinking_preview_max_chars = static_cast<int>(parsed);
        options.has_agent_thinking_preview_max_chars = true;
        return ok_error();
    }
    if (is_null_setting_value(value)) {
        if (name == generation::kMaxTokens || name == generation::kMaxOutputTokens) {
            options.has_max_output_tokens = false;
            return ok_error();
        }
        if (name == generation::kReasoning) {
            options.reasoning = ReasoningSelection::automatic();
            options.reasoning_explicit = true;
            return ok_error();
        }
        if (name == generation::kTemperature) {
            options.has_temperature = false;
            options.temperature_preset_applied = false;
            return ok_error();
        }
        if (name == generation::kTopK) {
            options.has_top_k = false;
            return ok_error();
        }
        if (name == generation::kTopP) {
            options.has_top_p = false;
            return ok_error();
        }
        if (name == generation::kMinP) {
            options.has_min_p = false;
            return ok_error();
        }
        if (name == generation::kRepeatPenalty) {
            options.has_repeat_penalty = false;
            return ok_error();
        }
        if (name == generation::kPresencePenalty) {
            options.has_presence_penalty = false;
            return ok_error();
        }
        return invalid_setting_value(name,
                                     "unknown setting; expected " + generation::chat_setting_names_description() +
                                         ", stream, show_thinking_traces, highlight, cmd-out, or auto-convert-html-to-md");
    }
    if (name == generation::kMaxTokens || name == generation::kMaxOutputTokens) {
        int parsed = 0;
        try {
            parsed = std::stoi(value);
        } catch (const std::exception&) {
            return invalid_setting_value(name, "expected an integer");
        }
        if (parsed < 0) {
            return invalid_setting_value(name, "expected a non-negative integer");
        }
        options.max_output_tokens = parsed;
        options.has_max_output_tokens = true;
        return ok_error();
    }
    if (name == generation::kReasoning) {
        ReasoningSelection selection;
        Error err = config::parse_reasoning_selection(value, selection);
        if (!err.ok()) {
            return err;
        }
        err = config::resolve_reasoning_off(options.model_catalog,
                                            options.provider,
                                            options.api,
                                            options.model,
                                            selection);
        if (!err.ok()) return err;
        options.reasoning = std::move(selection);
        options.reasoning_explicit = true;
        reconcile_preset_temperature(options);
        return ok_error();
    }
    if (name == generation::kTemperature) {
        try {
            options.temperature = std::stod(value);
        } catch (const std::exception&) {
            return invalid_setting_value(name, "expected a floating-point number");
        }
        if (!std::isfinite(options.temperature)) {
            return invalid_setting_value(name, "expected a finite number");
        }
        options.has_temperature = true;
        options.temperature_preset_applied = false;
        return ok_error();
    }
    if (name == generation::kTopK) {
        try {
            const long long parsed = std::stoll(value);
            if (parsed < 0 || parsed > std::numeric_limits<int>::max()) {
                return invalid_setting_value(name, "expected a non-negative integer");
            }
            options.top_k = static_cast<int>(parsed);
        } catch (const std::exception&) {
            return invalid_setting_value(name, "expected an integer");
        }
        options.has_top_k = true;
        return ok_error();
    }
    if (name == generation::kTopP || name == generation::kMinP || name == generation::kRepeatPenalty ||
        name == generation::kPresencePenalty) {
        double parsed = 0.0;
        try {
            parsed = std::stod(value);
        } catch (const std::exception&) {
            return invalid_setting_value(name, "expected a floating-point number");
        }
        if (!std::isfinite(parsed)) {
            return invalid_setting_value(name, "expected a finite number");
        }
        if (name == generation::kTopP) {
            options.top_p = parsed;
            options.has_top_p = true;
        } else if (name == generation::kMinP) {
            options.min_p = parsed;
            options.has_min_p = true;
        } else if (name == generation::kRepeatPenalty) {
            options.repeat_penalty = parsed;
            options.has_repeat_penalty = true;
        } else {
            options.presence_penalty = parsed;
            options.has_presence_penalty = true;
        }
        return ok_error();
    }
    return invalid_setting_value(name,
                                 "unknown setting; expected " + generation::chat_setting_names_description() +
                                     ", stream, show_thinking_traces, highlight, cmd-out, or auto-convert-html-to-md");
}

std::string settings_json_from_options(const cli::Options& options) {
    std::ostringstream out;
    out << '{';
    bool first = true;
    append_optional_bool(out, first, "stream", options.stream_explicit, options.stream);
    append_optional_number(out, first, "temperature", options.has_temperature, options.temperature);
    append_optional_number(out, first, "top_p", options.has_top_p, options.top_p);
    append_optional_int(out, first, "top_k", options.has_top_k, options.top_k);
    append_optional_number(out, first, "min_p", options.has_min_p, options.min_p);
    append_optional_number(out, first, "repeat_penalty", options.has_repeat_penalty, options.repeat_penalty);
    append_optional_number(out, first, "presence_penalty", options.has_presence_penalty, options.presence_penalty);
    append_optional_int(out, first, "max_output_tokens", options.has_max_output_tokens, options.max_output_tokens);
    if (options.reasoning.kind == ReasoningSelectionKind::Named) {
        append_json_string(out, first, "reasoning", options.reasoning.value);
    } else if (options.reasoning.kind == ReasoningSelectionKind::TokenBudget) {
        append_json_int(out, first, "reasoning", options.reasoning.tokens);
    } else {
        append_json_null(out, first, "reasoning");
    }
    append_optional_bool(out, first, "show_thinking_traces", options.has_show_thinking_traces,
                         options.show_thinking_traces);
    append_optional_string(out, first, "purpose", options.has_chat_purpose, options.chat_purpose);
    append_optional_int(out, first, "context_tokens", options.has_context_tokens, options.context_tokens);
    append_optional_int(out, first, "thinking_preview_max_chars",
                        options.has_agent_thinking_preview_max_chars,
                        options.agent_thinking_preview_max_chars);
    append_json_bool(out, first, "auto_convert_html_to_md", options.auto_convert_html_to_markdown);
    out << '}';
    return out.str();
}

Error apply_settings_json(cli::Options& options, const std::string& settings_json) {
    reset_thread_setting_overrides(options);
    if (settings_json.empty() || settings_json == "{}") {
        return ok_error();
    }
    json::ParseResult parsed = json::parse(settings_json);
    if (!parsed.error.ok()) {
        return parsed.error;
    }
    if (!parsed.value.is_object()) {
        return {ErrorCode::ProviderSchema, "chat settings must be a JSON object"};
    }
    const json::Value& root = parsed.value;
    if (const json::Value* stream = root.get("stream")) {
        if (stream->is_null()) {
            // cleared override
        } else if (stream->type != json::Value::Type::Bool) {
            return {ErrorCode::ProviderSchema, "chat settings stream must be a boolean or null"};
        } else {
            options.stream = stream->boolean;
            options.stream_explicit = true;
        }
    }
    if (const json::Value* temperature = root.get("temperature")) {
        if (temperature->is_null()) {
            // cleared override
        } else if (temperature->type != json::Value::Type::Number) {
            return {ErrorCode::ProviderSchema, "chat settings temperature must be a number or null"};
        } else {
            options.temperature = temperature->number;
            options.has_temperature = true;
            options.temperature_preset_applied = false;
        }
    }
    if (const json::Value* top_p = root.get("top_p")) {
        if (top_p->is_null()) {
            // cleared override
        } else if (top_p->type != json::Value::Type::Number) {
            return {ErrorCode::ProviderSchema, "chat settings top_p must be a number or null"};
        } else {
            options.top_p = top_p->number;
            options.has_top_p = true;
        }
    }
    if (const json::Value* convert = root.get("auto_convert_html_to_md")) {
        if (convert->type != json::Value::Type::Bool) {
            return {ErrorCode::ProviderSchema,
                    "chat settings auto_convert_html_to_md must be a boolean"};
        }
        options.auto_convert_html_to_markdown = convert->boolean;
    }
    if (const json::Value* top_k = root.get("top_k")) {
        if (top_k->is_null()) {
            // cleared override
        } else if (top_k->type != json::Value::Type::Number || top_k->number < 0.0) {
            return {ErrorCode::ProviderSchema, "chat settings top_k must be a non-negative integer or null"};
        } else {
            options.top_k = static_cast<int>(top_k->number);
            options.has_top_k = true;
        }
    }
    if (const json::Value* min_p = root.get("min_p")) {
        if (min_p->is_null()) {
            // cleared override
        } else if (min_p->type != json::Value::Type::Number) {
            return {ErrorCode::ProviderSchema, "chat settings min_p must be a number or null"};
        } else {
            options.min_p = min_p->number;
            options.has_min_p = true;
        }
    }
    if (const json::Value* repeat_penalty = root.get("repeat_penalty")) {
        if (repeat_penalty->is_null()) {
            // cleared override
        } else if (repeat_penalty->type != json::Value::Type::Number) {
            return {ErrorCode::ProviderSchema, "chat settings repeat_penalty must be a number or null"};
        } else {
            options.repeat_penalty = repeat_penalty->number;
            options.has_repeat_penalty = true;
        }
    }
    if (const json::Value* presence_penalty = root.get("presence_penalty")) {
        if (presence_penalty->is_null()) {
            // cleared override
        } else if (presence_penalty->type != json::Value::Type::Number) {
            return {ErrorCode::ProviderSchema, "chat settings presence_penalty must be a number or null"};
        } else {
            options.presence_penalty = presence_penalty->number;
            options.has_presence_penalty = true;
        }
    }
    if (const json::Value* max_output_tokens = root.get("max_output_tokens")) {
        if (max_output_tokens->is_null()) {
            // cleared override
        } else if (max_output_tokens->type != json::Value::Type::Number || max_output_tokens->number < 0.0) {
            return {ErrorCode::ProviderSchema,
                    "chat settings max_output_tokens must be a non-negative integer or null"};
        } else {
            options.max_output_tokens = static_cast<int>(max_output_tokens->number);
            options.has_max_output_tokens = true;
        }
    }
    if (const json::Value* reasoning = root.get("reasoning")) {
        if (reasoning->is_null()) {
            options.reasoning = ReasoningSelection::automatic();
        } else if (reasoning->type == json::Value::Type::Number) {
            if (!std::isfinite(reasoning->number) || reasoning->number < 0.0 ||
                std::floor(reasoning->number) != reasoning->number ||
                reasoning->number > static_cast<double>(std::numeric_limits<long long>::max())) {
                return {ErrorCode::ProviderSchema,
                        "chat settings reasoning must be a non-negative integer token budget"};
            }
            options.reasoning = ReasoningSelection::token_budget(
                static_cast<long long>(reasoning->number));
        } else if (reasoning->type == json::Value::Type::String) {
            Error err = config::parse_reasoning_selection(reasoning->string, options.reasoning, false);
            if (!err.ok()) {
                return {ErrorCode::ProviderSchema, "chat settings " + err.message};
            }
        } else {
            return {ErrorCode::ProviderSchema,
                    "chat settings reasoning must be a string, integer, or null"};
        }
        options.reasoning_explicit = true;
    } else if (const json::Value* legacy_budget = root.get("thinking_budget")) {
        // Read old thread files without retaining the retired runtime fields.
        if (legacy_budget->type == json::Value::Type::Number && legacy_budget->number >= 0.0 &&
            std::floor(legacy_budget->number) == legacy_budget->number) {
            options.reasoning = ReasoningSelection::token_budget(
                static_cast<long long>(legacy_budget->number));
            options.reasoning_explicit = true;
        } else if (legacy_budget->type == json::Value::Type::String) {
            Error err = config::parse_reasoning_selection(legacy_budget->string, options.reasoning, false);
            if (!err.ok()) return {ErrorCode::ProviderSchema, "chat settings " + err.message};
            options.reasoning_explicit = true;
        }
    } else if (const json::Value* legacy_toggle = root.get("enable_thinking")) {
        if (legacy_toggle->type == json::Value::Type::Bool) {
            options.reasoning = ReasoningSelection::named(
                legacy_toggle->boolean ? "enabled" : "none");
            options.reasoning_explicit = true;
        }
    }
    if (const json::Value* show_thinking_traces = root.get("show_thinking_traces")) {
        if (show_thinking_traces->is_null()) {
            // cleared override
        } else if (show_thinking_traces->type != json::Value::Type::Bool) {
            return {ErrorCode::ProviderSchema, "chat settings show_thinking_traces must be a boolean or null"};
        } else {
            options.show_thinking_traces = show_thinking_traces->boolean;
            options.has_show_thinking_traces = true;
        }
    }
    if (const json::Value* purpose = root.get("purpose")) {
        if (purpose->is_null()) {
            // cleared override
        } else if (purpose->type != json::Value::Type::String) {
            return {ErrorCode::ProviderSchema, "chat settings purpose must be a string or null"};
        } else {
            options.chat_purpose = purpose->string;
            options.has_chat_purpose = true;
        }
    }
    if (const json::Value* context_tokens = root.get("context_tokens")) {
        if (context_tokens->is_null()) {
            // cleared override
        } else if (context_tokens->type != json::Value::Type::Number || context_tokens->number < 0.0) {
            return {ErrorCode::ProviderSchema, "chat settings context_tokens must be a non-negative integer or null"};
        } else {
            options.context_tokens = static_cast<long long>(context_tokens->number);
            options.has_context_tokens = true;
        }
    }
    if (const json::Value* preview = root.get("thinking_preview_max_chars")) {
        if (preview->is_null()) {
            // Global [tui] fallback remains active.
        } else if (preview->type != json::Value::Type::Number ||
                   !std::isfinite(preview->number) || preview->number < 0.0 ||
                   preview->number > 1000.0 ||
                   std::floor(preview->number) != preview->number) {
            return {ErrorCode::ProviderSchema,
                    "chat settings thinking_preview_max_chars must be an integer from 0 through 1000 or null"};
        } else {
            options.agent_thinking_preview_max_chars =
                static_cast<int>(preview->number);
            options.has_agent_thinking_preview_max_chars = true;
        }
    }
    return ok_error();
}

std::string current_system_prompt(const Session& session) {
    for (const provider::Message& message : session.messages) {
        if (message.role == "system") {
            return message.content;
        }
    }
    return {};
}

std::string format_settings_summary(const cli::Options& options) {
    std::ostringstream out;
    out << "Settings";
    out << " purpose=" << (options.has_chat_purpose ? options.chat_purpose : "default");
    out << " temperature=" << (options.has_temperature ? std::to_string(options.temperature) : "default");
    out << " top_k=" << (options.has_top_k ? std::to_string(options.top_k) : "default");
    out << " top_p=" << (options.has_top_p ? std::to_string(options.top_p) : "default");
    out << " min_p=" << (options.has_min_p ? std::to_string(options.min_p) : "default");
    out << " repeat_penalty="
        << (options.has_repeat_penalty ? std::to_string(options.repeat_penalty) : "default");
    out << " presence_penalty="
        << (options.has_presence_penalty ? std::to_string(options.presence_penalty) : "default");
    out << " max_tokens="
        << (options.has_max_output_tokens ? std::to_string(options.max_output_tokens) : "default");
    out << " reasoning=" << config::reasoning_selection_value(options.reasoning);
    return out.str();
}

std::string format_settings_panel(const cli::Options& options,
                                  const std::string& advisory) {
    std::ostringstream out;
    out << "/setting (hide/show this panel)\n";
    auto append = [&](const char* name, const std::string& value) { out << name << '=' << value << '\n'; };
    append("stream", options.stream_explicit ? (options.stream ? "on" : "off") : "");
    append("temperature", options.has_temperature ? std::to_string(options.temperature) : "");
    append("top_p", options.has_top_p ? std::to_string(options.top_p) : "");
    append("top_k", options.has_top_k ? std::to_string(options.top_k) : "");
    append("min_p", options.has_min_p ? std::to_string(options.min_p) : "");
    append("repeat_penalty", options.has_repeat_penalty ? std::to_string(options.repeat_penalty) : "");
    append("presence_penalty",
           options.has_presence_penalty ? std::to_string(options.presence_penalty) : "");
    append("max_tokens", options.has_max_output_tokens ? std::to_string(options.max_output_tokens) : "");
    append("reasoning", config::reasoning_selection_value(options.reasoning));
    append("show_thinking_traces",
           options.has_show_thinking_traces ? (options.show_thinking_traces ? "on" : "off") : "");
    append("purpose", options.has_chat_purpose ? options.chat_purpose : "");
    append("context_tokens", options.has_context_tokens ? std::to_string(options.context_tokens) : "");
    if (options.agent) {
        append("thinking_preview_max_chars",
               options.has_agent_thinking_preview_max_chars
                   ? std::to_string(options.agent_thinking_preview_max_chars)
                   : "");
        append("cmd-out", options.agent_show_command_output ? "on" : "off");
    }
    append("highlight", options.tui_highlight ? "on" : "off");
    append("auto-convert-html-to-md", options.auto_convert_html_to_markdown ? "on" : "off");
    if (!advisory.empty()) append("warning", advisory);
    std::string text = out.str();
    if (!text.empty() && text.back() == '\n') {
        text.pop_back();
    }
    return text;
}

}  // namespace ainiux::chat
