#pragma once

#include <string>
#include <vector>

namespace ainiux::chat::generation {

inline constexpr const char kTemperature[] = "temperature";
inline constexpr const char kTopK[] = "top_k";
inline constexpr const char kTopP[] = "top_p";
inline constexpr const char kMinP[] = "min_p";
inline constexpr const char kRepeatPenalty[] = "repeat_penalty";
inline constexpr const char kPresencePenalty[] = "presence_penalty";
inline constexpr const char kMaxTokens[] = "max_tokens";
inline constexpr const char kMaxOutputTokens[] = "max_output_tokens";
inline constexpr const char kThinking[] = "thinking";
inline constexpr const char kEnableThinking[] = "enable_thinking";
inline constexpr const char kThinkingBudget[] = "thinking_budget";

inline constexpr const char kPurposeGeneral[] = "general";
inline constexpr const char kPurposeCoding[] = "coding";
inline constexpr const char kPurposeInstruct[] = "instruct";
inline constexpr const char kPurposeCreative[] = "creative";

const std::vector<const char*>& chat_setting_names();
std::string chat_setting_names_description();
bool is_chat_setting_name(const std::string& name);

const std::vector<const char*>& model_setting_keys();
std::string model_setting_keys_description();

const std::vector<const char*>& chat_purposes();
std::vector<std::string> chat_purpose_strings();
std::string chat_purpose_description();
bool is_chat_purpose(const std::string& purpose);

}  // namespace ainiux::chat::generation