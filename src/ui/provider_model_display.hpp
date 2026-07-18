#pragma once

#include <cstddef>
#include <string>

namespace ainiux::ui {

inline constexpr std::size_t kModelDisplayCharacterLimit = 26;

std::string compact_model_name_for_display(const std::string& model_name);
std::string provider_model_display_label(const std::string& provider_name,
                                         const std::string& model_name);

}  // namespace ainiux::ui
