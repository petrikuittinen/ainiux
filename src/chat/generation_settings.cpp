#include "chat/generation_settings.hpp"

namespace pkchat::chat::generation {

const std::vector<const char*>& chat_setting_names() {
    static const std::vector<const char*> kNames = {
        kTemperature,
        kTopK,
        kTopP,
        kMinP,
        kRepeatPenalty,
        kPresencePenalty,
        kMaxTokens,
        kThinking,
        kThinkingBudget,
    };
    return kNames;
}

std::string chat_setting_names_description() {
    return "temperature, top_k, top_p, min_p, repeat_penalty, presence_penalty, max_tokens, thinking, "
           "or thinking_budget";
}

bool is_chat_setting_name(const std::string& name) {
    for (const char* setting_name : chat_setting_names()) {
        if (name == setting_name) {
            return true;
        }
    }
    return name == kMaxOutputTokens;
}

const std::vector<const char*>& model_setting_keys() {
    static const std::vector<const char*> kKeys = {
        "model",
        "purpose",
        "default_system_prompt",
        kTemperature,
        kTopK,
        kTopP,
        kMinP,
        kRepeatPenalty,
        kPresencePenalty,
        kThinkingBudget,
    };
    return kKeys;
}

std::string model_setting_keys_description() {
    return "model, purpose, default_system_prompt, temperature, top_k, top_p, min_p, repeat_penalty, "
           "presence_penalty, or thinking_budget";
}

const std::vector<const char*>& chat_purposes() {
    static const std::vector<const char*> kPurposes = {
        kPurposeGeneral,
        kPurposeCoding,
        kPurposeInstruct,
        kPurposeCreative,
    };
    return kPurposes;
}

std::string chat_purpose_description() {
    return "general, coding, instruct, or creative";
}

std::vector<std::string> chat_purpose_strings() {
    std::vector<std::string> out;
    out.reserve(chat_purposes().size());
    for (const char* purpose : chat_purposes()) {
        out.emplace_back(purpose);
    }
    return out;
}

bool is_chat_purpose(const std::string& purpose) {
    for (const char* value : chat_purposes()) {
        if (purpose == value) {
            return true;
        }
    }
    return false;
}

}  // namespace pkchat::chat::generation