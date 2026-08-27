#pragma once

#include <string>
#include <vector>

namespace ainiux {

std::string redact_secrets(std::string text, const std::vector<std::string>& secrets);
bool is_sensitive_header_name(const std::string& name);

// Unwrap copied or line-continued API keys. HTTP/2 (nghttp2) rejects CR/LF in
// header values and otherwise fails POSTs with CURLE_BAD_FUNCTION_ARGUMENT.
std::string sanitize_api_key(std::string key);

}  // namespace ainiux
