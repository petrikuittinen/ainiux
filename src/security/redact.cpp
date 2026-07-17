#include "security/redact.hpp"

#include "common.hpp"

namespace ainiux {

std::string redact_secrets(std::string text, const std::vector<std::string>& secrets) {
    for (const std::string& secret : secrets) {
        if (secret.empty()) {
            continue;
        }
        std::string::size_type pos = 0;
        while ((pos = text.find(secret, pos)) != std::string::npos) {
            text.replace(pos, secret.size(), "[REDACTED]");
            pos += 10;
        }
    }
    return text;
}

bool is_sensitive_header_name(const std::string& name) {
    const std::string lower = ascii_lower(name);
    return lower == "authorization" || lower == "api-key" || lower == "x-api-key" ||
           lower == "cookie" || lower == "set-cookie";
}

}  // namespace ainiux
