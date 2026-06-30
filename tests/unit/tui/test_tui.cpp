#include "tui/test_tui.hpp"
#include "support/test_support.hpp"
#include "output/thinking.hpp"
#include "pkchat/version.hpp"
#include "provider/provider.hpp"
#include "tui/session_load.hpp"
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
    check(pkchat::tui::ready_status() == std::string("pkchat v") + pkchat::kVersion +
                                              ". TAB command/path /help Alt+enter newline PageUp/PageDown scroll",
          "TUI ready status displays compact version and key hints");

    pkchat::provider::ChatResult result;
    result.ttft_ms = 100;
    result.total_ms = 1100;
    result.completion_tokens = 20;
    result.completion_tokens_estimated = true;

    const std::string streaming =
        pkchat::tui::generation_ready_status("lm_studio", "gpt-test", result, true, {}, 0);
    check(streaming.find("[lmstudio / gpt-test]") == 0,
          "TUI streaming completion status starts with compact provider and model names");
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

void test_tui_last_unanswered_user_message_requires_final_user() {
    pkchat::chat::Session session;
    std::size_t index = 42;
    check(!pkchat::tui::last_unanswered_user_message(session, index),
          "TUI response helper rejects empty sessions");

    session.messages.push_back({"system", "stay concise"});
    session.messages.push_back({"user", "first"});
    check(pkchat::tui::last_unanswered_user_message(session, index) && index == 1,
          "TUI response helper accepts a final user message");

    session.messages.push_back({"assistant", "one"});
    check(!pkchat::tui::last_unanswered_user_message(session, index),
          "TUI response helper rejects user messages that already have assistant replies");
}

void test_tui_pop_last_chat_message_removes_user_or_assistant_only() {
    pkchat::chat::Session session;
    session.messages.push_back({"system", "stay concise"});
    std::string removed_role;
    check(!pkchat::tui::pop_last_chat_message(session, removed_role),
          "TUI pop leaves system-only sessions unchanged");
    check(session.messages.size() == 1 && removed_role.empty(),
          "TUI pop reports no removed role for system-only sessions");

    session.messages.push_back({"user", "first"});
    session.messages.push_back({"assistant", "one"});
    session.usage_json = "{\"prompt_tokens\":1,\"completion_tokens\":1,\"total_tokens\":2}";
    check(pkchat::tui::pop_last_chat_message(session, removed_role),
          "TUI pop removes the last assistant message");
    check(removed_role == "assistant" && session.messages.size() == 2 &&
              session.messages.back().role == "user",
          "TUI pop reports assistant and leaves the prior user message last");
    check(session.usage_json == "{}",
          "TUI pop clears stale usage metadata when removing an assistant response");
    check(pkchat::tui::pop_last_chat_message(session, removed_role),
          "TUI pop removes the last user message");
    check(removed_role == "user" && session.messages.size() == 1 &&
              session.messages.back().role == "system",
          "TUI pop reports user and preserves the system prompt");
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
        pkchat::tui::StyleRole::PanelTitle,
        pkchat::tui::StyleRole::PanelBorder,
        pkchat::tui::StyleRole::PanelHint,
        pkchat::tui::StyleRole::PanelHighlight,
        pkchat::tui::StyleRole::PanelBody,
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

void test_tui_provider_display_and_activity_status() {
    check(pkchat::provider::display_name_for_profile("custom_openai_chat") == "custom",
          "TUI provider display name shortens custom_openai_chat to custom");

    pkchat::provider::RequestContext context;
    context.profile.name = "custom_openai_chat";
    context.options.model = "Qwen3.6-35B";
    const std::string thinking =
        pkchat::tui::provider_model_status_message(context, pkchat::tui::kThinkingActivityIndicator, "thinking...");
    check(thinking.find("[custom / Qwen3.6-35B]") == 0 &&
              thinking.find(pkchat::tui::kThinkingActivityIndicator) != std::string::npos &&
              thinking.find("thinking...") != std::string::npos,
          "TUI thinking activity status keeps label, indicator, and text");

    const std::string streaming = pkchat::tui::provider_model_status_message(
        context, pkchat::tui::kStreamingActivityIndicator, "streaming response ...");
    check(streaming.find(pkchat::tui::kStreamingActivityIndicator) != std::string::npos &&
              streaming.find("streaming response ...") != std::string::npos,
          "TUI streaming activity status keeps indicator and text");
}

void test_tui_session_load_model_mismatch_detection() {
    pkchat::provider::RequestContext cli_context;
    cli_context.profile.name = "lm_studio";
    cli_context.options.model = "qwen-local";

    pkchat::chat::Session loaded;
    loaded.provider = "openai";
    loaded.model = "gpt-4";
    check(pkchat::tui::loaded_session_differs_from_cli(cli_context, loaded),
          "TUI session load detects provider and model mismatch against command line");

    loaded.provider = "lm_studio";
    loaded.model = "qwen-local";
    check(!pkchat::tui::loaded_session_differs_from_cli(cli_context, loaded),
          "TUI session load accepts matching provider and model");

    loaded.provider = "lmstudio";
    loaded.model = "other-model";
    check(pkchat::tui::loaded_session_differs_from_cli(cli_context, loaded),
          "TUI session load detects model mismatch for provider aliases");

    cli_context.options.model.clear();
    loaded.model = "other-model";
    check(!pkchat::tui::loaded_session_differs_from_cli(cli_context, loaded),
          "TUI session load skips model prompt when command line model is empty");
}

void test_tui_session_load_model_confirm_text() {
    pkchat::provider::RequestContext cli_context;
    cli_context.profile.name = "lm_studio";
    cli_context.options.model = "qwen-local";

    pkchat::chat::Session loaded;
    loaded.provider = "openai";
    loaded.model = "gpt-4";

    const std::string prompt = pkchat::tui::model_confirm_text(cli_context, loaded);
    check(prompt.find("Use new model? (yes, otherwise defaults to old model)") != std::string::npos,
          "TUI model confirm prompt asks whether to use the command-line model");
    check(prompt.find("gpt-4") != std::string::npos && prompt.find("qwen-local") != std::string::npos,
          "TUI model confirm prompt shows thread and command-line models");
}

void test_tui_restore_cli_context() {
    pkchat::provider::RequestContext cli_context;
    cli_context.profile.name = "lm_studio";
    cli_context.options.model = "cli-model";
    cli_context.options.provider = "lm_studio";

    pkchat::provider::RequestContext context = cli_context;
    context.profile.name = "openai";
    context.options.model = "thread-model";
    context.options.provider = "openai";

    pkchat::tui::restore_cli_context(context, cli_context);
    check(context.profile.name == "lm_studio" && context.options.model == "cli-model",
          "TUI restore_cli_context resets provider context to command-line defaults");
}

void test_tui_unicode_and_empty_status() {
    check(pkchat::tui::ready_status().find("pkchat v") == 0,
          "TUI ready status starts with the pkchat version label");
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
    test_tui_session_load_model_mismatch_detection();
    test_tui_session_load_model_confirm_text();
    test_tui_restore_cli_context();
    test_tui_provider_display_and_activity_status();
    test_tui_unicode_and_empty_status();
    test_tui_layout_reserves_editor_input_panel();
    test_tui_ready_and_generation_status();
    test_tui_last_unanswered_user_message_requires_final_user();
    test_tui_pop_last_chat_message_removes_user_or_assistant_only();
    test_tui_regeneration_plan_uses_last_user_turn();
    test_tui_theme_parsing_and_contrast();
    test_tui_thinking_trace_display();
}

}  // namespace pkchat::test::tui
