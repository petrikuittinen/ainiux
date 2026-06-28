#include "tui/test_tui.hpp"
#include "support/test_support.hpp"
#include "output/thinking.hpp"
#include "pkchat/version.hpp"
#include "provider/provider.hpp"
#include "tui/tui.hpp"
#include <string>
#include <vector>

namespace pkchat::test::tui {

namespace {

using pkchat::test::check;
using pkchat::test::read_fixture;

void test_tui_history_jump_helpers() {
    check(pkchat::tui::history_scroll_for_thread_beginning() > 1000000,
          "TUI Home jump requests a clamped scrollback maximum");
    check(pkchat::tui::history_scroll_for_thread_end() == 0,
          "TUI End jump returns to the live chat bottom");
}

void test_tui_layout_reserves_editor_input_panel() {
    pkchat::tui::Layout small = pkchat::tui::layout_for_terminal(8, 20);
    check(small.rows == 8 && small.cols == 20, "TUI layout clamps to requested small terminal");
    check(small.header_rows == 0 && small.history_row == 1, "TUI layout has no persistent header rows");
    check(small.history_rows >= 1, "TUI layout leaves room for chat history");
    check(small.input_rect.height == 3, "TUI layout keeps minimum multiline input height");
    check(small.input_rect.row + small.input_rect.height - 1 <= small.rows,
          "TUI input panel stays inside terminal rows");

    pkchat::tui::Layout large = pkchat::tui::layout_for_terminal(40, 100);
    check(large.input_rect.height == 8, "TUI layout uses one fifth of a large terminal for input");
    check(large.input_rect.width == 100, "TUI input panel tracks terminal width");
    check(large.history_rows > large.input_rect.height, "TUI layout keeps the editor from taking the full screen");
}

void test_tui_ready_and_generation_status() {
    check(pkchat::tui::ready_status() == std::string("Pkchat v") + pkchat::kVersion + " ready",
          "TUI ready status displays the current pkchat version");

    pkchat::provider::ChatResult result;
    result.ttft_ms = 100;
    result.total_ms = 1100;
    result.completion_tokens = 20;
    result.completion_tokens_estimated = true;

    const std::string streaming =
        pkchat::tui::generation_ready_status("lm_studio", "gpt-test", result, true, {}, 0);
    check(streaming.find("[lm_studio / gpt-test]") == 0,
          "TUI streaming completion status starts with provider and model");
    check(streaming.find("TTFT: 100 ms") != std::string::npos,
          "TUI streaming completion status displays time to first token");
    check(streaming.find("Token/s: 20.0 (estimated)") != std::string::npos,
          "TUI streaming completion status estimates throughput after the first token");

    const std::string non_streaming =
        pkchat::tui::generation_ready_status("lm_studio", "gpt-test", result, false, {}, 0);
    check(non_streaming.find("Response: 1100 ms") != std::string::npos,
          "TUI non-streaming completion status reports response latency instead of TTFT");
    check(non_streaming.find("Token/s: 18.2 (estimated)") != std::string::npos,
          "TUI non-streaming completion status estimates whole-response throughput");

    result.usage_json = "{\"prompt_tokens\":20,\"completion_tokens\":5,\"total_tokens\":25}";
    const std::vector<pkchat::provider::Message> messages = {
        {"user", "hi"}, {"assistant", "<think>x</think>ok"}};
    const std::string context_status =
        pkchat::tui::generation_ready_status("lm_studio", "gpt-test", result, true, messages, 100);
    check(context_status.find("TTFT 100ms | ~20.0 tok/s") != std::string::npos,
          "TUI context status uses compact timing and estimated-throughput notation");
    check(context_status.find("Context used: 25/100 (25.0%)") != std::string::npos,
          "TUI completion status displays estimated context usage");

    result.usage_json = "null";
    const std::string exhausted =
        pkchat::tui::generation_ready_status("lm_studio", "gpt-test", result, true, messages, 10);
    check(exhausted.find("Context used: 17/10 (170.0%)") != std::string::npos,
          "TUI context estimate reports usage beyond the configured window");
}

void test_tui_regeneration_plan_uses_last_user_turn() {
    pkchat::chat::Session session;
    session.messages.push_back({"system", "stay concise"});
    session.messages.push_back({"user", "first"});
    session.messages.push_back({"assistant", "one"});
    session.messages.push_back({"user", "second"});
    session.messages.push_back({"assistant", "two"});

    pkchat::tui::RegenerationPlan plan = pkchat::tui::regeneration_plan_for_session(session);
    check(plan.available, "TUI regeneration plan is available when a user turn exists");
    check(plan.erase_from == 3, "TUI regeneration plan erases from the last user turn");
    check(plan.prompt == "second", "TUI regeneration plan reuses the last user prompt");

    pkchat::chat::Session no_user;
    no_user.messages.push_back({"system", "only system"});
    plan = pkchat::tui::regeneration_plan_for_session(no_user);
    check(!plan.available, "TUI regeneration plan is unavailable without a user turn");
}

void test_tui_theme_parsing_and_contrast() {
    pkchat::tui::ThemeName theme = pkchat::tui::ThemeName::Dark;
    check(pkchat::tui::parse_theme_name("dark", theme), "TUI dark theme parses");
    check(theme == pkchat::tui::ThemeName::Dark, "TUI dark theme selected");
    check(pkchat::tui::parse_theme_name("Light", theme), "TUI light theme parses case-insensitively");
    check(theme == pkchat::tui::ThemeName::Light, "TUI light theme selected");
    check(!pkchat::tui::parse_theme_name("sepia", theme), "TUI rejects unknown theme");

    const std::vector<pkchat::tui::ThemeName> themes = {
        pkchat::tui::ThemeName::Dark,
        pkchat::tui::ThemeName::Light,
    };
    const std::vector<pkchat::tui::StyleRole> roles = {
        pkchat::tui::StyleRole::Text,
        pkchat::tui::StyleRole::Muted,
        pkchat::tui::StyleRole::ThinkingTrace,
        pkchat::tui::StyleRole::UserLabel,
        pkchat::tui::StyleRole::AssistantLabel,
        pkchat::tui::StyleRole::Error,
        pkchat::tui::StyleRole::Status,
        pkchat::tui::StyleRole::InputLabel,
    };

    for (pkchat::tui::ThemeName item : themes) {
        for (pkchat::tui::StyleRole role : roles) {
            const pkchat::tui::StylePair pair = pkchat::tui::style_pair_for(item, role);
            check(pkchat::tui::contrast_ratio(pair.foreground, pair.background) >= 4.5,
                  std::string("TUI theme contrast meets WCAG AA for ") + pkchat::tui::theme_name(item));
        }
    }

    const pkchat::tui::StylePair dark_text =
        pkchat::tui::style_pair_for(pkchat::tui::ThemeName::Dark, pkchat::tui::StyleRole::Text);
    const pkchat::tui::StylePair dark_thinking =
        pkchat::tui::style_pair_for(pkchat::tui::ThemeName::Dark, pkchat::tui::StyleRole::ThinkingTrace);
    check(pkchat::tui::contrast_ratio(dark_thinking.foreground, dark_thinking.background) <
              pkchat::tui::contrast_ratio(dark_text.foreground, dark_text.background),
          "TUI dark thinking trace text is dimmer than normal text");

    const pkchat::tui::StylePair light_text =
        pkchat::tui::style_pair_for(pkchat::tui::ThemeName::Light, pkchat::tui::StyleRole::Text);
    const pkchat::tui::StylePair light_thinking =
        pkchat::tui::style_pair_for(pkchat::tui::ThemeName::Light, pkchat::tui::StyleRole::ThinkingTrace);
    check(pkchat::tui::contrast_ratio(light_thinking.foreground, light_thinking.background) <
              pkchat::tui::contrast_ratio(light_text.foreground, light_text.background),
          "TUI light thinking trace text is less stark than normal text");
}

void test_tui_thinking_trace_display() {
    const std::string raw = "<think>internal trace</think>\n\nVisible answer";
    pkchat::tui::ThinkingDisplay shown = pkchat::tui::thinking_display_text(raw, true);
    check(shown.text == raw, "TUI thinking trace mode keeps raw assistant text");

    pkchat::tui::ThinkingDisplay hidden = pkchat::tui::thinking_display_text(raw, false);
    check(hidden.saw_thinking_tag, "TUI thinking display detects hidden trace tags");
    check(!hidden.open_thinking_tag, "TUI thinking display detects closed trace tags");
    check(hidden.text == "Visible answer", "TUI thinking notrace hides closed trace blocks");

    hidden = pkchat::tui::thinking_display_text("<think>still reasoning", false);
    check(hidden.saw_thinking_tag, "TUI thinking display detects an open trace tag");
    check(hidden.open_thinking_tag, "TUI thinking display reports an open trace tag");
    check(hidden.text.empty(), "TUI thinking notrace hides an unfinished trace");

    hidden = pkchat::tui::thinking_display_text("Before <think>hidden</think> after", false);
    check(hidden.text == "Before  after", "TUI thinking notrace preserves visible text around a trace");
}

void test_tui_unicode_and_empty_status() {
    check(pkchat::tui::ready_status().find("ready") != std::string::npos,
          "TUI ready status reports ready state");
    const std::string unicode_model = u8"模型-مرحبا-👨‍👩‍👧‍👦";
    pkchat::provider::ChatResult result;
    const std::string status = pkchat::tui::generation_ready_status(
        u8"提供商", unicode_model, result, false, {}, 0);
    check(status.find(unicode_model) != std::string::npos &&
              status.find(u8"提供商") != std::string::npos,
          "TUI generation status preserves Unicode provider and model names");

    pkchat::tui::ThinkingDisplay hidden =
        pkchat::tui::thinking_display_text("", false);
    check(hidden.text.empty() && !hidden.saw_thinking_tag,
          "TUI thinking display handles empty assistant text");
}

}  // namespace

void run_all() {
    test_tui_history_jump_helpers();
    test_tui_unicode_and_empty_status();
    test_tui_layout_reserves_editor_input_panel();
    test_tui_ready_and_generation_status();
    test_tui_regeneration_plan_uses_last_user_turn();
    test_tui_theme_parsing_and_contrast();
    test_tui_thinking_trace_display();
}

}  // namespace pkchat::test::tui
