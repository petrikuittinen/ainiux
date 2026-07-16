#include "config/test_config.hpp"
#include "support/test_support.hpp"
#include "cli/args.hpp"
#include "config/config.hpp"
#include "editor/editor.hpp"
#include "editor/editor_prompts.hpp"
#include "tui/theme_registry.hpp"
#include "provider/provider.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace pkchat::test::config {

namespace {

using pkchat::test::check;
using pkchat::test::read_fixture;

void test_config_applies_user_settings() {
    pkchat::config::ParseResult parsed =
        pkchat::config::read_file("tests/fixtures/config-home/pkchat/config.conf");
    check(parsed.error.ok(), "user config fixture parses");

    pkchat::cli::Options options;
    pkchat::Error err = pkchat::config::apply_document(parsed.document, options);
    check(err.ok(), "user config fixture passes schema validation");
    check(options.allow_private_url_fetch, "user config enables private URL fetching");
    check(options.auto_convert_html_to_markdown,
          "HTML-to-Markdown insertion conversion defaults to enabled");
    check(options.tui_theme == "dark", "user config selects the dark theme");
    check(options.show_thinking_traces, "user config shows thinking traces by default");

    pkchat::config::ParseResult highlight_config =
        pkchat::config::parse("[tui]\nhighlight = off\n", "highlight.conf");
    err = pkchat::config::apply_document(highlight_config.document, options);
    check(err.ok() && !options.tui_highlight,
          "tui.highlight accepts the on/off compatibility form");
    highlight_config = pkchat::config::parse("[tui]\nhighlight = true\n", "highlight-bool.conf");
    err = pkchat::config::apply_document(highlight_config.document, options);
    check(err.ok() && options.tui_highlight,
          "tui.highlight accepts boolean values");

    pkchat::config::ParseResult editor_config =
        pkchat::config::parse("[editor]\nundo_limit = 7\nhuge_file_size_warning = 2048\nfile_size_limit = -1\n"
                              "tab-width = 8\ntab-style = tab\nlinebreak = crlf\n"
                              "continue_prefix_max_chars = 8192\n"
                              "continue_postfix_max_chars = 1024\n"
                              "continue_prose_prefix_max_chars = 16300\n"
                              "continue_prose_postfix_max_chars = 4000\n"
                              "continue_max_tokens = 4096\n",
                              "editor.conf");
    check(editor_config.error.ok(), "editor config fixture parses");
    err = pkchat::config::apply_document(editor_config.document, options);
    check(err.ok() && options.editor_undo_limit == 7 &&
              options.editor_huge_file_size_warning == 2048 &&
              options.editor_file_size_limit == -1 &&
              options.editor_tab_width == 8 &&
              options.editor_tab_style == pkchat::editor::TabStyle::Tab &&
              options.editor_linebreak == pkchat::editor::LineBreak::Crlf &&
              options.editor_ai_continue_prefix_max_chars == 8192 &&
              options.editor_ai_continue_postfix_max_chars == 1024 &&
              options.editor_ai_continue_prose_prefix_max_chars == 16300 &&
              options.editor_ai_continue_prose_postfix_max_chars == 4000 &&
              options.editor_ai_continue_max_tokens == 4096,
          "editor config settings apply");

    pkchat::config::ParseResult zero_continue_config = pkchat::config::parse(
        "[editor]\ncontinue_prefix_max_chars = 0\ncontinue_postfix_max_chars = 0\n"
        "continue_prose_prefix_max_chars = 0\ncontinue_prose_postfix_max_chars = 0\n",
        "editor-zero.conf");
    err = pkchat::config::apply_document(zero_continue_config.document, options);
    check(err.ok() && options.editor_ai_continue_prefix_max_chars == 0 &&
              options.editor_ai_continue_postfix_max_chars == 0 &&
              options.editor_ai_continue_prose_prefix_max_chars == 0 &&
              options.editor_ai_continue_prose_postfix_max_chars == 0,
          "zero config limits disable all editor continuation context sides");

    pkchat::config::ParseResult invalid_prose_continue_config = pkchat::config::parse(
        "[editor]\ncontinue_prose_prefix_max_chars = -1\n", "editor-prose-invalid.conf");
    err = pkchat::config::apply_document(invalid_prose_continue_config.document, options);
    check(!err.ok() && err.message.find("non-negative") != std::string::npos,
          "negative prose continuation config limit is rejected");

    pkchat::config::ParseResult overflow_prose_continue_config = pkchat::config::parse(
        "[editor]\ncontinue_prose_postfix_max_chars = 999999999999999999999999999999\n",
        "editor-prose-overflow.conf");
    check(!overflow_prose_continue_config.error.ok(),
          "overflowing prose continuation config limit is rejected while parsing");

    pkchat::config::ParseResult removed_continue_config = pkchat::config::parse(
        "[editor]\ncontinue_read_chars = 10\n", "editor-removed.conf");
    err = pkchat::config::apply_document(removed_continue_config.document, options);
    check(!err.ok() && err.message.find("continue_read_chars") != std::string::npos,
          "removed editor continue_read_chars config setting is rejected");

    pkchat::cli::Options layered;
    pkchat::config::ParseResult system_continue_config = pkchat::config::parse(
        "[editor]\ncontinue_prefix_max_chars = 10\ncontinue_postfix_max_chars = 20\n"
        "continue_prose_prefix_max_chars = 100\ncontinue_prose_postfix_max_chars = 200\n",
        "system-editor.conf");
    pkchat::config::ParseResult user_continue_config = pkchat::config::parse(
        "[editor]\ncontinue_prefix_max_chars = 30\ncontinue_prose_prefix_max_chars = 300\n",
        "user-editor.conf");
    check(pkchat::config::apply_document(system_continue_config.document, layered).ok() &&
              pkchat::config::apply_document(user_continue_config.document, layered).ok(),
          "system and user continuation settings layer successfully");
    const char* layered_argv[] = {
        "pkchat", "--editor-continue-postfix-max-chars", "40",
        "--editor-continue-prose-postfix-max-chars", "400"};
    const pkchat::cli::ParseResult layered_cli =
        pkchat::cli::parse_args(5, const_cast<char**>(layered_argv), layered);
    check(layered_cli.error.ok() &&
              layered_cli.options.editor_ai_continue_prefix_max_chars == 30 &&
              layered_cli.options.editor_ai_continue_postfix_max_chars == 40 &&
              layered_cli.options.editor_ai_continue_prose_prefix_max_chars == 300 &&
              layered_cli.options.editor_ai_continue_prose_postfix_max_chars == 400,
          "continuation settings follow system then user then CLI precedence");

    pkchat::config::ParseResult insert_config = pkchat::config::parse(
        "[input]\nauto-convert-html-to-md = no\n", "insert.conf");
    check(insert_config.error.ok(), "insert conversion config fixture parses");
    err = pkchat::config::apply_document(insert_config.document, options);
    check(err.ok() && !options.auto_convert_html_to_markdown,
          "input auto-convert-html-to-md accepts no");

    options.editor_assist_config = pkchat::editor::default_editor_assist_config();
    pkchat::config::ParseResult assist_prompt_config = pkchat::config::parse(
        "[editor]\nassist_fact = \"Custom fact prompt\"\n", "assist-fact.conf");
    check(assist_prompt_config.error.ok(), "editor assist fact config parses");
    err = pkchat::config::apply_document(assist_prompt_config.document, options);
    const pkchat::editor::EditorAssistCommand* fact_command =
        pkchat::editor::find_assist_command(options.editor_assist_config, "/fact");
    check(err.ok() && fact_command != nullptr && fact_command->prompt == "Custom fact prompt",
          "editor assist fact config applies");

    const std::string system_home =
        std::filesystem::absolute("build/config-system").lexically_normal().string();
    std::filesystem::create_directories(system_home + "/pkchat");
    {
        std::ofstream system_config(system_home + "/pkchat/config.conf", std::ios::trunc);
        check(system_config.is_open(), "system config test file opens");
        system_config << "[url_fetch]\nallow_private_addresses = false\n"
                         "[tui]\ntheme = light\nthinking_traces = false\n";
        system_config.close();
        check(system_config.good(), "system config test file is written");
    }
    const std::string config_home =
        std::filesystem::absolute("tests/fixtures/config-home").lexically_normal().string();
    pkchat::config::Environment environment{config_home, system_home, "/nonexistent"};
    pkchat::config::LoadResult loaded = pkchat::config::load_automatic(pkchat::cli::Options{}, environment);
    check(loaded.error.ok(), "automatic user config loading succeeds");
    check(loaded.loaded_paths.size() == 4 &&
              loaded.loaded_paths[0].find("themes.conf") != std::string::npos &&
              loaded.loaded_paths[1].find("editor-commands.conf") != std::string::npos &&
              loaded.loaded_paths[2] == system_home + "/pkchat/config.conf" &&
              loaded.loaded_paths[3] == config_home + "/pkchat/config.conf",
          "automatic loading applies bundled themes and editor commands before system and user config");
    check(loaded.options.tui_themes.has("dark") && loaded.options.tui_themes.has("light") &&
              loaded.options.tui_themes.has("sepia"),
          "automatic loading includes built-in themes");
    const pkchat::editor::EditorAssistCommand* loaded_spell =
        pkchat::editor::find_assist_command(loaded.options.editor_assist_config, "/spell");
    check(loaded_spell != nullptr && loaded_spell->prompt.find("spelling") != std::string::npos,
          "automatic loading includes built-in editor assist commands");
    check(loaded.options.allow_private_url_fetch && loaded.options.show_thinking_traces &&
              loaded.options.tui_theme == "dark",
          "user settings partially override automatic system settings");

    pkchat::config::LoadResult system_only =
        pkchat::config::load_automatic(pkchat::cli::Options{}, environment, false);
    check(system_only.error.ok() && !system_only.options.allow_private_url_fetch &&
              !system_only.options.show_thinking_traces && system_only.options.tui_theme == "light",
          "disabling user config retains the automatic system config");
    check(system_only.loaded_paths.size() == 3 &&
              system_only.loaded_paths[0].find("themes.conf") != std::string::npos &&
              system_only.loaded_paths[1].find("editor-commands.conf") != std::string::npos &&
              system_only.loaded_paths[2] == system_home + "/pkchat/config.conf",
          "disabling user config still loads bundled themes, editor commands, and system config");
    bool skipped_user_config = false;
    for (const pkchat::config::ConfigDiagnostic& diagnostic : system_only.diagnostics) {
        if (diagnostic.scope == pkchat::config::ConfigScope::User &&
            diagnostic.kind == pkchat::config::ConfigFileKind::Config &&
            diagnostic.state == pkchat::config::ConfigFileState::Skipped) {
            skipped_user_config = true;
        }
    }
    check(skipped_user_config, "disabled user config is reported as skipped");

    const char* argv[] = {"pkchat", "--no-stream", "--nocolors"};
    pkchat::cli::ParseResult cli =
        pkchat::cli::parse_args(3, const_cast<char**>(argv), loaded.options);
    check(cli.error.ok() && !cli.options.stream && cli.options.no_colors,
          "command-line arguments apply over configured defaults");

    pkchat::cli::Options offline_base;
    offline_base.provider = "none";
    const char* positional_argv[] = {"pkchat", "lmstudio", "--list-models"};
    pkchat::cli::ParseResult positional =
        pkchat::cli::parse_args(3, const_cast<char**>(positional_argv), offline_base);
    pkchat::provider::ContextResult positional_context =
        pkchat::provider::build_context(positional.options);
    check(positional.error.ok() && positional_context.error.ok() &&
              positional_context.context.profile.name == "lm_studio",
          "positional provider shortcut overrides a configured provider");
}

void test_config_file_read_errors() {
    pkchat::config::ParseResult parsed =
        pkchat::config::read_file("build/config-file-does-not-exist.conf");
    check(!parsed.error.ok() && parsed.error.code == pkchat::ErrorCode::Config,
          "missing config file reports config error");

    parsed = pkchat::config::read_file("build", pkchat::config::kMaxConfigBytes);
    check(!parsed.error.ok() && parsed.error.message.find("not a regular file") != std::string::npos,
          "config reader rejects a directory");

    parsed = pkchat::config::read_file("config/pkchat.conf", 16);
    check(!parsed.error.ok() && parsed.error.message.find("exceeds 16 byte limit") != std::string::npos,
          "config reader enforces its byte limit");
}

void test_config_parses_supported_values() {
    const std::string input =
        std::string("\xEF\xBB\xBF") +
        "config_version = 1\r\n"
        "title = \"line\\nquote: \\\" slash: \\\\ tab:\\t\"\r\n"
        "url = https://example.test/page#fragment\r\n"
        "greeting = 你好 مرحبا 👋\r\n"
        "negative = -12\r\n"
        "ratio = 3.25\r\n"
        "scientific = 1e3\r\n"
        "enabled = false\r\n"
        "window = 64k\r\n"
        "[nested.section]\r\n"
        "name = value\r\n";
    pkchat::config::ParseResult parsed = pkchat::config::parse(input, "unicode.conf");
    check(parsed.error.ok(), "config accepts BOM, CRLF, Unicode, and supported scalar types");

    const pkchat::config::Entry* title = parsed.document.find("title");
    check(title != nullptr && title->value.string == "line\nquote: \" slash: \\ tab:\t",
          "quoted config escapes are decoded");
    const pkchat::config::Entry* url = parsed.document.find("url");
    check(url != nullptr && url->value.string == "https://example.test/page#fragment",
          "bare config string preserves hash characters");
    const pkchat::config::Entry* greeting = parsed.document.find("greeting");
    check(greeting != nullptr && greeting->value.string == "你好 مرحبا 👋", "bare config string preserves UTF-8");
    const pkchat::config::Entry* negative = parsed.document.find("negative");
    check(negative != nullptr && negative->value.is_integer() && negative->value.integer == -12,
          "signed config integer parsed");
    const pkchat::config::Entry* ratio = parsed.document.find("ratio");
    check(ratio != nullptr && ratio->value.is_float() && ratio->value.floating == 3.25,
          "config decimal float parsed");
    const pkchat::config::Entry* scientific = parsed.document.find("scientific");
    check(scientific != nullptr && scientific->value.is_float() && scientific->value.floating == 1000.0,
          "config exponent float parsed");
    const pkchat::config::Entry* enabled = parsed.document.find("enabled");
    check(enabled != nullptr && enabled->value.is_boolean() && !enabled->value.boolean,
          "config false boolean parsed");
    const pkchat::config::Entry* window = parsed.document.find("window");
    check(window != nullptr && window->value.is_string() && window->value.string == "64k",
          "context shorthand remains a schema-level string");
    const pkchat::config::Entry* nested = parsed.document.find("nested.section", "name");
    check(nested != nullptr && nested->value.string == "value", "dotted config section parsed");
}

void test_config_parses_multiline_strings() {
    const std::string input =
        "title = \"\"\"\n"
        "  first line\r\n"
        "  second \\\"quoted\\\" # [not a section]\r\n"
        "  escapes: \\n \\r \\t \\\\"
        "\r\n"
        "\"\"\"   \r\n"
        "next = value\r\n";
    pkchat::config::ParseResult parsed = pkchat::config::parse(input, "multiline.conf");
    check(parsed.error.ok(), "config accepts escaped multiline triple-quoted strings");
    const pkchat::config::Entry* title = parsed.document.find("title");
    check(title != nullptr &&
              title->value.string ==
                  "  first line\n  second \"quoted\" # [not a section]\n  escapes: \n \r \t \\\n",
          "multiline strings remove only the opening newline and normalize CRLF");
    const pkchat::config::Entry* next = parsed.document.find("next");
    check(next != nullptr && next->value.string == "value",
          "parser resumes at the assignment after a multiline string");

    parsed = pkchat::config::parse("value = \"\"\"unterminated\n", "multiline-end.conf");
    check(!parsed.error.ok() && parsed.error.message.find("multiline-end.conf:1:") != std::string::npos &&
              parsed.error.message.find("unterminated") != std::string::npos,
          "unterminated multiline strings report the opening source location");
    parsed = pkchat::config::parse("value = \"\"\"bad\\q\"\"\"\n", "multiline-escape.conf");
    check(!parsed.error.ok() && parsed.error.message.find("multiline-escape.conf:1:16") != std::string::npos &&
              parsed.error.message.find("escape") != std::string::npos,
          "multiline strings reject unsupported escapes with a source location");
    parsed = pkchat::config::parse("value = \"\"\"ok\"\"\" trailing\n", "multiline-trailing.conf");
    check(!parsed.error.ok() && parsed.error.message.find("multiline-trailing.conf:1:18") !=
                  std::string::npos &&
              parsed.error.message.find("unexpected text after multiline quoted string") !=
                  std::string::npos,
          "multiline strings reject non-whitespace trailing text");
}

void test_config_applies_model_settings() {
    pkchat::config::ParseResult parsed = pkchat::config::parse(
        "[Model-setting]\n"
        "model = Qwen3.6-*\n"
        "purpose = coding\n"
        "default_system_prompt = \"\"\n"
        "temperature = 0.6\n"
        "top_k = 20\n"
        "top_p = 0.95\n"
        "min_p = 0.0\n"
        "repeat_penalty = 1.0\n"
        "presence_penalty = 0.0\n",
        "model-setting.conf");
    check(parsed.error.ok(), "model-setting config parses");

    pkchat::cli::Options options;
    pkchat::Error err = pkchat::config::apply_document(parsed.document, options);
    check(err.ok() && options.model_settings.size() == 1, "model-setting config applies");
    const pkchat::ModelSetting& setting = options.model_settings.front();
    check(setting.model == "Qwen3.6-*" && setting.purpose == "coding" &&
              setting.default_system_prompt.empty() && setting.temperature == 0.6 && setting.top_k == 20 &&
              setting.top_p == 0.95 && setting.min_p == 0.0 && setting.repeat_penalty == 1.0 &&
              setting.presence_penalty == 0.0,
          "model-setting values are stored");

    pkchat::config::ParseResult override_config = pkchat::config::parse(
        "[Model-setting]\n"
        "model = Qwen3.6-*\n"
        "purpose = coding\n"
        "default_system_prompt = \"\"\n"
        "temperature = 0.4\n"
        "top_k = 10\n"
        "top_p = 0.8\n"
        "min_p = 0.0\n"
        "repeat_penalty = 1.0\n"
        "presence_penalty = 0.0\n",
        "model-setting-override.conf");
    options.model_settings.push_back({"Gemma-4-31B", "general", "", 1.0, 64, 0.95, 0.0, 1.0, 0.0});
    err = pkchat::config::apply_document(override_config.document, options);
    check(err.ok() && options.model_settings.size() == 2, "model-setting merge keeps other entries");
    check(options.model_settings[0].model == "Qwen3.6-*" && options.model_settings[0].temperature == 0.4 &&
              options.model_settings[0].top_k == 10 && options.model_settings[1].model == "Gemma-4-31B",
          "model-setting merge replaces matching model and purpose in place");
}

void test_config_model_setting_thinking_budget() {
    pkchat::config::ParseResult verbal = pkchat::config::parse(
        "[Model-setting]\n"
        "model = Qwen3.6-*\n"
        "purpose = general\n"
        "default_system_prompt = \"\"\n"
        "temperature = 0.8\n"
        "top_k = 20\n"
        "top_p = 0.95\n"
        "min_p = 0.0\n"
        "repeat_penalty = 1.0\n"
        "presence_penalty = 1.5\n"
        "thinking_budget = high\n",
        "model-setting-verbal-budget.conf");
    pkchat::cli::Options options;
    pkchat::Error err = pkchat::config::apply_document(verbal.document, options);
    check(err.ok() && options.model_settings.size() == 1 &&
              options.model_settings.front().thinking_budget == "high",
          "model-setting config accepts verbal thinking_budget");

    pkchat::config::ParseResult numeric = pkchat::config::parse(
        "[Model-setting]\n"
        "model = Qwen3.6-*\n"
        "purpose = instruct\n"
        "default_system_prompt = \"\"\n"
        "temperature = 0.7\n"
        "top_k = 20\n"
        "top_p = 0.80\n"
        "min_p = 0.0\n"
        "repeat_penalty = 1.0\n"
        "presence_penalty = 1.5\n"
        "thinking_budget = 8192\n",
        "model-setting-token-budget.conf");
    err = pkchat::config::apply_document(numeric.document, options);
    check(err.ok() && options.model_settings.size() == 2 &&
              options.model_settings.back().thinking_budget == "8192",
          "model-setting config accepts token thinking_budget");
}

void test_config_reads_common_template() {
    pkchat::config::ParseResult parsed = pkchat::config::read_file("config/pkchat.conf");
    check(parsed.error.ok(), "common config file parses");
    check(parsed.document.entries.size() == 161, "common config has every expected setting");
    pkchat::cli::Options highlight_options;
    pkchat::Error apply_error = pkchat::config::apply_document(parsed.document, highlight_options);
    check(apply_error.ok() && highlight_options.tui_highlight,
          "common config enables syntax highlighting by default");

    const pkchat::config::Entry* provider = parsed.document.find("provider");
    check(provider != nullptr && provider->value.is_string() && provider->value.string == "openai",
          "common config provider is stored as a string");
    check(provider != nullptr && provider->source.path == "config/pkchat.conf" && provider->source.line == 12,
          "common config entry retains source path and line");

    const pkchat::config::Entry* stream = parsed.document.find("generation", "stream");
    check(stream != nullptr && stream->value.is_boolean() && stream->value.boolean,
          "common config streaming value is a boolean");
    const pkchat::config::Entry* timeout = parsed.document.find("network.connect_timeout_seconds");
    check(timeout != nullptr && timeout->value.is_integer() && timeout->value.integer == 10,
          "common config timeout is an integer");
    const pkchat::config::Entry* model = parsed.document.find("model");
    check(model != nullptr && model->value.is_string() && model->value.string.empty(),
          "common config supports an empty bare string");

    pkchat::cli::Options options;
    pkchat::Error err = pkchat::config::apply_document(parsed.document, options);
    check(err.ok(), "every common config setting passes schema validation");
    check(options.provider == "openai" && options.stream && options.tui_theme == "dark" &&
              !options.show_thinking_traces && !options.allow_private_url_fetch &&
              options.max_web_search_results == 3 && options.web_search_provider == "auto" &&
              options.editor_undo_limit == 5 &&
              options.editor_huge_file_size_warning == 1073741824LL &&
              options.editor_file_size_limit == -1 && options.editor_auto_save_mode &&
              options.auto_convert_html_to_markdown &&
              options.editor_auto_save_postfix == "~" && options.editor_auto_save_threshold == 300 &&
              options.editor_auto_save_timeout_seconds == 30 &&
              options.editor_auto_save_size_limit == 10LL * 1024LL * 1024LL &&
              options.editor_ai_continue_prefix_max_chars ==
                  pkchat::editor::kDefaultAiContinuePrefixMaxChars &&
              options.editor_ai_continue_postfix_max_chars ==
                  pkchat::editor::kDefaultAiContinuePostfixMaxChars &&
              options.editor_ai_continue_prose_prefix_max_chars ==
                  pkchat::editor::kDefaultAiContinueProsePrefixMaxChars &&
              options.editor_ai_continue_prose_postfix_max_chars ==
                  pkchat::editor::kDefaultAiContinueProsePostfixMaxChars &&
              options.editor_ai_continue_max_tokens == pkchat::editor::kDefaultAiContinueMaxTokens,
          "common config maps to the built-in runtime defaults");
    check(options.model_settings.size() == 12, "common config includes model-setting presets");
    check(options.model_settings.front().model == "Qwen3.6-*" &&
              options.model_settings.front().purpose == "creative" &&
              options.model_settings.front().temperature == 1.0,
          "common config model-setting presets preserve order and values");
}

void test_config_rejects_invalid_input() {
    pkchat::config::ParseResult parsed = pkchat::config::parse("key = one\nkey = two\n", "duplicate.conf");
    check(!parsed.error.ok() && parsed.error.code == pkchat::ErrorCode::Config,
          "duplicate config key is rejected");
    check(parsed.error.message.find("duplicate.conf:2:1") != std::string::npos &&
              parsed.error.message.find("first defined at duplicate.conf:1:1") != std::string::npos,
          "duplicate config error reports both source locations");
    check(parsed.document.entries.empty(), "failed config parse returns no partially applied document");

    parsed = pkchat::config::parse("value = \"bad\\q\"\n", "escape.conf");
    check(!parsed.error.ok() && parsed.error.message.find("escape.conf:1:") != std::string::npos,
          "unsupported config string escape is rejected with location");

    parsed = pkchat::config::parse("number = 999999999999999999999999999\n", "overflow.conf");
    check(!parsed.error.ok() && parsed.error.message.find("signed 64-bit") != std::string::npos,
          "overflowing config integer is rejected");

    parsed = pkchat::config::parse("[bad section]\nkey = value\n", "section.conf");
    check(!parsed.error.ok() && parsed.error.message.find("invalid section name") != std::string::npos,
          "invalid config section name is rejected");

    const std::string invalid_utf8 = std::string("name = ") + static_cast<char>(0xC3) + "(\n";
    parsed = pkchat::config::parse(invalid_utf8, "utf8.conf");
    check(!parsed.error.ok() && parsed.error.message.find("utf8.conf:1:8") != std::string::npos &&
              parsed.error.message.find("invalid UTF-8") != std::string::npos,
          "invalid config UTF-8 is rejected at its byte column");
}

void test_config_schema_rejects_invalid_settings_transactionally() {
    pkchat::cli::Options options;
    options.tui_theme = "light";
    pkchat::config::ParseResult unknown =
        pkchat::config::parse("[tui]\ntheme = dark\ntypo = true\n", "unknown.conf");
    pkchat::Error err = pkchat::config::apply_document(unknown.document, options);
    check(!err.ok() && err.code == pkchat::ErrorCode::Config &&
              err.message.find("tui.typo") != std::string::npos,
          "config schema rejects unknown keys with the qualified name");
    check(options.tui_theme == "light", "invalid config does not partially change options");

    pkchat::config::ParseResult wrong_type =
        pkchat::config::parse("[url_fetch]\nallow_private_addresses = yes\n", "type.conf");
    err = pkchat::config::apply_document(wrong_type.document, options);
    check(!err.ok() && err.message.find("expected boolean, got string") != std::string::npos,
          "config schema reports expected and actual types");

    pkchat::config::ParseResult bad_version =
        pkchat::config::parse("config_version = 2\n", "version.conf");
    err = pkchat::config::apply_document(bad_version.document, options);
    check(!err.ok() && err.message.find("supported version is 1") != std::string::npos,
          "config schema rejects unsupported versions");

    pkchat::config::ParseResult bad_editor_limit =
        pkchat::config::parse("[editor]\nfile_size_limit = -2\n", "editor-limit.conf");
    err = pkchat::config::apply_document(bad_editor_limit.document, options);
    check(!err.ok() && err.message.find("expected -1 or a non-negative byte limit") != std::string::npos,
          "config schema rejects editor file limits below -1");

    pkchat::config::ParseResult bad_undo =
        pkchat::config::parse("[editor]\nundo_limit = -1\n", "editor-undo.conf");
    err = pkchat::config::apply_document(bad_undo.document, options);
    check(!err.ok() && err.message.find("non-negative integer") != std::string::npos,
          "config schema rejects negative editor undo limits");

    pkchat::config::ParseResult bad_tab_width =
        pkchat::config::parse("[editor]\ntab-width = 0\n", "editor-tab-width.conf");
    err = pkchat::config::apply_document(bad_tab_width.document, options);
    check(!err.ok() && err.message.find("1 through 32") != std::string::npos,
          "config schema rejects editor tab widths outside 1 through 32");

    pkchat::config::ParseResult bad_tab_style =
        pkchat::config::parse("[editor]\ntab-style = tabs\n", "editor-tab-style.conf");
    err = pkchat::config::apply_document(bad_tab_style.document, options);
    check(!err.ok() && err.message.find("spaces or tab") != std::string::npos,
          "config schema rejects unknown editor tab styles");

    pkchat::config::ParseResult bad_linebreak =
        pkchat::config::parse("[editor]\nlinebreak = native\n", "editor-linebreak.conf");
    err = pkchat::config::apply_document(bad_linebreak.document, options);
    check(!err.ok() && err.message.find("lf, cr, or crlf") != std::string::npos,
          "config schema rejects unknown editor linebreak modes");
}

void test_config_xdg_path_resolution() {
    pkchat::config::Environment environment{"relative", "/high:relative:/low", "/home/tester"};
    check(pkchat::config::user_config_path(environment) == "/home/tester/.config/pkchat/config.conf",
          "relative XDG_CONFIG_HOME falls back to HOME");
    check(pkchat::config::user_editor_commands_path(environment) ==
              "/home/tester/.config/pkchat/editor-commands.conf",
          "relative XDG_CONFIG_HOME falls back to HOME for editor commands");
    check(pkchat::config::user_themes_path(environment) == "/home/tester/.config/pkchat/themes.conf",
          "relative XDG_CONFIG_HOME falls back to HOME for themes");
    const std::vector<std::string> system = pkchat::config::system_config_paths(environment);
    check(system.size() == 2 && system[0] == "/low/pkchat/config.conf" &&
              system[1] == "/high/pkchat/config.conf",
          "system config directories load in reverse order and ignore relative entries");
    const std::vector<std::string> editor_commands =
        pkchat::config::system_editor_commands_paths(environment);
    check(editor_commands.size() == 2 &&
              editor_commands[0] == "/low/pkchat/editor-commands.conf" &&
              editor_commands[1] == "/high/pkchat/editor-commands.conf",
          "system editor-commands directories load in reverse order and ignore relative entries");
    const std::vector<std::string> themes = pkchat::config::system_themes_paths(environment);
    check(themes.size() == 2 && themes[0] == "/low/pkchat/themes.conf" &&
              themes[1] == "/high/pkchat/themes.conf",
          "system themes directories load in reverse order and ignore relative entries");
}

void test_themes_config() {
    pkchat::config::ParseResult parsed = pkchat::config::read_file("config/themes.conf");
    check(parsed.error.ok(), "themes.conf parses");
    pkchat::cli::Options options;
    pkchat::Error err = pkchat::config::apply_themes_document(parsed.document, options);
    check(err.ok(), "themes.conf applies");
    check(options.tui_themes.has("dark") && options.tui_themes.has("light") &&
              options.tui_themes.has("sepia"),
          "themes.conf defines built-in dark, light, and sepia themes");

    pkchat::config::ParseResult invalid = pkchat::config::parse("provider = openai\n", "bad-themes.conf");
    check(invalid.error.ok(), "invalid themes fixture parses");
    err = pkchat::config::apply_themes_document(invalid.document, options);
    check(!err.ok() && err.message.find("unknown themes setting") != std::string::npos,
          "themes.conf rejects unrelated settings");

    const std::string system_home =
        std::filesystem::absolute("build/config-theme-system").lexically_normal().string();
    std::filesystem::create_directories(system_home + "/pkchat");
    {
        std::ofstream system_themes(system_home + "/pkchat/themes.conf", std::ios::trunc);
        check(system_themes.is_open(), "system themes test file opens");
        system_themes << "[theme]\nname = sepia\nbackground = #F4ECD8\ntext = #5B4636\n"
                         "muted = #7A6A58\nthinking_trace = #6E5F4D\nuser_label = #8B5E34\n"
                         "assistant_label = #4F6F46\nerror = #9B2C2C\nstatus_foreground = #5B4636\n"
                         "status_background = #E8DCC8\nthinking_activity = #8B5E34\n"
                         "streaming_activity = #4F6F46\npanel_title = #8B5E34\npanel_border = #7A6A58\n"
                         "panel_hint = #6E5F4D\npanel_highlight = #B7791F\npanel_body = #5B4636\n"
                         "panel_background = #EFE2C8\n";
        system_themes.close();
        check(system_themes.good(), "system themes test file is written");
    }
    const std::string config_home =
        std::filesystem::absolute("build/config-theme-user").lexically_normal().string();
    std::filesystem::create_directories(config_home + "/pkchat");
    {
        std::ofstream user_themes(config_home + "/pkchat/themes.conf", std::ios::trunc);
        check(user_themes.is_open(), "user themes test file opens");
        user_themes << "[theme]\nname = dark\nbackground = #101010\ntext = #EEEEEE\n"
                         "muted = #AAAAAA\nthinking_trace = #888888\nuser_label = #66CCFF\n"
                         "assistant_label = #66FF99\nerror = #FF6666\nstatus_foreground = #FFFFFF\n"
                         "status_background = #202020\nthinking_activity = #66CCFF\n"
                         "streaming_activity = #66FF99\npanel_title = #66CCFF\npanel_border = #888888\n"
                         "panel_hint = #777777\npanel_highlight = #FFCC66\npanel_body = #DDDDDD\n"
                         "panel_background = #181818\n";
        user_themes.close();
        check(user_themes.good(), "user themes test file is written");
    }
    pkchat::config::Environment environment{config_home, system_home, "/nonexistent"};
    pkchat::config::LoadResult loaded = pkchat::config::load_automatic(pkchat::cli::Options{}, environment);
    check(loaded.error.ok(), "automatic loading with themes overrides succeeds");
    check(loaded.options.tui_themes.has("sepia") && loaded.options.tui_themes.has("dark"),
          "system and user themes merge into the registry");
    const pkchat::tui::ThemePalette* dark = loaded.options.tui_themes.find("dark");
    check(dark != nullptr && dark->background.r == 0x10 && dark->background.g == 0x10 &&
              dark->background.b == 0x10,
          "user themes.conf overrides the built-in dark theme");

    pkchat::config::ParseResult config_override =
        pkchat::config::parse("[tui]\ntheme = sepia\n", "config-theme.conf");
    check(config_override.error.ok(), "config theme override parses");
    err = pkchat::config::apply_document(config_override.document, loaded.options);
    check(err.ok() && loaded.options.tui_theme == "sepia",
          "config.conf selects a custom theme by name");
}

void test_editor_commands_config() {
    pkchat::config::ParseResult parsed = pkchat::config::read_file("config/editor-commands.conf");
    check(parsed.error.ok(), "editor-commands.conf parses");
    pkchat::cli::Options options;
    pkchat::Error err = pkchat::config::apply_editor_commands_document(parsed.document, options);
    check(err.ok(), "editor-commands.conf applies");
    const pkchat::editor::EditorAssistCommand* spell =
        pkchat::editor::find_assist_command(options.editor_assist_config, "/spell");
    const pkchat::editor::EditorAssistCommand* continue_command =
        pkchat::editor::find_assist_command(options.editor_assist_config, "/continue");
    check(spell != nullptr && continue_command != nullptr,
          "editor-commands.conf defines built-in assist commands");
    check(options.editor_assist_config.behavior_rules.find("one-shot") != std::string::npos,
          "editor-commands.conf defines assist behavior rules");

    pkchat::config::ParseResult invalid = pkchat::config::parse("provider = openai\n", "bad-editor.conf");
    check(invalid.error.ok(), "invalid editor-commands fixture parses");
    err = pkchat::config::apply_editor_commands_document(invalid.document, options);
    check(!err.ok() && err.message.find("unknown editor-commands setting") != std::string::npos,
          "editor-commands.conf rejects unrelated settings");

    const std::string system_home =
        std::filesystem::absolute("build/config-editor-system").lexically_normal().string();
    std::filesystem::create_directories(system_home + "/pkchat");
    {
        std::ofstream system_commands(system_home + "/pkchat/editor-commands.conf", std::ios::trunc);
        check(system_commands.is_open(), "system editor-commands test file opens");
        system_commands << "[command]\nstring = /spell\nmodes = selection, all\n"
                           "prompt = \"System spell override\"\n";
        system_commands.close();
        check(system_commands.good(), "system editor-commands test file is written");
    }
    const std::string config_home =
        std::filesystem::absolute("build/config-editor-user").lexically_normal().string();
    std::filesystem::create_directories(config_home + "/pkchat");
    {
        std::ofstream user_commands(config_home + "/pkchat/editor-commands.conf", std::ios::trunc);
        check(user_commands.is_open(), "user editor-commands test file opens");
        user_commands << "[command]\nstring = /spell\nmodes = selection, all\n"
                         "prompt = \"User spell override\"\n";
        user_commands.close();
        check(user_commands.good(), "user editor-commands test file is written");
    }
    pkchat::config::Environment environment{config_home, system_home, "/nonexistent"};
    pkchat::config::LoadResult loaded = pkchat::config::load_automatic(pkchat::cli::Options{}, environment);
    check(loaded.error.ok(), "automatic loading with editor-commands overrides succeeds");
    const pkchat::editor::EditorAssistCommand* overridden_spell =
        pkchat::editor::find_assist_command(loaded.options.editor_assist_config, "/spell");
    check(overridden_spell != nullptr && overridden_spell->prompt == "User spell override",
          "user editor-commands.conf overrides system editor-commands.conf");

    pkchat::config::ParseResult config_override = pkchat::config::parse(
        "[editor]\nassist_spell = \"Config spell override\"\n", "config-spell.conf");
    check(config_override.error.ok(), "config assist_spell override parses");
    err = pkchat::config::apply_document(config_override.document, loaded.options);
    overridden_spell = pkchat::editor::find_assist_command(loaded.options.editor_assist_config, "/spell");
    check(err.ok() && overridden_spell != nullptr &&
              overridden_spell->prompt == "Config spell override",
          "config.conf assist_spell overrides editor-commands.conf");

    pkchat::config::ParseResult defaults = pkchat::config::parse(
        "[command]\nstring = demo\nprompt = \"Demo prompt\"\n", "command-defaults.conf");
    options = pkchat::cli::Options{};
    err = pkchat::config::apply_document(defaults.document, options);
    const pkchat::editor::EditorAssistCommand* demo =
        pkchat::editor::find_assist_command(options.editor_assist_config, "/DEMO");
    check(err.ok() && demo != nullptr && demo->modes.size() == 4 &&
              demo->prompt == "Demo prompt",
          "missing command modes receive the standard four editor modes");
    const pkchat::config::ParseResult legacy = pkchat::config::parse(
        "[command]\nstring = /demo\nmodes = continue\nprompt = \"Legacy\"\n",
        "command-legacy.conf");
    options = pkchat::cli::Options{};
    err = pkchat::config::apply_document(legacy.document, options);
    demo = pkchat::editor::find_assist_command(options.editor_assist_config, "demo");
    check(err.ok() && demo != nullptr && demo->modes.size() == 1 &&
              demo->modes.front() == pkchat::editor::AssistCommandMode::Continue,
          "legacy slash-prefixed command strings and explicit modes remain authoritative");
}

void test_config_empty_and_numeric_edge_cases() {
    pkchat::config::ParseResult parsed = pkchat::config::parse("", "empty.conf");
    check(parsed.error.ok() && parsed.document.entries.empty(),
          "empty config document parses successfully");

    parsed = pkchat::config::parse("tiny = 0.0000000012\nzero = 0\n", "numeric.conf");
    check(parsed.error.ok(), "config parser accepts tiny floats and zero integers");
    const pkchat::config::Entry* tiny = parsed.document.find("tiny");
    const pkchat::config::Entry* zero = parsed.document.find("zero");
    check(tiny != nullptr && tiny->value.is_float() && tiny->value.floating > 0.0 &&
              tiny->value.floating < 1e-8,
          "config parser preserves very small positive numbers");
    check(zero != nullptr && zero->value.is_integer() && zero->value.integer == 0,
          "config parser preserves zero integers");

    parsed = pkchat::config::parse("huge = -999999999999999999999\n", "overflow.conf");
    check(!parsed.error.ok() && parsed.error.message.find("signed 64-bit") != std::string::npos,
          "config parser rejects overflowing negative integers");

    parsed = pkchat::config::parse("greeting = \"\"\n", "empty-string.conf");
    check(parsed.error.ok(), "config parser accepts an empty quoted string");
    const pkchat::config::Entry* greeting = parsed.document.find("greeting");
    check(greeting != nullptr && greeting->value.is_string() && greeting->value.string.empty(),
          "config parser stores empty string values");
}

}  // namespace

void run_all() {
    test_config_applies_user_settings();
    test_config_applies_model_settings();
    test_config_model_setting_thinking_budget();
    test_config_empty_and_numeric_edge_cases();
    test_config_file_read_errors();
    test_config_parses_supported_values();
    test_config_parses_multiline_strings();
    test_config_reads_common_template();
    test_config_rejects_invalid_input();
    test_config_schema_rejects_invalid_settings_transactionally();
    test_config_xdg_path_resolution();
    test_themes_config();
    test_editor_commands_config();
}

}  // namespace pkchat::test::config
