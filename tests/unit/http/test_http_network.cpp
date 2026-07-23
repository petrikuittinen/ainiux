#include "http/test_http.hpp"
#include "mock/slow_server.hpp"
#include "support/test_support.hpp"
#include "fetch/fetch.hpp"
#include "http/http.hpp"
#include <string>

namespace ainiux::test::http {

namespace {

using ainiux::test::check;

void test_http_slow_response_times_out() {
    ainiux::test::mock::SlowHttpServer server;
    check(server.start(3.0, 0.0, 1), "slow HTTP mock server starts");

    ainiux::http::Request request;
    request.url = server.base_url() + "/delay/3";
    request.connect_timeout_seconds = 2;
    request.timeout_seconds = 1;
    ainiux::http::Result result = ainiux::http::perform(request, {});
    check(!result.error.ok() && result.error.code == ainiux::ErrorCode::Timeout,
          "HTTP transport times out on a slow local mock response");
    check(result.error.message.find("timed out") != std::string::npos ||
              result.error.message.find("Timeout") != std::string::npos,
          "HTTP timeout error mentions timeout");
}

void test_http_slow_body_times_out() {
    ainiux::test::mock::SlowHttpServer server;
    check(server.start(0.0, 0.75, 8), "slow-body HTTP mock server starts");

    ainiux::http::Request request;
    request.url = server.base_url() + "/slow-body";
    request.connect_timeout_seconds = 2;
    request.timeout_seconds = 2;
    ainiux::http::Result result = ainiux::http::perform(request, {});
    check(!result.error.ok() && result.error.code == ainiux::ErrorCode::Timeout,
          "HTTP transport times out while reading a slow chunked body");
}

void test_fetch_slow_html_times_out() {
    ainiux::test::mock::SlowHttpServer server;
    check(server.start(3.0, 0.0, 1), "slow HTTP mock server starts for fetch timeout");

    ainiux::fetch::Options options;
    options.connect_timeout_seconds = 2;
    options.timeout_seconds = 1;
    options.allow_private = true;
    std::string body;
    ainiux::Error err = ainiux::fetch::fetch_html(server.base_url() + "/delay/3", options, body);
    check(!err.ok() && err.code == ainiux::ErrorCode::Timeout,
          "URL fetch times out against a slow local mock response");
}

void test_slow_server_stop_releases_port() {
    ainiux::test::mock::SlowHttpServer server;
    check(server.start(), "slow HTTP mock server starts for teardown check");
    const std::string url = server.base_url() + "/";
    server.stop();

    ainiux::http::Request request;
    request.url = url;
    request.connect_timeout_seconds = 1;
    request.timeout_seconds = 1;
    const ainiux::http::Result result = ainiux::http::perform(request, {});
    check(!result.error.ok() && result.error.code == ainiux::ErrorCode::Connect,
          "stopping the slow HTTP mock releases its listening port");
}

void test_http_connect_timeout_to_blackhole() {
    ainiux::http::Request request;
    request.url = "http://198.18.0.12:9/";
    request.connect_timeout_seconds = 1;
    request.timeout_seconds = 2;
    ainiux::http::Result result = ainiux::http::perform(request, {});
    check(!result.error.ok() &&
              (result.error.code == ainiux::ErrorCode::Timeout ||
               result.error.code == ainiux::ErrorCode::Connect),
          "HTTP transport fails when connect to an unreachable TEST-NET address times out or is refused");
}

}  // namespace

void run_network_faults() {
    test_http_slow_response_times_out();
    test_http_slow_body_times_out();
    test_fetch_slow_html_times_out();
    test_slow_server_stop_releases_port();
    test_http_connect_timeout_to_blackhole();
}

}  // namespace ainiux::test::http
