#pragma once

#include <functional>
#include <string>
#include <vector>

#include "common.hpp"
#include "runtime/runtime.hpp"

namespace pkchat::http {

using BodyCallback = std::function<Error(const std::string&)>;

struct Request {
    std::string method = "GET";
    std::string url;
    std::vector<std::string> headers;
    std::string body;
    long connect_timeout_seconds = 10;
    long timeout_seconds = 0;
    std::string proxy;
    bool insecure_tls = false;
    bool trace = false;
    bool block_private_addresses = false;
    runtime::CancellationToken cancellation;
    long max_body_bytes = 0;
    BodyCallback on_body;
};

struct Response {
    long status = 0;
    std::string body;
    std::string content_type;
    std::string stderr_text;
};

struct Result {
    Response response;
    Error error;
};

Result perform(const Request& request, const std::vector<std::string>& secrets);

}  // namespace pkchat::http
