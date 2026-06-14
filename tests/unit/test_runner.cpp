#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "cli/args.hpp"
#include "json/json.hpp"
#include "provider/provider.hpp"

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void test_cli_parse() {
    const char* argv[] = {"pkchat", "http://localhost:8000", "-p", "hello", "--no-stream", "--format", "json"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(7, const_cast<char**>(argv));
    check(parsed.error.ok(), "CLI parse should succeed");
    check(parsed.options.positional_url == "http://localhost:8000", "positional URL parsed");
    check(parsed.options.prompt == "hello", "prompt parsed");
    check(!parsed.options.stream, "no-stream parsed");
    check(parsed.options.format == pkchat::cli::OutputFormat::Json, "json format parsed");
}

void test_cli_rejects_unknown() {
    const char* argv[] = {"pkchat", "--bogus"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(2, const_cast<char**>(argv));
    check(!parsed.error.ok(), "unknown option rejected");
    check(parsed.error.code == pkchat::ErrorCode::BadArgs, "unknown option is bad args");
}

void test_url_normalization() {
    bool changed = false;
    pkchat::Error err;
    std::string url = pkchat::provider::normalize_base_url("http://localhost:8000", &changed, err);
    check(err.ok(), "base URL without path is valid");
    check(changed, "base URL without path is changed");
    check(url == "http://localhost:8000/v1", "base URL appends /v1");
    url = pkchat::provider::normalize_base_url("http://localhost:8000/v1", &changed, err);
    check(err.ok(), "base URL with /v1 is valid");
    check(!changed, "base URL with /v1 unchanged");
    check(url == "http://localhost:8000/v1", "base URL with /v1 preserved");
    url = pkchat::provider::normalize_base_url("ftp://localhost", &changed, err);
    check(!err.ok() && err.code == pkchat::ErrorCode::BadUrl, "bad URL rejected");
}

void test_json_parse() {
    pkchat::json::ParseResult parsed = pkchat::json::parse("{\"data\":[{\"id\":\"m1\"}],\"text\":\"hi\\nthere\"}");
    check(parsed.error.ok(), "JSON parse succeeds");
    const pkchat::json::Value* data = parsed.value.get("data");
    check(data != nullptr && data->is_array(), "JSON data array");
    const pkchat::json::Value* id = data->at(0)->get("id");
    check(id != nullptr && id->string == "m1", "JSON nested string");
}

void test_lmstudio_context() {
    const char* argv[] = {"pkchat", "--provider", "lmstudio", "--list-models"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(4, const_cast<char**>(argv));
    check(parsed.error.ok(), "lmstudio args parse");
    pkchat::provider::ContextResult ctx = pkchat::provider::build_context(parsed.options);
    check(ctx.error.ok(), "lmstudio context builds without key");
    check(ctx.context.profile.name == "lm_studio", "lmstudio alias normalized");
    check(ctx.context.base_url == "http://localhost:1234/v1", "lmstudio default base URL");
}

}  // namespace

int main() {
    test_cli_parse();
    test_cli_rejects_unknown();
    test_url_normalization();
    test_json_parse();
    test_lmstudio_context();
    if (failures != 0) {
        std::cerr << failures << " unit test(s) failed\n";
        return 1;
    }
    std::cout << "unit tests passed\n";
    return 0;
}
