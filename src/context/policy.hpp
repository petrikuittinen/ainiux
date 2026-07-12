#pragma once

#include <string>
#include <vector>

namespace pkchat::context::policy {

inline constexpr const char kError[] = "error";
inline constexpr const char kTruncateOldest[] = "truncate-oldest";
inline constexpr const char kTruncateMiddle[] = "truncate-middle";
inline constexpr const char kSummarizeOldest[] = "summarize-oldest";
inline constexpr const char kSummarizeMiddle[] = "summarize-middle";
inline constexpr const char kProviderAuto[] = "provider-auto";

const std::vector<const char*>& values();
std::vector<std::string> value_strings();
bool is_valid(const std::string& policy);
std::string usage_description();

}  // namespace pkchat::context::policy