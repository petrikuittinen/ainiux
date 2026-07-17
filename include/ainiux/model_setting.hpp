#pragma once

#include <string>

namespace ainiux {

struct ModelSetting {
    std::string model;
    std::string purpose;
    std::string default_system_prompt;
    double temperature = 0.0;
    int top_k = 0;
    double top_p = 0.0;
    double min_p = 0.0;
    double repeat_penalty = 1.0;
    double presence_penalty = 0.0;
    std::string thinking_budget;
};

}  // namespace ainiux