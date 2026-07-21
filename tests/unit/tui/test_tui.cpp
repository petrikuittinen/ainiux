#include "tui/test_tui.hpp"
#include "support/test_support.hpp"
#include "output/thinking.hpp"
#include "ainiux/version.hpp"
#include "provider/provider.hpp"
#include "tui/activity.hpp"
#include "editor/terminal_input.hpp"
#include "tui/input_handlers.hpp"
#include "tui/picker_input.hpp"
#include "tui/provider_actions.hpp"
#include "tui/session_load.hpp"
#include "config/config.hpp"
#include "tui/theme_registry.hpp"
#include "editor/assist_runtime.hpp"
#include "editor/editor_assist.hpp"
#include "editor/path_completion.hpp"
#include "tui/chat_assist.hpp"
#include "tui/tui.hpp"
#include "tui/detail/render.hpp"
#include <algorithm>
#include <string>
#include <vector>

namespace ainiux::test::tui {

namespace {

using ainiux::test::check;
using ainiux::test::read_fixture;

void test_tui_history_jump_helpers() {
    check(ainiux::tui::history_scroll_for_thread_beginning() > 1000000,
          "TUI Home jump requests a clamped scrollback maximum");
    check(ainiux::tui::history_scroll_for_thread_end() == 0,
          "TUI End jump returns to the live chat bottom");
}

void test_tui_provider_change_resets_only_on_actual_change() {
    ainiux::cli::Options options;
    options.tui = true;
    options.provider = "lmstudio";
    options.model = "remembered-model";
    options.reasoning = ainiux::ReasoningSelection::named("high");
    ainiux::provider::ContextResult built = ainiux::provider::build_context(options);
    check(built.error.ok(), "TUI provider reset test builds a local context");

    ainiux::chat::Session session;
    bool show_thinking_traces = false;
    std::string status;
    check(ainiux::tui::apply_selected_provider(built.context,
                                               session,
                                               show_thinking_traces,
                                               "lmstudio",
                                               status) &&
              built.context.options.model == "remembered-model" &&
              built.context.options.reasoning ==
                  ainiux::ReasoningSelection::named("high"),
          "TUI reselecting the actual provider preserves model and reasoning");
    check(ainiux::tui::apply_selected_provider(built.context,
                                               session,
                                               show_thinking_traces,
                                               "ollama",
                                               status) &&
              built.context.options.model.empty() &&
              built.context.options.reasoning.is_auto(),
          "TUI actual provider change clears model and resets reasoning to Auto");
}

void test_tui_reasoning_picker_input() {
    ainiux::tui::TuiMode mode = ainiux::tui::TuiMode::ReasoningList;
    bool quit = false;
    std::string status;
    std::vector<std::string> items = {"auto", "low", "high"};
    size_t selected = 0;
    bool picker_cancel_quits = false;
    std::vector<ainiux::chat::ThreadSummary> threads;
    size_t thread_selected = 0;
    size_t pending_thread_delete = 0;
    ainiux::tui::TuiPickerInputState state{
        mode,
        quit,
        status,
        items,
        selected,
        picker_cancel_quits,
        threads,
        thread_selected,
        true,
        pending_thread_delete,
    };
    std::string accepted;
    ainiux::tui::TuiPickerCallbacks callbacks;
    callbacks.on_reasoning_selected =
        [&](const std::string& value) { accepted = value; };

    ainiux::editor::clear_terminal_input_queue();
    ainiux::editor::push_terminal_input_bytes("[B");
    check(ainiux::tui::handle_tui_picker_input(27, state, callbacks) &&
              selected == 1 && status == "Selected reasoning 2/3",
          "TUI reasoning selector navigates through configured options");
    check(ainiux::tui::handle_tui_picker_input('\n', state, callbacks) &&
              accepted == "low",
          "TUI reasoning selector accepts the highlighted canonical value");

    mode = ainiux::tui::TuiMode::ReasoningList;
    items = {"auto", "low"};
    selected = 1;
    ainiux::editor::clear_terminal_input_queue();
    check(ainiux::tui::handle_tui_picker_input(27, state, callbacks) &&
              mode == ainiux::tui::TuiMode::Chat && items.empty() &&
              status == "Reasoning selection cancelled",
          "TUI reasoning selector cancellation returns to chat");

    bool confirmed = false;
    bool rejected = false;
    callbacks.on_reasoning_confirm_accepted = [&]() { confirmed = true; };
    callbacks.on_reasoning_confirm_rejected = [&]() { rejected = true; };
    callbacks.on_reasoning_confirm_retry = [&](const std::string& message) { status = message; };
    mode = ainiux::tui::TuiMode::ReasoningConfirm;
    check(ainiux::tui::handle_tui_picker_input('x', state, callbacks) &&
              status.find("Press y") == 0 && !confirmed && !rejected,
          "TUI unlisted reasoning confirmation rejects ambiguous input");
    check(ainiux::tui::handle_tui_picker_input('y', state, callbacks) && confirmed,
          "TUI unlisted reasoning confirmation accepts y");
    confirmed = false;
    check(ainiux::tui::handle_tui_picker_input(27, state, callbacks) && rejected && !confirmed,
          "TUI unlisted reasoning confirmation treats Esc as cancellation");
}

void test_tui_layout_reserves_editor_input_panel() {
    ainiux::tui::Layout small = ainiux::tui::layout_for_terminal(8, 20);
    check(small.rows == 8 && small.cols == 20, "TUI layout clamps to requested small terminal");
    check(small.header_rows == 0 && small.history_row == 1, "TUI layout has no persistent header rows");
    check(small.history_rows >= 1, "TUI layout leaves room for chat history");
    check(small.input_rect.height == 3, "TUI layout keeps minimum multiline input height");
    check(small.input_rect.row + small.input_rect.height - 1 <= small.rows,
          "TUI input panel stays inside terminal rows");

    ainiux::tui::Layout large = ainiux::tui::layout_for_terminal(40, 100);
    check(large.input_rect.height == 8, "TUI layout uses one fifth of a large terminal for input");
    check(large.input_rect.width == 100, "TUI input panel tracks terminal width");
    check(large.history_rows > large.input_rect.height, "TUI layout keeps the editor from taking the full screen");
}

void test_tui_sqlite_unavailable_status() {
    const std::string reason =
        "could not open SQLite database: /home/test/.ainiux/ainiux.db: file is not a database";
    const std::string status = ainiux::tui::sqlite_unavailable_status(reason);
    check(status.find("Saved chat database unavailable:") == 0,
          "SQLite unavailable status names the saved chat database");
    check(status.find(reason) != std::string::npos,
          "SQLite unavailable status includes the underlying open error");

    const std::string fallback = ainiux::tui::sqlite_unavailable_status("");
    check(fallback.find("Saved chat database unavailable") == 0 &&
              fallback.find("~/.ainiux/ainiux.db") != std::string::npos,
          "SQLite unavailable status without details suggests moving the database aside");
}

void test_chat_assist_command_completions_include_configured_commands() {
    const ainiux::editor::EditorAssistConfig config = ainiux::editor::default_editor_assist_config();
    const std::vector<std::string> completions = ainiux::editor::chat_assist_command_completions(config);
    check(std::find(completions.begin(), completions.end(), "/spell") != completions.end(),
          "chat assist completions include /spell from editor-commands defaults");
    check(std::find(completions.begin(), completions.end(), "/Chinese") != completions.end(),
          "chat assist completions include /Chinese from editor-commands defaults");
    check(std::find(completions.begin(), completions.end(), "/help") != completions.end(),
          "chat assist completions include chat /help");
    check(std::find(completions.begin(), completions.end(), "/editor") != completions.end(),
          "chat assist completions include chat /editor");
    check(std::find(completions.begin(), completions.end(), "/Chinese selection") == completions.end(),
          "chat assist completions omit scoped editor variants");
    check(std::find(completions.begin(), completions.end(), "/Chinese newbuffer") == completions.end(),
          "chat assist completions omit newbuffer scoped variants");
}

void test_chat_assist_request_text_strips_content_tags() {
    const std::vector<ainiux::provider::Message> messages = {
        {"system", "rules"},
        {"user", "<content>Helo world</content>"},
    };
    check(ainiux::editor::assist_request_text_from_messages(messages) == "Helo world",
          "assist request text helper strips content wrapper tags");
}

void test_chat_assist_turn_prompt_uses_configured_command_text() {
    const ainiux::editor::EditorAssistConfig config = ainiux::editor::default_editor_assist_config();
    const ainiux::editor::ParsedAssistCommand chinese =
        ainiux::editor::parse_assist_command("/Chinese", config);
    const std::optional<std::string> chinese_prompt =
        ainiux::tui::chat_assist_turn_prompt(chinese, config);
    check(chinese.ok && chinese_prompt.has_value(), "chat /Chinese parses as a configured command");
    check(chinese_prompt->find("Chinese") != std::string::npos,
          "chat /Chinese turn prompt asks for Chinese translation");

    const ainiux::editor::ParsedAssistCommand finnish =
        ainiux::editor::parse_assist_command("/Finnish", config);
    const std::optional<std::string> finnish_prompt =
        ainiux::tui::chat_assist_turn_prompt(finnish, config);
    check(finnish.ok && finnish_prompt.has_value(), "chat /Finnish parses as a configured command");
    check(finnish_prompt->find("Finnish") != std::string::npos,
          "chat /Finnish turn prompt asks for Finnish translation");
    check(finnish_prompt->find("suomi") == std::string::npos,
          "chat /Finnish turn prompt is the configured instruction, not a translation");
}

void test_chat_slash_command_tab_completion_matches_assist_commands() {
    ainiux::editor::ContextualCompleter completer;
    const ainiux::editor::EditorAssistConfig config = ainiux::editor::default_editor_assist_config();
    completer.set_assist_config(&config);

    ainiux::editor::EditorState state = ainiux::editor::EditorState::from_text("/eng");
    state.mode = ainiux::editor::EditorMode::Chat;
    state.cursor = state.text.size();
    check(ainiux::editor::is_chat_slash_command_tab_completion(state),
          "chat slash-command tab completion is active on the first token");
    const ainiux::editor::PathCompletionResult result = completer.complete(state);
    check(result.handled && result.changed && result.match_count == 1,
          "chat tab completion expands a case-insensitive assist command prefix");
    check(state.text.str() == "/English",
          "chat tab completion resolves /eng to /English");

    completer.reset();
    ainiux::editor::EditorState chinese = ainiux::editor::EditorState::from_text("/Chi");
    chinese.mode = ainiux::editor::EditorMode::Chat;
    chinese.cursor = chinese.text.size();
    const ainiux::editor::PathCompletionResult chinese_result = completer.complete(chinese);
    check(chinese_result.handled && chinese_result.changed && chinese_result.match_count == 1,
          "chat tab completion completes a unique assist prefix in one press");
    check(chinese.text.str() == "/Chinese",
          "chat tab completion resolves /Chi to /Chinese");
}

void test_configured_assist_slash_command_detection() {
    const ainiux::editor::EditorAssistConfig config = ainiux::editor::default_editor_assist_config();
    check(ainiux::editor::is_configured_assist_slash_command("/spell all", config),
          "configured assist slash command detection matches /spell");
    check(!ainiux::editor::is_configured_assist_slash_command("/model gpt-4", config),
          "configured assist slash command detection ignores unrelated chat commands");
    const ainiux::editor::ParsedAssistCommand parsed =
        ainiux::editor::parse_assist_command("/grammar selection", config);
    check(parsed.ok && parsed.kind == ainiux::editor::AssistCommandKind::Configured,
          "chat and editor share parse_assist_command for configured commands");

    ainiux::provider::RequestContext context;
    ainiux::chat::Session session;
    ainiux::editor::EditorState input = ainiux::editor::EditorState::from_text("ordinary Chinese text");
    std::string status;
    int history_scroll = 0;
    bool started = false;
    ainiux::tui::ChatAssistCallbacks callbacks;
    callbacks.start_turn = [&](const std::string&) { started = true; };
    const bool slashless_handled = ainiux::tui::try_handle_chat_assist_command(
        "Chinese", input, config, context, session, status, history_scroll, callbacks);
    check(!slashless_handled && !started,
          "slashless assist names remain ordinary chat message text");
    const bool slashed_handled = ainiux::tui::try_handle_chat_assist_command(
        "/Chinese", input, config, context, session, status, history_scroll, callbacks);
    check(slashed_handled && started,
          "slash-prefixed assist names remain chat commands");
}

void test_tui_ready_and_generation_status() {
    check(ainiux::tui::ready_status() ==
              "Tab complete | Ctrl+Space continue | Alt+Enter newline",
          "TUI ready status displays compact input hints");
    check(ainiux::tui::ready_status().size() <= 80,
          "TUI ready status fits an 80-column terminal");

    ainiux::provider::ChatResult result;
    result.ttft_ms = 100;
    result.total_ms = 1100;
    result.completion_tokens = 20;
    result.completion_tokens_estimated = true;

    const std::string streaming =
        ainiux::tui::generation_ready_status("lm_studio", "gpt-test", result, true, {}, 0);
    check(streaming.find("[lmstudio / gpt-test]") == 0,
          "TUI streaming completion status starts with compact provider and model names");
    check(streaming.find("TTFT: 100 ms") != std::string::npos,
          "TUI streaming completion status displays time to first token");
    check(streaming.find("| ~20.0 token/s") != std::string::npos,
          "TUI streaming completion status estimates throughput after the first token");

    const std::string non_streaming =
        ainiux::tui::generation_ready_status("lm_studio", "gpt-test", result, false, {}, 0);
    check(non_streaming.find("Response: 1100 ms") != std::string::npos,
          "TUI non-streaming completion status reports response latency instead of TTFT");
    check(non_streaming.find("| ~18.2 token/s") != std::string::npos,
          "TUI non-streaming completion status estimates whole-response throughput");

    result.completion_tokens_estimated = false;
    const std::string exact =
        ainiux::tui::generation_ready_status("lm_studio", "gpt-test", result, true, {}, 0);
    check(exact.find("| 20.0 token/s") != std::string::npos &&
              exact.find("| ~20.0 token/s") == std::string::npos,
          "TUI completion status omits the estimate marker for provider-reported usage");

    result.completion_tokens_estimated = true;
    result.usage_json = "{\"prompt_tokens\":20,\"completion_tokens\":5,\"total_tokens\":25}";
    const std::vector<ainiux::provider::Message> messages = {
        {"user", "hi"}, {"assistant", "<think>x</think>ok"}};
    const std::string context_status =
        ainiux::tui::generation_ready_status("lm_studio", "gpt-test", result, true, messages, 100);
    check(context_status.find("TTFT 100ms | ~20.0 token/s") != std::string::npos,
          "TUI context status uses compact timing and estimated-throughput notation");
    check(context_status.find("context: 25 (25%)") != std::string::npos,
          "TUI completion status displays estimated context usage");

    result.usage_json = "null";
    const std::string exhausted =
        ainiux::tui::generation_ready_status("lm_studio", "gpt-test", result, true, messages, 10);
    check(exhausted.find("context: 17 (170%)") != std::string::npos,
          "TUI context estimate reports usage beyond the configured window");
}

void test_tui_last_unanswered_user_message_requires_final_user() {
    ainiux::chat::Session session;
    std::size_t index = 42;
    check(!ainiux::tui::last_unanswered_user_message(session, index),
          "TUI response helper rejects empty sessions");

    session.messages.push_back({"system", "stay concise"});
    session.messages.push_back({"user", "first"});
    check(ainiux::tui::last_unanswered_user_message(session, index) && index == 1,
          "TUI response helper accepts a final user message");

    session.messages.push_back({"assistant", "one"});
    check(!ainiux::tui::last_unanswered_user_message(session, index),
          "TUI response helper rejects user messages that already have assistant replies");
}

void test_tui_last_editable_chat_message_finds_last_user_or_assistant() {
    ainiux::chat::Session session;
    session.messages.push_back({"system", "stay concise"});
    std::size_t index = 0;
    check(!ainiux::tui::last_editable_chat_message(session, index),
          "TUI history edit helper rejects system-only sessions");

    session.messages.push_back({"user", "first"});
    session.messages.push_back({"assistant", "one"});
    check(ainiux::tui::last_editable_chat_message(session, index) && index == 2,
          "TUI history edit helper selects the last assistant message");
    session.messages.push_back({"user", "second"});
    check(ainiux::tui::last_editable_chat_message(session, index) && index == 3,
          "TUI history edit helper selects the last user message");
}

void test_tui_pop_last_chat_message_removes_user_or_assistant_only() {
    ainiux::chat::Session session;
    session.messages.push_back({"system", "stay concise"});
    std::string removed_role;
    check(!ainiux::tui::pop_last_chat_message(session, removed_role),
          "TUI pop leaves system-only sessions unchanged");
    check(session.messages.size() == 1 && removed_role.empty(),
          "TUI pop reports no removed role for system-only sessions");

    session.messages.push_back({"user", "first"});
    session.messages.push_back({"assistant", "one"});
    session.usage_json = "{\"prompt_tokens\":1,\"completion_tokens\":1,\"total_tokens\":2}";
    check(ainiux::tui::pop_last_chat_message(session, removed_role),
          "TUI pop removes the last assistant message");
    check(removed_role == "assistant" && session.messages.size() == 2 &&
              session.messages.back().role == "user",
          "TUI pop reports assistant and leaves the prior user message last");
    check(session.usage_json == "{}",
          "TUI pop clears stale usage metadata when removing an assistant response");
    check(ainiux::tui::pop_last_chat_message(session, removed_role),
          "TUI pop removes the last user message");
    check(removed_role == "user" && session.messages.size() == 1 &&
              session.messages.back().role == "system",
          "TUI pop reports user and preserves the system prompt");
}

void test_tui_regeneration_plan_uses_last_user_turn() {
    ainiux::chat::Session session;
    session.messages.push_back({"system", "stay concise"});
    session.messages.push_back({"user", "first"});
    session.messages.push_back({"assistant", "one"});
    session.messages.push_back({"user", "second"});
    session.messages.push_back({"assistant", "two"});

    ainiux::tui::RegenerationPlan plan = ainiux::tui::regeneration_plan_for_session(session);
    check(plan.available, "TUI regeneration plan is available when a user turn exists");
    check(plan.erase_from == 3, "TUI regeneration plan erases from the last user turn");
    check(plan.prompt == "second", "TUI regeneration plan reuses the last user prompt");

    ainiux::chat::Session no_user;
    no_user.messages.push_back({"system", "only system"});
    plan = ainiux::tui::regeneration_plan_for_session(no_user);
    check(!plan.available, "TUI regeneration plan is unavailable without a user turn");
}

void test_tui_theme_parsing_and_contrast() {
    const ainiux::tui::ThemeRegistry registry = ainiux::tui::default_theme_registry();
    std::string theme;
    check(registry.normalize_name("dark", theme), "TUI dark theme parses");
    check(theme == "dark", "TUI dark theme selected");
    check(registry.normalize_name("Light", theme), "TUI light theme parses case-insensitively");
    check(theme == "light", "TUI light theme selected");
    check(!registry.normalize_name("sepia", theme), "TUI rejects unknown theme");

    const std::vector<std::string> themes = registry.names();
    const std::vector<ainiux::tui::StyleRole> roles = {
        ainiux::tui::StyleRole::Text,
        ainiux::tui::StyleRole::Muted,
        ainiux::tui::StyleRole::ThinkingTrace,
        ainiux::tui::StyleRole::UserLabel,
        ainiux::tui::StyleRole::AssistantLabel,
        ainiux::tui::StyleRole::Error,
        ainiux::tui::StyleRole::Status,
        ainiux::tui::StyleRole::InputLabel,
        ainiux::tui::StyleRole::ThinkingActivity,
        ainiux::tui::StyleRole::StreamingActivity,
        ainiux::tui::StyleRole::PanelTitle,
        ainiux::tui::StyleRole::PanelBorder,
        ainiux::tui::StyleRole::PanelHint,
        ainiux::tui::StyleRole::PanelHighlight,
        ainiux::tui::StyleRole::PanelBody,
        ainiux::tui::StyleRole::SyntaxComment,
        ainiux::tui::StyleRole::SyntaxKeyword,
        ainiux::tui::StyleRole::SyntaxType,
        ainiux::tui::StyleRole::SyntaxString,
        ainiux::tui::StyleRole::SyntaxNumber,
        ainiux::tui::StyleRole::SyntaxLiteral,
        ainiux::tui::StyleRole::SyntaxFunction,
        ainiux::tui::StyleRole::SyntaxVariable,
        ainiux::tui::StyleRole::SyntaxOperator,
        ainiux::tui::StyleRole::SyntaxPreprocessor,
        ainiux::tui::StyleRole::SyntaxTag,
        ainiux::tui::StyleRole::SyntaxAttribute,
        ainiux::tui::StyleRole::SyntaxProperty,
        ainiux::tui::StyleRole::SyntaxHeading,
        ainiux::tui::StyleRole::SyntaxEmphasis,
        ainiux::tui::StyleRole::SyntaxLink,
    };

    for (const std::string& item : themes) {
        for (ainiux::tui::StyleRole role : roles) {
            const ainiux::tui::StylePair pair = ainiux::tui::style_pair_for(registry, item, role);
            check(ainiux::tui::contrast_ratio(pair.foreground, pair.background) >= 4.5,
                  std::string("TUI theme contrast meets WCAG AA for ") + item);
        }
        const ainiux::tui::Rgb link =
            ainiux::tui::style_pair_for(registry, item, ainiux::tui::StyleRole::SyntaxLink)
                .foreground;
        const ainiux::tui::Rgb url =
            ainiux::tui::style_pair_for(registry, item, ainiux::tui::StyleRole::SyntaxAttribute)
                .foreground;
        check(link.r != url.r || link.g != url.g || link.b != url.b,
              std::string("TUI Markdown URL color differs from link text for ") + item);
    }

    const ainiux::tui::StylePair dark_text =
        ainiux::tui::style_pair_for(registry, "dark", ainiux::tui::StyleRole::Text);
    const ainiux::tui::StylePair dark_thinking =
        ainiux::tui::style_pair_for(registry, "dark", ainiux::tui::StyleRole::ThinkingTrace);
    check(ainiux::tui::contrast_ratio(dark_thinking.foreground, dark_thinking.background) <
              ainiux::tui::contrast_ratio(dark_text.foreground, dark_text.background),
          "TUI dark thinking trace text is dimmer than normal text");

    const ainiux::tui::StylePair light_text =
        ainiux::tui::style_pair_for(registry, "light", ainiux::tui::StyleRole::Text);
    const ainiux::tui::StylePair light_thinking =
        ainiux::tui::style_pair_for(registry, "light", ainiux::tui::StyleRole::ThinkingTrace);
    check(ainiux::tui::contrast_ratio(light_thinking.foreground, light_thinking.background) <
              ainiux::tui::contrast_ratio(light_text.foreground, light_text.background),
          "TUI light thinking trace text is less stark than normal text");

    ainiux::config::ParseResult parsed = ainiux::config::read_file("config/themes.conf");
    check(parsed.error.ok(), "themes.conf parses");
    ainiux::cli::Options options;
    ainiux::Error err = ainiux::config::apply_themes_document(parsed.document, options);
    check(err.ok(), "themes.conf applies");
    check(options.tui_themes.has("dark") && options.tui_themes.has("light") &&
              options.tui_themes.has("sepia"),
          "themes.conf defines built-in dark, light, and sepia themes");
    for (const std::string& item : options.tui_themes.names()) {
        for (ainiux::tui::StyleRole role : roles) {
            if (role < ainiux::tui::StyleRole::SyntaxComment) {
                continue;
            }
            const ainiux::tui::StylePair pair =
                ainiux::tui::style_pair_for(options.tui_themes, item, role);
            check(ainiux::tui::contrast_ratio(pair.foreground, pair.background) >= 4.5,
                  std::string("configured syntax color meets WCAG AA for ") + item);
        }
    }

    const ainiux::tui::ThemeCommandResult listed =
        ainiux::tui::handle_theme_command(options.tui_themes, "dark", "", true);
    check(listed.ok && listed.message.find("dark") != std::string::npos &&
              listed.message.find("light") != std::string::npos,
          "theme command lists configured themes");
    const ainiux::tui::ThemeCommandResult switched =
        ainiux::tui::handle_theme_command(options.tui_themes, "dark", "light", true);
    check(switched.ok && switched.selected_theme == "light", "theme command switches themes");
}

void test_tui_buffer_list_uses_colored_panel_widget() {
    const std::string text =
        "Buffers - Enter opens - N new - DEL close - Esc cancels\n> file1.txt - Ln 1, Col 1\n  file2.txt - Ln 2, Col 3";
    const std::vector<ainiux::tui::StyledLine> lines =
        ainiux::tui::detail::panel_lines_for_text(text, ainiux::tui::TuiMode::ThreadList, 80, "Buffers");

    check(!lines.empty() && !lines.front().segments.empty() &&
              lines.front().segments.front().role == ainiux::tui::StyleRole::PanelBorder,
          "buffer list panel starts with a colored border rule");
    bool saw_title = false;
    bool saw_hint = false;
    bool saw_selected = false;
    for (const ainiux::tui::StyledLine& line : lines) {
        for (const ainiux::tui::StyledSegment& segment : line.segments) {
            saw_title = saw_title || segment.role == ainiux::tui::StyleRole::PanelTitle;
            saw_hint = saw_hint || segment.role == ainiux::tui::StyleRole::PanelHint;
            saw_selected = saw_selected || segment.role == ainiux::tui::StyleRole::PanelHighlight;
        }
    }
    check(saw_title, "buffer list panel uses the shared title styling");
    check(saw_hint, "buffer list panel uses the shared hint styling");
    check(saw_selected, "buffer list panel uses the shared selected-row styling");

    for (ainiux::tui::TuiMode mode : {ainiux::tui::TuiMode::ProviderList,
                                      ainiux::tui::TuiMode::ModelList}) {
        const std::vector<ainiux::tui::StyledLine> selector_lines =
            ainiux::tui::detail::panel_lines_for_text(
                u8"↑↓ move · Enter select · Esc cancel\n› selected\n  other", mode, 80);
        bool selector_title = false;
        bool selector_hint = false;
        bool selector_highlight = false;
        for (const ainiux::tui::StyledLine& line : selector_lines) {
            for (const ainiux::tui::StyledSegment& segment : line.segments) {
                selector_title = selector_title || segment.role == ainiux::tui::StyleRole::PanelTitle;
                selector_hint = selector_hint || segment.role == ainiux::tui::StyleRole::PanelHint;
                selector_highlight =
                    selector_highlight || segment.role == ainiux::tui::StyleRole::PanelHighlight;
            }
        }
        check(selector_title && selector_hint && selector_highlight,
              "provider/model selectors use the shared colored panel roles");
    }
}

void test_tui_thinking_trace_display() {
    const std::string raw = "<think>internal trace</think>\n\nVisible answer";
    ainiux::tui::ThinkingDisplay shown = ainiux::tui::thinking_display_text(raw, true);
    check(shown.text == raw, "TUI thinking trace mode keeps raw assistant text");

    ainiux::tui::ThinkingDisplay hidden = ainiux::tui::thinking_display_text(raw, false);
    check(hidden.saw_thinking_tag, "TUI thinking display detects hidden trace tags");
    check(!hidden.open_thinking_tag, "TUI thinking display detects closed trace tags");
    check(hidden.text == "Visible answer", "TUI thinking notrace hides closed trace blocks");

    hidden = ainiux::tui::thinking_display_text("<think>still reasoning", false);
    check(hidden.saw_thinking_tag, "TUI thinking display detects an open trace tag");
    check(hidden.open_thinking_tag, "TUI thinking display reports an open trace tag");
    check(hidden.text.empty(), "TUI thinking notrace hides an unfinished trace");

    hidden = ainiux::tui::thinking_display_text("Before <think>hidden</think> after", false);
    check(hidden.text == "Before  after", "TUI thinking notrace preserves visible text around a trace");
}

void test_tui_markdown_history_highlighting() {
    ainiux::chat::Session session;
    session.messages.push_back({"user", "# Heading\nPlain *emphasis* and [link](https://example.test)"});
    std::vector<ainiux::tui::StyledLine> lines =
        ainiux::tui::detail::history_lines_for_session(
            session, 100, false, ainiux::tui::ActivityKind::None, 0, true);
    bool saw_heading = false;
    bool saw_emphasis = false;
    bool saw_link = false;
    bool saw_link_url = false;
    for (const ainiux::tui::StyledLine& line : lines) {
        for (const ainiux::tui::StyledSegment& segment : line.segments) {
            saw_heading = saw_heading || segment.role == ainiux::tui::StyleRole::SyntaxHeading;
            saw_emphasis = saw_emphasis || segment.role == ainiux::tui::StyleRole::SyntaxEmphasis;
            saw_link = saw_link || segment.role == ainiux::tui::StyleRole::SyntaxLink;
            saw_link_url = saw_link_url ||
                           segment.role == ainiux::tui::StyleRole::SyntaxAttribute;
        }
    }
    check(saw_heading && saw_emphasis && saw_link && saw_link_url,
          "TUI chat history gives Markdown link text and URLs distinct semantic colors");

    lines = ainiux::tui::detail::history_lines_for_session(
        session, 100, false, ainiux::tui::ActivityKind::None, 0, false);
    bool saw_syntax = false;
    for (const ainiux::tui::StyledLine& line : lines) {
        for (const ainiux::tui::StyledSegment& segment : line.segments) {
            saw_syntax = saw_syntax || segment.role == ainiux::tui::StyleRole::SyntaxHeading ||
                         segment.role == ainiux::tui::StyleRole::SyntaxEmphasis ||
                         segment.role == ainiux::tui::StyleRole::SyntaxLink;
        }
    }
    check(!saw_syntax, "TUI chat /highlight off path renders Markdown as plain text");

    session.messages.clear();
    session.messages.push_back({"assistant", "<think># private</think>\n# public"});
    lines = ainiux::tui::detail::history_lines_for_session(
        session, 100, true, ainiux::tui::ActivityKind::None, 0, true);
    bool saw_thinking = false;
    saw_heading = false;
    for (const ainiux::tui::StyledLine& line : lines) {
        for (const ainiux::tui::StyledSegment& segment : line.segments) {
            saw_thinking = saw_thinking || segment.role == ainiux::tui::StyleRole::ThinkingTrace;
            saw_heading = saw_heading || segment.role == ainiux::tui::StyleRole::SyntaxHeading;
        }
    }
    check(saw_thinking && saw_heading,
          "TUI thinking-trace style keeps priority while visible Markdown remains highlighted");

    session.messages.clear();
    session.messages.push_back({"assistant", "```python\ndef greet(name: str):\n    return 17\n```"});
    lines = ainiux::tui::detail::history_lines_for_session(
        session, 100, false, ainiux::tui::ActivityKind::None, 0, true);
    bool saw_keyword = false;
    bool saw_type = false;
    bool saw_number = false;
    for (const ainiux::tui::StyledLine& line : lines) {
        for (const ainiux::tui::StyledSegment& segment : line.segments) {
            saw_keyword = saw_keyword || segment.role == ainiux::tui::StyleRole::SyntaxKeyword;
            saw_type = saw_type || segment.role == ainiux::tui::StyleRole::SyntaxType;
            saw_number = saw_number || segment.role == ainiux::tui::StyleRole::SyntaxNumber;
        }
    }
    check(saw_keyword && saw_type && saw_number,
          "TUI chat highlights recognized languages inside Markdown fences");

    session.messages.clear();
    session.messages.push_back(
        {"assistant",
         "```html\n<button style=\"color: #fff\" "
         "onclick=\"const value = 17; run(value);\">Go</button>\n```"});
    lines = ainiux::tui::detail::history_lines_for_session(
        session, 100, false, ainiux::tui::ActivityKind::None, 0, true);
    bool saw_property = false;
    saw_keyword = false;
    saw_number = false;
    for (const ainiux::tui::StyledLine& line : lines) {
        for (const ainiux::tui::StyledSegment& segment : line.segments) {
            saw_property = saw_property || segment.role == ainiux::tui::StyleRole::SyntaxProperty;
            saw_keyword = saw_keyword || segment.role == ainiux::tui::StyleRole::SyntaxKeyword;
            saw_number = saw_number || segment.role == ainiux::tui::StyleRole::SyntaxNumber;
        }
    }
    check(saw_property && saw_keyword && saw_number,
          "TUI chat renders CSS and JavaScript inside HTML Markdown fences");

    session.messages.clear();
    session.messages.push_back(
        {"assistant",
         "```sql\nSELECT count(*) FROM articles WHERE active = TRUE;\n```\n"
         "```yaml\nenabled: true\n```"});
    lines = ainiux::tui::detail::history_lines_for_session(
        session, 100, false, ainiux::tui::ActivityKind::None, 0, true);
    saw_keyword = false;
    saw_property = false;
    bool saw_literal = false;
    for (const ainiux::tui::StyledLine& line : lines) {
        for (const ainiux::tui::StyledSegment& segment : line.segments) {
            saw_keyword = saw_keyword || segment.role == ainiux::tui::StyleRole::SyntaxKeyword;
            saw_property = saw_property || segment.role == ainiux::tui::StyleRole::SyntaxProperty;
            saw_literal = saw_literal || segment.role == ainiux::tui::StyleRole::SyntaxLiteral;
        }
    }
    check(saw_keyword && saw_property && saw_literal,
          "TUI chat highlights newly supported SQL and YAML Markdown fences");
}

void test_tui_agent_history_chrome() {
    ainiux::chat::Session session;
    session.messages.push_back({"user", "fix the attempts"});
    session.messages.push_back({"tool", "1: read_file(\"game.py\") → ok"});
    session.messages.push_back({"assistant", "Updated medium and hard to 8 attempts."});

    // Chat mode keeps classic labels.
    std::vector<ainiux::tui::StyledLine> chat_lines =
        ainiux::tui::detail::history_lines_for_session(
            session, 100, false, ainiux::tui::ActivityKind::None, 0, false, false);
    bool saw_you = false;
    bool saw_assistant_label = false;
    for (const auto& line : chat_lines) {
        if (line.segments.empty()) continue;
        if (line.segments.front().text.find("You:") != std::string::npos) saw_you = true;
        if (line.segments.front().text.find("Assistant:") != std::string::npos)
            saw_assistant_label = true;
    }
    check(saw_you && saw_assistant_label, "chat mode keeps You:/Assistant: labels");

    // Agent mode: "> prompt", no Assistant:/Tool: labels, no hanging indent on tools.
    std::vector<ainiux::tui::StyledLine> agent_lines =
        ainiux::tui::detail::history_lines_for_session(
            session, 100, false, ainiux::tui::ActivityKind::None, 0, false, true);
    bool saw_prompt_marker = false;
    bool saw_assistant_label_agent = false;
    bool saw_tool_label = false;
    bool saw_tool_body = false;
    bool saw_answer = false;
    for (const auto& line : agent_lines) {
        std::string joined;
        for (const auto& seg : line.segments) joined += seg.text;
        if (!line.segments.empty() && line.segments.front().text == "> ") saw_prompt_marker = true;
        if (joined.find("Assistant:") != std::string::npos) saw_assistant_label_agent = true;
        if (joined.find("Tool:") != std::string::npos) saw_tool_label = true;
        if (joined.find("1: read_file") != std::string::npos) saw_tool_body = true;
        if (joined.find("Updated medium") != std::string::npos) saw_answer = true;
    }
    check(saw_prompt_marker, "agent mode shows user prompts as \"> \"");
    check(!saw_assistant_label_agent, "agent mode omits Assistant: label");
    check(!saw_tool_label, "agent mode omits Tool: label");
    check(saw_tool_body && saw_answer, "agent mode still shows tool lines and answers flush-left");
}

void test_tui_input_label_and_activity_indicators() {
    const std::string label = ainiux::tui::input_label_text();
    check(label == ainiux::tui::input_label_text_for_mode(false),
          "TUI input label concatenates app version branding with helper text");
    check(label.find(ainiux::versionNumber) != std::string::npos, "TUI input label includes the current version");
    check(label.find("/help") != std::string::npos &&
              label.find(u8"history Ctrl+B ↑ Ctrl+D ↓") != std::string::npos,
          "TUI input label shows compact help and history navigation hints");
    check(label.size() <= 80, "TUI input label fits an 80-column terminal");

    const std::string thinking_a =
        ainiux::tui::activity_indicator_text(ainiux::tui::ActivityKind::Thinking, 0);
    const std::string thinking_b =
        ainiux::tui::activity_indicator_text(ainiux::tui::ActivityKind::Thinking, 1);
    const std::string thinking_c =
        ainiux::tui::activity_indicator_text(ainiux::tui::ActivityKind::Thinking, 2);
    check(!thinking_a.empty() && thinking_a != thinking_b && thinking_b != thinking_c &&
              thinking_c != thinking_a,
          "TUI thinking activity indicator rotates across frames");
    check(ainiux::tui::activity_indicator_width(ainiux::tui::ActivityKind::Thinking) <= 4,
          "TUI thinking activity indicator stays within four cells");

    const std::string streaming_a =
        ainiux::tui::activity_indicator_text(ainiux::tui::ActivityKind::Streaming, 0);
    const std::string streaming_b =
        ainiux::tui::activity_indicator_text(ainiux::tui::ActivityKind::Streaming, 1);
    const std::string streaming_c =
        ainiux::tui::activity_indicator_text(ainiux::tui::ActivityKind::Streaming, 2);
    const std::string streaming_loop =
        ainiux::tui::activity_indicator_text(ainiux::tui::ActivityKind::Streaming, 3);
    check(streaming_a != streaming_b && streaming_b != streaming_c && streaming_c != streaming_a,
          "TUI streaming activity indicator rotates across three frames");
    check(streaming_loop == streaming_a, "TUI streaming activity indicator loops after three frames");
    check(ainiux::tui::activity_indicator_width(ainiux::tui::ActivityKind::Streaming) == 3,
          "TUI streaming activity indicator uses three cells");
    check(streaming_a != thinking_a, "TUI streaming and thinking indicators differ");

    ainiux::chat::Session session;
    session.messages.push_back({"user", "hello"});
    session.messages.push_back({"assistant", "<think>hidden</think>"});
    check(ainiux::tui::activity_kind_for_pending_assistant(session, 1, false) ==
              ainiux::tui::ActivityKind::Thinking,
          "TUI activity helper reports thinking for hidden trace-only output");
    session.messages.back().content = "Visible answer";
    check(ainiux::tui::activity_kind_for_pending_assistant(session, 1, false) ==
              ainiux::tui::ActivityKind::Streaming,
          "TUI activity helper reports streaming once visible answer text arrives");

    const auto thinking_segments = ainiux::tui::activity_status_segments(
        "[custom / Qwen3.6-35B]", ainiux::tui::ActivityKind::Thinking, 0, "thinking...");
    check(thinking_segments.size() >= 3, "TUI thinking activity status uses styled segments");
    check(thinking_segments[0].text.find("custom") != std::string::npos,
          "TUI thinking activity status keeps provider label");
    check(thinking_segments.size() >= 3 &&
              thinking_segments[2].role == ainiux::tui::StyleRole::ThinkingActivity,
          "TUI thinking activity status colors the indicator");
    check(!thinking_segments[2].text.empty(),
          "TUI thinking activity status renders a rotating indicator");
}

void test_tui_provider_display_and_activity_status() {
    check(ainiux::provider::display_name_for_profile("custom_openai_chat") == "custom",
          "TUI provider display name shortens custom_openai_chat to custom");

    ainiux::chat::Session session;
    session.provider = "gemini";
    session.model = "models/gemini-3.1-flash-lite-preview";
    check(ainiux::tui::session_status_label(session) ==
              u8"[gemini / gemini-3.1-flash-lite-pre…]",
          "TUI activity status strips and truncates provider-prefixed model names");
}

void test_tui_chat_startup_status() {
    ainiux::provider::RequestContext offline;
    offline.profile.offline = true;
    check(ainiux::tui::chat_startup_status(offline).find("/provider") != std::string::npos,
          "TUI startup status prompts for provider when offline");
    check(ainiux::tui::chat_startup_status(offline).find("/list") != std::string::npos &&
              ainiux::tui::chat_startup_status(offline).find("enable sending") != std::string::npos,
          "offline TUI status explains browsing and disabled sending without opening a picker");

    ainiux::provider::RequestContext missing_model;
    missing_model.profile.name = "lm_studio";
    check(ainiux::tui::chat_startup_status(missing_model).find("/model") != std::string::npos,
          "TUI startup status prompts for model when provider is configured");

    ainiux::provider::RequestContext ready;
    ready.profile.name = "lm_studio";
    ready.options.model = "qwen-local";
    const std::string ready_status = ainiux::tui::chat_startup_status(ready);
    check(ready_status.find("[lmstudio / qwen-local]") == 0,
          "TUI startup status shows provider and model when ready");
    check(ready_status.find("/provider") != std::string::npos &&
              ready_status.find("/list") != std::string::npos,
          "TUI startup status reminds about provider changes and thread list when ready");

    check(!ainiux::tui::chat_provider_model_ready(offline),
          "TUI send readiness rejects a missing provider");
    check(!ainiux::tui::chat_provider_model_ready(missing_model),
          "TUI send readiness rejects a missing model");
    check(ainiux::tui::chat_provider_model_ready(ready),
          "TUI send readiness accepts a configured provider and model");
    check(ainiux::tui::chat_provider_model_required_status(missing_model)
                  .find("sending disabled") != std::string::npos,
          "TUI explains that sending is disabled until model setup is complete");
    check(ainiux::tui::chat_provider_model_required_status(ready, true)
                  .find("/provider, then /model") != std::string::npos,
          "TUI can require provider reselection before model selection");
}

void test_tui_incomplete_thread_labels() {
    check(ainiux::tui::saved_provider_model_complete("openrouter", "model-id"),
          "TUI recognizes complete saved provider/model metadata");
    check(!ainiux::tui::saved_provider_model_complete("openrouter", ""),
          "TUI recognizes a missing saved model");
    check(!ainiux::tui::saved_provider_model_complete("none", "model-id"),
          "TUI treats the offline profile as a missing saved provider");

    ainiux::chat::ThreadSummary missing_model;
    missing_model.id = 1;
    missing_model.name = "What kills trolls";
    missing_model.modified_at = "2026-07-18T00:00:00Z";
    missing_model.last_provider = "openrouter";
    missing_model.message_count = 4;
    const std::string model_text = ainiux::tui::thread_picker_text({missing_model}, 0);
    check(model_text.find("[SETUP: model missing] What kills trolls") != std::string::npos,
          "TUI thread picker puts a missing-model warning before the thread title");

    ainiux::chat::ThreadSummary missing_both = missing_model;
    missing_both.last_provider.clear();
    const std::string both_text = ainiux::tui::thread_picker_text({missing_both}, 0);
    check(both_text.find("[SETUP: provider+model missing]") != std::string::npos,
          "TUI thread picker clearly labels missing provider and model metadata");
}

void test_tui_loaded_context_does_not_inherit_model_or_endpoints() {
    ainiux::cli::Options active_options;
    active_options.tui = true;
    active_options.key = "test-key";
    ainiux::provider::apply_provider_target(active_options, "http://localhost:30000/v1");
    active_options.model = "local-model";
    active_options.chat_url = "http://localhost:30000/v1/chat/completions";
    active_options.models_url = "http://localhost:30000/v1/models";
    ainiux::provider::ContextResult built = ainiux::provider::build_context(active_options);
    check(built.error.ok(), "TUI regression test builds the prior local request context");
    if (!built.error.ok()) {
        return;
    }

    ainiux::chat::Session loaded;
    loaded.provider = "openrouter";
    loaded.base_url = "https://openrouter.ai/api/v1";
    loaded.model.clear();
    loaded.settings_json = "{\"reasoning\":3072}";
    const ainiux::Error applied =
        ainiux::tui::apply_loaded_session_to_context(built.context, loaded);
    check(applied.ok(), "TUI applies an incomplete saved OpenRouter context");
    if (!applied.ok()) {
        return;
    }
    check(built.context.profile.name == "openrouter",
          "TUI restores the saved provider for an incomplete thread");
    check(built.context.options.model.empty(),
          "TUI preserves an empty saved model instead of inheriting the local model");
    check(built.context.chat_url.find("localhost:30000") == std::string::npos &&
              built.context.models_url.find("localhost:30000") == std::string::npos,
          "TUI clears endpoint overrides inherited from the previous thread");
    check(built.context.chat_url.find("openrouter.ai") != std::string::npos,
          "TUI rebuilds the request URL from the saved provider context");
    check(built.context.options.reasoning ==
              ainiux::ReasoningSelection::token_budget(3072),
          "TUI thread loading restores the complete saved reasoning selection");
}

void test_tui_session_load_model_mismatch_detection() {
    ainiux::provider::RequestContext active_context;
    active_context.profile.name = "lm_studio";
    active_context.options.model = "qwen-local";

    ainiux::chat::Session loaded;
    loaded.provider = "openai";
    loaded.model = "gpt-4";
    check(ainiux::tui::loaded_session_differs_from_context(active_context, loaded),
          "TUI session load detects provider and model mismatch against active context");

    loaded.provider = "lm_studio";
    loaded.model = "qwen-local";
    check(!ainiux::tui::loaded_session_differs_from_context(active_context, loaded),
          "TUI session load accepts matching provider and model");

    loaded.provider = "lmstudio";
    loaded.model = "other-model";
    check(ainiux::tui::loaded_session_differs_from_context(active_context, loaded),
          "TUI session load detects model mismatch for provider aliases");

    active_context.options.model.clear();
    loaded.model = "other-model";
    check(!ainiux::tui::loaded_session_differs_from_context(active_context, loaded),
          "TUI session load skips model prompt when active model is empty");

    ainiux::provider::RequestContext offline_context;
    offline_context.profile.name = "none";
    offline_context.profile.offline = true;
    loaded.provider = "openai";
    loaded.model = "gpt-4";
    check(!ainiux::tui::loaded_session_differs_from_context(offline_context, loaded),
          "TUI session load skips mismatch prompt before provider selection");

    active_context.profile.name = "lm_studio";
    active_context.options.model = "qwen-local";
    ainiux::provider::RequestContext cli_context = offline_context;
    loaded.provider = "openai";
    loaded.model = "gpt-4";
    check(ainiux::tui::loaded_session_differs_from_context(active_context, loaded),
          "TUI session load compares against in-session provider selection, not startup CLI");
    check(!ainiux::tui::loaded_session_differs_from_cli(cli_context, loaded),
          "TUI session load CLI helper still reflects startup defaults for compatibility");
}

void test_tui_session_load_model_confirm_text() {
    ainiux::provider::RequestContext active_context;
    active_context.profile.name = "lm_studio";
    active_context.options.model = "qwen-local";

    ainiux::chat::Session loaded;
    loaded.provider = "openai";
    loaded.model = "gpt-4";

    const std::string prompt = ainiux::tui::model_confirm_text(active_context, loaded);
    check(prompt.find("Keep current provider and model?") != std::string::npos,
          "TUI model confirm prompt asks whether to keep the active provider/model");
    check(prompt.find("Current:") != std::string::npos,
          "TUI model confirm prompt labels the active provider/model as current");
    check(prompt.find("gpt-4") != std::string::npos && prompt.find("qwen-local") != std::string::npos,
          "TUI model confirm prompt shows thread and current models");
}

void test_tui_restore_cli_context() {
    ainiux::provider::RequestContext cli_context;
    cli_context.profile.name = "lm_studio";
    cli_context.options.model = "cli-model";
    cli_context.options.provider = "lm_studio";

    ainiux::provider::RequestContext context = cli_context;
    context.profile.name = "openai";
    context.options.model = "thread-model";
    context.options.provider = "openai";

    ainiux::tui::restore_cli_context(context, cli_context);
    check(context.profile.name == "lm_studio" && context.options.model == "cli-model",
          "TUI restore_cli_context resets provider context to command-line defaults");
}

void test_tui_ctrl_chat_history_scroll_shortcuts() {
    ainiux::tui::Layout layout = ainiux::tui::layout_for_terminal(24, 80);
    int history_scroll = 0;
    ainiux::tui::scroll_chat_history_page_up(layout, history_scroll);
    check(history_scroll > 0, "Ctrl+B helper scrolls chat history back");
    ainiux::tui::scroll_chat_history_page_down(layout, history_scroll);
    check(history_scroll == 0, "Ctrl+D helper scrolls chat history forward");
}

void test_tui_chat_history_scroll_keys() {
    ainiux::tui::Layout layout = ainiux::tui::layout_for_terminal(24, 80);
    int history_scroll = 0;

    ainiux::editor::MovementKeyEvent plain_page_up;
    plain_page_up.key = ainiux::editor::MovementKey::PageUp;
    check(!ainiux::tui::apply_chat_history_scroll(plain_page_up, layout, history_scroll),
          "plain PageUp does not scroll chat history");

    ainiux::editor::MovementKeyEvent alt_page_up;
    alt_page_up.key = ainiux::editor::MovementKey::PageUp;
    alt_page_up.alt = true;
    check(ainiux::tui::apply_chat_history_scroll(alt_page_up, layout, history_scroll),
          "Alt+PageUp scrolls chat history");
    check(history_scroll > 0, "Alt+PageUp increases history scroll offset");

    ainiux::editor::MovementKeyEvent alt_home;
    alt_home.key = ainiux::editor::MovementKey::Home;
    alt_home.alt = true;
    check(ainiux::tui::apply_chat_history_scroll(alt_home, layout, history_scroll),
          "Alt+Home jumps to the oldest chat history");
    check(history_scroll == ainiux::tui::history_scroll_for_thread_beginning(),
          "Alt+Home uses the thread-beginning scroll sentinel");

    history_scroll = 12;
    ainiux::editor::MovementKeyEvent alt_end;
    alt_end.key = ainiux::editor::MovementKey::End;
    alt_end.alt = true;
    check(ainiux::tui::apply_chat_history_scroll(alt_end, layout, history_scroll),
          "Alt+End returns to the live chat bottom");
    check(history_scroll == 0, "Alt+End resets history scroll offset");
}

void test_tui_read_terminal_input_marks_alt_meta_prefix() {
    ainiux::editor::clear_terminal_input_queue();
    ainiux::editor::push_terminal_input_bytes("\x1b\x1b[5~");

    ainiux::editor::TerminalInputEvent event;
    check(ainiux::editor::read_terminal_input(event, 0) &&
              event.type == ainiux::editor::TerminalInputType::Byte && event.byte == 27,
          "Alt+PageUp read_terminal_input returns the leading ESC byte");

    ainiux::editor::EditorState input = ainiux::editor::EditorState::from_text("hello");
    ainiux::tui::Layout layout = ainiux::tui::layout_for_terminal(24, 80);
    int history_scroll = 0;
    std::string status;
    const ainiux::tui::EscapeResult result =
        ainiux::tui::handle_escape(input, layout, history_scroll, status, false);
    check(result == ainiux::tui::EscapeResult::Handled,
          "pending Alt/meta prefix makes PageUp scroll chat history");
    check(history_scroll > 0, "pending Alt/meta prefix scrolls chat history");
    check(input.cursor == 0, "pending Alt/meta prefix does not move the input cursor");
}

void test_tui_handle_escape_alt_pageup_scrolls_history() {
    ainiux::editor::clear_terminal_input_queue();
    ainiux::editor::push_terminal_input_bytes("\x1b[5~");

    ainiux::editor::EditorState input = ainiux::editor::EditorState::from_text("hello");
    ainiux::tui::Layout layout = ainiux::tui::layout_for_terminal(24, 80);
    int history_scroll = 0;
    std::string status;
    const ainiux::tui::EscapeResult result =
        ainiux::tui::handle_escape(input, layout, history_scroll, status, false);
    check(result == ainiux::tui::EscapeResult::Handled,
          "Alt+PageUp escape sequence is handled");
    check(history_scroll > 0, "Alt+PageUp scrolls chat history");
    check(input.cursor == 0, "Alt+PageUp does not move the input cursor");
}

void test_tui_handle_escape_plain_pageup_moves_input() {
    ainiux::editor::clear_terminal_input_queue();
    ainiux::editor::push_terminal_input_bytes("[5~");

    std::string text = "line0\nline1\nline2\nline3\nline4";
    ainiux::editor::EditorState input = ainiux::editor::EditorState::from_text(text);
    input.cursor = input.text.size();
    ainiux::tui::Layout layout = ainiux::tui::layout_for_terminal(24, 80);
    int history_scroll = 0;
    std::string status;
    const size_t cursor_before = input.cursor;
    const ainiux::tui::EscapeResult result =
        ainiux::tui::handle_escape(input, layout, history_scroll, status, false);
    check(result == ainiux::tui::EscapeResult::Handled, "plain PageUp escape sequence is handled");
    check(history_scroll == 0, "plain PageUp does not scroll chat history");
    check(input.cursor < cursor_before, "plain PageUp moves the input cursor");
}

void test_tui_unicode_and_empty_status() {
    check(ainiux::tui::ready_status().find(ainiux::app_version_label()) == std::string::npos,
          "TUI ready status leaves app version branding to the input label line");
    const std::string unicode_model = u8"模型-مرحبا-👨‍👩‍👧‍👦";
    ainiux::provider::ChatResult result;
    const std::string status = ainiux::tui::generation_ready_status(
        u8"提供商", unicode_model, result, false, {}, 0);
    check(status.find(unicode_model) != std::string::npos &&
              status.find(u8"提供商") != std::string::npos,
          "TUI generation status preserves Unicode provider and model names");

    ainiux::tui::ThinkingDisplay hidden =
        ainiux::tui::thinking_display_text("", false);
    check(hidden.text.empty() && !hidden.saw_thinking_tag,
          "TUI thinking display handles empty assistant text");
}

}  // namespace

void run_all() {
    test_tui_history_jump_helpers();
    test_tui_provider_change_resets_only_on_actual_change();
    test_tui_reasoning_picker_input();
    test_tui_session_load_model_mismatch_detection();
    test_tui_session_load_model_confirm_text();
    test_tui_incomplete_thread_labels();
    test_tui_loaded_context_does_not_inherit_model_or_endpoints();
    test_tui_restore_cli_context();
    test_tui_input_label_and_activity_indicators();
    test_tui_provider_display_and_activity_status();
    test_tui_chat_startup_status();
    test_tui_unicode_and_empty_status();
    test_tui_layout_reserves_editor_input_panel();
    test_tui_sqlite_unavailable_status();
    test_chat_assist_turn_prompt_uses_configured_command_text();
    test_chat_slash_command_tab_completion_matches_assist_commands();
    test_chat_assist_command_completions_include_configured_commands();
    test_chat_assist_request_text_strips_content_tags();
    test_configured_assist_slash_command_detection();
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
    test_tui_buffer_list_uses_colored_panel_widget();
    test_tui_thinking_trace_display();
    test_tui_markdown_history_highlighting();
    test_tui_agent_history_chrome();
}

}  // namespace ainiux::test::tui
