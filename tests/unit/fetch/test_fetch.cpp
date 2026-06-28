#include "fetch/test_fetch.hpp"
#include "support/test_support.hpp"
#include "fetch/fetch.hpp"
#include <string>

namespace pkchat::test::fetch {

namespace {

using pkchat::test::check;
using pkchat::test::read_fixture;

void test_safe_fetch_rejects_private_literal() {
    pkchat::fetch::Options options;
    std::string body;
    pkchat::Error err = pkchat::fetch::fetch_html("http://127.0.0.1/private", options, body);
    check(!err.ok() && err.code == pkchat::ErrorCode::BadUrl,
          "shared URL fetch rejects a private literal before transport");
    err = pkchat::fetch::fetch_html("file:///tmp/page.html", options, body);
    check(!err.ok() && err.code == pkchat::ErrorCode::BadUrl,
          "shared URL fetch rejects non-HTTP schemes");
}

void test_fetch_validation_edge_cases() {
    pkchat::fetch::Options options;
    std::string body;
    pkchat::Error err = pkchat::fetch::fetch_html("", options, body);
    check(!err.ok() && err.code == pkchat::ErrorCode::BadArgs &&
              err.message.find("non-empty URL") != std::string::npos,
          "URL fetch rejects an empty URL");
    options.max_bytes = 0;
    err = pkchat::fetch::fetch_html("https://example.com/", options, body);
    check(!err.ok() && err.code == pkchat::ErrorCode::BadArgs &&
              err.message.find("greater than zero") != std::string::npos,
          "URL fetch rejects zero max-bytes limit");
    options.max_bytes = 1024;
    err = pkchat::fetch::fetch_html("http://localhost/page", options, body);
    check(!err.ok() && err.code == pkchat::ErrorCode::BadUrl,
          "URL fetch rejects localhost before transport");
    err = pkchat::fetch::fetch_html("http://10.0.0.1/internal", options, body);
    check(!err.ok() && err.code == pkchat::ErrorCode::BadUrl,
          "URL fetch rejects RFC1918 addresses before transport");
    err = pkchat::fetch::fetch_html("http://[::1]/", options, body);
    check(!err.ok() && err.code == pkchat::ErrorCode::BadUrl,
          "URL fetch rejects IPv6 loopback before transport");
    err = pkchat::fetch::fetch_html("not-a-url", options, body);
    check(!err.ok() && err.code == pkchat::ErrorCode::BadUrl,
          "URL fetch rejects malformed URLs before transport");
    options.proxy = "http://proxy.local:8080";
    err = pkchat::fetch::fetch_html("https://example.com/", options, body);
    check(!err.ok() && err.code == pkchat::ErrorCode::BadUrl &&
              err.message.find("proxy") != std::string::npos,
          "URL fetch rejects proxy use without private-fetch override");
}

}  // namespace

void run_all() {
    test_safe_fetch_rejects_private_literal();
    test_fetch_validation_edge_cases();
}

}  // namespace pkchat::test::fetch
