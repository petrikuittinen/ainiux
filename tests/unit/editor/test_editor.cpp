#include "editor/test_editor.hpp"
#include "support/test_support.hpp"
#include "common.hpp"
#include "cli/args.hpp"
#include "config/config.hpp"
#include "editor/ai_continue.hpp"
#include "editor/autosave.hpp"
#include "editor/editor_ai_setup.hpp"
#include "editor/clipboard.hpp"
#include "editor/editor.hpp"
#include "editor/editor_assist.hpp"
#include "editor/editor_help.hpp"
#include "editor/file_session.hpp"
#include "editor/path_completion.hpp"
#include "editor/reformat.hpp"
#include "editor/selection.hpp"
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
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace pkchat::test::editor {

namespace {

using pkchat::test::check;
using pkchat::test::read_fixture;

void test_editor_ai_continue_helpers() {
    pkchat::editor::PieceTable text = pkchat::editor::PieceTable::from_string("abcdefghij");
    check(text.range_text(2, 4) == "cdef", "range_text returns a bounded substring");
    check(text.range_text(0, 100) == "abcdefghij", "range_text clamps to buffer size");

    check(pkchat::editor::continue_status_message("custom_openai_chat", "gpt-test", "thinking... ESC to abort") ==
              "[custom / gpt-test] thinking... ESC to abort",
          "continue status message uses compact provider display names");

    pkchat::provider::ChatResult continue_result;
    continue_result.ttft_ms = 100;
    continue_result.total_ms = 1100;
    continue_result.completion_tokens = 20;
    continue_result.completion_tokens_estimated = true;
    check(pkchat::editor::continue_completion_status_message("custom_openai_chat",
                                                             "gpt-test",
                                                             continue_result,
                                                             true) ==
              "[custom / gpt-test] | TTFT: 100 ms | Token/s: 20.0 (estimated)",
          "continue completion status reuses TUI generation metrics formatting");

    pkchat::editor::EditorState state = pkchat::editor::EditorState::from_text("Once upon a ");
    state.cursor = state.text.size();
    check(state.insert_without_undo("time").ok(), "stream insert succeeds");
    check(state.text.str() == "Once upon a time", "stream insert appends at cursor");
    check(!state.can_undo(), "stream insert does not create undo entries by itself");
    check(state.insert("!").ok(), "normal insert after stream chunk creates undo");
    check(state.undo(), "undo after stream chunk succeeds");
    check(state.text.str() == "Once upon a time", "undo removes only the normal insert");

    pkchat::editor::EditorState gap =
        pkchat::editor::EditorState::from_text("beforeAFTER");
    gap.cursor = 6;
    const pkchat::editor::EditorSnapshot gap_before = gap.capture_state();
    check(gap.insert_without_undo(" inserted ").ok() &&
              gap.text.str() == "before inserted AFTER",
          "stream insertion leaves the original postfix bytes after inserted code");
    check(gap.insert_without_undo("code").ok(), "a second streamed code chunk inserts at the gap");
    gap.finalize_stream_edit(gap_before);
    check(gap.undo() && gap.text.str() == "beforeAFTER",
          "all streamed gap-completion chunks are one undoable edit");

    pkchat::editor::EditorState cancelled_gap =
        pkchat::editor::EditorState::from_text("leftRIGHT");
    cancelled_gap.cursor = 4;
    const pkchat::editor::EditorSnapshot cancelled_before = cancelled_gap.capture_state();
    check(cancelled_gap.insert_without_undo("partial  \n").ok(),
          "partial code output can be inserted before cancellation");
    cancelled_gap.finalize_stream_edit(cancelled_before);
    check(cancelled_gap.text.str() == "leftpartial  \nRIGHT",
          "cancellation commit keeps partial code and its exact trailing whitespace");
    check(cancelled_gap.undo() && cancelled_gap.text.str() == "leftRIGHT",
          "cancelled partial code remains a single undoable edit");

    const char* none_argv[] = {"pkchat", "--provider", "none", "--editor"};
    pkchat::cli::ParseResult none_parsed = pkchat::cli::parse_args(4, const_cast<char**>(none_argv));
    check(none_parsed.error.ok(), "none provider editor args parse");
    pkchat::provider::ContextResult context = pkchat::provider::build_context(none_parsed.options);
    check(context.error.ok(), "none provider context builds");
    pkchat::editor::AiContinueContext ai_continue;
    ai_continue.request = context.context;
    ai_continue.settings = pkchat::editor::ai_continue_settings(pkchat::cli::Options{});
    check(!pkchat::editor::validate_continue_request(ai_continue).ok(),
          "none provider rejects AI continue");

    const char* lm_argv[] = {"pkchat", "lmstudio", "--editor"};
    pkchat::cli::ParseResult lm_parsed = pkchat::cli::parse_args(3, const_cast<char**>(lm_argv));
    check(lm_parsed.error.ok(), "lmstudio editor args without model parse");
    context = pkchat::provider::build_context(lm_parsed.options);
    check(context.error.ok(), "lmstudio provider context builds without model");
    check(pkchat::provider::profile_auto_selects_default_model(context.context.profile,
                                                             context.context.base_url),
          "lmstudio editor auto-selects the first model");
    ai_continue.request = context.context;
    check(pkchat::editor::validate_continue_request(ai_continue).code == pkchat::ErrorCode::BadArgs,
          "continue validation still requires a resolved model");

    const char* localhost_argv[] = {"pkchat", "http://localhost:30000/v1", "--editor"};
    pkchat::cli::ParseResult localhost_parsed = pkchat::cli::parse_args(3, const_cast<char**>(localhost_argv));
    check(localhost_parsed.error.ok(), "localhost editor args without model parse");
    pkchat::provider::ContextResult localhost_context =
        pkchat::provider::build_context(localhost_parsed.options);
    check(localhost_context.error.ok(), "localhost custom endpoint context builds without model");
    check(pkchat::provider::profile_auto_selects_default_model(localhost_context.context.profile,
                                                               localhost_context.context.base_url),
          "localhost custom endpoint auto-selects the first model");

    pkchat::provider::RequestContext openai_context;
    openai_context.profile.name = "openai";
    check(!pkchat::provider::profile_auto_selects_default_model(openai_context.profile,
                                                                openai_context.base_url),
          "openai editor does not auto-select a model");

    const char* lm_model_argv[] = {"pkchat", "lmstudio", "-m", "mock-model", "--editor"};
    pkchat::cli::ParseResult lm_model_parsed = pkchat::cli::parse_args(5, const_cast<char**>(lm_model_argv));
    check(lm_model_parsed.error.ok(), "lmstudio provider editor args parse");
    context = pkchat::provider::build_context(lm_model_parsed.options);
    check(context.error.ok(), "lmstudio provider context builds");
    ai_continue.request = context.context;
    ai_continue.settings.max_output_tokens = 1234;
    check(pkchat::editor::validate_continue_request(ai_continue).ok(), "configured provider allows continue");
    const pkchat::provider::RequestContext job_context = pkchat::editor::continue_request_context(ai_continue);
    check(job_context.options.stream, "continue forces streaming");
    check(job_context.options.has_max_output_tokens, "continue sets max output tokens");
    check(job_context.options.max_output_tokens == 1234, "continue uses configured token limit");

    pkchat::cli::Options default_options;
    const pkchat::editor::AiContinueSettings default_settings =
        pkchat::editor::ai_continue_settings(default_options);
    check(default_settings.max_prefix_chars ==
                  pkchat::editor::kDefaultAiContinuePrefixMaxChars &&
              default_settings.max_postfix_chars ==
                  pkchat::editor::kDefaultAiContinuePostfixMaxChars &&
              default_settings.max_prose_prefix_chars ==
                  pkchat::editor::kDefaultAiContinueProsePrefixMaxChars &&
              default_settings.max_prose_postfix_chars ==
                  pkchat::editor::kDefaultAiContinueProsePostfixMaxChars,
          "default continue context uses independent 4000/2000 code and 16384/4096 prose limits");
    pkchat::cli::Options cli_settings_options = default_options;
    cli_settings_options.editor_ai_continue_prefix_max_chars = 99;
    cli_settings_options.editor_ai_continue_postfix_max_chars = 88;
    cli_settings_options.editor_ai_continue_prose_prefix_max_chars = 77;
    cli_settings_options.editor_ai_continue_prose_postfix_max_chars = 66;
    const pkchat::editor::AiContinueSettings cli_settings =
        pkchat::editor::ai_continue_settings(cli_settings_options);
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
    const pkchat::editor::AiContinueSettings env_settings =
        pkchat::editor::ai_continue_settings(cli_settings_options);
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
    const pkchat::editor::AiContinueSettings invalid_env_settings =
        pkchat::editor::ai_continue_settings(cli_settings_options);
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
    const pkchat::editor::AiContinueSettings zero_env_settings =
        pkchat::editor::ai_continue_settings(default_options);
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

void test_editor_ai_setup_helpers() {
    check(pkchat::editor::editor_no_provider_message() ==
              "No provider chosen. Use /provider to choose one",
          "editor no-provider message mentions /provider");
    check(pkchat::editor::editor_no_model_message() == "No model chosen. Use /model to choose one",
          "editor no-model message mentions /model");

    std::optional<pkchat::editor::AiContinueContext> no_context;
    check(!pkchat::editor::editor_ai_has_provider(no_context), "missing context has no provider");
    check(!pkchat::editor::editor_ai_ready(no_context), "missing context is not AI-ready");
    check(pkchat::editor::editor_startup_status(no_context).find("/provider") != std::string::npos,
          "startup status without context mentions /provider");

    const char* none_argv[] = {"pkchat", "--provider", "none", "--editor"};
    pkchat::cli::ParseResult none_parsed = pkchat::cli::parse_args(4, const_cast<char**>(none_argv));
    check(none_parsed.error.ok(), "none provider editor args parse for ai setup");
    pkchat::provider::ContextResult none_context = pkchat::provider::build_context(none_parsed.options);
    check(none_context.error.ok(), "none provider context builds for ai setup");
    pkchat::editor::AiContinueContext offline_continue;
    offline_continue.request = none_context.context;
    check(!pkchat::editor::editor_ai_has_provider(offline_continue),
          "offline provider is not considered chosen");
    check(pkchat::editor::editor_startup_status(offline_continue).find("Local editor") != std::string::npos,
          "offline startup status mentions local editor");

    const char* lm_model_argv[] = {"pkchat", "lmstudio", "-m", "mock-model", "--editor"};
    pkchat::cli::ParseResult lm_model_parsed = pkchat::cli::parse_args(5, const_cast<char**>(lm_model_argv));
    check(lm_model_parsed.error.ok(), "lmstudio editor args parse for ai setup");
    pkchat::provider::ContextResult ready_context = pkchat::provider::build_context(lm_model_parsed.options);
    check(ready_context.error.ok(), "lmstudio provider context builds for ai setup");
    pkchat::editor::AiContinueContext ready_continue;
    ready_continue.request = ready_context.context;
    ready_continue.assist_config = pkchat::editor::default_editor_assist_config();
    check(pkchat::editor::editor_ai_has_provider(ready_continue), "configured provider is chosen");
    ready_continue.request.options.model = "mock-model";
    check(pkchat::editor::editor_ai_ready(ready_continue), "provider with model is AI-ready");
    const std::string ready_startup_status = pkchat::editor::editor_startup_status(ready_continue);
    check(ready_startup_status.find("[lmstudio / mock-model]") == 0,
          "ready startup status shows provider and model like chat mode");
    check(ready_startup_status.find("ready") != std::string::npos,
          "ready startup status mentions ready");

    std::optional<pkchat::editor::AiContinueContext> created;
    pkchat::editor::EditorAssistConfig assist_config = pkchat::editor::default_editor_assist_config();
    check(pkchat::editor::ensure_editor_ai_context(created, assist_config).ok(),
          "ensure_editor_ai_context creates offline context");
    check(created.has_value(), "ensure_editor_ai_context populates optional");
    check(!pkchat::editor::editor_ai_has_provider(created), "created default context stays offline");

    check(pkchat::editor::apply_editor_model(created, "mock-model").code ==
              pkchat::ErrorCode::UnsupportedFeature,
          "apply_editor_model requires a provider first");
    check(pkchat::editor::apply_editor_provider_target(created, assist_config, "openai").ok(),
          "apply_editor_provider_target can switch to openai");
    check(pkchat::editor::editor_ai_has_provider(created), "openai provider is active after apply");
    check(pkchat::editor::apply_editor_model(created, "gpt-test").ok(),
          "apply_editor_model succeeds after provider is chosen");
    check(created->request.options.model == "gpt-test", "apply_editor_model stores model name");

    const char* openrouter_argv[] = {"pkchat", "openrouter", "--editor"};
    pkchat::cli::ParseResult openrouter_parsed =
        pkchat::cli::parse_args(3, const_cast<char**>(openrouter_argv));
    check(openrouter_parsed.error.ok(), "openrouter editor args without model parse");
    pkchat::provider::ContextResult openrouter_context =
        pkchat::provider::build_context(openrouter_parsed.options);
    check(openrouter_context.error.ok(), "openrouter provider context builds without model");
    check(openrouter_context.context.options.model.empty(),
          "openrouter editor startup leaves model empty");
    pkchat::editor::AiContinueContext openrouter_continue;
    openrouter_continue.request = openrouter_context.context;
    check(pkchat::editor::editor_ai_has_provider(openrouter_continue),
          "openrouter editor startup has provider");
    check(!pkchat::editor::editor_ai_ready(openrouter_continue),
          "openrouter editor startup is not AI-ready without model");
    const std::string openrouter_startup_status =
        pkchat::editor::editor_startup_status(openrouter_continue);
    check(openrouter_startup_status.find("/model") != std::string::npos,
          "openrouter editor startup status mentions /model");
    check(openrouter_startup_status.find("Choose a model with /model") != std::string::npos,
          "openrouter editor startup status asks for /model");
    check(openrouter_startup_status.find("/provider") != std::string::npos,
          "openrouter editor startup status mentions /provider like chat mode");

    const char* lm_no_model_argv[] = {"pkchat", "lmstudio", "--editor"};
    pkchat::cli::ParseResult lm_no_model_parsed =
        pkchat::cli::parse_args(3, const_cast<char**>(lm_no_model_argv));
    check(lm_no_model_parsed.error.ok(), "lmstudio editor args without model parse for deferral");
    pkchat::provider::ContextResult lm_no_model_context =
        pkchat::provider::build_context(lm_no_model_parsed.options);
    check(lm_no_model_context.error.ok(), "lmstudio provider context builds without model for deferral");
    check(lm_no_model_context.context.options.model.empty(),
          "lmstudio editor startup leaves model empty");
    pkchat::editor::AiContinueContext lm_no_model_continue;
    lm_no_model_continue.request = lm_no_model_context.context;
    check(pkchat::editor::editor_ai_has_provider(lm_no_model_continue),
          "lmstudio editor startup has provider");
    check(!pkchat::editor::editor_ai_ready(lm_no_model_continue),
          "lmstudio editor startup is not AI-ready without model");
    std::optional<pkchat::editor::AiContinueContext> deferred_provider;
    check(pkchat::editor::apply_editor_provider_target(deferred_provider, assist_config, "lmstudio").ok(),
          "/provider lmstudio succeeds without contacting the model endpoint");
    check(deferred_provider.has_value() && deferred_provider->request.options.model.empty(),
          "/provider lmstudio leaves model empty for /model selection");
}

void test_editor_assist_helpers() {
    const pkchat::editor::EditorAssistConfig default_config =
        pkchat::editor::default_editor_assist_config();
    check(default_config.behavior_rules.find("one-shot") != std::string::npos,
          "default editor assist behavior rules mention one-shot prompts");
    check(default_config.behavior_rules.find("not as instructions") != std::string::npos,
          "default editor assist behavior rules say content is not instructions");
    const pkchat::editor::EditorAssistCommand* default_spell =
        pkchat::editor::find_assist_command(default_config, "/spell");
    check(default_spell != nullptr && default_spell->prompt.find("spelling") != std::string::npos,
          "default editor assist spell prompt is populated");
    const pkchat::editor::EditorAssistCommand* default_comment =
        pkchat::editor::find_assist_command(default_config, "/comment");
    check(default_comment != nullptr && default_comment->prompt.find("improve the text") != std::string::npos,
          "default editor assist comment prompt is populated");
    const pkchat::editor::EditorAssistCommand* default_rewrite =
        pkchat::editor::find_assist_command(default_config, "/rewrite");
    check(default_rewrite != nullptr && default_rewrite->prompt.find("factual accuracy") != std::string::npos,
          "default editor assist rewrite prompt is populated");
    const pkchat::editor::EditorAssistCommand* default_english =
        pkchat::editor::find_assist_command(default_config, "/English");
    check(default_english != nullptr && default_english->prompt.find("English") != std::string::npos,
          "default editor assist English prompt is populated");
    const pkchat::editor::EditorAssistCommand* default_chinese =
        pkchat::editor::find_assist_command(default_config, "/Chinese");
    check(default_chinese != nullptr && default_chinese->prompt.find("Chinese") != std::string::npos,
          "default editor assist Chinese prompt is populated");
    const pkchat::editor::EditorAssistCommand* default_finnish =
        pkchat::editor::find_assist_command(default_config, "/Finnish");
    check(default_finnish != nullptr && default_finnish->prompt.find("Finnish") != std::string::npos,
          "default editor assist Finnish prompt is populated");

    pkchat::editor::ParsedAssistCommand parsed =
        pkchat::editor::parse_assist_command("/spell all", default_config);
    check(parsed.ok && parsed.kind == pkchat::editor::AssistCommandKind::Configured &&
              parsed.scope == pkchat::editor::AssistScope::All,
          "/spell all parses");

    parsed = pkchat::editor::parse_assist_command("spell all", default_config);
    check(parsed.ok && parsed.kind == pkchat::editor::AssistCommandKind::Configured &&
              parsed.scope == pkchat::editor::AssistScope::All,
          "slashless editor assist commands parse");
    parsed = pkchat::editor::parse_assist_command("CHINESE n", default_config);
    check(parsed.ok && parsed.kind == pkchat::editor::AssistCommandKind::Configured &&
              parsed.scope == pkchat::editor::AssistScope::NewBuffer,
          "slashless editor assist commands are case-insensitive");

    parsed = pkchat::editor::parse_assist_command("/grammar selection", default_config);
    check(parsed.ok && parsed.kind == pkchat::editor::AssistCommandKind::Configured &&
              parsed.scope == pkchat::editor::AssistScope::Selection,
          "/grammar selection parses");

    parsed = pkchat::editor::parse_assist_command("/spell", default_config);
    check(parsed.ok && parsed.kind == pkchat::editor::AssistCommandKind::Configured &&
              !parsed.scope.has_value(),
          "bare /spell requests scope");

    parsed = pkchat::editor::parse_assist_command("/continue", default_config);
    check(parsed.ok && parsed.kind == pkchat::editor::AssistCommandKind::Configured &&
              !parsed.scope.has_value(),
          "bare /continue runs without scope");

    parsed = pkchat::editor::parse_assist_command("/fact newbuffer", default_config);
    check(parsed.ok && parsed.kind == pkchat::editor::AssistCommandKind::Configured &&
              parsed.scope == pkchat::editor::AssistScope::NewBuffer,
          "/fact newbuffer parses");

    parsed = pkchat::editor::parse_assist_command("/fact n", default_config);
    check(parsed.ok && parsed.kind == pkchat::editor::AssistCommandKind::Configured &&
              parsed.scope == pkchat::editor::AssistScope::NewBuffer,
          "/fact n parses as new buffer");

    parsed = pkchat::editor::parse_assist_command("/fact insert", default_config);
    check(parsed.ok && parsed.kind == pkchat::editor::AssistCommandKind::Configured &&
              parsed.scope == pkchat::editor::AssistScope::Insert,
          "/fact insert parses");

    parsed = pkchat::editor::parse_assist_command("/comment all", default_config);
    check(parsed.ok && parsed.kind == pkchat::editor::AssistCommandKind::Configured &&
              parsed.scope == pkchat::editor::AssistScope::All,
          "/comment all parses");

    parsed = pkchat::editor::parse_assist_command("/rewrite selection", default_config);
    check(parsed.ok && parsed.kind == pkchat::editor::AssistCommandKind::Configured &&
              parsed.scope == pkchat::editor::AssistScope::Selection,
          "/rewrite selection parses");

    parsed = pkchat::editor::parse_assist_command("/english newbuffer", default_config);
    check(parsed.ok && parsed.kind == pkchat::editor::AssistCommandKind::Configured &&
              parsed.scope == pkchat::editor::AssistScope::NewBuffer,
          "/English parses case-insensitively");

    parsed = pkchat::editor::parse_assist_command("/Chinese n", default_config);
    check(parsed.ok && parsed.kind == pkchat::editor::AssistCommandKind::Configured &&
              parsed.scope == pkchat::editor::AssistScope::NewBuffer,
          "/Chinese n parses as new buffer");

    parsed = pkchat::editor::parse_assist_command("/Finnish all", default_config);
    check(parsed.ok && parsed.kind == pkchat::editor::AssistCommandKind::Configured &&
              parsed.scope == pkchat::editor::AssistScope::All,
          "/Finnish all parses");

    parsed = pkchat::editor::parse_assist_command("/prompt rewrite formally", default_config);
    check(parsed.ok && parsed.kind == pkchat::editor::AssistCommandKind::Prompt &&
              parsed.custom_prompt == "rewrite formally",
          "/prompt captures custom text");
    check(pkchat::editor::assist_prompt_mode_message() ==
              "/prompt for selection (s), all (a), insert (i), new buffer (n)",
          "/prompt offers the standard four scoped AI choices");
    check(pkchat::editor::assist_prompt_mode_for_key('s') ==
                  pkchat::editor::AssistPromptMode::Selection &&
              pkchat::editor::assist_prompt_mode_for_key('A') ==
                  pkchat::editor::AssistPromptMode::All &&
              pkchat::editor::assist_prompt_mode_for_key('i') ==
                  pkchat::editor::AssistPromptMode::Insert &&
              pkchat::editor::assist_prompt_mode_for_key('N') ==
                  pkchat::editor::AssistPromptMode::NewBuffer,
          "/prompt mode keys select all four advertised choices case-insensitively");
    check(!pkchat::editor::assist_prompt_mode_for_key('c').has_value(),
          "/prompt no longer accepts the continue-only mode key");

    parsed = pkchat::editor::parse_assist_command("/prompt", default_config);
    check(!parsed.ok, "bare /prompt is rejected");

    parsed = pkchat::editor::parse_assist_command("/regenerate", default_config);
    check(parsed.ok && parsed.kind == pkchat::editor::AssistCommandKind::Regenerate,
          "/regenerate parses");

    parsed = pkchat::editor::parse_assist_command("/regenerate now", default_config);
    check(!parsed.ok, "/regenerate rejects arguments");

    parsed = pkchat::editor::parse_assist_command("/search pkchat cli", default_config);
    check(parsed.ok && parsed.kind == pkchat::editor::AssistCommandKind::WebSearch,
          "/search parses with query");
    check(parsed.custom_prompt == "pkchat cli", "/search stores the query text");

    parsed = pkchat::editor::parse_assist_command("/search", default_config);
    check(!parsed.ok, "bare /search is rejected");

    parsed = pkchat::editor::parse_assist_command("/quit", default_config);
    check(parsed.ok && parsed.kind == pkchat::editor::AssistCommandKind::Quit, "/quit parses");

    parsed = pkchat::editor::parse_assist_command("//quit", default_config);
    check(parsed.ok && parsed.kind == pkchat::editor::AssistCommandKind::Quit,
          "duplicate leading slashes in /quit are accepted");

    const std::vector<std::string> completions =
        pkchat::editor::assist_command_completions(default_config);
    check(!completions.empty() && completions.front() == "/spell", "assist completions include /spell");
    check(std::find(completions.begin(), completions.end(), "/regenerate") != completions.end(),
          "assist completions include /regenerate");
    for (const char* builtin : {"/spell", "/grammar", "/fact", "/comment", "/rewrite", "/English",
                                "/Chinese", "/Finnish"}) {
        for (const char* mode : {"selection", "all", "newbuffer", "insert"}) {
            const std::string variant = std::string(builtin) + " " + mode;
            check(std::find(completions.begin(), completions.end(), variant) != completions.end(),
                  std::string("builtin assist completions include ") + variant);
        }
    }
    check(std::find(completions.begin(), completions.end(), "/continue") != completions.end(),
          "builtin assist completions include bare /continue");
    for (const char* builtin : {"/spell", "/grammar", "/fact", "/comment", "/rewrite", "/English",
                                "/Chinese", "/Finnish"}) {
        const pkchat::editor::EditorAssistCommand* command =
            pkchat::editor::find_assist_command(default_config, builtin);
        check(command != nullptr && command->modes.size() == 4,
              std::string("default ") + builtin +
                  " exposes selection, all, newbuffer, and insert modes");
        const std::string scope_prompt = pkchat::editor::assist_scope_prompt(*command);
        check(scope_prompt.find("selection (s)") != std::string::npos &&
                  scope_prompt.find("all (a)") != std::string::npos &&
                  scope_prompt.find("new buffer (n)") != std::string::npos &&
                  scope_prompt.find("insert (i)") != std::string::npos &&
                  scope_prompt.find("continue (c)") == std::string::npos,
              std::string("default ") + builtin + " scope prompt lists scoped modes without continue");
    }
    {
        const pkchat::editor::EditorAssistCommand* continue_command =
            pkchat::editor::find_assist_command(default_config, "/continue");
        check(continue_command != nullptr && continue_command->modes.size() == 1,
              "default /continue exposes only continue mode");
    }

    std::string input = "/sp";
    pkchat::editor::AssistCompleterState completer;
    pkchat::editor::AssistCompletionResult completion =
        pkchat::editor::complete_assist_command(input, completer, default_config);
    check(completion.changed && input.rfind("/spell", 0) == 0, "assist tab completion expands /sp");

    input = "/";
    completer = pkchat::editor::AssistCompleterState{};
    pkchat::editor::complete_assist_command(input, completer, default_config);
    check(completer.active && input == "/", "assist tab completion on / enters cycle mode");
    input += "fa";
    completion = pkchat::editor::complete_assist_command(input, completer, default_config);
    check(input == "/fact", "assist tab completion rematches after editing / to /fa");
    check(completer.active && completer.candidates.size() == 5,
          "/fa matches /fact and its four scoped variants for cycling");

    input = "/";
    completer = pkchat::editor::AssistCompleterState{};
    pkchat::editor::complete_assist_command(input, completer, default_config);
    input = "/q";
    completion = pkchat::editor::complete_assist_command(input, completer, default_config);
    check(input == "/quit", "assist tab completion rematches /q after stale / cycle state");

    input = "/en";
    completer = pkchat::editor::AssistCompleterState{};
    completion = pkchat::editor::complete_assist_command(input, completer, default_config);
    check(completion.changed && input == "/English",
          "assist tab completion matches capitalized commands case-insensitively");
    input = "/c";
    completer = pkchat::editor::AssistCompleterState{};
    completion = pkchat::editor::complete_assist_command(input, completer, default_config);
    check(!completion.changed && input == "/c" && completer.active,
          "assist tab completion keeps ambiguous mixed-case /c prefix");

    input = "rew";
    completer = pkchat::editor::AssistCompleterState{};
    completion = pkchat::editor::complete_assist_command(input, completer, default_config);
    check(completion.changed && input == "rewrite",
          "slashless completion preserves the slashless form");
    input = "ch";
    completer = pkchat::editor::AssistCompleterState{};
    completion = pkchat::editor::complete_assist_command(input, completer, default_config);
    check(completion.changed && input == "Chinese",
          "slashless completion handles case-insensitive configured names");
    check(pkchat::editor::editor_assist_path_prefix_length("open notes") == 5 &&
              pkchat::editor::editor_assist_path_prefix_length("/saveas notes") == 8,
          "slashless and slashed path commands expose path completion");

    pkchat::editor::EditorState state =
        pkchat::editor::EditorState::from_text("hello wrld");
    state.selection.anchor = 0;
    state.selection.active = 5;
    pkchat::editor::AiContinueContext context;
    context.request.profile.name = "lm_studio";
    context.request.options.model = "mock-model";
    context.assist_config = default_config;
    const std::optional<size_t> spell_index = pkchat::editor::assist_command_index(default_config, "/spell");
    check(spell_index.has_value(), "default assist config indexes /spell");
    pkchat::editor::AssistExecution execution = pkchat::editor::build_assist_execution(
        state,
        context,
        pkchat::editor::AssistCommandKind::Configured,
        *spell_index,
        pkchat::editor::AssistScope::Selection,
        "",
        std::nullopt);
    check(execution.ok && !execution.stream &&
              execution.edit_kind == pkchat::editor::AssistEditKind::ReplaceInPlace &&
              execution.replace_start == 0 && execution.replace_count == 5,
          "spell selection builds in-place execution");
    check(execution.messages.size() == 2 && execution.messages.front().role == "system" &&
              execution.messages.front().content.find(default_spell->prompt) != std::string::npos &&
              execution.messages.front().content.find(default_config.behavior_rules) != std::string::npos,
          "spell selection uses task prompt plus default assist rules in system message");
    check(execution.messages.back().role == "user" &&
              execution.messages.back().content == "<content>hello</content>",
          "spell selection wraps buffer text in content tags for user message");

    pkchat::editor::EditorState prompt_state =
        pkchat::editor::EditorState::from_text("selected and remaining");
    prompt_state.selection.anchor = 0;
    prompt_state.selection.active = 8;
    execution = pkchat::editor::build_assist_execution(
        prompt_state,
        context,
        pkchat::editor::AssistCommandKind::Prompt,
        0,
        std::nullopt,
        "Rewrite clearly",
        pkchat::editor::AssistPromptMode::Selection);
    check(execution.ok && !execution.stream &&
              execution.edit_kind == pkchat::editor::AssistEditKind::ReplaceInPlace &&
              execution.replace_start == 0 && execution.replace_count == 8,
          "/prompt selection replaces the selected text");

    execution = pkchat::editor::build_assist_execution(
        prompt_state,
        context,
        pkchat::editor::AssistCommandKind::Prompt,
        0,
        std::nullopt,
        "Rewrite clearly",
        pkchat::editor::AssistPromptMode::All);
    check(execution.ok && !execution.stream &&
              execution.edit_kind == pkchat::editor::AssistEditKind::ReplaceInPlace &&
              execution.replace_start == 0 && execution.replace_count == prompt_state.text.size(),
          "/prompt all replaces the whole buffer");

    execution = pkchat::editor::build_assist_execution(
        prompt_state,
        context,
        pkchat::editor::AssistCommandKind::Prompt,
        0,
        std::nullopt,
        "Rewrite clearly",
        pkchat::editor::AssistPromptMode::Insert);
    check(execution.ok && execution.stream &&
              execution.edit_kind == pkchat::editor::AssistEditKind::StreamInsert &&
              execution.messages.back().content == "<content>selected</content>",
          "/prompt insert streams from the selected text at the cursor");

    execution = pkchat::editor::build_assist_execution(
        prompt_state,
        context,
        pkchat::editor::AssistCommandKind::Prompt,
        0,
        std::nullopt,
        "Rewrite clearly",
        pkchat::editor::AssistPromptMode::NewBuffer);
    check(execution.ok && execution.stream &&
              execution.edit_kind == pkchat::editor::AssistEditKind::NewBuffer &&
              execution.messages.back().content == "<content>selected</content>",
          "/prompt new buffer streams from the selected text into a new buffer");

    prompt_state.clear_selection();
    execution = pkchat::editor::build_assist_execution(
        prompt_state,
        context,
        pkchat::editor::AssistCommandKind::Prompt,
        0,
        std::nullopt,
        "Rewrite clearly",
        pkchat::editor::AssistPromptMode::NewBuffer);
    check(!execution.ok &&
              execution.error_message.find("new buffer requires an active selection") !=
                  std::string::npos,
          "/prompt new buffer rejects a missing selection");

    const pkchat::editor::EditorAssistCommand* default_continue =
        pkchat::editor::find_assist_command(default_config, "/continue");
    check(default_continue != nullptr, "default assist config includes /continue");
    const std::optional<size_t> continue_index =
        pkchat::editor::assist_command_index(default_config, "/continue");
    check(continue_index.has_value(), "default assist config indexes /continue");
    context.request.options.system = "Custom system";
    state.cursor = state.text.size();
    execution = pkchat::editor::build_assist_execution(
        state,
        context,
        pkchat::editor::AssistCommandKind::Configured,
        *continue_index,
        pkchat::editor::AssistScope::Continue,
        "",
        std::nullopt);
    check(execution.ok && execution.stream &&
              execution.edit_kind == pkchat::editor::AssistEditKind::StreamInsert &&
              execution.prose_completion && !execution.code_completion,
          "/continue continue builds streaming execution");
    check(execution.messages.back().content.find(
              "PKCHAT_PROSE_CONTEXT_V1\nMODE_BYTES 4\ntext\nPREFIX_BYTES 10\nhello wrld\n"
              "CURSOR_BYTES 9\n<CURSOR/>\nEND_PKCHAT_PROSE_CONTEXT_V1") !=
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
    pkchat::editor::EditorState long_state =
        pkchat::editor::EditorState::from_text(std::string(5000, 'a'));
    long_state.cursor = long_state.text.size();
    execution = pkchat::editor::build_assist_execution(
        long_state,
        context,
        pkchat::editor::AssistCommandKind::Configured,
        *continue_index,
        pkchat::editor::AssistScope::Continue,
        "",
        std::nullopt);
    check(execution.ok, "long /continue continue execution builds");
    check(execution.messages.back().content.size() <
              execution.usage_messages.back().content.size(),
          "/continue continue keeps truncated request text but full-prefix usage messages");
    check(execution.messages.front().content.rfind("Custom system", 0) == 0 &&
              execution.messages.front().content.find(default_continue->prompt) != std::string::npos,
          "user --system is prepended to assist task system prompt");

    pkchat::editor::EditorState markdown_state =
        pkchat::editor::EditorState::from_text("# prose tail");
    markdown_state.cursor = markdown_state.text.size();
    markdown_state.set_language(pkchat::highlight::Language::Markdown, false);
    execution = pkchat::editor::build_assist_execution(
        markdown_state, context, pkchat::editor::AssistCommandKind::Configured,
        *continue_index, pkchat::editor::AssistScope::Continue, "", std::nullopt);
    check(execution.prose_completion && !execution.code_completion &&
              execution.messages.back().content.find("MODE_BYTES 8\nmarkdown\n") !=
                  std::string::npos,
          "Markdown /continue uses dedicated prose continuation framing");

    context.settings.max_prose_prefix_chars = 3;
    context.settings.max_prose_postfix_chars = 4;
    const std::string prose_beta = "\xCE\xB2";
    pkchat::editor::EditorState prose_middle = pkchat::editor::EditorState::from_text(
        std::string("\xCE\xB1") + prose_beta + "AB  ending\xE5\xB0\xBE");
    prose_middle.cursor = std::string("\xCE\xB1").size() + prose_beta.size() + 2;
    execution = pkchat::editor::build_assist_execution(
        prose_middle, context, pkchat::editor::AssistCommandKind::Configured,
        *continue_index, pkchat::editor::AssistScope::Continue, "", std::nullopt);
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
    execution = pkchat::editor::build_assist_execution(
        prose_middle, context, pkchat::editor::AssistCommandKind::Configured,
        *continue_index, pkchat::editor::AssistScope::Continue, "", std::nullopt);
    check(execution.messages.back().content.find("POSTFIX_BYTES 2\n  \n") !=
              std::string::npos,
          "bounded prose postfix preserves an immediate whitespace-only slice exactly");

    prose_middle.cursor = 0;
    context.settings.max_prose_prefix_chars = 0;
    context.settings.max_prose_postfix_chars = 3;
    execution = pkchat::editor::build_assist_execution(
        prose_middle, context, pkchat::editor::AssistCommandKind::Configured,
        *continue_index, pkchat::editor::AssistScope::Continue, "", std::nullopt);
    check(execution.messages.back().content.find("PREFIX_BYTES 0\n\nCURSOR_BYTES 9") !=
                  std::string::npos &&
              execution.messages.back().content.find(
                  std::string("POSTFIX_BYTES 5\n\xCE\xB1\xCE\xB2") + "A\n") !=
                  std::string::npos,
          "prose continuation at buffer start supports disabled prefix and bounded UTF-8 postfix");

    std::string invalid_prose_source = "A";
    invalid_prose_source.push_back(static_cast<char>(0xFF));
    invalid_prose_source += "Brest";
    pkchat::editor::EditorState invalid_prose_state =
        pkchat::editor::EditorState::from_text(invalid_prose_source);
    invalid_prose_state.cursor = 3;
    context.settings.max_prose_prefix_chars = 2;
    context.settings.max_prose_postfix_chars = 2;
    execution = pkchat::editor::build_assist_execution(
        invalid_prose_state, context, pkchat::editor::AssistCommandKind::Configured,
        *continue_index, pkchat::editor::AssistScope::Continue, "", std::nullopt);
    std::string invalid_prose_prefix;
    invalid_prose_prefix.push_back(static_cast<char>(0xFF));
    invalid_prose_prefix += "B";
    check(execution.messages.back().content.find(
              "PREFIX_BYTES 2\n" + invalid_prose_prefix + "\nCURSOR_BYTES") !=
              std::string::npos,
          "invalid UTF-8 prose bytes are preserved and count as one context unit");

    pkchat::editor::EditorState prose_whitespace_postfix =
        pkchat::editor::EditorState::from_text("abc \t\r\n\f\v");
    prose_whitespace_postfix.cursor = 3;
    context.settings.max_prose_prefix_chars = 3;
    context.settings.max_prose_postfix_chars = 20;
    execution = pkchat::editor::build_assist_execution(
        prose_whitespace_postfix, context, pkchat::editor::AssistCommandKind::Configured,
        *continue_index, pkchat::editor::AssistScope::Continue, "", std::nullopt);
    check(execution.messages.back().content.find("POSTFIX_BYTES") == std::string::npos,
          "complete whitespace-only prose remainder is omitted");
    check(execution.messages.front().content.find("Continue at substantial length") !=
              std::string::npos,
          "whitespace-only remainder uses the end-of-document long-form prose prompt");

    context.settings.max_prose_postfix_chars = 0;
    prose_middle.cursor = 4;
    execution = pkchat::editor::build_assist_execution(
        prose_middle, context, pkchat::editor::AssistCommandKind::Configured,
        *continue_index, pkchat::editor::AssistScope::Continue, "", std::nullopt);
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
    pkchat::editor::EditorState python_state =
        pkchat::editor::EditorState::from_text(std::string("\xCE\xB1") + beta + "AB  tail\xE5\xB0\xBE");
    python_state.cursor = std::string("\xCE\xB1").size() + beta.size() + 2;
    python_state.set_language(pkchat::highlight::Language::Python, false);
    python_state.highlight_enabled = false;
    execution = pkchat::editor::build_assist_execution(
        python_state, context, pkchat::editor::AssistCommandKind::Configured,
        *continue_index, pkchat::editor::AssistScope::Continue, "", std::nullopt);
    check(execution.ok && execution.code_completion &&
              execution.completion_language == pkchat::highlight::Language::Python,
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
    execution = pkchat::editor::build_assist_execution(
        python_state, context, pkchat::editor::AssistCommandKind::Configured,
        *continue_index, pkchat::editor::AssistScope::Continue, "", std::nullopt);
    check(execution.messages.back().content.find("POSTFIX_BYTES 2\n  \n") !=
              std::string::npos,
          "bounded postfix is sent exactly even when its immediate slice is whitespace");

    pkchat::editor::EditorState code_start =
        pkchat::editor::EditorState::from_text("print(value)");
    code_start.cursor = 0;
    code_start.set_language(pkchat::highlight::Language::Python, false);
    context.settings.max_prefix_chars = 5;
    context.settings.max_postfix_chars = 3;
    execution = pkchat::editor::build_assist_execution(
        code_start, context, pkchat::editor::AssistCommandKind::Configured,
        *continue_index, pkchat::editor::AssistScope::Continue, "", std::nullopt);
    check(execution.messages.back().content.find("PREFIX_BYTES 0\n\n<CURSOR/>") !=
                  std::string::npos &&
              execution.messages.back().content.find("POSTFIX_BYTES 3\npri\n") !=
                  std::string::npos,
          "code completion at buffer start sends empty prefix and bounded leading postfix");

    code_start.cursor = code_start.text.size();
    execution = pkchat::editor::build_assist_execution(
        code_start, context, pkchat::editor::AssistCommandKind::Configured,
        *continue_index, pkchat::editor::AssistScope::Continue, "", std::nullopt);
    check(execution.messages.back().content.find("PREFIX_BYTES 5\nalue)") !=
                  std::string::npos &&
              execution.messages.back().content.find("POSTFIX_BYTES") == std::string::npos,
          "code completion at buffer end sends bounded prefix and omits empty postfix");

    std::string invalid_source = "A";
    invalid_source.push_back(static_cast<char>(0xFF));
    invalid_source += "Brest";
    pkchat::editor::EditorState invalid_state =
        pkchat::editor::EditorState::from_text(invalid_source);
    invalid_state.cursor = 3;
    invalid_state.set_language(pkchat::highlight::Language::Python, false);
    context.settings.max_prefix_chars = 2;
    context.settings.max_postfix_chars = 2;
    execution = pkchat::editor::build_assist_execution(
        invalid_state, context, pkchat::editor::AssistCommandKind::Configured,
        *continue_index, pkchat::editor::AssistScope::Continue, "", std::nullopt);
    std::string invalid_prefix;
    invalid_prefix.push_back(static_cast<char>(0xFF));
    invalid_prefix += "B";
    check(execution.messages.back().content.find("PREFIX_BYTES 2\n" + invalid_prefix) !=
              std::string::npos,
          "invalid UTF-8 bytes are preserved and count as one continuation context unit");

    pkchat::editor::EditorState whitespace_postfix =
        pkchat::editor::EditorState::from_text("abc \t\r\n\f\v");
    whitespace_postfix.cursor = 3;
    whitespace_postfix.set_language(pkchat::highlight::Language::Json, false);
    context.settings.max_prefix_chars = 0;
    context.settings.max_postfix_chars = 20;
    execution = pkchat::editor::build_assist_execution(
        whitespace_postfix, context, pkchat::editor::AssistCommandKind::Configured,
        *continue_index, pkchat::editor::AssistScope::Continue, "", std::nullopt);
    check(execution.messages.back().content.find("LANGUAGE json\nPREFIX_BYTES 0\n\n<CURSOR/>") !=
                  std::string::npos &&
              execution.messages.back().content.find("POSTFIX_BYTES") == std::string::npos,
          "disabled prefix is empty and a complete whitespace-only postfix is omitted");

    context.settings.max_postfix_chars = 0;
    execution = pkchat::editor::build_assist_execution(
        python_state, context, pkchat::editor::AssistCommandKind::Configured,
        *continue_index, pkchat::editor::AssistScope::Continue, "", std::nullopt);
    check(execution.messages.back().content.find("POSTFIX_BYTES") == std::string::npos,
          "zero postfix limit omits postfix data in code completion");

    context.settings.max_prefix_chars = pkchat::editor::kDefaultAiContinuePrefixMaxChars;
    context.settings.max_postfix_chars = pkchat::editor::kDefaultAiContinuePostfixMaxChars;
    context.settings.max_prose_prefix_chars =
        pkchat::editor::kDefaultAiContinueProsePrefixMaxChars;
    context.settings.max_prose_postfix_chars =
        pkchat::editor::kDefaultAiContinueProsePostfixMaxChars;

    pkchat::cli::Options configured_options;
    configured_options.editor_assist_config = pkchat::editor::default_editor_assist_config();
    pkchat::config::ParseResult assist_config = pkchat::config::parse(
        "[editor]\nassist_spell = \"Custom spell prompt\"\n", "assist.conf");
    check(assist_config.error.ok(), "editor assist prompt config parses");
    check(pkchat::config::apply_document(assist_config.document, configured_options).ok(),
          "editor assist prompt config applies");
    context.assist_config = configured_options.editor_assist_config;
    const pkchat::editor::EditorAssistCommand* configured_spell =
        pkchat::editor::find_assist_command(context.assist_config, "/spell");
    check(configured_spell != nullptr, "configured assist spell command remains available");
    execution = pkchat::editor::build_assist_execution(
        state,
        context,
        pkchat::editor::AssistCommandKind::Configured,
        *pkchat::editor::assist_command_index(context.assist_config, "/spell"),
        pkchat::editor::AssistScope::Selection,
        "",
        std::nullopt);
    check(execution.messages.front().content.find("Custom spell prompt") != std::string::npos,
          "configured assist_spell overrides the built-in spell prompt");
    check(execution.messages.back().content.find("<content>") == 0,
          "configured assist wraps editor text in content tags");

    pkchat::config::ParseResult custom_continue_config = pkchat::config::parse(
        "[editor]\nassist_continue = \"Carry the mystery forward.\"\n",
        "assist-continue.conf");
    check(custom_continue_config.error.ok() &&
              pkchat::config::apply_document(
                  custom_continue_config.document, configured_options).ok(),
          "custom /continue prompt config applies");
    context.assist_config = configured_options.editor_assist_config;
    pkchat::editor::EditorState custom_continue_state =
        pkchat::editor::EditorState::from_text("The locked door opened.");
    custom_continue_state.cursor = custom_continue_state.text.size();
    execution = pkchat::editor::build_assist_execution(
        custom_continue_state, context, pkchat::editor::AssistCommandKind::Configured,
        *pkchat::editor::assist_command_index(context.assist_config, "/continue"),
        pkchat::editor::AssistScope::Continue, "", std::nullopt);
    check(execution.messages.front().content.find("Carry the mystery forward.") !=
                  std::string::npos &&
              execution.messages.front().content.find("Mandatory continuation rules") !=
                  std::string::npos,
          "custom /continue prompt remains alongside mandatory prose insertion constraints");

    pkchat::config::ParseResult custom_command_config = pkchat::config::parse(
        "[command]\n"
        "string = /example\n"
        "modes = all, selection\n"
        "prompt = \"Output 5 examples of the user-given topic.\"\n",
        "command.conf");
    check(custom_command_config.error.ok(), "repeatable [command] config parses");
    configured_options = pkchat::cli::Options{};
    check(pkchat::config::apply_document(custom_command_config.document, configured_options).ok(),
          "repeatable [command] config applies");
    const pkchat::editor::EditorAssistCommand* example_command =
        pkchat::editor::find_assist_command(configured_options.editor_assist_config, "/example");
    check(example_command != nullptr &&
              example_command->modes.size() == 2 &&
              example_command->prompt.find("5 examples") != std::string::npos,
          "configured [command] block adds a custom editor assist command");
    parsed = pkchat::editor::parse_assist_command("/example all",
                                                  configured_options.editor_assist_config);
    check(parsed.ok && parsed.kind == pkchat::editor::AssistCommandKind::Configured &&
              parsed.scope == pkchat::editor::AssistScope::All,
          "configured custom command parses with scope");

    pkchat::config::ParseResult override_command_config = pkchat::config::parse(
        "[command]\n"
        "string = /spell\n"
        "modes = selection, all\n"
        "prompt = \"Override spell prompt\"\n",
        "override-command.conf");
    check(override_command_config.error.ok(), "configured command override parses");
    configured_options = pkchat::cli::Options{};
    check(pkchat::config::apply_document(override_command_config.document, configured_options).ok(),
          "configured command override applies");
    const pkchat::editor::EditorAssistCommand* overridden_spell =
        pkchat::editor::find_assist_command(configured_options.editor_assist_config, "/spell");
    check(overridden_spell != nullptr && overridden_spell->prompt == "Override spell prompt",
          "configured command with matching string overrides a built-in command");

    pkchat::config::ParseResult insert_modes_config = pkchat::config::parse(
        "[command]\n"
        "string = /expand\n"
        "modes = continue, insert, local_insert\n"
        "prompt = \"Expand the input.\"\n",
        "insert-modes.conf");
    check(insert_modes_config.error.ok(), "configured continue and insert modes parse");
    configured_options = pkchat::cli::Options{};
    check(pkchat::config::apply_document(insert_modes_config.document, configured_options).ok(),
          "configured continue and insert modes apply");
    const pkchat::editor::EditorAssistCommand* expand_command =
        pkchat::editor::find_assist_command(configured_options.editor_assist_config, "/expand");
    check(expand_command != nullptr && expand_command->modes.size() == 3,
          "configured command stores continue and insert modes");

    parsed = pkchat::editor::parse_assist_command("/expand continue",
                                                  configured_options.editor_assist_config);
    check(parsed.ok && parsed.scope == pkchat::editor::AssistScope::Continue,
          "/expand continue parses continue mode");
    parsed = pkchat::editor::parse_assist_command("/expand insert",
                                                  configured_options.editor_assist_config);
    check(parsed.ok && parsed.scope == pkchat::editor::AssistScope::Insert,
          "/expand insert parses insert mode");
    parsed = pkchat::editor::parse_assist_command("/expand l",
                                                  configured_options.editor_assist_config);
    check(parsed.ok && parsed.scope == pkchat::editor::AssistScope::Insert,
          "legacy local_insert scope alias l parses as insert");

    const std::vector<std::string> expand_completions =
        pkchat::editor::assist_command_completions(configured_options.editor_assist_config);
    check(std::find(expand_completions.begin(), expand_completions.end(), "/expand continue") !=
              expand_completions.end() &&
              std::find(expand_completions.begin(), expand_completions.end(), "/expand insert") !=
                  expand_completions.end(),
          "assist completions include continue and insert variants");

    check(pkchat::editor::assist_scope_prompt(*expand_command).find("continue (c)") != std::string::npos &&
              pkchat::editor::assist_scope_prompt(*expand_command).find("insert (i)") !=
                  std::string::npos,
          "assist scope prompt advertises continue and insert keys");

    pkchat::editor::EditorState insert_state =
        pkchat::editor::EditorState::from_text("Once upon a time");
    insert_state.cursor = insert_state.text.size();
    context.assist_config = configured_options.editor_assist_config;
    const std::optional<size_t> expand_index =
        pkchat::editor::assist_command_index(context.assist_config, "/expand");
    check(expand_index.has_value(), "configured /expand command is indexed");
    execution = pkchat::editor::build_assist_execution(
        insert_state,
        context,
        pkchat::editor::AssistCommandKind::Configured,
        *expand_index,
        pkchat::editor::AssistScope::Continue,
        "",
        std::nullopt);
    check(execution.ok && execution.stream &&
              execution.edit_kind == pkchat::editor::AssistEditKind::StreamInsert,
          "continue mode builds streaming execution after the cursor");
    check(execution.messages.back().content == "<content>Once upon a time</content>",
          "continue mode sends tail-before-cursor context as input");

    insert_state.selection.anchor = 5;
    insert_state.selection.active = 9;
    execution = pkchat::editor::build_assist_execution(
        insert_state,
        context,
        pkchat::editor::AssistCommandKind::Configured,
        *expand_index,
        pkchat::editor::AssistScope::Insert,
        "",
        std::nullopt);
    check(execution.ok && execution.stream &&
              execution.edit_kind == pkchat::editor::AssistEditKind::StreamInsert,
          "insert mode builds streaming execution after the cursor");
    check(execution.messages.back().content == "<content>upon</content>",
          "insert mode sends the current selection as input");

    insert_state.clear_selection();
    execution = pkchat::editor::build_assist_execution(
        insert_state,
        context,
        pkchat::editor::AssistCommandKind::Configured,
        *expand_index,
        pkchat::editor::AssistScope::Insert,
        "",
        std::nullopt);
    check(!execution.ok &&
              execution.error_message.find("insert requires an active selection") != std::string::npos,
          "insert mode rejects missing selection");

    pkchat::config::ParseResult newbuffer_modes_config = pkchat::config::parse(
        "[command]\n"
        "string = /summarize\n"
        "modes = selection, all, newbuffer\n"
        "prompt = \"Summarize the input.\"\n",
        "newbuffer-modes.conf");
    check(newbuffer_modes_config.error.ok(), "configured newbuffer mode parses");
    configured_options = pkchat::cli::Options{};
    check(pkchat::config::apply_document(newbuffer_modes_config.document, configured_options).ok(),
          "configured newbuffer mode applies");
    parsed = pkchat::editor::parse_assist_command("/summarize n",
                                                  configured_options.editor_assist_config);
    check(parsed.ok && parsed.scope == pkchat::editor::AssistScope::NewBuffer,
          "/summarize n parses as new buffer");
    insert_state.selection.anchor = 0;
    insert_state.selection.active = 4;
    context.assist_config = configured_options.editor_assist_config;
    execution = pkchat::editor::build_assist_execution(
        insert_state,
        context,
        pkchat::editor::AssistCommandKind::Configured,
        *pkchat::editor::assist_command_index(context.assist_config, "/summarize"),
        pkchat::editor::AssistScope::NewBuffer,
        "",
        std::nullopt);
    check(execution.ok && execution.stream &&
              execution.edit_kind == pkchat::editor::AssistEditKind::NewBuffer,
          "new buffer mode builds streaming execution into a new buffer");
    check(execution.messages.back().content == "<content>Once</content>",
          "new buffer mode sends the current selection as input");
    insert_state.clear_selection();
    execution = pkchat::editor::build_assist_execution(
        insert_state,
        context,
        pkchat::editor::AssistCommandKind::Configured,
        *pkchat::editor::assist_command_index(context.assist_config, "/summarize"),
        pkchat::editor::AssistScope::NewBuffer,
        "",
        std::nullopt);
    check(!execution.ok &&
              execution.error_message.find("new buffer requires an active selection") !=
                  std::string::npos,
          "new buffer mode rejects missing selection");

    check(pkchat::editor::trim_assist_inplace_response("  fixed text \n") == "fixed text",
          "in-place assist responses are trimmed");
    check(pkchat::editor::trim_assist_inplace_response(
              "<think>hidden trace</think>\n\nVisible rewrite") == "Visible rewrite",
          "in-place assist responses drop thinking traces");
    check(pkchat::editor::trim_assist_inplace_response("<content>fixed text</content>") == "fixed text",
          "in-place assist responses strip content tags");
    check(pkchat::editor::trim_assist_inplace_response("plain text without tags") == "plain text without tags",
          "in-place assist responses leave untagged output unchanged");
    check(pkchat::editor::trim_assist_inplace_response("continued text</content>") == "continued text",
          "in-place assist responses strip trailing close tag without open tag");
    check(pkchat::editor::trim_assist_inplace_response("continued text</content></tool_call>") ==
              "continued text",
          "in-place assist responses strip trailing tool-call wrapper artifacts");
    check(pkchat::editor::trim_assist_inplace_response("continued text</content></tool_call>  \n") ==
              "continued text",
          "in-place assist responses strip trailing wrapper artifacts with whitespace");

    {
        pkchat::editor::AssistStreamFilter stream_filter;
        std::string streamed;
        streamed += stream_filter.feed("<content>hello");
        streamed += stream_filter.feed("</content>");
        streamed += stream_filter.finish();
        check(streamed == "hello",
              "streamed assist output strips content wrapper tags");
    }
    {
        pkchat::editor::AssistStreamFilter stream_filter;
        std::string streamed = stream_filter.feed("continued text</content>");
        streamed += stream_filter.finish();
        check(streamed == "continued text",
              "streamed assist output strips trailing close tag without open tag");
    }
    {
        pkchat::editor::AssistStreamFilter stream_filter;
        std::string streamed = stream_filter.feed("hello</cont");
        streamed += stream_filter.feed("ent>");
        streamed += stream_filter.finish();
        check(streamed == "hello",
              "streamed assist output strips a close tag split across chunks");
    }
    {
        pkchat::editor::AssistStreamFilter stream_filter;
        std::string streamed = stream_filter.feed("plain");
        streamed += stream_filter.finish();
        check(streamed == "plain",
              "streamed assist output leaves untagged text unchanged");
    }
    {
        pkchat::editor::AssistStreamFilter stream_filter;
        std::string streamed = stream_filter.feed("continued text</content></tool_call>");
        streamed += stream_filter.finish();
        check(streamed == "continued text",
              "streamed assist output strips trailing tool-call wrapper artifacts");
    }
    {
        pkchat::editor::AssistStreamFilter stream_filter;
        std::string streamed = stream_filter.feed("continued text</content></tool_cal");
        streamed += stream_filter.feed("l>  ");
        streamed += stream_filter.finish();
        check(streamed == "continued text",
              "streamed assist output strips split tool-call wrapper artifacts");
    }

    {
        pkchat::editor::ProseAssistStreamFilter stream_filter;
        const std::string raw = "  immediate prose\n\t";
        std::string streamed = stream_filter.feed(raw.substr(0, 1));
        streamed += stream_filter.feed(raw.substr(1, 7));
        streamed += stream_filter.feed(raw.substr(8));
        streamed += stream_filter.finish();
        check(streamed == raw,
              "raw prose stream preserves leading and trailing whitespace exactly");
    }
    {
        pkchat::editor::ProseAssistStreamFilter stream_filter;
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
        pkchat::editor::ProseAssistStreamFilter stream_filter;
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
        pkchat::editor::ProseAssistStreamFilter stream_filter;
        std::string streamed = stream_filter.feed("<content>partial bridge</con");
        streamed += stream_filter.finish();
        check(streamed == "partial bridge</con",
              "finishing a partial prose stream keeps incomplete wrapper-like output bytes");
    }

    {
        pkchat::editor::CodeAssistStreamFilter stream_filter(
            pkchat::highlight::Language::Python);
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
        pkchat::editor::CodeAssistStreamFilter stream_filter(
            pkchat::highlight::Language::Python);
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
        pkchat::editor::CodeAssistStreamFilter stream_filter(
            pkchat::highlight::Language::Sql);
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
        pkchat::editor::CodeAssistStreamFilter stream_filter(
            pkchat::highlight::Language::Python);
        std::string output;
        const pkchat::Error error = stream_filter.feed("```javascript\nalert(1);\n```", output);
        check(!error.ok() && error.code == pkchat::ErrorCode::ProviderSchema && output.empty(),
              "explicitly mismatched leading code fence is rejected");
    }
    {
        pkchat::editor::CodeAssistStreamFilter stream_filter(
            pkchat::highlight::Language::Cpp);
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
        pkchat::editor::EditorState state = pkchat::editor::EditorState::from_text("hello</content>");
        state.cursor = state.text.size();
        pkchat::editor::strip_trailing_assist_close_tag_without_undo(state);
        check(state.text.str() == "hello",
              "streamed assist post-clear strips trailing close tag from buffer");
        check(!state.can_undo(), "streamed assist post-clear does not create undo history");
    }
    {
        pkchat::editor::EditorState state = pkchat::editor::EditorState::from_text("keep</content>tail");
        state.cursor = state.text.size();
        pkchat::editor::strip_trailing_assist_close_tag_without_undo(state);
        check(state.text.str() == "keep</content>tail",
              "streamed assist post-clear ignores close tag not at insertion tail");
    }
    {
        pkchat::editor::EditorState state =
            pkchat::editor::EditorState::from_text("hello</content></tool_call>  ");
        state.cursor = state.text.size();
        pkchat::editor::strip_trailing_assist_close_tag_without_undo(state);
        check(state.text.str() == "hello",
              "streamed assist post-clear strips trailing tool-call wrapper artifacts");
    }

    const pkchat::provider::RequestContext assist_context =
        pkchat::editor::assist_request_context(context, true);
    check(assist_context.suppress_streaming_reasoning,
          "editor assist suppresses streamed reasoning deltas");
}

void test_editor_contextual_completion_modes() {
    const std::string directory = "build/pkchat-context-completion";
    std::error_code filesystem_error;
    std::filesystem::create_directories(directory, filesystem_error);
    check(!filesystem_error, "contextual completion fixture directory is created");

    {
        std::ofstream fixture(directory + "/pkchat-context-file.txt",
                              std::ios::binary | std::ios::trunc);
        fixture << "context";
        check(static_cast<bool>(fixture), "contextual completion fixture file is written");
    }

    pkchat::editor::ContextualCompleter completer;

    pkchat::editor::EditorState empty = pkchat::editor::EditorState::from_text("");
    empty.mode = pkchat::editor::EditorMode::Chat;
    pkchat::editor::PathCompletionResult result = completer.complete(empty);
    check(!result.handled && empty.text.str().empty(),
          "chat Tab on empty input is ignored");

    pkchat::editor::EditorState editor_path =
        pkchat::editor::EditorState::from_text(directory + "/pkchat-context-fi");
    editor_path.cursor = editor_path.text.size();
    result = completer.complete(editor_path);
    check(!result.handled &&
              editor_path.text.str() == directory + "/pkchat-context-fi",
          "editor-mode Tab does not run generic path completion");

    pkchat::editor::EditorState command = pkchat::editor::EditorState::from_text("/he");
    command.mode = pkchat::editor::EditorMode::Chat;
    command.cursor = command.text.size();
    result = completer.complete(command);
    check(result.handled && result.kind == pkchat::editor::CompletionKind::Command &&
              result.match_count == 1 && command.text.str() == "/help",
          "chat command completion works at the start of the first line");

    completer.reset();
    pkchat::editor::EditorState path_command = pkchat::editor::EditorState::from_text("/in");
    path_command.mode = pkchat::editor::EditorMode::Chat;
    path_command.cursor = path_command.text.size();
    result = completer.complete(path_command);
    check(result.handled && result.kind == pkchat::editor::CompletionKind::Command &&
              path_command.text.str() == "/insert ",
          "chat command completion adds the path-command separator");

    completer.reset();
    pkchat::editor::EditorState provider_command =
        pkchat::editor::EditorState::from_text("/prov");
    provider_command.mode = pkchat::editor::EditorMode::Chat;
    provider_command.cursor = provider_command.text.size();
    result = completer.complete(provider_command);
    check(result.handled && result.kind == pkchat::editor::CompletionKind::Command &&
              provider_command.text.str() == "/provider ",
          "chat command completion includes /provider");

    completer.reset();
    pkchat::editor::EditorState list_command =
        pkchat::editor::EditorState::from_text("/li");
    list_command.mode = pkchat::editor::EditorMode::Chat;
    list_command.cursor = list_command.text.size();
    result = completer.complete(list_command);
    check(result.handled && result.kind == pkchat::editor::CompletionKind::Command &&
              list_command.text.str() == "/list",
          "chat command completion includes /list");

    completer.reset();
    pkchat::editor::EditorState path =
        pkchat::editor::EditorState::from_text("/insert " + directory + "/pkchat-context-fi");
    path.mode = pkchat::editor::EditorMode::Chat;
    path.cursor = path.text.size();
    result = completer.complete(path);
    check(result.handled && result.kind == pkchat::editor::CompletionKind::Path &&
              result.match_count == 1 &&
              path.text.str() == "/insert " + directory + "/pkchat-context-file.txt",
          "chat path completion runs after /insert");

    completer.reset();
    pkchat::editor::EditorState fetch =
        pkchat::editor::EditorState::from_text("/fetch " + directory + "/pkchat-context-fi");
    fetch.mode = pkchat::editor::EditorMode::Chat;
    fetch.cursor = fetch.text.size();
    result = completer.complete(fetch);
    check(!result.handled &&
              fetch.text.str() == "/fetch " + directory + "/pkchat-context-fi",
          "chat path completion ignores non-file commands");

    pkchat::editor::EditorState second_line = pkchat::editor::EditorState::from_text("hello\n/he");
    second_line.mode = pkchat::editor::EditorMode::Chat;
    second_line.cursor = second_line.text.size();
    result = completer.complete(second_line);
    check(!result.handled && second_line.text.str() == "hello\n/he",
          "chat command completion is limited to the first line start");
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

    pkchat::editor::EditorSettings settings;
    settings.huge_file_size_warning = 5;
    settings.file_size_limit = -1;
    pkchat::editor::FileLoadCheck load_check;
    err = pkchat::editor::check_load_file_size(path, settings, load_check);
    check(err.ok() && load_check.size == 12 && load_check.should_warn,
          "editor file size check reports configured huge-file warning");

    settings.file_size_limit = 4;
    err = pkchat::editor::load_file(path, settings, loaded);
    check(!err.ok() && err.message.find("FILE_SIZE_LIMIT") != std::string::npos,
          "editor file load rejects files above the configured size limit");

    settings.file_size_limit = -1;
    err = pkchat::editor::load_file(path, settings, loaded);
    check(err.ok() && loaded.str() == "first\nsecond",
          "editor file load has no configured upper limit when file_size_limit is -1");
}

void test_editor_linebreak_modes() {
    struct Case {
        const char* name;
        const char* bytes;
        pkchat::editor::LineBreak linebreak;
    };
    const Case cases[] = {
        {"lf", "first\nsecond\n", pkchat::editor::LineBreak::Lf},
        {"cr", "first\rsecond\r", pkchat::editor::LineBreak::Cr},
        {"crlf", "first\r\nsecond\r\n", pkchat::editor::LineBreak::Crlf},
    };
    for (const Case& item : cases) {
        const std::string path = std::string("build/unit-editor-linebreak-") + item.name + ".txt";
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(item.bytes, static_cast<std::streamsize>(std::char_traits<char>::length(item.bytes)));
        }
        pkchat::editor::LoadedFile loaded;
        pkchat::editor::EditorSettings settings;
        check(pkchat::editor::load_file(path, settings, loaded).ok(),
              std::string("editor loads ") + item.name + " line endings");
        check(loaded.text.str() == "first\nsecond\n" && loaded.linebreak == item.linebreak &&
                  !loaded.mixed_linebreaks,
              std::string("editor detects and normalizes ") + item.name + " line endings");
        const std::string saved = path + ".saved";
        check(pkchat::editor::save_file(saved, loaded.text, loaded.linebreak).ok(),
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
    pkchat::editor::EditorSettings settings;
    settings.linebreak = pkchat::editor::LineBreak::Cr;
    pkchat::editor::LoadedFile mixed;
    check(pkchat::editor::load_file(mixed_path, settings, mixed).ok(),
          "editor loads mixed line endings");
    check(mixed.mixed_linebreaks && mixed.linebreak == pkchat::editor::LineBreak::Cr &&
              mixed.text.str() == "one\ntwo\nthree\n",
          "mixed line endings normalize and use the configured default");

    const std::string no_ending_path = "build/unit-editor-no-linebreak.txt";
    {
        std::ofstream out(no_ending_path, std::ios::binary | std::ios::trunc);
        out << "no final ending";
    }
    settings.linebreak = pkchat::editor::LineBreak::Crlf;
    pkchat::editor::LoadedFile no_ending;
    check(pkchat::editor::load_file(no_ending_path, settings, no_ending).ok() &&
              no_ending.linebreak == pkchat::editor::LineBreak::Crlf &&
              no_ending.text.str() == "no final ending",
          "file without a line ending inherits the configured default without adding one");
}

void test_editor_indentation_detection() {
    using pkchat::editor::IndentationDetection;
    using pkchat::editor::TabStyle;

    IndentationDetection detected = pkchat::editor::detect_indentation(
        "function run() {\n  if (ready) {\n    call();\n  }\n}", 4, TabStyle::Spaces);
    check(detected.tab_width_detected && detected.tab_width == 2 &&
              detected.tab_style_detected && detected.tab_style == TabStyle::Spaces,
          "editor detects a consistent two-space indentation step");

    detected = pkchat::editor::detect_indentation(
        "if (ready) {\n    while (open) {\n        call();\n    }\n}", 2, TabStyle::Tab);
    check(detected.tab_width_detected && detected.tab_width == 4 &&
              detected.tab_style_detected && detected.tab_style == TabStyle::Spaces,
          "editor detects a consistent four-space indentation step");

    detected = pkchat::editor::detect_indentation(
        "if ready\n\tcall\nend", 8, TabStyle::Spaces);
    check(!detected.tab_width_detected && detected.tab_width == 8 &&
              detected.tab_style_detected && detected.tab_style == TabStyle::Tab,
          "editor detects tab indentation while retaining the fallback display width");

    detected = pkchat::editor::detect_indentation(
        "top\n  child\n     inconsistent", 6, TabStyle::Tab);
    check(!detected.tab_width_detected && detected.tab_width == 6 &&
              detected.tab_style == TabStyle::Spaces,
          "ambiguous indentation steps retain the configured width");

    detected = pkchat::editor::detect_indentation("const value = 1;", 7, TabStyle::Tab);
    check(!detected.tab_width_detected && !detected.tab_style_detected &&
              detected.tab_width == 7 && detected.tab_style == TabStyle::Tab,
          "one-line files retain configured indentation defaults");

    std::string after_limit;
    for (size_t line = 0; line < 20; ++line) {
        after_limit += "top_level();\n";
    }
    after_limit += "  ignored_after_limit();\n";
    detected = pkchat::editor::detect_indentation(after_limit, 5, TabStyle::Spaces);
    check(!detected.tab_width_detected && detected.tab_width == 5,
          "indentation detection inspects only the first twenty physical lines");

    pkchat::editor::EditorSettings settings;
    settings.tab_width = 4;
    settings.tab_style = TabStyle::Spaces;
    pkchat::editor::LoadedFile javascript;
    check(pkchat::editor::load_file("tests/highlight/javascript_file.js", settings, javascript).ok() &&
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
    pkchat::editor::LoadedFile fallback;
    check(pkchat::editor::load_file(one_line_path, settings, fallback).ok() &&
              !fallback.tab_width_detected && !fallback.tab_style_detected &&
              fallback.tab_width == 6 && fallback.tab_style == TabStyle::Tab,
          "file loading retains configured indentation when detection is inconclusive");
}

void test_editor_tab_indentation() {
    pkchat::editor::EditorState state = pkchat::editor::EditorState::from_text("ab");
    state.tab_width = 4;
    state.cursor = 2;
    check(state.indent().ok() && state.text.str() == "ab  " && state.cursor == 4,
          "space Tab advances to the next configured tab stop");
    check(state.undo() && state.text.str() == "ab", "single-position Tab is one undo step");

    state = pkchat::editor::EditorState::from_text("ab");
    state.tab_style = pkchat::editor::TabStyle::Tab;
    state.cursor = 1;
    check(state.indent().ok() && state.text.str() == "a\tb",
          "tab style inserts one literal tab at the cursor");

    state = pkchat::editor::EditorState::from_text("a\nb\nc");
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

    state = pkchat::editor::EditorState::from_text("a\nb\nc");
    state.tab_width = 2;
    state.selection.anchor = state.text.line_start(2);
    state.selection.active = 0;
    state.cursor = 0;
    check(state.indent().ok() && state.text.str() == "  a\n  b\nc" &&
              state.selection.anchor == state.text.line_start(2) &&
              state.selection.active == 0,
          "block Tab preserves reverse selection direction");

    state = pkchat::editor::EditorState::from_text("\t  alpha\n  \tbeta");
    state.tab_width = 4;
    state.selection.anchor = 0;
    state.selection.active = state.text.size();
    state.cursor = state.text.size();
    check(state.outdent().ok() && state.text.str() == "\talpha\nbeta",
          "block Shift+Tab removes one display indentation level from mixed whitespace");
    check(state.undo() && state.text.str() == "\t  alpha\n  \tbeta",
          "block outdent is one undo step");

    state = pkchat::editor::EditorState::from_text("    alpha");
    state.tab_width = 4;
    state.cursor = state.text.size();
    check(state.outdent().ok() && state.text.str() == "alpha" && state.cursor == 5,
          "Shift+Tab without a selection outdents the current line and preserves cursor content position");

    const pkchat::editor::PieceTable tabs =
        pkchat::editor::PieceTable::from_string("\tx");
    check(tabs.display_column_for_offset(1, 8) == 8 &&
              tabs.offset_for_line_column(0, 8, 8) == 1,
          "editor display columns honor the active tab width");

    std::string large_text;
    for (size_t i = 0; i < 5000; ++i) {
        large_text += "line\n";
    }
    state = pkchat::editor::EditorState::from_text(large_text);
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
    std::vector<pkchat::editor::EditorState> buffers;
    buffers.push_back(pkchat::editor::EditorState::from_text("win"));
    buffers.push_back(
        pkchat::editor::EditorState::from_text("windowHeight windowWidth"));
    buffers[0].cursor = buffers[0].text.size();

    pkchat::editor::WordCompleter completer;
    pkchat::editor::WordCompletionResult result = completer.complete(buffers[0], buffers, 0);
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

    buffers[0] = pkchat::editor::EditorState::from_text("Win");
    buffers[0].cursor = buffers[0].text.size();
    buffers[1] =
        pkchat::editor::EditorState::from_text("WindowHeight windowWidth WindowSize");
    completer.reset();
    result = completer.complete(buffers[0], buffers, 0);
    check(result.completed && result.match_count == 2 && buffers[0].text.str() == "Window",
          "an uppercase prefix enables case-sensitive smart-case matching");

    buffers[0] = pkchat::editor::EditorState::from_text("ä");
    buffers[0].cursor = buffers[0].text.size();
    buffers[1] = pkchat::editor::EditorState::from_text("Äiti");
    completer.reset();
    result = completer.complete(buffers[0], buffers, 0);
    check(result.completed && buffers[0].text.str() == "Äiti",
          "lowercase Unicode prefixes use full case-folded matching");

    buffers[0] = pkchat::editor::EditorState::from_text("stras");
    buffers[0].cursor = buffers[0].text.size();
    buffers[1] = pkchat::editor::EditorState::from_text("Straße");
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
        buffers[0] = pkchat::editor::EditorState::from_text(test.prefix);
        buffers[0].cursor = buffers[0].text.size();
        buffers[1] = pkchat::editor::EditorState::from_text(test.candidate);
        completer.reset();
        result = completer.complete(buffers[0], buffers, 0);
        check(result.completed && buffers[0].text.str() == test.candidate,
              "document completion supports Unicode letters, marks, and underscore: " +
                  test.prefix);
    }

    buffers[0] = pkchat::editor::EditorState::from_text("wo");
    buffers[0].cursor = buffers[0].text.size();
    buffers[1] = pkchat::editor::EditorState::from_text(
        std::string("bad") + static_cast<char>(0xFF) + "word");
    completer.reset();
    result = completer.complete(buffers[0], buffers, 0);
    check(result.completed && buffers[0].text.str() == "word",
          "invalid UTF-8 bytes are preserved and treated as word boundaries");

    buffers[0] = pkchat::editor::EditorState::from_text("win");
    buffers[0].cursor = buffers[0].text.size();
    buffers[1] = pkchat::editor::EditorState::from_text("windowWidth");
    check(buffers[1].completion_word_index().occurrence_count("windowWidth") == 1,
          "per-buffer word index records occurrence counts");
    pkchat::editor::EditorState edited_buffer = buffers[1];
    check(edited_buffer.replace(0, std::string("windowWidth").size(), "paneWidth").ok(),
          "indexed buffer edit succeeds");
    const pkchat::editor::WordIndex& edited_index =
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

    buffers[0] = pkchat::editor::EditorState::from_text("windowHeight");
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
    pkchat::editor::EditorState large_buffer =
        pkchat::editor::EditorState::from_text(std::move(large));
    const pkchat::editor::WordIndex& large_index =
        large_buffer.completion_word_index();
    check(large_index.unique_word_count() == 50000,
          "large buffers build a deduplicated ordered word index");
    std::map<std::string, size_t> large_matches;
    large_index.append_matches("symbol_4999", true, large_matches);
    check(large_matches.size() == 10,
          "large-buffer prefix lookup uses the ordered index and returns the bounded range");
}

void test_editor_language_reformatting() {
    using pkchat::editor::EditorState;
    using pkchat::editor::ReformatRequest;
    using pkchat::editor::ReformatResult;
    using pkchat::highlight::Language;

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
        const ReformatResult result = pkchat::editor::reformat_indentation(request);
        check(result.error.ok() && result.replacement ==
                  "if (ready) {\n    value();\n}\n",
              std::string("brace reformat profile works for ") +
                  pkchat::highlight::language_name(language));
    }
    ReformatRequest tab_request;
    tab_request.content = "if (ready) {\nvalue();\n}";
    tab_request.language = Language::Cpp;
    tab_request.tab_style = pkchat::editor::TabStyle::Tab;
    tab_request.first_line = 0;
    tab_request.last_line = 2;
    check(pkchat::editor::reformat_indentation(tab_request).replacement ==
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
        request.last_line = pkchat::highlight::split_lines(test.input).size() - 1;
        const ReformatResult result = pkchat::editor::reformat_indentation(request);
        check(result.error.ok() && result.replacement == test.expected,
              std::string("language reformat profile works for ") +
                  pkchat::highlight::language_name(test.language));
    }

    ReformatRequest protected_request;
    protected_request.content =
        "if (ready) {\nconst char *text = \"}\"; // {\ncall();\n}";
    protected_request.language = Language::Cpp;
    protected_request.first_line = 0;
    protected_request.last_line = 3;
    ReformatResult protected_result =
        pkchat::editor::reformat_indentation(protected_request);
    check(protected_result.error.ok() && protected_result.replacement ==
              "if (ready) {\n    const char *text = \"}\"; // {\n    call();\n}",
          "reformat ignores braces inside strings and comments");

    ReformatRequest markdown_request;
    markdown_request.content = "```cpp\n   if (x) {\n bad();\n   }\n```";
    markdown_request.language = Language::Markdown;
    markdown_request.first_line = 0;
    markdown_request.last_line = 4;
    const ReformatResult markdown_result =
        pkchat::editor::reformat_indentation(markdown_request);
    check(markdown_result.error.ok() && markdown_result.replacement == markdown_request.content,
          "reformat preserves Markdown fenced-code contents exactly");

    ReformatRequest pathological_request;
    pathological_request.content =
        std::string(pkchat::highlight::kMaximumHighlightedLineBytes + 1, 'x') +
        "\n  preserve_after_unsafe_line();";
    pathological_request.language = Language::Cpp;
    pathological_request.first_line = 0;
    pathological_request.last_line = 1;
    const ReformatResult pathological_result =
        pkchat::editor::reformat_indentation(pathological_request);
    check(pathological_result.error.ok() && !pathological_result.warning.empty() &&
              pathological_result.replacement == pathological_request.content,
          "reformat safely preserves a region after an unclassifiable pathological line");

    EditorState state = EditorState::from_text("if (ready) {\ncall();\n}\nafter();");
    state.set_language(Language::Cpp, false);
    state.selection.anchor = 0;
    state.selection.active = state.text.line_start(3);
    state.cursor = state.selection.active;
    ReformatRequest selected_request;
    check(pkchat::editor::build_reformat_request(state, false, selected_request).ok() &&
              selected_request.first_line == 0 && selected_request.last_line == 2,
          "selected reformat expands touched lines and excludes a following column-zero line");
    ReformatResult selected_result =
        pkchat::editor::reformat_indentation(selected_request);
    check(pkchat::editor::apply_reformat_result(state, selected_result, false).ok() &&
              state.text.str() == "if (ready) {\n    call();\n}\nafter();" &&
              state.selection.has_range(),
          "selected reformat applies one leading-whitespace replacement and keeps the block selected");
    check(state.undo() && state.text.str() == "if (ready) {\ncall();\n}\nafter();",
          "selected language reformat is one undo operation");

    state = EditorState::from_text("if (ready) {\ncall();\n}");
    state.set_language(Language::Cpp, false);
    state.cursor = state.text.line_start(1) + 2;
    ReformatRequest all_request;
    check(pkchat::editor::build_reformat_request(state, true, all_request).ok(),
          "reformat-all request accepts an unselected buffer");
    const ReformatResult all_result = pkchat::editor::reformat_indentation(all_request);
    check(pkchat::editor::apply_reformat_result(state, all_result, true).ok() &&
              !state.selection.has_range() && state.text.line_for_offset(state.cursor) == 1,
          "reformat-all preserves the logical cursor line and clears selection");

    state = EditorState::from_text("plain text");
    ReformatRequest invalid_request;
    check(!pkchat::editor::build_reformat_request(state, false, invalid_request).ok(),
          "/reformat without a selection reports an actionable error");
    check(pkchat::editor::build_reformat_request(state, true, invalid_request).ok() &&
              !pkchat::editor::reformat_indentation(invalid_request).error.ok(),
          "text mode reformat reports unsupported mode instead of guessing");

    pkchat::runtime::CancellationSource cancellation;
    cancellation.cancel();
    invalid_request.language = Language::Cpp;
    check(pkchat::editor::reformat_indentation(invalid_request, cancellation.token()).error.code ==
              pkchat::ErrorCode::Cancelled,
          "language reformat observes cancellation before processing the buffer");

    pkchat::runtime::EventQueue<pkchat::editor::ReformatEvent> events;
    pkchat::runtime::JobHandle job;
    ReformatRequest async_request;
    async_request.content = "if (ready) {\ncall();\n}";
    async_request.language = Language::Cpp;
    async_request.first_line = 0;
    async_request.last_line = 2;
    pkchat::editor::start_reformat_job(async_request, events, job);
    pkchat::editor::ReformatEvent event;
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
        pkchat::highlight::split_lines(large_request.content).size() - 1;
    const ReformatResult large_result = pkchat::editor::reformat_indentation(large_request);
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
    pkchat::editor::EditorState state =
        pkchat::editor::EditorState::from_text("alpha\nbeta\ngamma");
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

    pkchat::editor::EditorState state =
        pkchat::editor::EditorState::from_text(invalid);
    pkchat::editor::RenderedPanel rendered = state.render({1, 1, 1, 8});
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
    pkchat::editor::Clipboard clipboard;
    pkchat::editor::EditorState state = pkchat::editor::EditorState::from_text("alpha beta\ngamma");
    state.cursor = state.text.offset_for_line_column(0, 6);
    pkchat::Error err = state.kill_to_line_end(clipboard);
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

    pkchat::editor::EditorState middle = pkchat::editor::EditorState::from_text("alpha\n\ngamma");
    middle.cursor = middle.text.line_start(1);
    err = middle.kill_to_line_end(clipboard);
    check(err.ok(), "editor kill empty middle line succeeds");
    check(middle.text.str() == "alpha\ngamma", "editor kill empty middle line removes that line");
    check(middle.cursor == middle.text.line_start(1), "editor kill empty middle line keeps cursor at next line start");
    check(clipboard.text() == "\n", "editor kill empty middle line copies newline to clipboard");

    pkchat::editor::EditorState last = pkchat::editor::EditorState::from_text("alpha\n");
    last.cursor = last.text.line_start(1);
    err = last.kill_to_line_end(clipboard);
    check(err.ok(), "editor kill empty final line succeeds");
    check(last.text.str() == "alpha", "editor kill empty final line removes preceding newline");
    check(last.cursor == last.text.size(), "editor kill empty final line moves cursor to new end");
    check(clipboard.text() == "\n", "editor kill empty final line copies newline to clipboard");

    pkchat::editor::EditorState only = pkchat::editor::EditorState::from_text("");
    err = only.kill_to_line_end(clipboard);
    check(err.ok(), "editor kill single empty buffer succeeds");
    check(only.text.str().empty(), "editor kill single empty buffer is a no-op");
}

void test_editor_movement_sequence_parse() {
    pkchat::editor::MovementKeyEvent event;
    check(pkchat::editor::parse_movement_sequence("[D", event) && !event.shift && !event.alt &&
              event.key == pkchat::editor::MovementKey::Left,
          "left arrow sequence parses");
    check(pkchat::editor::parse_movement_sequence("[1;2C", event) && event.shift && !event.alt &&
              event.key == pkchat::editor::MovementKey::Right,
          "shift right arrow sequence parses");
    check(pkchat::editor::parse_movement_sequence("[5;2~", event) && event.shift && !event.alt &&
              event.key == pkchat::editor::MovementKey::PageUp,
          "shift page up sequence parses");
    check(pkchat::editor::parse_movement_sequence("[1;3H", event) && !event.shift && event.alt &&
              !event.ctrl && event.key == pkchat::editor::MovementKey::Home,
          "alt home sequence parses");
    check(pkchat::editor::parse_movement_sequence("[1;5H", event) && !event.shift && !event.alt &&
              event.ctrl && event.key == pkchat::editor::MovementKey::Home,
          "ctrl home sequence parses");
    check(pkchat::editor::parse_movement_sequence("[1;5F", event) && !event.shift && !event.alt &&
              event.ctrl && event.key == pkchat::editor::MovementKey::End,
          "ctrl end sequence parses");
    check(pkchat::editor::parse_movement_sequence("[1;3~", event) && !event.shift && event.alt &&
              !event.ctrl && event.key == pkchat::editor::MovementKey::Home,
          "alt home tilde sequence parses");
    check(pkchat::editor::parse_movement_sequence("[1;5~", event) && !event.shift && !event.alt &&
              event.ctrl && event.key == pkchat::editor::MovementKey::Home,
          "ctrl home tilde sequence parses");
    check(pkchat::editor::parse_movement_sequence("[4;3~", event) && !event.shift && event.alt &&
              !event.ctrl && event.key == pkchat::editor::MovementKey::End,
          "alt end tilde sequence parses");
    check(pkchat::editor::parse_movement_sequence("[5;3~", event) && !event.shift && event.alt &&
              !event.ctrl && event.key == pkchat::editor::MovementKey::PageUp,
          "alt page up sequence parses");
    check(pkchat::editor::parse_movement_sequence("[6;3~", event) && !event.shift && event.alt &&
              !event.ctrl && event.key == pkchat::editor::MovementKey::PageDown,
          "alt page down sequence parses");
    check(pkchat::editor::parse_movement_sequence("[57362;3u", event) && !event.shift && event.alt &&
              !event.ctrl && event.key == pkchat::editor::MovementKey::PageUp,
          "kitty Alt+PageUp sequence parses");
    check(pkchat::editor::parse_movement_sequence("[57360;3u", event) && !event.shift && event.alt &&
              event.key == pkchat::editor::MovementKey::Home,
          "kitty Alt+Home sequence parses");
}

void test_editor_select_all() {
    pkchat::editor::EditorState state =
        pkchat::editor::EditorState::from_text("alpha\nbeta\ngamma");
    state.cursor = state.text.offset_for_line_column(1, 2);

    state.select_all();
    check(state.selection.has_range(), "select all creates a range selection");
    check(state.selection.start() == 0, "select all starts at the beginning of the buffer");
    check(state.selection.end() == state.text.size(), "select all ends at the end of the buffer");
    check(state.cursor == state.text.size(), "select all moves the cursor to the end of the buffer");
    check(state.selected_text() == "alpha\nbeta\ngamma", "select all selects the full buffer");
}

void test_editor_line_home_end_navigation() {
    pkchat::editor::EditorState state =
        pkchat::editor::EditorState::from_text("alpha\nbeta\ngamma");
    state.cursor = state.text.offset_for_line_column(1, 2);
    const pkchat::editor::Rect rect{1, 1, 10, 40};

    state.apply_movement(pkchat::editor::MovementKey::Home, rect, false, false);
    check(state.cursor == state.text.line_start(1), "editor Home moves to the beginning of the current line");
    check(state.preferred_column == 0, "editor Home resets the preferred column to the line start");

    state.apply_movement(pkchat::editor::MovementKey::End, rect, false, false);
    check(state.cursor == state.text.line_start(1) + state.text.line_length(1),
          "editor End moves to the end of the current line");

    state.apply_movement(pkchat::editor::MovementKey::Home, rect, false, false, true);
    check(state.cursor == 0, "editor Ctrl+Home moves to the beginning of the buffer");
    check(state.scroll_line == 0, "editor Ctrl+Home scrolls to the top of the buffer");

    state.apply_movement(pkchat::editor::MovementKey::End, rect, false, false, true);
    check(state.cursor == state.text.size(), "editor Ctrl+End moves to the end of the buffer");
}

void test_editor_wrapped_line_home_end_navigation() {
    pkchat::editor::EditorState state =
        pkchat::editor::EditorState::from_text("abcdefghij\nXYZ");
    const pkchat::editor::Rect rect{1, 1, 10, 4};
    state.cursor = state.text.line_start(0) + 6;

    state.apply_movement(pkchat::editor::MovementKey::Home, rect, false, false);
    check(state.cursor == state.text.line_start(0) + 4,
          "editor Home moves to the beginning of the current wrapped row");

    state.apply_movement(pkchat::editor::MovementKey::End, rect, false, false);
    check(state.cursor == state.text.line_start(0) + 8,
          "editor End moves to the end of the current wrapped row");

    state.cursor = state.text.line_start(0) + 2;
    state.apply_movement(pkchat::editor::MovementKey::End, rect, false, false);
    check(state.cursor == state.text.line_start(0) + 4,
          "editor End on the first wrapped row stops at that row boundary");
}

void test_editor_page_navigation() {
    pkchat::editor::EditorState state =
        pkchat::editor::EditorState::from_text("zero\none\ntwo\nthree\nfour\nfive");
    pkchat::editor::Rect rect{1, 1, 2, 20};
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
    pkchat::editor::EditorState state = pkchat::editor::EditorState::from_text("hello");
    pkchat::editor::Clipboard clipboard;
    clipboard.set("local");
    state.cursor = state.text.size();
    pkchat::Error err = pkchat::editor::paste_with_clipboard_preference(state, clipboard, "external");
    check(err.ok(), "paste prefers local clipboard");
    check(state.text.str() == "hellolocal", "local clipboard overrides terminal paste payload");

    clipboard.clear();
    err = pkchat::editor::paste_with_clipboard_preference(state, clipboard, "external");
    check(err.ok(), "paste falls back to terminal payload when local clipboard is empty");
    check(state.text.str() == "hellolocalexternal", "terminal paste payload is inserted");
}

void test_editor_path_completion() {
    const std::string directory = "build/pkchat-tab-completion";
    std::error_code filesystem_error;
    std::filesystem::create_directories(directory + "/pkchat-folder", filesystem_error);
    check(!filesystem_error, "path completion fixture directory is created");

    const std::vector<std::string> files = {
        "pkchat-single-result.txt",
        "pkchat-cycle-alpha.txt",
        "pkchat-cycle-alpine.txt",
    };
    for (const std::string& name : files) {
        std::ofstream fixture(directory + "/" + name, std::ios::binary | std::ios::trunc);
        fixture << name;
        check(static_cast<bool>(fixture), "path completion fixture file is written: " + name);
    }

    pkchat::editor::PathCompleter completer;
    const std::string unique_prefix = "/insert " + directory + "/pkchat-single-r";
    pkchat::editor::EditorState unique = pkchat::editor::EditorState::from_text(unique_prefix);
    unique.cursor = unique.text.size();
    pkchat::editor::PathCompletionResult result = completer.complete(unique);
    check(result.error.ok() && result.match_count == 1, "path completion finds a unique file");
    check(unique.text.str() == "/insert " + directory + "/pkchat-single-result.txt",
          "one Tab fully completes a unique path");

    completer.reset();
    const std::string cycle_prefix = "/attach " + directory + "/pkchat-cy";
    pkchat::editor::EditorState cycling = pkchat::editor::EditorState::from_text(cycle_prefix);
    cycling.cursor = cycling.text.size();
    result = completer.complete(cycling);
    const std::string common = "/attach " + directory + "/pkchat-cycle-alp";
    check(result.error.ok() && result.match_count == 2 && !result.cycling,
          "first Tab reports multiple path matches");
    check(cycling.text.str() == common, "first Tab completes the unambiguous common path prefix");

    result = completer.complete(cycling);
    check(result.cycling && result.choice_index == 0,
          "second Tab selects the first sorted path choice");
    check(cycling.text.str() == "/attach " + directory + "/pkchat-cycle-alpha.txt",
          "second Tab inserts the first path choice");

    result = completer.complete(cycling);
    check(result.cycling && result.choice_index == 1,
          "third Tab selects the next path choice");
    check(cycling.text.str() == "/attach " + directory + "/pkchat-cycle-alpine.txt",
          "third Tab inserts the next path choice");

    result = completer.complete(cycling);
    check(result.cycling && result.choice_index == 0,
          "repeated Tab wraps path choices in sorted order");

    completer.reset();
    pkchat::editor::EditorState directory_state =
        pkchat::editor::EditorState::from_text(directory + "/pkchat-fol");
    directory_state.cursor = directory_state.text.size();
    result = completer.complete(directory_state);
    check(result.match_count == 1 && directory_state.text.str() == directory + "/pkchat-folder/",
          "directory completion appends a slash");

    completer.reset();
    pkchat::editor::EditorState missing =
        pkchat::editor::EditorState::from_text(directory + "/pkchat-does-not-exist");
    missing.cursor = missing.text.size();
    result = completer.complete(missing);
    check(result.error.ok() && result.match_count == 0 &&
              missing.text.str() == directory + "/pkchat-does-not-exist",
          "path completion leaves an unmatched path unchanged");

    pkchat::editor::EditorState cancelled =
        pkchat::editor::EditorState::from_text(directory + "/pkchat-single-r");
    cancelled.cursor = cancelled.text.size();
    result = completer.complete(cancelled, []() { return true; });
    check(result.error.code == pkchat::ErrorCode::Cancelled &&
              cancelled.text.str() == directory + "/pkchat-single-r",
          "a cancelled path scan leaves editor input unchanged");

    completer.reset();
    pkchat::editor::EditorState reset_cycle = pkchat::editor::EditorState::from_text(cycle_prefix);
    reset_cycle.cursor = reset_cycle.text.size();
    completer.complete(reset_cycle);
    completer.reset();
    result = completer.complete(reset_cycle);
    check(!result.cycling && reset_cycle.text.str() == common,
          "resetting completion prevents a later Tab from cycling stale choices");

    std::string minibuffer_input = directory + "/pkchat-sing";
    pkchat::editor::PathCompleter minibuffer_completer;
    result = pkchat::editor::complete_path_input(minibuffer_input, minibuffer_completer);
    check(result.error.ok() && result.match_count == 1,
          "minibuffer path completion finds a unique file");
    check(minibuffer_input == directory + "/pkchat-single-result.txt",
          "minibuffer path completion completes editor save/open paths");
}

void test_expand_user_path() {
    if (const char* home = std::getenv("HOME")) {
        check(pkchat::expand_user_path("~") == home, "expand_user_path expands bare tilde");
        check(pkchat::expand_user_path("~/notes.txt") == std::string(home) + "/notes.txt",
              "expand_user_path expands tilde-prefixed paths");
    }
    check(pkchat::expand_user_path("-") == "-", "expand_user_path preserves stdin dash");
    check(pkchat::expand_user_path("stdin") == "stdin", "expand_user_path preserves stdin literal");
    check(pkchat::expand_user_path("/tmp/file") == "/tmp/file",
          "expand_user_path leaves absolute paths unchanged");
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

void test_editor_search_navigation() {
    pkchat::editor::EditorState state =
        pkchat::editor::EditorState::from_text("alpha beta\nbeta gamma\nalpha");

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
    pkchat::editor::EditorState state =
        pkchat::editor::EditorState::from_text("one two one two one");

    size_t replacements = 0;
    pkchat::Error err = state.replace_all_from(4, "one", "ONE", replacements);
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
    pkchat::editor::EditorState state = pkchat::editor::EditorState::from_text("alpha beta gamma");
    pkchat::editor::Rect rect{1, 1, 1, 20};
    pkchat::editor::Clipboard clipboard;

    state.cursor = 5;
    state.apply_movement(pkchat::editor::MovementKey::Left, rect, true);
    state.apply_movement(pkchat::editor::MovementKey::Left, rect, true);
    state.apply_movement(pkchat::editor::MovementKey::Left, rect, true);
    state.apply_movement(pkchat::editor::MovementKey::Left, rect, true);
    state.apply_movement(pkchat::editor::MovementKey::Left, rect, true);
    check(state.selection.has_range(), "shift movement creates a selection");
    check(state.selected_text() == "alpha", "selected text matches the highlighted range");

    pkchat::editor::EditorState ascii = pkchat::editor::EditorState::from_text("abcdef");
    ascii.cursor = 1;
    for (int i = 0; i < 3; ++i) {
        ascii.apply_movement(pkchat::editor::MovementKey::Right, rect, true);
    }
    check(ascii.selected_text() == "bcde",
          "shift-right selection includes the character at the cursor endpoint");
    ascii = pkchat::editor::EditorState::from_text("abcdef");
    ascii.cursor = 4;
    for (int i = 0; i < 3; ++i) {
        ascii.apply_movement(pkchat::editor::MovementKey::Left, rect, true);
    }
    check(ascii.selected_text() == "bcde",
          "shift-left selection includes the anchor-side endpoint character");
    ascii = pkchat::editor::EditorState::from_text("abcdef");
    ascii.cursor = 0;
    for (int i = 0; i < 2; ++i) {
        ascii.apply_movement(pkchat::editor::MovementKey::Right, rect, true);
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
    check(pkchat::editor::parse_byte_size("10M", parsed).ok() && parsed == 10LL * 1024LL * 1024LL,
          "editor auto-save byte size parses megabytes");
    check(pkchat::editor::parse_byte_size("512k", parsed).ok() && parsed == 512LL * 1024LL,
          "editor auto-save byte size parses kilobytes");
    check(pkchat::editor::parse_byte_size("2G", parsed).ok() && parsed == 2LL * 1024LL * 1024LL * 1024LL,
          "editor auto-save byte size parses gigabytes");
    check(pkchat::editor::parse_byte_size("1T", parsed).ok() &&
              parsed == 1024LL * 1024LL * 1024LL * 1024LL,
          "editor auto-save byte size parses terabytes");

    check(pkchat::editor::autosave_path_for("notes.txt", "~") == "notes.txt~",
          "editor auto-save path appends postfix to the file name");

    pkchat::editor::EditorSettings settings;
    settings.auto_save_mode = true;
    settings.auto_save_threshold = 300;
    settings.auto_save_timeout_seconds = 30;

    pkchat::editor::EditorState state = pkchat::editor::EditorState::from_text("alpha");
    state.path = "build/autosave-source.txt";
    state.dirty = true;
    state.record_autosave_change(299);
    const auto idle_short = std::chrono::seconds(5);
    check(!pkchat::editor::evaluate_autosave(state, settings, idle_short).should_save,
          "editor auto-save waits until the change threshold is reached");
    state.record_autosave_change(1);
    check(pkchat::editor::evaluate_autosave(state, settings, idle_short).threshold_met,
          "editor auto-save threshold triggers when enough bytes changed");
    state.reset_autosave_pending();
    state.record_autosave_change(10);
    const auto idle_long = std::chrono::seconds(31);
    check(pkchat::editor::evaluate_autosave(state, settings, idle_long).timeout_met,
          "editor auto-save timeout triggers after idle time with pending changes");

    settings.auto_save_size_limit = 4;
    state.text = pkchat::editor::PieceTable::from_string("12345");
    state.dirty = true;
    state.record_autosave_change(5);
    std::string skip_message;
    check(!pkchat::editor::perform_autosave(state, settings, skip_message).ok() &&
              skip_message.find("auto_save_size_limit") != std::string::npos,
          "editor auto-save skips buffers above the configured size limit");

    settings.auto_save_size_limit = pkchat::editor::kDefaultAutoSaveSizeLimit;
    state.text = pkchat::editor::PieceTable::from_string("autosave payload");
    state.dirty = true;
    state.record_autosave_change(100);
    std::string autosave_message;
    check(pkchat::editor::perform_autosave(state, settings, autosave_message).ok(),
          "editor auto-save writes the backup file");
    check(state.dirty, "editor auto-save does not clear the dirty flag");
    check(state.autosave_pending_bytes() == 0, "editor auto-save clears pending byte counter");
    check(autosave_message.find("build/autosave-source.txt~") != std::string::npos,
          "editor auto-save reports the backup path");

    pkchat::editor::PieceTable backup;
    check(pkchat::editor::load_file("build/autosave-source.txt~", backup).ok() &&
              backup.str() == "autosave payload",
          "editor auto-save backup file contains the current buffer");

    state.text = pkchat::editor::PieceTable::from_string("auto\nsave\n");
    state.linebreak = pkchat::editor::LineBreak::Crlf;
    state.dirty = true;
    state.record_autosave_change(10);
    check(pkchat::editor::perform_autosave(state, settings, autosave_message).ok(),
          "editor auto-save writes using the buffer linebreak mode");
    {
        std::ifstream raw_backup("build/autosave-source.txt~", std::ios::binary);
        const std::string raw((std::istreambuf_iterator<char>(raw_backup)),
                              std::istreambuf_iterator<char>());
        check(raw == "auto\r\nsave\r\n",
              "editor auto-save preserves CRLF and final-line-ending state");
    }

    pkchat::editor::remove_autosave_file(state.path, settings);
    check(!std::filesystem::exists("build/autosave-source.txt~"),
          "editor auto-save backup is removed after an explicit save cleanup");

    pkchat::config::ParseResult parsed_config = pkchat::config::parse(
        "[editor]\nauto-save-mode = off\nauto-save-postfix = \"#\"\nauto-save-threshold = 128\n"
        "auto-save-timeout = 12\nauto-save-size-limit = 2M\n",
        "autosave.conf");
    check(parsed_config.error.ok(), "editor auto-save config parses");
    pkchat::cli::Options options;
    pkchat::Error err = pkchat::config::apply_document(parsed_config.document, options);
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

    pkchat::editor::EditorSettings recovery_settings;
    const pkchat::editor::AutosaveRecoveryOffer offer =
        pkchat::editor::check_autosave_recovery_offer(main_path, recovery_settings);
    check(offer.should_offer && offer.autosave_path == backup_path,
          "editor auto-save recovery is offered when backup is newer");

    recovery_settings.auto_save_mode = false;
    check(!pkchat::editor::check_autosave_recovery_offer(main_path, recovery_settings).should_offer,
          "editor auto-save recovery is disabled when auto-save mode is off");

    const std::string prompt =
        pkchat::editor::autosave_recovery_prompt_message(main_path, backup_path);
    check(prompt.find(main_path) != std::string::npos && prompt.find(backup_path) != std::string::npos,
          "editor auto-save recovery prompt names both files");
}

void test_editor_undo_redo_key_bindings() {
    check(pkchat::editor::is_editor_undo_key(26), "Ctrl+Z is an editor undo key");
    check(pkchat::editor::is_editor_undo_key(21), "Ctrl+U is an editor undo key");
    check(!pkchat::editor::is_editor_undo_key(18), "Ctrl+R is not an editor undo key");

    check(!pkchat::editor::is_editor_redo_key(18), "Ctrl+R is not an editor redo key");
    check(pkchat::editor::is_editor_redo_key(25), "Ctrl+Y is an editor redo key");
    check(!pkchat::editor::is_editor_redo_key(26), "Ctrl+Z is not an editor redo key");

    unsigned char decoded = 0;
    check(pkchat::editor::decode_control_key_sequence("[26;5u", decoded) && decoded == 26,
          "kitty Ctrl+Z sequence decodes to undo key");
    check(pkchat::editor::decode_control_key_sequence("[18;5u", decoded) && decoded == 18,
          "kitty Ctrl+R sequence decodes to regenerate key");
}

void test_editor_revert_to_snapshot() {
    pkchat::editor::EditorState state = pkchat::editor::EditorState::from_text("alpha beta");
    state.cursor = state.text.offset_for_line_column(0, 6);
    const pkchat::editor::EditorSnapshot before = state.capture_state();
    pkchat::Error err = state.insert_without_undo("GAMMA");
    check(err.ok(), "editor insert before revert succeeds");
    check(state.text.str() == "alpha GAMMAbeta", "editor assist-style insert changes text");
    state.revert_to_snapshot(before);
    check(state.text.str() == "alpha beta", "editor revert_to_snapshot restores pre-assist buffer");
    check(state.cursor == before.cursor, "editor revert_to_snapshot restores cursor");
    check(state.dirty, "editor revert_to_snapshot marks buffer dirty after content change");
}

void test_editor_undo_redo() {
    pkchat::editor::EditorState state = pkchat::editor::EditorState::from_text("alpha");
    state.cursor = state.text.size();
    check(state.undo_limit() == pkchat::editor::kDefaultUndoLimit,
          "editor undo history defaults to five entries");

    pkchat::Error err = state.insert(" beta");
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

    pkchat::editor::EditorState limited = pkchat::editor::EditorState::from_text("");
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

    pkchat::editor::EditorState paste_state = pkchat::editor::EditorState::from_text("hello");
    paste_state.cursor = paste_state.text.size();
    pkchat::editor::Clipboard clipboard;
    clipboard.set(" world");
    check(paste_state.paste(clipboard).ok(), "editor paste succeeds");
    check(paste_state.text.str() == "hello world", "editor paste appends clipboard text");
    check(paste_state.undo(), "editor paste is undoable");
    check(paste_state.text.str() == "hello", "editor undo restores text before paste");
}

void test_editor_unicode_combining_sequence_wraps_on_grapheme_boundary() {
    const std::string combining = "e" "\xCC\x81";
    const std::string text = combining + "yy";

    pkchat::editor::EditorState state =
        pkchat::editor::EditorState::from_text(text);
    pkchat::editor::RenderedPanel rendered = state.render({1, 1, 3, 1});
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

    pkchat::editor::PieceTable cjk =
        pkchat::editor::PieceTable::from_string("a" + ni + "b");
    check(cjk.display_column_for_offset(1) == 1,
          "editor display column after ASCII is one");
    check(cjk.display_column_for_offset(1 + ni.size()) == 3,
          "editor display column counts a Chinese character as two terminal cells");
    check(cjk.offset_for_line_column(0, 2) == 1,
          "editor column lookup does not place the cursor inside a wide Chinese cell");
    check(cjk.offset_for_line_column(0, 3) == 1 + ni.size(),
          "editor column lookup reaches the byte offset after a wide Chinese character");

    pkchat::editor::PieceTable emoji_table =
        pkchat::editor::PieceTable::from_string("a" + emoji + "b");
    check(emoji_table.display_column_for_offset(1 + emoji.size()) == 3,
          "editor display column counts an emoji as two terminal cells");

    pkchat::editor::PieceTable combining_table =
        pkchat::editor::PieceTable::from_string(combining + "x");
    check(combining_table.display_column_for_offset(combining.size()) == 1,
          "editor display column gives combining marks zero width");
    check(combining_table.offset_for_line_column(0, 1) == combining.size(),
          "editor column lookup lands after the whole combining sequence");
}

void test_editor_unicode_emoji_pair_wraps_on_cell_boundaries() {
    const std::string grin = "\xF0\x9F\x98\x80";
    const std::string pair = grin + grin;

    pkchat::editor::EditorState state =
        pkchat::editor::EditorState::from_text(pair);
    pkchat::editor::RenderedPanel rendered = state.render({1, 1, 2, 3});
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

    pkchat::editor::EditorState state =
        pkchat::editor::EditorState::from_text(grapheme + "!");
    pkchat::editor::RenderedPanel rendered = state.render({1, 1, 1, 2});
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

    pkchat::editor::EditorState state =
        pkchat::editor::EditorState::from_text(family_emoji);
    pkchat::editor::RenderedPanel rendered = state.render({1, 1, 1, 2});
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

    pkchat::editor::EditorState combining =
        pkchat::editor::EditorState::from_text(composed_visual_e + "x");
    combining.move_right();
    check(combining.cursor == composed_visual_e.size(),
          "editor move_right treats base letter plus combining mark as one grapheme");
    combining.move_left();
    check(combining.cursor == 0,
          "editor move_left treats base letter plus combining mark as one grapheme");
    combining.cursor = composed_visual_e.size();
    pkchat::Error err = combining.erase_before_cursor();
    check(err.ok(), "editor backspace before a combining sequence succeeds");
    check(combining.text.str() == "x",
          "editor backspace removes the whole combining grapheme, not only the mark");

    pkchat::editor::EditorState emoji =
        pkchat::editor::EditorState::from_text(family_emoji + "!");
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

    pkchat::editor::EditorState cjk =
        pkchat::editor::EditorState::from_text(ni + hao);
    pkchat::editor::RenderedPanel rendered = cjk.render({1, 1, 2, 2});
    check(rendered.lines.size() == 2, "editor CJK render produces requested rows");
    check(rendered.lines[0] == ni,
          "editor wraps after one two-cell Chinese character in a two-column panel");
    check(rendered.lines[1] == hao,
          "editor keeps the second Chinese character intact on the next visual row");
    cjk.cursor = ni.size();
    rendered = cjk.render({1, 1, 2, 2});
    check(rendered.cursor.visible && rendered.cursor.row == 1 && rendered.cursor.col == 0,
          "editor cursor after a wide character at wrap boundary maps to the next visual row");

    pkchat::editor::EditorState marks =
        pkchat::editor::EditorState::from_text(combining + "x");
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

    pkchat::editor::EditorState state =
        pkchat::editor::EditorState::from_text(chinese + "\n" + arabic + "\n" +
                                               cyrillic + "\n" + nordic);
    check(state.text.line_count() == 4, "editor multilingual buffer has four lines");
    check(state.text.line_text(0) == chinese, "editor line_text preserves Chinese UTF-8");
    check(state.text.line_text(1) == arabic, "editor line_text preserves Arabic UTF-8");
    check(state.text.line_text(2) == cyrillic, "editor line_text preserves Cyrillic UTF-8");
    check(state.text.line_text(3) == nordic, "editor line_text preserves Nordic UTF-8");

    pkchat::editor::Clipboard clipboard;
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
    pkchat::Error err = state.replace_all_from(0, arabic, replacement, replacements);
    check(err.ok(), "editor replace-all accepts Unicode search and replacement");
    check(replacements == 1, "editor replace-all counts the Arabic occurrence");
    check(state.text.line_text(1) == replacement,
          "editor replace-all substitutes Arabic text with replacement UTF-8");

    const std::string path = "build/unit-editor-unicode.txt";
    err = pkchat::editor::save_file(path, state.text);
    check(err.ok(), "editor saves multilingual file");
    pkchat::editor::PieceTable loaded;
    err = pkchat::editor::load_file(path, loaded);
    check(err.ok(), "editor loads multilingual file");
    check(loaded.str() == state.text.str(),
          "editor file round trip preserves multilingual UTF-8 exactly");
}

void test_editor_utf8_codepoint_navigation_and_editing() {
    const std::string ni = "\xE4\xBD\xA0";
    const std::string hao = "\xE5\xA5\xBD";
    const std::string chinese = ni + hao;

    pkchat::editor::PieceTable table =
        pkchat::editor::PieceTable::from_string("A" + chinese + "B");
    check(table.next_char_offset(1) == 1 + ni.size(),
          "editor next_char_offset skips a complete three-byte UTF-8 code point");
    check(table.next_char_offset(1 + ni.size()) == 1 + chinese.size(),
          "editor next_char_offset skips the second Chinese code point");
    check(table.previous_char_offset(1 + chinese.size()) == 1 + ni.size(),
          "editor previous_char_offset lands on a UTF-8 leading byte");

    pkchat::editor::EditorState state =
        pkchat::editor::EditorState::from_text("A" + chinese + "B");
    state.move_right();
    check(state.cursor == 1, "editor move_right crosses ASCII one byte at a time");
    state.move_right();
    check(state.cursor == 1 + ni.size(), "editor move_right skips the first Chinese character bytes");
    state.move_right();
    check(state.cursor == 1 + chinese.size(), "editor move_right skips the second Chinese character bytes");
    state.move_left();
    check(state.cursor == 1 + ni.size(), "editor move_left skips a complete Chinese character");

    pkchat::Error err = state.erase_before_cursor();
    check(err.ok(), "editor backspace before cursor succeeds for UTF-8");
    check(state.text.str() == "A" + hao + "B",
          "editor backspace removes one full UTF-8 code point instead of one byte");
    check(state.cursor == 1, "editor backspace leaves cursor at the removed code point start");

    state = pkchat::editor::EditorState::from_text("A" + chinese + "B");
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

void test_editor_word_wrap_breaks_on_spaces() {
    pkchat::editor::EditorState state = pkchat::editor::EditorState::from_text("alpha beta");
    pkchat::editor::RenderedPanel rendered = state.render({1, 1, 2, 8});
    check(rendered.lines[0] == "alpha   ", "editor wraps at a word break when available");
    check(rendered.lines[1] == "beta    ", "editor continues after the wrapped word break");
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

void test_editor_file_io_failures() {
    pkchat::editor::PieceTable table;
    pkchat::Error err = pkchat::editor::load_file("build/editor-missing-file.txt", table);
    check(!err.ok() && err.code == pkchat::ErrorCode::FileRead,
          "editor load reports a file-read error for a missing path");

    table = pkchat::editor::PieceTable::from_string(u8"مرحبا 你好");
    err = pkchat::editor::save_file("build/no-such-dir/editor-save.txt", table);
    check(!err.ok() && err.code == pkchat::ErrorCode::FileWrite,
          "editor save to a missing parent directory reports a file-write error");
}

}  // namespace

void test_editor_control_key_sequence_decode() {
    unsigned char decoded = 0;
    check(pkchat::editor::decode_control_key_sequence("[110;5u", decoded) && decoded == 14,
          "editor decodes kitty-style Ctrl+N as new-buffer");
    check(pkchat::editor::decode_control_key_sequence("[19;5u", decoded) && decoded == 19,
          "editor decodes kitty-style Ctrl+S");
    check(pkchat::editor::decode_control_key_sequence("[27;5;19~", decoded) && decoded == 19,
          "editor decodes xterm modifyOtherKeys Ctrl+S");
    check(pkchat::editor::decode_control_key_sequence("[83;5u", decoded) &&
              decoded == pkchat::editor::editor_key_save_as(),
          "editor decodes kitty-style Ctrl+Shift+S as save-as");
    check(pkchat::editor::decode_control_key_sequence("[115;5u", decoded) &&
              decoded == pkchat::editor::editor_key_save_as(),
          "editor decodes kitty-style Ctrl+Shift+s as save-as");
    check(pkchat::editor::decode_control_key_sequence("[23;5u", decoded) && decoded == 23,
          "editor decodes kitty-style Ctrl+W as close-buffer");
    check(pkchat::editor::decode_control_key_sequence("[Z", decoded) &&
              decoded == pkchat::editor::editor_key_backtab(),
          "editor decodes common xterm Shift+Tab as backtab");
    check(pkchat::editor::decode_control_key_sequence("[9;2u", decoded) &&
              decoded == pkchat::editor::editor_key_backtab(),
          "editor decodes kitty Shift+Tab as backtab");
    check(pkchat::editor::decode_control_key_sequence("[27;2;9~", decoded) &&
              decoded == pkchat::editor::editor_key_backtab(),
          "editor decodes xterm modifyOtherKeys Shift+Tab as backtab");
    check(!pkchat::editor::decode_control_key_sequence("[A", decoded),
          "editor ignores arrow-key escape sequences");
}

void test_editor_save_as_overwrite_helpers() {
    const std::string existing = "build/pkchat-editor-save-as-existing.txt";
    std::ofstream out(existing, std::ios::trunc);
    check(static_cast<bool>(out), "editor save-as overwrite fixture is created");
    out << "existing";
    out.close();

    check(!pkchat::editor::needs_overwrite_confirm("build/pkchat-editor-save-as-missing.txt", "scratch.txt"),
          "editor save-as skips overwrite confirm for a new path");
    check(pkchat::editor::needs_overwrite_confirm(existing, "scratch.txt"),
          "editor save-as requires overwrite confirm when the target file exists");
    check(!pkchat::editor::needs_overwrite_confirm(existing, existing),
          "editor save skips overwrite confirm when saving to the current path");
    check(pkchat::editor::needs_overwrite_confirm(existing, ""),
          "editor save requires overwrite confirm when saving to an existing path from scratch");

    const std::string prompt = pkchat::editor::overwrite_prompt_message(existing);
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
    check(pkchat::editor::canonicalize_editor_target(target.string(), canonical).ok() &&
              fs::path(canonical).is_absolute(),
          "editor lock canonicalizes the target path");
    pkchat::editor::EditorLockAttempt first =
        pkchat::editor::acquire_editor_file_lock(target.string());
    check(first.lock != nullptr && fs::is_directory(canonical + ".LOCK"),
          "editor lock acquisition atomically creates FILE.LOCK");
    pkchat::editor::EditorLockOwner owner;
    check(pkchat::editor::read_editor_lock_owner(canonical + ".LOCK", owner).ok() &&
              owner.schema_version == 1 && owner.pid == static_cast<long long>(getpid()) &&
              owner.canonical_target == canonical && !owner.token.empty(),
          "editor lock writes complete bounded owner metadata");
    pkchat::editor::EditorLockAttempt contended =
        pkchat::editor::acquire_editor_file_lock(target.string());
    check(!contended.lock && contended.error.code == pkchat::ErrorCode::FileLock &&
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
    pkchat::editor::EditorLockAttempt remote =
        pkchat::editor::acquire_editor_file_lock(target.string());
    check(!remote.lock && remote.owner_metadata_valid &&
              remote.conflicting_owner.hostname == "remote-host" &&
              fs::exists(canonical + ".LOCK"),
          "remote-host lock owner is never removed automatically");

    const fs::path alias = root / "alias.txt";
    fs::create_symlink(target.filename(), alias);
    pkchat::editor::EditorLockAttempt alias_attempt =
        pkchat::editor::acquire_editor_file_lock(alias.string());
    check(!alias_attempt.lock && alias_attempt.conflicting_owner.canonical_target == canonical,
          "symlink aliases contend on the canonical target lock");

    pkchat::editor::EditorState copied;
    copied.set_path(target.string());
    copied.canonical_path = canonical;
    copied.file_lock = first.lock;
    pkchat::editor::EditorState copied_again = copied;
    first.lock.reset();
    copied.file_lock.reset();
    check(fs::exists(canonical + ".LOCK"),
          "EditorState copies share lock ownership for the full buffer lifetime");
    copied_again.file_lock.reset();
    check(!fs::exists(canonical + ".LOCK"), "last EditorState owner releases the lock directory");

    pkchat::editor::EditorLockAttempt token_lock =
        pkchat::editor::acquire_editor_file_lock(target.string());
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
        pkchat::editor::EditorLockAttempt child_lock =
            pkchat::editor::acquire_editor_file_lock(target.string());
        _exit(child_lock.lock ? 0 : 1);
    }
    int child_status = 0;
    waitpid(child, &child_status, 0);
    check(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0,
          "child creates a lock for stale recovery testing");
    pkchat::editor::EditorLockAttempt recovered =
        pkchat::editor::acquire_editor_file_lock(target.string());
    check(recovered.lock != nullptr && recovered.stale_lock_recovered,
          "dead same-host owner lock is recovered once");
    recovered.lock.reset();

    const pid_t nonempty_child = fork();
    if (nonempty_child == 0) {
        pkchat::editor::EditorLockAttempt child_lock =
            pkchat::editor::acquire_editor_file_lock(target.string());
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
    pkchat::editor::EditorLockAttempt nonempty =
        pkchat::editor::acquire_editor_file_lock(target.string());
    check(!nonempty.lock && fs::exists(canonical + ".LOCK/owner") &&
              fs::exists(canonical + ".LOCK/unexpected"),
          "dead local lock with unexpected contents is not removed recursively");
    fs::remove(canonical + ".LOCK/unexpected");
    fs::remove(canonical + ".LOCK/owner");
    fs::remove(canonical + ".LOCK");

    pkchat::editor::EditorLockAttempt upgrade_blocker =
        pkchat::editor::acquire_editor_file_lock(target.string());
    pkchat::editor::EditorState upgrade = pkchat::editor::EditorState::from_text("original");
    upgrade.set_path(target.string());
    check(upgrade.begin_file_session(target.string(), true).code == pkchat::ErrorCode::FileLock &&
              upgrade.read_only,
          "contended existing file begins as read-only");
    upgrade_blocker.lock.reset();
    check(upgrade.insert("!").ok() && !upgrade.read_only && upgrade.file_lock &&
              upgrade.text.str() == "!original",
          "first edit retries the lock and upgrades an unchanged file to writable");
    upgrade.release_file_session();

    pkchat::editor::EditorLockAttempt reload_blocker =
        pkchat::editor::acquire_editor_file_lock(target.string());
    pkchat::editor::EditorState reload_declined =
        pkchat::editor::EditorState::from_text("original");
    reload_declined.set_path(target.string());
    check(reload_declined.begin_file_session(target.string(), true).code ==
              pkchat::ErrorCode::FileLock,
          "changed-file reload fixture begins read-only");
    {
        std::ofstream out(target, std::ios::trunc);
        out << "external before retry";
    }
    reload_blocker.lock.reset();
    check(reload_declined.insert("!").code == pkchat::ErrorCode::FileLock &&
              reload_declined.reload_required && reload_declined.file_lock,
          "edit retry holds the newly acquired lock when disk content changed");
    pkchat::editor::MinibufferState reload_minibuffer;
    pkchat::editor::start_minibuffer(reload_minibuffer,
                                     pkchat::editor::MinibufferAction::ConfirmReloadAfterLock,
                                     "reload?");
    pkchat::editor::ReplaceSession reload_replace;
    pkchat::editor::EditorSettings reload_settings;
    std::string reload_search;
    std::string reload_pending_path;
    bool reload_quit = false;
    bool reload_pending_quit = false;
    pkchat::editor::PendingSaveRequest reload_pending_save;
    pkchat::editor::PendingAutosaveRecovery reload_recovery;
    pkchat::editor::PathCompleter reload_completer;
    check(pkchat::editor::handle_minibuffer_key(reload_declined,
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
    pkchat::editor::EditorState failed_save_as =
        pkchat::editor::EditorState::from_text("keep original session");
    failed_save_as.set_path(target.string());
    check(failed_save_as.begin_file_session(target.string(), true).ok(),
          "failed Save As fixture owns its original file session");
    const std::shared_ptr<pkchat::editor::EditorFileLock> original_session =
        failed_save_as.file_lock;
    pkchat::editor::EditorLockAttempt destination_blocker =
        pkchat::editor::acquire_editor_file_lock(blocked_destination.string());
    pkchat::editor::MinibufferState failed_save_minibuffer;
    pkchat::editor::PendingSaveRequest failed_save_pending;
    bool failed_save_quit = false;
    pkchat::editor::request_save_editor_to_path(failed_save_as,
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

    pkchat::editor::EditorLockAttempt save_as_blocker =
        pkchat::editor::acquire_editor_file_lock(target.string());
    pkchat::editor::EditorState save_as =
        pkchat::editor::EditorState::from_text("read-only save as");
    save_as.set_path(target.string());
    check(save_as.begin_file_session(target.string(), true).code == pkchat::ErrorCode::FileLock,
          "read-only Save As fixture is contended");
    const fs::path save_as_target = root / "retargeted.txt";
    pkchat::editor::MinibufferState save_as_minibuffer;
    pkchat::editor::PendingSaveRequest save_as_pending;
    bool save_as_quit = false;
    pkchat::editor::request_save_editor_to_path(save_as,
                                                save_as_target.string(),
                                                save_as_minibuffer,
                                                true,
                                                false,
                                                save_as_quit,
                                                save_as_pending,
                                                reload_settings);
    pkchat::editor::PieceTable save_as_saved;
    check(pkchat::editor::load_file(save_as_target.string(), save_as_saved).ok() &&
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
    pkchat::editor::EditorLockAttempt malformed =
        pkchat::editor::acquire_editor_file_lock(target.string());
    check(!malformed.lock && !malformed.owner_metadata_valid && fs::exists(canonical + ".LOCK"),
          "malformed unverifiable lock is never removed");
    fs::remove(canonical + ".LOCK/owner");
    fs::remove(canonical + ".LOCK");

    fs::create_directory(canonical + ".LOCK");
    pkchat::editor::EditorLockAttempt missing_metadata =
        pkchat::editor::acquire_editor_file_lock(target.string());
    check(!missing_metadata.lock && !missing_metadata.owner_metadata_valid &&
              fs::exists(canonical + ".LOCK"),
          "missing lock metadata is unverifiable and never removed automatically");
    fs::remove(canonical + ".LOCK");

    pkchat::editor::EditorState read_only = pkchat::editor::EditorState::from_text("abc");
    read_only.read_only = true;
    check(read_only.insert("x").code == pkchat::ErrorCode::FileLock &&
              read_only.erase_before_cursor().code == pkchat::ErrorCode::FileLock &&
              read_only.replace(0, 1, "z").code == pkchat::ErrorCode::FileLock &&
              read_only.indent().code == pkchat::ErrorCode::FileLock &&
              read_only.outdent().code == pkchat::ErrorCode::FileLock && !read_only.undo() &&
              !read_only.redo(),
          "central EditorState mutation guard rejects read-only changes");
    check(pkchat::editor::editor_status_line(read_only).find("[RO]") != std::string::npos,
          "read-only editor status renders [RO]");

    pkchat::editor::EditorState saving = pkchat::editor::EditorState::from_text("pkchat version");
    saving.set_path(target.string());
    check(saving.begin_file_session(target.string(), true).ok(),
          "writable editor state acquires its main-file lock");
    {
        std::ofstream out(target, std::ios::trunc);
        out << "external version is longer";
    }
    pkchat::editor::MinibufferState minibuffer;
    pkchat::editor::PendingSaveRequest pending;
    pkchat::editor::EditorSettings settings;
    bool quit = false;
    pkchat::editor::request_save_editor_to_path(saving,
                                                target.string(),
                                                minibuffer,
                                                true,
                                                false,
                                                quit,
                                                pending,
                                                settings);
    check(pending.external_change &&
              minibuffer.action == pkchat::editor::MinibufferAction::ConfirmOverwrite,
          "saving detects an external file fingerprint change");
    pkchat::editor::ReplaceSession replace;
    std::string search;
    std::string pending_load;
    bool pending_quit = false;
    pkchat::editor::PendingAutosaveRecovery recovery;
    pkchat::editor::PathCompleter completer;
    check(pkchat::editor::handle_minibuffer_key(saving,
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
    pkchat::editor::PieceTable cancelled;
    check(pkchat::editor::load_file(target.string(), cancelled).ok() &&
              cancelled.str() == "external version is longer",
          "cancelled overwrite preserves the external file");
    pkchat::editor::request_save_editor_to_path(saving,
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
    check(pkchat::editor::handle_minibuffer_key(saving,
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
              minibuffer.action == pkchat::editor::MinibufferAction::ConfirmOverwrite,
          "a second external change requires a new confirmation");
    check(pkchat::editor::handle_minibuffer_key(saving,
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
    pkchat::editor::PieceTable saved;
    check(pkchat::editor::load_file(target.string(), saved).ok() &&
              saved.str() == "pkchat version" && saving.has_disk_fingerprint,
          "confirmed overwrite saves content and refreshes the fingerprint");
    saving.release_file_session();
    fs::remove_all(root);
}

void test_editor_help_document_and_command() {
    std::string help_text;
    pkchat::Error err = pkchat::editor::load_editor_help_markdown(help_text);
    check(err.ok() && !help_text.empty(), "editor help document loads");
    check(help_text.find("# pkchat Editor Help") != std::string::npos,
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
    check(help_text.find("Choose a model with /model") != std::string::npos,
          "editor help document documents deferred model selection");
    check(help_text.find("/provider") != std::string::npos && help_text.find("/model") != std::string::npos,
          "editor help document documents /provider and /model");

    check(pkchat::editor::is_editor_help_command("/help"), "editor /help command is recognized");
    check(pkchat::editor::is_editor_help_command("  /HELP  "), "editor /help command is case-insensitive");
    check(!pkchat::editor::is_editor_help_command("/helpful"), "editor help command rejects prefixes");

    const std::vector<std::string> completions =
        pkchat::editor::assist_command_completions(pkchat::editor::default_editor_assist_config());
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
    check(std::find(completions.begin(), completions.end(), "/provider ") != completions.end(),
          "assist command completions include /provider");
    check(std::find(completions.begin(), completions.end(), "/model ") != completions.end(),
          "assist command completions include /model");
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

    pkchat::editor::ParsedEditorSlashCommand slash =
        pkchat::editor::parse_editor_slash_command("/save");
    check(slash.command == pkchat::editor::EditorSlashCommand::Save && slash.path.empty(),
          "editor /save slash command is recognized");
    slash = pkchat::editor::parse_editor_slash_command("/SAVEAS");
    check(slash.command == pkchat::editor::EditorSlashCommand::SaveAs && slash.path.empty(),
          "editor /saveas slash command is case-insensitive");
    slash = pkchat::editor::parse_editor_slash_command("/open build/unit-editor.txt");
    check(slash.command == pkchat::editor::EditorSlashCommand::Open &&
              slash.path == "build/unit-editor.txt",
          "editor /open PATH preserves the path argument");
    slash = pkchat::editor::parse_editor_slash_command("/saveas out/new.txt");
    check(slash.command == pkchat::editor::EditorSlashCommand::SaveAs &&
              slash.path == "out/new.txt",
          "editor /saveas PATH preserves the path argument");
    slash = pkchat::editor::parse_editor_slash_command("/find");
    check(slash.command == pkchat::editor::EditorSlashCommand::Find,
          "editor /find slash command is recognized");
    slash = pkchat::editor::parse_editor_slash_command("/replace");
    check(slash.command == pkchat::editor::EditorSlashCommand::Replace,
          "editor /replace slash command is recognized");
    slash = pkchat::editor::parse_editor_slash_command("/open");
    check(slash.command == pkchat::editor::EditorSlashCommand::Open && slash.path.empty(),
          "editor bare /open slash command is recognized");
    slash = pkchat::editor::parse_editor_slash_command("/new");
    check(slash.command == pkchat::editor::EditorSlashCommand::New && slash.path.empty(),
          "editor /new slash command is recognized");
    slash = pkchat::editor::parse_editor_slash_command("/new name");
    check(slash.command == pkchat::editor::EditorSlashCommand::None,
          "editor /new rejects arguments");
    slash = pkchat::editor::parse_editor_slash_command("/list");
    check(slash.command == pkchat::editor::EditorSlashCommand::List && slash.path.empty(),
          "editor /list slash command is recognized");
    slash = pkchat::editor::parse_editor_slash_command("/close");
    check(slash.command == pkchat::editor::EditorSlashCommand::Close && slash.path.empty(),
          "editor /close slash command is recognized");
    slash = pkchat::editor::parse_editor_slash_command("/close file.txt");
    check(slash.command == pkchat::editor::EditorSlashCommand::None,
          "editor /close rejects arguments");
    slash = pkchat::editor::parse_editor_slash_command("/chat");
    check(slash.command == pkchat::editor::EditorSlashCommand::Chat && slash.path.empty(),
          "editor /chat slash command is recognized");
    check(std::find(completions.begin(), completions.end(), "/chat") != completions.end(),
          "assist command completions include /chat");
    slash = pkchat::editor::parse_editor_slash_command("/save extra words");
    check(slash.command == pkchat::editor::EditorSlashCommand::None,
          "editor file slash commands reject multi-token path arguments");

    check(pkchat::editor::editor_assist_path_prefix_length("/open build/") == 6,
          "editor assist path mode starts after /open");
    check(pkchat::editor::editor_assist_path_prefix_length("/saveas foo") == 8,
          "editor assist path mode starts after /saveas");
    check(pkchat::editor::editor_assist_path_prefix_length("/insert build/") == 8,
          "editor assist path mode starts after /insert");
    check(pkchat::editor::editor_assist_path_prefix_length("/open") == std::string::npos,
          "editor assist path mode requires a separator after /open");
    check(pkchat::editor::editor_assist_path_prefix_length("/insert") == std::string::npos,
          "editor assist path mode requires a separator after /insert");
    check(pkchat::editor::editor_assist_path_prefix_length("/search query") == std::string::npos,
          "editor assist path mode ignores non-file commands");
}

void test_editor_assist_path_completion() {
    const std::string directory = "build/pkchat-assist-path-completion";
    std::error_code filesystem_error;
    std::filesystem::create_directories(directory, filesystem_error);
    check(!filesystem_error, "assist path completion fixture directory is created");

    const std::string file = directory + "/target.txt";
    std::ofstream fixture(file, std::ios::binary | std::ios::trunc);
    fixture << "ok";
    check(static_cast<bool>(fixture), "assist path completion fixture file is written");

    pkchat::editor::AssistCompleterState completer;
    std::string input = "/open " + directory + "/tar";
    pkchat::editor::AssistCompletionResult result =
        pkchat::editor::complete_assist_command(input, completer, pkchat::editor::default_editor_assist_config());
    check(result.kind == pkchat::editor::CompletionKind::Path && result.error.ok() &&
              result.match_count == 1,
          "assist path completion finds a unique file after /open");
    check(input == "/open " + file, "assist path completion completes /open PATH");

    completer = pkchat::editor::AssistCompleterState{};
    input = "/insert " + directory + "/tar";
    result = pkchat::editor::complete_assist_command(
        input, completer, pkchat::editor::default_editor_assist_config());
    check(result.kind == pkchat::editor::CompletionKind::Path && result.error.ok() &&
              result.match_count == 1,
          "assist path completion finds a unique file after /insert");
    check(input == "/insert " + file, "assist path completion completes /insert PATH");

    completer = pkchat::editor::AssistCompleterState{};
    input = "/search " + directory + "/tar";
    result = pkchat::editor::complete_assist_command(input, completer, pkchat::editor::default_editor_assist_config());
    check(result.kind == pkchat::editor::CompletionKind::Command,
          "assist tab completion stays in command mode for /search");
    check(input == "/search " + directory + "/tar",
          "assist tab completion does not complete paths for /search");
}

void test_editor_minibuffer_paste() {
    pkchat::editor::MinibufferState minibuffer;
    check(!pkchat::editor::paste_into_minibuffer(minibuffer, "ignored").ok(),
          "minibuffer paste requires an active prompt");

    pkchat::editor::start_minibuffer(minibuffer,
                                     pkchat::editor::MinibufferAction::AssistCommand,
                                     "Command: ",
                                     "/insert ");
    check(pkchat::editor::paste_into_minibuffer(
              minibuffer, "https://example.com/page?x=1&y=2\r\n").ok(),
          "command minibuffer accepts a pasted URL with a trailing newline");
    check(minibuffer.input == "/insert https://example.com/page?x=1&y=2",
          "command minibuffer strips trailing paste newlines");

    const std::string before_multiline = minibuffer.input;
    check(!pkchat::editor::paste_into_minibuffer(minibuffer, "first\nsecond").ok(),
          "command minibuffer rejects multiline paste");
    check(minibuffer.input == before_multiline,
          "rejected multiline paste leaves command input unchanged");

    pkchat::editor::start_minibuffer(minibuffer,
                                     pkchat::editor::MinibufferAction::ConfirmQuit,
                                     "Quit? ");
    check(!pkchat::editor::paste_into_minibuffer(minibuffer, "y").ok(),
          "confirmation minibuffers reject pasted answers");
}

void test_editor_missing_file_error_message() {
    pkchat::editor::FileLoadCheck load_check;
    pkchat::editor::EditorSettings settings;
    pkchat::Error err =
        pkchat::editor::check_load_file_size("this_file_doesnt_exist.txt", settings, load_check);
    check(!err.ok() && err.message == "file not found: this_file_doesnt_exist.txt",
          "editor missing file load reports file not found");
}

void test_editor_buffer_list_helpers() {
    std::vector<pkchat::editor::EditorState> buffers;
    pkchat::editor::EditorState first = pkchat::editor::EditorState::from_text("alpha");
    first.path = "file1.txt";
    first.cursor = first.text.size();
    buffers.push_back(first);

    pkchat::editor::EditorState second = pkchat::editor::EditorState::from_text("beta\nsecond");
    second.path = "file2.txt";
    second.dirty = true;
    second.cursor = second.text.size();
    buffers.push_back(second);

    pkchat::editor::EditorState scratch;
    buffers.push_back(scratch);

    const std::string rendered = pkchat::editor::editor_buffer_list_text(buffers, 1);
    check(rendered.find("Buffers - Enter opens - N new - DEL close - Esc cancels") != std::string::npos,
          "editor buffer list includes chooser instructions");
    check(rendered.find("  file1.txt - Ln 1, Col 6") != std::string::npos,
          "editor buffer list renders an inactive clean file");
    check(rendered.find("> file2.txt * - Ln 2, Col 7") != std::string::npos,
          "editor buffer list marks selected dirty file");
    check(rendered.find("  [scratch 3] - Ln 1, Col 1") != std::string::npos,
          "editor buffer list renders scratch buffers with stable labels");

    check(pkchat::editor::move_editor_buffer_selection(1, buffers.size(), pkchat::editor::MovementKey::Up) == 0,
          "editor buffer list moves selection up");
    check(pkchat::editor::move_editor_buffer_selection(0, buffers.size(), pkchat::editor::MovementKey::Down) == 1,
          "editor buffer list moves selection down");
    check(pkchat::editor::move_editor_buffer_selection(1, buffers.size(), pkchat::editor::MovementKey::Home) == 0,
          "editor buffer list home selects first buffer");
    check(pkchat::editor::move_editor_buffer_selection(0, buffers.size(), pkchat::editor::MovementKey::End) == 2,
          "editor buffer list end selects last buffer");

    pkchat::editor::Clipboard clipboard;
    first.select_all();
    check(first.copy_selection(clipboard).ok(), "editor copies from one buffer");
    second.cursor = second.text.size();
    check(second.paste(clipboard).ok(), "editor pastes copied text into another buffer");
    check(second.text.str() == "beta\nsecondalpha",
          "editor clipboard content is independent of the source buffer");
}

void test_editor_markdown_mode_and_structured_highlighting() {
    pkchat::editor::EditorState state =
        pkchat::editor::EditorState::from_text("# Heading and *emphasis*");
    state.set_path("README.MD");
    state.highlight_enabled = true;
    check(state.language == pkchat::highlight::Language::Markdown && state.language_automatic,
          "editor automatically detects Markdown case-insensitively");

    pkchat::editor::EditorState new_file_state;
    new_file_state.set_path("definitely-does-not-exist-yet.md");
    check(new_file_state.language == pkchat::highlight::Language::Markdown &&
              new_file_state.language_automatic,
          "editor detects Markdown from a new path before the file exists");

    const std::vector<std::pair<const char*, pkchat::highlight::Language>> detected_modes = {
        {"new.py", pkchat::highlight::Language::Python},
        {"new.c", pkchat::highlight::Language::C},
        {"new.hpp", pkchat::highlight::Language::Cpp},
        {"new.cs", pkchat::highlight::Language::CSharp},
        {"new.java", pkchat::highlight::Language::Java},
        {"new.jsx", pkchat::highlight::Language::JavaScript},
        {"new.tsx", pkchat::highlight::Language::TypeScript},
        {"new.html", pkchat::highlight::Language::Html},
        {"new.css", pkchat::highlight::Language::Css},
        {"new.xml", pkchat::highlight::Language::Xml},
        {"new.jsonl", pkchat::highlight::Language::Json},
        {"new.sh", pkchat::highlight::Language::Bash},
        {"new.php", pkchat::highlight::Language::Php},
        {"new.pl", pkchat::highlight::Language::Perl},
        {"new.rb", pkchat::highlight::Language::Ruby},
        {"new.rs", pkchat::highlight::Language::Rust},
        {"new.go", pkchat::highlight::Language::Go},
        {"new.ps1", pkchat::highlight::Language::PowerShell},
        {"new.asm", pkchat::highlight::Language::Assembly},
        {"new.sql", pkchat::highlight::Language::Sql},
        {"new.toml", pkchat::highlight::Language::Toml},
        {"new.yaml", pkchat::highlight::Language::Yaml},
        {"new.ini", pkchat::highlight::Language::Ini},
    };
    for (const auto& detected : detected_modes) {
        pkchat::editor::EditorState detected_state;
        detected_state.set_path(detected.first);
        check(detected_state.language == detected.second && detected_state.language_automatic,
              std::string("editor automatically selects mode for new file: ") + detected.first);
    }

    state.selection.anchor = 2;
    state.selection.active = 8;
    const pkchat::editor::RenderedPanel rendered = state.render({1, 1, 1, 40});
    check(rendered.lines.size() == 1 && rendered.lines[0].find("\x1b") == std::string::npos,
          "editor rendered text contains no embedded ANSI selection markup");
    bool saw_heading = false;
    bool saw_selected_heading = false;
    for (const pkchat::editor::RenderedPanel::Span& span : rendered.line_spans[0]) {
        saw_heading = saw_heading ||
                      (span.syntax && span.role == pkchat::highlight::TokenRole::Heading);
        saw_selected_heading = saw_selected_heading ||
                               (span.syntax && span.selected &&
                                span.role == pkchat::highlight::TokenRole::Heading);
    }
    check(saw_heading, "editor rendering includes Markdown heading spans");
    check(saw_selected_heading,
          "editor rendering overlays selection independently on Markdown syntax spans");

    pkchat::editor::EditorState python_state =
        pkchat::editor::EditorState::from_text("def greet(name: str): return 17");
    python_state.set_language(pkchat::highlight::Language::Python, false);
    python_state.highlight_enabled = true;
    const pkchat::editor::RenderedPanel python_rendered = python_state.render({1, 1, 1, 40});
    bool saw_python_keyword = false;
    bool saw_python_type = false;
    for (const pkchat::editor::RenderedPanel::Span& span : python_rendered.line_spans[0]) {
        saw_python_keyword = saw_python_keyword ||
                             (span.syntax && span.role == pkchat::highlight::TokenRole::Keyword);
        saw_python_type = saw_python_type ||
                          (span.syntax && span.role == pkchat::highlight::TokenRole::Type);
    }
    check(saw_python_keyword && saw_python_type,
          "editor rendering applies a manually selected programming-language mode");

    state.set_language(pkchat::highlight::Language::Text, false);
    state.set_path("renamed.md");
    check(state.language == pkchat::highlight::Language::Text && !state.language_automatic,
          "editor manual text mode survives save-as path changes");
    state.language_automatic = true;
    state.redetect_language();
    check(state.language == pkchat::highlight::Language::Markdown,
          "editor automatic mode resumes filename detection");
    state.set_path("renamed.txt");
    check(state.language == pkchat::highlight::Language::Text,
          "editor automatic mode re-detects after save-as");

    state.linebreak = pkchat::editor::LineBreak::Lf;
    const std::string status = pkchat::editor::editor_status_line(state);
    check(status.find("(text LF)") != std::string::npos &&
              status.find("Mode: Editor") == std::string::npos &&
              status.find("Syntax:") == std::string::npos &&
              status.find("(auto)") == std::string::npos &&
              status.find("(manual)") == std::string::npos,
          "editor status line displays the compact syntax and LF mode");
    state.linebreak = pkchat::editor::LineBreak::Crlf;
    check(pkchat::editor::editor_status_line(state).find("(text CRLF)") != std::string::npos,
          "editor status line displays CRLF mode");
    state.linebreak = pkchat::editor::LineBreak::Cr;
    check(pkchat::editor::editor_status_line(state).find("(text CR)") != std::string::npos,
          "editor status line displays CR mode");
}

void run_all() {
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

}  // namespace pkchat::test::editor
