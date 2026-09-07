#include "chat/generation_settings.hpp"

namespace ainiux::chat::generation {

const std::vector<const char*>& chat_setting_names() {
    static const std::vector<const char*> kNames = {
        kTemperature,
        kTopK,
        kTopP,
        kMinP,
        kRepeatPenalty,
        kPresencePenalty,
        kMaxTokens,
        kReasoning,
    };
    return kNames;
}

std::string chat_setting_names_description() {
    return "temperature, top_k, top_p, min_p, repeat_penalty, presence_penalty, max_tokens, or reasoning";
}

bool is_chat_setting_name(const std::string& name) {
    for (const char* setting_name : chat_setting_names()) {
        if (name == setting_name) {
            return true;
        }
    }
    return name == kMaxOutputTokens;
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

bool is_chat_purpose(const std::string& purpose) {
    for (const char* value : chat_purposes()) {
        if (purpose == value) {
            return true;
        }
    }
    return false;
}

}  // namespace ainiux::chat::generation
