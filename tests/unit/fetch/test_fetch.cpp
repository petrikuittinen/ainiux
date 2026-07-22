#include "fetch/test_fetch.hpp"
#include "support/test_support.hpp"
#include "fetch/fetch.hpp"
#include "html/html.hpp"
#include "json/json.hpp"
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

void test_iso8859_1_html_to_utf8() {
    // Mirrors matti.naskali.net-style pages: meta charset=iso-8859-1 with 0xE4 = ä.
    const std::string latin1_html =
        "<html>\r\n<head>\r\n"
        "   <meta http-equiv=\"Content-Type\" content=\"text/html; charset=iso-8859-1\">\r\n"
        "   <title>Eino Leino - Lapin Kes\xe4</title>\r\n"
        "</head><body>Lapissa kaikki kukkii: kes\xe4</body></html>";
    check(!ainiux::html::is_valid_utf8(latin1_html),
          "fixture is intentionally not UTF-8");

    const std::string utf8 =
        ainiux::fetch::convert_fetched_body_to_utf8(latin1_html, "text/html");
    check(ainiux::html::is_valid_utf8(utf8), "converted body is valid UTF-8");
    check(utf8.find("Lapin Kesä") != std::string::npos,
          "ISO-8859-1 ä becomes UTF-8 in title");
    check(utf8.find("kesä") != std::string::npos, "ISO-8859-1 ä becomes UTF-8 in body");

    // JSON tool results must remain well-formed UTF-8 for local model servers.
    ainiux::json::Value root;
    root.type = ainiux::json::Value::Type::Object;
    ainiux::json::Value body_value;
    body_value.type = ainiux::json::Value::Type::String;
    body_value.string = utf8;
    root.object["body"] = std::move(body_value);
    const std::string json = ainiux::json::stringify(root);
    check(ainiux::html::is_valid_utf8(json), "JSON tool payload is valid UTF-8");
    check(json.find("Kesä") != std::string::npos, "JSON contains the poem title as UTF-8");
}

void test_json_escape_rejects_raw_latin1() {
    // Safety net: even if a caller embeds ISO-8859-1, escape_string must not emit
    // raw 0xE4 into the JSON string.
    const std::string latin1 = "Kes\xe4";
    const std::string escaped = ainiux::json::escape_string(latin1);
    check(escaped.find('\xe4') == std::string::npos,
          "escape_string does not emit raw latin1 high bytes");
    check(escaped.find("\\u00e4") != std::string::npos,
          "escape_string encodes latin1 high bytes as \\u00XX");
}

}  // namespace

void run_all() {
    test_safe_fetch_rejects_private_literal();
    test_fetch_validation_edge_cases();
    test_iso8859_1_html_to_utf8();
    test_json_escape_rejects_raw_latin1();
}

}  // namespace ainiux::test::fetch
