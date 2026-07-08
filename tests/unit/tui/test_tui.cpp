#include "tui/test_tui.hpp"
#include "support/test_support.hpp"
#include "output/thinking.hpp"
#include "pkchat/version.hpp"
#include "provider/provider.hpp"
#include "tui/activity.hpp"
#include "editor/terminal_input.hpp"
#include "tui/input_handlers.hpp"
#include "tui/session_load.hpp"
#include "tui/tui.hpp"
#include <algorithm>
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
                                              ". TAB command/path /help Alt+enter newline Ctrl+B/D scroll chat Alt+Home/End jump",
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

void test_tui_last_editable_chat_message_finds_last_user_or_assistant() {
    pkchat::chat::Session session;
    session.messages.push_back({"system", "stay concise"});
    std::size_t index = 0;
    check(!pkchat::tui::last_editable_chat_message(session, index),
          "TUI history edit helper rejects system-only sessions");

    session.messages.push_back({"user", "first"});
    session.messages.push_back({"assistant", "one"});
    check(pkchat::tui::last_editable_chat_message(session, index) && index == 2,
          "TUI history edit helper selects the last assistant message");
    session.messages.push_back({"user", "second"});
    check(pkchat::tui::last_editable_chat_message(session, index) && index == 3,
          "TUI history edit helper selects the last user message");
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
        pkchat::tui::StyleRole::ThinkingActivity,
        pkchat::tui::StyleRole::StreamingActivity,
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

void test_tui_input_label_and_activity_indicators() {
    const std::string label = pkchat::tui::input_label_text();
    check(label.find("Pkchat v") == 0, "TUI input label starts with Pkchat branding");
    check(label.find(pkchat::kVersion) != std::string::npos, "TUI input label includes the current version");
    check(label.find("input:") != std::string::npos, "TUI input label ends with input prompt");

    const std::string thinking_a =
        pkchat::tui::activity_indicator_text(pkchat::tui::ActivityKind::Thinking, 0);
    const std::string thinking_b =
        pkchat::tui::activity_indicator_text(pkchat::tui::ActivityKind::Thinking, 1);
    const std::string thinking_c =
        pkchat::tui::activity_indicator_text(pkchat::tui::ActivityKind::Thinking, 2);
    check(!thinking_a.empty() && thinking_a != thinking_b && thinking_b != thinking_c &&
              thinking_c != thinking_a,
          "TUI thinking activity indicator rotates across frames");
    check(pkchat::tui::activity_indicator_width(pkchat::tui::ActivityKind::Thinking) <= 4,
          "TUI thinking activity indicator stays within four cells");

    const std::string streaming_a =
        pkchat::tui::activity_indicator_text(pkchat::tui::ActivityKind::Streaming, 0);
    const std::string streaming_b =
        pkchat::tui::activity_indicator_text(pkchat::tui::ActivityKind::Streaming, 1);
    const std::string streaming_c =
        pkchat::tui::activity_indicator_text(pkchat::tui::ActivityKind::Streaming, 2);
    const std::string streaming_loop =
        pkchat::tui::activity_indicator_text(pkchat::tui::ActivityKind::Streaming, 3);
    check(streaming_a != streaming_b && streaming_b != streaming_c && streaming_c != streaming_a,
          "TUI streaming activity indicator rotates across three frames");
    check(streaming_loop == streaming_a, "TUI streaming activity indicator loops after three frames");
    check(pkchat::tui::activity_indicator_width(pkchat::tui::ActivityKind::Streaming) == 3,
          "TUI streaming activity indicator uses three cells");
    check(streaming_a != thinking_a, "TUI streaming and thinking indicators differ");

    pkchat::chat::Session session;
    session.messages.push_back({"user", "hello"});
    session.messages.push_back({"assistant", "<think>hidden</think>"});
    check(pkchat::tui::activity_kind_for_pending_assistant(session, 1, false) ==
              pkchat::tui::ActivityKind::Thinking,
          "TUI activity helper reports thinking for hidden trace-only output");
    session.messages.back().content = "Visible answer";
    check(pkchat::tui::activity_kind_for_pending_assistant(session, 1, false) ==
              pkchat::tui::ActivityKind::Streaming,
          "TUI activity helper reports streaming once visible answer text arrives");

    const auto thinking_segments = pkchat::tui::activity_status_segments(
        "[custom / Qwen3.6-35B]", pkchat::tui::ActivityKind::Thinking, 0, "thinking...");
    check(thinking_segments.size() >= 3, "TUI thinking activity status uses styled segments");
    check(thinking_segments[0].text.find("custom") != std::string::npos,
          "TUI thinking activity status keeps provider label");
    check(thinking_segments.size() >= 3 &&
              thinking_segments[2].role == pkchat::tui::StyleRole::ThinkingActivity,
          "TUI thinking activity status colors the indicator");
    check(!thinking_segments[2].text.empty(),
          "TUI thinking activity status renders a rotating indicator");
}

void test_tui_provider_display_and_activity_status() {
    check(pkchat::provider::display_name_for_profile("custom_openai_chat") == "custom",
          "TUI provider display name shortens custom_openai_chat to custom");
}

void test_tui_selectable_provider_ids() {
    const std::vector<std::string> providers = pkchat::tui::selectable_provider_ids();
    check(!providers.empty(), "TUI provider picker includes at least one provider");
    check(std::find(providers.begin(), providers.end(), "none") == providers.end(),
          "TUI provider picker excludes the offline none profile");
    check(std::find(providers.begin(), providers.end(), "custom_openai_chat") == providers.end(),
          "TUI provider picker excludes custom_openai_chat without a base URL");
    check(std::find(providers.begin(), providers.end(), "lm_studio") != providers.end(),
          "TUI provider picker includes lm_studio");
}

void test_tui_chat_startup_status() {
    pkchat::provider::RequestContext offline;
    offline.profile.offline = true;
    check(pkchat::tui::chat_startup_status(offline).find("/provider") != std::string::npos,
          "TUI startup status prompts for provider when offline");

    pkchat::provider::RequestContext missing_model;
    missing_model.profile.name = "lm_studio";
    check(pkchat::tui::chat_startup_status(missing_model).find("/model") != std::string::npos,
          "TUI startup status prompts for model when provider is configured");

    pkchat::provider::RequestContext ready;
    ready.profile.name = "lm_studio";
    ready.options.model = "qwen-local";
    const std::string ready_status = pkchat::tui::chat_startup_status(ready);
    check(ready_status.find("/provider") != std::string::npos &&
              ready_status.find("/list") != std::string::npos,
          "TUI startup status reminds about provider changes and thread list when ready");
}

void test_tui_provider_and_model_picker_text() {
    const std::vector<std::string> providers = {"lm_studio", "openai"};
    const std::string provider_text = pkchat::tui::provider_picker_text(providers, 1);
    check(provider_text.find("Enter select") != std::string::npos,
          "TUI provider picker text documents Enter selection");
    check(provider_text.find(u8"› openai") != std::string::npos,
          "TUI provider picker highlights the selected provider");

    const std::vector<std::string> models = {"alpha", "beta"};
    const std::string model_text = pkchat::tui::model_picker_text(models, 0);
    check(model_text.find(u8"› alpha") != std::string::npos,
          "TUI model picker highlights the selected model");
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

void test_tui_ctrl_chat_history_scroll_shortcuts() {
    pkchat::tui::Layout layout = pkchat::tui::layout_for_terminal(24, 80);
    int history_scroll = 0;
    pkchat::tui::scroll_chat_history_page_up(layout, history_scroll);
    check(history_scroll > 0, "Ctrl+B helper scrolls chat history back");
    pkchat::tui::scroll_chat_history_page_down(layout, history_scroll);
    check(history_scroll == 0, "Ctrl+D helper scrolls chat history forward");
}

void test_tui_chat_history_scroll_keys() {
    pkchat::tui::Layout layout = pkchat::tui::layout_for_terminal(24, 80);
    int history_scroll = 0;

    pkchat::editor::MovementKeyEvent plain_page_up;
    plain_page_up.key = pkchat::editor::MovementKey::PageUp;
    check(!pkchat::tui::apply_chat_history_scroll(plain_page_up, layout, history_scroll),
          "plain PageUp does not scroll chat history");

    pkchat::editor::MovementKeyEvent alt_page_up;
    alt_page_up.key = pkchat::editor::MovementKey::PageUp;
    alt_page_up.alt = true;
    check(pkchat::tui::apply_chat_history_scroll(alt_page_up, layout, history_scroll),
          "Alt+PageUp scrolls chat history");
    check(history_scroll > 0, "Alt+PageUp increases history scroll offset");

    pkchat::editor::MovementKeyEvent alt_home;
    alt_home.key = pkchat::editor::MovementKey::Home;
    alt_home.alt = true;
    check(pkchat::tui::apply_chat_history_scroll(alt_home, layout, history_scroll),
          "Alt+Home jumps to the oldest chat history");
    check(history_scroll == pkchat::tui::history_scroll_for_thread_beginning(),
          "Alt+Home uses the thread-beginning scroll sentinel");

    history_scroll = 12;
    pkchat::editor::MovementKeyEvent alt_end;
    alt_end.key = pkchat::editor::MovementKey::End;
    alt_end.alt = true;
    check(pkchat::tui::apply_chat_history_scroll(alt_end, layout, history_scroll),
          "Alt+End returns to the live chat bottom");
    check(history_scroll == 0, "Alt+End resets history scroll offset");
}

void test_tui_read_terminal_input_marks_alt_meta_prefix() {
    pkchat::editor::clear_terminal_input_queue();
    pkchat::editor::push_terminal_input_bytes("\x1b\x1b[5~");

    pkchat::editor::TerminalInputEvent event;
    check(pkchat::editor::read_terminal_input(event, 0) &&
              event.type == pkchat::editor::TerminalInputType::Byte && event.byte == 27,
          "Alt+PageUp read_terminal_input returns the leading ESC byte");

    pkchat::editor::EditorState input = pkchat::editor::EditorState::from_text("hello");
    pkchat::tui::Layout layout = pkchat::tui::layout_for_terminal(24, 80);
    int history_scroll = 0;
    std::string status;
    const pkchat::tui::EscapeResult result =
        pkchat::tui::handle_escape(input, layout, history_scroll, status, false);
    check(result == pkchat::tui::EscapeResult::Handled,
          "pending Alt/meta prefix makes PageUp scroll chat history");
    check(history_scroll > 0, "pending Alt/meta prefix scrolls chat history");
    check(input.cursor == 0, "pending Alt/meta prefix does not move the input cursor");
}

void test_tui_handle_escape_alt_pageup_scrolls_history() {
    pkchat::editor::clear_terminal_input_queue();
    pkchat::editor::push_terminal_input_bytes("\x1b[5~");

    pkchat::editor::EditorState input = pkchat::editor::EditorState::from_text("hello");
    pkchat::tui::Layout layout = pkchat::tui::layout_for_terminal(24, 80);
    int history_scroll = 0;
    std::string status;
    const pkchat::tui::EscapeResult result =
        pkchat::tui::handle_escape(input, layout, history_scroll, status, false);
    check(result == pkchat::tui::EscapeResult::Handled,
          "Alt+PageUp escape sequence is handled");
    check(history_scroll > 0, "Alt+PageUp scrolls chat history");
    check(input.cursor == 0, "Alt+PageUp does not move the input cursor");
}

void test_tui_handle_escape_plain_pageup_moves_input() {
    pkchat::editor::clear_terminal_input_queue();
    pkchat::editor::push_terminal_input_bytes("[5~");

    std::string text = "line0\nline1\nline2\nline3\nline4";
    pkchat::editor::EditorState input = pkchat::editor::EditorState::from_text(text);
    input.cursor = input.text.size();
    pkchat::tui::Layout layout = pkchat::tui::layout_for_terminal(24, 80);
    int history_scroll = 0;
    std::string status;
    const size_t cursor_before = input.cursor;
    const pkchat::tui::EscapeResult result =
        pkchat::tui::handle_escape(input, layout, history_scroll, status, false);
    check(result == pkchat::tui::EscapeResult::Handled, "plain PageUp escape sequence is handled");
    check(history_scroll == 0, "plain PageUp does not scroll chat history");
    check(input.cursor < cursor_before, "plain PageUp moves the input cursor");
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
    test_tui_input_label_and_activity_indicators();
    test_tui_provider_display_and_activity_status();
    test_tui_selectable_provider_ids();
    test_tui_chat_startup_status();
    test_tui_provider_and_model_picker_text();
    test_tui_unicode_and_empty_status();
    test_tui_layout_reserves_editor_input_panel();
    test_tui_ready_and_generation_status();
    test_tui_ctrl_chat_history_scroll_shortcuts();
    test_tui_chat_history_scroll_keys();
    test_tui_read_terminal_input_marks_alt_meta_prefix();
    test_tui_handle_escape_alt_pageup_scrolls_history();
    test_tui_handle_escape_plain_pageup_moves_input();
    test_tui_last_unanswered_user_message_requires_final_user();
    test_tui_last_editable_chat_message_finds_last_user_or_assistant();
    test_tui_pop_last_chat_message_removes_user_or_assistant_only();
    test_tui_regeneration_plan_uses_last_user_turn();
    test_tui_theme_parsing_and_contrast();
    test_tui_thinking_trace_display();
}

}  // namespace pkchat::test::tui
