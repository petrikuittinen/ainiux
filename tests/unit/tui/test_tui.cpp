#include "tui/test_tui.hpp"
#include "tui/background_metadata.hpp"
#include "support/test_support.hpp"
#include "app/user_shell.hpp"
#include "output/thinking.hpp"
#include "ainiux/version.hpp"
#include "provider/provider.hpp"
#include "tui/activity.hpp"
#include "tui/agent_widgets.hpp"
#include "tui/agent_progress.hpp"
#include "editor/terminal_input.hpp"
#include "editor/detail/wrap.hpp"
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
#include "tui/commands.hpp"
#include "tui/detail/frame_buffer.hpp"
#include "tui/tui.hpp"
#include "tui/detail/render.hpp"
#include "ui/provider_model_display.hpp"
#include "ui/scrollbar.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

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

void test_terminal_frame_renderer_updates_only_changed_rows() {
    ainiux::tui::detail::TerminalFrameRenderer renderer;
    ainiux::tui::detail::TerminalFrame first(3, 20);
    first.set_row(1, "\x1b[1;1Hfirst row");
    first.set_row(2, "\x1b[2;1Hsecond row");
    first.set_row(3, "\x1b[3;1Hthird row");
    first.cursor_row = 2;
    first.cursor_col = 4;

    std::ostringstream output;
    check(renderer.present(first, output) == 3 &&
              output.str().find("first row") != std::string::npos &&
              output.str().find("second row") != std::string::npos &&
              output.str().find("\x1b[?25l") != std::string::npos &&
              output.str().find("\x1b[?25h") != std::string::npos,
          "first retained TUI frame draws every row and brackets repaint with cursor visibility");

    output.str("");
    output.clear();
    check(renderer.present(first, output) == 0 && output.str().empty(),
          "identical retained TUI frame emits no terminal output");

    ainiux::tui::detail::TerminalFrame changed = first;
    changed.set_row(2, "\x1b[2;1Hchanged row");
    output.str("");
    output.clear();
    check(renderer.present(changed, output) == 1 &&
              output.str().find("changed row") != std::string::npos &&
              output.str().find("first row") == std::string::npos &&
              output.str().find("third row") == std::string::npos &&
              output.str().find("\x1b[?25l") == std::string::npos &&
              output.str().find("\x1b[?25h") == std::string::npos,
          "retained TUI frame emits only a changed row without toggling cursor visibility");

    changed.cursor_row = 3;
    changed.cursor_col = 2;
    output.str("");
    output.clear();
    check(renderer.present(changed, output) == 0 &&
              output.str() == "\x1b[3;2H",
          "cursor-only TUI update moves the cursor without hiding or showing it");

    changed.cursor_visible = false;
    output.str("");
    output.clear();
    check(renderer.present(changed, output) == 0 && output.str() == "\x1b[?25l",
          "retained TUI frame hides an off-screen editor caret without repainting rows");
    changed.cursor_visible = true;
    output.str("");
    output.clear();
    check(renderer.present(changed, output) == 0 && output.str() == "\x1b[?25h",
          "retained TUI frame restores cursor visibility explicitly");

    ainiux::tui::detail::TerminalFrame resized(4, 24);
    output.str("");
    output.clear();
    check(renderer.present(resized, output) == 4,
          "terminal resize invalidates every retained row");
}

