#include "json/test_json.hpp"
#include "support/test_support.hpp"
#include "json/json.hpp"
#include <string>

namespace ainiux::test::json {

namespace {

using ainiux::test::check;
using ainiux::test::read_fixture;

void test_json_parse() {
    ainiux::json::ParseResult parsed = ainiux::json::parse("{\"data\":[{\"id\":\"m1\"}],\"text\":\"hi\\nthere\"}");
    check(parsed.error.ok(), "JSON parse succeeds");
    const ainiux::json::Value* data = parsed.value.get("data");
    check(data != nullptr && data->is_array(), "JSON data array");
    const ainiux::json::Value* id = data->at(0)->get("id");
    check(id != nullptr && id->string == "m1", "JSON nested string");
}

void test_json_parse_empty_and_invalid() {
    ainiux::json::ParseResult parsed = ainiux::json::parse("");
    check(!parsed.error.ok() && parsed.error.code == ainiux::ErrorCode::JsonParse,
          "JSON parse rejects empty input");
    parsed = ainiux::json::parse("   ");
    check(!parsed.error.ok() && parsed.error.code == ainiux::ErrorCode::JsonParse,
          "JSON parse rejects whitespace-only input");
    parsed = ainiux::json::parse("{\"a\":}");
    check(!parsed.error.ok() && parsed.error.code == ainiux::ErrorCode::JsonParse,
          "JSON parse rejects malformed object syntax");
    parsed = ainiux::json::parse("null trailing");
    check(!parsed.error.ok() && parsed.error.message.find("trailing JSON data") != std::string::npos,
          "JSON parse rejects trailing data after a valid value");
}

void test_json_numeric_edge_cases() {
    ainiux::json::ParseResult zero = ainiux::json::parse("0");
    check(zero.error.ok() && zero.value.type == ainiux::json::Value::Type::Number && zero.value.number == 0.0,
          "JSON parse accepts zero");
    ainiux::json::ParseResult negative_zero = ainiux::json::parse("-0");
    check(negative_zero.error.ok() && negative_zero.value.number == 0.0,
          "JSON parse accepts negative zero");
    ainiux::json::ParseResult tiny = ainiux::json::parse("0.0000000012");
    check(tiny.error.ok() && tiny.value.number > 0.0 && tiny.value.number < 1e-8,
          "JSON parse accepts very small positive numbers");
    ainiux::json::ParseResult huge_positive = ainiux::json::parse("999999999999999999999");
    check(huge_positive.error.ok() && huge_positive.value.number > 0.0,
          "JSON parse accepts very large positive integers");
    ainiux::json::ParseResult huge_negative = ainiux::json::parse("-999999999999999999999");
    check(huge_negative.error.ok() && huge_negative.value.number < 0.0,
          "JSON parse accepts very large negative integers");
    ainiux::json::ParseResult scientific = ainiux::json::parse("1e308");
    check(scientific.error.ok() && scientific.value.number > 0.0,
          "JSON parse accepts large scientific-notation numbers");
}

void test_json_unicode_and_escaping() {
    const std::string unicode = u8"你好 مرحبا 👨‍👩‍👧‍👦";
    ainiux::json::ParseResult parsed = ainiux::json::parse("{\"text\":\"" + unicode + "\"}");
    check(parsed.error.ok() && parsed.value.get("text")->string == unicode,
          "JSON parse preserves Arabic, Chinese, and complex emoji text");
    parsed = ainiux::json::parse("{\"text\":\"\\u4F60\\u597D\"}");
    check(parsed.error.ok() && parsed.value.get("text")->string == u8"你好",
          "JSON parse decodes Unicode escape sequences");

    const std::string escaped = ainiux::json::escape_string("quote\"slash\\newline\n");
    check(escaped.find("\\\"") != std::string::npos && escaped.find("\\\\") != std::string::npos &&
              escaped.find("\\n") != std::string::npos,
          "JSON escape_string escapes quotes, slashes, and control characters");
    check(ainiux::json::quote(unicode) == "\"" + ainiux::json::escape_string(unicode) + "\"",
          "JSON quote wraps escaped Unicode text");

    const std::string long_text(100000, 'x');
    parsed = ainiux::json::parse("{\"long\":\"" + long_text + "\"}");
    check(parsed.error.ok() && parsed.value.get("long")->string == long_text,
          "JSON parse preserves very long string values");

    const std::string invalid_utf8 =
        std::string("{\"text\":\"") + static_cast<char>(0xFF) + "\"}";
    parsed = ainiux::json::parse(invalid_utf8);
    check(!parsed.error.ok() && parsed.error.code == ainiux::ErrorCode::JsonParse &&
              parsed.error.message.find("invalid UTF-8") != std::string::npos,
          "raw invalid UTF-8 in JSON strings is rejected");

    const std::string truncated_utf8 =
        std::string("{\"text\":\"") + static_cast<char>(0xE2);
    parsed = ainiux::json::parse(truncated_utf8);
    check(!parsed.error.ok() && parsed.error.message.find("incomplete UTF-8") != std::string::npos,
          "truncated UTF-8 in JSON strings is rejected");
}

}  // namespace

void run_all() {
    test_json_parse();
    test_json_parse_empty_and_invalid();
    test_json_numeric_edge_cases();
    test_json_unicode_and_escaping();
}

}  // namespace ainiux::test::json
