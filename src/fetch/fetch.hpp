#pragma once

#include <string>

#include "common.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::fetch {

struct Options {
    long connect_timeout_seconds = 10;
    long timeout_seconds = 30;
    long max_bytes = 1048576;
    std::string proxy;
    bool insecure_tls = false;
    bool trace_http = false;
    bool allow_private = false;
};

Error fetch_html(const std::string& url,
                 const Options& options,
                 std::string& html,
                 runtime::CancellationToken cancellation = runtime::CancellationToken());
Error fetch_markdown(const std::string& url,
                     const Options& options,
                     std::string& markdown,
                     runtime::CancellationToken cancellation = runtime::CancellationToken());
Error fetch_text(const std::string& url,
                 const Options& options,
                 std::string& text,
                 runtime::CancellationToken cancellation = runtime::CancellationToken());

}  // namespace ainiux::fetch
