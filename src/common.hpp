#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace pkchat {

enum class ErrorCode {
    Ok = 0,
    BadArgs,
    BadUrl,
    Dns,
    Connect,
    Tls,
    Timeout,
    HttpStatus,
    Auth,
    RateLimit,
    JsonParse,
    SseParse,
    ProviderSchema,
    UnsupportedFeature,
    FileRead,
    FileWrite,
    Config,
    Cancelled,
    StreamComplete,
    Internal
};

struct Error {
    ErrorCode code = ErrorCode::Ok;
    std::string message;

    bool ok() const { return code == ErrorCode::Ok; }
};

inline Error ok_error() { return {}; }

const char* error_code_name(ErrorCode code);

std::string ascii_trim(std::string text);
std::string ascii_lower(std::string text);
std::string expand_user_path(std::string path);
std::vector<std::string> split_lines_crlf(const std::string& input);
int positive_int_from_env(const char* name, int default_value);
size_t positive_size_from_env(const char* name, size_t default_value);

}  // namespace pkchat
