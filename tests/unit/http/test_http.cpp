#include "http/test_http.hpp"
#include "support/test_support.hpp"
#include "http/http.hpp"
#include "runtime/runtime.hpp"
#include <string>

namespace ainiux::test::http {

namespace {

using ainiux::test::check;

void test_http_private_address_socket_block() {
    ainiux::http::Request request;
    request.url = "http://127.0.0.1:1/";
    request.connect_timeout_seconds = 1;
    request.block_private_addresses = true;
    ainiux::http::Result result = ainiux::http::perform(request, {});
    check(!result.error.ok() && result.error.code == ainiux::ErrorCode::BadUrl,
          "HTTP transport blocks the resolved loopback socket address");
    check(result.error.message.find("127.0.0.1") != std::string::npos,
          "resolved-address refusal identifies the blocked address");
}

void test_http_request_validation_and_cancellation() {
    ainiux::http::Request request;
    request.url = "";
    request.connect_timeout_seconds = 1;
    ainiux::http::Result result = ainiux::http::perform(request, {});
    check(!result.error.ok(), "HTTP transport rejects an empty URL");

    ainiux::runtime::CancellationSource source;
    source.cancel();
    request.url = "http://127.0.0.1:9/";
    request.block_private_addresses = true;
    request.cancellation = source.token();
    result = ainiux::http::perform(request, {});
    check(!result.error.ok() &&
              (result.error.code == ainiux::ErrorCode::Cancelled ||
               result.error.code == ainiux::ErrorCode::BadUrl),
          "pre-cancelled HTTP request fails before or during transport");
}

}  // namespace

void run_all() {
    test_http_private_address_socket_block();
    test_http_request_validation_and_cancellation();
}

}  // namespace ainiux::test::http