#pragma once

#include <string>
#include <vector>

namespace pkchat {

std::string redact_secrets(std::string text, const std::vector<std::string>& secrets);
bool is_sensitive_header_name(const std::string& name);

}  // namespace pkchat
