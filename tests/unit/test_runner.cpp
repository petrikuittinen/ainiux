#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "chat/session.hpp"
#include "cli/args.hpp"
#include "editor/editor.hpp"
#include "json/json.hpp"
#include "provider/provider.hpp"
#include "runtime/runtime.hpp"
#include "tui/tui.hpp"

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void test_cli_parse() {
    const char* argv[] = {"pkchat", "http://localhost:8000", "-p", "hello", "--no-stream", "--format", "json", "-v", "--save-chat", "chat.json"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(10, const_cast<char**>(argv));
    check(parsed.error.ok(), "CLI parse should succeed");
    check(parsed.options.positional_url == "http://localhost:8000", "positional URL parsed");
    check(parsed.options.prompt == "hello", "prompt parsed");
    check(!parsed.options.stream, "no-stream parsed");
    check(parsed.options.format == pkchat::cli::OutputFormat::Json, "json format parsed");
    check(parsed.options.verbose, "verbose parsed");
    check(parsed.options.save_chat_path == "chat.json", "save chat parsed");
}

void test_cli_provider_shortcut_parse() {
    const char* argv[] = {"pkchat", "openrouter", "-model", "provider/model", "-i"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(5, const_cast<char**>(argv));
    check(parsed.error.ok(), "provider shortcut args parse");
    check(parsed.options.positional_url == "openrouter", "provider shortcut stored as positional");
    check(parsed.options.model == "provider/model", "-model alias parsed");
    check(parsed.options.repl, "-i parsed for provider shortcut");
}

void test_cli_repl_parse() {
    const char* argv[] = {"pkchat", "--repl", "--load-chat", "chat.json"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(4, const_cast<char**>(argv));
    check(parsed.error.ok(), "REPL args parse");
    check(parsed.options.repl, "REPL flag parsed");
    check(parsed.options.load_chat_path == "chat.json", "load chat parsed");
}

void test_cli_tui_parse() {
    const char* argv[] = {"pkchat", "--tui", "lmstudio"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(3, const_cast<char**>(argv));
    check(parsed.error.ok(), "TUI args parse");
    check(parsed.options.tui, "TUI flag parsed");
    check(parsed.options.positional_url == "lmstudio", "TUI positional profile parsed");
}

void test_cli_editor_parse() {
    const char* argv[] = {"pkchat", "--editor", "notes.txt", "--output", "saved.txt"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(5, const_cast<char**>(argv));
    check(parsed.error.ok(), "editor args parse");
    check(parsed.options.editor, "editor flag parsed");
    check(parsed.options.positional_url == "notes.txt", "editor positional file parsed");
    check(parsed.options.output_path == "saved.txt", "editor save-as output parsed");
}

void test_editor_piece_table_edits() {
    pkchat::editor::PieceTable table = pkchat::editor::PieceTable::from_string("alpha\nbeta\ngamma");
    check(table.size() == 16, "piece table initial size");
    check(table.line_count() == 3, "piece table initial line count");
    check(table.line_text(1) == "beta", "piece table line text");

    pkchat::Error err = table.insert(6, "wide\n");
    check(err.ok(), "piece table insert succeeds");
    check(table.str() == "alpha\nwide\nbeta\ngamma", "piece table insert preserves text");
    check(table.line_count() == 4, "piece table insert updates line count");

    err = table.erase(6, 5);
    check(err.ok(), "piece table erase succeeds");
    check(table.str() == "alpha\nbeta\ngamma", "piece table erase restores text");

    err = table.insert(table.size(), "\nlast");
    check(err.ok(), "piece table append succeeds");
    check(table.line_text(3) == "last", "piece table append line text");
}

void test_editor_rectangular_rendering() {
    pkchat::editor::EditorState state = pkchat::editor::EditorState::from_text("one\ntwo\nthree");
    pkchat::editor::Rect rect{4, 10, 2, 4};
    state.cursor = state.text.offset_for_line_column(1, 1);
    pkchat::editor::RenderedPanel rendered = state.render(rect);
    check(rendered.lines.size() == 2, "editor panel respects height");
    check(rendered.lines[0] == "one ", "editor panel pads first visible line");
    check(rendered.lines[1] == "two ", "editor panel pads second visible line");
    check(rendered.cursor.visible, "editor cursor visible in panel");
    check(rendered.cursor.row == 1 && rendered.cursor.col == 1, "editor cursor maps to panel coordinates");

    state.cursor = state.text.offset_for_line_column(2, 3);
    state.ensure_cursor_visible(rect);
    rendered = state.render(rect);
    check(state.scroll_line == 1, "editor vertical scroll follows cursor");
    check(rendered.lines[0] == "two ", "editor scrolled first line");
    check(rendered.lines[1] == "thre", "editor clips to panel width");
    check(rendered.cursor.visible && rendered.cursor.row == 1 && rendered.cursor.col == 3,
          "editor cursor remains visible after scroll");
}

void test_editor_word_wrap_rendering() {
    pkchat::editor::EditorState state = pkchat::editor::EditorState::from_text("abcdefghij");
    pkchat::editor::Rect rect{1, 1, 3, 4};
    pkchat::editor::RenderedPanel rendered = state.render(rect);
    check(rendered.lines.size() == 3, "editor wrapped panel respects height");
    check(rendered.lines[0] == "abcd", "editor hard-wraps long words first row");
    check(rendered.lines[1] == "efgh", "editor hard-wraps long words second row");
    check(rendered.lines[2] == "ij  ", "editor pads final wrapped row");

    state.cursor = state.text.offset_for_line_column(0, 8);
    state.ensure_cursor_visible({1, 1, 2, 4});
    rendered = state.render({1, 1, 2, 4});
    check(state.scroll_line == 1, "editor wrapped scroll follows cursor row");
    check(rendered.lines[0] == "efgh", "editor render starts at wrapped scroll row");
    check(rendered.lines[1] == "ij  ", "editor render includes next wrapped row");
    check(rendered.cursor.visible && rendered.cursor.row == 1 && rendered.cursor.col == 0,
          "editor cursor maps inside wrapped line");
}

void test_editor_word_wrap_breaks_on_spaces() {
    pkchat::editor::EditorState state = pkchat::editor::EditorState::from_text("alpha beta");
    pkchat::editor::RenderedPanel rendered = state.render({1, 1, 2, 8});
    check(rendered.lines[0] == "alpha   ", "editor wraps at a word break when available");
    check(rendered.lines[1] == "beta    ", "editor continues after the wrapped word break");
}

void test_editor_vertical_navigation_modes() {
    pkchat::editor::Rect rect{1, 1, 3, 4};
    pkchat::editor::EditorState logical = pkchat::editor::EditorState::from_text("abcdefghij\nXYZ");
    logical.cursor = logical.text.offset_for_line_column(0, 2);
    logical.preferred_column = 2;
    logical.move_down(rect);
    check(logical.cursor == logical.text.offset_for_line_column(1, 2),
          "editor default vertical movement uses logical lines");

    pkchat::editor::EditorState visual = pkchat::editor::EditorState::from_text("abcdefghij\nXYZ");
    visual.vertical_movement = pkchat::editor::VerticalMovementMode::VisualRow;
    visual.cursor = visual.text.offset_for_line_column(0, 2);
    visual.preferred_column = 2;
    visual.move_down(rect);
    check(visual.cursor == visual.text.line_start(0) + 6,
          "editor visual movement moves to wrapped row below within the same line");
    visual.move_down(rect);
    check(visual.cursor == visual.text.line_start(0) + 10,
          "editor visual movement moves to final short wrapped row");
    visual.move_down(rect);
    check(visual.cursor == visual.text.offset_for_line_column(1, 2),
          "editor visual movement crosses to next hard line after wrapped rows");
    visual.move_up(rect);
    check(visual.cursor == visual.text.line_start(0) + 10,
          "editor visual movement moves back up into previous line wrap overflow");
}

void test_editor_file_round_trip() {
    const std::string path = "build/unit-editor.txt";
    pkchat::editor::PieceTable table = pkchat::editor::PieceTable::from_string("first\nsecond");
    pkchat::Error err = pkchat::editor::save_file(path, table);
    check(err.ok(), "editor file saves");
    pkchat::editor::PieceTable loaded;
    err = pkchat::editor::load_file(path, loaded);
    check(err.ok(), "editor file loads");
    check(loaded.str() == "first\nsecond", "editor file round trip preserves text");
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

void test_chat_session_json_round_trip() {
    pkchat::provider::RequestContext context;
    context.profile.name = "custom_openai_chat";
    context.base_url = "http://localhost:8000/v1";
    context.options.model = "mock-model";
    context.options.stream = false;
    pkchat::chat::Session session = pkchat::chat::new_session(context);
    session.created_at = "2026-06-14T00:00:00Z";
    session.updated_at = session.created_at;
    session.messages.push_back({"user", "hello"});
    session.messages.push_back({"assistant", "Hello"});

    const std::string encoded = pkchat::chat::session_to_json(session);
    pkchat::json::ParseResult parsed = pkchat::json::parse(encoded);
    check(parsed.error.ok(), "chat session JSON parses");
    const pkchat::json::Value* messages = parsed.value.get("messages");
    check(messages != nullptr && messages->is_array() && messages->array.size() == 2, "chat messages persisted");

    const std::string path = "build/unit-chat.json";
    pkchat::Error err = pkchat::chat::save_session_atomic(path, session);
    check(err.ok(), "chat session saves atomically");
    pkchat::chat::Session loaded;
    err = pkchat::chat::load_session(path, loaded);
    check(err.ok(), "chat session loads");
    check(loaded.messages.size() == 2, "loaded chat has messages");
    check(!loaded.messages.empty() && loaded.messages[0].content == "hello", "loaded user message preserved");
}

void test_chat_session_rejects_corrupt_json() {
    const std::string path = "build/corrupt-chat.json";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "{bad json";
    out.close();
    pkchat::chat::Session session;
    pkchat::Error err = pkchat::chat::load_session(path, session);
    check(!err.ok(), "corrupt chat file rejected");
    check(err.code == pkchat::ErrorCode::JsonParse, "corrupt chat file reports JSON parse error");
}

void test_runtime_event_queue_and_job_cancel() {
    pkchat::runtime::EventQueue<int> queue;
    int value = 0;
    check(!queue.try_pop(value), "empty runtime queue has no event");
    queue.push(7);
    check(queue.try_pop(value) && value == 7, "runtime queue preserves event value");

    pkchat::runtime::JobHandle job;
    std::atomic<bool> entered{false};
    job.start([&](pkchat::runtime::CancellationToken token) {
        entered.store(true, std::memory_order_release);
        while (!token.cancelled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        queue.push(42);
    });
    for (int i = 0; i < 100 && !entered.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(job.running(), "runtime job reports running");
    job.cancel();
    check(queue.wait_pop_for(value, std::chrono::milliseconds(1000)) && value == 42, "runtime job observes cancellation");
    job.join();
    check(!job.running(), "runtime job reports stopped after join");
}

void test_openrouter_shortcut_context() {
    const char* argv[] = {"pkchat", "openrouter", "-model", "provider/model", "-p", "hello", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(8, const_cast<char**>(argv));
    check(parsed.error.ok(), "openrouter shortcut args parse");
    pkchat::provider::ContextResult ctx = pkchat::provider::build_context(parsed.options);
    check(ctx.error.ok(), "openrouter shortcut context builds with auth header");
    check(ctx.context.profile.name == "openrouter", "openrouter shortcut selects profile");
    check(ctx.context.base_url == "https://openrouter.ai/api/v1", "openrouter shortcut uses standard base URL");
}
void test_openai_context_allows_missing_model() {
    const char* argv[] = {"pkchat", "--provider", "openai", "-p", "hello", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(7, const_cast<char**>(argv));
    check(parsed.error.ok(), "openai args without model parse");
    pkchat::provider::ContextResult ctx = pkchat::provider::build_context(parsed.options);
    check(ctx.error.ok(), "openai context builds without model so caller can discover one");
    check(ctx.context.options.model.empty(), "openai context keeps missing model empty before discovery");
}

void test_lmstudio_shortcut_context() {
    const char* argv[] = {"pkchat", "lmstudio", "-i"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(3, const_cast<char**>(argv));
    check(parsed.error.ok(), "lmstudio shortcut args parse");
    pkchat::provider::ContextResult ctx = pkchat::provider::build_context(parsed.options);
    check(ctx.error.ok(), "lmstudio shortcut context builds without key or model");
    check(ctx.context.profile.name == "lm_studio", "lmstudio shortcut selects profile");
    check(ctx.context.base_url == "http://localhost:1234/v1", "lmstudio shortcut uses default base URL");
    check(ctx.context.options.model.empty(), "lmstudio shortcut does not require model");
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
    test_cli_provider_shortcut_parse();
    test_cli_repl_parse();
    test_cli_tui_parse();
    test_cli_editor_parse();
    test_url_normalization();
    test_json_parse();
    test_lmstudio_context();
    test_openrouter_shortcut_context();
    test_openai_context_allows_missing_model();
    test_lmstudio_shortcut_context();
    test_chat_session_json_round_trip();
    test_chat_session_rejects_corrupt_json();
    test_runtime_event_queue_and_job_cancel();
    test_editor_piece_table_edits();
    test_editor_rectangular_rendering();
    test_editor_word_wrap_rendering();
    test_editor_word_wrap_breaks_on_spaces();
    test_editor_vertical_navigation_modes();
    test_editor_file_round_trip();
    test_tui_layout_reserves_editor_input_panel();
    test_tui_regeneration_plan_uses_last_user_turn();
    if (failures != 0) {
        std::cerr << failures << " unit test(s) failed\n";
        return 1;
    }
    std::cout << "unit tests passed\n";
    return 0;
}
