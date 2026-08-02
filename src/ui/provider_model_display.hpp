#pragma once

#include <cstddef>
#include <string>

namespace ainiux::ui {

inline constexpr std::size_t kModelDisplayCharacterLimit = 26;

std::string compact_model_name_for_display(const std::string& model_name);
std::string provider_model_display_label(const std::string& provider_name,
                                         const std::string& model_name,
                                         const std::string& qualifier = {});

// Model-only chrome (no provider): "[short-model]" or "[short-model reasoning]".
// Empty model returns "" (callers choose help/placeholder text).
std::string model_reasoning_bracket(const std::string& model_name,
                                    const std::string& reasoning = {});
// e.g. "[gemma-4-26b-a4b] auto-selected" — short model only, no provider.
std::string model_status_message(const std::string& model_name, const std::string& suffix);

}  // namespace ainiux::ui
