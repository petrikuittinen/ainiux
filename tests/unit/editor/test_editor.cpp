#include "editor/test_editor.hpp"
#include "support/test_support.hpp"
#include "common.hpp"
#include "app/interactive_mode.hpp"
#include "cli/args.hpp"
#include "config/config.hpp"
#include "editor/ai_continue.hpp"
#include "editor/autosave.hpp"
#include "editor/editor_ai_setup.hpp"
#include "editor/clipboard.hpp"
#include "editor/editor.hpp"
#include "editor/editor_assist.hpp"
#include "editor/editor_help.hpp"
#include "editor/editor_picker.hpp"
#include "editor/file_session.hpp"
#include "editor/path_completion.hpp"
#include "editor/reformat.hpp"
#include "editor/selection.hpp"
#include "editor/split.hpp"
#include "editor/terminal_input.hpp"
#include "editor/terminal_ui.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace ainiux::test::editor {

namespace {

using ainiux::test::check;
using ainiux::test::read_fixture;

void test_editor_ai_continue_helpers() {
    ainiux::editor::PieceTable text = ainiux::editor::PieceTable::from_string("abcdefghij");
    check(text.range_text(2, 4) == "cdef", "range_text returns a bounded substring");
    check(text.range_text(0, 100) == "abcdefghij", "range_text clamps to buffer size");

    check(ainiux::editor::continue_status_message("custom_openai_chat", "gpt-test", "thinking... ESC to abort") ==
              "[custom/gpt-test] thinking... ESC to abort",
          "continue status message uses compact provider display names");
    check(ainiux::editor::continue_status_message(
              "gemini", "models/gemini-3.1-flash-lite-preview", "thinking...") ==
              u8"[gemini/gemini-3.1-flash-lite-pre…] thinking...",
          "editor status strips and truncates provider-prefixed model names");

    ainiux::provider::ChatResult continue_result;
    continue_result.ttft_ms = 100;
    continue_result.total_ms = 1100;
    continue_result.completion_tokens = 20;
    continue_result.completion_tokens_estimated = true;
    const std::string continue_complete =
        ainiux::editor::continue_completion_status_message(
            "custom_openai_chat", "gpt-test", continue_result, true);
    check(continue_complete.find(
              "[custom/gpt-test] | TTFT: 100 ms | ~20.0 token/s") == 0 &&
              continue_complete.find("| context: 23 tok") != std::string::npos,
          "continue completion status reuses TUI generation metrics formatting: " +
              continue_complete);

    ainiux::editor::EditorState state = ainiux::editor::EditorState::from_text("Once upon a ");
    state.cursor = state.text.size();
    check(state.insert_without_undo("time").ok(), "stream insert succeeds");
    check(state.text.str() == "Once upon a time", "stream insert appends at cursor");
    check(!state.can_undo(), "stream insert does not create undo entries by itself");
    check(state.insert("!").ok(), "normal insert after stream chunk creates undo");
    check(state.undo(), "undo after stream chunk succeeds");
    check(state.text.str() == "Once upon a time", "undo removes only the normal insert");

    ainiux::editor::EditorState gap =
        ainiux::editor::EditorState::from_text("beforeAFTER");
    gap.cursor = 6;
    const ainiux::editor::EditorSnapshot gap_before = gap.capture_state();
    check(gap.insert_without_undo(" inserted ").ok() &&
              gap.text.str() == "before inserted AFTER",
          "stream insertion leaves the original postfix bytes after inserted code");
    check(gap.insert_without_undo("code").ok(), "a second streamed code chunk inserts at the gap");
    gap.finalize_stream_edit(gap_before);
    check(gap.undo() && gap.text.str() == "beforeAFTER",
          "all streamed gap-completion chunks are one undoable edit");

    ainiux::editor::EditorState cancelled_gap =
        ainiux::editor::EditorState::from_text("leftRIGHT");
    cancelled_gap.cursor = 4;
    const ainiux::editor::EditorSnapshot cancelled_before = cancelled_gap.capture_state();
    check(cancelled_gap.insert_without_undo("partial  \n").ok(),
          "partial code output can be inserted before cancellation");
    cancelled_gap.finalize_stream_edit(cancelled_before);
    check(cancelled_gap.text.str() == "leftpartial  \nRIGHT",
          "cancellation commit keeps partial code and its exact trailing whitespace");
    check(cancelled_gap.undo() && cancelled_gap.text.str() == "leftRIGHT",
          "cancelled partial code remains a single undoable edit");

    const char* none_argv[] = {"ainiux", "--provider", "none", "--editor"};
    ainiux::cli::ParseResult none_parsed = ainiux::cli::parse_args(4, const_cast<char**>(none_argv));
    check(none_parsed.error.ok(), "none provider editor args parse");
    ainiux::provider::ContextResult context = ainiux::provider::build_context(none_parsed.options);
    check(context.error.ok(), "none provider context builds");
    ainiux::editor::AiContinueContext ai_continue;
    ai_continue.request = context.context;
    ai_continue.settings = ainiux::editor::ai_continue_settings(ainiux::cli::Options{});
    check(!ainiux::editor::validate_continue_request(ai_continue).ok(),
          "none provider rejects AI continue");

    const char* lm_argv[] = {"ainiux", "lmstudio", "--editor"};
    ainiux::cli::ParseResult lm_parsed = ainiux::cli::parse_args(3, const_cast<char**>(lm_argv));
    check(lm_parsed.error.ok(), "lmstudio editor args without model parse");
    context = ainiux::provider::build_context(lm_parsed.options);
    check(context.error.ok(), "lmstudio provider context builds without model");
    check(ainiux::provider::needs_interactive_model_selection(context.context),
          "lmstudio editor discovers models when no model was supplied");
    ai_continue.request = context.context;
    check(ainiux::editor::validate_continue_request(ai_continue).code == ainiux::ErrorCode::BadArgs,
          "continue validation still requires a resolved model");

    const char* localhost_argv[] = {"ainiux", "http://localhost:30000/v1", "--editor"};
    ainiux::cli::ParseResult localhost_parsed = ainiux::cli::parse_args(3, const_cast<char**>(localhost_argv));
    check(localhost_parsed.error.ok(), "localhost editor args without model parse");
    ainiux::provider::ContextResult localhost_context =
        ainiux::provider::build_context(localhost_parsed.options);
    check(localhost_context.error.ok(), "localhost custom endpoint context builds without model");
    check(ainiux::provider::needs_interactive_model_selection(localhost_context.context),
          "localhost custom endpoint discovers models when no model was supplied");

    ainiux::provider::RequestContext openai_context;
    openai_context.profile.name = "openai";
    check(ainiux::provider::needs_interactive_model_selection(openai_context),
          "remote provider without a model requires interactive model discovery");

    const char* lm_model_argv[] = {"ainiux", "lmstudio", "-m", "mock-model", "--editor"};
    ainiux::cli::ParseResult lm_model_parsed = ainiux::cli::parse_args(5, const_cast<char**>(lm_model_argv));
    check(lm_model_parsed.error.ok(), "lmstudio provider editor args parse");
    context = ainiux::provider::build_context(lm_model_parsed.options);
    check(context.error.ok(), "lmstudio provider context builds");
    ai_continue.request = context.context;
    ai_continue.settings.max_output_tokens = 1234;
    check(ainiux::editor::validate_continue_request(ai_continue).ok(), "configured provider allows continue");
    const ainiux::provider::RequestContext job_context = ainiux::editor::continue_request_context(ai_continue);
    check(job_context.options.stream, "continue forces streaming");
    check(job_context.options.has_max_output_tokens, "continue sets max output tokens");
    check(job_context.options.max_output_tokens == 1234, "continue uses configured token limit");

    ainiux::cli::Options default_options;
    const ainiux::editor::AiContinueSettings default_settings =
        ainiux::editor::ai_continue_settings(default_options);
    check(default_settings.max_prefix_chars ==
                  ainiux::editor::kDefaultAiContinuePrefixMaxChars &&
              default_settings.max_postfix_chars ==
                  ainiux::editor::kDefaultAiContinuePostfixMaxChars &&
              default_settings.max_prose_prefix_chars ==
                  ainiux::editor::kDefaultAiContinueProsePrefixMaxChars &&
              default_settings.max_prose_postfix_chars ==
                  ainiux::editor::kDefaultAiContinueProsePostfixMaxChars,
          "default continue context uses independent 4000/2000 code and 16384/4096 prose limits");
    ainiux::cli::Options cli_settings_options = default_options;
    cli_settings_options.editor_ai_continue_prefix_max_chars = 99;
    cli_settings_options.editor_ai_continue_postfix_max_chars = 88;
    cli_settings_options.editor_ai_continue_prose_prefix_max_chars = 77;
    cli_settings_options.editor_ai_continue_prose_postfix_max_chars = 66;
    const ainiux::editor::AiContinueSettings cli_settings =
        ainiux::editor::ai_continue_settings(cli_settings_options);
    check(cli_settings.max_prefix_chars == 99 && cli_settings.max_postfix_chars == 88 &&
              cli_settings.max_prose_prefix_chars == 77 &&
              cli_settings.max_prose_postfix_chars == 66,
          "effective CLI continuation settings override built-in/config values");

    const char* previous_prefix_raw = std::getenv("MAX_CONTINUE_PREFIX");
    const char* previous_postfix_raw = std::getenv("MAX_CONTINUE_POSTFIX");
    const char* previous_prose_prefix_raw = std::getenv("MAX_CONTINUE_PROSE_PREFIX");
    const char* previous_prose_postfix_raw = std::getenv("MAX_CONTINUE_PROSE_POSTFIX");
    const std::optional<std::string> previous_prefix =
        previous_prefix_raw == nullptr ? std::nullopt
                                       : std::optional<std::string>(previous_prefix_raw);
    const std::optional<std::string> previous_postfix =
        previous_postfix_raw == nullptr ? std::nullopt
                                        : std::optional<std::string>(previous_postfix_raw);
    const std::optional<std::string> previous_prose_prefix =
        previous_prose_prefix_raw == nullptr
            ? std::nullopt
            : std::optional<std::string>(previous_prose_prefix_raw);
    const std::optional<std::string> previous_prose_postfix =
        previous_prose_postfix_raw == nullptr
            ? std::nullopt
            : std::optional<std::string>(previous_prose_postfix_raw);
    const char* previous_tokens_raw = std::getenv("MAX_AI_CONTINUE_TOKENS");
    const std::optional<std::string> previous_tokens =
        previous_tokens_raw == nullptr ? std::nullopt
                                       : std::optional<std::string>(previous_tokens_raw);
#if defined(_WIN32)
    _putenv_s("MAX_CONTINUE_PREFIX", "16");
    _putenv_s("MAX_CONTINUE_POSTFIX", "8");
    _putenv_s("MAX_CONTINUE_PROSE_PREFIX", "32");
    _putenv_s("MAX_CONTINUE_PROSE_POSTFIX", "12");
    _putenv_s("MAX_AI_CONTINUE_TOKENS", "2048");
#else
    setenv("MAX_CONTINUE_PREFIX", "16", 1);
    setenv("MAX_CONTINUE_POSTFIX", "8", 1);
    setenv("MAX_CONTINUE_PROSE_PREFIX", "32", 1);
    setenv("MAX_CONTINUE_PROSE_POSTFIX", "12", 1);
    setenv("MAX_AI_CONTINUE_TOKENS", "2048", 1);
#endif
    const ainiux::editor::AiContinueSettings env_settings =
        ainiux::editor::ai_continue_settings(cli_settings_options);
    check(env_settings.max_prefix_chars == 16, "MAX_CONTINUE_PREFIX overrides default");
    check(env_settings.max_postfix_chars == 8, "MAX_CONTINUE_POSTFIX overrides default");
    check(env_settings.max_prose_prefix_chars == 32,
          "MAX_CONTINUE_PROSE_PREFIX overrides CLI/config prose prefix");
    check(env_settings.max_prose_postfix_chars == 12,
          "MAX_CONTINUE_PROSE_POSTFIX overrides CLI/config prose postfix");
    check(env_settings.max_output_tokens == 2048, "MAX_AI_CONTINUE_TOKENS overrides default");
#if !defined(_WIN32)
    setenv("MAX_CONTINUE_PROSE_PREFIX", "invalid", 1);
    setenv("MAX_CONTINUE_PROSE_POSTFIX", "999999999999999999999999999999", 1);
#else
    _putenv_s("MAX_CONTINUE_PROSE_PREFIX", "invalid");
    _putenv_s("MAX_CONTINUE_PROSE_POSTFIX", "999999999999999999999999999999");
#endif
    const ainiux::editor::AiContinueSettings invalid_env_settings =
        ainiux::editor::ai_continue_settings(cli_settings_options);
    check(invalid_env_settings.max_prose_prefix_chars == 77 &&
              invalid_env_settings.max_prose_postfix_chars == 66,
          "invalid and overflowing prose environment limits preserve CLI/config values");
#if !defined(_WIN32)
    setenv("MAX_CONTINUE_PREFIX", "0", 1);
    setenv("MAX_CONTINUE_POSTFIX", "0", 1);
    setenv("MAX_CONTINUE_PROSE_PREFIX", "0", 1);
    setenv("MAX_CONTINUE_PROSE_POSTFIX", "0", 1);
#else
    _putenv_s("MAX_CONTINUE_PREFIX", "0");
    _putenv_s("MAX_CONTINUE_POSTFIX", "0");
    _putenv_s("MAX_CONTINUE_PROSE_PREFIX", "0");
    _putenv_s("MAX_CONTINUE_PROSE_POSTFIX", "0");
#endif
    const ainiux::editor::AiContinueSettings zero_env_settings =
        ainiux::editor::ai_continue_settings(default_options);
    check(zero_env_settings.max_prefix_chars == 0 &&
              zero_env_settings.max_postfix_chars == 0 &&
              zero_env_settings.max_prose_prefix_chars == 0 &&
              zero_env_settings.max_prose_postfix_chars == 0,
          "zero environment limits disable code and prose context sides");
#if defined(_WIN32)
    if (previous_prefix.has_value()) {
        _putenv_s("MAX_CONTINUE_PREFIX", previous_prefix->c_str());
    } else {
        _putenv_s("MAX_CONTINUE_PREFIX", "");
    }
    if (previous_postfix.has_value()) {
        _putenv_s("MAX_CONTINUE_POSTFIX", previous_postfix->c_str());
    } else {
        _putenv_s("MAX_CONTINUE_POSTFIX", "");
    }
    if (previous_prose_prefix.has_value()) {
        _putenv_s("MAX_CONTINUE_PROSE_PREFIX", previous_prose_prefix->c_str());
    } else {
        _putenv_s("MAX_CONTINUE_PROSE_PREFIX", "");
    }
    if (previous_prose_postfix.has_value()) {
        _putenv_s("MAX_CONTINUE_PROSE_POSTFIX", previous_prose_postfix->c_str());
    } else {
        _putenv_s("MAX_CONTINUE_PROSE_POSTFIX", "");
    }
    if (previous_tokens.has_value()) {
        _putenv_s("MAX_AI_CONTINUE_TOKENS", previous_tokens->c_str());
    } else {
        _putenv_s("MAX_AI_CONTINUE_TOKENS", "");
    }
#else
    if (previous_prefix.has_value()) {
        setenv("MAX_CONTINUE_PREFIX", previous_prefix->c_str(), 1);
    } else {
        unsetenv("MAX_CONTINUE_PREFIX");
    }
    if (previous_postfix.has_value()) {
        setenv("MAX_CONTINUE_POSTFIX", previous_postfix->c_str(), 1);
    } else {
        unsetenv("MAX_CONTINUE_POSTFIX");
    }
    if (previous_prose_prefix.has_value()) {
        setenv("MAX_CONTINUE_PROSE_PREFIX", previous_prose_prefix->c_str(), 1);
    } else {
        unsetenv("MAX_CONTINUE_PROSE_PREFIX");
    }
    if (previous_prose_postfix.has_value()) {
        setenv("MAX_CONTINUE_PROSE_POSTFIX", previous_prose_postfix->c_str(), 1);
    } else {
        unsetenv("MAX_CONTINUE_PROSE_POSTFIX");
    }
    if (previous_tokens.has_value()) {
        setenv("MAX_AI_CONTINUE_TOKENS", previous_tokens->c_str(), 1);
    } else {
        unsetenv("MAX_AI_CONTINUE_TOKENS");
    }
#endif
}

std::string make_clipboard_helper_directory() {
    char pattern[] = "/tmp/ainiux-clipboard-unit-XXXXXX";
    char* directory = mkdtemp(pattern);
    return directory == nullptr ? std::string() : std::string(directory);
}

void write_clipboard_helper(const std::string& directory,
                            const std::string& name,
                            const std::string& body) {
    const std::string path = directory + "/" + name;
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "#!/bin/sh\n" << body << "\n";
    }
    (void)chmod(path.c_str(), 0700);
}

void test_system_clipboard_helpers() {
    namespace ed = ainiux::editor;
    const std::string directory = make_clipboard_helper_directory();
    check(!directory.empty(), "clipboard helper test creates a temporary directory");
    if (directory.empty()) return;

    write_clipboard_helper(directory, "wl-paste", "printf 'first\\nsecond \316\251'");
    write_clipboard_helper(directory, "wl-copy", "/bin/cat > '" + directory + "/copied'");
    write_clipboard_helper(directory, "xclip", "exit 9");
    write_clipboard_helper(directory, "xsel", "exit 0");
    write_clipboard_helper(directory, "pbpaste", "exit 0");
    write_clipboard_helper(directory, "pbcopy", "exit 0");
    write_clipboard_helper(directory, "termux-clipboard-get", "exit 0");
    write_clipboard_helper(directory, "termux-clipboard-set", "exit 0");
    write_clipboard_helper(directory, "powershell.exe", "exit 0");
    write_clipboard_helper(directory, "clip.exe", "exit 0");
    ed::ClipboardEnvironment environment;
    environment.path = directory + ":relative:/does/not/exist";
    environment.wayland = true;
    environment.x11 = true;

    ed::ClipboardCommand command;
    check(ed::resolve_clipboard_command(environment, false, command) &&
              command.backend == "Wayland" && command.arguments.empty(),
          "clipboard backend prefers Wayland and constructs fixed read arguments");
    check(ed::resolve_clipboard_command(environment, true, command) &&
              command.arguments ==
                  std::vector<std::string>({"--type", "text/plain;charset=utf-8"}),
          "clipboard backend constructs fixed Wayland write arguments");

    ainiux::runtime::CancellationSource source;
    ed::SystemClipboardResult read =
        ed::read_system_clipboard(environment, source.token());
    check(read.ok() && read.text == std::string("first\nsecond \316\251"),
          "clipboard helper preserves exact multiline Unicode text");
    {
        ed::ClipboardRuntime runtime;
        const std::uint64_t generation = runtime.start_read(environment);
        ed::ClipboardRuntimeEvent event;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!runtime.try_pop(event) &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        check(event.generation == generation && event.result.ok() &&
                  event.result.text == std::string("first\nsecond \316\251"),
              "clipboard runtime delivers a helper read result");
    }
    const std::string copied = "line one\nline two \360\237\230\200\n";
    ed::SystemClipboardResult written =
        ed::write_system_clipboard(environment, copied, source.token());
    check(written.ok(), "clipboard helper write succeeds");
    std::ifstream copied_input(directory + "/copied", std::ios::binary);
    const std::string copied_actual((std::istreambuf_iterator<char>(copied_input)),
                                    std::istreambuf_iterator<char>());
    check(copied_actual == copied,
          "clipboard helper receives clipboard text only through stdin without newline changes");
    write_clipboard_helper(directory, "wl-copy", "exit 0");
    const ed::SystemClipboardResult closed_input =
        ed::write_system_clipboard(environment, std::string(1024 * 1024, 'x'),
                                   source.token());
    check(closed_input.error == ed::SystemClipboardError::Failed,
          "clipboard write handles a helper that closes stdin without SIGPIPE termination");

    write_clipboard_helper(directory, "wl-paste", "exit 0");
    read = ed::read_system_clipboard(environment, source.token());
    check(read.error == ed::SystemClipboardError::Empty,
          "clipboard helper rejects an empty read");
    write_clipboard_helper(directory, "wl-paste", "printf '\\000x'");
    read = ed::read_system_clipboard(environment, source.token());
    check(read.error == ed::SystemClipboardError::NonText,
          "clipboard helper rejects non-text NUL bytes");
    write_clipboard_helper(directory, "wl-paste", "printf '\\377'");
    read = ed::read_system_clipboard(environment, source.token());
    check(read.error == ed::SystemClipboardError::Malformed,
          "clipboard helper rejects malformed UTF-8");

    environment.wayland = false;
    check(ed::resolve_clipboard_command(environment, false, command) &&
              command.backend == "X11 xclip" &&
              command.arguments ==
                  std::vector<std::string>({"-selection", "clipboard", "-out"}),
          "clipboard backend prefers xclip over xsel with fixed clipboard-selection arguments");
    read = ed::read_system_clipboard(environment, source.token());
    check(read.error == ed::SystemClipboardError::Failed,
          "clipboard helper reports a nonzero exit");

    environment = {};
    environment.path = directory;
    environment.macos = true;
    check(ed::resolve_clipboard_command(environment, false, command) &&
              command.backend == "macOS" && command.executable == directory + "/pbpaste",
          "clipboard backend selects pbpaste on macOS");
    environment = {};
    environment.path = directory;
    environment.wsl = true;
    check(ed::resolve_clipboard_command(environment, false, command) &&
              command.backend == "WSL" &&
              command.arguments ==
                  std::vector<std::string>({"-NoProfile", "-NonInteractive",
                                            "-Command", "Get-Clipboard -Raw"}),
          "clipboard backend constructs fixed PowerShell arguments on WSL");
    environment.termux = true;
    check(ed::resolve_clipboard_command(environment, true, command) &&
              command.backend == "Termux" &&
              command.executable == directory + "/termux-clipboard-set",
          "clipboard backend prefers the Termux clipboard helper when detected");

    environment = {};
    environment.path = "relative:" + directory + "/missing";
    check(!ed::resolve_clipboard_command(environment, false, command),
          "clipboard executable lookup ignores relative PATH entries");
    read = ed::read_system_clipboard(environment, source.token());
    check(read.error == ed::SystemClipboardError::Unavailable,
          "clipboard read reports missing helpers");

    std::filesystem::remove_all(directory);
}

