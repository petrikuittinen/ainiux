#include "security/redact.hpp"

#include <algorithm>
#include <cctype>

namespace pkchat {

namespace {

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

}  // namespace

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
    const std::string lower = lower_ascii(name);
    return lower == "authorization" || lower == "api-key" || lower == "x-api-key" ||
           lower == "cookie" || lower == "set-cookie";
}

}  // namespace pkchat
