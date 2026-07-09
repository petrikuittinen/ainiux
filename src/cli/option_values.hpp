#pragma once

#include <string>
#include <vector>

namespace pkchat::cli::option_values {

inline constexpr const char kImageCapabilityAuto[] = "auto";
inline constexpr const char kImageCapabilityAllow[] = "allow";
inline constexpr const char kImageCapabilityDeny[] = "deny";

const std::vector<const char*>& image_capabilities();
std::vector<std::string> image_capability_strings();
std::string image_capability_description();
bool is_image_capability(const std::string& value);

}  // namespace pkchat::cli::option_values