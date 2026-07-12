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
#include "editor/path_completion.hpp"
#include "editor/selection.hpp"
#include "editor/terminal_input.hpp"
#include "editor/terminal_ui.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

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
    check(default_settings.max_read_chars == pkchat::editor::kDefaultAiContinueReadChars,
          "default continue read uses the configured 16k default");

    const char* previous_read = std::getenv("MAX_AI_CONTINUE_READ");
    const char* previous_tokens = std::getenv("MAX_AI_CONTINUE_TOKENS");
#if defined(_WIN32)
    _putenv_s("MAX_AI_CONTINUE_READ", "16");
    _putenv_s("MAX_AI_CONTINUE_TOKENS", "2048");
#else
    setenv("MAX_AI_CONTINUE_READ", "16", 1);
    setenv("MAX_AI_CONTINUE_TOKENS", "2048", 1);
#endif
    const pkchat::editor::AiContinueSettings env_settings = pkchat::editor::ai_continue_settings(default_options);
    check(env_settings.max_read_chars == 16, "MAX_AI_CONTINUE_READ overrides default");
    check(env_settings.max_output_tokens == 2048, "MAX_AI_CONTINUE_TOKENS overrides default");
#if defined(_WIN32)
    if (previous_read != nullptr) {
        _putenv_s("MAX_AI_CONTINUE_READ", previous_read);
    } else {
        _putenv_s("MAX_AI_CONTINUE_READ", "");
    }
    if (previous_tokens != nullptr) {
        _putenv_s("MAX_AI_CONTINUE_TOKENS", previous_tokens);
    } else {
        _putenv_s("MAX_AI_CONTINUE_TOKENS", "");
    }
