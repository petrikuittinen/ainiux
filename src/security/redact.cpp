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
           lower == "x-goog-api-key" || lower == "cookie" || lower == "set-cookie";
}

std::string sanitize_api_key(std::string key) {
    std::string out;
    out.reserve(key.size());
    for (std::string::size_type i = 0; i < key.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(key[i]);
        if (c == '\\' && i + 1 < key.size()) {
            const unsigned char next = static_cast<unsigned char>(key[i + 1]);
            if (next == '\n' || next == '\r') {
                continue;
            }
        }
        if (c == '\n' || c == '\r' || c == '\t') {
            continue;
        }
        out.push_back(static_cast<char>(c));
    }
    return ascii_trim(std::move(out));
}

}  // namespace ainiux
