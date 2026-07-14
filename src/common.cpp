#include "common.hpp"

#include <cstdlib>
#include <utility>

namespace pkchat {

const char* error_code_name(ErrorCode code) {
    switch (code) {
        case ErrorCode::Ok:
            return "PKCHAT_OK";
        case ErrorCode::BadArgs:
            return "PKCHAT_ERR_BAD_ARGS";
        case ErrorCode::BadUrl:
            return "PKCHAT_ERR_BAD_URL";
        case ErrorCode::Dns:
            return "PKCHAT_ERR_DNS";
        case ErrorCode::Connect:
            return "PKCHAT_ERR_CONNECT";
        case ErrorCode::Tls:
            return "PKCHAT_ERR_TLS";
        case ErrorCode::Timeout:
            return "PKCHAT_ERR_TIMEOUT";
        case ErrorCode::HttpStatus:
            return "PKCHAT_ERR_HTTP_STATUS";
        case ErrorCode::Auth:
            return "PKCHAT_ERR_AUTH";
        case ErrorCode::RateLimit:
            return "PKCHAT_ERR_RATE_LIMIT";
        case ErrorCode::JsonParse:
            return "PKCHAT_ERR_JSON_PARSE";
        case ErrorCode::SseParse:
            return "PKCHAT_ERR_SSE_PARSE";
        case ErrorCode::ProviderSchema:
            return "PKCHAT_ERR_PROVIDER_SCHEMA";
        case ErrorCode::UnsupportedFeature:
            return "PKCHAT_ERR_UNSUPPORTED_FEATURE";
        case ErrorCode::FileRead:
            return "PKCHAT_ERR_FILE_READ";
        case ErrorCode::FileWrite:
            return "PKCHAT_ERR_FILE_WRITE";
        case ErrorCode::FileLock:
            return "PKCHAT_ERR_FILE_LOCK";
        case ErrorCode::Config:
            return "PKCHAT_ERR_CONFIG";
        case ErrorCode::Cancelled:
            return "PKCHAT_ERR_CANCELLED";
        case ErrorCode::StreamComplete:
            return "PKCHAT_ERR_STREAM_COMPLETE";
        case ErrorCode::Internal:
            return "PKCHAT_ERR_INTERNAL";
    }
    return "PKCHAT_ERR_INTERNAL";
}

std::string ascii_trim(std::string text) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string ascii_lower(std::string text) {
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return text;
}

std::string expand_user_path(std::string path) {
    if (path.empty() || path == "-" || path == "stdin") {
        return path;
    }
    if (path == "~") {
        if (const char* home = std::getenv("HOME")) {
            if (*home != '\0') {
                return home;
            }
        }
        return path;
    }
    if (path.rfind("~/", 0) == 0) {
        if (const char* home = std::getenv("HOME")) {
            if (*home != '\0') {
                return std::string(home) + path.substr(1);
            }
        }
    }
    return path;
}

std::vector<std::string> split_lines_crlf(const std::string& input) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= input.size()) {
        size_t end = input.find('\n', start);
        if (end == std::string::npos) {
            end = input.size();
        }
        std::string line = input.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
        if (end == input.size()) {
            break;
        }
        start = end + 1;
    }
    return lines;
}

int positive_int_from_env(const char* name, int default_value) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || raw[0] == '\0') {
        return default_value;
    }
    char* end = nullptr;
    const long parsed = std::strtol(raw, &end, 10);
    if (end == raw || *end != '\0' || parsed <= 0) {
        return default_value;
    }
    return static_cast<int>(parsed);
}

size_t positive_size_from_env(const char* name, size_t default_value) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || raw[0] == '\0') {
        return default_value;
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(raw, &end, 10);
    if (end == raw || *end != '\0' || parsed == 0) {
        return default_value;
    }
    return static_cast<size_t>(parsed);
}

}  // namespace pkchat
