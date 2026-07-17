#include "context/test_context.hpp"
#include "support/test_support.hpp"
#include "context/context.hpp"
#include "context/policy.hpp"
#include "provider/provider.hpp"
#include <string>
#include <vector>

namespace ainiux::test::context {

namespace {

using ainiux::test::check;
using ainiux::test::read_fixture;

void test_context_policies_preserve_full_messages() {
    std::vector<ainiux::provider::Message> messages = {
        {"system", "system"},
        {"user", std::string(400, 'a')},
        {"assistant", std::string(400, 'b')},
        {"user", std::string(400, 'c')},
        {"assistant", std::string(400, 'd')},
    };
    const std::vector<ainiux::provider::Message> original = messages;
    ainiux::context::PreparedMessages error = ainiux::context::prepare(messages, "error", 500);
    check(!error.error.ok(), "error context policy rejects an oversized request");

    ainiux::context::PreparedMessages truncated = ainiux::context::prepare(messages, "truncate-oldest", 500);
    check(truncated.error.ok() && truncated.compacted, "truncate-oldest compacts provider messages");
    check(truncated.event.messages_compacted > 0 && ainiux::context::estimated_text_bytes(truncated.messages) <= 500,
          "truncate-oldest respects the configured text budget");
    check(messages.size() == original.size() && messages[1].content == original[1].content,
          "context preparation leaves the full source transcript unchanged");

    ainiux::context::PreparedMessages summarized = ainiux::context::prepare(messages, "summarize-oldest", 600);
    check(summarized.error.ok() && summarized.compacted, "summarize-oldest compacts provider messages");
    check(ainiux::context::estimated_text_bytes(summarized.messages) <= 600,
          "summarize-oldest respects the configured text budget");
    bool summary_seen = false;
    for (const ainiux::provider::Message& message : summarized.messages) {
        summary_seen = summary_seen || message.content.find("Context summary of") != std::string::npos;
    }
    check(summary_seen, "summarize-oldest inserts a visible request-only summary");

    ainiux::context::PreparedMessages middle = ainiux::context::prepare(messages, "summarize-middle", 1000);
    check(middle.error.ok() && middle.compacted, "summarize-middle compacts middle provider messages");
    check(middle.messages.back().content == messages.back().content,
          "summarize-middle preserves the newest message");

    ainiux::context::PreparedMessages truncate_middle =
        ainiux::context::prepare(messages, "truncate-middle", 1000);
    check(truncate_middle.error.ok() && truncate_middle.compacted,
          "truncate-middle compacts middle provider messages");
    check(truncate_middle.messages.back().content == messages.back().content,
          "truncate-middle preserves the newest message");
    check(ainiux::context::estimated_text_bytes(truncate_middle.messages) <= 1000,
          "truncate-middle respects the configured text budget");
    bool truncate_middle_summary_seen = false;
    for (const ainiux::provider::Message& message : truncate_middle.messages) {
        truncate_middle_summary_seen =
            truncate_middle_summary_seen ||
            message.content.find("Context summary of") != std::string::npos;
    }
    check(!truncate_middle_summary_seen, "truncate-middle does not insert a summary message");
    ainiux::context::PreparedMessages automatic = ainiux::context::prepare(messages, "provider-auto", 1);
    check(automatic.error.ok() && !automatic.compacted && automatic.messages.size() == messages.size(),
          "provider-auto delegates context management without changing messages");

    const std::vector<ainiux::provider::Message> visible_only = {
        {"user", "question"}, {"assistant", "answer"}};
    const std::vector<ainiux::provider::Message> with_thinking = {
        {"user", "question"}, {"assistant", "<think>hidden reasoning tokens</think>\n\nanswer"}};
    check(ainiux::context::estimated_text_tokens(with_thinking) >
              ainiux::context::estimated_text_tokens(visible_only),
          "context token estimate includes assistant thinking traces");

    const std::vector<ainiux::provider::Message> unicode = {
        {"user", "你好 مرحبا"}};
    check(ainiux::context::estimated_text_tokens(unicode) > 0,
          "context token estimate handles non-ASCII transcript text");
}

void test_context_numeric_and_unicode_edge_cases() {
    check(ainiux::context::estimated_text_bytes({}) == 0,
          "empty transcript has zero estimated text bytes");
    check(ainiux::context::estimated_text_tokens({}) == 3,
          "empty transcript uses the fixed request overhead token estimate");

    ainiux::context::PreparedMessages empty =
        ainiux::context::prepare({}, "truncate-oldest", 500);
    check(empty.error.ok() && !empty.compacted && empty.messages.empty(),
          "context preparation accepts an empty message list");

    ainiux::context::PreparedMessages zero_budget = ainiux::context::prepare(
        {{"user", "hello"}}, "truncate-oldest", 0);
    check(zero_budget.error.ok() && !zero_budget.compacted,
          "zero max-bytes budget skips compaction");

    ainiux::context::PreparedMessages unknown = ainiux::context::prepare(
        {{"user", std::string(1000, 'x')}}, "bogus-policy", 100);
    check(!unknown.error.ok(), "unknown context policy is rejected once compaction is needed");
    check(unknown.error.message.find("bogus-policy") != std::string::npos,
          "unknown context policy error names the requested policy");

    const std::vector<ainiux::provider::Message> ascii_only = {{"user", "hello"}};
    const std::vector<ainiux::provider::Message> unicode_dense = {
        {"user", u8"مرحبا 你好 👨‍👩‍👧‍👦"}};
    check(ainiux::context::estimated_text_tokens(unicode_dense) >=
              ainiux::context::estimated_text_tokens(ascii_only),
          "Unicode-dense transcript token estimate is not smaller than ASCII text");

    std::vector<ainiux::provider::Message> long_messages = {{"system", "stay"}};
    for (int i = 0; i < 20; ++i) {
        long_messages.push_back({"user", std::string(2000, 'z')});
        long_messages.push_back({"assistant", std::string(2000, 'y')});
    }
    long_messages.push_back({"user", "final"});
    ainiux::context::PreparedMessages long_text =
        ainiux::context::prepare(long_messages, "truncate-oldest", 1000);
    check(long_text.error.ok() && long_text.compacted &&
              ainiux::context::estimated_text_bytes(long_text.messages) <= 1000 &&
              long_text.messages.back().content == "final",
          "very long transcript is compacted to the configured byte budget");
}

}  // namespace

void test_context_usage_formatting() {
    ainiux::provider::ChatResult result;
    result.usage_json = "{\"prompt_tokens\":20,\"completion_tokens\":5,\"total_tokens\":25}";
    const std::vector<ainiux::provider::Message> messages = {
        {"user", "hi"}, {"assistant", "ok"}};
    check(ainiux::context::estimated_usage_tokens(messages, result) == 25,
          "context usage prefers provider-reported totals when available");

    result.usage_json = "null";
    result.total_tokens = -1;
    result.prompt_tokens = -1;
    result.completion_tokens = 5;
    check(ainiux::context::estimated_usage_tokens(messages, result) > 5,
          "context usage adds completion tokens when provider totals are missing");

    result.completion_tokens = 0;
    result.prompt_tokens = 18;
    check(ainiux::context::estimated_usage_tokens(messages, result) == 18,
          "context usage uses provider prompt tokens when totals are missing");

    result.usage_json = "null";
    check(ainiux::context::format_context_usage(1000, 10000) == "1000 (10%)",
          "context usage formats token count and percentage");
    check(ainiux::context::format_context_usage(20, 131072) == "20 (<1%)",
          "context usage shows sub-one-percent usage without rounding to zero");
    check(ainiux::context::format_context_usage(1000, 0).empty(),
          "context usage is omitted without a configured window");
}

void test_truncate_middle_preserves_beginning() {
    std::vector<ainiux::provider::Message> messages = {
        {"system", "sys"},
        {"user", "KEEP_FIRST"},
        {"assistant", std::string(300, 'a')},
        {"user", std::string(300, 'b')},
        {"assistant", std::string(300, 'c')},
        {"user", std::string(300, 'd')},
        {"assistant", std::string(300, 'e')},
        {"user", "KEEP_LAST"},
    };
    const size_t budget = 900;
    ainiux::context::PreparedMessages oldest =
        ainiux::context::prepare(messages, "truncate-oldest", budget);
    ainiux::context::PreparedMessages middle =
        ainiux::context::prepare(messages, "truncate-middle", budget);
    check(oldest.error.ok() && oldest.compacted, "truncate-oldest compacts the oversized transcript");
    check(middle.error.ok() && middle.compacted, "truncate-middle compacts the oversized transcript");

    bool oldest_keeps_first = false;
    bool middle_keeps_first = false;
    for (const ainiux::provider::Message& message : oldest.messages) {
        oldest_keeps_first = oldest_keeps_first || message.content == "KEEP_FIRST";
    }
    for (const ainiux::provider::Message& message : middle.messages) {
        middle_keeps_first = middle_keeps_first || message.content == "KEEP_FIRST";
    }
    check(!oldest_keeps_first, "truncate-oldest removes the earliest non-system message first");
    check(middle_keeps_first, "truncate-middle preserves the earliest non-system message");
    check(middle.messages.back().content == "KEEP_LAST",
          "truncate-middle preserves the newest message while trimming the middle");
}

void test_context_policy_metadata() {
    check(ainiux::context::policy::is_valid(ainiux::context::policy::kTruncateMiddle),
          "context policy metadata accepts truncate-middle");
    check(ainiux::context::policy::is_valid(ainiux::context::policy::kSummarizeMiddle),
          "context policy metadata accepts summarize-middle");
    check(!ainiux::context::policy::is_valid("bogus-policy"),
          "context policy metadata rejects unknown policies");
    check(ainiux::context::policy::value_strings().size() == ainiux::context::policy::values().size(),
          "context policy string list mirrors canonical values");
}

void run_all() {
    test_context_policies_preserve_full_messages();
    test_truncate_middle_preserves_beginning();
    test_context_numeric_and_unicode_edge_cases();
    test_context_usage_formatting();
    test_context_policy_metadata();
}

}  // namespace ainiux::test::context
