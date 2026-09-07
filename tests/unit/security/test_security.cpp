#include "security/test_security.hpp"
#include "provider/provider.hpp"
#include "support/test_support.hpp"
#include "security/redact.hpp"
#include <algorithm>
#include <string>
#include <vector>

namespace ainiux::test::security {

namespace {

using ainiux::test::check;

void test_redact_secrets_edge_cases() {
    check(ainiux::redact_secrets("Bearer secret-token", {"secret-token"}) ==
              "Bearer [REDACTED]",
          "redact_secrets replaces configured secrets");
    check(ainiux::redact_secrets("unchanged text", {}) == "unchanged text",
          "redact_secrets leaves text unchanged when secret list is empty");
    check(ainiux::redact_secrets("unchanged text", {""}) == "unchanged text",
          "redact_secrets ignores empty secret entries");
    check(ainiux::redact_secrets("", {"anything"}) == "",
          "redact_secrets handles empty input text");
    check(ainiux::redact_secrets("aa aa", {"a"}) == "[REDACTED][REDACTED] [REDACTED][REDACTED]",
          "redact_secrets replaces repeated overlapping secret matches");
    check(ainiux::redact_secrets("你好 secret-token مرحبا", {"secret-token"}) ==
              "你好 [REDACTED] مرحبا",
          "redact_secrets preserves surrounding Unicode text");
}

void test_sensitive_header_names() {
    check(ainiux::is_sensitive_header_name("Authorization"), "Authorization is sensitive");
    check(ainiux::is_sensitive_header_name("x-api-key"), "x-api-key is sensitive");
    check(ainiux::is_sensitive_header_name("x-goog-api-key"), "x-goog-api-key is sensitive");
    check(ainiux::is_sensitive_header_name("Set-Cookie"), "Set-Cookie is sensitive");
    check(!ainiux::is_sensitive_header_name("Content-Type"), "Content-Type is not sensitive");
    check(!ainiux::is_sensitive_header_name(""), "empty header name is not sensitive");
}

void test_request_secrets_are_complete_and_overlap_safe() {
    provider::RequestContext context;
    context.api_key = "shared-secret";
    context.options.key = "secret";
    context.headers = {
        "Authorization: Bearer shared-secret",
        "X-API-Key: shared-secret",
        "Content-Type: secret/plain",
        "Cookie: session=cookie-secret",
    };
    const std::vector<std::string> secrets = request_secrets(context);
    check(secrets.size() == 4,
          "request secret collection deduplicates keys and ignores ordinary headers");
    check(std::is_sorted(secrets.begin(), secrets.end(),
                         [](const std::string& left, const std::string& right) {
                             return left.size() > right.size();
                         }) &&
              std::find(secrets.begin(), secrets.end(), "Bearer shared-secret") != secrets.end() &&
              secrets.back() == "secret",
          "request secrets are ordered longest-first");
    check(redact_secrets("Bearer shared-secret / secret", secrets) ==
              "[REDACTED] / [REDACTED]",
          "overlapping request credentials redact without exposing a suffix");
}

void test_sanitize_api_key_unwraps_line_continuations() {
    check(ainiux::sanitize_api_key("sk-abc") == "sk-abc", "clean API keys are unchanged");
    check(ainiux::sanitize_api_key("  sk-abc\n") == "sk-abc", "API keys trim surrounding whitespace");
    check(ainiux::sanitize_api_key(std::string("sk-ab\\\n") + "c-key") == "sk-abc-key",
          "API keys unwrap POSIX backslash-newline line continuations");
    check(ainiux::sanitize_api_key("sk-ab\nc-key") == "sk-abc-key",
          "API keys drop an interior line break from a wrapped paste");
    check(ainiux::sanitize_api_key("sk-ab\tc-key") == "sk-abc-key",
          "API keys drop interior tabs");
    check(ainiux::sanitize_api_key("") == "", "empty API keys stay empty");
}

}  // namespace

void run_all() {
    test_redact_secrets_edge_cases();
    test_sensitive_header_names();
    test_request_secrets_are_complete_and_overlap_safe();
    test_sanitize_api_key_unwraps_line_continuations();
}

}  // namespace ainiux::test::security