#else
    if (previous_read != nullptr) {
        setenv("MAX_AI_CONTINUE_READ", previous_read, 1);
    } else {
        unsetenv("MAX_AI_CONTINUE_READ");
    }
    if (previous_tokens != nullptr) {
        setenv("MAX_AI_CONTINUE_TOKENS", previous_tokens, 1);
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
          "bare /continue requests scope");

    parsed = pkchat::editor::parse_assist_command("/fact continue", default_config);
    check(parsed.ok && parsed.kind == pkchat::editor::AssistCommandKind::Configured &&
              parsed.scope == pkchat::editor::AssistScope::Continue,
          "/fact continue parses");

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

    parsed = pkchat::editor::parse_assist_command("/english insert", default_config);
    check(parsed.ok && parsed.kind == pkchat::editor::AssistCommandKind::Configured &&
              parsed.scope == pkchat::editor::AssistScope::Insert,
          "/English parses case-insensitively");

    parsed = pkchat::editor::parse_assist_command("/Chinese continue", default_config);
    check(parsed.ok && parsed.kind == pkchat::editor::AssistCommandKind::Configured &&
              parsed.scope == pkchat::editor::AssistScope::Continue,
          "/Chinese continue parses");

    parsed = pkchat::editor::parse_assist_command("/Finnish all", default_config);
    check(parsed.ok && parsed.kind == pkchat::editor::AssistCommandKind::Configured &&
              parsed.scope == pkchat::editor::AssistScope::All,
          "/Finnish all parses");

    parsed = pkchat::editor::parse_assist_command("/prompt rewrite formally", default_config);
    check(parsed.ok && parsed.kind == pkchat::editor::AssistCommandKind::Prompt &&
              parsed.custom_prompt == "rewrite formally",
          "/prompt captures custom text");

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
    for (const char* builtin : {"/spell", "/grammar", "/continue", "/fact", "/comment", "/rewrite",
                                "/English", "/Chinese", "/Finnish"}) {
        for (const char* mode : {"selection", "all", "continue", "insert"}) {
            const std::string variant = std::string(builtin) + " " + mode;
            check(std::find(completions.begin(), completions.end(), variant) != completions.end(),
                  std::string("builtin assist completions include ") + variant);
        }
    }
    for (const char* builtin : {"/spell", "/grammar", "/continue", "/fact", "/comment", "/rewrite",
                                "/English", "/Chinese", "/Finnish"}) {
        const pkchat::editor::EditorAssistCommand* command =
            pkchat::editor::find_assist_command(default_config, builtin);
        check(command != nullptr && command->modes.size() == 4,
              std::string("default ") + builtin + " exposes all four scoped modes");
        const std::string scope_prompt = pkchat::editor::assist_scope_prompt(*command);
        check(scope_prompt.find("selection (s)") != std::string::npos &&
                  scope_prompt.find("all (a)") != std::string::npos &&
                  scope_prompt.find("continue (c)") != std::string::npos &&
                  scope_prompt.find("insert (i)") != std::string::npos,
              std::string("default ") + builtin + " scope prompt lists all four modes");
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
              execution.edit_kind == pkchat::editor::AssistEditKind::StreamInsert,
          "/continue continue builds streaming execution");
    check(execution.messages.back().content == "<content>hello wrld</content>",
          "/continue continue sends tail-before-cursor context as input");
    check(execution.usage_messages.empty(),
          "/continue continue omits separate usage messages when the full prefix fits the read limit");

    context.settings.max_read_chars = 4096;
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
    pkchat::editor::EditorState state = pkchat::editor::EditorState::from_text("alpha beta\ngamma");
    state.cursor = state.text.offset_for_line_column(0, 6);
    pkchat::Error err = state.kill_to_line_end();
    check(err.ok(), "editor kill to line end succeeds");
    check(state.text.str() == "alpha \ngamma", "editor kill to line end erases text before newline only");
    check(state.cursor == state.text.offset_for_line_column(0, 6), "editor kill to line end keeps cursor in place");
    check(state.dirty, "editor kill to line end marks dirty after deleting text");

    err = state.kill_to_line_end();
    check(err.ok(), "editor kill at end of line succeeds");
    check(state.text.str() == "alpha \ngamma", "editor kill at end of non-empty line leaves newline intact");

    pkchat::editor::EditorState middle = pkchat::editor::EditorState::from_text("alpha\n\ngamma");
    middle.cursor = middle.text.line_start(1);
    err = middle.kill_to_line_end();
    check(err.ok(), "editor kill empty middle line succeeds");
    check(middle.text.str() == "alpha\ngamma", "editor kill empty middle line removes that line");
    check(middle.cursor == middle.text.line_start(1), "editor kill empty middle line keeps cursor at next line start");

    pkchat::editor::EditorState last = pkchat::editor::EditorState::from_text("alpha\n");
    last.cursor = last.text.line_start(1);
    err = last.kill_to_line_end();
    check(err.ok(), "editor kill empty final line succeeds");
    check(last.text.str() == "alpha", "editor kill empty final line removes preceding newline");
    check(last.cursor == last.text.size(), "editor kill empty final line moves cursor to new end");

    pkchat::editor::EditorState only = pkchat::editor::EditorState::from_text("");
    err = only.kill_to_line_end();
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
    std::filesystem::last_write_time(backup_path, main_time + std::filesystem::file_time_type::clock::now().time_since_epoch() -
                                                         main_time.time_since_epoch() +
                                                         std::filesystem::file_time_type::duration{3600});

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
    slash = pkchat::editor::parse_editor_slash_command("/save extra words");
    check(slash.command == pkchat::editor::EditorSlashCommand::None,
          "editor file slash commands reject multi-token path arguments");

    check(pkchat::editor::editor_assist_path_prefix_length("/open build/") == 6,
          "editor assist path mode starts after /open");
    check(pkchat::editor::editor_assist_path_prefix_length("/saveas foo") == 8,
          "editor assist path mode starts after /saveas");
    check(pkchat::editor::editor_assist_path_prefix_length("/open") == std::string::npos,
          "editor assist path mode requires a separator after /open");
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
    input = "/search " + directory + "/tar";
    result = pkchat::editor::complete_assist_command(input, completer, pkchat::editor::default_editor_assist_config());
    check(result.kind == pkchat::editor::CompletionKind::Command,
          "assist tab completion stays in command mode for /search");
    check(input == "/search " + directory + "/tar",
          "assist tab completion does not complete paths for /search");
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
    check(rendered.find("Buffers - Enter opens - N new - Esc cancels") != std::string::npos,
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

void run_all() {
    test_editor_control_key_sequence_decode();
    test_editor_save_as_overwrite_helpers();
    test_editor_help_document_and_command();
    test_editor_assist_path_completion();
    test_editor_missing_file_error_message();
    test_editor_buffer_list_helpers();
    test_editor_ai_continue_helpers();
    test_editor_ai_setup_helpers();
    test_editor_file_io_failures();
    test_editor_assist_helpers();
    test_editor_contextual_completion_modes();
    test_editor_file_round_trip();
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
