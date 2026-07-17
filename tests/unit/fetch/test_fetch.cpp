#include "fetch/test_fetch.hpp"
#include "support/test_support.hpp"
#include "fetch/fetch.hpp"
#include <string>

namespace ainiux::test::fetch {

namespace {

using ainiux::test::check;
using ainiux::test::read_fixture;

void test_safe_fetch_rejects_private_literal() {
    ainiux::fetch::Options options;
    std::string body;
    ainiux::Error err = ainiux::fetch::fetch_html("http://127.0.0.1/private", options, body);
    check(!err.ok() && err.code == ainiux::ErrorCode::BadUrl,
          "shared URL fetch rejects a private literal before transport");
    err = ainiux::fetch::fetch_html("file:///tmp/page.html", options, body);
    check(!err.ok() && err.code == ainiux::ErrorCode::BadUrl,
          "shared URL fetch rejects non-HTTP schemes");
}

void test_fetch_validation_edge_cases() {
    ainiux::fetch::Options options;
    std::string body;
    ainiux::Error err = ainiux::fetch::fetch_html("", options, body);
    check(!err.ok() && err.code == ainiux::ErrorCode::BadArgs &&
              err.message.find("non-empty URL") != std::string::npos,
          "URL fetch rejects an empty URL");
    options.max_bytes = 0;
    err = ainiux::fetch::fetch_html("https://example.com/", options, body);
    check(!err.ok() && err.code == ainiux::ErrorCode::BadArgs &&
              err.message.find("greater than zero") != std::string::npos,
          "URL fetch rejects zero max-bytes limit");
    options.max_bytes = 1024;
    err = ainiux::fetch::fetch_html("http://localhost/page", options, body);
    check(!err.ok() && err.code == ainiux::ErrorCode::BadUrl,
          "URL fetch rejects localhost before transport");
    err = ainiux::fetch::fetch_html("http://10.0.0.1/internal", options, body);
    check(!err.ok() && err.code == ainiux::ErrorCode::BadUrl,
          "URL fetch rejects RFC1918 addresses before transport");
    err = ainiux::fetch::fetch_html("http://[::1]/", options, body);
    check(!err.ok() && err.code == ainiux::ErrorCode::BadUrl,
          "URL fetch rejects IPv6 loopback before transport");
    err = ainiux::fetch::fetch_html("not-a-url", options, body);
    check(!err.ok() && err.code == ainiux::ErrorCode::BadUrl,
          "URL fetch rejects malformed URLs before transport");
    options.proxy = "http://proxy.local:8080";
    err = ainiux::fetch::fetch_html("https://example.com/", options, body);
    check(!err.ok() && err.code == ainiux::ErrorCode::BadUrl &&
              err.message.find("proxy") != std::string::npos,
          "URL fetch rejects proxy use without private-fetch override");
}

}  // namespace

void run_all() {
    test_safe_fetch_rejects_private_literal();
    test_fetch_validation_edge_cases();
}

}  // namespace ainiux::test::fetch
