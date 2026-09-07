#include "security/redact.hpp"

#include <algorithm>

#include "common.hpp"
#include "provider/provider.hpp"

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

void append_request_secrets(const provider::RequestContext& context,
                            std::vector<std::string>& secrets) {
    if (!context.api_key.empty()) secrets.push_back(context.api_key);
    if (!context.options.key.empty()) secrets.push_back(context.options.key);
    for (const std::string& header : context.headers) {
        const std::size_t colon = header.find(':');
        if (colon == std::string::npos ||
            !is_sensitive_header_name(ascii_trim(header.substr(0, colon)))) {
            continue;
        }
        const std::string value = ascii_trim(header.substr(colon + 1));
        if (!value.empty()) secrets.push_back(value);
    }
    std::sort(secrets.begin(), secrets.end(),
              [](const std::string& left, const std::string& right) {
                  if (left.size() != right.size()) return left.size() > right.size();
                  return left < right;
              });
    secrets.erase(std::unique(secrets.begin(), secrets.end()), secrets.end());
}

std::vector<std::string> request_secrets(const provider::RequestContext& context) {
    std::vector<std::string> secrets;
    append_request_secrets(context, secrets);
    return secrets;
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
