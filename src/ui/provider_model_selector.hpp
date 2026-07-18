#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace ainiux::ui {

std::vector<std::string> selectable_provider_ids();
std::string provider_selector_text(const std::vector<std::string>& provider_ids, size_t selected);
std::string model_selector_text(const std::vector<std::string>& models, size_t selected);
bool should_auto_select_only_model(const std::vector<std::string>& models);

}  // namespace ainiux::ui
