#include "json/test_json.hpp"
#include "support/test_support.hpp"
#include "json/json.hpp"
#include <string>

namespace pkchat::test::json {

namespace {

using pkchat::test::check;
using pkchat::test::read_fixture;

void test_json_parse() {
    pkchat::json::ParseResult parsed = pkchat::json::parse("{\"data\":[{\"id\":\"m1\"}],\"text\":\"hi\\nthere\"}");
    check(parsed.error.ok(), "JSON parse succeeds");
    const pkchat::json::Value* data = parsed.value.get("data");
    check(data != nullptr && data->is_array(), "JSON data array");
    const pkchat::json::Value* id = data->at(0)->get("id");
    check(id != nullptr && id->string == "m1", "JSON nested string");
}

void test_json_parse_empty_and_invalid() {
    pkchat::json::ParseResult parsed = pkchat::json::parse("");
    check(!parsed.error.ok() && parsed.error.code == pkchat::ErrorCode::JsonParse,
          "JSON parse rejects empty input");
    parsed = pkchat::json::parse("   ");
    check(!parsed.error.ok() && parsed.error.code == pkchat::ErrorCode::JsonParse,
          "JSON parse rejects whitespace-only input");
    parsed = pkchat::json::parse("{\"a\":}");
    check(!parsed.error.ok() && parsed.error.code == pkchat::ErrorCode::JsonParse,
          "JSON parse rejects malformed object syntax");
    parsed = pkchat::json::parse("null trailing");
    check(!parsed.error.ok() && parsed.error.message.find("trailing JSON data") != std::string::npos,
          "JSON parse rejects trailing data after a valid value");
}

void test_json_numeric_edge_cases() {
    pkchat::json::ParseResult zero = pkchat::json::parse("0");
    check(zero.error.ok() && zero.value.type == pkchat::json::Value::Type::Number && zero.value.number == 0.0,
          "JSON parse accepts zero");
    pkchat::json::ParseResult negative_zero = pkchat::json::parse("-0");
    check(negative_zero.error.ok() && negative_zero.value.number == 0.0,
          "JSON parse accepts negative zero");
    pkchat::json::ParseResult tiny = pkchat::json::parse("0.0000000012");
    check(tiny.error.ok() && tiny.value.number > 0.0 && tiny.value.number < 1e-8,
          "JSON parse accepts very small positive numbers");
    pkchat::json::ParseResult huge_positive = pkchat::json::parse("999999999999999999999");
    check(huge_positive.error.ok() && huge_positive.value.number > 0.0,
          "JSON parse accepts very large positive integers");
    pkchat::json::ParseResult huge_negative = pkchat::json::parse("-999999999999999999999");
    check(huge_negative.error.ok() && huge_negative.value.number < 0.0,
          "JSON parse accepts very large negative integers");
    pkchat::json::ParseResult scientific = pkchat::json::parse("1e308");
    check(scientific.error.ok() && scientific.value.number > 0.0,
          "JSON parse accepts large scientific-notation numbers");
}

void test_json_unicode_and_escaping() {
    const std::string unicode = u8"你好 مرحبا 👨‍👩‍👧‍👦";
    pkchat::json::ParseResult parsed = pkchat::json::parse("{\"text\":\"" + unicode + "\"}");
    check(parsed.error.ok() && parsed.value.get("text")->string == unicode,
          "JSON parse preserves Arabic, Chinese, and complex emoji text");
    parsed = pkchat::json::parse("{\"text\":\"\\u4F60\\u597D\"}");
    check(parsed.error.ok() && parsed.value.get("text")->string == u8"你好",
          "JSON parse decodes Unicode escape sequences");

    const std::string escaped = pkchat::json::escape_string("quote\"slash\\newline\n");
    check(escaped.find("\\\"") != std::string::npos && escaped.find("\\\\") != std::string::npos &&
              escaped.find("\\n") != std::string::npos,
          "JSON escape_string escapes quotes, slashes, and control characters");
    check(pkchat::json::quote(unicode) == "\"" + pkchat::json::escape_string(unicode) + "\"",
          "JSON quote wraps escaped Unicode text");

    const std::string long_text(100000, 'x');
    parsed = pkchat::json::parse("{\"long\":\"" + long_text + "\"}");
    check(parsed.error.ok() && parsed.value.get("long")->string == long_text,
          "JSON parse preserves very long string values");
}

}  // namespace

void run_all() {
    test_json_parse();
    test_json_parse_empty_and_invalid();
    test_json_numeric_edge_cases();
    test_json_unicode_and_escaping();
}

}  // namespace pkchat::test::json
