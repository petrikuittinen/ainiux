#include "chat/settings.hpp"

#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

#include "json/json.hpp"

namespace pkchat::chat {
namespace {

bool parse_bool_setting(const std::string& value, bool& output) {
    if (value == "1" || value == "true" || value == "on" || value == "yes") {
        output = true;
        return true;
    }
    if (value == "0" || value == "false" || value == "off" || value == "no") {
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

}  // namespace

bool thinking_budget_is_token_count(const std::string& value) {
    if (value.empty()) {
        return false;
    }
    for (unsigned char ch : value) {
        if (ch < '0' || ch > '9') {
            return false;
        }
    }
    return true;
}

namespace {

Error validate_thinking_budget_value(const std::string& value) {
    if (value.empty()) {
        return {ErrorCode::BadArgs, "thinking_budget must not be empty"};
    }
    if (!thinking_budget_is_token_count(value)) {
        return ok_error();
    }
    try {
        const long long tokens = std::stoll(value);
        if (tokens < 0) {
            return {ErrorCode::BadArgs, "thinking_budget token count must be non-negative"};
        }
    } catch (const std::exception&) {
        return {ErrorCode::BadArgs, "thinking_budget token count is too large"};
    }
    return ok_error();
}

}  // namespace

bool model_pattern_matches(const std::string& pattern, const std::string& model) {
    if (pattern.empty() || model.empty()) {
        return false;
    }
    if (pattern.back() == '*') {
        const std::string prefix = pattern.substr(0, pattern.size() - 1);
        return model.size() >= prefix.size() && model.compare(0, prefix.size(), prefix) == 0;
    }
    return pattern == model;
}

const ModelSetting* find_model_setting(const std::string& model,
                                       const std::string& purpose,
                                       const std::vector<ModelSetting>& presets) {
    const ModelSetting* best = nullptr;
    size_t best_prefix = 0;
    for (const ModelSetting& preset : presets) {
        if (preset.purpose != purpose || !model_pattern_matches(preset.model, model)) {
            continue;
        }
        size_t prefix_len = preset.model.size();
        if (preset.model.back() == '*') {
            --prefix_len;
        }
        if (best == nullptr || prefix_len > best_prefix) {
            best = &preset;
            best_prefix = prefix_len;
        }
    }
    return best;
}

void reset_thread_setting_overrides(cli::Options& options) {
    options.stream_explicit = false;
    options.has_temperature = false;
    options.has_top_p = false;
    options.has_top_k = false;
    options.has_min_p = false;
    options.has_repeat_penalty = false;
    options.has_presence_penalty = false;
    options.has_max_output_tokens = false;
    options.has_enable_thinking = false;
    options.has_thinking_budget = false;
    options.thinking_budget.clear();
    options.has_chat_purpose = false;
    options.chat_purpose.clear();
    options.has_context_tokens = false;
    options.context_tokens = 0;
    options.has_show_thinking_traces = false;
}

Error apply_model_setting_preset(cli::Options& options, const ModelSetting& preset) {
    options.chat_purpose = preset.purpose;
    options.has_chat_purpose = true;
    options.temperature = preset.temperature;
    options.has_temperature = true;
    options.top_k = preset.top_k;
    options.has_top_k = true;
    options.top_p = preset.top_p;
    options.has_top_p = true;
    options.min_p = preset.min_p;
    options.has_min_p = true;
    options.repeat_penalty = preset.repeat_penalty;
    options.has_repeat_penalty = true;
    options.presence_penalty = preset.presence_penalty;
    options.has_presence_penalty = true;
    if (!preset.default_system_prompt.empty()) {
        options.system = preset.default_system_prompt;
    }
    if (!preset.thinking_budget.empty()) {
        options.thinking_budget = preset.thinking_budget;
        options.has_thinking_budget = true;
    }
    return ok_error();
}

void append_thinking_budget_json(std::ostringstream& out, const std::string& value) {
    out << ",\"thinking_budget\":";
    if (thinking_budget_is_token_count(value)) {
        out << value;
    } else {
        out << json::quote(value);
    }
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
    if (is_null_setting_value(value)) {
        if (name == "max_tokens" || name == "max_output_tokens") {
            options.has_max_output_tokens = false;
            return ok_error();
        }
        if (name == "thinking") {
            options.has_enable_thinking = false;
            return ok_error();
        }
        if (name == "thinking_budget") {
            options.has_thinking_budget = false;
            options.thinking_budget.clear();
            return ok_error();
        }
        if (name == "temperature") {
            options.has_temperature = false;
            return ok_error();
        }
        if (name == "top_k") {
            options.has_top_k = false;
            return ok_error();
        }
        if (name == "top_p") {
            options.has_top_p = false;
            return ok_error();
        }
        if (name == "min_p") {
            options.has_min_p = false;
            return ok_error();
        }
        if (name == "repeat_penalty") {
            options.has_repeat_penalty = false;
            return ok_error();
        }
        if (name == "presence_penalty") {
            options.has_presence_penalty = false;
            return ok_error();
        }
        return invalid_setting_value(name,
                                     "unknown setting; expected temperature, top_k, top_p, min_p, "
                                     "repeat_penalty, presence_penalty, max_tokens, thinking, or thinking_budget");
    }
    if (name == "max_tokens" || name == "max_output_tokens") {
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
    if (name == "thinking") {
        bool enabled = false;
        if (!parse_bool_setting(value, enabled)) {
            return invalid_setting_value(name, "expected on or off");
        }
        options.enable_thinking = enabled;
        options.has_enable_thinking = true;
        return ok_error();
    }
    if (name == "thinking_budget") {
        Error err = validate_thinking_budget_value(value);
        if (!err.ok()) {
            return err;
        }
        options.thinking_budget = value;
        options.has_thinking_budget = true;
        return ok_error();
    }
    if (name == "temperature") {
        try {
            options.temperature = std::stod(value);
        } catch (const std::exception&) {
            return invalid_setting_value(name, "expected a floating-point number");
        }
        if (!std::isfinite(options.temperature)) {
            return invalid_setting_value(name, "expected a finite number");
        }
        options.has_temperature = true;
        return ok_error();
    }
    if (name == "top_k") {
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
    if (name == "top_p" || name == "min_p" || name == "repeat_penalty" || name == "presence_penalty") {
        double parsed = 0.0;
        try {
            parsed = std::stod(value);
        } catch (const std::exception&) {
            return invalid_setting_value(name, "expected a floating-point number");
        }
        if (!std::isfinite(parsed)) {
            return invalid_setting_value(name, "expected a finite number");
        }
        if (name == "top_p") {
            options.top_p = parsed;
            options.has_top_p = true;
        } else if (name == "min_p") {
            options.min_p = parsed;
            options.has_min_p = true;
        } else if (name == "repeat_penalty") {
            options.repeat_penalty = parsed;
            options.has_repeat_penalty = true;
        } else {
            options.presence_penalty = parsed;
            options.has_presence_penalty = true;
        }
        return ok_error();
    }
    return invalid_setting_value(name,
                                 "unknown setting; expected temperature, top_k, top_p, min_p, "
                                 "repeat_penalty, presence_penalty, max_tokens, thinking, or thinking_budget");
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
    append_optional_bool(out, first, "enable_thinking", options.has_enable_thinking, options.enable_thinking);
    if (options.has_thinking_budget) {
        if (thinking_budget_is_token_count(options.thinking_budget)) {
            append_json_int(out, first, "thinking_budget", std::stoll(options.thinking_budget));
        } else {
            append_json_string(out, first, "thinking_budget", options.thinking_budget);
        }
    } else {
        append_json_null(out, first, "thinking_budget");
    }
    append_optional_bool(out, first, "show_thinking_traces", options.has_show_thinking_traces,
                         options.show_thinking_traces);
    append_optional_string(out, first, "purpose", options.has_chat_purpose, options.chat_purpose);
    append_optional_int(out, first, "context_tokens", options.has_context_tokens, options.context_tokens);
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
    if (const json::Value* enable_thinking = root.get("enable_thinking")) {
        if (enable_thinking->is_null()) {
            // cleared override
        } else if (enable_thinking->type != json::Value::Type::Bool) {
            return {ErrorCode::ProviderSchema, "chat settings enable_thinking must be a boolean or null"};
        } else {
            options.enable_thinking = enable_thinking->boolean;
            options.has_enable_thinking = true;
        }
    }
    if (const json::Value* thinking_budget = root.get("thinking_budget")) {
        if (thinking_budget->is_null()) {
            // cleared override
        } else if (thinking_budget->type == json::Value::Type::Number) {
            if (thinking_budget->number < 0.0) {
                return {ErrorCode::ProviderSchema,
                        "chat settings thinking_budget must be a non-negative token count"};
            }
            options.thinking_budget = std::to_string(static_cast<long long>(thinking_budget->number));
            options.has_thinking_budget = true;
        } else if (thinking_budget->type == json::Value::Type::String) {
            Error err = validate_thinking_budget_value(thinking_budget->string);
            if (!err.ok()) {
                return {ErrorCode::ProviderSchema, err.message};
            }
            options.thinking_budget = thinking_budget->string;
            options.has_thinking_budget = true;
        } else {
            return {ErrorCode::ProviderSchema,
                    "chat settings thinking_budget must be a token count, verbal label string, or null"};
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
    out << " thinking="
        << (options.has_enable_thinking ? (options.enable_thinking ? "on" : "off") : "default");
    out << " thinking_budget=" << (options.has_thinking_budget ? options.thinking_budget : "default");
    return out.str();
}

std::string format_settings_panel(const cli::Options& options) {
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
    append("thinking", options.has_enable_thinking ? (options.enable_thinking ? "on" : "off") : "");
    append("thinking_budget", options.has_thinking_budget ? options.thinking_budget : "");
    append("show_thinking_traces",
           options.has_show_thinking_traces ? (options.show_thinking_traces ? "trace" : "notrace") : "");
    append("purpose", options.has_chat_purpose ? options.chat_purpose : "");
    append("context_tokens", options.has_context_tokens ? std::to_string(options.context_tokens) : "");
    std::string text = out.str();
    if (!text.empty() && text.back() == '\n') {
        text.pop_back();
    }
    return text;
}

}  // namespace pkchat::chat