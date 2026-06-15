#include "common.hpp"

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
        case ErrorCode::Config:
            return "PKCHAT_ERR_CONFIG";
        case ErrorCode::Cancelled:
            return "PKCHAT_ERR_CANCELLED";
        case ErrorCode::Internal:
            return "PKCHAT_ERR_INTERNAL";
    }
    return "PKCHAT_ERR_INTERNAL";
}

}  // namespace pkchat
