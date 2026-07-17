#include "output/test_output.hpp"
#include "support/test_support.hpp"
#include "output/thinking.hpp"
#include <string>

namespace ainiux::test::output {

namespace {

using ainiux::test::check;
using ainiux::test::read_fixture;

void test_thinking_trace_splitter() {
    ainiux::output::ThinkingChunk split = ainiux::output::split_thinking_traces(
        "<think>internal trace</think>\n\nVisible answer");
    check(split.visible == "Visible answer", "thinking splitter keeps only visible response content");
    check(split.trace == "<think>internal trace</think>", "thinking splitter extracts trace with tags");

    ainiux::output::ThinkingTraceSplitter streaming;
    ainiux::output::ThinkingChunk first = streaming.feed("<thi");
    ainiux::output::ThinkingChunk second = streaming.feed("nk>split trace</TH");
    ainiux::output::ThinkingChunk third = streaming.feed("INK>\r\nanswer");
    ainiux::output::ThinkingChunk final = streaming.finish();
    check(first.visible.empty() && second.visible.empty(), "partial thinking tag never leaks as visible output");
    check(first.trace.empty(), "partial thinking tag waits for classification");
    check(second.trace == "<think>split trace", "streaming splitter extracts reasoning across chunks");
    check(third.trace == "</THINK>", "streaming splitter preserves closing trace tag");
    check(third.visible + final.visible == "answer", "streaming splitter removes trace separator newlines");

    split = ainiux::output::split_thinking_traces("Before <think>hidden</think> after");
    check(split.visible == "Before  after", "thinking splitter preserves visible text around trace");
    check(split.trace == "<think>hidden</think>", "thinking splitter extracts embedded trace");

    split = ainiux::output::split_thinking_traces("<think>unfinished");
    check(split.visible.empty(), "unfinished thinking trace does not leak into visible output");
    check(split.trace == "<think>unfinished", "unfinished thinking trace is sent to trace output");
}

void test_thinking_trace_empty_and_unicode() {
    ainiux::output::ThinkingChunk split = ainiux::output::split_thinking_traces("");
    check(split.visible.empty() && split.trace.empty(),
          "thinking splitter returns empty visible and trace output for empty input");

    split = ainiux::output::split_thinking_traces(
        u8"<think>مرحبا 你好 👨‍👩‍👧‍👦</think>\n\nanswer");
    check(split.visible == "answer", "thinking splitter keeps visible answer after Unicode trace");
    check(split.trace.find(u8"مرحبا") != std::string::npos &&
              split.trace.find(u8"👨‍👩‍👧‍👦") != std::string::npos,
          "thinking splitter preserves Arabic, Chinese, and emoji inside trace blocks");

    ainiux::output::ThinkingTraceSplitter streaming;
    check(streaming.feed("").visible.empty() && streaming.feed("").trace.empty(),
          "streaming thinking splitter accepts empty chunks");
    const std::string long_trace = "<think>" + std::string(100000, 'z') + "</think>\n\nok";
    split = ainiux::output::split_thinking_traces(long_trace);
    check(split.visible == "ok" && split.trace.size() > 100000,
          "thinking splitter handles very long trace content");
}

}  // namespace

void run_all() {
    test_thinking_trace_splitter();
    test_thinking_trace_empty_and_unicode();
}

}  // namespace ainiux::test::output