void test_system_clipboard_cancellation_and_limits() {
    namespace ed = ainiux::editor;
    const std::string directory = make_clipboard_helper_directory();
    if (directory.empty()) {
        check(false, "clipboard failure tests create a temporary directory");
        return;
    }
    write_clipboard_helper(directory, "wl-paste", "/bin/sleep 5");
    ed::ClipboardEnvironment environment;
    environment.path = directory;
    environment.wayland = true;
    ainiux::runtime::CancellationSource source;
    ed::SystemClipboardResult cancelled;
    const auto started = std::chrono::steady_clock::now();
    std::thread worker([&]() {
        cancelled = ed::read_system_clipboard(environment, source.token());
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    source.cancel();
    worker.join();
    check(cancelled.error == ed::SystemClipboardError::Cancelled &&
              std::chrono::steady_clock::now() - started < std::chrono::seconds(1),
          "clipboard cancellation terminates and reaps a blocked helper promptly");

    ainiux::runtime::CancellationSource timeout_source;
    const ed::SystemClipboardResult timed_out =
        ed::read_system_clipboard(environment, timeout_source.token());
    check(timed_out.error == ed::SystemClipboardError::Timeout,
          "clipboard helper is terminated after the two-second timeout");

    write_clipboard_helper(directory, "wl-paste",
                           "/usr/bin/head -c 16777217 /dev/zero");
    ainiux::runtime::CancellationSource size_source;
    const ed::SystemClipboardResult oversized =
        ed::read_system_clipboard(environment, size_source.token());
    check(oversized.error == ed::SystemClipboardError::TooLarge,
          "clipboard helper rejects output above 16 MiB");
    std::filesystem::remove_all(directory);
}

void test_osc52_clipboard_decode() {
    namespace ed = ainiux::editor;
    std::string text;
    std::string error;
    check(ed::decode_osc52_clipboard_payload("bGluZSAxCuKYoSBsaW5lIDI=", text, error) &&
              text == std::string("line 1\n\342\230\241 line 2"),
          "OSC 52 decoder preserves multiline Unicode text");
    check(!ed::decode_osc52_clipboard_payload("%%==", text, error) &&
              error.find("malformed") != std::string::npos,
          "OSC 52 decoder rejects malformed base64");
    check(!ed::decode_osc52_clipboard_payload("", text, error),
          "OSC 52 decoder rejects an empty reply");

    ed::clear_terminal_input_queue();
    ed::request_terminal_clipboard();
    ed::push_terminal_input_bytes("\x1b]52;c;b25lCnR3bw==\x1b\\Z");
    ed::TerminalInputEvent event;
    check(ed::read_terminal_input(event, 0) &&
              event.type == ed::TerminalInputType::Osc52ClipboardResponse &&
              event.text == "one\ntwo",
          "terminal input recognizes an ST-terminated OSC 52 reply");
    check(ed::read_terminal_input(event, 0) &&
              event.type == ed::TerminalInputType::Byte && event.byte == 'Z',
          "OSC 52 reply decoding preserves adjacent keyboard input");

    ed::clear_terminal_input_queue();
    ed::request_terminal_clipboard();
    ed::push_terminal_input_bytes("\x1b]52;c;eA==\x07");
    check(ed::read_terminal_input(event, 0) &&
              event.type == ed::TerminalInputType::Osc52ClipboardResponse &&
              event.text == "x",
          "terminal input recognizes a BEL-terminated OSC 52 reply");
    ed::cancel_terminal_clipboard_request();
}

void test_editor_ai_setup_helpers() {
    check(ainiux::editor::editor_no_provider_message() ==
              "No provider chosen. Use /provider to choose one",
          "editor no-provider message mentions /provider");
    check(ainiux::editor::editor_no_model_message() == "No model chosen. Use /model to choose one",
          "editor no-model message mentions /model");

    std::optional<ainiux::editor::AiContinueContext> no_context;
    check(!ainiux::editor::editor_ai_has_provider(no_context), "missing context has no provider");
    check(!ainiux::editor::editor_ai_ready(no_context), "missing context is not AI-ready");
    check(ainiux::editor::editor_startup_status(no_context).find("/provider") != std::string::npos,
          "startup status without context mentions /provider");

    const char* none_argv[] = {"ainiux", "--provider", "none", "--editor"};
    ainiux::cli::ParseResult none_parsed = ainiux::cli::parse_args(4, const_cast<char**>(none_argv));
    check(none_parsed.error.ok(), "none provider editor args parse for ai setup");
    ainiux::provider::ContextResult none_context = ainiux::provider::build_context(none_parsed.options);
    check(none_context.error.ok(), "none provider context builds for ai setup");
    ainiux::editor::AiContinueContext offline_continue;
    offline_continue.request = none_context.context;
    check(!ainiux::editor::editor_ai_has_provider(offline_continue),
          "offline provider is not considered chosen");
    check(ainiux::editor::editor_startup_status(offline_continue).find("Local editor") != std::string::npos,
          "offline startup status mentions local editor");

    const char* lm_model_argv[] = {"ainiux", "lmstudio", "-m", "mock-model", "--editor"};
    ainiux::cli::ParseResult lm_model_parsed = ainiux::cli::parse_args(5, const_cast<char**>(lm_model_argv));
    check(lm_model_parsed.error.ok(), "lmstudio editor args parse for ai setup");
    ainiux::provider::ContextResult ready_context = ainiux::provider::build_context(lm_model_parsed.options);
    check(ready_context.error.ok(), "lmstudio provider context builds for ai setup");
    ainiux::editor::AiContinueContext ready_continue;
    ready_continue.request = ready_context.context;
    ready_continue.assist_config = ainiux::editor::default_editor_assist_config();
    check(ainiux::editor::editor_ai_has_provider(ready_continue), "configured provider is chosen");
    ready_continue.request.options.model = "mock-model";
    check(ainiux::editor::editor_ai_ready(ready_continue), "provider with model is AI-ready");
    const std::string ready_startup_status = ainiux::editor::editor_startup_status(ready_continue);
    check(ready_startup_status.find("[lmstudio/mock-model]") == 0,
          "ready startup status shows provider and model like chat mode");
    check(ready_startup_status.find("ready") != std::string::npos,
          "ready startup status mentions ready");

    std::optional<ainiux::editor::AiContinueContext> created;
    ainiux::editor::EditorAssistConfig assist_config = ainiux::editor::default_editor_assist_config();
    check(ainiux::editor::ensure_editor_ai_context(created, assist_config).ok(),
          "ensure_editor_ai_context creates offline context");
    check(created.has_value(), "ensure_editor_ai_context populates optional");
    check(!ainiux::editor::editor_ai_has_provider(created), "created default context stays offline");

    check(ainiux::editor::apply_editor_model(created, "mock-model").code ==
              ainiux::ErrorCode::UnsupportedFeature,
          "apply_editor_model requires a provider first");
    check(ainiux::editor::apply_editor_provider_target(created, assist_config, "openai").ok(),
          "apply_editor_provider_target can switch to openai");
    check(ainiux::editor::editor_ai_has_provider(created), "openai provider is active after apply");
    check(ainiux::editor::apply_editor_model(created, "gpt-test").ok(),
          "apply_editor_model succeeds after provider is chosen");
    check(created->request.options.model == "gpt-test", "apply_editor_model stores model name");
    created->request.options.context_tokens = 131072;
    created->request.options.reasoning = ainiux::ReasoningSelection::named("high");
    check(ainiux::editor::apply_editor_model(created, "gpt-test").ok() &&
              created->request.options.reasoning ==
                  ainiux::ReasoningSelection::named("high"),
          "reselecting the actual editor model preserves reasoning");
    check(ainiux::editor::apply_editor_model(created, "gpt-other").ok() &&
              created->request.options.reasoning.is_auto(),
          "changing the editor model resets reasoning to Auto");
    check(created->request.options.context_tokens == 0,
          "changing the editor model clears its previous automatic context window");
    created->request.options.has_context_tokens = true;
    created->request.options.context_tokens = 1000000;
    check(ainiux::editor::apply_editor_model(created, "gpt-explicit").ok() &&
              created->request.options.context_tokens == 1000000,
          "changing the editor model preserves an explicit context override");
    created->request.options.reasoning = ainiux::ReasoningSelection::named("high");
    check(ainiux::editor::apply_editor_provider_target(created, assist_config, "openai").ok() &&
              created->request.options.model == "gpt-explicit" &&
              created->request.options.reasoning ==
                  ainiux::ReasoningSelection::named("high"),
          "reselecting the actual editor provider preserves model and reasoning");
    check(ainiux::editor::apply_editor_provider_target(created, assist_config, "openrouter").ok(),
          "switching an editor provider succeeds after a model was selected");
    check(created->request.options.model.empty(),
          "switching an editor provider clears the previous provider's model");

    const char* openrouter_argv[] = {"ainiux", "openrouter", "--editor"};
    ainiux::cli::ParseResult openrouter_parsed =
        ainiux::cli::parse_args(3, const_cast<char**>(openrouter_argv));
    check(openrouter_parsed.error.ok(), "openrouter editor args without model parse");
    ainiux::provider::ContextResult openrouter_context =
        ainiux::provider::build_context(openrouter_parsed.options);
    check(openrouter_context.error.ok(), "openrouter provider context builds without model");
    check(openrouter_context.context.options.model.empty(),
          "openrouter editor startup leaves model empty");
    ainiux::editor::AiContinueContext openrouter_continue;
    openrouter_continue.request = openrouter_context.context;
    check(ainiux::editor::editor_ai_has_provider(openrouter_continue),
          "openrouter editor startup has provider");
    check(!ainiux::editor::editor_ai_ready(openrouter_continue),
          "openrouter editor startup is not AI-ready without model");
    const std::string openrouter_startup_status =
        ainiux::editor::editor_startup_status(openrouter_continue);
    check(openrouter_startup_status.find("/model") != std::string::npos,
          "openrouter editor startup status mentions /model");
    check(openrouter_startup_status.find("Choose a model with /model") != std::string::npos,
          "openrouter editor startup status asks for /model");
    check(openrouter_startup_status.find("/provider") != std::string::npos,
          "openrouter editor startup status mentions /provider like chat mode");

    const char* lm_no_model_argv[] = {"ainiux", "lmstudio", "--editor"};
    ainiux::cli::ParseResult lm_no_model_parsed =
        ainiux::cli::parse_args(3, const_cast<char**>(lm_no_model_argv));
    check(lm_no_model_parsed.error.ok(), "lmstudio editor args without model parse for deferral");
    ainiux::provider::ContextResult lm_no_model_context =
        ainiux::provider::build_context(lm_no_model_parsed.options);
    check(lm_no_model_context.error.ok(), "lmstudio provider context builds without model for deferral");
    check(lm_no_model_context.context.options.model.empty(),
          "lmstudio editor startup leaves model empty");
    ainiux::editor::AiContinueContext lm_no_model_continue;
    lm_no_model_continue.request = lm_no_model_context.context;
    check(ainiux::editor::editor_ai_has_provider(lm_no_model_continue),
          "lmstudio editor startup has provider");
    check(!ainiux::editor::editor_ai_ready(lm_no_model_continue),
          "lmstudio editor startup is not AI-ready without model");
    std::optional<ainiux::editor::AiContinueContext> deferred_provider;
    check(ainiux::editor::apply_editor_provider_target(deferred_provider, assist_config, "lmstudio").ok(),
          "/provider lmstudio succeeds without contacting the model endpoint");
    check(deferred_provider.has_value() && deferred_provider->request.options.model.empty(),
          "/provider lmstudio leaves model empty for /model selection");
}

void test_editor_reasoning_picker() {
    ainiux::editor::EditorProviderModelPicker picker;
    picker.open_reasoning(
        {"auto", "low", "high"},
        {"Auto (provider default: medium)", "Low", "High"},
        1);
    picker.refresh_view();
    check(picker.active && picker.for_reasoning && picker.selected == 1 &&
              picker.view.text.range_text(0, picker.view.text.size()).find("> Low") !=
                  std::string::npos,
          "editor reasoning picker highlights the current value");

    std::string status;
    check(picker.handle_escape("[B", status) && picker.selected == 2 &&
              status == "Selected reasoning 3/3",
          "editor reasoning picker navigates with the shared selector keys");
    picker.refresh_view();
    check(picker.view.text.range_text(0, picker.view.text.size()).find("> High") !=
              std::string::npos,
          "editor reasoning picker refresh highlights the navigated value");
    check(picker.handle_escape("", status) && !picker.active &&
              status == "Reasoning selection cancelled",
          "editor reasoning picker cancellation returns to editing");
}

void test_chat_editor_reasoning_context_sync() {
    ainiux::app::InteractiveSession session;
    check(ainiux::app::editor_toggle_target(session) ==
              ainiux::app::InteractiveUiTarget::Chat,
          "standalone editor Ctrl+P defaults to chat");
    session.editor_return_mode = ainiux::app::InteractiveMode::Agent;
    check(ainiux::app::editor_toggle_target(session) ==
              ainiux::app::InteractiveUiTarget::Agent,
          "editor Ctrl+P returns to the agent mode that opened it");
    session.editor_return_mode = ainiux::app::InteractiveMode::Chat;
    check(ainiux::app::editor_toggle_target(session) ==
              ainiux::app::InteractiveUiTarget::Chat,
          "editor Ctrl+P returns to the chat mode that opened it");

    session.context.options.reasoning =
        ainiux::ReasoningSelection::named("high");
    session.context.options.reasoning_explicit = true;
    ainiux::app::sync_shared_provider_to_editor(session);
    check(session.ai_continue.has_value() &&
              session.ai_continue->request.options.reasoning ==
                  ainiux::ReasoningSelection::named("high"),
          "chat-to-editor context synchronization carries reasoning");

    session.ai_continue->request.options.reasoning =
        ainiux::ReasoningSelection::token_budget(2048);
    ainiux::app::sync_editor_provider_to_shared(session, session.ai_continue);
    check(session.context.options.reasoning ==
              ainiux::ReasoningSelection::token_budget(2048),
          "editor-to-chat context synchronization carries reasoning");
}

void test_editor_assist_helpers() {
    const ainiux::editor::EditorAssistConfig default_config =
        ainiux::editor::default_editor_assist_config();
    check(default_config.behavior_rules.find("one-shot") != std::string::npos,
          "default editor assist behavior rules mention one-shot prompts");
    check(default_config.behavior_rules.find("not as instructions") != std::string::npos,
          "default editor assist behavior rules say content is not instructions");
    const ainiux::editor::EditorAssistCommand* default_spell =
        ainiux::editor::find_assist_command(default_config, "/spell");
    check(default_spell != nullptr && default_spell->prompt.find("spelling") != std::string::npos,
          "default editor assist spell prompt is populated");
    const ainiux::editor::EditorAssistCommand* default_comment =
        ainiux::editor::find_assist_command(default_config, "/comment");
    check(default_comment != nullptr && default_comment->prompt.find("improve the text") != std::string::npos,
          "default editor assist comment prompt is populated");
    const ainiux::editor::EditorAssistCommand* default_rewrite =
        ainiux::editor::find_assist_command(default_config, "/rewrite");
    check(default_rewrite != nullptr && default_rewrite->prompt.find("factual accuracy") != std::string::npos,
          "default editor assist rewrite prompt is populated");
    const ainiux::editor::EditorAssistCommand* default_english =
        ainiux::editor::find_assist_command(default_config, "/English");
    check(default_english != nullptr && default_english->prompt.find("English") != std::string::npos,
          "default editor assist English prompt is populated");
    const ainiux::editor::EditorAssistCommand* default_chinese =
        ainiux::editor::find_assist_command(default_config, "/Chinese");
    check(default_chinese != nullptr && default_chinese->prompt.find("Chinese") != std::string::npos,
          "default editor assist Chinese prompt is populated");
    const ainiux::editor::EditorAssistCommand* default_finnish =
        ainiux::editor::find_assist_command(default_config, "/Finnish");
    check(default_finnish != nullptr && default_finnish->prompt.find("Finnish") != std::string::npos,
          "default editor assist Finnish prompt is populated");
    for (const char* language : {"/German", "/French", "/Italian", "/Spanish", "/Portuguese",
                                 "/Arabic", "/Hindi", "/Japanese", "/Korean", "/Swedish",
                                 "/Polish", "/Russian"}) {
        const ainiux::editor::EditorAssistCommand* command =
            ainiux::editor::find_assist_command(default_config, language);
        check(command != nullptr && !command->prompt.empty(),
              std::string("default editor assist prompt is populated for ") + language);
    }

    ainiux::editor::ParsedAssistCommand parsed =
        ainiux::editor::parse_assist_command("/spell all", default_config);
    check(parsed.ok && parsed.kind == ainiux::editor::AssistCommandKind::Configured &&
              parsed.scope == ainiux::editor::AssistScope::All,
          "/spell all parses");

    parsed = ainiux::editor::parse_assist_command("spell all", default_config);
    check(parsed.ok && parsed.kind == ainiux::editor::AssistCommandKind::Configured &&
              parsed.scope == ainiux::editor::AssistScope::All,
          "slashless editor assist commands parse");
    parsed = ainiux::editor::parse_assist_command("CHINESE n", default_config);
    check(parsed.ok && parsed.kind == ainiux::editor::AssistCommandKind::Configured &&
              parsed.scope == ainiux::editor::AssistScope::NewBuffer,
          "slashless editor assist commands are case-insensitive");

    parsed = ainiux::editor::parse_assist_command("/grammar selection", default_config);
    check(parsed.ok && parsed.kind == ainiux::editor::AssistCommandKind::Configured &&
              parsed.scope == ainiux::editor::AssistScope::Selection,
          "/grammar selection parses");

    parsed = ainiux::editor::parse_assist_command("/spell", default_config);
    check(parsed.ok && parsed.kind == ainiux::editor::AssistCommandKind::Configured &&
              !parsed.scope.has_value(),
          "bare /spell requests scope");

    parsed = ainiux::editor::parse_assist_command("/continue", default_config);
    check(parsed.ok && parsed.kind == ainiux::editor::AssistCommandKind::Configured &&
              !parsed.scope.has_value(),
          "bare /continue runs without scope");

    parsed = ainiux::editor::parse_assist_command("/fact newbuffer", default_config);
    check(parsed.ok && parsed.kind == ainiux::editor::AssistCommandKind::Configured &&
              parsed.scope == ainiux::editor::AssistScope::NewBuffer,
          "/fact newbuffer parses");
    parsed = ainiux::editor::parse_assist_command("/fact v", default_config);
    check(parsed.ok && parsed.kind == ainiux::editor::AssistCommandKind::Configured &&
              parsed.scope == ainiux::editor::AssistScope::NewBufferVSplit,
          "/fact v parses as vertical new-buffer split");
    parsed = ainiux::editor::parse_assist_command("/spell hsplit", default_config);
    check(parsed.ok && parsed.kind == ainiux::editor::AssistCommandKind::Configured &&
              parsed.scope == ainiux::editor::AssistScope::NewBufferHSplit,
          "/spell hsplit parses as horizontal new-buffer split");

    parsed = ainiux::editor::parse_assist_command("/fact n", default_config);
    check(parsed.ok && parsed.kind == ainiux::editor::AssistCommandKind::Configured &&
              parsed.scope == ainiux::editor::AssistScope::NewBuffer,
          "/fact n parses as new buffer");

    parsed = ainiux::editor::parse_assist_command("/fact insert", default_config);
    check(parsed.ok && parsed.kind == ainiux::editor::AssistCommandKind::Configured &&
              parsed.scope == ainiux::editor::AssistScope::Insert,
          "/fact insert parses");

    parsed = ainiux::editor::parse_assist_command("/comment all", default_config);
    check(parsed.ok && parsed.kind == ainiux::editor::AssistCommandKind::Configured &&
              parsed.scope == ainiux::editor::AssistScope::All,
          "/comment all parses");

    parsed = ainiux::editor::parse_assist_command("/rewrite selection", default_config);
    check(parsed.ok && parsed.kind == ainiux::editor::AssistCommandKind::Configured &&
              parsed.scope == ainiux::editor::AssistScope::Selection,
          "/rewrite selection parses");

    parsed = ainiux::editor::parse_assist_command("/english newbuffer", default_config);
    check(parsed.ok && parsed.kind == ainiux::editor::AssistCommandKind::Configured &&
              parsed.scope == ainiux::editor::AssistScope::NewBuffer,
          "/English parses case-insensitively");

    parsed = ainiux::editor::parse_assist_command("/Chinese n", default_config);
    check(parsed.ok && parsed.kind == ainiux::editor::AssistCommandKind::Configured &&
              parsed.scope == ainiux::editor::AssistScope::NewBuffer,
          "/Chinese n parses as new buffer");

    parsed = ainiux::editor::parse_assist_command("/Finnish all", default_config);
    check(parsed.ok && parsed.kind == ainiux::editor::AssistCommandKind::Configured &&
              parsed.scope == ainiux::editor::AssistScope::All,
          "/Finnish all parses");

    parsed = ainiux::editor::parse_assist_command("/prompt rewrite formally", default_config);
    check(parsed.ok && parsed.kind == ainiux::editor::AssistCommandKind::Prompt &&
              parsed.custom_prompt == "rewrite formally",
          "/prompt captures custom text");
    check(ainiux::editor::assist_prompt_mode_message() ==
              "/prompt for selection (s), all (a), insert (i), new buffer (n), vsplit (v), hsplit (h)",
          "/prompt offers the standard scoped AI choices including split new-buffer modes");
    check(ainiux::editor::assist_prompt_mode_for_key('s') ==
                  ainiux::editor::AssistPromptMode::Selection &&
              ainiux::editor::assist_prompt_mode_for_key('A') ==
                  ainiux::editor::AssistPromptMode::All &&
              ainiux::editor::assist_prompt_mode_for_key('i') ==
                  ainiux::editor::AssistPromptMode::Insert &&
              ainiux::editor::assist_prompt_mode_for_key('N') ==
                  ainiux::editor::AssistPromptMode::NewBuffer &&
              ainiux::editor::assist_prompt_mode_for_key('v') ==
                  ainiux::editor::AssistPromptMode::NewBufferVSplit &&
              ainiux::editor::assist_prompt_mode_for_key('H') ==
                  ainiux::editor::AssistPromptMode::NewBufferHSplit,
          "/prompt mode keys select advertised choices case-insensitively");
    check(!ainiux::editor::assist_prompt_mode_for_key('c').has_value(),
          "/prompt no longer accepts the continue-only mode key");

    parsed = ainiux::editor::parse_assist_command("/prompt", default_config);
    check(!parsed.ok, "bare /prompt is rejected");

    parsed = ainiux::editor::parse_assist_command("/regenerate", default_config);
    check(parsed.ok && parsed.kind == ainiux::editor::AssistCommandKind::Regenerate,
          "/regenerate parses");

    parsed = ainiux::editor::parse_assist_command("/regenerate now", default_config);
    check(!parsed.ok, "/regenerate rejects arguments");

    parsed = ainiux::editor::parse_assist_command("/search ainiux cli", default_config);
    check(parsed.ok && parsed.kind == ainiux::editor::AssistCommandKind::WebSearch,
          "/search parses with query");
    check(parsed.custom_prompt == "ainiux cli", "/search stores the query text");

    parsed = ainiux::editor::parse_assist_command("/search", default_config);
    check(!parsed.ok, "bare /search is rejected");

    parsed = ainiux::editor::parse_assist_command("/quit", default_config);
    check(parsed.ok && parsed.kind == ainiux::editor::AssistCommandKind::Quit, "/quit parses");

    parsed = ainiux::editor::parse_assist_command("//quit", default_config);
    check(parsed.ok && parsed.kind == ainiux::editor::AssistCommandKind::Quit,
          "duplicate leading slashes in /quit are accepted");

    const std::vector<std::string> completions =
        ainiux::editor::assist_command_completions(default_config);
    check(!completions.empty() && completions.front() == "/spell", "assist completions include /spell");
    check(std::find(completions.begin(), completions.end(), "/regenerate") != completions.end(),
          "assist completions include /regenerate");
    for (const char* builtin :
         {"/spell",     "/grammar", "/fact",     "/comment",  "/rewrite",  "/English",
          "/Chinese",   "/Finnish", "/German",   "/French",   "/Italian",  "/Spanish",
          "/Portuguese","/Arabic",  "/Hindi",    "/Japanese", "/Korean",   "/Swedish",
          "/Polish",    "/Russian"}) {
        for (const char* mode : {"selection", "all", "newbuffer", "insert", "v", "h"}) {
            const std::string variant = std::string(builtin) + " " + mode;
            check(std::find(completions.begin(), completions.end(), variant) != completions.end(),
                  std::string("builtin assist completions include ") + variant);
        }
    }
    check(std::find(completions.begin(), completions.end(), "/continue") != completions.end(),
          "builtin assist completions include bare /continue");
    for (const char* builtin :
         {"/spell",     "/grammar", "/fact",     "/comment",  "/rewrite",  "/English",
          "/Chinese",   "/Finnish", "/German",   "/French",   "/Italian",  "/Spanish",
          "/Portuguese","/Arabic",  "/Hindi",    "/Japanese", "/Korean",   "/Swedish",
          "/Polish",    "/Russian"}) {
        const ainiux::editor::EditorAssistCommand* command =
            ainiux::editor::find_assist_command(default_config, builtin);
        check(command != nullptr && command->modes.size() == 4,
              std::string("default ") + builtin +
                  " exposes selection, all, newbuffer, and insert modes");
        const std::string scope_prompt = ainiux::editor::assist_scope_prompt(*command);
        check(scope_prompt.find("selection (s)") != std::string::npos &&
                  scope_prompt.find("all (a)") != std::string::npos &&
                  scope_prompt.find("new buffer (n)") != std::string::npos &&
                  scope_prompt.find("vsplit (v)") != std::string::npos &&
                  scope_prompt.find("hsplit (h)") != std::string::npos &&
                  scope_prompt.find("insert (i)") != std::string::npos &&
                  scope_prompt.find("continue (c)") == std::string::npos,
              std::string("default ") + builtin + " scope prompt lists scoped modes without continue");
    }
    {
        const ainiux::editor::EditorAssistCommand* continue_command =
            ainiux::editor::find_assist_command(default_config, "/continue");
        check(continue_command != nullptr && continue_command->modes.size() == 1,
              "default /continue exposes only continue mode");
    }

    std::string input = "/sp";
    ainiux::editor::AssistCompleterState completer;
    ainiux::editor::AssistCompletionResult completion =
        ainiux::editor::complete_assist_command(input, completer, default_config);
    check(!completion.changed && input == "/sp" && completer.active,
          "assist tab completion enters cycle mode for /spell, /speech, and /Spanish");
    completion = ainiux::editor::complete_assist_command(input, completer, default_config);
    check(completion.cycling && input.rfind("/spell", 0) == 0,
          "assist tab completion cycles from /sp to /spell");

    input = "/";
    completer = ainiux::editor::AssistCompleterState{};
    ainiux::editor::complete_assist_command(input, completer, default_config);
    check(completer.active && input == "/", "assist tab completion on / enters cycle mode");
    input += "fa";
    completion = ainiux::editor::complete_assist_command(input, completer, default_config);
    check(input == "/fact", "assist tab completion rematches after editing / to /fa");
    check(completer.active && completer.candidates.size() == 7,
          "/fa matches /fact and its six scoped variants for cycling");

    input = "/";
    completer = ainiux::editor::AssistCompleterState{};
    ainiux::editor::complete_assist_command(input, completer, default_config);
    input = "/q";
    completion = ainiux::editor::complete_assist_command(input, completer, default_config);
    check(input == "/quit", "assist tab completion rematches /q after stale / cycle state");

    input = "/en";
    completer = ainiux::editor::AssistCompleterState{};
    completion = ainiux::editor::complete_assist_command(input, completer, default_config);
    check(completion.changed && input == "/English",
          "assist tab completion matches capitalized commands case-insensitively");
    input = "/c";
    completer = ainiux::editor::AssistCompleterState{};
    completion = ainiux::editor::complete_assist_command(input, completer, default_config);
    check(!completion.changed && input == "/c" && completer.active,
          "assist tab completion keeps ambiguous mixed-case /c prefix");

    input = "rew";
    completer = ainiux::editor::AssistCompleterState{};
    completion = ainiux::editor::complete_assist_command(input, completer, default_config);
    check(completion.changed && input == "rewrite",
          "slashless completion preserves the slashless form");
    input = "ch";
    completer = ainiux::editor::AssistCompleterState{};
    completion = ainiux::editor::complete_assist_command(input, completer, default_config);
    check(completion.changed && input == "Chinese",
          "slashless completion handles case-insensitive configured names");
    check(ainiux::editor::editor_assist_path_prefix_length("open notes") == 5 &&
              ainiux::editor::editor_assist_path_prefix_length("/saveas notes") == 8,
          "slashless and slashed path commands expose path completion");

    ainiux::editor::EditorState state =
        ainiux::editor::EditorState::from_text("hello wrld");
    state.selection.anchor = 0;
    state.selection.active = 5;
    ainiux::editor::AiContinueContext context;
    context.request.profile.name = "lm_studio";
    context.request.options.model = "mock-model";
    context.assist_config = default_config;
    const std::optional<size_t> spell_index = ainiux::editor::assist_command_index(default_config, "/spell");
    check(spell_index.has_value(), "default assist config indexes /spell");
    ainiux::editor::AssistExecution execution = ainiux::editor::build_assist_execution(
        state,
        context,
        ainiux::editor::AssistCommandKind::Configured,
        *spell_index,
        ainiux::editor::AssistScope::Selection,
        "",
        std::nullopt);
    check(execution.ok && !execution.stream &&
              execution.edit_kind == ainiux::editor::AssistEditKind::ReplaceInPlace &&
              execution.replace_start == 0 && execution.replace_count == 5,
          "spell selection builds in-place execution");
    check(execution.messages.size() == 2 && execution.messages.front().role == "system" &&
              execution.messages.front().content.find(default_spell->prompt) != std::string::npos &&
              execution.messages.front().content.find(default_config.behavior_rules) != std::string::npos,
          "spell selection uses task prompt plus default assist rules in system message");
    check(execution.messages.back().role == "user" &&
              execution.messages.back().content == "<content>hello</content>",
          "spell selection wraps buffer text in content tags for user message");

    ainiux::editor::EditorState prompt_state =
        ainiux::editor::EditorState::from_text("selected and remaining");
    prompt_state.selection.anchor = 0;
    prompt_state.selection.active = 8;
    execution = ainiux::editor::build_assist_execution(
        prompt_state,
        context,
        ainiux::editor::AssistCommandKind::Prompt,
        0,
        std::nullopt,
        "Rewrite clearly",
        ainiux::editor::AssistPromptMode::Selection);
    check(execution.ok && !execution.stream &&
              execution.edit_kind == ainiux::editor::AssistEditKind::ReplaceInPlace &&
              execution.replace_start == 0 && execution.replace_count == 8,
          "/prompt selection replaces the selected text");

    execution = ainiux::editor::build_assist_execution(
        prompt_state,
        context,
        ainiux::editor::AssistCommandKind::Prompt,
        0,
        std::nullopt,
        "Rewrite clearly",
        ainiux::editor::AssistPromptMode::All);
    check(execution.ok && !execution.stream &&
              execution.edit_kind == ainiux::editor::AssistEditKind::ReplaceInPlace &&
              execution.replace_start == 0 && execution.replace_count == prompt_state.text.size(),
          "/prompt all replaces the whole buffer");

    execution = ainiux::editor::build_assist_execution(
        prompt_state,
        context,
        ainiux::editor::AssistCommandKind::Prompt,
        0,
        std::nullopt,
        "Rewrite clearly",
        ainiux::editor::AssistPromptMode::Insert);
    check(execution.ok && execution.stream &&
              execution.edit_kind == ainiux::editor::AssistEditKind::StreamInsert &&
              execution.messages.back().content == "<content>selected</content>",
          "/prompt insert streams from the selected text at the cursor");

    execution = ainiux::editor::build_assist_execution(
        prompt_state,
        context,
        ainiux::editor::AssistCommandKind::Prompt,
        0,
        std::nullopt,
        "Rewrite clearly",
        ainiux::editor::AssistPromptMode::NewBuffer);
    check(execution.ok && execution.stream &&
              execution.edit_kind == ainiux::editor::AssistEditKind::NewBuffer &&
              execution.messages.back().content == "<content>selected</content>",
          "/prompt new buffer streams from the selected text into a new buffer");

    prompt_state.clear_selection();
    execution = ainiux::editor::build_assist_execution(
        prompt_state,
        context,
        ainiux::editor::AssistCommandKind::Prompt,
        0,
        std::nullopt,
        "Rewrite clearly",
        ainiux::editor::AssistPromptMode::NewBuffer);
    check(!execution.ok &&
              execution.error_message.find("new buffer requires an active selection") !=
                  std::string::npos,
          "/prompt new buffer rejects a missing selection");

    const ainiux::editor::EditorAssistCommand* default_continue =
        ainiux::editor::find_assist_command(default_config, "/continue");
    check(default_continue != nullptr, "default assist config includes /continue");
    const std::optional<size_t> continue_index =
        ainiux::editor::assist_command_index(default_config, "/continue");
    check(continue_index.has_value(), "default assist config indexes /continue");
    context.request.options.system = "Custom system";
    state.cursor = state.text.size();
    execution = ainiux::editor::build_assist_execution(
        state,
        context,
        ainiux::editor::AssistCommandKind::Configured,
        *continue_index,
        ainiux::editor::AssistScope::Continue,
        "",
        std::nullopt);
    check(execution.ok && execution.stream &&
              execution.edit_kind == ainiux::editor::AssistEditKind::StreamInsert &&
              execution.prose_completion && !execution.code_completion,
          "/continue continue builds streaming execution");
    check(execution.messages.back().content.find(
              "AINIUX_PROSE_CONTEXT_V1\nMODE_BYTES 4\ntext\nPREFIX_BYTES 10\nhello wrld\n"
              "CURSOR_BYTES 9\n<CURSOR/>\nEND_AINIUX_PROSE_CONTEXT_V1") !=
              std::string::npos &&
              execution.messages.back().content.find("POSTFIX_BYTES") == std::string::npos,
          "/continue at prose buffer end sends a length-delimited prefix and omits postfix");
    check(execution.messages.front().content.rfind("Custom system", 0) == 0 &&
              execution.messages.front().content.find(default_continue->prompt) !=
                  std::string::npos &&
              execution.messages.front().content.find("Continue at substantial length") !=
                  std::string::npos &&
              execution.messages.front().content.find("concrete examples and relevant numbers") !=
                  std::string::npos &&
              execution.messages.front().content.find("make brave, coherent choices and use vivid language") !=
                  std::string::npos &&
              execution.messages.front().content.find("Never offer suggestions, alternatives") !=
                  std::string::npos &&
              execution.messages.front().content.find("Never summarize, paraphrase, recap, restart, repeat") !=
                  std::string::npos,
          "end-of-document prose continuation demands substantial concrete and creative writing");
    check(execution.usage_messages.empty(),
          "/continue continue omits separate usage messages when the full prefix fits the read limit");

    context.settings.max_prose_prefix_chars = 4096;
    ainiux::editor::EditorState long_state =
        ainiux::editor::EditorState::from_text(std::string(5000, 'a'));
    long_state.cursor = long_state.text.size();
    execution = ainiux::editor::build_assist_execution(
        long_state,
        context,
        ainiux::editor::AssistCommandKind::Configured,
        *continue_index,
        ainiux::editor::AssistScope::Continue,
        "",
        std::nullopt);
    check(execution.ok, "long /continue continue execution builds");
    check(execution.messages.back().content.size() <
              execution.usage_messages.back().content.size(),
          "/continue continue keeps truncated request text but full-prefix usage messages");
    check(execution.messages.front().content.rfind("Custom system", 0) == 0 &&
              execution.messages.front().content.find(default_continue->prompt) != std::string::npos,
          "user --system is prepended to assist task system prompt");

    ainiux::editor::EditorState markdown_state =
        ainiux::editor::EditorState::from_text("# prose tail");
    markdown_state.cursor = markdown_state.text.size();
    markdown_state.set_language(ainiux::highlight::Language::Markdown, false);
    execution = ainiux::editor::build_assist_execution(
        markdown_state, context, ainiux::editor::AssistCommandKind::Configured,
        *continue_index, ainiux::editor::AssistScope::Continue, "", std::nullopt);
    check(execution.prose_completion && !execution.code_completion &&
              execution.messages.back().content.find("MODE_BYTES 8\nmarkdown\n") !=
                  std::string::npos,
          "Markdown /continue uses dedicated prose continuation framing");

    context.settings.max_prose_prefix_chars = 3;
    context.settings.max_prose_postfix_chars = 4;
    const std::string prose_beta = "\xCE\xB2";
    ainiux::editor::EditorState prose_middle = ainiux::editor::EditorState::from_text(
        std::string("\xCE\xB1") + prose_beta + "AB  ending\xE5\xB0\xBE");
    prose_middle.cursor = std::string("\xCE\xB1").size() + prose_beta.size() + 2;
    execution = ainiux::editor::build_assist_execution(
        prose_middle, context, ainiux::editor::AssistCommandKind::Configured,
        *continue_index, ainiux::editor::AssistScope::Continue, "", std::nullopt);
    const std::string& prose_request = execution.messages.back().content;
    check(prose_request.find("PREFIX_BYTES 4\n" + prose_beta + "AB\nCURSOR_BYTES 9\n<CURSOR/>\n") !=
                  std::string::npos &&
              prose_request.find("POSTFIX_BYTES 4\n  en\n") != std::string::npos,
          "middle prose continuation slices immediate UTF-8 prefix and postfix by characters");
    check(execution.messages.front().content.find("natural bridge") != std::string::npos &&
              execution.messages.front().content.find("immutable POSTFIX") != std::string::npos &&
              execution.messages.front().content.find("Continue at substantial length") ==
                  std::string::npos,
          "middle prose continuation requires a developed bridge without the end-only long-form prompt");

    context.settings.max_prose_postfix_chars = 2;
    execution = ainiux::editor::build_assist_execution(
        prose_middle, context, ainiux::editor::AssistCommandKind::Configured,
        *continue_index, ainiux::editor::AssistScope::Continue, "", std::nullopt);
    check(execution.messages.back().content.find("POSTFIX_BYTES 2\n  \n") !=
              std::string::npos,
          "bounded prose postfix preserves an immediate whitespace-only slice exactly");

    prose_middle.cursor = 0;
    context.settings.max_prose_prefix_chars = 0;
    context.settings.max_prose_postfix_chars = 3;
    execution = ainiux::editor::build_assist_execution(
        prose_middle, context, ainiux::editor::AssistCommandKind::Configured,
        *continue_index, ainiux::editor::AssistScope::Continue, "", std::nullopt);
    check(execution.messages.back().content.find("PREFIX_BYTES 0\n\nCURSOR_BYTES 9") !=
                  std::string::npos &&
              execution.messages.back().content.find(
                  std::string("POSTFIX_BYTES 5\n\xCE\xB1\xCE\xB2") + "A\n") !=
                  std::string::npos,
          "prose continuation at buffer start supports disabled prefix and bounded UTF-8 postfix");

    std::string invalid_prose_source = "A";
    invalid_prose_source.push_back(static_cast<char>(0xFF));
    invalid_prose_source += "Brest";
    ainiux::editor::EditorState invalid_prose_state =
        ainiux::editor::EditorState::from_text(invalid_prose_source);
    invalid_prose_state.cursor = 3;
    context.settings.max_prose_prefix_chars = 2;
    context.settings.max_prose_postfix_chars = 2;
    execution = ainiux::editor::build_assist_execution(
        invalid_prose_state, context, ainiux::editor::AssistCommandKind::Configured,
        *continue_index, ainiux::editor::AssistScope::Continue, "", std::nullopt);
    std::string invalid_prose_prefix;
    invalid_prose_prefix.push_back(static_cast<char>(0xFF));
    invalid_prose_prefix += "B";
    check(execution.messages.back().content.find(
              "PREFIX_BYTES 2\n" + invalid_prose_prefix + "\nCURSOR_BYTES") !=
              std::string::npos,
          "invalid UTF-8 prose bytes are preserved and count as one context unit");

    ainiux::editor::EditorState prose_whitespace_postfix =
        ainiux::editor::EditorState::from_text("abc \t\r\n\f\v");
    prose_whitespace_postfix.cursor = 3;
    context.settings.max_prose_prefix_chars = 3;
    context.settings.max_prose_postfix_chars = 20;
    execution = ainiux::editor::build_assist_execution(
        prose_whitespace_postfix, context, ainiux::editor::AssistCommandKind::Configured,
        *continue_index, ainiux::editor::AssistScope::Continue, "", std::nullopt);
    check(execution.messages.back().content.find("POSTFIX_BYTES") == std::string::npos,
          "complete whitespace-only prose remainder is omitted");
    check(execution.messages.front().content.find("Continue at substantial length") !=
              std::string::npos,
          "whitespace-only remainder uses the end-of-document long-form prose prompt");

    context.settings.max_prose_postfix_chars = 0;
    prose_middle.cursor = 4;
    execution = ainiux::editor::build_assist_execution(
        prose_middle, context, ainiux::editor::AssistCommandKind::Configured,
        *continue_index, ainiux::editor::AssistScope::Continue, "", std::nullopt);
    check(execution.messages.back().content.find("POSTFIX_BYTES") == std::string::npos,
          "zero prose postfix limit omits existing suffix data");
    check(execution.messages.front().content.find("Continue at substantial length") ==
                  std::string::npos &&
              execution.messages.front().content.find("postfix context is disabled") !=
                  std::string::npos,
          "disabled prose postfix in the middle does not trigger the end-of-document prompt");

    context.settings.max_prefix_chars = 3;
    context.settings.max_postfix_chars = 4;
    const std::string beta = "\xCE\xB2";
    ainiux::editor::EditorState python_state =
        ainiux::editor::EditorState::from_text(std::string("\xCE\xB1") + beta + "AB  tail\xE5\xB0\xBE");
    python_state.cursor = std::string("\xCE\xB1").size() + beta.size() + 2;
    python_state.set_language(ainiux::highlight::Language::Python, false);
    python_state.highlight_enabled = false;
    execution = ainiux::editor::build_assist_execution(
        python_state, context, ainiux::editor::AssistCommandKind::Configured,
        *continue_index, ainiux::editor::AssistScope::Continue, "", std::nullopt);
    check(execution.ok && execution.code_completion &&
              execution.completion_language == ainiux::highlight::Language::Python,
          "Python /continue uses code completion even with visual highlighting disabled");
    check(execution.messages.front().content.find("exact python code") != std::string::npos &&
              execution.messages.front().content.find(default_continue->prompt) == std::string::npos,
          "code completion uses the dedicated canonical-language instruction, not the prose prompt");
    const std::string& python_request = execution.messages.back().content;
    check(python_request.find("LANGUAGE python\n") != std::string::npos &&
              python_request.find("PREFIX_BYTES 4\n" + beta + "AB\n<CURSOR/>") !=
                  std::string::npos &&
              python_request.find("POSTFIX_BYTES 4\n  ta\n") != std::string::npos,
          "code completion slices multibyte prefix and immediate postfix by UTF-8 characters");

    context.settings.max_postfix_chars = 2;
    execution = ainiux::editor::build_assist_execution(
        python_state, context, ainiux::editor::AssistCommandKind::Configured,
        *continue_index, ainiux::editor::AssistScope::Continue, "", std::nullopt);
    check(execution.messages.back().content.find("POSTFIX_BYTES 2\n  \n") !=
              std::string::npos,
          "bounded postfix is sent exactly even when its immediate slice is whitespace");

    ainiux::editor::EditorState code_start =
        ainiux::editor::EditorState::from_text("print(value)");
    code_start.cursor = 0;
    code_start.set_language(ainiux::highlight::Language::Python, false);
    context.settings.max_prefix_chars = 5;
    context.settings.max_postfix_chars = 3;
    execution = ainiux::editor::build_assist_execution(
        code_start, context, ainiux::editor::AssistCommandKind::Configured,
        *continue_index, ainiux::editor::AssistScope::Continue, "", std::nullopt);
    check(execution.messages.back().content.find("PREFIX_BYTES 0\n\n<CURSOR/>") !=
                  std::string::npos &&
              execution.messages.back().content.find("POSTFIX_BYTES 3\npri\n") !=
                  std::string::npos,
          "code completion at buffer start sends empty prefix and bounded leading postfix");

    code_start.cursor = code_start.text.size();
    execution = ainiux::editor::build_assist_execution(
        code_start, context, ainiux::editor::AssistCommandKind::Configured,
        *continue_index, ainiux::editor::AssistScope::Continue, "", std::nullopt);
    check(execution.messages.back().content.find("PREFIX_BYTES 5\nalue)") !=
                  std::string::npos &&
              execution.messages.back().content.find("POSTFIX_BYTES") == std::string::npos,
          "code completion at buffer end sends bounded prefix and omits empty postfix");

    std::string invalid_source = "A";
    invalid_source.push_back(static_cast<char>(0xFF));
    invalid_source += "Brest";
    ainiux::editor::EditorState invalid_state =
        ainiux::editor::EditorState::from_text(invalid_source);
    invalid_state.cursor = 3;
    invalid_state.set_language(ainiux::highlight::Language::Python, false);
    context.settings.max_prefix_chars = 2;
    context.settings.max_postfix_chars = 2;
    execution = ainiux::editor::build_assist_execution(
        invalid_state, context, ainiux::editor::AssistCommandKind::Configured,
        *continue_index, ainiux::editor::AssistScope::Continue, "", std::nullopt);
    std::string invalid_prefix;
    invalid_prefix.push_back(static_cast<char>(0xFF));
    invalid_prefix += "B";
    check(execution.messages.back().content.find("PREFIX_BYTES 2\n" + invalid_prefix) !=
              std::string::npos,
          "invalid UTF-8 bytes are preserved and count as one continuation context unit");

    ainiux::editor::EditorState whitespace_postfix =
        ainiux::editor::EditorState::from_text("abc \t\r\n\f\v");
    whitespace_postfix.cursor = 3;
    whitespace_postfix.set_language(ainiux::highlight::Language::Json, false);
    context.settings.max_prefix_chars = 0;
    context.settings.max_postfix_chars = 20;
    execution = ainiux::editor::build_assist_execution(
        whitespace_postfix, context, ainiux::editor::AssistCommandKind::Configured,
        *continue_index, ainiux::editor::AssistScope::Continue, "", std::nullopt);
    check(execution.messages.back().content.find("LANGUAGE json\nPREFIX_BYTES 0\n\n<CURSOR/>") !=
                  std::string::npos &&
              execution.messages.back().content.find("POSTFIX_BYTES") == std::string::npos,
          "disabled prefix is empty and a complete whitespace-only postfix is omitted");

    context.settings.max_postfix_chars = 0;
    execution = ainiux::editor::build_assist_execution(
        python_state, context, ainiux::editor::AssistCommandKind::Configured,
        *continue_index, ainiux::editor::AssistScope::Continue, "", std::nullopt);
    check(execution.messages.back().content.find("POSTFIX_BYTES") == std::string::npos,
          "zero postfix limit omits postfix data in code completion");

    context.settings.max_prefix_chars = ainiux::editor::kDefaultAiContinuePrefixMaxChars;
    context.settings.max_postfix_chars = ainiux::editor::kDefaultAiContinuePostfixMaxChars;
    context.settings.max_prose_prefix_chars =
        ainiux::editor::kDefaultAiContinueProsePrefixMaxChars;
    context.settings.max_prose_postfix_chars =
        ainiux::editor::kDefaultAiContinueProsePostfixMaxChars;

    ainiux::cli::Options configured_options;
    configured_options.editor_assist_config = ainiux::editor::default_editor_assist_config();
    ainiux::config::ParseResult assist_config = ainiux::config::parse(
        "[editor]\nassist_spell = \"Custom spell prompt\"\n", "assist.conf");
    check(assist_config.error.ok(), "editor assist prompt config parses");
    check(ainiux::config::apply_document(assist_config.document, configured_options).ok(),
          "editor assist prompt config applies");
    context.assist_config = configured_options.editor_assist_config;
    const ainiux::editor::EditorAssistCommand* configured_spell =
        ainiux::editor::find_assist_command(context.assist_config, "/spell");
    check(configured_spell != nullptr, "configured assist spell command remains available");
    execution = ainiux::editor::build_assist_execution(
        state,
        context,
        ainiux::editor::AssistCommandKind::Configured,
        *ainiux::editor::assist_command_index(context.assist_config, "/spell"),
        ainiux::editor::AssistScope::Selection,
        "",
        std::nullopt);
    check(execution.messages.front().content.find("Custom spell prompt") != std::string::npos,
          "configured assist_spell overrides the built-in spell prompt");
    check(execution.messages.back().content.find("<content>") == 0,
          "configured assist wraps editor text in content tags");

    ainiux::config::ParseResult custom_continue_config = ainiux::config::parse(
        "[editor]\nassist_continue = \"Carry the mystery forward.\"\n",
        "assist-continue.conf");
    check(custom_continue_config.error.ok() &&
              ainiux::config::apply_document(
                  custom_continue_config.document, configured_options).ok(),
          "custom /continue prompt config applies");
    context.assist_config = configured_options.editor_assist_config;
    ainiux::editor::EditorState custom_continue_state =
        ainiux::editor::EditorState::from_text("The locked door opened.");
    custom_continue_state.cursor = custom_continue_state.text.size();
    execution = ainiux::editor::build_assist_execution(
        custom_continue_state, context, ainiux::editor::AssistCommandKind::Configured,
        *ainiux::editor::assist_command_index(context.assist_config, "/continue"),
        ainiux::editor::AssistScope::Continue, "", std::nullopt);
    check(execution.messages.front().content.find("Carry the mystery forward.") !=
                  std::string::npos &&
              execution.messages.front().content.find("Mandatory continuation rules") !=
                  std::string::npos,
          "custom /continue prompt remains alongside mandatory prose insertion constraints");

    ainiux::config::ParseResult custom_command_config = ainiux::config::parse(
        "[command]\n"
        "string = /example\n"
        "modes = all, selection\n"
        "prompt = \"Output 5 examples of the user-given topic.\"\n",
        "command.conf");
    check(custom_command_config.error.ok(), "repeatable [command] config parses");
    configured_options = ainiux::cli::Options{};
    check(ainiux::config::apply_document(custom_command_config.document, configured_options).ok(),
          "repeatable [command] config applies");
    const ainiux::editor::EditorAssistCommand* example_command =
        ainiux::editor::find_assist_command(configured_options.editor_assist_config, "/example");
    check(example_command != nullptr &&
              example_command->modes.size() == 2 &&
              example_command->prompt.find("5 examples") != std::string::npos,
          "configured [command] block adds a custom editor assist command");
    parsed = ainiux::editor::parse_assist_command("/example all",
                                                  configured_options.editor_assist_config);
    check(parsed.ok && parsed.kind == ainiux::editor::AssistCommandKind::Configured &&
              parsed.scope == ainiux::editor::AssistScope::All,
          "configured custom command parses with scope");

    ainiux::config::ParseResult override_command_config = ainiux::config::parse(
        "[command]\n"
        "string = /spell\n"
        "modes = selection, all\n"
        "prompt = \"Override spell prompt\"\n",
        "override-command.conf");
    check(override_command_config.error.ok(), "configured command override parses");
    configured_options = ainiux::cli::Options{};
    check(ainiux::config::apply_document(override_command_config.document, configured_options).ok(),
          "configured command override applies");
    const ainiux::editor::EditorAssistCommand* overridden_spell =
        ainiux::editor::find_assist_command(configured_options.editor_assist_config, "/spell");
    check(overridden_spell != nullptr && overridden_spell->prompt == "Override spell prompt",
          "configured command with matching string overrides a built-in command");

    ainiux::config::ParseResult insert_modes_config = ainiux::config::parse(
        "[command]\n"
        "string = /expand\n"
        "modes = continue, insert, local_insert\n"
        "prompt = \"Expand the input.\"\n",
        "insert-modes.conf");
    check(insert_modes_config.error.ok(), "configured continue and insert modes parse");
    configured_options = ainiux::cli::Options{};
    check(ainiux::config::apply_document(insert_modes_config.document, configured_options).ok(),
          "configured continue and insert modes apply");
    const ainiux::editor::EditorAssistCommand* expand_command =
        ainiux::editor::find_assist_command(configured_options.editor_assist_config, "/expand");
    check(expand_command != nullptr && expand_command->modes.size() == 3,
          "configured command stores continue and insert modes");

    parsed = ainiux::editor::parse_assist_command("/expand continue",
                                                  configured_options.editor_assist_config);
    check(parsed.ok && parsed.scope == ainiux::editor::AssistScope::Continue,
          "/expand continue parses continue mode");
    parsed = ainiux::editor::parse_assist_command("/expand insert",
                                                  configured_options.editor_assist_config);
    check(parsed.ok && parsed.scope == ainiux::editor::AssistScope::Insert,
          "/expand insert parses insert mode");
    parsed = ainiux::editor::parse_assist_command("/expand l",
                                                  configured_options.editor_assist_config);
    check(parsed.ok && parsed.scope == ainiux::editor::AssistScope::Insert,
          "legacy local_insert scope alias l parses as insert");

    const std::vector<std::string> expand_completions =
        ainiux::editor::assist_command_completions(configured_options.editor_assist_config);
    check(std::find(expand_completions.begin(), expand_completions.end(), "/expand continue") !=
              expand_completions.end() &&
              std::find(expand_completions.begin(), expand_completions.end(), "/expand insert") !=
                  expand_completions.end(),
          "assist completions include continue and insert variants");

    check(ainiux::editor::assist_scope_prompt(*expand_command).find("continue (c)") != std::string::npos &&
              ainiux::editor::assist_scope_prompt(*expand_command).find("insert (i)") !=
                  std::string::npos,
          "assist scope prompt advertises continue and insert keys");

    ainiux::editor::EditorState insert_state =
        ainiux::editor::EditorState::from_text("Once upon a time");
    insert_state.cursor = insert_state.text.size();
    context.assist_config = configured_options.editor_assist_config;
    const std::optional<size_t> expand_index =
        ainiux::editor::assist_command_index(context.assist_config, "/expand");
    check(expand_index.has_value(), "configured /expand command is indexed");
    execution = ainiux::editor::build_assist_execution(
        insert_state,
        context,
        ainiux::editor::AssistCommandKind::Configured,
        *expand_index,
        ainiux::editor::AssistScope::Continue,
        "",
        std::nullopt);
    check(execution.ok && execution.stream &&
              execution.edit_kind == ainiux::editor::AssistEditKind::StreamInsert,
          "continue mode builds streaming execution after the cursor");
    check(execution.messages.back().content == "<content>Once upon a time</content>",
          "continue mode sends tail-before-cursor context as input");

    insert_state.selection.anchor = 5;
    insert_state.selection.active = 9;
    execution = ainiux::editor::build_assist_execution(
        insert_state,
        context,
        ainiux::editor::AssistCommandKind::Configured,
        *expand_index,
        ainiux::editor::AssistScope::Insert,
        "",
        std::nullopt);
    check(execution.ok && execution.stream &&
              execution.edit_kind == ainiux::editor::AssistEditKind::StreamInsert,
          "insert mode builds streaming execution after the cursor");
    check(execution.messages.back().content == "<content>upon</content>",
          "insert mode sends the current selection as input");

    insert_state.clear_selection();
    execution = ainiux::editor::build_assist_execution(
        insert_state,
        context,
        ainiux::editor::AssistCommandKind::Configured,
        *expand_index,
        ainiux::editor::AssistScope::Insert,
        "",
        std::nullopt);
    check(!execution.ok &&
              execution.error_message.find("insert requires an active selection") != std::string::npos,
          "insert mode rejects missing selection");

    ainiux::config::ParseResult newbuffer_modes_config = ainiux::config::parse(
        "[command]\n"
        "string = /summarize\n"
        "modes = selection, all, newbuffer\n"
        "prompt = \"Summarize the input.\"\n",
        "newbuffer-modes.conf");
    check(newbuffer_modes_config.error.ok(), "configured newbuffer mode parses");
    configured_options = ainiux::cli::Options{};
    check(ainiux::config::apply_document(newbuffer_modes_config.document, configured_options).ok(),
          "configured newbuffer mode applies");
    parsed = ainiux::editor::parse_assist_command("/summarize n",
                                                  configured_options.editor_assist_config);
    check(parsed.ok && parsed.scope == ainiux::editor::AssistScope::NewBuffer,
          "/summarize n parses as new buffer");
    insert_state.selection.anchor = 0;
    insert_state.selection.active = 4;
    context.assist_config = configured_options.editor_assist_config;
    execution = ainiux::editor::build_assist_execution(
        insert_state,
        context,
        ainiux::editor::AssistCommandKind::Configured,
        *ainiux::editor::assist_command_index(context.assist_config, "/summarize"),
        ainiux::editor::AssistScope::NewBuffer,
        "",
        std::nullopt);
    check(execution.ok && execution.stream &&
              execution.edit_kind == ainiux::editor::AssistEditKind::NewBuffer,
          "new buffer mode builds streaming execution into a new buffer");
    check(execution.messages.back().content == "<content>Once</content>",
          "new buffer mode sends the current selection as input");
    insert_state.clear_selection();
    execution = ainiux::editor::build_assist_execution(
        insert_state,
        context,
        ainiux::editor::AssistCommandKind::Configured,
        *ainiux::editor::assist_command_index(context.assist_config, "/summarize"),
        ainiux::editor::AssistScope::NewBuffer,
        "",
        std::nullopt);
    check(!execution.ok &&
              execution.error_message.find("new buffer requires an active selection") !=
                  std::string::npos,
          "new buffer mode rejects missing selection");

    check(ainiux::editor::trim_assist_inplace_response("  fixed text \n") == "fixed text",
          "in-place assist responses are trimmed");
    check(ainiux::editor::trim_assist_inplace_response(
              "<think>hidden trace</think>\n\nVisible rewrite") == "Visible rewrite",
          "in-place assist responses drop thinking traces");
    check(ainiux::editor::trim_assist_inplace_response("<content>fixed text</content>") == "fixed text",
          "in-place assist responses strip content tags");
    check(ainiux::editor::trim_assist_inplace_response("plain text without tags") == "plain text without tags",
          "in-place assist responses leave untagged output unchanged");
    check(ainiux::editor::trim_assist_inplace_response("continued text</content>") == "continued text",
          "in-place assist responses strip trailing close tag without open tag");
    check(ainiux::editor::trim_assist_inplace_response("continued text</content></tool_call>") ==
              "continued text",
          "in-place assist responses strip trailing tool-call wrapper artifacts");
    check(ainiux::editor::trim_assist_inplace_response("continued text</content></tool_call>  \n") ==
              "continued text",
          "in-place assist responses strip trailing wrapper artifacts with whitespace");

    {
        ainiux::editor::AssistStreamFilter stream_filter;
        std::string streamed;
        streamed += stream_filter.feed("<content>hello");
        streamed += stream_filter.feed("</content>");
        streamed += stream_filter.finish();
        check(streamed == "hello",
              "streamed assist output strips content wrapper tags");
    }
    {
        ainiux::editor::AssistStreamFilter stream_filter;
        std::string streamed = stream_filter.feed("continued text</content>");
        streamed += stream_filter.finish();
        check(streamed == "continued text",
              "streamed assist output strips trailing close tag without open tag");
    }
    {
        ainiux::editor::AssistStreamFilter stream_filter;
        std::string streamed = stream_filter.feed("hello</cont");
        streamed += stream_filter.feed("ent>");
        streamed += stream_filter.finish();
        check(streamed == "hello",
              "streamed assist output strips a close tag split across chunks");
    }
    {
        ainiux::editor::AssistStreamFilter stream_filter;
        std::string streamed = stream_filter.feed("plain");
        streamed += stream_filter.finish();
        check(streamed == "plain",
              "streamed assist output leaves untagged text unchanged");
    }
    {
        ainiux::editor::AssistStreamFilter stream_filter;
        std::string streamed = stream_filter.feed("continued text</content></tool_call>");
        streamed += stream_filter.finish();
        check(streamed == "continued text",
              "streamed assist output strips trailing tool-call wrapper artifacts");
    }
    {
        ainiux::editor::AssistStreamFilter stream_filter;
        std::string streamed = stream_filter.feed("continued text</content></tool_cal");
        streamed += stream_filter.feed("l>  ");
        streamed += stream_filter.finish();
        check(streamed == "continued text",
              "streamed assist output strips split tool-call wrapper artifacts");
    }

    {
        ainiux::editor::ProseAssistStreamFilter stream_filter;
        const std::string raw = "  immediate prose\n\t";
        std::string streamed = stream_filter.feed(raw.substr(0, 1));
        streamed += stream_filter.feed(raw.substr(1, 7));
        streamed += stream_filter.feed(raw.substr(8));
        streamed += stream_filter.finish();
        check(streamed == raw,
              "raw prose stream preserves leading and trailing whitespace exactly");
    }
    {
        ainiux::editor::ProseAssistStreamFilter stream_filter;
        const std::vector<std::string> chunks = {
            "<co", "ntent> \xE4", "\xBD", "\xA0 bridge\n\t</con", "tent>"};
        std::string streamed;
        for (const std::string& chunk : chunks) {
            streamed += stream_filter.feed(chunk);
        }
        streamed += stream_filter.finish();
        check(streamed == " \xE4\xBD\xA0 bridge\n\t",
              "wrapped prose strips only boundary tags across arbitrary chunks and split UTF-8");
    }
    {
        ainiux::editor::ProseAssistStreamFilter stream_filter;
        const std::string wrapped =
            "<content>keep <content> and </content> inside, then continue</content>";
        std::string streamed;
        for (char byte : wrapped) {
            streamed += stream_filter.feed(std::string(1, byte));
        }
        streamed += stream_filter.finish();
        check(streamed == "keep <content> and </content> inside, then continue",
              "wrapped prose preserves tag-like text inside the body");
    }
    {
        ainiux::editor::ProseAssistStreamFilter stream_filter;
        std::string streamed = stream_filter.feed("<content>partial bridge</con");
        streamed += stream_filter.finish();
        check(streamed == "partial bridge</con",
              "finishing a partial prose stream keeps incomplete wrapper-like output bytes");
    }

    {
        ainiux::editor::CodeAssistStreamFilter stream_filter(
            ainiux::highlight::Language::Python);
        std::string streamed;
        std::string output;
        check(stream_filter.feed("    ret", output).ok(),
              "raw code stream accepts leading indentation");
        streamed += output;
        check(stream_filter.feed("urn 1\n\t", output).ok(),
              "raw code stream accepts later chunks");
        streamed += output;
        check(stream_filter.finish(output).ok(), "raw code stream finishes");
        streamed += output;
        check(streamed == "    return 1\n\t",
              "raw code stream preserves leading indentation and trailing whitespace exactly");
    }
    {
        ainiux::editor::CodeAssistStreamFilter stream_filter(
            ainiux::highlight::Language::Python);
        std::string streamed;
        std::string output;
        const std::vector<std::string> chunks = {
            "\n`", "``py", "thon\r", "\n    value = \"\xE4", "\xBD", "\xA0\"\n  \n`", "``\n"};
        for (const std::string& chunk : chunks) {
            check(stream_filter.feed(chunk, output).ok(),
                  "matching fenced code accepts arbitrary stream chunk boundaries");
            streamed += output;
        }
        check(stream_filter.finish(output).ok(), "matching fenced code stream finishes");
        streamed += output;
        check(streamed == "    value = \"\xE4\xBD\xA0\"\n  \n",
              "matching code fence and optional leading blank are stripped without changing body bytes");
    }
    {
        ainiux::editor::CodeAssistStreamFilter stream_filter(
            ainiux::highlight::Language::Sql);
        std::string output;
        std::string streamed;
        check(stream_filter.feed("```\nSELECT 1;\n```", output).ok(),
              "blank Markdown fence is accepted for code completion");
        streamed += output;
        check(stream_filter.finish(output).ok(), "blank fenced stream finishes");
        streamed += output;
        check(streamed == "SELECT 1;\n", "blank Markdown fence is stripped");
    }
    {
        ainiux::editor::CodeAssistStreamFilter stream_filter(
            ainiux::highlight::Language::Python);
        std::string output;
        const ainiux::Error error = stream_filter.feed("```javascript\nalert(1);\n```", output);
        check(!error.ok() && error.code == ainiux::ErrorCode::ProviderSchema && output.empty(),
              "explicitly mismatched leading code fence is rejected");
    }
    {
        ainiux::editor::CodeAssistStreamFilter stream_filter(
            ainiux::highlight::Language::Cpp);
        const std::string raw = "    const char* fence = \"```\";\n\n";
        std::string output;
        std::string streamed;
        check(stream_filter.feed(raw.substr(0, 3), output).ok(),
              "raw fence-like code prefix begins streaming");
        streamed += output;
        check(stream_filter.feed(raw.substr(3), output).ok(),
              "raw code containing fence-like text is accepted");
        streamed += output;
        check(stream_filter.finish(output).ok(), "raw fence-like code stream finishes");
        streamed += output;
        check(streamed == raw, "fence-like text inside raw code is preserved exactly");
    }

    {
        ainiux::editor::EditorState state = ainiux::editor::EditorState::from_text("hello</content>");
        state.cursor = state.text.size();
        ainiux::editor::strip_trailing_assist_close_tag_without_undo(state);
        check(state.text.str() == "hello",
              "streamed assist post-clear strips trailing close tag from buffer");
        check(!state.can_undo(), "streamed assist post-clear does not create undo history");
    }
    {
        ainiux::editor::EditorState state = ainiux::editor::EditorState::from_text("keep</content>tail");
        state.cursor = state.text.size();
        ainiux::editor::strip_trailing_assist_close_tag_without_undo(state);
        check(state.text.str() == "keep</content>tail",
              "streamed assist post-clear ignores close tag not at insertion tail");
    }
    {
        ainiux::editor::EditorState state =
            ainiux::editor::EditorState::from_text("hello</content></tool_call>  ");
        state.cursor = state.text.size();
        ainiux::editor::strip_trailing_assist_close_tag_without_undo(state);
        check(state.text.str() == "hello",
              "streamed assist post-clear strips trailing tool-call wrapper artifacts");
    }

    const ainiux::provider::RequestContext assist_context =
        ainiux::editor::assist_request_context(context, true);
    check(assist_context.suppress_streaming_reasoning,
          "editor assist suppresses streamed reasoning deltas");
}

void test_editor_contextual_completion_modes() {
    const std::string directory = "build/ainiux-context-completion";
    std::error_code filesystem_error;
    std::filesystem::create_directories(directory, filesystem_error);
    check(!filesystem_error, "contextual completion fixture directory is created");

    {
        std::ofstream fixture(directory + "/ainiux-context-file.txt",
                              std::ios::binary | std::ios::trunc);
        fixture << "context";
        check(static_cast<bool>(fixture), "contextual completion fixture file is written");
    }

    ainiux::editor::ContextualCompleter completer;

    ainiux::editor::EditorState empty = ainiux::editor::EditorState::from_text("");
    empty.mode = ainiux::editor::EditorMode::Chat;
    ainiux::editor::PathCompletionResult result = completer.complete(empty);
    check(!result.handled && empty.text.str().empty(),
          "chat Tab on empty input is ignored");

    ainiux::editor::EditorState editor_path =
        ainiux::editor::EditorState::from_text(directory + "/ainiux-context-fi");
    editor_path.cursor = editor_path.text.size();
    result = completer.complete(editor_path);
    check(!result.handled &&
              editor_path.text.str() == directory + "/ainiux-context-fi",
          "editor-mode Tab does not run generic path completion");

    ainiux::editor::EditorState command = ainiux::editor::EditorState::from_text("/he");
    command.mode = ainiux::editor::EditorMode::Chat;
    command.cursor = command.text.size();
    result = completer.complete(command);
    check(result.handled && result.kind == ainiux::editor::CompletionKind::Command &&
              result.match_count == 1 && command.text.str() == "/help",
          "chat command completion works at the start of the first line");

    completer.reset();
    ainiux::editor::EditorState path_command = ainiux::editor::EditorState::from_text("/in");
    path_command.mode = ainiux::editor::EditorMode::Chat;
    path_command.cursor = path_command.text.size();
    result = completer.complete(path_command);
    check(result.handled && result.kind == ainiux::editor::CompletionKind::Command &&
              path_command.text.str() == "/insert ",
          "chat command completion adds the path-command separator");

    completer.reset();
    ainiux::editor::EditorState provider_command =
        ainiux::editor::EditorState::from_text("/prov");
    provider_command.mode = ainiux::editor::EditorMode::Chat;
    provider_command.cursor = provider_command.text.size();
    result = completer.complete(provider_command);
    check(result.handled && result.kind == ainiux::editor::CompletionKind::Command &&
              provider_command.text.str() == "/provider ",
          "chat command completion includes /provider");

    completer.reset();
    ainiux::editor::EditorState list_command =
        ainiux::editor::EditorState::from_text("/li");
    list_command.mode = ainiux::editor::EditorMode::Chat;
    list_command.cursor = list_command.text.size();
    result = completer.complete(list_command);
    check(result.handled && result.kind == ainiux::editor::CompletionKind::Command &&
              list_command.text.str() == "/list",
          "chat command completion includes /list");

    completer.reset();
    ainiux::editor::EditorState path =
        ainiux::editor::EditorState::from_text("/insert " + directory + "/ainiux-context-fi");
    path.mode = ainiux::editor::EditorMode::Chat;
    path.cursor = path.text.size();
    result = completer.complete(path);
    check(result.handled && result.kind == ainiux::editor::CompletionKind::Path &&
              result.match_count == 1 &&
              path.text.str() == "/insert " + directory + "/ainiux-context-file.txt",
          "chat path completion runs after /insert");

    completer.reset();
    ainiux::editor::EditorState fetch =
        ainiux::editor::EditorState::from_text("/fetch " + directory + "/ainiux-context-fi");
    fetch.mode = ainiux::editor::EditorMode::Chat;
    fetch.cursor = fetch.text.size();
    result = completer.complete(fetch);
    check(!result.handled &&
              fetch.text.str() == "/fetch " + directory + "/ainiux-context-fi",
          "chat path completion ignores non-file commands");

    ainiux::editor::EditorState second_line = ainiux::editor::EditorState::from_text("hello\n/he");
    second_line.mode = ainiux::editor::EditorMode::Chat;
    second_line.cursor = second_line.text.size();
    result = completer.complete(second_line);
    check(!result.handled && second_line.text.str() == "hello\n/he",
          "chat command completion is limited to the first line start");
}

void test_editor_file_round_trip() {
    const std::string path = "build/unit-editor.txt";
    ainiux::editor::PieceTable table = ainiux::editor::PieceTable::from_string("first\nsecond");
    ainiux::Error err = ainiux::editor::save_file(path, table);
    check(err.ok(), "editor file saves");
    ainiux::editor::PieceTable loaded;
    err = ainiux::editor::load_file(path, loaded);
    check(err.ok(), "editor file loads");
    check(loaded.str() == "first\nsecond", "editor file round trip preserves text");

    ainiux::editor::EditorSettings settings;
    settings.huge_file_size_warning = 5;
    settings.file_size_limit = -1;
    ainiux::editor::FileLoadCheck load_check;
    err = ainiux::editor::check_load_file_size(path, settings, load_check);
    check(err.ok() && load_check.size == 12 && load_check.should_warn,
          "editor file size check reports configured huge-file warning");

    settings.file_size_limit = 4;
    err = ainiux::editor::load_file(path, settings, loaded);
    check(!err.ok() && err.message.find("FILE_SIZE_LIMIT") != std::string::npos,
          "editor file load rejects files above the configured size limit");

    settings.file_size_limit = -1;
    err = ainiux::editor::load_file(path, settings, loaded);
    check(err.ok() && loaded.str() == "first\nsecond",
          "editor file load has no configured upper limit when file_size_limit is -1");
}

void test_editor_linebreak_modes() {
    struct Case {
        const char* name;
        const char* bytes;
        ainiux::editor::LineBreak linebreak;
    };
    const Case cases[] = {
        {"lf", "first\nsecond\n", ainiux::editor::LineBreak::Lf},
        {"cr", "first\rsecond\r", ainiux::editor::LineBreak::Cr},
        {"crlf", "first\r\nsecond\r\n", ainiux::editor::LineBreak::Crlf},
    };
    for (const Case& item : cases) {
        const std::string path = std::string("build/unit-editor-linebreak-") + item.name + ".txt";
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(item.bytes, static_cast<std::streamsize>(std::char_traits<char>::length(item.bytes)));
        }
        ainiux::editor::LoadedFile loaded;
        ainiux::editor::EditorSettings settings;
        check(ainiux::editor::load_file(path, settings, loaded).ok(),
              std::string("editor loads ") + item.name + " line endings");
        check(loaded.text.str() == "first\nsecond\n" && loaded.linebreak == item.linebreak &&
                  !loaded.mixed_linebreaks,
              std::string("editor detects and normalizes ") + item.name + " line endings");
        const std::string saved = path + ".saved";
        check(ainiux::editor::save_file(saved, loaded.text, loaded.linebreak).ok(),
              std::string("editor saves ") + item.name + " line endings");
        std::ifstream in(saved, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
        check(bytes == item.bytes,
              std::string("editor round trip preserves ") + item.name + " and final ending");
    }

    const std::string mixed_path = "build/unit-editor-linebreak-mixed.txt";
    {
        std::ofstream out(mixed_path, std::ios::binary | std::ios::trunc);
        out << "one\r\ntwo\nthree\r";
    }
    ainiux::editor::EditorSettings settings;
    settings.linebreak = ainiux::editor::LineBreak::Cr;
    ainiux::editor::LoadedFile mixed;
    check(ainiux::editor::load_file(mixed_path, settings, mixed).ok(),
          "editor loads mixed line endings");
    check(mixed.mixed_linebreaks && mixed.linebreak == ainiux::editor::LineBreak::Cr &&
              mixed.text.str() == "one\ntwo\nthree\n",
          "mixed line endings normalize and use the configured default");

    const std::string no_ending_path = "build/unit-editor-no-linebreak.txt";
    {
        std::ofstream out(no_ending_path, std::ios::binary | std::ios::trunc);
        out << "no final ending";
    }
    settings.linebreak = ainiux::editor::LineBreak::Crlf;
    ainiux::editor::LoadedFile no_ending;
    check(ainiux::editor::load_file(no_ending_path, settings, no_ending).ok() &&
              no_ending.linebreak == ainiux::editor::LineBreak::Crlf &&
              no_ending.text.str() == "no final ending",
          "file without a line ending inherits the configured default without adding one");
}

void test_editor_indentation_detection() {
    using ainiux::editor::IndentationDetection;
    using ainiux::editor::TabStyle;

    IndentationDetection detected = ainiux::editor::detect_indentation(
        "function run() {\n  if (ready) {\n    call();\n  }\n}", 4, TabStyle::Spaces);
    check(detected.tab_width_detected && detected.tab_width == 2 &&
              detected.tab_style_detected && detected.tab_style == TabStyle::Spaces,
          "editor detects a consistent two-space indentation step");

    detected = ainiux::editor::detect_indentation(
        "if (ready) {\n    while (open) {\n        call();\n    }\n}", 2, TabStyle::Tab);
    check(detected.tab_width_detected && detected.tab_width == 4 &&
              detected.tab_style_detected && detected.tab_style == TabStyle::Spaces,
          "editor detects a consistent four-space indentation step");

    detected = ainiux::editor::detect_indentation(
        "if ready\n\tcall\nend", 8, TabStyle::Spaces);
    check(!detected.tab_width_detected && detected.tab_width == 8 &&
              detected.tab_style_detected && detected.tab_style == TabStyle::Tab,
          "editor detects tab indentation while retaining the fallback display width");

    detected = ainiux::editor::detect_indentation(
        "top\n  child\n     inconsistent", 6, TabStyle::Tab);
    check(!detected.tab_width_detected && detected.tab_width == 6 &&
              detected.tab_style == TabStyle::Spaces,
          "ambiguous indentation steps retain the configured width");

    detected = ainiux::editor::detect_indentation("const value = 1;", 7, TabStyle::Tab);
    check(!detected.tab_width_detected && !detected.tab_style_detected &&
              detected.tab_width == 7 && detected.tab_style == TabStyle::Tab,
          "one-line files retain configured indentation defaults");

    std::string after_limit;
    for (size_t line = 0; line < 20; ++line) {
        after_limit += "top_level();\n";
    }
    after_limit += "  ignored_after_limit();\n";
    detected = ainiux::editor::detect_indentation(after_limit, 5, TabStyle::Spaces);
    check(!detected.tab_width_detected && detected.tab_width == 5,
          "indentation detection inspects only the first twenty physical lines");

    ainiux::editor::EditorSettings settings;
    settings.tab_width = 4;
    settings.tab_style = TabStyle::Spaces;
    ainiux::editor::LoadedFile javascript;
    check(ainiux::editor::load_file("tests/highlight/javascript_file.js", settings, javascript).ok() &&
              javascript.tab_width_detected && javascript.tab_width == 2 &&
              javascript.tab_style_detected && javascript.tab_style == TabStyle::Spaces,
          "loading the JavaScript fixture initializes its detected two-space indentation");

    const std::string one_line_path = "build/unit-editor-indent-fallback.js";
    {
        std::ofstream out(one_line_path, std::ios::binary | std::ios::trunc);
        out << "const compact = true;";
    }
    settings.tab_width = 6;
    settings.tab_style = TabStyle::Tab;
    ainiux::editor::LoadedFile fallback;
    check(ainiux::editor::load_file(one_line_path, settings, fallback).ok() &&
              !fallback.tab_width_detected && !fallback.tab_style_detected &&
              fallback.tab_width == 6 && fallback.tab_style == TabStyle::Tab,
          "file loading retains configured indentation when detection is inconclusive");
}

void test_editor_tab_indentation() {
    ainiux::editor::EditorState state = ainiux::editor::EditorState::from_text("ab");
    state.tab_width = 4;
    state.cursor = 2;
    check(state.indent().ok() && state.text.str() == "ab  " && state.cursor == 4,
          "space Tab advances to the next configured tab stop");
    check(state.undo() && state.text.str() == "ab", "single-position Tab is one undo step");

    state = ainiux::editor::EditorState::from_text("ab");
    state.tab_style = ainiux::editor::TabStyle::Tab;
    state.cursor = 1;
    check(state.indent().ok() && state.text.str() == "a\tb",
          "tab style inserts one literal tab at the cursor");

    state = ainiux::editor::EditorState::from_text("a\nb\nc");
    state.tab_width = 2;
    state.selection.anchor = 0;
    state.selection.active = state.text.line_start(2);
    state.cursor = state.selection.active;
    check(state.indent().ok() && state.text.str() == "  a\n  b\nc",
          "block Tab indents every selected line and excludes a following column-zero line");
    check(state.selection.anchor == 0 && state.selection.active == state.text.line_start(2),
          "block Tab preserves a forward selection over the transformed block");
    check(state.undo() && state.text.str() == "a\nb\nc",
          "arbitrary block indentation is one undo step");

    state = ainiux::editor::EditorState::from_text("a\nb\nc");
    state.tab_width = 2;
    state.selection.anchor = state.text.line_start(2);
    state.selection.active = 0;
    state.cursor = 0;
    check(state.indent().ok() && state.text.str() == "  a\n  b\nc" &&
              state.selection.anchor == state.text.line_start(2) &&
              state.selection.active == 0,
          "block Tab preserves reverse selection direction");

    state = ainiux::editor::EditorState::from_text("\t  alpha\n  \tbeta");
    state.tab_width = 4;
    state.selection.anchor = 0;
    state.selection.active = state.text.size();
    state.cursor = state.text.size();
    check(state.outdent().ok() && state.text.str() == "\talpha\nbeta",
          "block Shift+Tab removes one display indentation level from mixed whitespace");
    check(state.undo() && state.text.str() == "\t  alpha\n  \tbeta",
          "block outdent is one undo step");

    state = ainiux::editor::EditorState::from_text("    alpha");
    state.tab_width = 4;
    state.cursor = state.text.size();
    check(state.outdent().ok() && state.text.str() == "alpha" && state.cursor == 5,
          "Shift+Tab without a selection outdents the current line and preserves cursor content position");

    const ainiux::editor::PieceTable tabs =
        ainiux::editor::PieceTable::from_string("\tx");
    check(tabs.display_column_for_offset(1, 8) == 8 &&
              tabs.offset_for_line_column(0, 8, 8) == 1,
          "editor display columns honor the active tab width");

    std::string large_text;
    for (size_t i = 0; i < 5000; ++i) {
        large_text += "line\n";
    }
    state = ainiux::editor::EditorState::from_text(large_text);
    state.tab_width = 2;
    state.selection.anchor = 0;
    state.selection.active = state.text.size();
    state.cursor = state.text.size();
    check(state.indent().ok() && state.text.str().size() == large_text.size() + 10000,
          "large selected blocks indent in one bounded transformation");
    check(state.undo() && state.text.str() == large_text,
          "large block indentation remains one undo step");
}

void test_editor_word_completion() {
    std::vector<ainiux::editor::EditorState> buffers;
    buffers.push_back(ainiux::editor::EditorState::from_text("win"));
    buffers.push_back(
        ainiux::editor::EditorState::from_text("windowHeight windowWidth"));
    buffers[0].cursor = buffers[0].text.size();

    ainiux::editor::WordCompleter completer;
    ainiux::editor::WordCompletionResult result = completer.complete(buffers[0], buffers, 0);
    check(result.error.ok() && result.completed && result.match_count == 2,
          "document Tab finds words across all open buffers");
    check(buffers[0].text.str() == "window",
          "first document Tab inserts the candidates' longest common prefix");

    result = completer.complete(buffers[0], buffers, 0);
    check(result.completed && result.cycling && buffers[0].text.str() == "windowHeight",
          "second document Tab selects the first full candidate");
    result = completer.complete(buffers[0], buffers, 0);
    check(result.completed && buffers[0].text.str() == "windowWidth",
          "third document Tab rotates to the next full candidate");
    result = completer.complete(buffers[0], buffers, 0);
    check(result.completed && buffers[0].text.str() == "windowHeight",
          "document Tab candidate cycling wraps");
    check(buffers[0].undo() && buffers[0].text.str() == "win",
          "one document completion session is one undo operation");

    buffers[0] = ainiux::editor::EditorState::from_text("Win");
    buffers[0].cursor = buffers[0].text.size();
    buffers[1] =
        ainiux::editor::EditorState::from_text("WindowHeight windowWidth WindowSize");
    completer.reset();
    result = completer.complete(buffers[0], buffers, 0);
    check(result.completed && result.match_count == 2 && buffers[0].text.str() == "Window",
          "an uppercase prefix enables case-sensitive smart-case matching");

    buffers[0] = ainiux::editor::EditorState::from_text("ä");
    buffers[0].cursor = buffers[0].text.size();
    buffers[1] = ainiux::editor::EditorState::from_text("Äiti");
    completer.reset();
    result = completer.complete(buffers[0], buffers, 0);
    check(result.completed && buffers[0].text.str() == "Äiti",
          "lowercase Unicode prefixes use full case-folded matching");

    buffers[0] = ainiux::editor::EditorState::from_text("stras");
    buffers[0].cursor = buffers[0].text.size();
    buffers[1] = ainiux::editor::EditorState::from_text("Straße");
    completer.reset();
    result = completer.complete(buffers[0], buffers, 0);
    check(result.completed && buffers[0].text.str() == "Straße",
          "document completion applies expanding Unicode full case folds");

    struct MultilingualCase {
        std::string prefix;
        std::string candidate;
    };
    const std::vector<MultilingualCase> multilingual = {
        {"你好", "你好世界"},
        {"مرح", "مرحباكم"},
        {"при", "Приветствие"},
        {"e\xCC\x81", "e\xCC\x81" "clair"},
        {"foo_", "foo_bar"},
    };
    for (const MultilingualCase& test : multilingual) {
        buffers[0] = ainiux::editor::EditorState::from_text(test.prefix);
        buffers[0].cursor = buffers[0].text.size();
        buffers[1] = ainiux::editor::EditorState::from_text(test.candidate);
        completer.reset();
        result = completer.complete(buffers[0], buffers, 0);
        check(result.completed && buffers[0].text.str() == test.candidate,
              "document completion supports Unicode letters, marks, and underscore: " +
                  test.prefix);
    }

    buffers[0] = ainiux::editor::EditorState::from_text("wo");
    buffers[0].cursor = buffers[0].text.size();
    buffers[1] = ainiux::editor::EditorState::from_text(
        std::string("bad") + static_cast<char>(0xFF) + "word");
    completer.reset();
    result = completer.complete(buffers[0], buffers, 0);
    check(result.completed && buffers[0].text.str() == "word",
          "invalid UTF-8 bytes are preserved and treated as word boundaries");

    buffers[0] = ainiux::editor::EditorState::from_text("win");
    buffers[0].cursor = buffers[0].text.size();
    buffers[1] = ainiux::editor::EditorState::from_text("windowWidth");
    check(buffers[1].completion_word_index().occurrence_count("windowWidth") == 1,
          "per-buffer word index records occurrence counts");
    ainiux::editor::EditorState edited_buffer = buffers[1];
    check(edited_buffer.replace(0, std::string("windowWidth").size(), "paneWidth").ok(),
          "indexed buffer edit succeeds");
    const ainiux::editor::WordIndex& edited_index =
        edited_buffer.completion_word_index();
    check(edited_index.occurrence_count("windowWidth") == 0 &&
              edited_index.occurrence_count("paneWidth") == 1,
          "ordinary edits update only the affected indexed word window");
    check(buffers[1].completion_word_index().occurrence_count("windowWidth") == 1,
          "copy-on-write indexes keep copied editor buffers isolated");
    buffers[1] = std::move(edited_buffer);
    completer.reset();
    result = completer.complete(buffers[0], buffers, 0);
    check(!result.completed,
          "removed cross-buffer words disappear from completion without a stale match");

    buffers[0] = ainiux::editor::EditorState::from_text("windowHeight");
    buffers[0].cursor = 3;
    buffers.resize(1);
    completer.reset();
    result = completer.complete(buffers[0], buffers, 0);
    check(!result.completed,
          "document completion excludes the occurrence currently being edited");

    buffers[0].selection.anchor = 0;
    buffers[0].selection.active = 3;
    result = completer.complete(buffers[0], buffers, 0);
    check(!result.completed,
          "selected document blocks stay in the indentation domain, not word completion");

    std::string large;
    for (size_t index = 0; index < 50000; ++index) {
        large += "symbol_" + std::to_string(index) + ' ';
    }
    ainiux::editor::EditorState large_buffer =
        ainiux::editor::EditorState::from_text(std::move(large));
    const ainiux::editor::WordIndex& large_index =
        large_buffer.completion_word_index();
    check(large_index.unique_word_count() == 50000,
          "large buffers build a deduplicated ordered word index");
    std::map<std::string, size_t> large_matches;
    large_index.append_matches("symbol_4999", true, large_matches);
    check(large_matches.size() == 10,
          "large-buffer prefix lookup uses the ordered index and returns the bounded range");
}

void test_editor_language_reformatting() {
    using ainiux::editor::EditorState;
    using ainiux::editor::ReformatRequest;
    using ainiux::editor::ReformatResult;
    using ainiux::highlight::Language;

    const std::vector<Language> brace_languages = {
        Language::C,          Language::Cpp,        Language::CSharp,
        Language::Java,       Language::JavaScript, Language::TypeScript,
        Language::Css,        Language::Json,       Language::Php,
        Language::Perl,       Language::Rust,       Language::Go,
        Language::PowerShell,
    };
    for (Language language : brace_languages) {
        ReformatRequest request;
        request.content = "if (ready) {\nvalue();\n}\n";
        request.language = language;
        request.tab_width = 4;
        request.first_line = 0;
        request.last_line = 3;
        const ReformatResult result = ainiux::editor::reformat_indentation(request);
        check(result.error.ok() && result.replacement ==
                  "if (ready) {\n    value();\n}\n",
              std::string("brace reformat profile works for ") +
                  ainiux::highlight::language_name(language));
    }
    ReformatRequest tab_request;
    tab_request.content = "if (ready) {\nvalue();\n}";
    tab_request.language = Language::Cpp;
    tab_request.tab_style = ainiux::editor::TabStyle::Tab;
    tab_request.first_line = 0;
    tab_request.last_line = 2;
    check(ainiux::editor::reformat_indentation(tab_request).replacement ==
              "if (ready) {\n\tvalue();\n}",
          "language reformat honors literal-tab indentation style");

    struct ReformatCase {
        Language language;
        std::string input;
        std::string expected;
    };
    const std::vector<ReformatCase> cases = {
        {Language::Ruby,
         "if ready\nputs value\nelse\nputs other\nend\nitems.each do\nputs value\nend",
         "if ready\n    puts value\nelse\n    puts other\nend\nitems.each do\n    puts value\nend"},
        {Language::Bash,
         "if ready; then\necho yes\nelse\necho no\nfi",
         "if ready; then\n    echo yes\nelse\n    echo no\nfi"},
        {Language::Html,
         "<main>\n<script>\nfunction run() {\ncall();\n}\n</script>\n</main>",
         "<main>\n    <script>\n        function run() {\n            call();\n        }\n    </script>\n</main>"},
        {Language::HtmlOnly,
         "<main>\n<br>\n<span>x</span>\n</main>",
         "<main>\n    <br>\n    <span>x</span>\n</main>"},
        {Language::Xml,
         "<root>\n<item>text</item>\n</root>",
         "<root>\n    <item>text</item>\n</root>"},
        {Language::Sql,
         "BEGIN\nSELECT CASE\nWHEN ready THEN value\nEND\nEND",
         "BEGIN\n    SELECT CASE\n        WHEN ready THEN value\n    END\nEND"},
        {Language::Python,
         "if ready:\n      call()\n        nested()\nnext_call()",
         "if ready:\n    call()\n        nested()\nnext_call()"},
        {Language::Yaml,
         "root:\n\tchild:\n\t\tvalue: yes",
         "root:\n    child:\n        value: yes"},
        {Language::Assembly,
         "start:\nmov ax, bx\nnext:\nret",
         "start:\n    mov ax, bx\nnext:\n    ret"},
        {Language::Toml,
         "[table]\n      value = 1",
         "[table]\n    value = 1"},
        {Language::Ini,
         "[section]\n      value=yes",
         "[section]\n    value=yes"},
    };
    for (const ReformatCase& test : cases) {
        ReformatRequest request;
        request.content = test.input;
        request.language = test.language;
        request.first_line = 0;
        request.last_line = ainiux::highlight::split_lines(test.input).size() - 1;
        const ReformatResult result = ainiux::editor::reformat_indentation(request);
        check(result.error.ok() && result.replacement == test.expected,
              std::string("language reformat profile works for ") +
                  ainiux::highlight::language_name(test.language));
    }

    ReformatRequest protected_request;
    protected_request.content =
        "if (ready) {\nconst char *text = \"}\"; // {\ncall();\n}";
    protected_request.language = Language::Cpp;
    protected_request.first_line = 0;
    protected_request.last_line = 3;
    ReformatResult protected_result =
        ainiux::editor::reformat_indentation(protected_request);
    check(protected_result.error.ok() && protected_result.replacement ==
              "if (ready) {\n    const char *text = \"}\"; // {\n    call();\n}",
          "reformat ignores braces inside strings and comments");

    ReformatRequest markdown_request;
    markdown_request.content = "```cpp\n   if (x) {\n bad();\n   }\n```";
    markdown_request.language = Language::Markdown;
    markdown_request.first_line = 0;
    markdown_request.last_line = 4;
    const ReformatResult markdown_result =
        ainiux::editor::reformat_indentation(markdown_request);
    check(markdown_result.error.ok() && markdown_result.replacement == markdown_request.content,
          "reformat preserves Markdown fenced-code contents exactly");

    ReformatRequest pathological_request;
    pathological_request.content =
        std::string(ainiux::highlight::kMaximumHighlightedLineBytes + 1, 'x') +
        "\n  preserve_after_unsafe_line();";
    pathological_request.language = Language::Cpp;
    pathological_request.first_line = 0;
    pathological_request.last_line = 1;
    const ReformatResult pathological_result =
        ainiux::editor::reformat_indentation(pathological_request);
    check(pathological_result.error.ok() && !pathological_result.warning.empty() &&
              pathological_result.replacement == pathological_request.content,
          "reformat safely preserves a region after an unclassifiable pathological line");

    EditorState state = EditorState::from_text("if (ready) {\ncall();\n}\nafter();");
    state.set_language(Language::Cpp, false);
    state.selection.anchor = 0;
    state.selection.active = state.text.line_start(3);
    state.cursor = state.selection.active;
    ReformatRequest selected_request;
    check(ainiux::editor::build_reformat_request(state, false, selected_request).ok() &&
              selected_request.first_line == 0 && selected_request.last_line == 2,
          "selected reformat expands touched lines and excludes a following column-zero line");
    ReformatResult selected_result =
        ainiux::editor::reformat_indentation(selected_request);
    check(ainiux::editor::apply_reformat_result(state, selected_result, false).ok() &&
              state.text.str() == "if (ready) {\n    call();\n}\nafter();" &&
              state.selection.has_range(),
          "selected reformat applies one leading-whitespace replacement and keeps the block selected");
    check(state.undo() && state.text.str() == "if (ready) {\ncall();\n}\nafter();",
          "selected language reformat is one undo operation");

    state = EditorState::from_text("if (ready) {\ncall();\n}");
    state.set_language(Language::Cpp, false);
    state.cursor = state.text.line_start(1) + 2;
    ReformatRequest all_request;
    check(ainiux::editor::build_reformat_request(state, true, all_request).ok(),
          "reformat-all request accepts an unselected buffer");
    const ReformatResult all_result = ainiux::editor::reformat_indentation(all_request);
    check(ainiux::editor::apply_reformat_result(state, all_result, true).ok() &&
              !state.selection.has_range() && state.text.line_for_offset(state.cursor) == 1,
          "reformat-all preserves the logical cursor line and clears selection");

    state = EditorState::from_text("plain text");
    ReformatRequest invalid_request;
    check(!ainiux::editor::build_reformat_request(state, false, invalid_request).ok(),
          "/reformat without a selection reports an actionable error");
    check(ainiux::editor::build_reformat_request(state, true, invalid_request).ok() &&
              !ainiux::editor::reformat_indentation(invalid_request).error.ok(),
          "text mode reformat reports unsupported mode instead of guessing");

    ainiux::runtime::CancellationSource cancellation;
    cancellation.cancel();
    invalid_request.language = Language::Cpp;
    check(ainiux::editor::reformat_indentation(invalid_request, cancellation.token()).error.code ==
              ainiux::ErrorCode::Cancelled,
          "language reformat observes cancellation before processing the buffer");

    ainiux::runtime::EventQueue<ainiux::editor::ReformatEvent> events;
    ainiux::runtime::JobHandle job;
    ReformatRequest async_request;
    async_request.content = "if (ready) {\ncall();\n}";
    async_request.language = Language::Cpp;
    async_request.first_line = 0;
    async_request.last_line = 2;
    ainiux::editor::start_reformat_job(async_request, events, job);
    ainiux::editor::ReformatEvent event;
    check(events.wait_pop_for(event, std::chrono::seconds(2)) && event.result.error.ok() &&
              event.result.replacement == "if (ready) {\n    call();\n}",
          "language reformat runs through the cancellable runtime job queue");
    job.join();

    std::string large;
    large.reserve(300000);
    for (size_t index = 0; index < 6000; ++index) {
        large += "if (ready) {\ncall();\n}\n";
    }
    ReformatRequest large_request;
    large_request.content = std::move(large);
    large_request.language = Language::Cpp;
    large_request.first_line = 0;
    large_request.last_line =
        ainiux::highlight::split_lines(large_request.content).size() - 1;
    const ReformatResult large_result = ainiux::editor::reformat_indentation(large_request);
    check(large_result.error.ok() && large_result.replacement.size() >= large_request.content.size(),
          "large language reformat runs as one linear transformation");

    EditorState revision_state = EditorState::from_text("value");
    const std::uint64_t identity = revision_state.buffer_id();
    const std::uint64_t revision = revision_state.revision();
    EditorState copied_state = revision_state;
    check(copied_state.buffer_id() == identity && copied_state.revision() == revision &&
              copied_state.insert("x").ok() && copied_state.revision() != revision &&
              revision_state.revision() == revision,
          "buffer identity survives editor handoffs while revisions detect stale reformat input");
}

void test_editor_home_end_navigation() {
    ainiux::editor::EditorState state =
        ainiux::editor::EditorState::from_text("alpha\nbeta\ngamma");
    state.cursor = state.text.line_start(2);
    state.scroll_line = 4;
    state.preferred_column = 3;

    state.move_home();
    check(state.cursor == 0, "editor Home moves to the beginning of the buffer");
    check(state.scroll_line == 0, "editor Home scrolls to the top of the buffer");
    check(state.preferred_column == 0, "editor Home resets the preferred column");

    state.move_end();
    check(state.cursor == state.text.size(), "editor End moves to the end of the buffer");
}

void test_editor_invalid_utf8_rendering_is_sanitized() {
    std::string invalid = "A";
    invalid.push_back(static_cast<char>(0xFF));
    invalid.push_back(static_cast<char>(0xE2));
    invalid.push_back(static_cast<char>(0x82));
    invalid += "B";

    ainiux::editor::EditorState state =
        ainiux::editor::EditorState::from_text(invalid);
    ainiux::editor::RenderedPanel rendered = state.render({1, 1, 1, 8});
    check(rendered.lines.size() == 1, "editor invalid UTF-8 render produces a row");
    check(rendered.lines[0].find(static_cast<char>(0xFF)) == std::string::npos,
          "editor render does not emit raw invalid 0xFF bytes to the terminal");
    check(rendered.lines[0].find(std::string() + static_cast<char>(0xE2) +
                                 static_cast<char>(0x82)) == std::string::npos,
          "editor render does not emit raw truncated UTF-8 bytes to the terminal");
    check(rendered.lines[0].find('?') != std::string::npos,
          "editor render replaces invalid UTF-8 with a visible placeholder");
}

void test_editor_kill_to_line_end() {
    ainiux::editor::Clipboard clipboard;
    ainiux::editor::EditorState state = ainiux::editor::EditorState::from_text("alpha beta\ngamma");
    state.cursor = state.text.offset_for_line_column(0, 6);
    ainiux::Error err = state.kill_to_line_end(clipboard);
    check(err.ok(), "editor kill to line end succeeds");
    check(state.text.str() == "alpha \ngamma", "editor kill to line end erases text before newline only");
    check(state.cursor == state.text.offset_for_line_column(0, 6), "editor kill to line end keeps cursor in place");
    check(state.dirty, "editor kill to line end marks dirty after deleting text");
    check(clipboard.text() == "beta", "editor kill to line end copies killed text to clipboard");

    err = state.paste(clipboard);
    check(err.ok(), "editor paste after kill succeeds");
    check(state.text.str() == "alpha beta\ngamma", "editor paste after kill restores killed text");

    clipboard.clear();
    err = state.kill_to_line_end(clipboard);
    check(err.ok(), "editor kill at end of line succeeds");
    check(state.text.str() == "alpha beta\ngamma", "editor kill at end of non-empty line leaves newline intact");
    check(clipboard.empty(), "editor kill at end of non-empty line does not change clipboard");

    ainiux::editor::EditorState middle = ainiux::editor::EditorState::from_text("alpha\n\ngamma");
    middle.cursor = middle.text.line_start(1);
    err = middle.kill_to_line_end(clipboard);
    check(err.ok(), "editor kill empty middle line succeeds");
    check(middle.text.str() == "alpha\ngamma", "editor kill empty middle line removes that line");
    check(middle.cursor == middle.text.line_start(1), "editor kill empty middle line keeps cursor at next line start");
    check(clipboard.text() == "\n", "editor kill empty middle line copies newline to clipboard");

    ainiux::editor::EditorState last = ainiux::editor::EditorState::from_text("alpha\n");
    last.cursor = last.text.line_start(1);
    err = last.kill_to_line_end(clipboard);
    check(err.ok(), "editor kill empty final line succeeds");
    check(last.text.str() == "alpha", "editor kill empty final line removes preceding newline");
    check(last.cursor == last.text.size(), "editor kill empty final line moves cursor to new end");
    check(clipboard.text() == "\n", "editor kill empty final line copies newline to clipboard");

    ainiux::editor::EditorState only = ainiux::editor::EditorState::from_text("");
    err = only.kill_to_line_end(clipboard);
    check(err.ok(), "editor kill single empty buffer succeeds");
    check(only.text.str().empty(), "editor kill single empty buffer is a no-op");
}

void test_editor_movement_sequence_parse() {
    ainiux::editor::MovementKeyEvent event;
    check(ainiux::editor::parse_movement_sequence("[D", event) && !event.shift && !event.alt &&
              event.key == ainiux::editor::MovementKey::Left,
          "left arrow sequence parses");
    check(ainiux::editor::parse_movement_sequence("[1;2C", event) && event.shift && !event.alt &&
              event.key == ainiux::editor::MovementKey::Right,
          "shift right arrow sequence parses");
    check(ainiux::editor::parse_movement_sequence("[5;2~", event) && event.shift && !event.alt &&
              event.key == ainiux::editor::MovementKey::PageUp,
          "shift page up sequence parses");
    check(ainiux::editor::parse_movement_sequence("[1;3H", event) && !event.shift && event.alt &&
              !event.ctrl && event.key == ainiux::editor::MovementKey::Home,
          "alt home sequence parses");
    check(ainiux::editor::parse_movement_sequence("[1;5H", event) && !event.shift && !event.alt &&
              event.ctrl && event.key == ainiux::editor::MovementKey::Home,
          "ctrl home sequence parses");
    check(ainiux::editor::parse_movement_sequence("[1;5F", event) && !event.shift && !event.alt &&
              event.ctrl && event.key == ainiux::editor::MovementKey::End,
          "ctrl end sequence parses");
    check(ainiux::editor::parse_movement_sequence("[1;3~", event) && !event.shift && event.alt &&
              !event.ctrl && event.key == ainiux::editor::MovementKey::Home,
          "alt home tilde sequence parses");
    check(ainiux::editor::parse_movement_sequence("[1;5~", event) && !event.shift && !event.alt &&
              event.ctrl && event.key == ainiux::editor::MovementKey::Home,
          "ctrl home tilde sequence parses");
    check(ainiux::editor::parse_movement_sequence("[4;3~", event) && !event.shift && event.alt &&
              !event.ctrl && event.key == ainiux::editor::MovementKey::End,
          "alt end tilde sequence parses");
    check(ainiux::editor::parse_movement_sequence("[5;3~", event) && !event.shift && event.alt &&
              !event.ctrl && event.key == ainiux::editor::MovementKey::PageUp,
          "alt page up sequence parses");
    check(ainiux::editor::parse_movement_sequence("[6;3~", event) && !event.shift && event.alt &&
              !event.ctrl && event.key == ainiux::editor::MovementKey::PageDown,
          "alt page down sequence parses");
    check(ainiux::editor::parse_movement_sequence("[57362;3u", event) && !event.shift && event.alt &&
              !event.ctrl && event.key == ainiux::editor::MovementKey::PageUp,
          "kitty Alt+PageUp sequence parses");
    check(ainiux::editor::parse_movement_sequence("[57360;3u", event) && !event.shift && event.alt &&
              event.key == ainiux::editor::MovementKey::Home,
          "kitty Alt+Home sequence parses");
}

void test_editor_select_all() {
    ainiux::editor::EditorState state =
        ainiux::editor::EditorState::from_text("alpha\nbeta\ngamma");
    state.cursor = state.text.offset_for_line_column(1, 2);

    state.select_all();
    check(state.selection.has_range(), "select all creates a range selection");
    check(state.selection.start() == 0, "select all starts at the beginning of the buffer");
    check(state.selection.end() == state.text.size(), "select all ends at the end of the buffer");
    check(state.cursor == state.text.size(), "select all moves the cursor to the end of the buffer");
    check(state.selected_text() == "alpha\nbeta\ngamma", "select all selects the full buffer");
}

void test_editor_line_home_end_navigation() {
    ainiux::editor::EditorState state =
        ainiux::editor::EditorState::from_text("alpha\nbeta\ngamma");
    state.cursor = state.text.offset_for_line_column(1, 2);
    const ainiux::editor::Rect rect{1, 1, 10, 40};

    state.apply_movement(ainiux::editor::MovementKey::Home, rect, false, false);
    check(state.cursor == state.text.line_start(1), "editor Home moves to the beginning of the current line");
    check(state.preferred_column == 0, "editor Home resets the preferred column to the line start");

    state.apply_movement(ainiux::editor::MovementKey::End, rect, false, false);
    check(state.cursor == state.text.line_start(1) + state.text.line_length(1),
          "editor End moves to the end of the current line");

    state.apply_movement(ainiux::editor::MovementKey::Home, rect, false, false, true);
    check(state.cursor == 0, "editor Ctrl+Home moves to the beginning of the buffer");
    check(state.scroll_line == 0, "editor Ctrl+Home scrolls to the top of the buffer");

    state.apply_movement(ainiux::editor::MovementKey::End, rect, false, false, true);
    check(state.cursor == state.text.size(), "editor Ctrl+End moves to the end of the buffer");
}

void test_editor_wrapped_line_home_end_navigation() {
    ainiux::editor::EditorState state =
        ainiux::editor::EditorState::from_text("abcdefghij\nXYZ");
    const ainiux::editor::Rect rect{1, 1, 10, 4};
    state.cursor = state.text.line_start(0) + 6;

    state.apply_movement(ainiux::editor::MovementKey::Home, rect, false, false);
    check(state.cursor == state.text.line_start(0) + 4,
          "editor Home moves to the beginning of the current wrapped row");

    state.apply_movement(ainiux::editor::MovementKey::End, rect, false, false);
    check(state.cursor == state.text.line_start(0) + 8,
          "editor End moves to the end of the current wrapped row");

    state.cursor = state.text.line_start(0) + 2;
    state.apply_movement(ainiux::editor::MovementKey::End, rect, false, false);
    check(state.cursor == state.text.line_start(0) + 4,
          "editor End on the first wrapped row stops at that row boundary");
}

void test_editor_page_navigation() {
    ainiux::editor::EditorState state =
        ainiux::editor::EditorState::from_text("zero\none\ntwo\nthree\nfour\nfive");
    ainiux::editor::Rect rect{1, 1, 2, 20};
    state.cursor = state.text.line_start(5);
    state.preferred_column = 0;

    state.page_up(rect);
    check(state.text.line_for_offset(state.cursor) == 3,
          "editor PageUp moves by the editor panel height");
    check(state.scroll_line == 2,
          "editor PageUp scrolls the editor window to keep the cursor visible");

    state.page_down(rect);
    check(state.text.line_for_offset(state.cursor) == 5,
          "editor PageDown moves by the editor panel height");
    check(state.scroll_line == 4,
          "editor PageDown scrolls the editor window to keep the cursor visible");
}

void test_editor_paste_prefers_local_clipboard() {
    ainiux::editor::EditorState state = ainiux::editor::EditorState::from_text("hello");
    ainiux::editor::Clipboard clipboard;
    clipboard.set("local");
    state.cursor = state.text.size();
    ainiux::Error err = ainiux::editor::paste_with_clipboard_preference(state, clipboard, "external");
    check(err.ok(), "paste prefers local clipboard");
    check(state.text.str() == "hellolocal", "local clipboard overrides terminal paste payload");

    clipboard.clear();
    err = ainiux::editor::paste_with_clipboard_preference(state, clipboard, "external");
    check(err.ok(), "paste falls back to terminal payload when local clipboard is empty");
    check(state.text.str() == "hellolocalexternal", "terminal paste payload is inserted");
    check(clipboard.empty(),
          "external paste does not populate the authoritative internal clipboard");

    state.selection.anchor = 0;
    state.selection.active = 4;
    state.cursor = 4;
    err = ainiux::editor::paste_with_clipboard_preference(state, clipboard, "replaced");
    check(err.ok() && state.text.str() == "replacedlocalexternal",
          "external paste replaces the active selection as one edit");
    check(state.undo() && state.text.str() == "hellolocalexternal",
          "external selection replacement is one undoable edit");
}

void test_editor_path_completion() {
    const std::string directory = "build/ainiux-tab-completion";
    std::error_code filesystem_error;
    std::filesystem::create_directories(directory + "/ainiux-folder", filesystem_error);
    check(!filesystem_error, "path completion fixture directory is created");

    const std::vector<std::string> files = {
        "ainiux-single-result.txt",
        "ainiux-cycle-alpha.txt",
        "ainiux-cycle-alpine.txt",
    };
    for (const std::string& name : files) {
        std::ofstream fixture(directory + "/" + name, std::ios::binary | std::ios::trunc);
        fixture << name;
        check(static_cast<bool>(fixture), "path completion fixture file is written: " + name);
    }

    ainiux::editor::PathCompleter completer;
    const std::string unique_prefix = "/insert " + directory + "/ainiux-single-r";
    ainiux::editor::EditorState unique = ainiux::editor::EditorState::from_text(unique_prefix);
    unique.cursor = unique.text.size();
    ainiux::editor::PathCompletionResult result = completer.complete(unique);
    check(result.error.ok() && result.match_count == 1, "path completion finds a unique file");
    check(unique.text.str() == "/insert " + directory + "/ainiux-single-result.txt",
          "one Tab fully completes a unique path");

    completer.reset();
    const std::string cycle_prefix = "/attach " + directory + "/ainiux-cy";
    ainiux::editor::EditorState cycling = ainiux::editor::EditorState::from_text(cycle_prefix);
    cycling.cursor = cycling.text.size();
    result = completer.complete(cycling);
    const std::string common = "/attach " + directory + "/ainiux-cycle-alp";
    check(result.error.ok() && result.match_count == 2 && !result.cycling,
          "first Tab reports multiple path matches");
    check(cycling.text.str() == common, "first Tab completes the unambiguous common path prefix");

    result = completer.complete(cycling);
    check(result.cycling && result.choice_index == 0,
          "second Tab selects the first sorted path choice");
    check(cycling.text.str() == "/attach " + directory + "/ainiux-cycle-alpha.txt",
          "second Tab inserts the first path choice");

    result = completer.complete(cycling);
    check(result.cycling && result.choice_index == 1,
          "third Tab selects the next path choice");
    check(cycling.text.str() == "/attach " + directory + "/ainiux-cycle-alpine.txt",
          "third Tab inserts the next path choice");

    result = completer.complete(cycling);
    check(result.cycling && result.choice_index == 0,
          "repeated Tab wraps path choices in sorted order");

    completer.reset();
    ainiux::editor::EditorState directory_state =
        ainiux::editor::EditorState::from_text(directory + "/ainiux-fol");
    directory_state.cursor = directory_state.text.size();
    result = completer.complete(directory_state);
    check(result.match_count == 1 && directory_state.text.str() == directory + "/ainiux-folder/",
          "directory completion appends a slash");

    completer.reset();
    ainiux::editor::EditorState missing =
        ainiux::editor::EditorState::from_text(directory + "/ainiux-does-not-exist");
    missing.cursor = missing.text.size();
    result = completer.complete(missing);
    check(result.error.ok() && result.match_count == 0 &&
              missing.text.str() == directory + "/ainiux-does-not-exist",
          "path completion leaves an unmatched path unchanged");

    ainiux::editor::EditorState cancelled =
        ainiux::editor::EditorState::from_text(directory + "/ainiux-single-r");
    cancelled.cursor = cancelled.text.size();
    result = completer.complete(cancelled, []() { return true; });
    check(result.error.code == ainiux::ErrorCode::Cancelled &&
              cancelled.text.str() == directory + "/ainiux-single-r",
          "a cancelled path scan leaves editor input unchanged");

    completer.reset();
    ainiux::editor::EditorState reset_cycle = ainiux::editor::EditorState::from_text(cycle_prefix);
    reset_cycle.cursor = reset_cycle.text.size();
    completer.complete(reset_cycle);
    completer.reset();
    result = completer.complete(reset_cycle);
    check(!result.cycling && reset_cycle.text.str() == common,
          "resetting completion prevents a later Tab from cycling stale choices");

    std::string minibuffer_input = directory + "/ainiux-sing";
    ainiux::editor::PathCompleter minibuffer_completer;
    result = ainiux::editor::complete_path_input(minibuffer_input, minibuffer_completer);
    check(result.error.ok() && result.match_count == 1,
          "minibuffer path completion finds a unique file");
    check(minibuffer_input == directory + "/ainiux-single-result.txt",
          "minibuffer path completion completes editor save/open paths");
}

void test_expand_user_path() {
    if (const char* home = std::getenv("HOME")) {
        check(ainiux::expand_user_path("~") == home, "expand_user_path expands bare tilde");
        check(ainiux::expand_user_path("~/notes.txt") == std::string(home) + "/notes.txt",
              "expand_user_path expands tilde-prefixed paths");
    }
    check(ainiux::expand_user_path("-") == "-", "expand_user_path preserves stdin dash");
    check(ainiux::expand_user_path("stdin") == "stdin", "expand_user_path preserves stdin literal");
    check(ainiux::expand_user_path("/tmp/file") == "/tmp/file",
          "expand_user_path leaves absolute paths unchanged");
}

void test_editor_piece_table_edits() {
    ainiux::editor::PieceTable table = ainiux::editor::PieceTable::from_string("alpha\nbeta\ngamma");
    check(table.size() == 16, "piece table initial size");
    check(table.line_count() == 3, "piece table initial line count");
    check(table.line_text(1) == "beta", "piece table line text");

    ainiux::Error err = table.insert(6, "wide\n");
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
    ainiux::editor::EditorState state = ainiux::editor::EditorState::from_text("one\ntwo\nthree");
    ainiux::editor::Rect rect{4, 10, 2, 4};
    state.cursor = state.text.offset_for_line_column(1, 1);
    ainiux::editor::RenderedPanel rendered = state.render(rect);
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

void test_editor_search_navigation() {
    ainiux::editor::EditorState state =
        ainiux::editor::EditorState::from_text("alpha beta\nbeta gamma\nalpha");

    check(state.search("beta"), "editor search finds a substring at or after the cursor");
    check(state.cursor == 6, "editor search moves to the first matching substring");
    check(!state.dirty, "editor search does not mark the buffer dirty");

    check(state.search_next("beta"), "editor F3-style search next finds the following match");
    check(state.cursor == 11, "editor search next moves to the next match");

    check(state.search_next("beta"), "editor search next wraps to the first match");
    check(state.cursor == 6, "editor search next wraps from the last match");

    check(state.search_previous("beta"), "editor Shift+F3-style search previous wraps backward");
    check(state.cursor == 11, "editor search previous moves to the previous match");

    const size_t before = state.cursor;
    check(!state.search("missing"), "editor search reports a missing substring");
    check(state.cursor == before, "editor search leaves cursor in place when not found");
}

void test_editor_search_replace() {
    ainiux::editor::EditorState state =
        ainiux::editor::EditorState::from_text("one two one two one");

    size_t replacements = 0;
    ainiux::Error err = state.replace_all_from(4, "one", "ONE", replacements);
    check(err.ok(), "editor replace-all from cursor succeeds");
    check(replacements == 2, "editor replace-all counts replacements to the end of the buffer");
    check(state.text.str() == "one two ONE two ONE",
          "editor replace-all only changes occurrences at or after the start offset");
    check(state.dirty, "editor replace-all marks the buffer dirty");
    check(state.undo(), "editor replace-all is undoable as one edit");
    check(state.text.str() == "one two one two one",
          "editor undo restores the buffer before replace-all");

    err = state.replace_all_from(0, "two", "", replacements);
    check(err.ok(), "editor replace-all accepts an empty replacement");
    check(replacements == 2, "editor delete-by-replace counts removed occurrences");
    check(state.text.str() == "one  one  one",
          "editor empty replacement deletes all matching occurrences to the end");

    err = state.replace_all_from(0, "", "x", replacements);
    check(!err.ok(), "editor replace-all rejects an empty search string");
}

void test_editor_selection_and_clipboard() {
    ainiux::editor::EditorState state = ainiux::editor::EditorState::from_text("alpha beta gamma");
    ainiux::editor::Rect rect{1, 1, 1, 20};
    ainiux::editor::Clipboard clipboard;

    state.cursor = 5;
    state.apply_movement(ainiux::editor::MovementKey::Left, rect, true);
    state.apply_movement(ainiux::editor::MovementKey::Left, rect, true);
    state.apply_movement(ainiux::editor::MovementKey::Left, rect, true);
    state.apply_movement(ainiux::editor::MovementKey::Left, rect, true);
    state.apply_movement(ainiux::editor::MovementKey::Left, rect, true);
    check(state.selection.has_range(), "shift movement creates a selection");
    check(state.selected_text() == "alpha", "selected text matches the highlighted range");

    ainiux::editor::EditorState ascii = ainiux::editor::EditorState::from_text("abcdef");
    ascii.cursor = 1;
    for (int i = 0; i < 3; ++i) {
        ascii.apply_movement(ainiux::editor::MovementKey::Right, rect, true);
    }
    check(ascii.selected_text() == "bcde",
          "shift-right selection includes the character at the cursor endpoint");
    ascii = ainiux::editor::EditorState::from_text("abcdef");
    ascii.cursor = 4;
    for (int i = 0; i < 3; ++i) {
        ascii.apply_movement(ainiux::editor::MovementKey::Left, rect, true);
    }
    check(ascii.selected_text() == "bcde",
          "shift-left selection includes the anchor-side endpoint character");
    ascii = ainiux::editor::EditorState::from_text("abcdef");
    ascii.cursor = 0;
    for (int i = 0; i < 2; ++i) {
        ascii.apply_movement(ainiux::editor::MovementKey::Right, rect, true);
    }
    check(ascii.selected_text() == "ab",
          "short forward selections do not over-extend the final character");
    ascii.selection.anchor = 1;
    ascii.selection.active = 5;
    check(ascii.selected_text() == "bcde", "manual forward selection uses an exclusive end offset");

    check(state.copy_selection(clipboard).ok(), "copy selection succeeds");
    check(clipboard.text() == "alpha", "clipboard stores copied text");

    state.cursor = state.text.size();
    state.clear_selection();
    check(state.paste(clipboard).ok(), "paste inserts clipboard text");
    check(state.text.str() == "alpha beta gammaalpha", "paste appends clipboard at cursor");

    state.cursor = 0;
    state.selection.anchor = 0;
    state.selection.active = 5;
    check(state.cut_selection(clipboard).ok(), "cut selection succeeds");
    check(clipboard.text() == "alpha", "cut leaves clipboard unchanged from copied text");
    check(state.text.str() == " beta gammaalpha", "cut removes selected text");
    check(state.undo(), "cut is undoable");
    check(state.text.str() == "alpha beta gammaalpha", "undo restores cut text");

    state.selection.anchor = 6;
    state.selection.active = 10;
    check(state.paste(clipboard).ok(), "paste replaces active selection");
    check(state.text.str() == "alpha alpha gammaalpha", "paste replaces selected range");
}

void test_editor_autosave() {
    long long parsed = 0;
    check(ainiux::editor::parse_byte_size("10M", parsed).ok() && parsed == 10LL * 1024LL * 1024LL,
          "editor auto-save byte size parses megabytes");
    check(ainiux::editor::parse_byte_size("512k", parsed).ok() && parsed == 512LL * 1024LL,
          "editor auto-save byte size parses kilobytes");
    check(ainiux::editor::parse_byte_size("2G", parsed).ok() && parsed == 2LL * 1024LL * 1024LL * 1024LL,
          "editor auto-save byte size parses gigabytes");
    check(ainiux::editor::parse_byte_size("1T", parsed).ok() &&
              parsed == 1024LL * 1024LL * 1024LL * 1024LL,
          "editor auto-save byte size parses terabytes");

    check(ainiux::editor::autosave_path_for("notes.txt", "~") == "notes.txt~",
          "editor auto-save path appends postfix to the file name");

    ainiux::editor::EditorSettings settings;
    settings.auto_save_mode = true;
    settings.auto_save_threshold = 300;
    settings.auto_save_timeout_seconds = 30;

    ainiux::editor::EditorState state = ainiux::editor::EditorState::from_text("alpha");
    state.path = "build/autosave-source.txt";
    state.dirty = true;
    state.record_autosave_change(299);
    const auto idle_short = std::chrono::seconds(5);
    check(!ainiux::editor::evaluate_autosave(state, settings, idle_short).should_save,
          "editor auto-save waits until the change threshold is reached");
    state.record_autosave_change(1);
    check(ainiux::editor::evaluate_autosave(state, settings, idle_short).threshold_met,
          "editor auto-save threshold triggers when enough bytes changed");
    state.reset_autosave_pending();
    state.record_autosave_change(10);
    const auto idle_long = std::chrono::seconds(31);
    check(ainiux::editor::evaluate_autosave(state, settings, idle_long).timeout_met,
          "editor auto-save timeout triggers after idle time with pending changes");

    settings.auto_save_size_limit = 4;
    state.text = ainiux::editor::PieceTable::from_string("12345");
    state.dirty = true;
    state.record_autosave_change(5);
    std::string skip_message;
    check(!ainiux::editor::perform_autosave(state, settings, skip_message).ok() &&
              skip_message.find("auto_save_size_limit") != std::string::npos,
          "editor auto-save skips buffers above the configured size limit");

    settings.auto_save_size_limit = ainiux::editor::kDefaultAutoSaveSizeLimit;
    state.text = ainiux::editor::PieceTable::from_string("autosave payload");
    state.dirty = true;
    state.record_autosave_change(100);
    std::string autosave_message;
    check(ainiux::editor::perform_autosave(state, settings, autosave_message).ok(),
          "editor auto-save writes the backup file");
    check(state.dirty, "editor auto-save does not clear the dirty flag");
    check(state.autosave_pending_bytes() == 0, "editor auto-save clears pending byte counter");
    check(autosave_message.find("build/autosave-source.txt~") != std::string::npos,
          "editor auto-save reports the backup path");

    ainiux::editor::PieceTable backup;
    check(ainiux::editor::load_file("build/autosave-source.txt~", backup).ok() &&
              backup.str() == "autosave payload",
          "editor auto-save backup file contains the current buffer");

    state.text = ainiux::editor::PieceTable::from_string("auto\nsave\n");
    state.linebreak = ainiux::editor::LineBreak::Crlf;
    state.dirty = true;
    state.record_autosave_change(10);
    check(ainiux::editor::perform_autosave(state, settings, autosave_message).ok(),
          "editor auto-save writes using the buffer linebreak mode");
    {
        std::ifstream raw_backup("build/autosave-source.txt~", std::ios::binary);
        const std::string raw((std::istreambuf_iterator<char>(raw_backup)),
                              std::istreambuf_iterator<char>());
        check(raw == "auto\r\nsave\r\n",
              "editor auto-save preserves CRLF and final-line-ending state");
    }

    ainiux::editor::remove_autosave_file(state.path, settings);
    check(!std::filesystem::exists("build/autosave-source.txt~"),
          "editor auto-save backup is removed after an explicit save cleanup");

    ainiux::config::ParseResult parsed_config = ainiux::config::parse(
        "[editor]\nauto-save-mode = off\nauto-save-postfix = \"#\"\nauto-save-threshold = 128\n"
        "auto-save-timeout = 12\nauto-save-size-limit = 2M\n",
        "autosave.conf");
    check(parsed_config.error.ok(), "editor auto-save config parses");
    ainiux::cli::Options options;
    ainiux::Error err = ainiux::config::apply_document(parsed_config.document, options);
    check(err.ok() && !options.editor_auto_save_mode && options.editor_auto_save_postfix == "#" &&
              options.editor_auto_save_threshold == 128 &&
              options.editor_auto_save_timeout_seconds == 12 &&
              options.editor_auto_save_size_limit == 2LL * 1024LL * 1024LL,
          "editor auto-save config settings apply");

    const std::string main_path = "build/autosave-recovery-main.txt";
    const std::string backup_path = "build/autosave-recovery-main.txt~";
    {
        std::ofstream main_file(main_path, std::ios::trunc);
        main_file << "saved yesterday";
        std::ofstream backup_file(backup_path, std::ios::trunc);
        backup_file << "unsaved today";
    }
    const auto main_time = std::filesystem::last_write_time(main_path);
    std::filesystem::last_write_time(backup_path, main_time + std::chrono::seconds(1));

    ainiux::editor::EditorSettings recovery_settings;
    const ainiux::editor::AutosaveRecoveryOffer offer =
        ainiux::editor::check_autosave_recovery_offer(main_path, recovery_settings);
    check(offer.should_offer && offer.autosave_path == backup_path,
          "editor auto-save recovery is offered when backup is newer");

    recovery_settings.auto_save_mode = false;
    check(!ainiux::editor::check_autosave_recovery_offer(main_path, recovery_settings).should_offer,
          "editor auto-save recovery is disabled when auto-save mode is off");

    const std::string prompt =
        ainiux::editor::autosave_recovery_prompt_message(main_path, backup_path);
    check(prompt.find(main_path) != std::string::npos && prompt.find(backup_path) != std::string::npos,
          "editor auto-save recovery prompt names both files");
}

void test_editor_undo_redo_key_bindings() {
    check(ainiux::editor::is_editor_undo_key(26), "Ctrl+Z is an editor undo key");
    check(ainiux::editor::is_editor_undo_key(21), "Ctrl+U is an editor undo key");
    check(!ainiux::editor::is_editor_undo_key(18), "Ctrl+R is not an editor undo key");

    check(!ainiux::editor::is_editor_redo_key(18), "Ctrl+R is not an editor redo key");
    check(ainiux::editor::is_editor_redo_key(25), "Ctrl+Y is an editor redo key");
    check(!ainiux::editor::is_editor_redo_key(26), "Ctrl+Z is not an editor redo key");

    unsigned char decoded = 0;
    check(ainiux::editor::decode_control_key_sequence("[26;5u", decoded) && decoded == 26,
          "kitty Ctrl+Z sequence decodes to undo key");
    check(ainiux::editor::decode_control_key_sequence("[18;5u", decoded) && decoded == 18,
          "kitty Ctrl+R sequence decodes to regenerate key");
    check(ainiux::editor::decode_control_key_sequence("[116;7u", decoded) &&
              decoded == ainiux::editor::editor_key_toggle_thinking_traces(),
          "kitty Alt+Ctrl+T decodes to the thinking-trace toggle");
    check(ainiux::editor::decode_control_key_sequence("[27;7;116~", decoded) &&
              decoded == ainiux::editor::editor_key_toggle_thinking_traces(),
          "xterm modifyOtherKeys Alt+Ctrl+T decodes to the thinking-trace toggle");
    ainiux::editor::clear_terminal_input_queue();
    ainiux::editor::push_terminal_input_bytes(std::string("\x1b", 1) +
                                               std::string(1, static_cast<char>(20)));
    ainiux::editor::TerminalInputEvent alt_ctrl_t;
    check(ainiux::editor::read_terminal_input(alt_ctrl_t, 0) &&
              alt_ctrl_t.type == ainiux::editor::TerminalInputType::Byte &&
              alt_ctrl_t.byte == ainiux::editor::editor_key_toggle_thinking_traces(),
          "legacy Esc-prefixed Alt+Ctrl+T decodes to the thinking-trace toggle");
    ainiux::editor::clear_terminal_input_queue();
}

void test_editor_split_layout() {
    using ainiux::editor::Rect;
    using ainiux::editor::SplitKind;
    using ainiux::editor::SplitLayout;
    using ainiux::editor::window_prefix_action;

    check(window_prefix_action('v') == "split-v", "window prefix v is vertical split");
    check(window_prefix_action('3') == "split-v", "window prefix 3 aliases vertical split");
    check(window_prefix_action('h') == "split-h", "window prefix h is horizontal split");
    check(window_prefix_action('2') == "split-h", "window prefix 2 aliases horizontal split");
    check(window_prefix_action('o') == "other", "window prefix o is other pane");
    check(window_prefix_action('0') == "close", "window prefix 0 closes pane");
    check(window_prefix_action('1') == "maximize", "window prefix 1 maximizes pane");
    check(window_prefix_action(27) == "cancel", "Esc cancels window prefix");
    check(window_prefix_action(7) == "cancel", "Ctrl+G cancels window prefix");
    check(window_prefix_action('x').empty(), "unknown window prefix is rejected");

    SplitLayout layout(0);
    const Rect large{1, 1, 40, 80};
    check(layout.leaf_count() == 1, "new layout starts with one pane");
    check(!layout.has_split(), "new layout is not split");
    check(layout.split_focused(SplitKind::Vertical, large), "vertical split succeeds on large area");
    check(layout.leaf_count() == 2, "vertical split creates two panes");
    check(layout.has_split(), "layout reports split after vertical split");
    check(layout.focused_leaf() == 0, "focus stays on first pane after split");
    {
        const std::optional<size_t> other = layout.other_scroll_leaf();
        check(other.has_value() && *other == 1,
              "after split, other_scroll_leaf is the new sibling pane");
    }

    const auto panes = layout.layout_panes(large);
    check(panes.size() == 2, "layout_panes returns two rectangles");
    check(panes[0].rect.width > 0 && panes[1].rect.width > 0,
          "both vertical panes have positive width");
    check(panes[0].rect.col + panes[0].rect.width < panes[1].rect.col,
          "vertical panes leave a separator column between them");

    layout.focus_next();
    check(layout.focused_leaf() == 1, "focus_next moves to second pane");
    {
        const std::optional<size_t> other = layout.other_scroll_leaf();
        check(other.has_value() && *other == 0,
              "other_scroll_leaf is the previously focused pane after focus_next");
    }
    layout.focus_next();
    check(layout.focused_leaf() == 0, "focus_next wraps to first pane");
    {
        const std::optional<size_t> other = layout.other_scroll_leaf();
        check(other.has_value() && *other == 1,
              "other_scroll_leaf tracks the last left pane after wrap");
    }

    layout.set_focused_buffer(3);
    check(layout.focused_buffer() == 3, "focused pane buffer index updates");
    layout.on_buffer_removed(3, 0);
    check(layout.focused_buffer() == 2 || layout.focused_buffer() == 0,
          "buffer removal remaps focused buffer index");

    layout.maximize_focused();
    check(layout.leaf_count() == 1 && !layout.has_split(), "maximize collapses to one pane");

    check(layout.split_and_open_buffer(SplitKind::Horizontal, large, 4),
          "split_and_open_buffer succeeds on large area");
    check(layout.leaf_count() == 2 && layout.focused_leaf() == 1,
          "split_and_open_buffer focuses the new sibling pane");
    check(layout.focused_buffer() == 4, "split_and_open_buffer assigns the new buffer to the sibling");
    {
        const std::optional<size_t> other = layout.other_scroll_leaf();
        check(other.has_value() && *other == 0,
              "split_and_open_buffer records the original pane for other-scroll");
    }
    layout.maximize_focused();

    check(layout.split_focused(SplitKind::Horizontal, large), "horizontal split succeeds");
    check(layout.leaf_count() == 2, "horizontal split creates two panes");
    const auto h_panes = layout.layout_panes(large);
    check(h_panes.size() == 2 && h_panes[0].rect.row + h_panes[0].rect.height < h_panes[1].rect.row,
          "horizontal panes leave a separator row between them");

    check(layout.close_focused(), "close removes a pane when more than one exists");
    check(layout.leaf_count() == 1, "close leaves a single pane");
    check(!layout.other_scroll_leaf().has_value(),
          "other_scroll_leaf is empty with a single pane");
    check(!layout.close_focused(), "close fails when only one pane remains");

    const Rect tiny{1, 1, 2, 10};
    check(!layout.split_focused(SplitKind::Vertical, tiny),
          "vertical split is refused when the pane is too narrow");
}

void test_editor_revert_to_snapshot() {
    ainiux::editor::EditorState state = ainiux::editor::EditorState::from_text("alpha beta");
    state.cursor = state.text.offset_for_line_column(0, 6);
    const ainiux::editor::EditorSnapshot before = state.capture_state();
    ainiux::Error err = state.insert_without_undo("GAMMA");
    check(err.ok(), "editor insert before revert succeeds");
    check(state.text.str() == "alpha GAMMAbeta", "editor assist-style insert changes text");
    state.revert_to_snapshot(before);
    check(state.text.str() == "alpha beta", "editor revert_to_snapshot restores pre-assist buffer");
    check(state.cursor == before.cursor, "editor revert_to_snapshot restores cursor");
    check(state.dirty, "editor revert_to_snapshot marks buffer dirty after content change");
}

void test_editor_undo_redo() {
    ainiux::editor::EditorState state = ainiux::editor::EditorState::from_text("alpha");
    state.cursor = state.text.size();
    check(state.undo_limit() == ainiux::editor::kDefaultUndoLimit,
          "editor undo history defaults to five entries");

    ainiux::Error err = state.insert(" beta");
    check(err.ok(), "editor insert before undo succeeds");
    check(state.text.str() == "alpha beta", "editor insert changes text before undo");
    check(state.can_undo() && !state.can_redo(), "editor records undo and clears redo after an edit");

    check(state.undo(), "editor undo succeeds");
    check(state.text.str() == "alpha", "editor undo restores previous buffer text");
    check(state.cursor == 5, "editor undo restores previous cursor");
    check(!state.can_undo() && state.can_redo(), "editor undo moves state to redo stack");

    check(state.redo(), "editor redo succeeds");
    check(state.text.str() == "alpha beta", "editor redo restores undone insert");
    check(state.cursor == state.text.size(), "editor redo restores cursor after insert");

    err = state.erase_before_cursor();
    check(err.ok() && state.text.str() == "alpha bet", "editor delete records an undoable edit");
    check(state.undo(), "editor undo after delete succeeds");
    check(state.text.str() == "alpha beta", "editor undo restores deleted character");

    err = state.insert("!");
    check(err.ok(), "editor new edit after undo succeeds");
    check(!state.can_redo(), "editor new edit clears stale redo history");

    err = state.replace(0, 5, "ALPHA");
    check(err.ok() && state.text.str() == "ALPHA beta!", "editor replace changes text");
    check(state.undo(), "editor replace is undoable as one edit");
    check(state.text.str() == "alpha beta!", "editor undo restores text before replace");

    ainiux::editor::EditorState limited = ainiux::editor::EditorState::from_text("");
    limited.set_undo_limit(2);
    check(limited.undo_limit() == 2, "editor undo history limit can be changed");
    check(limited.insert("a").ok(), "editor limited undo first edit succeeds");
    check(limited.insert("b").ok(), "editor limited undo second edit succeeds");
    check(limited.insert("c").ok(), "editor limited undo third edit succeeds");
    check(limited.undo() && limited.text.str() == "ab",
          "editor limited undo restores the newest retained edit");
    check(limited.undo() && limited.text.str() == "a",
          "editor limited undo restores the oldest retained edit");
    check(!limited.undo(), "editor undo history discards entries beyond the configured limit");

    limited.set_undo_limit(0);
    check(limited.insert("z").ok(), "editor zero undo limit still allows edits");
    check(!limited.can_undo(), "editor zero undo limit stores no undo entries");

    ainiux::editor::EditorState paste_state = ainiux::editor::EditorState::from_text("hello");
    paste_state.cursor = paste_state.text.size();
    ainiux::editor::Clipboard clipboard;
    clipboard.set(" world");
    check(paste_state.paste(clipboard).ok(), "editor paste succeeds");
    check(paste_state.text.str() == "hello world", "editor paste appends clipboard text");
    check(paste_state.undo(), "editor paste is undoable");
    check(paste_state.text.str() == "hello", "editor undo restores text before paste");
}

void test_editor_unicode_combining_sequence_wraps_on_grapheme_boundary() {
    const std::string combining = "e" "\xCC\x81";
    const std::string text = combining + "yy";

    ainiux::editor::EditorState state =
        ainiux::editor::EditorState::from_text(text);
    ainiux::editor::RenderedPanel rendered = state.render({1, 1, 3, 1});
    check(rendered.lines.size() == 3,
          "editor combining sequence render produces three rows in a one-cell-wide panel");
    check(rendered.lines[0] == combining,
          "editor keeps a base letter plus combining mark intact on the first visual row");
    check(rendered.lines[1] == "y",
          "editor wraps following ASCII only after the whole combining grapheme");
    check(rendered.lines[2] == "y",
          "editor keeps trailing ASCII characters intact on later visual rows");
}

void test_editor_unicode_display_columns_and_offsets() {
    const std::string ni = "\xE4\xBD\xA0";
    const std::string emoji = "\xF0\x9F\x98\x80";
    const std::string combining = "e" "\xCC\x81";

    ainiux::editor::PieceTable cjk =
        ainiux::editor::PieceTable::from_string("a" + ni + "b");
    check(cjk.display_column_for_offset(1) == 1,
          "editor display column after ASCII is one");
    check(cjk.display_column_for_offset(1 + ni.size()) == 3,
          "editor display column counts a Chinese character as two terminal cells");
    check(cjk.offset_for_line_column(0, 2) == 1,
          "editor column lookup does not place the cursor inside a wide Chinese cell");
    check(cjk.offset_for_line_column(0, 3) == 1 + ni.size(),
          "editor column lookup reaches the byte offset after a wide Chinese character");

    ainiux::editor::PieceTable emoji_table =
        ainiux::editor::PieceTable::from_string("a" + emoji + "b");
    check(emoji_table.display_column_for_offset(1 + emoji.size()) == 3,
          "editor display column counts an emoji as two terminal cells");

    ainiux::editor::PieceTable combining_table =
        ainiux::editor::PieceTable::from_string(combining + "x");
    check(combining_table.display_column_for_offset(combining.size()) == 1,
          "editor display column gives combining marks zero width");
    check(combining_table.offset_for_line_column(0, 1) == combining.size(),
          "editor column lookup lands after the whole combining sequence");
}

void test_editor_unicode_emoji_pair_wraps_on_cell_boundaries() {
    const std::string grin = "\xF0\x9F\x98\x80";
    const std::string pair = grin + grin;

    ainiux::editor::EditorState state =
        ainiux::editor::EditorState::from_text(pair);
    ainiux::editor::RenderedPanel rendered = state.render({1, 1, 2, 3});
    check(rendered.lines.size() == 2,
          "editor emoji pair render produces two rows in a three-cell-wide panel");
    check(rendered.lines[0] == grin + " ",
          "editor wraps after the first two-cell emoji without splitting it");
    check(rendered.lines[1] == grin + " ",
          "editor keeps the second emoji intact on the next visual row");

    state.cursor = grin.size();
    rendered = state.render({1, 1, 2, 3});
    check(rendered.cursor.visible && rendered.cursor.row == 1 && rendered.cursor.col == 0,
          "editor cursor after the first wrapped emoji maps to the second visual row");
}

void test_editor_unicode_emoji_skin_tone_wrap_keeps_modifier() {
    const std::string thumbs_up = "\xF0\x9F\x91\x8D";
    const std::string skin_tone = "\xF0\x9F\x8F\xBD";
    const std::string grapheme = thumbs_up + skin_tone;

    ainiux::editor::EditorState state =
        ainiux::editor::EditorState::from_text(grapheme + "!");
    ainiux::editor::RenderedPanel rendered = state.render({1, 1, 1, 2});
    check(rendered.lines[0] == grapheme,
          "editor wrap keeps an emoji plus skin-tone modifier intact in a two-cell panel");

    rendered = state.render({1, 1, 2, 3});
    check(rendered.lines[0] == grapheme + "!",
          "editor keeps skin-tone emoji and trailing ASCII together when they fit");
    check(rendered.lines[1] == "   ",
          "editor leaves the second visual row blank when only one row is needed");
}

void test_editor_unicode_emoji_zwj_wrap_keeps_sequence_intact() {
    const std::string family_emoji =
        "\xF0\x9F\x91\xA8" "\xE2\x80\x8D"
        "\xF0\x9F\x91\xA9" "\xE2\x80\x8D"
        "\xF0\x9F\x91\xA7" "\xE2\x80\x8D"
        "\xF0\x9F\x91\xA6";

    ainiux::editor::EditorState state =
        ainiux::editor::EditorState::from_text(family_emoji);
    ainiux::editor::RenderedPanel rendered = state.render({1, 1, 1, 2});
    check(rendered.lines.size() == 1,
          "editor ZWJ family emoji render produces one row in a two-cell-wide panel");
    check(rendered.lines[0] == family_emoji,
          "editor wrap keeps a ZWJ family emoji sequence intact on one visual row");

    state.cursor = family_emoji.size();
    rendered = state.render({1, 1, 2, 2});
    check(rendered.cursor.visible && rendered.cursor.row == 1 && rendered.cursor.col == 0,
          "editor cursor after a two-cell ZWJ emoji lands at the next visual row start");
}

void test_editor_unicode_grapheme_navigation_and_delete() {
    const std::string combining_acute = "\xCC\x81";
    const std::string composed_visual_e = "e" + combining_acute;
    const std::string family_emoji =
        "\xF0\x9F\x91\xA8" "\xE2\x80\x8D"
        "\xF0\x9F\x91\xA9" "\xE2\x80\x8D"
        "\xF0\x9F\x91\xA7" "\xE2\x80\x8D"
        "\xF0\x9F\x91\xA6";

    ainiux::editor::EditorState combining =
        ainiux::editor::EditorState::from_text(composed_visual_e + "x");
    combining.move_right();
    check(combining.cursor == composed_visual_e.size(),
          "editor move_right treats base letter plus combining mark as one grapheme");
    combining.move_left();
    check(combining.cursor == 0,
          "editor move_left treats base letter plus combining mark as one grapheme");
    combining.cursor = composed_visual_e.size();
    ainiux::Error err = combining.erase_before_cursor();
    check(err.ok(), "editor backspace before a combining sequence succeeds");
    check(combining.text.str() == "x",
          "editor backspace removes the whole combining grapheme, not only the mark");

    ainiux::editor::EditorState emoji =
        ainiux::editor::EditorState::from_text(family_emoji + "!");
    emoji.move_right();
    check(emoji.cursor == family_emoji.size(),
          "editor move_right treats a ZWJ emoji sequence as one grapheme");
    emoji.cursor = family_emoji.size();
    err = emoji.erase_before_cursor();
    check(err.ok(), "editor backspace before a ZWJ emoji sequence succeeds");
    check(emoji.text.str() == "!",
          "editor backspace removes the complete ZWJ emoji sequence");
}

void test_editor_unicode_rendering_wraps_on_cell_boundaries() {
    const std::string ni = "\xE4\xBD\xA0";
    const std::string hao = "\xE5\xA5\xBD";
    const std::string combining = "e" "\xCC\x81";

    ainiux::editor::EditorState cjk =
        ainiux::editor::EditorState::from_text(ni + hao);
    ainiux::editor::RenderedPanel rendered = cjk.render({1, 1, 2, 2});
    check(rendered.lines.size() == 2, "editor CJK render produces requested rows");
    check(rendered.lines[0] == ni,
          "editor wraps after one two-cell Chinese character in a two-column panel");
    check(rendered.lines[1] == hao,
          "editor keeps the second Chinese character intact on the next visual row");
    cjk.cursor = ni.size();
    rendered = cjk.render({1, 1, 2, 2});
    check(rendered.cursor.visible && rendered.cursor.row == 1 && rendered.cursor.col == 0,
          "editor cursor after a wide character at wrap boundary maps to the next visual row");

    ainiux::editor::EditorState marks =
        ainiux::editor::EditorState::from_text(combining + "x");
    rendered = marks.render({1, 1, 1, 2});
    check(rendered.lines[0] == combining + "x",
          "editor render keeps a combining sequence and following ASCII in two cells");
}

void test_editor_unicode_selection_search_replace_and_file_round_trip() {
    const std::string chinese = "\xE4\xBD\xA0" "\xE5\xA5\xBD";
    const std::string arabic =
        "\xD9\x85" "\xD8\xB1" "\xD8\xAD" "\xD8\xA8" "\xD8\xA7";
    const std::string cyrillic =
        "\xD0\x9F" "\xD1\x80" "\xD0\xB8" "\xD0\xB2" "\xD0\xB5" "\xD1\x82";
    const std::string nordic =
        "\xC3\x84 \xC3\x96 \xC3\x85 \xC3\xA4 \xC3\xB6 \xC3\xA5";
    const std::string replacement =
        "\xD8\xB3" "\xD9\x84" "\xD8\xA7" "\xD9\x85";

    ainiux::editor::EditorState state =
        ainiux::editor::EditorState::from_text(chinese + "\n" + arabic + "\n" +
                                               cyrillic + "\n" + nordic);
    check(state.text.line_count() == 4, "editor multilingual buffer has four lines");
    check(state.text.line_text(0) == chinese, "editor line_text preserves Chinese UTF-8");
    check(state.text.line_text(1) == arabic, "editor line_text preserves Arabic UTF-8");
    check(state.text.line_text(2) == cyrillic, "editor line_text preserves Cyrillic UTF-8");
    check(state.text.line_text(3) == nordic, "editor line_text preserves Nordic UTF-8");

    ainiux::editor::Clipboard clipboard;
    const size_t arabic_start = state.text.line_start(1);
    state.selection.anchor = arabic_start;
    state.selection.active = arabic_start + arabic.size();
    check(state.selected_text() == arabic, "editor selected_text preserves Arabic byte range");
    check(state.copy_selection(clipboard).ok(), "editor copies Arabic selection");
    check(clipboard.text() == arabic, "editor clipboard stores Arabic selection exactly");
    check(state.cut_selection(clipboard).ok(), "editor cuts Arabic selection");
    check(state.undo(), "editor cut of Arabic text is undoable");
    check(state.text.line_text(1) == arabic, "editor undo restores Arabic text exactly");

    check(state.search(cyrillic), "editor search finds Cyrillic text");
    check(state.cursor == state.text.line_start(2), "editor search moves cursor to Cyrillic text start");

    size_t replacements = 0;
    ainiux::Error err = state.replace_all_from(0, arabic, replacement, replacements);
    check(err.ok(), "editor replace-all accepts Unicode search and replacement");
    check(replacements == 1, "editor replace-all counts the Arabic occurrence");
    check(state.text.line_text(1) == replacement,
          "editor replace-all substitutes Arabic text with replacement UTF-8");

    const std::string path = "build/unit-editor-unicode.txt";
    err = ainiux::editor::save_file(path, state.text);
    check(err.ok(), "editor saves multilingual file");
    ainiux::editor::PieceTable loaded;
    err = ainiux::editor::load_file(path, loaded);
    check(err.ok(), "editor loads multilingual file");
    check(loaded.str() == state.text.str(),
          "editor file round trip preserves multilingual UTF-8 exactly");
}

void test_editor_utf8_codepoint_navigation_and_editing() {
    const std::string ni = "\xE4\xBD\xA0";
    const std::string hao = "\xE5\xA5\xBD";
    const std::string chinese = ni + hao;

    ainiux::editor::PieceTable table =
        ainiux::editor::PieceTable::from_string("A" + chinese + "B");
    check(table.next_char_offset(1) == 1 + ni.size(),
          "editor next_char_offset skips a complete three-byte UTF-8 code point");
    check(table.next_char_offset(1 + ni.size()) == 1 + chinese.size(),
          "editor next_char_offset skips the second Chinese code point");
    check(table.previous_char_offset(1 + chinese.size()) == 1 + ni.size(),
          "editor previous_char_offset lands on a UTF-8 leading byte");

    ainiux::editor::EditorState state =
        ainiux::editor::EditorState::from_text("A" + chinese + "B");
    state.move_right();
    check(state.cursor == 1, "editor move_right crosses ASCII one byte at a time");
    state.move_right();
    check(state.cursor == 1 + ni.size(), "editor move_right skips the first Chinese character bytes");
    state.move_right();
    check(state.cursor == 1 + chinese.size(), "editor move_right skips the second Chinese character bytes");
    state.move_left();
    check(state.cursor == 1 + ni.size(), "editor move_left skips a complete Chinese character");

    ainiux::Error err = state.erase_before_cursor();
    check(err.ok(), "editor backspace before cursor succeeds for UTF-8");
    check(state.text.str() == "A" + hao + "B",
          "editor backspace removes one full UTF-8 code point instead of one byte");
    check(state.cursor == 1, "editor backspace leaves cursor at the removed code point start");

    state = ainiux::editor::EditorState::from_text("A" + chinese + "B");
    state.cursor = 1;
    err = state.erase_at_cursor();
    check(err.ok(), "editor delete at cursor succeeds for UTF-8");
    check(state.text.str() == "A" + hao + "B",
          "editor delete removes one full UTF-8 code point instead of one byte");
    check(state.undo(), "editor UTF-8 delete is undoable");
    check(state.text.str() == "A" + chinese + "B",
          "editor undo restores deleted UTF-8 bytes exactly");
}

void test_editor_vertical_navigation_modes() {
    ainiux::editor::Rect rect{1, 1, 3, 4};
    ainiux::editor::EditorState logical = ainiux::editor::EditorState::from_text("abcdefghij\nXYZ");
    logical.cursor = logical.text.offset_for_line_column(0, 2);
    logical.preferred_column = 2;
    logical.move_down(rect);
    check(logical.cursor == logical.text.offset_for_line_column(1, 2),
          "editor default vertical movement uses logical lines");

    ainiux::editor::EditorState visual = ainiux::editor::EditorState::from_text("abcdefghij\nXYZ");
    visual.vertical_movement = ainiux::editor::VerticalMovementMode::VisualRow;
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

void test_editor_word_wrap_breaks_on_spaces() {
    ainiux::editor::EditorState state = ainiux::editor::EditorState::from_text("alpha beta");
    ainiux::editor::RenderedPanel rendered = state.render({1, 1, 2, 8});
    check(rendered.lines[0] == "alpha   ", "editor wraps at a word break when available");
    check(rendered.lines[1] == "beta    ", "editor continues after the wrapped word break");
}

void test_editor_word_wrap_rendering() {
    ainiux::editor::EditorState state = ainiux::editor::EditorState::from_text("abcdefghij");
    ainiux::editor::Rect rect{1, 1, 3, 4};
    ainiux::editor::RenderedPanel rendered = state.render(rect);
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

void test_editor_file_io_failures() {
    ainiux::editor::PieceTable table;
    ainiux::Error err = ainiux::editor::load_file("build/editor-missing-file.txt", table);
    check(!err.ok() && err.code == ainiux::ErrorCode::FileRead,
          "editor load reports a file-read error for a missing path");

    table = ainiux::editor::PieceTable::from_string(u8"مرحبا 你好");
    err = ainiux::editor::save_file("build/no-such-dir/editor-save.txt", table);
    check(!err.ok() && err.code == ainiux::ErrorCode::FileWrite,
          "editor save to a missing parent directory reports a file-write error");
}

}  // namespace

void test_editor_control_key_sequence_decode() {
    unsigned char decoded = 0;
    check(ainiux::editor::decode_control_key_sequence("[110;5u", decoded) && decoded == 14,
          "editor decodes kitty-style Ctrl+N as new-buffer");
    check(ainiux::editor::decode_control_key_sequence("[19;5u", decoded) && decoded == 19,
          "editor decodes kitty-style Ctrl+S");
    check(ainiux::editor::decode_control_key_sequence("[27;5;19~", decoded) && decoded == 19,
          "editor decodes xterm modifyOtherKeys Ctrl+S");
    check(ainiux::editor::decode_control_key_sequence("[83;5u", decoded) &&
              decoded == ainiux::editor::editor_key_save_as(),
          "editor decodes kitty-style Ctrl+Shift+S as save-as");
    check(ainiux::editor::decode_control_key_sequence("[115;5u", decoded) &&
              decoded == ainiux::editor::editor_key_save_as(),
          "editor decodes kitty-style Ctrl+Shift+s as save-as");
    check(ainiux::editor::decode_control_key_sequence("[23;5u", decoded) && decoded == 23,
          "editor decodes kitty-style Ctrl+W as close-buffer");
    check(ainiux::editor::decode_control_key_sequence("[Z", decoded) &&
              decoded == ainiux::editor::editor_key_backtab(),
          "editor decodes common xterm Shift+Tab as backtab");
    check(ainiux::editor::decode_control_key_sequence("[9;2u", decoded) &&
              decoded == ainiux::editor::editor_key_backtab(),
          "editor decodes kitty Shift+Tab as backtab");
    check(ainiux::editor::decode_control_key_sequence("[27;2;9~", decoded) &&
              decoded == ainiux::editor::editor_key_backtab(),
          "editor decodes xterm modifyOtherKeys Shift+Tab as backtab");
    check(!ainiux::editor::decode_control_key_sequence("[A", decoded),
          "editor ignores arrow-key escape sequences");
}

void test_editor_save_as_overwrite_helpers() {
    const std::string existing = "build/ainiux-editor-save-as-existing.txt";
    std::ofstream out(existing, std::ios::trunc);
    check(static_cast<bool>(out), "editor save-as overwrite fixture is created");
    out << "existing";
    out.close();

    check(!ainiux::editor::needs_overwrite_confirm("build/ainiux-editor-save-as-missing.txt", "scratch.txt"),
          "editor save-as skips overwrite confirm for a new path");
    check(ainiux::editor::needs_overwrite_confirm(existing, "scratch.txt"),
          "editor save-as requires overwrite confirm when the target file exists");
    check(!ainiux::editor::needs_overwrite_confirm(existing, existing),
          "editor save skips overwrite confirm when saving to the current path");
    check(ainiux::editor::needs_overwrite_confirm(existing, ""),
          "editor save requires overwrite confirm when saving to an existing path from scratch");

    const std::string prompt = ainiux::editor::overwrite_prompt_message(existing);
    check(prompt.find(existing) != std::string::npos, "editor overwrite prompt names the target path");
    check(prompt.find("Press y to overwrite") != std::string::npos,
          "editor overwrite prompt explains y to overwrite");
    check(prompt.find("any other key to cancel") != std::string::npos,
          "editor overwrite prompt explains other keys cancel");
}

void test_editor_file_locking_and_read_only_sessions() {
    namespace fs = std::filesystem;
    const fs::path root = fs::absolute("build/editor-lock-tests");
    fs::remove_all(root);
    fs::create_directories(root);
    const fs::path target = root / "document.txt";
    {
        std::ofstream out(target);
        out << "original";
    }

    std::string canonical;
    check(ainiux::editor::canonicalize_editor_target(target.string(), canonical).ok() &&
              fs::path(canonical).is_absolute(),
          "editor lock canonicalizes the target path");
    ainiux::editor::EditorLockAttempt first =
        ainiux::editor::acquire_editor_file_lock(target.string());
    check(first.lock != nullptr && fs::is_directory(canonical + ".LOCK"),
          "editor lock acquisition atomically creates FILE.LOCK");
    ainiux::editor::EditorLockOwner owner;
    check(ainiux::editor::read_editor_lock_owner(canonical + ".LOCK", owner).ok() &&
              owner.schema_version == 1 && owner.pid == static_cast<long long>(getpid()) &&
              owner.canonical_target == canonical && !owner.token.empty(),
          "editor lock writes complete bounded owner metadata");
    ainiux::editor::EditorLockAttempt contended =
        ainiux::editor::acquire_editor_file_lock(target.string());
    check(!contended.lock && contended.error.code == ainiux::ErrorCode::FileLock &&
              contended.owner_metadata_valid,
          "live editor lock contention is reported without removal");

    {
        const std::string owner_path = canonical + ".LOCK/owner";
        std::ifstream in(owner_path, std::ios::binary);
        std::string metadata((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const size_t host = metadata.find("hostname=");
        const size_t host_end = metadata.find('\n', host);
        if (host != std::string::npos && host_end != std::string::npos) {
            metadata.replace(host, host_end - host, "hostname=72656d6f74652d686f7374");
        }
        std::ofstream out(owner_path, std::ios::binary | std::ios::trunc);
        out << metadata;
    }
    ainiux::editor::EditorLockAttempt remote =
        ainiux::editor::acquire_editor_file_lock(target.string());
    check(!remote.lock && remote.owner_metadata_valid &&
              remote.conflicting_owner.hostname == "remote-host" &&
              fs::exists(canonical + ".LOCK"),
          "remote-host lock owner is never removed automatically");

    const fs::path alias = root / "alias.txt";
    fs::create_symlink(target.filename(), alias);
    ainiux::editor::EditorLockAttempt alias_attempt =
        ainiux::editor::acquire_editor_file_lock(alias.string());
    check(!alias_attempt.lock && alias_attempt.conflicting_owner.canonical_target == canonical,
          "symlink aliases contend on the canonical target lock");

    ainiux::editor::EditorState copied;
    copied.set_path(target.string());
    copied.canonical_path = canonical;
    copied.file_lock = first.lock;
    ainiux::editor::EditorState copied_again = copied;
    first.lock.reset();
    copied.file_lock.reset();
    check(fs::exists(canonical + ".LOCK"),
          "EditorState copies share lock ownership for the full buffer lifetime");
    copied_again.file_lock.reset();
    check(!fs::exists(canonical + ".LOCK"), "last EditorState owner releases the lock directory");

    ainiux::editor::EditorLockAttempt token_lock =
        ainiux::editor::acquire_editor_file_lock(target.string());
    const std::string token_directory = token_lock.lock->lock_directory();
    {
        std::ifstream in(token_directory + "/owner", std::ios::binary);
        std::string metadata((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const size_t token = metadata.find("token=");
        check(token != std::string::npos, "lock metadata contains ownership token");
        if (token != std::string::npos) metadata.replace(token, metadata.find('\n', token) - token, "token=78");
        std::ofstream out(token_directory + "/owner", std::ios::binary | std::ios::trunc);
        out << metadata;
    }
    token_lock.lock.reset();
    check(fs::exists(token_directory), "token mismatch prevents lock cleanup by a former owner");
    fs::remove(token_directory + "/owner");
    fs::remove(token_directory);

    const pid_t child = fork();
    if (child == 0) {
        ainiux::editor::EditorLockAttempt child_lock =
            ainiux::editor::acquire_editor_file_lock(target.string());
        _exit(child_lock.lock ? 0 : 1);
    }
    int child_status = 0;
    waitpid(child, &child_status, 0);
    check(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0,
          "child creates a lock for stale recovery testing");
    ainiux::editor::EditorLockAttempt recovered =
        ainiux::editor::acquire_editor_file_lock(target.string());
    check(recovered.lock != nullptr && recovered.stale_lock_recovered,
          "dead same-host owner lock is recovered once");
    recovered.lock.reset();

    const pid_t nonempty_child = fork();
    if (nonempty_child == 0) {
        ainiux::editor::EditorLockAttempt child_lock =
            ainiux::editor::acquire_editor_file_lock(target.string());
        _exit(child_lock.lock ? 0 : 1);
    }
    int nonempty_status = 0;
    waitpid(nonempty_child, &nonempty_status, 0);
    check(WIFEXITED(nonempty_status) && WEXITSTATUS(nonempty_status) == 0,
          "child creates a lock for nonempty stale-lock testing");
    {
        std::ofstream unexpected(canonical + ".LOCK/unexpected");
        unexpected << "do not remove";
    }
    ainiux::editor::EditorLockAttempt nonempty =
        ainiux::editor::acquire_editor_file_lock(target.string());
    check(!nonempty.lock && fs::exists(canonical + ".LOCK/owner") &&
              fs::exists(canonical + ".LOCK/unexpected"),
          "dead local lock with unexpected contents is not removed recursively");
    fs::remove(canonical + ".LOCK/unexpected");
    fs::remove(canonical + ".LOCK/owner");
    fs::remove(canonical + ".LOCK");

    ainiux::editor::EditorLockAttempt upgrade_blocker =
        ainiux::editor::acquire_editor_file_lock(target.string());
    ainiux::editor::EditorState upgrade = ainiux::editor::EditorState::from_text("original");
    upgrade.set_path(target.string());
    check(upgrade.begin_file_session(target.string(), true).code == ainiux::ErrorCode::FileLock &&
              upgrade.read_only,
          "contended existing file begins as read-only");
    upgrade_blocker.lock.reset();
    check(upgrade.insert("!").ok() && !upgrade.read_only && upgrade.file_lock &&
              upgrade.text.str() == "!original",
          "first edit retries the lock and upgrades an unchanged file to writable");
    upgrade.release_file_session();

    ainiux::editor::EditorLockAttempt reload_blocker =
        ainiux::editor::acquire_editor_file_lock(target.string());
    ainiux::editor::EditorState reload_declined =
        ainiux::editor::EditorState::from_text("original");
    reload_declined.set_path(target.string());
    check(reload_declined.begin_file_session(target.string(), true).code ==
              ainiux::ErrorCode::FileLock,
          "changed-file reload fixture begins read-only");
    {
        std::ofstream out(target, std::ios::trunc);
        out << "external before retry";
    }
    reload_blocker.lock.reset();
    check(reload_declined.insert("!").code == ainiux::ErrorCode::FileLock &&
              reload_declined.reload_required && reload_declined.file_lock,
          "edit retry holds the newly acquired lock when disk content changed");
    ainiux::editor::MinibufferState reload_minibuffer;
    ainiux::editor::start_minibuffer(reload_minibuffer,
                                     ainiux::editor::MinibufferAction::ConfirmReloadAfterLock,
                                     "reload?");
    ainiux::editor::ReplaceSession reload_replace;
    ainiux::editor::EditorSettings reload_settings;
    std::string reload_search;
    std::string reload_pending_path;
    bool reload_quit = false;
    bool reload_pending_quit = false;
    ainiux::editor::PendingSaveRequest reload_pending_save;
    ainiux::editor::PendingAutosaveRecovery reload_recovery;
    ainiux::editor::PathCompleter reload_completer;
    check(ainiux::editor::handle_minibuffer_key(reload_declined,
                                                reload_minibuffer,
                                                'n',
                                                reload_quit,
                                                reload_search,
                                                reload_replace,
                                                reload_settings,
                                                reload_pending_path,
                                                reload_pending_quit,
                                                reload_pending_save,
                                                reload_recovery,
                                                reload_completer) &&
              reload_declined.read_only && !reload_declined.reload_required &&
              !reload_declined.file_lock,
          "declining changed-file reload releases the new lock and remains read-only");

    const fs::path blocked_destination = root / "blocked-save-as.txt";
    ainiux::editor::EditorState failed_save_as =
        ainiux::editor::EditorState::from_text("keep original session");
    failed_save_as.set_path(target.string());
    check(failed_save_as.begin_file_session(target.string(), true).ok(),
          "failed Save As fixture owns its original file session");
    const std::shared_ptr<ainiux::editor::EditorFileLock> original_session =
        failed_save_as.file_lock;
    ainiux::editor::EditorLockAttempt destination_blocker =
        ainiux::editor::acquire_editor_file_lock(blocked_destination.string());
    ainiux::editor::MinibufferState failed_save_minibuffer;
    ainiux::editor::PendingSaveRequest failed_save_pending;
    bool failed_save_quit = false;
    ainiux::editor::request_save_editor_to_path(failed_save_as,
                                                blocked_destination.string(),
                                                failed_save_minibuffer,
                                                true,
                                                false,
                                                failed_save_quit,
                                                failed_save_pending,
                                                reload_settings);
    check(failed_save_as.path == target.string() &&
              failed_save_as.canonical_path == canonical &&
              failed_save_as.file_lock == original_session && failed_save_pending.path.empty(),
          "failed Save As retains the original path and lock session");
    destination_blocker.lock.reset();
    failed_save_as.release_file_session();

    ainiux::editor::EditorLockAttempt save_as_blocker =
        ainiux::editor::acquire_editor_file_lock(target.string());
    ainiux::editor::EditorState save_as =
        ainiux::editor::EditorState::from_text("read-only save as");
    save_as.set_path(target.string());
    check(save_as.begin_file_session(target.string(), true).code == ainiux::ErrorCode::FileLock,
          "read-only Save As fixture is contended");
    const fs::path save_as_target = root / "retargeted.txt";
    ainiux::editor::MinibufferState save_as_minibuffer;
    ainiux::editor::PendingSaveRequest save_as_pending;
    bool save_as_quit = false;
    ainiux::editor::request_save_editor_to_path(save_as,
                                                save_as_target.string(),
                                                save_as_minibuffer,
                                                true,
                                                false,
                                                save_as_quit,
                                                save_as_pending,
                                                reload_settings);
    ainiux::editor::PieceTable save_as_saved;
    check(ainiux::editor::load_file(save_as_target.string(), save_as_saved).ok() &&
              save_as_saved.str() == "read-only save as" && !save_as.read_only &&
              save_as.file_lock && save_as.canonical_path != canonical &&
              fs::exists(canonical + ".LOCK"),
          "Save As from read-only retargets only after saving and retains destination lock");
    save_as.release_file_session();
    save_as_blocker.lock.reset();

    fs::create_directory(canonical + ".LOCK");
    {
        std::ofstream out(canonical + ".LOCK/owner");
        out << "malformed\n";
    }
    ainiux::editor::EditorLockAttempt malformed =
        ainiux::editor::acquire_editor_file_lock(target.string());
    check(!malformed.lock && !malformed.owner_metadata_valid && fs::exists(canonical + ".LOCK"),
          "malformed unverifiable lock is never removed");
    fs::remove(canonical + ".LOCK/owner");
    fs::remove(canonical + ".LOCK");

    fs::create_directory(canonical + ".LOCK");
    ainiux::editor::EditorLockAttempt missing_metadata =
        ainiux::editor::acquire_editor_file_lock(target.string());
    check(!missing_metadata.lock && !missing_metadata.owner_metadata_valid &&
              fs::exists(canonical + ".LOCK"),
          "missing lock metadata is unverifiable and never removed automatically");
    fs::remove(canonical + ".LOCK");

    ainiux::editor::EditorState read_only = ainiux::editor::EditorState::from_text("abc");
    read_only.read_only = true;
    check(read_only.insert("x").code == ainiux::ErrorCode::FileLock &&
              read_only.erase_before_cursor().code == ainiux::ErrorCode::FileLock &&
              read_only.replace(0, 1, "z").code == ainiux::ErrorCode::FileLock &&
              read_only.indent().code == ainiux::ErrorCode::FileLock &&
              read_only.outdent().code == ainiux::ErrorCode::FileLock && !read_only.undo() &&
              !read_only.redo(),
          "central EditorState mutation guard rejects read-only changes");
    check(ainiux::editor::editor_status_line(read_only).find("[RO]") != std::string::npos,
          "read-only editor status renders [RO]");

    ainiux::editor::EditorState saving = ainiux::editor::EditorState::from_text("ainiux version");
    saving.set_path(target.string());
    check(saving.begin_file_session(target.string(), true).ok(),
          "writable editor state acquires its main-file lock");
    {
        std::ofstream out(target, std::ios::trunc);
        out << "external version is longer";
    }
    ainiux::editor::MinibufferState minibuffer;
    ainiux::editor::PendingSaveRequest pending;
    ainiux::editor::EditorSettings settings;
    bool quit = false;
    ainiux::editor::request_save_editor_to_path(saving,
                                                target.string(),
                                                minibuffer,
                                                true,
                                                false,
                                                quit,
                                                pending,
                                                settings);
    check(pending.external_change &&
              minibuffer.action == ainiux::editor::MinibufferAction::ConfirmOverwrite,
          "saving detects an external file fingerprint change");
    ainiux::editor::ReplaceSession replace;
    std::string search;
    std::string pending_load;
    bool pending_quit = false;
    ainiux::editor::PendingAutosaveRecovery recovery;
    ainiux::editor::PathCompleter completer;
    check(ainiux::editor::handle_minibuffer_key(saving,
                                                minibuffer,
                                                'n',
                                                quit,
                                                search,
                                                replace,
                                                settings,
                                                pending_load,
                                                pending_quit,
                                                pending,
                                                recovery,
                                                completer) &&
              pending.path.empty(),
          "external-change overwrite can be cancelled");
    ainiux::editor::PieceTable cancelled;
    check(ainiux::editor::load_file(target.string(), cancelled).ok() &&
              cancelled.str() == "external version is longer",
          "cancelled overwrite preserves the external file");
    ainiux::editor::request_save_editor_to_path(saving,
                                                target.string(),
                                                minibuffer,
                                                true,
                                                false,
                                                quit,
                                                pending,
                                                settings);
    {
        std::ofstream out(target, std::ios::trunc);
        out << "a second external version changed during confirmation";
    }
    check(ainiux::editor::handle_minibuffer_key(saving,
                                                minibuffer,
                                                'y',
                                                quit,
                                                search,
                                                replace,
                                                settings,
                                                pending_load,
                                                pending_quit,
                                                pending,
                                                recovery,
                                                completer),
          "first overwrite confirmation rechecks the observed disk version");
    check(!pending.path.empty() &&
              minibuffer.action == ainiux::editor::MinibufferAction::ConfirmOverwrite,
          "a second external change requires a new confirmation");
    check(ainiux::editor::handle_minibuffer_key(saving,
                                                minibuffer,
                                                'y',
                                                quit,
                                                search,
                                                replace,
                                                settings,
                                                pending_load,
                                                pending_quit,
                                                pending,
                                                recovery,
                                                completer),
          "explicit overwrite confirmation for the rechecked version is handled");
    ainiux::editor::PieceTable saved;
    check(ainiux::editor::load_file(target.string(), saved).ok() &&
              saved.str() == "ainiux version" && saving.has_disk_fingerprint,
          "confirmed overwrite saves content and refreshes the fingerprint");
    saving.release_file_session();
    fs::remove_all(root);
}

void test_editor_help_document_and_command() {
    std::string help_text;
    ainiux::Error err = ainiux::editor::load_editor_help_markdown(help_text);
    check(err.ok() && !help_text.empty(), "editor help document loads");
    check(help_text.find("# ainiux Editor Help") != std::string::npos,
          "editor help document contains the title heading");
    check(help_text.find("Ctrl+Space") != std::string::npos,
          "editor help document documents Ctrl+Space continue");
    check(help_text.find("Ctrl+Shift+S") != std::string::npos,
          "editor help document documents Ctrl+Shift+S save as");
    check(help_text.find("Ctrl+N") != std::string::npos,
          "editor help document documents Ctrl+N new buffer");
    check(help_text.find("/saveas") != std::string::npos,
          "editor help document documents /saveas slash command");
    check(help_text.find("/spell") != std::string::npos && help_text.find("/help") != std::string::npos,
          "editor help document lists slash commands");
    check(help_text.find("/regenerate") != std::string::npos,
          "editor help document documents /regenerate");
    check(help_text.find("/new") != std::string::npos,
          "editor help document documents /new");
    check(help_text.find("/list") != std::string::npos,
          "editor help document documents /list");
    check(help_text.find("/close") != std::string::npos,
          "editor help document documents /close");
    check(help_text.find("/vsplit") != std::string::npos &&
              help_text.find("/hsplit") != std::string::npos &&
              help_text.find("/closesplit") != std::string::npos &&
              help_text.find("/maximize") != std::string::npos &&
              help_text.find("/nosplit") != std::string::npos,
          "editor help document documents split slash commands");
    check(help_text.find("loads `/v1/models`") != std::string::npos &&
              help_text.find("selected automatically") != std::string::npos &&
              help_text.find("multiple results open") != std::string::npos,
          "editor help document documents startup model discovery");
    check(help_text.find("no startup picker or model request") != std::string::npos,
          "editor help document documents bare offline startup");
    check(help_text.find("/provider") != std::string::npos && help_text.find("/model") != std::string::npos,
          "editor help document documents /provider and /model");

    check(ainiux::editor::is_editor_help_command("/help"), "editor /help command is recognized");
    check(ainiux::editor::is_editor_help_command("  /HELP  "), "editor /help command is case-insensitive");
    check(!ainiux::editor::is_editor_help_command("/helpful"), "editor help command rejects prefixes");

    const std::vector<std::string> completions =
        ainiux::editor::assist_command_completions(ainiux::editor::default_editor_assist_config());
    check(std::find(completions.begin(), completions.end(), "/help") != completions.end(),
          "assist command completions include /help");
    check(std::find(completions.begin(), completions.end(), "/save") != completions.end(),
          "assist command completions include /save");
    check(std::find(completions.begin(), completions.end(), "/open ") != completions.end(),
          "assist command completions include /open");
    check(std::find(completions.begin(), completions.end(), "/new") != completions.end(),
          "assist command completions include /new");
    check(std::find(completions.begin(), completions.end(), "/list") != completions.end(),
          "assist command completions include /list");
    check(std::find(completions.begin(), completions.end(), "/close") != completions.end(),
          "assist command completions include /close");
    check(std::find(completions.begin(), completions.end(), "/vsplit") != completions.end(),
          "assist command completions include /vsplit");
    check(std::find(completions.begin(), completions.end(), "/hsplit") != completions.end(),
          "assist command completions include /hsplit");
    check(std::find(completions.begin(), completions.end(), "/closesplit") != completions.end(),
          "assist command completions include /closesplit");
    check(std::find(completions.begin(), completions.end(), "/maximize") != completions.end(),
          "assist command completions include /maximize");
    check(std::find(completions.begin(), completions.end(), "/nosplit") != completions.end(),
          "assist command completions include /nosplit");
    check(std::find(completions.begin(), completions.end(), "/provider ") != completions.end(),
          "assist command completions include /provider");
    check(std::find(completions.begin(), completions.end(), "/model ") != completions.end(),
          "assist command completions include /model");
    check(std::find(completions.begin(), completions.end(), "/context ") != completions.end(),
          "assist command completions include /context");
    check(std::find(completions.begin(), completions.end(), "/mode python") != completions.end() &&
              std::find(completions.begin(), completions.end(), "/mode typescript") != completions.end() &&
              std::find(completions.begin(), completions.end(), "/mode htmlonly") != completions.end() &&
              std::find(completions.begin(), completions.end(), "/mode bash") != completions.end() &&
              std::find(completions.begin(), completions.end(), "/mode php") != completions.end() &&
              std::find(completions.begin(), completions.end(), "/mode rust") != completions.end() &&
              std::find(completions.begin(), completions.end(), "/mode powershell") != completions.end() &&
              std::find(completions.begin(), completions.end(), "/mode sql") != completions.end() &&
              std::find(completions.begin(), completions.end(), "/mode yaml") != completions.end(),
          "editor command completions include programming-language modes");
    check(std::find(completions.begin(), completions.end(), "/reformat") != completions.end() &&
              std::find(completions.begin(), completions.end(), "/reformat-all") != completions.end(),
          "editor command completions include language reformat commands");

    ainiux::editor::ParsedEditorSlashCommand slash =
        ainiux::editor::parse_editor_slash_command("/save");
    check(slash.command == ainiux::editor::EditorSlashCommand::Save && slash.path.empty(),
          "editor /save slash command is recognized");
    slash = ainiux::editor::parse_editor_slash_command("/SAVEAS");
    check(slash.command == ainiux::editor::EditorSlashCommand::SaveAs && slash.path.empty(),
          "editor /saveas slash command is case-insensitive");
    slash = ainiux::editor::parse_editor_slash_command("/open build/unit-editor.txt");
    check(slash.command == ainiux::editor::EditorSlashCommand::Open &&
              slash.path == "build/unit-editor.txt",
          "editor /open PATH preserves the path argument");
    slash = ainiux::editor::parse_editor_slash_command("/saveas out/new.txt");
    check(slash.command == ainiux::editor::EditorSlashCommand::SaveAs &&
              slash.path == "out/new.txt",
          "editor /saveas PATH preserves the path argument");
    slash = ainiux::editor::parse_editor_slash_command("/find");
    check(slash.command == ainiux::editor::EditorSlashCommand::Find,
          "editor /find slash command is recognized");
    slash = ainiux::editor::parse_editor_slash_command("/replace");
    check(slash.command == ainiux::editor::EditorSlashCommand::Replace,
          "editor /replace slash command is recognized");
    slash = ainiux::editor::parse_editor_slash_command("/open");
    check(slash.command == ainiux::editor::EditorSlashCommand::Open && slash.path.empty(),
          "editor bare /open slash command is recognized");
    slash = ainiux::editor::parse_editor_slash_command("/new");
    check(slash.command == ainiux::editor::EditorSlashCommand::New && slash.path.empty(),
          "editor /new slash command is recognized");
    slash = ainiux::editor::parse_editor_slash_command("/new name");
    check(slash.command == ainiux::editor::EditorSlashCommand::None,
          "editor /new rejects arguments");
    slash = ainiux::editor::parse_editor_slash_command("/list");
    check(slash.command == ainiux::editor::EditorSlashCommand::List && slash.path.empty(),
          "editor /list slash command is recognized");
    slash = ainiux::editor::parse_editor_slash_command("/close");
    check(slash.command == ainiux::editor::EditorSlashCommand::Close && slash.path.empty(),
          "editor /close slash command is recognized");
    slash = ainiux::editor::parse_editor_slash_command("/close file.txt");
    check(slash.command == ainiux::editor::EditorSlashCommand::None,
          "editor /close rejects arguments");
    slash = ainiux::editor::parse_editor_slash_command("/vsplit");
    check(slash.command == ainiux::editor::EditorSlashCommand::VSplit && slash.path.empty(),
          "editor /vsplit slash command is recognized");
    slash = ainiux::editor::parse_editor_slash_command("/hsplit");
    check(slash.command == ainiux::editor::EditorSlashCommand::HSplit && slash.path.empty(),
          "editor /hsplit slash command is recognized");
    slash = ainiux::editor::parse_editor_slash_command("/closesplit");
    check(slash.command == ainiux::editor::EditorSlashCommand::CloseSplit && slash.path.empty(),
          "editor /closesplit slash command is recognized");
    slash = ainiux::editor::parse_editor_slash_command("/maximize");
    check(slash.command == ainiux::editor::EditorSlashCommand::Maximize && slash.path.empty(),
          "editor /maximize slash command is recognized");
    slash = ainiux::editor::parse_editor_slash_command("/nosplit");
    check(slash.command == ainiux::editor::EditorSlashCommand::Maximize && slash.path.empty(),
          "editor /nosplit is an alias for /maximize");
    slash = ainiux::editor::parse_editor_slash_command("/vsplit extra");
    check(slash.command == ainiux::editor::EditorSlashCommand::None,
          "editor /vsplit rejects arguments");
    slash = ainiux::editor::parse_editor_slash_command("/chat");
    check(slash.command == ainiux::editor::EditorSlashCommand::Chat && slash.path.empty(),
          "editor /chat slash command is recognized");
    slash = ainiux::editor::parse_editor_slash_command("chat");
    check(slash.command == ainiux::editor::EditorSlashCommand::Chat && slash.path.empty(),
          "editor chat command works without a slash");
    slash = ainiux::editor::parse_editor_slash_command("agent");
    check(slash.command == ainiux::editor::EditorSlashCommand::Agent && slash.path.empty(),
          "editor agent command works without a slash");
    slash = ainiux::editor::parse_editor_slash_command("/editor");
    check(slash.command == ainiux::editor::EditorSlashCommand::Editor && slash.path.empty(),
          "editor /editor command is recognized");
    check(std::find(completions.begin(), completions.end(), "/chat") != completions.end() &&
              std::find(completions.begin(), completions.end(), "/agent") != completions.end() &&
              std::find(completions.begin(), completions.end(), "/editor") != completions.end(),
          "assist command completions include all explicit mode commands");
    slash = ainiux::editor::parse_editor_slash_command("/save extra words");
    check(slash.command == ainiux::editor::EditorSlashCommand::None,
          "editor file slash commands reject multi-token path arguments");

    check(ainiux::editor::editor_assist_path_prefix_length("/open build/") == 6,
          "editor assist path mode starts after /open");
    check(ainiux::editor::editor_assist_path_prefix_length("/saveas foo") == 8,
          "editor assist path mode starts after /saveas");
    check(ainiux::editor::editor_assist_path_prefix_length("/insert build/") == 8,
          "editor assist path mode starts after /insert");
    check(ainiux::editor::editor_assist_path_prefix_length("/open") == std::string::npos,
          "editor assist path mode requires a separator after /open");
    check(ainiux::editor::editor_assist_path_prefix_length("/insert") == std::string::npos,
          "editor assist path mode requires a separator after /insert");
    check(ainiux::editor::editor_assist_path_prefix_length("/search query") == std::string::npos,
          "editor assist path mode ignores non-file commands");
}

void test_editor_assist_path_completion() {
    const std::string directory = "build/ainiux-assist-path-completion";
    std::error_code filesystem_error;
    std::filesystem::create_directories(directory, filesystem_error);
    check(!filesystem_error, "assist path completion fixture directory is created");

    const std::string file = directory + "/target.txt";
    std::ofstream fixture(file, std::ios::binary | std::ios::trunc);
    fixture << "ok";
    check(static_cast<bool>(fixture), "assist path completion fixture file is written");

    ainiux::editor::AssistCompleterState completer;
    std::string input = "/open " + directory + "/tar";
    ainiux::editor::AssistCompletionResult result =
        ainiux::editor::complete_assist_command(input, completer, ainiux::editor::default_editor_assist_config());
    check(result.kind == ainiux::editor::CompletionKind::Path && result.error.ok() &&
              result.match_count == 1,
          "assist path completion finds a unique file after /open");
    check(input == "/open " + file, "assist path completion completes /open PATH");

    completer = ainiux::editor::AssistCompleterState{};
    input = "/insert " + directory + "/tar";
    result = ainiux::editor::complete_assist_command(
        input, completer, ainiux::editor::default_editor_assist_config());
    check(result.kind == ainiux::editor::CompletionKind::Path && result.error.ok() &&
              result.match_count == 1,
          "assist path completion finds a unique file after /insert");
    check(input == "/insert " + file, "assist path completion completes /insert PATH");

    completer = ainiux::editor::AssistCompleterState{};
    input = "/search " + directory + "/tar";
    result = ainiux::editor::complete_assist_command(input, completer, ainiux::editor::default_editor_assist_config());
    check(result.kind == ainiux::editor::CompletionKind::Command,
          "assist tab completion stays in command mode for /search");
    check(input == "/search " + directory + "/tar",
          "assist tab completion does not complete paths for /search");
}

void test_editor_minibuffer_paste() {
    ainiux::editor::MinibufferState minibuffer;
    check(!ainiux::editor::paste_into_minibuffer(minibuffer, "ignored").ok(),
          "minibuffer paste requires an active prompt");

    ainiux::editor::start_minibuffer(minibuffer,
                                     ainiux::editor::MinibufferAction::AssistCommand,
                                     "Command: ",
                                     "/insert ");
    check(ainiux::editor::paste_into_minibuffer(
              minibuffer, "https://example.com/page?x=1&y=2\r\n").ok(),
          "command minibuffer accepts a pasted URL with a trailing newline");
    check(minibuffer.input == "/insert https://example.com/page?x=1&y=2",
          "command minibuffer strips trailing paste newlines");

    const std::string before_multiline = minibuffer.input;
    check(!ainiux::editor::paste_into_minibuffer(minibuffer, "first\nsecond").ok(),
          "command minibuffer rejects multiline paste");
    check(minibuffer.input == before_multiline,
          "rejected multiline paste leaves command input unchanged");

    ainiux::editor::start_minibuffer(minibuffer,
                                     ainiux::editor::MinibufferAction::ConfirmQuit,
                                     "Quit? ");
    check(!ainiux::editor::paste_into_minibuffer(minibuffer, "y").ok(),
          "confirmation minibuffers reject pasted answers");

    ainiux::editor::start_minibuffer(minibuffer,
                                     ainiux::editor::MinibufferAction::ConfirmReasoning,
                                     "Use unlisted reasoning? ");
    check(!ainiux::editor::paste_into_minibuffer(minibuffer, "y").ok(),
          "reasoning confirmation rejects pasted answers");
}

void test_editor_missing_file_error_message() {
    ainiux::editor::FileLoadCheck load_check;
    ainiux::editor::EditorSettings settings;
    ainiux::Error err =
        ainiux::editor::check_load_file_size("this_file_doesnt_exist.txt", settings, load_check);
    check(!err.ok() && err.message == "file not found: this_file_doesnt_exist.txt",
          "editor missing file load reports file not found");
}

void test_editor_buffer_list_helpers() {
    std::vector<ainiux::editor::EditorState> buffers;
    ainiux::editor::EditorState first = ainiux::editor::EditorState::from_text("alpha");
    first.path = "file1.txt";
    first.cursor = first.text.size();
    buffers.push_back(first);

    ainiux::editor::EditorState second = ainiux::editor::EditorState::from_text("beta\nsecond");
    second.path = "file2.txt";
    second.dirty = true;
    second.cursor = second.text.size();
    buffers.push_back(second);

    ainiux::editor::EditorState scratch;
    buffers.push_back(scratch);

    const std::string rendered = ainiux::editor::editor_buffer_list_text(buffers, 1);
    check(rendered.find("Buffers - Enter opens - N new - DEL close - Esc cancels") != std::string::npos,
          "editor buffer list includes chooser instructions");
    check(rendered.find("  file1.txt - Ln 1, Col 6") != std::string::npos,
          "editor buffer list renders an inactive clean file");
    check(rendered.find("> file2.txt * - Ln 2, Col 7") != std::string::npos,
          "editor buffer list marks selected dirty file");
    check(rendered.find("  [scratch 3] - Ln 1, Col 1") != std::string::npos,
          "editor buffer list renders scratch buffers with stable labels");

    check(ainiux::editor::move_editor_buffer_selection(1, buffers.size(), ainiux::editor::MovementKey::Up) == 0,
          "editor buffer list moves selection up");
    check(ainiux::editor::move_editor_buffer_selection(0, buffers.size(), ainiux::editor::MovementKey::Down) == 1,
          "editor buffer list moves selection down");
    check(ainiux::editor::move_editor_buffer_selection(1, buffers.size(), ainiux::editor::MovementKey::Home) == 0,
          "editor buffer list home selects first buffer");
    check(ainiux::editor::move_editor_buffer_selection(0, buffers.size(), ainiux::editor::MovementKey::End) == 2,
          "editor buffer list end selects last buffer");

    ainiux::editor::Clipboard clipboard;
    first.select_all();
    check(first.copy_selection(clipboard).ok(), "editor copies from one buffer");
    second.cursor = second.text.size();
    check(second.paste(clipboard).ok(), "editor pastes copied text into another buffer");
    check(second.text.str() == "beta\nsecondalpha",
          "editor clipboard content is independent of the source buffer");
}

void test_editor_markdown_mode_and_structured_highlighting() {
    ainiux::editor::EditorState state =
        ainiux::editor::EditorState::from_text("# Heading and *emphasis*");
    state.set_path("README.MD");
    state.highlight_enabled = true;
    check(state.language == ainiux::highlight::Language::Markdown && state.language_automatic,
          "editor automatically detects Markdown case-insensitively");

    ainiux::editor::EditorState new_file_state;
    new_file_state.set_path("definitely-does-not-exist-yet.md");
    check(new_file_state.language == ainiux::highlight::Language::Markdown &&
              new_file_state.language_automatic,
          "editor detects Markdown from a new path before the file exists");

    const std::vector<std::pair<const char*, ainiux::highlight::Language>> detected_modes = {
        {"new.py", ainiux::highlight::Language::Python},
        {"new.c", ainiux::highlight::Language::C},
        {"new.hpp", ainiux::highlight::Language::Cpp},
        {"new.cs", ainiux::highlight::Language::CSharp},
        {"new.java", ainiux::highlight::Language::Java},
        {"new.jsx", ainiux::highlight::Language::JavaScript},
        {"new.tsx", ainiux::highlight::Language::TypeScript},
        {"new.html", ainiux::highlight::Language::Html},
        {"new.css", ainiux::highlight::Language::Css},
        {"new.xml", ainiux::highlight::Language::Xml},
        {"new.jsonl", ainiux::highlight::Language::Json},
        {"new.sh", ainiux::highlight::Language::Bash},
        {"new.php", ainiux::highlight::Language::Php},
        {"new.pl", ainiux::highlight::Language::Perl},
        {"new.rb", ainiux::highlight::Language::Ruby},
        {"new.rs", ainiux::highlight::Language::Rust},
        {"new.go", ainiux::highlight::Language::Go},
        {"new.ps1", ainiux::highlight::Language::PowerShell},
        {"new.asm", ainiux::highlight::Language::Assembly},
        {"new.sql", ainiux::highlight::Language::Sql},
        {"new.toml", ainiux::highlight::Language::Toml},
        {"new.yaml", ainiux::highlight::Language::Yaml},
        {"new.ini", ainiux::highlight::Language::Ini},
    };
    for (const auto& detected : detected_modes) {
        ainiux::editor::EditorState detected_state;
        detected_state.set_path(detected.first);
        check(detected_state.language == detected.second && detected_state.language_automatic,
              std::string("editor automatically selects mode for new file: ") + detected.first);
    }

    state.selection.anchor = 2;
    state.selection.active = 8;
    const ainiux::editor::RenderedPanel rendered = state.render({1, 1, 1, 40});
    check(rendered.lines.size() == 1 && rendered.lines[0].find("\x1b") == std::string::npos,
          "editor rendered text contains no embedded ANSI selection markup");
    bool saw_heading = false;
    bool saw_selected_heading = false;
    for (const ainiux::editor::RenderedPanel::Span& span : rendered.line_spans[0]) {
        saw_heading = saw_heading ||
                      (span.syntax && span.role == ainiux::highlight::TokenRole::Heading);
        saw_selected_heading = saw_selected_heading ||
                               (span.syntax && span.selected &&
                                span.role == ainiux::highlight::TokenRole::Heading);
    }
    check(saw_heading, "editor rendering includes Markdown heading spans");
    check(saw_selected_heading,
          "editor rendering overlays selection independently on Markdown syntax spans");

    ainiux::editor::EditorState python_state =
        ainiux::editor::EditorState::from_text("def greet(name: str): return 17");
    python_state.set_language(ainiux::highlight::Language::Python, false);
    python_state.highlight_enabled = true;
    const ainiux::editor::RenderedPanel python_rendered = python_state.render({1, 1, 1, 40});
    bool saw_python_keyword = false;
    bool saw_python_type = false;
    for (const ainiux::editor::RenderedPanel::Span& span : python_rendered.line_spans[0]) {
        saw_python_keyword = saw_python_keyword ||
                             (span.syntax && span.role == ainiux::highlight::TokenRole::Keyword);
        saw_python_type = saw_python_type ||
                          (span.syntax && span.role == ainiux::highlight::TokenRole::Type);
    }
    check(saw_python_keyword && saw_python_type,
          "editor rendering applies a manually selected programming-language mode");

    state.set_language(ainiux::highlight::Language::Text, false);
    state.set_path("renamed.md");
    check(state.language == ainiux::highlight::Language::Text && !state.language_automatic,
          "editor manual text mode survives save-as path changes");
    state.language_automatic = true;
    state.redetect_language();
    check(state.language == ainiux::highlight::Language::Markdown,
          "editor automatic mode resumes filename detection");
    state.set_path("renamed.txt");
    check(state.language == ainiux::highlight::Language::Text,
          "editor automatic mode re-detects after save-as");

    state.linebreak = ainiux::editor::LineBreak::Lf;
    const std::string status = ainiux::editor::editor_status_line(state);
    check(status.find("(text LF)") != std::string::npos &&
              status.find("Mode: Editor") == std::string::npos &&
              status.find("Syntax:") == std::string::npos &&
              status.find("(auto)") == std::string::npos &&
              status.find("(manual)") == std::string::npos,
          "editor status line displays the compact syntax and LF mode");
    state.linebreak = ainiux::editor::LineBreak::Crlf;
    check(ainiux::editor::editor_status_line(state).find("(text CRLF)") != std::string::npos,
          "editor status line displays CRLF mode");
    state.linebreak = ainiux::editor::LineBreak::Cr;
    check(ainiux::editor::editor_status_line(state).find("(text CR)") != std::string::npos,
          "editor status line displays CR mode");
}

void run_all() {
    test_system_clipboard_helpers();
    test_system_clipboard_cancellation_and_limits();
    test_osc52_clipboard_decode();
    test_editor_file_locking_and_read_only_sessions();
    test_editor_control_key_sequence_decode();
    test_editor_save_as_overwrite_helpers();
    test_editor_help_document_and_command();
    test_editor_assist_path_completion();
    test_editor_minibuffer_paste();
    test_editor_missing_file_error_message();
    test_editor_buffer_list_helpers();
    test_editor_markdown_mode_and_structured_highlighting();
    test_editor_ai_continue_helpers();
    test_editor_ai_setup_helpers();
    test_editor_reasoning_picker();
    test_chat_editor_reasoning_context_sync();
    test_editor_file_io_failures();
    test_editor_assist_helpers();
    test_editor_contextual_completion_modes();
    test_editor_file_round_trip();
    test_editor_linebreak_modes();
    test_editor_indentation_detection();
    test_editor_tab_indentation();
    test_editor_word_completion();
    test_editor_language_reformatting();
    test_editor_home_end_navigation();
    test_editor_select_all();
    test_editor_line_home_end_navigation();
    test_editor_wrapped_line_home_end_navigation();
    test_editor_invalid_utf8_rendering_is_sanitized();
    test_editor_kill_to_line_end();
    test_editor_movement_sequence_parse();
    test_editor_page_navigation();
    test_editor_paste_prefers_local_clipboard();
    test_editor_path_completion();
    test_expand_user_path();
    test_editor_piece_table_edits();
    test_editor_rectangular_rendering();
    test_editor_search_navigation();
    test_editor_search_replace();
    test_editor_selection_and_clipboard();
    test_editor_autosave();
    test_editor_undo_redo_key_bindings();
    test_editor_revert_to_snapshot();
    test_editor_undo_redo();
    test_editor_split_layout();
    test_editor_unicode_combining_sequence_wraps_on_grapheme_boundary();
    test_editor_unicode_display_columns_and_offsets();
    test_editor_unicode_emoji_pair_wraps_on_cell_boundaries();
    test_editor_unicode_emoji_skin_tone_wrap_keeps_modifier();
    test_editor_unicode_emoji_zwj_wrap_keeps_sequence_intact();
    test_editor_unicode_grapheme_navigation_and_delete();
    test_editor_unicode_rendering_wraps_on_cell_boundaries();
    test_editor_unicode_selection_search_replace_and_file_round_trip();
    test_editor_utf8_codepoint_navigation_and_editing();
    test_editor_vertical_navigation_modes();
    test_editor_word_wrap_breaks_on_spaces();
    test_editor_word_wrap_rendering();
}

}  // namespace ainiux::test::editor
