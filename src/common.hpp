#pragma once

#include <string>

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
    Internal
};

struct Error {
    ErrorCode code = ErrorCode::Ok;
    std::string message;

    bool ok() const { return code == ErrorCode::Ok; }
};

inline Error ok_error() { return {}; }

const char* error_code_name(ErrorCode code);

}  // namespace pkchat
