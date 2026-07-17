#include "security/test_security.hpp"
#include "support/test_support.hpp"
#include "security/redact.hpp"
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
    check(ainiux::is_sensitive_header_name("Set-Cookie"), "Set-Cookie is sensitive");
    check(!ainiux::is_sensitive_header_name("Content-Type"), "Content-Type is not sensitive");
    check(!ainiux::is_sensitive_header_name(""), "empty header name is not sensitive");
}

}  // namespace

void run_all() {
    test_redact_secrets_edge_cases();
    test_sensitive_header_names();
}

}  // namespace ainiux::test::security