void test_shared_tui_render_skips_identical_frame() {
    ainiux::chat::Session session;
    ainiux::editor::EditorState input =
        ainiux::tui::detail::empty_input_editor(10);
    std::string status = "Ready";
    int history_scroll = 0;
    ainiux::tui::detail::TerminalFrameRenderer renderer;
    std::ostringstream output;
    std::streambuf* previous = std::cout.rdbuf(output.rdbuf());

    ainiux::tui::detail::render(
        session, input, status, history_scroll, false,
        ainiux::tui::TuiMode::Chat, "", ainiux::tui::ActivityKind::None,
        0, false, true, {nullptr, "dark", false}, renderer);
    const std::string first = output.str();
    output.str("");
    output.clear();
    ainiux::tui::detail::render(
        session, input, status, history_scroll, false,
        ainiux::tui::TuiMode::Chat, "", ainiux::tui::ActivityKind::None,
        0, false, true, {nullptr, "dark", false}, renderer);
    const std::string second = output.str();
    output.str("");
    output.clear();
    ainiux::tui::detail::render(
        session, input, status, history_scroll, false,
        ainiux::tui::TuiMode::Chat, "", ainiux::tui::ActivityKind::None,
        0, false, false, {nullptr, "dark", false}, renderer);
    const std::string hidden_scrollbar = output.str();
    std::cout.rdbuf(previous);

    check(!first.empty() && second.empty(),
          "shared chat/agent TUI renderer emits nothing for an identical idle frame");
    check(!hidden_scrollbar.empty() &&
              hidden_scrollbar.find(ainiux::ui::kScrollbarThumbGlyph) == std::string::npos &&
              hidden_scrollbar.find(ainiux::ui::kScrollbarTrackGlyph) == std::string::npos,
          "shared chat/agent TUI renderer replaces a hidden scrollbar with blank cells");
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

    bool project_reset = false;
    bool project_declined = false;
    callbacks.on_agent_new_accepted = [&]() { project_reset = true; };
    callbacks.on_agent_new_rejected = [&]() { project_declined = true; };
    callbacks.on_agent_new_retry = [&](const std::string& message) { status = message; };
    mode = ainiux::tui::TuiMode::AgentNewConfirm;
    const std::string previous_status = status;
    check(ainiux::tui::handle_tui_picker_input('x', state, callbacks) &&
              status == previous_status && !project_reset && !project_declined,
          "agent project reset confirmation ignores invalid input");
    check(ainiux::tui::handle_tui_picker_input(27, state, callbacks) &&
              project_declined && !project_reset,
          "agent project reset defaults to No on Esc");
    project_declined = false;
    check(ainiux::tui::handle_tui_picker_input('r', state, callbacks) &&
              project_reset && !project_declined,
          "agent project reset accepts its semantic mnemonic");

    bool guard_allowed = false;
    bool guard_denied = false;
    callbacks.on_guard_approval_accepted = [&]() { guard_allowed = true; };
    callbacks.on_guard_approval_rejected = [&]() { guard_denied = true; };
    mode = ainiux::tui::TuiMode::GuardApprovalConfirm;
    check(ainiux::tui::handle_tui_picker_input('x', state, callbacks) &&
              !guard_allowed && !guard_denied,
          "agent Guard Yes/No ignores ambiguous input");
    check(ainiux::tui::handle_tui_picker_input('y', state, callbacks) &&
              guard_allowed && !guard_denied,
          "agent Guard accepts y as Yes");
    guard_allowed = false;
    check(ainiux::tui::handle_tui_picker_input(27, state, callbacks) &&
              guard_denied && !guard_allowed,
          "agent Guard treats Esc as No");

    bool continued = false;
    bool stopped = false;
    callbacks.on_agent_continue_accepted = [&]() { continued = true; };
    callbacks.on_agent_continue_rejected = [&]() { stopped = true; };
    mode = ainiux::tui::TuiMode::AgentContinueConfirm;
    check(ainiux::tui::handle_tui_picker_input('x', state, callbacks) &&
              !continued && !stopped,
          "agent continuation confirmation ignores ambiguous input");
    check(ainiux::tui::handle_tui_picker_input('c', state, callbacks) &&
              continued && !stopped,
          "agent continuation confirmation accepts its Continue mnemonic");
    continued = false;
    check(ainiux::tui::handle_tui_picker_input(27, state, callbacks) &&
              stopped && !continued,
          "agent continuation confirmation defaults to Stop on Esc");
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

    completer.reset();
    completer.set_agent_mode(true);
    ainiux::editor::EditorState show_index =
        ainiux::editor::EditorState::from_text("/show-i");
    show_index.mode = ainiux::editor::EditorMode::Chat;
    show_index.cursor = show_index.text.size();
    const ainiux::editor::PathCompletionResult show_index_result =
        completer.complete(show_index);
    check(show_index_result.handled && show_index_result.changed &&
              show_index_result.match_count == 1 &&
              show_index.text.str() == "/show-index",
          "agent tab completion exposes /show-index");

    completer.reset();
    ainiux::editor::EditorState index_code =
        ainiux::editor::EditorState::from_text("/index-c");
    index_code.mode = ainiux::editor::EditorMode::Chat;
    index_code.cursor = index_code.text.size();
    const ainiux::editor::PathCompletionResult index_code_result =
        completer.complete(index_code);
    check(index_code_result.handled && index_code_result.changed &&
              index_code_result.match_count == 1 &&
              index_code.text.str() == "/index-code",
          "agent tab completion exposes /index-code");
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

void test_tui_agent_chrome_formatters() {
    check(ainiux::tui::effective_agent_context_window(0) == 0,
          "agent chrome does not invent a context window");
    check(ainiux::tui::effective_agent_context_window(131072) == 131072,
          "agent chrome keeps an explicit context window");
    const std::string usage =
        ainiux::tui::format_agent_context_usage(187982, 262144);
    check(usage == "187982 tok (71.7%)",
          "agent chrome formats used tokens with one-decimal percent");
    check(ainiux::tui::format_agent_context_usage(87575, 0) == "87575 tok",
          "agent chrome shows token-only usage when model metadata has no context window");

    const std::string label = ainiux::tui::agent_provider_model_reasoning_label(
        "openrouter", "gpt-5.6-luna", "high");
    check(label == "[openrouter/gpt-5.6-luna high]",
          "agent chrome uses provider/model reasoning bracket form");

    const std::string line = ainiux::tui::agent_input_label_text(
        "openrouter", "gpt-5.6-luna", "high", 187982, 0);
    check(line.find(ainiux::versionNumber) != std::string::npos,
          "agent chrome line includes version");
    check(line.find("[openrouter/gpt-5.6-luna high]") != std::string::npos,
          "agent chrome line includes provider/model/reasoning");
    check(line.find("187982 tok") != std::string::npos &&
              line.find('%') == std::string::npos,
          "agent chrome omits percentage when model context metadata is unknown");

    ainiux::tui::AgentChrome chrome;
    chrome.enabled = true;
    chrome.provider = "openrouter";
    chrome.model = "gpt-5.6-luna";
    chrome.reasoning = "high";
    chrome.used_tokens = 187982;
    chrome.window_tokens = 262144;
    const auto segments = ainiux::tui::input_label_segments_for_mode(true, chrome);
    std::string joined;
    for (const auto& seg : segments) joined += seg.text;
    check(joined.find("tok") != std::string::npos && joined.find("%") != std::string::npos,
          "agent input-label segments include token usage");
    check(joined.find("openrouter/gpt-5.6-luna") != std::string::npos,
          "agent input-label segments include provider/model");
    check(ainiux::tui::agent_ready_status().find("agent") != std::string::npos,
          "agent ready status is a short idle hint");
}

void test_tui_background_metadata_refresh_policy() {
    using namespace ainiux::tui;
    ainiux::provider::RequestContext context;
    context.profile.name = "openai";
    context.options.model = "current-model";
    context.options.timeout_seconds = 0;
    const ainiux::provider::RequestContext automatic =
        automatic_metadata_request(context);
    check(automatic.options.timeout_seconds ==
              kAutomaticMetadataTimeoutSeconds,
          "automatic metadata refresh bounds an unlimited request at 15 seconds");
    context.options.timeout_seconds = 41;
    check(automatic_metadata_request(context).options.timeout_seconds == 41,
          "automatic metadata refresh preserves an explicit request timeout");
    context.options.context_tokens = 999;
    apply_automatic_context_catalog(context);
    check(context.options.context_tokens == 0,
          "automatic context refresh uses catalog/token-only fallback without /models");
}

void test_agent_widgets_and_dynamic_geometry() {
    using namespace ainiux::tui;
    const std::string status = agent_status_line(
        "custom/qwen3.6-35b-a3b-mtp", "auto", 1562, 131072, 80);
    check(status == std::string(ainiux::app_version_label()) +
                        " [qwen3.6-35b-a3b-mtp auto] 1562 tok (1.2%)",
          "agent status line omits the provider and retains model, reasoning, and usage");
    check(status.find("custom/") == std::string::npos &&
              ainiux::editor::detail::display_width_for_range(
                  status, 0, status.size()) <= 80,
          "agent status line fits 80 columns without a provider");

    const std::string ready =
        agent_activity_line(AgentActivityState::Ready, false, 0, -1, 80);
    check(ready == "Agent ready. /help /quit",
          "initial agent activity line includes idle commands");
    check(agent_activity_line(AgentActivityState::Preparing, true, 3, -1, 80) ==
              "Agent preparing (ESC to abort) 0:03",
          "agent preparation never renders as ready");
    check(agent_activity_line(AgentActivityState::Unavailable, false, 0, -1, 80) ==
              "Agent unavailable. Check the notice above or switch provider/model.",
          "failed preparation renders an explicit unavailable state");
    const std::string thinking =
        agent_activity_line(AgentActivityState::Thinking, true, 131, -1, 80);
    check(thinking == "Agent thinking (ESC to abort) 2:11",
          "thinking activity includes cancellation and live minute-second timing");
    const std::string working =
        agent_activity_line(AgentActivityState::Working, true, 7, -1, 80);
    check(working == "Agent working (ESC to abort) 0:07",
          "working activity includes zero-padded elapsed seconds");
    const std::string compacting =
        agent_activity_line(AgentActivityState::Compacting, true, 2, -1, 80);
    check(compacting == "Agent compacting... (ESC to abort) 0:02",
          "compacting activity has its own label and animated dots");
    const std::string completed =
        agent_activity_line(AgentActivityState::Ready, false, 0, 4960, 80);
    check(completed == "Agent ready. Task completed in 4.96 seconds.",
          "completed activity reports the real task duration");

    const std::string narrow_status = agent_status_line(
        "very-long-model-name-that-needs-shortening", "high",
        1562, 131072, 48);
    check(ainiux::editor::detail::display_width_for_range(
                  narrow_status, 0, narrow_status.size()) <= 48,
          "narrow agent status is clipped to terminal width");

    AgentInputGeometry one_line = agent_input_geometry(40, 80, 1, 25);
    AgentInputGeometry multiline = agent_input_geometry(40, 80, 6, 25);
    AgentInputGeometry capped = agent_input_geometry(40, 80, 100, 25);
    check(one_line.box_height == 3 && multiline.box_height == 8 &&
              capped.box_height == 10,
          "agent frame grows with visual rows and caps the entire box at 25 percent");
    AgentInputGeometry tiny = agent_input_geometry(6, 20, 20, 10);
    check(tiny.box_height == 3 && tiny.content_rect.height == 1,
          "tiny terminals retain history, activity, status, and a three-row prompt box");
    Layout agent_layout = layout_for_agent_terminal(40, 80, multiline.box_height);
    Layout chat_layout = layout_for_terminal(40, 80);
    check(agent_layout.activity_row + 1 == agent_layout.status_row &&
              agent_layout.status_row + 1 == agent_layout.input_label_row &&
              agent_layout.input_rect.height == 6 && chat_layout.input_rect.height == 8,
          "dynamic agent input geometry does not change the fixed chat layout");

    AgentInputFrame frame{"/home/eye/my_code_project", "act", "smart", ""};
    check(abbreviate_agent_workspace(frame.workspace) == "~/my_code_project",
          "agent frame abbreviates the home directory");
    check(agent_input_title(frame, 40) == "~/my_code_project act",
          "agent frame title includes workspace and mode");
    check(agent_input_title(
              {"/home/eye/escape_prison", "goal", "smart", ""}, 40) ==
              "~/escape_prison goal",
          "agent frame title shows goal while a session goal is active");
    check(agent_input_title(
              {"/a/very/long/leading/path/project", "plan", "smart", ""}, 18)
              .find("project plan") !=
              std::string::npos,
          "agent frame elision preserves the final project directory and future mode");
    const std::string top_border = agent_input_top_border(frame, 40);
    check(top_border.find(u8"┌─ ~/my_code_project act ") == 0 &&
              top_border.find(" smart ") != std::string::npos &&
              top_border.size() >= std::string(u8"┐").size() &&
              top_border.compare(top_border.size() - std::string(u8"┐").size(),
                                 std::string(u8"┐").size(), u8"┐") == 0 &&
              ainiux::editor::detail::display_width_for_range(
                  top_border, 0, top_border.size()) == 40 &&
              agent_input_bottom_border(40).find(u8"└") == 0,
          "agent frame constructs project/task and permission borders");
    frame.credit_label = "4.50 USD";
    const std::string credit_border = agent_input_top_border(frame, 48);
    check(credit_border.find(" smart 4.50 USD ") != std::string::npos &&
              credit_border.compare(
                  credit_border.size() - std::string(u8"┐").size(),
                  std::string(u8"┐").size(), u8"┐") == 0 &&
              ainiux::editor::detail::display_width_for_range(
                  credit_border, 0, credit_border.size()) == 48,
          "agent frame shows formatted credits after permission at exact width");
    frame.index_enabled = true;
    frame.credit_label = "8.93 USD";
    const std::string index_border = agent_input_top_border(frame, 64);
    check(index_border.find(" smart index 8.93 USD ") != std::string::npos &&
              index_border.find("index") != std::string::npos &&
              ainiux::editor::detail::display_width_for_range(
                  index_border, 0, index_border.size()) == 64,
          "agent frame shows index marker between permission and credits");
    frame.index_enabled = false;
    const std::string no_index_border = agent_input_top_border(frame, 64);
    check(no_index_border.find(" index ") == std::string::npos &&
              no_index_border.find(" smart 8.93 USD ") != std::string::npos,
          "agent frame omits index marker when indexing is off");

    ainiux::editor::EditorState draft =
        ainiux::editor::EditorState::from_text(u8"ab界d\nsecond\nthird");
    check(draft.visual_row_count_bounded(4, 2) == 2 &&
              draft.visual_row_count_bounded(20, 10) == 3,
          "bounded editor measurement handles Unicode wrapping and explicit newlines");
}

void test_agent_inline_choices() {
    using namespace ainiux::tui;
    InlineChoiceModel approval = agent_inline_choices_for_mode(
        TuiMode::GuardApprovalConfirm);
    check(render_inline_choices(approval) == "(1) [Y]es  (2) [N]o",
          "agent Guard confirmation renders explicit Yes/No choices");
    check(parse_inline_choice_key(approval, '1').matched &&
              parse_inline_choice_key(approval, 'y').index == 0 &&
              parse_inline_choice_key(approval, 'N').index == 1 &&
              parse_inline_choice_key(approval, 27).index == 1 &&
              !parse_inline_choice_key(approval, 'x').matched,
          "agent Guard accepts y/n, numbers, and Esc as No");
    InlineChoiceModel permissions =
        agent_inline_choices_for_mode(TuiMode::AgentPermissionSelect);
    check(render_inline_choices(permissions) ==
              "(1) [C]onfirm  (2) [S]mart  (3) [Y]olo" &&
              parse_inline_choice_key(permissions, '3').index == 2,
          "agent permissions render the three canonical choices");
    InlineChoiceModel continuation = agent_inline_choices_for_mode(
        TuiMode::AgentContinueConfirm);
    check(render_inline_choices(continuation) == "(1) [C]ontinue  (2) [S]top" &&
              parse_inline_choice_key(continuation, 'c').index == 0 &&
              parse_inline_choice_key(continuation, 27).index == 1,
          "agent turn cap renders Continue/Stop with Stop as the safe default");
    InlineChoiceModel index_build = agent_inline_choices_for_mode(
        TuiMode::AgentIndexBuildConfirm);
    check(render_inline_choices(index_build) == "(1) [Y]es  (2) [N]o" &&
              parse_inline_choice_key(index_build, 'y').index == 0 &&
              parse_inline_choice_key(index_build, 'n').index == 1 &&
              parse_inline_choice_key(index_build, 27).index == 1,
          "agent missing-index offer renders Yes/No with No as the safe default");
    InlineChoiceModel four{{{"Alpha", 'a'}, {"Beta", 'b'}, {"Gamma", 'g'}, {"Delta", 'd'}}, 3};
    check(valid_inline_choices(four) && parse_inline_choice_key(four, '4').index == 3,
          "inline choice widget supports four choices");
    InlineChoiceModel duplicate{{{"Approve", 'a'}, {"Again", 'a'}}, 1};
    std::string reason;
    check(!valid_inline_choices(duplicate, &reason) &&
              reason.find("unique") != std::string::npos,
          "inline choice widget rejects duplicate mnemonics");
}

void test_tui_ready_and_generation_status() {
    check(ainiux::tui::ready_status().find("Tab complete") != std::string::npos &&
              ainiux::tui::ready_status().find("Ctrl+Space") != std::string::npos &&
              ainiux::tui::ready_status().find(u8"history Ctrl+B ↑ Ctrl+D ↓") !=
                  std::string::npos,
          "TUI ready status displays compact input and history hints");
    check(ainiux::editor::detail::display_width_for_range(
              ainiux::tui::ready_status(), 0, ainiux::tui::ready_status().size()) <= 90,
          "TUI ready status stays within a practical terminal width");

    ainiux::provider::ChatResult result;
    result.ttft_ms = 100;
    result.total_ms = 1100;
    result.completion_tokens = 20;
    result.completion_tokens_estimated = true;

    const std::string streaming =
        ainiux::tui::generation_ready_status("lm_studio", "gpt-test", result, true, {}, 0);
    check(streaming.find("[lmstudio/gpt-test]") == std::string::npos,
          "TUI streaming completion status leaves provider/model on the input label");
    check(streaming.find("TTFT: 100 ms") == 0,
          "TUI streaming completion status starts with time to first token");
    check(streaming.find("| ~20.0 token/s") != std::string::npos,
          "TUI streaming completion status estimates throughput after the first token");
    check(streaming.find(u8"history Ctrl+B ↑ Ctrl+D ↓") != std::string::npos,
          "TUI streaming completion status includes history navigation help");

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
    check(context_status.find("TTFT: 100 ms | ~20.0 token/s") != std::string::npos,
          "TUI ready status uses metrics without embedding context usage");
    check(context_status.find("context:") == std::string::npos,
          "TUI completion status omits context usage (shown on the input label)");
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

void test_tui_copy_last_chat_message_uses_raw_transcript_text() {
    ainiux::chat::Session session;
    ainiux::editor::Clipboard clipboard;
    std::string copied_role = "stale";
    check(!ainiux::tui::copy_last_chat_message(session, clipboard, copied_role) &&
              clipboard.empty() && copied_role.empty(),
          "TUI history copy rejects an empty session");

    session.messages.push_back({"user", "question"});
    session.messages.push_back(
        {"assistant", "first logical line\nsecond line with █ and ░ text"});
    session.messages.push_back({"notice", "display-only row"});
    check(ainiux::tui::copy_last_chat_message(session, clipboard, copied_role) &&
              copied_role == "assistant" &&
              clipboard.text() == "first logical line\nsecond line with █ and ░ text",
          "TUI history copy preserves raw message newlines and ignores display-only rows");

    session.messages.push_back({"user", ""});
    check(!ainiux::tui::copy_last_chat_message(session, clipboard, copied_role) &&
              copied_role.empty(),
          "TUI history copy rejects an empty last message without replacing the clipboard");
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
    auto same_rgb = [](const ainiux::tui::Rgb& left, const ainiux::tui::Rgb& right) {
        return left.r == right.r && left.g == right.g && left.b == right.b;
    };
    check(same_rgb(light_text.foreground, {0x00, 0x00, 0x00}) &&
              same_rgb(light_text.background, {0xFF, 0xFF, 0xFF}),
          "TUI light theme uses VS Code Light+ black text on white");
    struct ExpectedSyntaxColor {
        ainiux::tui::StyleRole role;
        ainiux::tui::Rgb color;
    };
    const std::vector<ExpectedSyntaxColor> light_plus_syntax = {
        {ainiux::tui::StyleRole::SyntaxComment, {0x00, 0x80, 0x00}},
        {ainiux::tui::StyleRole::SyntaxKeyword, {0x00, 0x00, 0xFF}},
        {ainiux::tui::StyleRole::SyntaxType, {0x26, 0x7F, 0x99}},
        {ainiux::tui::StyleRole::SyntaxString, {0xA3, 0x15, 0x15}},
        {ainiux::tui::StyleRole::SyntaxNumber, {0x09, 0x86, 0x58}},
        {ainiux::tui::StyleRole::SyntaxFunction, {0x79, 0x5E, 0x26}},
        {ainiux::tui::StyleRole::SyntaxVariable, {0x00, 0x10, 0x80}},
        {ainiux::tui::StyleRole::SyntaxOperator, {0x00, 0x00, 0x00}},
        {ainiux::tui::StyleRole::SyntaxTag, {0x80, 0x00, 0x00}},
        {ainiux::tui::StyleRole::SyntaxAttribute, {0xE5, 0x00, 0x00}},
        {ainiux::tui::StyleRole::SyntaxProperty, {0x04, 0x51, 0xA5}},
        {ainiux::tui::StyleRole::SyntaxHeading, {0x80, 0x00, 0x00}},
        {ainiux::tui::StyleRole::SyntaxEmphasis, {0x80, 0x00, 0x80}},
        {ainiux::tui::StyleRole::SyntaxLink, {0x04, 0x51, 0xA5}},
    };
    for (const ExpectedSyntaxColor& expected : light_plus_syntax) {
        const ainiux::tui::Rgb actual =
            ainiux::tui::style_pair_for(registry, "light", expected.role).foreground;
        check(same_rgb(actual, expected.color),
              "built-in light theme keeps the VS Code Light+ syntax palette");
    }

    ainiux::config::ParseResult parsed = ainiux::config::read_file("config/themes.conf");
    check(parsed.error.ok(), "themes.conf parses");
    ainiux::cli::Options options;
    ainiux::Error err = ainiux::config::apply_themes_document(parsed.document, options);
    check(err.ok(), "themes.conf applies");
    check(options.tui_themes.has("dark") && options.tui_themes.has("light") &&
              options.tui_themes.has("sepia"),
          "themes.conf defines built-in dark, light, and sepia themes");
    for (const ExpectedSyntaxColor& expected : light_plus_syntax) {
        const ainiux::tui::Rgb actual =
            ainiux::tui::style_pair_for(options.tui_themes, "light", expected.role).foreground;
        check(same_rgb(actual, expected.color),
              "configured light theme matches the built-in Light+ syntax palette");
    }
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
    check(switched.ok && switched.selected_theme == "light" && switched.colors_enabled,
          "theme command switches themes and enables colors");
    const ainiux::tui::ThemeCommandResult colors_off =
        ainiux::tui::handle_theme_command(options.tui_themes, "dark", "off", true);
    check(colors_off.ok && !colors_off.colors_enabled && colors_off.selected_theme == "dark",
          "theme off disables styling while retaining the selected palette");
    const ainiux::tui::ThemeCommandResult colors_back =
        ainiux::tui::handle_theme_command(options.tui_themes, "dark", "light", false);
    check(colors_back.ok && colors_back.colors_enabled && colors_back.selected_theme == "light",
          "selecting a named theme re-enables styling");

    const ainiux::tui::TextAttributes heading_attributes =
        ainiux::tui::text_attributes_for_token(ainiux::highlight::TokenRole::Heading);
    const ainiux::tui::TextAttributes emphasis_attributes =
        ainiux::tui::text_attributes_for_token(ainiux::highlight::TokenRole::Emphasis);
    const ainiux::tui::TextAttributes combined_attributes =
        ainiux::tui::text_attributes_for_token(
            ainiux::highlight::TokenRole::StrongEmphasis);
    const ainiux::tui::TextAttributes link_attributes =
        ainiux::tui::text_attributes_for_token(
            ainiux::highlight::TokenRole::LinkDestination);
    check(heading_attributes.bold && !heading_attributes.italic,
          "Markdown headings map to terminal bold");
    check(emphasis_attributes.italic && !emphasis_attributes.bold,
          "Markdown emphasis maps to terminal italic");
    check(combined_attributes.bold && combined_attributes.italic,
          "Markdown strong emphasis maps to terminal bold and italic");
    check(link_attributes.underline,
          "Markdown link destinations map to terminal underline");
    check(ainiux::tui::ansi_text_attributes_sequence(combined_attributes) ==
              std::string("\x1b[1m\x1b[3m"),
          "terminal text attributes emit standard bold and italic ANSI sequences");
}

void test_tui_buffer_list_uses_colored_panel_widget() {
    const std::string text =
        "Buffers - Enter opens - Tab/Insert new - DEL close - Esc cancels\n> file1.txt - Ln 1, Col 1\n  file2.txt - Ln 2, Col 3";
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
    session.messages.push_back(
        {"user",
         "# Heading\nPlain *emphasis*, **strong**, [link](https://example.test), and "
         "https://bare.example"});
    std::vector<ainiux::tui::StyledLine> lines =
        ainiux::tui::detail::history_lines_for_session(
            session, 100, false, ainiux::tui::ActivityKind::None, 0, true);
    bool saw_heading = false;
    bool saw_emphasis = false;
    bool saw_link = false;
    bool saw_link_url = false;
    bool saw_bold_heading = false;
    bool saw_bold_strong = false;
    bool saw_italic_emphasis = false;
    bool saw_underlined_link = false;
    bool saw_underlined_url = false;
    for (const ainiux::tui::StyledLine& line : lines) {
        for (const ainiux::tui::StyledSegment& segment : line.segments) {
            saw_heading = saw_heading || segment.role == ainiux::tui::StyleRole::SyntaxHeading;
            saw_emphasis = saw_emphasis || segment.role == ainiux::tui::StyleRole::SyntaxEmphasis;
            saw_link = saw_link || segment.role == ainiux::tui::StyleRole::SyntaxLink;
            saw_link_url = saw_link_url ||
                           segment.role == ainiux::tui::StyleRole::SyntaxAttribute;
            saw_bold_heading = saw_bold_heading ||
                               (segment.role == ainiux::tui::StyleRole::SyntaxHeading &&
                                segment.attributes.bold);
            saw_bold_strong = saw_bold_strong ||
                              (segment.text.find("**strong**") != std::string::npos &&
                               segment.attributes.bold);
            saw_italic_emphasis = saw_italic_emphasis ||
                                  (segment.text.find("*emphasis*") != std::string::npos &&
                                   segment.attributes.italic);
            saw_underlined_link = saw_underlined_link ||
                                  (segment.role == ainiux::tui::StyleRole::SyntaxLink &&
                                   segment.attributes.underline);
            saw_underlined_url = saw_underlined_url ||
                                 (segment.role == ainiux::tui::StyleRole::SyntaxAttribute &&
                                  segment.attributes.underline);
        }
    }
    check(saw_heading && saw_emphasis && saw_link && saw_link_url,
          "TUI chat history gives Markdown link text and URLs distinct semantic colors");
    check(saw_bold_heading && saw_bold_strong && saw_italic_emphasis &&
              saw_underlined_link && saw_underlined_url,
          "TUI chat history applies Markdown bold, italic, and underline attributes");

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
    session.messages.push_back(
        {"assistant", "> user prompt\nAnswer...\nwindow_width_max\n**Bold here:**"});
    lines = ainiux::tui::detail::history_lines_for_session(
        session, 100, false, ainiux::tui::ActivityKind::None, 0, true, true);
    std::vector<std::string> rendered_text;
    bool snake_case_emphasis = false;
    bool punctuation_bold = false;
    for (const ainiux::tui::StyledLine& line : lines) {
        std::string text;
        for (const ainiux::tui::StyledSegment& segment : line.segments) {
            text += segment.text;
            if (segment.text.find("window_width_max") != std::string::npos) {
                snake_case_emphasis =
                    snake_case_emphasis || segment.attributes.italic ||
                    segment.attributes.bold;
            }
            if (segment.text.find("**Bold here:**") != std::string::npos) {
                punctuation_bold = punctuation_bold || segment.attributes.bold;
            }
        }
        rendered_text.push_back(std::move(text));
    }
    check(rendered_text ==
              std::vector<std::string>{
                  "> user prompt", "Answer...", "window_width_max", "**Bold here:**"},
          "TUI Markdown styling preserves blockquote, answer, identifier, and bold text bytes");
    check(!snake_case_emphasis && punctuation_bold,
          "TUI Markdown keeps snake_case plain and bolds punctuation inside strong delimiters");

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
    ainiux::provider::Message user{"user", "fix the attempts"};
    user.created_at_ms = 1'000'000;
    ainiux::provider::Message tool{
        "tool", "1: read_file(\"game.py\") → ok in 150 ms"};
    tool.created_at_ms = 1'006'540;  // +6.54s
    ainiux::provider::Message assistant{"assistant", "Updated medium and hard to 8 attempts."};
    assistant.created_at_ms = 1'012'000;  // +12.00s
    session.messages.push_back(std::move(user));
    session.messages.push_back(std::move(tool));
    session.messages.push_back(std::move(assistant));

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
    bool saw_tool_elapsed = false;
    bool saw_answer_elapsed = false;
    bool saw_task_complete_alone = false;
    bool saw_full_wall_clock = false;
    for (const auto& line : agent_lines) {
        std::string joined;
        for (const auto& seg : line.segments) joined += seg.text;
        if (!line.segments.empty() && line.segments.front().text == "> ") saw_prompt_marker = true;
        if (joined.find("Assistant:") != std::string::npos) saw_assistant_label_agent = true;
        if (joined.find("Tool:") != std::string::npos) saw_tool_label = true;
        if (joined.find("1: read_file") != std::string::npos) saw_tool_body = true;
        if (joined.find("Updated medium") != std::string::npos) saw_answer = true;
        if (joined.find("in 150 ms") != std::string::npos) saw_tool_elapsed = true;
        if (joined.find("Task complete in 12.00 seconds.") != std::string::npos) {
            saw_answer_elapsed = true;
            // Completion banner must not share a line with answer body text.
            if (joined.find("Updated medium") == std::string::npos) saw_task_complete_alone = true;
        }
        // ISO-like stamps (e.g. 1970-…T…) only — not "Task complete…".
        if (joined.find("1970") != std::string::npos ||
            (joined.size() >= 11 && joined[4] == '-' && joined[7] == '-' &&
             joined.find('T') != std::string::npos && joined.find("Task") == std::string::npos))
            saw_full_wall_clock = true;
    }
    check(saw_prompt_marker, "agent mode shows user prompts as \"> \"");
    check(!saw_assistant_label_agent, "agent mode omits Assistant: label");
    check(!saw_tool_label, "agent mode omits Tool: label");
    check(saw_tool_body && saw_answer, "agent mode still shows tool lines and answers flush-left");
    check(saw_tool_elapsed && saw_answer_elapsed,
          "agent mode shows tool ms timing and Task complete for the final answer");
    check(saw_task_complete_alone,
          "Task complete banner is on its own line, not merged with answer text");
    check(!saw_full_wall_clock, "agent mode does not show full wall-clock timestamps by default");

    session.provider = "custom_openai_chat";
    session.model = "qwen3.6-35b-a3b-mtp";
    session.messages.back().content.clear();
    agent_lines = ainiux::tui::detail::history_lines_for_session(
        session, 100, false, ainiux::tui::ActivityKind::Streaming, 0, false, true);
    bool saw_history_animation = false;
    bool repeated_model = false;
    for (const auto& line : agent_lines) {
        for (const auto& segment : line.segments) {
            saw_history_animation =
                saw_history_animation ||
                segment.role == ainiux::tui::StyleRole::StreamingActivity;
            repeated_model =
                repeated_model || segment.text.find("qwen3.6-35b-a3b-mtp") != std::string::npos;
        }
    }
    check(saw_history_animation, "agent mode shows streaming animation in history");
    check(!repeated_model, "agent history animation does not repeat the model from agent chrome");
}

void test_agent_shell_notice_preserves_listing_newlines() {
    // Chat/agent history does not apply prose reflow. Shell notices must keep
    // physical newlines so `ls -la` stays readable.
    ainiux::chat::Session session;
    const std::string listing =
        "$ ls -laFg\n"
        "exit=0  3ms  cwd=/home/eye/escape_prison\n"
        "total 124\n"
        "drwxrwxr-x 4 eye 4096 Jul 31 17:40 ./\n"
        "drwxr-x--- 48 eye 4096 Jul 31 15:54 ../\n"
        "-rwxrwxr-x 1 eye 2122 Jul 31 17:30 server.sh*\n";
    session.messages.push_back({"notice", listing});

    const std::vector<ainiux::tui::StyledLine> lines =
        ainiux::tui::detail::history_lines_for_session(
            session, 80, false, ainiux::tui::ActivityKind::None, 0, true, true);

    std::string joined;
    for (const auto& line : lines) {
        for (const auto& segment : line.segments) joined += segment.text;
        joined.push_back('\n');
    }
    check(joined.find("total 124") != std::string::npos &&
              joined.find("server.sh") != std::string::npos,
          "shell notice still contains listing rows");
    check(joined.find("total 124 drwxrwxr-x") == std::string::npos &&
              joined.find("total 124\n") != std::string::npos,
          "shell notice keeps listing newlines instead of reflowing into one paragraph");
    check(joined.find("exit=0  3ms  cwd=") != std::string::npos,
          "shell notice metadata line is preserved");
}

void test_tui_input_label_and_activity_indicators() {
    const std::string label = ainiux::tui::input_label_text();
    check(label == ainiux::tui::input_label_text_for_mode(false),
          "TUI input label concatenates app version branding with chat chrome");
    check(label.find(ainiux::versionNumber) != std::string::npos, "TUI input label includes the current version");
    check(label.find(" Chat ") != std::string::npos &&
              label.find("[choose model /model]") != std::string::npos,
          "TUI input label prompts for model selection when none is chosen");
    check(label.find("Ctrl+H help") == std::string::npos &&
              label.find("history Ctrl+B") == std::string::npos,
          "TUI input label leaves help and history navigation to the status row");

    ainiux::tui::AgentChrome chrome;
    chrome.model = "deepseek-v4-flash";
    chrome.reasoning = "max";
    chrome.used_tokens = 187982;
    chrome.window_tokens = 262144;
    chrome.credit_label = "$1.23";
    const std::string rich = ainiux::tui::chat_input_label_text(chrome);
    check(rich.find("[deepseek-v4-flash max]") != std::string::npos,
          "chat input label shows model and reasoning without provider");
    check(rich.find("187982 tok (71.7%)") != std::string::npos,
          "chat input label shows token usage with one-decimal percent");
    check(rich.find("$1.23") != std::string::npos, "chat input label appends credit balance");
    check(ainiux::tui::chat_model_reasoning_bracket("", "high") == "[choose model /model]",
          "empty model uses the choose-model bracket text");

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
    check(ainiux::tui::show_activity_on_status_row(
              ainiux::tui::ActivityKind::Streaming, false),
          "chat mode keeps streaming activity on the status row");
    check(!ainiux::tui::show_activity_on_status_row(
              ainiux::tui::ActivityKind::Streaming, true),
          "agent mode keeps streaming activity out of the status row");
    check(!ainiux::tui::show_activity_on_status_row(
              ainiux::tui::ActivityKind::None, false),
          "idle mode does not render status-row activity");

    ainiux::chat::Session session;
    session.provider = "custom_openai_chat";
    session.model = "qwen3.6-35b-a3b-mtp";
    session.messages.push_back({"user", "hello"});
    session.messages.push_back({"assistant", "<think>hidden</think>"});
    check(ainiux::tui::activity_kind_for_pending_assistant(session, 1, false) ==
              ainiux::tui::ActivityKind::Thinking,
          "TUI activity helper reports thinking for hidden trace-only output");
    std::vector<ainiux::tui::StyledLine> chat_history =
        ainiux::tui::detail::history_lines_for_session(
            session, 100, false, ainiux::tui::ActivityKind::Thinking, 0, false, false);
    bool chat_history_has_activity = false;
    bool chat_history_repeats_model = false;
    for (const auto& line : chat_history) {
        for (const auto& segment : line.segments) {
            chat_history_has_activity =
                chat_history_has_activity ||
                segment.role == ainiux::tui::StyleRole::ThinkingActivity ||
                segment.role == ainiux::tui::StyleRole::StreamingActivity;
            chat_history_repeats_model =
                chat_history_repeats_model ||
                segment.text.find("qwen3.6-35b-a3b-mtp") != std::string::npos;
        }
    }
    check(!chat_history_has_activity,
          "chat history omits the thinking animation owned by the status row");
    check(!chat_history_repeats_model,
          "chat history does not repeat the model from the activity status row");
    session.messages.back().content = "Visible answer";
    check(ainiux::tui::activity_kind_for_pending_assistant(session, 1, false) ==
              ainiux::tui::ActivityKind::Streaming,
          "TUI activity helper reports streaming once visible answer text arrives");
    chat_history = ainiux::tui::detail::history_lines_for_session(
        session, 100, false, ainiux::tui::ActivityKind::Streaming, 0, false, false);
    chat_history_has_activity = false;
    bool chat_history_has_answer = false;
    for (const auto& line : chat_history) {
        for (const auto& segment : line.segments) {
            chat_history_has_activity =
                chat_history_has_activity ||
                segment.role == ainiux::tui::StyleRole::StreamingActivity;
            chat_history_has_answer =
                chat_history_has_answer ||
                segment.text.find("Visible answer") != std::string::npos;
        }
    }
    check(!chat_history_has_activity && chat_history_has_answer,
          "streaming chat history shows answer text without a second animation");

    const auto thinking_segments = ainiux::tui::activity_status_segments(
        "[custom/Qwen3.6-35B]", ainiux::tui::ActivityKind::Thinking, 0, "thinking...");
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
              u8"[gemini/gemini-3.1-flash-lite-pre…]",
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
    check(ready_status.find("[lmstudio/qwen-local]") == std::string::npos,
          "TUI startup status leaves provider/model on the input label");
    check(ready_status.find("ready") != std::string::npos,
          "TUI startup status reports ready when model is set");
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

    const std::string empty_list = ainiux::tui::thread_picker_text({}, 0);
    check(empty_list.find("Newest first") != std::string::npos &&
              empty_list.find("Tab/Insert new") != std::string::npos,
          "TUI thread picker renders the header when there are no saved threads");

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

void test_tui_mouse_history_scroll() {
    const ainiux::tui::Layout layout = ainiux::tui::layout_for_terminal(24, 80);
    int history_scroll = 0;
    ainiux::editor::MouseInputEvent mouse;
    mouse.button = ainiux::editor::MouseButton::WheelUp;
    mouse.row = layout.history_row;
    mouse.col = 1;
    check(ainiux::tui::apply_chat_mouse_scroll(
              mouse, layout, ainiux::tui::TuiMode::Chat, history_scroll) &&
              history_scroll == 1,
          "chat wheel up moves exactly one rendered history row toward older content");
    mouse.button = ainiux::editor::MouseButton::WheelDown;
    check(ainiux::tui::apply_chat_mouse_scroll(
              mouse, layout, ainiux::tui::TuiMode::Chat, history_scroll) &&
              history_scroll == 0,
          "chat wheel down moves exactly one row toward the live bottom");
    check(ainiux::tui::apply_chat_mouse_scroll(
              mouse, layout, ainiux::tui::TuiMode::Chat, history_scroll) &&
              history_scroll == 0,
          "chat wheel down clamps at the live bottom");

    mouse.button = ainiux::editor::MouseButton::WheelUp;
    mouse.row = layout.input_rect.row;
    check(!ainiux::tui::apply_chat_mouse_scroll(
              mouse, layout, ainiux::tui::TuiMode::Chat, history_scroll) &&
              history_scroll == 0,
          "chat ignores wheel input in the input panel");
    mouse.row = layout.history_row;
    check(!ainiux::tui::apply_chat_mouse_scroll(
              mouse, layout, ainiux::tui::TuiMode::ThreadList, history_scroll),
          "chat ignores wheel input while a picker occupies the history band");
    mouse.button = ainiux::editor::MouseButton::Left;
    check(!ainiux::tui::apply_chat_mouse_scroll(
              mouse, layout, ainiux::tui::TuiMode::Chat, history_scroll),
          "chat consumes but ignores mouse clicks");
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

void test_tui_thread_picker_accepts_delayed_arrow_sequence() {
    ainiux::editor::clear_terminal_input_queue();

    int input_pipe[2] = {-1, -1};
    if (pipe(input_pipe) != 0) {
        check(false, "thread picker delayed-arrow test creates an input pipe");
        return;
    }
    const int saved_stdin = dup(STDIN_FILENO);
    if (saved_stdin < 0 || dup2(input_pipe[0], STDIN_FILENO) < 0) {
        check(false, "thread picker delayed-arrow test redirects stdin");
        close(input_pipe[0]);
        close(input_pipe[1]);
        if (saved_stdin >= 0) close(saved_stdin);
        return;
    }
    close(input_pipe[0]);

    const unsigned char escape = 27;
    const bool wrote_escape = write(input_pipe[1], &escape, 1) == 1;
    std::thread suffix_writer([write_fd = input_pipe[1]] {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        static constexpr char kArrowDown[] = "[B";
        (void)write(write_fd, kArrowDown, sizeof(kArrowDown) - 1);
        close(write_fd);
    });

    ainiux::editor::TerminalInputEvent event;
    const bool read_escape = ainiux::editor::read_terminal_input(event, 0);
    std::vector<ainiux::chat::ThreadSummary> threads(2);
    size_t selected = 0;
    size_t pending_delete = static_cast<size_t>(-1);
    std::string status;
    ainiux::tui::TuiMode mode = ainiux::tui::TuiMode::ThreadList;
    const ainiux::tui::PickerEscapeResult picker_result =
        read_escape
            ? ainiux::tui::handle_thread_list_escape(
                  threads, selected, status, pending_delete, mode)
            : ainiux::tui::PickerEscapeResult::Cancelled;

    suffix_writer.join();
    const bool restored_stdin = dup2(saved_stdin, STDIN_FILENO) >= 0;
    close(saved_stdin);
    ainiux::editor::clear_terminal_input_queue();

    check(wrote_escape && restored_stdin,
          "thread picker delayed-arrow test writes input and restores stdin");
    check(read_escape &&
              event.type == ainiux::editor::TerminalInputType::Byte &&
              event.byte == 27,
          "terminal input keeps a delayed arrow sequence attached to ESC");
    check(picker_result == ainiux::tui::PickerEscapeResult::Navigated &&
              mode == ainiux::tui::TuiMode::ThreadList && selected == 1,
          "thread picker moves down instead of cancelling on a delayed arrow sequence");
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
    ainiux::provider::ChatResult result;
    result.total_ms = 42;
    const std::string status = ainiux::tui::generation_ready_status(
        "https://example.test/v1", u8"模型-مرحبا", result, false, {}, 0);
    check(status.find("Response: 42 ms") != std::string::npos &&
              status.find("[custom/") == std::string::npos,
          "TUI generation status is metrics-only and never dumps custom URL profiles");
    check(ainiux::ui::provider_model_display_label("custom_openai_chat", "qwen") ==
              "[custom/qwen]",
          "custom OpenAI-compatible URL providers display as short custom, not registry ids");

    ainiux::tui::ThinkingDisplay hidden =
        ainiux::tui::thinking_display_text("", false);
    check(hidden.text.empty() && !hidden.saw_thinking_tag,
          "TUI thinking display handles empty assistant text");
}

void test_agent_project_slash_command_parsing() {
    const auto bare_new = ainiux::tui::parse_agent_slash_command("/new");
    check(bare_new.action == ainiux::tui::AgentSlashAction::NewProject &&
              bare_new.argument.empty(),
          "agent /new parses without a path");
    const auto spaced =
        ainiux::tui::parse_agent_slash_command("/new   project path with spaces  ");
    check(spaced.action == ainiux::tui::AgentSlashAction::NewProject &&
              spaced.argument == "project path with spaces",
          "agent /new keeps the trimmed remainder as one path");
    const auto compact = ainiux::tui::parse_agent_slash_command("/compact");
    check(compact.action == ainiux::tui::AgentSlashAction::Compact,
          "agent /compact parses");
    const auto summary =
        ainiux::tui::parse_agent_slash_command("/compact summary");
    check(summary.action == ainiux::tui::AgentSlashAction::Compact &&
              summary.argument == "summary",
          "agent /compact accepts a one-shot strategy override");
    const auto invalid =
        ainiux::tui::parse_agent_slash_command("/compact now");
    check(invalid.action == ainiux::tui::AgentSlashAction::Invalid &&
              invalid.error == "Usage: /compact [fast|smart|summary]",
          "agent /compact rejects arguments");
    check(ainiux::tui::parse_agent_slash_command("/show-index").action ==
              ainiux::tui::AgentSlashAction::ShowIndex,
          "agent /show-index parses");
    const auto invalid_show_index =
        ainiux::tui::parse_agent_slash_command("/show-index now");
    check(invalid_show_index.action ==
                  ainiux::tui::AgentSlashAction::Invalid &&
              invalid_show_index.error == "Usage: /show-index",
          "agent /show-index rejects arguments");
    check(ainiux::tui::parse_agent_slash_command("/index-code").action ==
              ainiux::tui::AgentSlashAction::IndexCode,
          "agent /index-code parses");
    const auto invalid_index_code =
        ainiux::tui::parse_agent_slash_command("/index-code now");
    check(invalid_index_code.action ==
                  ainiux::tui::AgentSlashAction::Invalid &&
              invalid_index_code.error == "Usage: /index-code",
          "agent /index-code rejects arguments");
    check(ainiux::tui::parse_agent_slash_command("/plan").action ==
              ainiux::tui::AgentSlashAction::Plan,
          "agent /plan parses as a task-mode switch");
    check(ainiux::tui::parse_agent_slash_command("/act").action ==
              ainiux::tui::AgentSlashAction::Act,
          "agent /act parses as a task-mode switch");
    check(ainiux::tui::parse_agent_slash_command("/plan later").action ==
              ainiux::tui::AgentSlashAction::None,
          "agent task-mode commands require an exact match");
    const auto permissions =
        ainiux::tui::parse_agent_slash_command("/permissions yolo");
    check(permissions.action == ainiux::tui::AgentSlashAction::Permissions &&
              permissions.argument == "yolo",
          "agent direct permission command parses");
    const auto invalid_permissions =
        ainiux::tui::parse_agent_slash_command("/permissions unsafe");
    check(invalid_permissions.action == ainiux::tui::AgentSlashAction::Invalid &&
              invalid_permissions.error ==
                  "Usage: /permissions [confirm|smart|yolo]",
          "agent permission command rejects unknown modes");
    check(ainiux::tui::parse_agent_slash_command("/goal").action ==
              ainiux::tui::AgentSlashAction::GoalStatus,
          "agent /goal with no args is status");
    const auto goal_set =
        ainiux::tui::parse_agent_slash_command("/goal create file X containing Y");
    check(goal_set.action == ainiux::tui::AgentSlashAction::GoalSet &&
              goal_set.argument == "create file X containing Y",
          "agent /goal <condition> sets the goal");
    check(ainiux::tui::parse_agent_slash_command("/goal clear").action ==
              ainiux::tui::AgentSlashAction::GoalClear,
          "agent /goal clear parses");
    check(ainiux::tui::parse_agent_slash_command("/goal stop").action ==
              ainiux::tui::AgentSlashAction::GoalClear,
          "agent /goal stop aliases clear");
    check(ainiux::tui::parse_agent_slash_command("/goal pause").action ==
              ainiux::tui::AgentSlashAction::GoalPause,
          "agent /goal pause parses");
    check(ainiux::tui::parse_agent_slash_command("/goal resume").action ==
              ainiux::tui::AgentSlashAction::GoalResume,
          "agent /goal resume parses");
    check(ainiux::tui::parse_agent_slash_command("/goal clear now").action ==
              ainiux::tui::AgentSlashAction::Invalid,
          "agent /goal clear rejects extra args");
}

void test_agent_project_history_handoff_clears_successful_empty_project() {
    ainiux::chat::Session session;
    session.messages.push_back({"user", "old project question"});
    session.messages.push_back({"assistant", "old project answer"});
    std::vector<ainiux::provider::Message> previous = std::move(session.messages);
    session.messages.clear();

    ainiux::tui::apply_agent_project_history_handoff(
        session, previous, {}, true);
    check(session.messages.empty() && previous.empty(),
          "successful /new with empty history clears the old visible transcript");

    previous.push_back({"user", "restore on failure"});
    ainiux::tui::apply_agent_project_history_handoff(
        session, previous, {}, false);
    check(session.messages.size() == 1 &&
              session.messages[0].content == "restore on failure" &&
              previous.empty(),
          "failed /new restores the prior visible transcript");
}

void test_agent_progress_replaces_rows_in_place() {
    chat::Session session;
    std::vector<ainiux::tui::AgentLiveRow> rows;
    ainiux::tui::apply_agent_progress_update(
        session, rows,
        {agent::AgentProgressAction::Upsert, agent::AgentProgressKind::Thinking,
         1, 0, "Thinking: first", 0});
    ainiux::tui::apply_agent_progress_update(
        session, rows,
        {agent::AgentProgressAction::Upsert, agent::AgentProgressKind::Thinking,
         1, 0, "Thinking: updated", 0});
    check(session.messages.size() == 1 &&
              session.messages[0].content == "Thinking: updated",
          "Thinking deltas reuse one live history row");

    ainiux::tui::apply_agent_progress_update(
        session, rows,
        {agent::AgentProgressAction::Upsert, agent::AgentProgressKind::Tool,
         1, 1, "1: read_file(\"a\") …", 0});
    ainiux::tui::apply_agent_progress_update(
        session, rows,
        {agent::AgentProgressAction::Upsert, agent::AgentProgressKind::Tool,
         1, 2, "2: glob(\"*.cpp\") …", 0});
    ainiux::tui::apply_agent_progress_update(
        session, rows,
        {agent::AgentProgressAction::Commit, agent::AgentProgressKind::Tool,
         1, 1, "1: read_file(\"a\") → ok in 0 ms", 10});
    check(session.messages.size() == 3 &&
              session.messages[1].content.find("→ ok") != std::string::npos &&
              session.messages[2].content.find("glob") != std::string::npos,
          "tool completion replaces its running row without reordering tools");

    ainiux::tui::apply_agent_progress_update(
        session, rows,
        {agent::AgentProgressAction::Upsert, agent::AgentProgressKind::Notice,
         1, 99, "Waiting for provider · retry 1 in 1s", 0});
    check(session.messages.back().role == "notice" &&
              app::provider_chat_messages(session.messages).empty(),
          "live retry notice is display-only and excluded from provider context");
    ainiux::tui::apply_agent_progress_update(
        session, rows,
        {agent::AgentProgressAction::Discard, agent::AgentProgressKind::Notice,
         1, 99, {}, 0});

    ainiux::tui::apply_agent_progress_update(
        session, rows,
        {agent::AgentProgressAction::Discard, agent::AgentProgressKind::Thinking,
         1, 0, {}, 0});
    check(session.messages.size() == 2 && session.messages[0].role == "tool",
          "failed provisional Thinking row is discarded and row indexes stay valid");
}

}  // namespace

void run_all() {
    test_agent_progress_replaces_rows_in_place();
    test_terminal_frame_renderer_updates_only_changed_rows();
    test_shared_tui_render_skips_identical_frame();
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
    test_tui_agent_chrome_formatters();
    test_tui_background_metadata_refresh_policy();
    test_agent_widgets_and_dynamic_geometry();
    test_agent_inline_choices();
    test_tui_ctrl_chat_history_scroll_shortcuts();
    test_tui_mouse_history_scroll();
    test_tui_chat_history_scroll_keys();
    test_tui_read_terminal_input_marks_alt_meta_prefix();
    test_tui_thread_picker_accepts_delayed_arrow_sequence();
    test_tui_handle_escape_alt_pageup_scrolls_history();
    test_tui_handle_escape_plain_pageup_moves_input();
    test_tui_last_unanswered_user_message_requires_final_user();
    test_tui_last_editable_chat_message_finds_last_user_or_assistant();
    test_tui_copy_last_chat_message_uses_raw_transcript_text();
    test_tui_pop_last_chat_message_removes_user_or_assistant_only();
    test_tui_regeneration_plan_uses_last_user_turn();
    test_tui_theme_parsing_and_contrast();
    test_tui_buffer_list_uses_colored_panel_widget();
    test_tui_thinking_trace_display();
    test_tui_markdown_history_highlighting();
    test_tui_agent_history_chrome();
    test_agent_shell_notice_preserves_listing_newlines();
    test_agent_project_slash_command_parsing();
    test_agent_project_history_handoff_clears_successful_empty_project();
}

}  // namespace ainiux::test::tui
