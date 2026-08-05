#include "config/test_config.hpp"
#include "support/test_support.hpp"
#include "cli/args.hpp"
#include "config/config.hpp"
#include "config/model_catalog.hpp"
#include "editor/editor.hpp"
#include "editor/editor_prompts.hpp"
#include "tui/theme_registry.hpp"
#include "provider/provider.hpp"
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace ainiux::test::config {

namespace {

using ainiux::test::check;
using ainiux::test::read_fixture;

void test_config_applies_user_settings() {
    ainiux::config::ParseResult parsed =
        ainiux::config::read_file("tests/fixtures/config-home/ainiux/config.conf");
    check(parsed.error.ok(), "user config fixture parses");

    ainiux::cli::Options options;
    ainiux::Error err = ainiux::config::apply_document(parsed.document, options);
    check(err.ok(), "user config fixture passes schema validation");
    check(options.allow_private_url_fetch, "user config enables private URL fetching");
    check(options.auto_convert_html_to_markdown,
          "HTML-to-Markdown insertion conversion defaults to enabled");
    check(options.tui_theme == "dark", "user config selects the dark theme");
    check(options.show_thinking_traces, "user config shows thinking traces by default");

    ainiux::config::ParseResult highlight_config =
        ainiux::config::parse("[tui]\nhighlight = off\n", "highlight.conf");
    err = ainiux::config::apply_document(highlight_config.document, options);
    check(err.ok() && !options.tui_highlight,
          "tui.highlight accepts off");
    highlight_config = ainiux::config::parse("[tui]\nhighlight = on\n", "highlight-bool.conf");
    err = ainiux::config::apply_document(highlight_config.document, options);
    check(err.ok() && options.tui_highlight,
          "tui.highlight accepts on");
    ainiux::config::ParseResult theme_off_config =
        ainiux::config::parse("[tui]\ntheme = off\n", "theme-off.conf");
    err = ainiux::config::apply_document(theme_off_config.document, options);
    check(err.ok() && options.no_colors && options.tui_theme == "dark",
          "tui.theme off disables colors without replacing the selected palette");
    ainiux::config::ParseResult reserved_theme = ainiux::config::parse(
        "[theme]\nname = \"off\"\nbackground = #000000\ntext = #ffffff\n",
        "reserved-theme.conf");
    err = ainiux::config::apply_themes_document(reserved_theme.document, options);
    check(!err.ok() && err.message.find("reserved") != std::string::npos,
          "custom themes cannot use the reserved off name");
    ainiux::config::ParseResult reasoning_off_config =
        ainiux::config::parse("[generation]\nreasoning = off\n", "reasoning-off.conf");
    err = ainiux::config::apply_document(reasoning_off_config.document, options);
    check(err.ok() && options.reasoning == ainiux::ReasoningSelection::named("off"),
          "generation reasoning accepts semantic off despite boolean tokenization");

    ainiux::config::ParseResult editor_config =
        ainiux::config::parse("[editor]\nundo_limit = 7\nhuge_file_size_warning = 2048\nfile_size_limit = -1\n"
                              "tab-width = 8\ntab-style = tab\nlinebreak = crlf\n"
                              "alignment-width = 72\n"
                              "continue_prefix_max_chars = 8192\n"
                              "continue_postfix_max_chars = 1024\n"
                              "continue_prose_prefix_max_chars = 16300\n"
                              "continue_prose_postfix_max_chars = 4000\n"
                              "continue_max_tokens = 4096\n",
                              "editor.conf");
    check(editor_config.error.ok(), "editor config fixture parses");
    err = ainiux::config::apply_document(editor_config.document, options);
    check(err.ok() && options.editor_undo_limit == 7 &&
              options.editor_huge_file_size_warning == 2048 &&
              options.editor_file_size_limit == -1 &&
              options.editor_tab_width == 8 &&
              options.editor_tab_style == ainiux::editor::TabStyle::Tab &&
              options.editor_linebreak == ainiux::editor::LineBreak::Crlf &&
              options.editor_text_align_width == 72 &&
              options.editor_ai_continue_prefix_max_chars == 8192 &&
              options.editor_ai_continue_postfix_max_chars == 1024 &&
              options.editor_ai_continue_prose_prefix_max_chars == 16300 &&
              options.editor_ai_continue_prose_postfix_max_chars == 4000 &&
              options.editor_ai_continue_max_tokens == 4096,
          "editor config settings apply");

    ainiux::config::ParseResult zero_continue_config = ainiux::config::parse(
        "[editor]\ncontinue_prefix_max_chars = 0\ncontinue_postfix_max_chars = 0\n"
        "continue_prose_prefix_max_chars = 0\ncontinue_prose_postfix_max_chars = 0\n",
        "editor-zero.conf");
    err = ainiux::config::apply_document(zero_continue_config.document, options);
    check(err.ok() && options.editor_ai_continue_prefix_max_chars == 0 &&
              options.editor_ai_continue_postfix_max_chars == 0 &&
              options.editor_ai_continue_prose_prefix_max_chars == 0 &&
              options.editor_ai_continue_prose_postfix_max_chars == 0,
          "zero config limits disable all editor continuation context sides");

    ainiux::config::ParseResult invalid_prose_continue_config = ainiux::config::parse(
        "[editor]\ncontinue_prose_prefix_max_chars = -1\n", "editor-prose-invalid.conf");
    err = ainiux::config::apply_document(invalid_prose_continue_config.document, options);
    check(!err.ok() && err.message.find("non-negative") != std::string::npos,
          "negative prose continuation config limit is rejected");

    ainiux::config::ParseResult overflow_prose_continue_config = ainiux::config::parse(
        "[editor]\ncontinue_prose_postfix_max_chars = 999999999999999999999999999999\n",
        "editor-prose-overflow.conf");
    check(!overflow_prose_continue_config.error.ok(),
          "overflowing prose continuation config limit is rejected while parsing");

    ainiux::config::ParseResult removed_continue_config = ainiux::config::parse(
        "[editor]\ncontinue_read_chars = 10\n", "editor-removed.conf");
    err = ainiux::config::apply_document(removed_continue_config.document, options);
    check(!err.ok() && err.message.find("continue_read_chars") != std::string::npos,
          "removed editor continue_read_chars config setting is rejected");

    ainiux::cli::Options layered;
    ainiux::config::ParseResult system_continue_config = ainiux::config::parse(
        "[editor]\ncontinue_prefix_max_chars = 10\ncontinue_postfix_max_chars = 20\n"
        "continue_prose_prefix_max_chars = 100\ncontinue_prose_postfix_max_chars = 200\n",
        "system-editor.conf");
    ainiux::config::ParseResult user_continue_config = ainiux::config::parse(
        "[editor]\ncontinue_prefix_max_chars = 30\ncontinue_prose_prefix_max_chars = 300\n",
        "user-editor.conf");
    check(ainiux::config::apply_document(system_continue_config.document, layered).ok() &&
              ainiux::config::apply_document(user_continue_config.document, layered).ok(),
          "system and user continuation settings layer successfully");
    check(layered.editor_ai_continue_prefix_max_chars == 30 &&
              layered.editor_ai_continue_postfix_max_chars == 20 &&
              layered.editor_ai_continue_prose_prefix_max_chars == 300 &&
              layered.editor_ai_continue_prose_postfix_max_chars == 200,
          "continuation settings follow system then user config precedence");

    ainiux::config::ParseResult insert_config = ainiux::config::parse(
        "[input]\nauto-convert-html-to-md = off\n", "insert.conf");
    check(insert_config.error.ok(), "insert conversion config fixture parses");
    err = ainiux::config::apply_document(insert_config.document, options);
    check(err.ok() && !options.auto_convert_html_to_markdown,
          "input auto-convert-html-to-md accepts off");

    options.editor_assist_config = ainiux::editor::default_editor_assist_config();
    ainiux::config::ParseResult assist_prompt_config = ainiux::config::parse(
        "[editor]\nassist_fact = \"Custom fact prompt\"\n", "assist-fact.conf");
    check(assist_prompt_config.error.ok(), "editor assist fact config parses");
    err = ainiux::config::apply_document(assist_prompt_config.document, options);
    const ainiux::editor::EditorAssistCommand* fact_command =
        ainiux::editor::find_assist_command(options.editor_assist_config, "/fact");
    check(err.ok() && fact_command != nullptr && fact_command->prompt == "Custom fact prompt",
          "editor assist fact config applies");

    const std::string config_home =
        std::filesystem::absolute("tests/fixtures/config-home").lexically_normal().string();
    ainiux::config::Environment environment{config_home, "/nonexistent"};
    ainiux::config::LoadResult loaded = ainiux::config::load_automatic(ainiux::cli::Options{}, environment);
    check(loaded.error.ok(), "automatic user config loading succeeds");
    check(loaded.loaded_paths.size() == 6 &&
              loaded.loaded_paths[0].find("models.conf") != std::string::npos &&
              loaded.loaded_paths[1].find("benchmarks.conf") != std::string::npos &&
              loaded.loaded_paths[2].find("themes.conf") != std::string::npos &&
              loaded.loaded_paths[3].find("editor-commands.conf") != std::string::npos &&
              loaded.loaded_paths[4].find("ainiux.conf") != std::string::npos &&
              loaded.loaded_paths[5] == config_home + "/ainiux/config.conf",
          "automatic loading applies installed defaults before user config");
    check(loaded.options.tui_themes.has("dark") && loaded.options.tui_themes.has("light") &&
              loaded.options.tui_themes.has("sepia"),
          "automatic loading includes built-in themes");
    const ainiux::editor::EditorAssistCommand* loaded_spell =
        ainiux::editor::find_assist_command(loaded.options.editor_assist_config, "/spell");
    check(loaded_spell != nullptr && loaded_spell->prompt.find("spelling") != std::string::npos,
          "automatic loading includes built-in editor assist commands");
    const std::array<const char*, 36> added_assist_commands = {
        "expand",      "shorten",   "summarize", "simplify",  "variations",
        "checklist",   "table",     "keypoints", "sentiment", "quiz",
        "questions",   "risk",      "entities",  "brainstorm", "outline",
        "hooks",       "title",     "marketing", "explain",   "transliterate",
        "readability", "speech",    "fiction",   "blog",      "article",
        "joke",        "roast",     "grumpyman", "Trump",
        "style-formal", "style-casual", "style-humor",
        "fix",         "refactor",  "tests",     "plan",
    };
    for (const char* command_name : added_assist_commands) {
        const ainiux::editor::EditorAssistCommand* editor_command =
            ainiux::editor::find_assist_command(loaded.options.editor_assist_config, command_name);
        const ainiux::editor::EditorAssistCommand* chat_command =
            ainiux::editor::find_assist_command(loaded.options.editor_assist_config,
                                                "/" + std::string(command_name));
        check(editor_command != nullptr && editor_command == chat_command &&
                  !editor_command->prompt.empty(),
              std::string("bundled AI command supports editor and chat naming: ") + command_name);
    }
    const std::array<const char*, 13> multiline_assist_commands = {
        "speech", "fiction", "blog", "article", "joke", "roast", "grumpyman", "Trump",
        "marketing", "fix", "refactor", "tests", "plan",
    };
    for (const char* command_name : multiline_assist_commands) {
        const ainiux::editor::EditorAssistCommand* command =
            ainiux::editor::find_assist_command(loaded.options.editor_assist_config, command_name);
        check(command != nullptr && command->prompt.find('\n') != std::string::npos,
              std::string("complex AI command preserves its multiline prompt: ") + command_name);
    }
    check(loaded.options.allow_private_url_fetch && loaded.options.show_thinking_traces &&
              loaded.options.tui_theme == "dark",
          "user settings override installed defaults");

    ainiux::config::LoadResult system_only =
        ainiux::config::load_automatic(ainiux::cli::Options{}, environment, false);
    check(system_only.error.ok() && !system_only.options.allow_private_url_fetch &&
              !system_only.options.show_thinking_traces && system_only.options.tui_theme == "dark",
          "disabling user config retains installed defaults");
    check(system_only.loaded_paths.size() == 5 &&
              system_only.loaded_paths[0].find("models.conf") != std::string::npos &&
              system_only.loaded_paths[1].find("benchmarks.conf") != std::string::npos &&
              system_only.loaded_paths[2].find("themes.conf") != std::string::npos &&
              system_only.loaded_paths[3].find("editor-commands.conf") != std::string::npos &&
              system_only.loaded_paths[4].find("ainiux.conf") != std::string::npos,
          "disabling user config still loads installed defaults");
    bool skipped_user_config = false;
    for (const ainiux::config::ConfigDiagnostic& diagnostic : system_only.diagnostics) {
        if (diagnostic.scope == ainiux::config::ConfigScope::User &&
            diagnostic.kind == ainiux::config::ConfigFileKind::Config &&
            diagnostic.state == ainiux::config::ConfigFileState::Skipped) {
            skipped_user_config = true;
        }
    }
    check(skipped_user_config, "disabled user config is reported as skipped");

    const char* argv[] = {"ainiux", "--no-stream", "--nocolors"};
    ainiux::cli::ParseResult cli =
        ainiux::cli::parse_args(3, const_cast<char**>(argv), loaded.options);
    check(cli.error.ok() && !cli.options.stream && cli.options.no_colors,
          "command-line arguments apply over configured defaults");

    ainiux::cli::Options offline_base;
    offline_base.provider = "none";
    const char* positional_argv[] = {"ainiux", "lmstudio", "--list-models"};
    ainiux::cli::ParseResult positional =
        ainiux::cli::parse_args(3, const_cast<char**>(positional_argv), offline_base);
    ainiux::provider::ContextResult positional_context =
        ainiux::provider::build_context(positional.options);
    check(positional.error.ok() && positional_context.error.ok() &&
              positional_context.context.profile.name == "lm_studio",
          "positional provider shortcut overrides a configured provider");
}

void test_config_file_read_errors() {
    ainiux::config::ParseResult parsed =
        ainiux::config::read_file("build/config-file-does-not-exist.conf");
    check(!parsed.error.ok() && parsed.error.code == ainiux::ErrorCode::Config,
          "missing config file reports config error");

    parsed = ainiux::config::read_file("build", ainiux::config::kMaxConfigBytes);
    check(!parsed.error.ok() && parsed.error.message.find("not a regular file") != std::string::npos,
          "config reader rejects a directory");

    parsed = ainiux::config::read_file("config/ainiux.conf", 16);
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
        "enabled = off\r\n"
        "window = 64k\r\n"
        "[nested.section]\r\n"
        "name = value\r\n";
    ainiux::config::ParseResult parsed = ainiux::config::parse(input, "unicode.conf");
    check(parsed.error.ok(), "config accepts BOM, CRLF, Unicode, and supported scalar types");

    const ainiux::config::Entry* title = parsed.document.find("title");
    check(title != nullptr && title->value.string == "line\nquote: \" slash: \\ tab:\t",
          "quoted config escapes are decoded");
    const ainiux::config::Entry* url = parsed.document.find("url");
    check(url != nullptr && url->value.string == "https://example.test/page#fragment",
          "bare config string preserves hash characters");
    const ainiux::config::Entry* greeting = parsed.document.find("greeting");
    check(greeting != nullptr && greeting->value.string == "你好 مرحبا 👋", "bare config string preserves UTF-8");
    const ainiux::config::Entry* negative = parsed.document.find("negative");
    check(negative != nullptr && negative->value.is_integer() && negative->value.integer == -12,
          "signed config integer parsed");
    const ainiux::config::Entry* ratio = parsed.document.find("ratio");
    check(ratio != nullptr && ratio->value.is_float() && ratio->value.floating == 3.25,
          "config decimal float parsed");
    const ainiux::config::Entry* scientific = parsed.document.find("scientific");
    check(scientific != nullptr && scientific->value.is_float() && scientific->value.floating == 1000.0,
          "config exponent float parsed");
    const ainiux::config::Entry* enabled = parsed.document.find("enabled");
    check(enabled != nullptr && enabled->value.is_boolean() && !enabled->value.boolean,
          "config off boolean parsed");
    const ainiux::config::Entry* window = parsed.document.find("window");
    check(window != nullptr && window->value.is_string() && window->value.string == "64k",
          "context shorthand remains a schema-level string");
    const ainiux::config::Entry* nested = parsed.document.find("nested.section", "name");
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
    ainiux::config::ParseResult parsed = ainiux::config::parse(input, "multiline.conf");
    check(parsed.error.ok(), "config accepts escaped multiline triple-quoted strings");
    const ainiux::config::Entry* title = parsed.document.find("title");
    check(title != nullptr &&
              title->value.string ==
                  "  first line\n  second \"quoted\" # [not a section]\n  escapes: \n \r \t \\\n",
          "multiline strings remove only the opening newline and normalize CRLF");
    const ainiux::config::Entry* next = parsed.document.find("next");
    check(next != nullptr && next->value.string == "value",
          "parser resumes at the assignment after a multiline string");

    parsed = ainiux::config::parse("value = \"\"\"unterminated\n", "multiline-end.conf");
    check(!parsed.error.ok() && parsed.error.message.find("multiline-end.conf:1:") != std::string::npos &&
              parsed.error.message.find("unterminated") != std::string::npos,
          "unterminated multiline strings report the opening source location");
    parsed = ainiux::config::parse("value = \"\"\"bad\\q\"\"\"\n", "multiline-escape.conf");
    check(!parsed.error.ok() && parsed.error.message.find("multiline-escape.conf:1:16") != std::string::npos &&
              parsed.error.message.find("escape") != std::string::npos,
          "multiline strings reject unsupported escapes with a source location");
    parsed = ainiux::config::parse("value = \"\"\"ok\"\"\" trailing\n", "multiline-trailing.conf");
    check(!parsed.error.ok() && parsed.error.message.find("multiline-trailing.conf:1:18") !=
                  std::string::npos &&
              parsed.error.message.find("unexpected text after multiline quoted string") !=
                  std::string::npos,
          "multiline strings reject non-whitespace trailing text");
}

void test_benchmark_prompt_configuration() {
    ainiux::config::ParseResult bundled =
        ainiux::config::read_file("config/benchmarks.conf");
    check(bundled.error.ok(), "bundled benchmark prompt config parses");
    ainiux::cli::Options options;
    ainiux::Error err =
        ainiux::config::apply_benchmarks_document(bundled.document, options);
    check(err.ok() &&
              options.benchmark_grading_prompts.system_prompt.find('\n') !=
                  std::string::npos &&
              options.benchmark_grading_prompts.case_prompt.find(
                  "{{benchmark_case_json}}") != std::string::npos,
          "benchmark prompt config preserves multiline prompt content");
    check(ainiux::config::validate_benchmark_grading_prompts(
              options.benchmark_grading_prompts)
              .ok(),
          "complete benchmark grading prompts validate");

    ainiux::config::ParseResult system = ainiux::config::parse(
        "[grading]\nsystem_prompt = \"system prompt override\"\n",
        "system-benchmarks.conf");
    ainiux::config::ParseResult user = ainiux::config::parse(
        "[grading]\ncase_prompt = \"user {{benchmark_case_json}} prompt\"\n",
        "user-benchmarks.conf");
    err = ainiux::config::apply_benchmarks_document(system.document, options);
    check(err.ok(), "partial system benchmark prompt override applies");
    err = ainiux::config::apply_benchmarks_document(user.document, options);
    check(err.ok() && options.benchmark_grading_prompts.system_prompt ==
                          "system prompt override" &&
              options.benchmark_grading_prompts.case_prompt ==
                  "user {{benchmark_case_json}} prompt",
          "benchmark prompt keys merge independently in precedence order");

    ainiux::config::ParseResult invalid = ainiux::config::parse(
        "[grading]\ncase_prompt = \"no placeholder\"\n", "missing.conf");
    err = ainiux::config::apply_benchmarks_document(invalid.document, options);
    check(!err.ok() && err.message.find("missing.conf:2:1") != std::string::npos &&
              err.message.find("exactly once") != std::string::npos,
          "benchmark case prompt requires one placeholder with source location");
    invalid = ainiux::config::parse(
        "[grading]\ncase_prompt = \"{{benchmark_case_json}} and "
        "{{benchmark_case_json}}\"\n",
        "duplicate-placeholder.conf");
    err = ainiux::config::apply_benchmarks_document(invalid.document, options);
    check(!err.ok() && err.message.find("exactly once") != std::string::npos,
          "benchmark case prompt rejects a duplicate placeholder");
    invalid = ainiux::config::parse(
        "[grading]\nsystem_prompt = \"   \"\n", "empty-prompt.conf");
    err = ainiux::config::apply_benchmarks_document(invalid.document, options);
    check(!err.ok() && err.message.find("must not be empty") != std::string::npos,
          "benchmark prompt config rejects whitespace-only prompts");
    invalid = ainiux::config::parse(
        "[grading]\ntyop = value\n", "unknown-benchmark-key.conf");
    err = ainiux::config::apply_benchmarks_document(invalid.document, options);
    check(!err.ok() &&
              err.message.find("unknown-benchmark-key.conf:2:1") !=
                  std::string::npos,
          "benchmark prompt config rejects unknown keys with source locations");
    check(!ainiux::config::validate_benchmark_grading_prompts({}).ok(),
          "missing effective grading prompts are diagnosed only when validated");

    const std::string root =
        std::filesystem::absolute("build/benchmark-prompt-config")
            .lexically_normal()
            .string();
    const std::string user_root = root + "/user";
    const std::string bundled_path = root + "/bundled.conf";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(user_root + "/ainiux");
    {
        std::ofstream file(bundled_path);
        file << "[grading]\nsystem_prompt = \"automatic system prompt\"\n";
    }
    {
        std::ofstream file(user_root + "/ainiux/benchmarks.conf");
        file << "[grading]\ncase_prompt = \"automatic user "
                "{{benchmark_case_json}} prompt\"\n";
    }
    const std::optional<std::string> previous_bundled =
        ainiux::test::test_environment("AINIUX_BENCHMARKS");
    ainiux::test::set_test_environment("AINIUX_BENCHMARKS", bundled_path);
    const ainiux::config::Environment environment{user_root, "/nonexistent"};
    ainiux::config::LoadResult automatic = ainiux::config::load_automatic(
        ainiux::cli::Options{}, environment, true);
    check(automatic.error.ok() &&
              automatic.options.benchmark_grading_prompts.system_prompt ==
                  "automatic system prompt" &&
              automatic.options.benchmark_grading_prompts.case_prompt ==
                  "automatic user {{benchmark_case_json}} prompt",
          "automatic benchmark prompts load bundled then user overrides");
    bool saw_bundled_prompt_diagnostic = false;
    bool saw_user_prompt_diagnostic = false;
    for (const ainiux::config::ConfigDiagnostic& diagnostic :
         automatic.diagnostics) {
        if (diagnostic.kind != ainiux::config::ConfigFileKind::Benchmarks) {
            continue;
        }
        saw_bundled_prompt_diagnostic =
            saw_bundled_prompt_diagnostic ||
            (diagnostic.scope == ainiux::config::ConfigScope::Bundled &&
             diagnostic.state == ainiux::config::ConfigFileState::Loaded);
        saw_user_prompt_diagnostic =
            saw_user_prompt_diagnostic ||
            (diagnostic.scope == ainiux::config::ConfigScope::User &&
             diagnostic.state == ainiux::config::ConfigFileState::Loaded);
    }
    check(saw_bundled_prompt_diagnostic && saw_user_prompt_diagnostic,
          "benchmark prompt configuration exposes bundled and user diagnostics");
    ainiux::config::LoadResult without_user = ainiux::config::load_automatic(
        ainiux::cli::Options{}, environment, false);
    check(without_user.error.ok() &&
              without_user.options.benchmark_grading_prompts.system_prompt ==
                  "automatic system prompt" &&
              without_user.options.benchmark_grading_prompts.case_prompt !=
                  "automatic user {{benchmark_case_json}} prompt",
          "--no-config skips user benchmark prompts but retains bundled prompts");
    bool saw_skipped_user_prompts = false;
    for (const ainiux::config::ConfigDiagnostic& diagnostic :
         without_user.diagnostics) {
        saw_skipped_user_prompts =
            saw_skipped_user_prompts ||
            (diagnostic.kind == ainiux::config::ConfigFileKind::Benchmarks &&
             diagnostic.scope == ainiux::config::ConfigScope::User &&
             diagnostic.state == ainiux::config::ConfigFileState::Skipped);
    }
    check(saw_skipped_user_prompts,
          "--no-config reports the user benchmark prompt file as skipped");

    const std::string override_path = root + "/override.conf";
    {
        std::ofstream file(override_path);
        file << "[grading]\nsystem_prompt = \"environment system\"\n"
                "case_prompt = \"environment {{benchmark_case_json}} case\"\n";
    }
    const std::optional<std::string> previous_override =
        ainiux::test::test_environment("AINIUX_BENCHMARKS");
    ainiux::test::set_test_environment("AINIUX_BENCHMARKS", override_path);
    ainiux::config::LoadResult overridden = ainiux::config::load_automatic(
        ainiux::cli::Options{}, {"", "/nonexistent"}, false);
    if (previous_override.has_value())
        ainiux::test::set_test_environment("AINIUX_BENCHMARKS", *previous_override);
    else
        ainiux::test::unset_test_environment("AINIUX_BENCHMARKS");
    if (previous_bundled.has_value())
        ainiux::test::set_test_environment("AINIUX_BENCHMARKS", *previous_bundled);
    else
        ainiux::test::unset_test_environment("AINIUX_BENCHMARKS");
    check(overridden.error.ok() &&
              overridden.options.benchmark_grading_prompts.system_prompt ==
                  "environment system" &&
              overridden.options.benchmark_grading_prompts.case_prompt ==
                  "environment {{benchmark_case_json}} case",
          "AINIUX_BENCHMARKS overrides the bundled benchmark prompt path");
}

void test_config_applies_model_catalog() {
    ainiux::config::ParseResult parsed = ainiux::config::parse(
        "[model]\n"
        "id = qwen-coder\n"
        "provider = qwen\n"
        "api = chat\n"
        "model = \"^qwen3[.]6(?:[-.].*)?$\"\n"
        "value = low|1024\n"
        "context_window = 1M\n"
        "priority = 100\n"
        "reasoning_protocol = qwen_chat\n"
        "reasoning_default = medium\n"
        "temperature = supported\n"
        "enabled = on\n"
        "[preset]\n"
        "model_id = qwen-coder\n"
        "purpose = coding\n"
        "top_k = 20\n"
        "top_p = 0.95\n"
        "enabled = on\n"
        "reasoning = high\n",
        "models.conf");
    check(parsed.error.ok(), "models catalog parses");

    ainiux::cli::Options options;
    ainiux::Error err = ainiux::config::apply_models_document(parsed.document, options);
    check(err.ok() && options.model_catalog.models.size() == 1 &&
              options.model_catalog.presets.size() == 1,
          "model and preset blocks apply");
    const ainiux::ModelCapability& capability = options.model_catalog.models.front();
    check(capability.id == "qwen-coder" && capability.provider == "qwen" &&
              capability.api == "chat" && capability.priority == 100 &&
              capability.reasoning_protocol == ainiux::ReasoningProtocol::QwenChat &&
              capability.reasoning_default == ainiux::ReasoningSelection::named("medium") &&
              capability.reasoning_options.size() == 2 &&
              capability.reasoning_options[0] == ainiux::ReasoningSelection::named("low") &&
              capability.reasoning_options[1] == ainiux::ReasoningSelection::token_budget(1024) &&
              capability.context_window_tokens == 1000000 &&
              capability.temperature == ainiux::TemperatureSupport::Supported,
          "model capability, context fallback, and compact reasoning values are stored");
    const ainiux::ModelSetting& preset = options.model_catalog.presets.front();
    check(!preset.temperature.has_value() && preset.top_k == 20 && preset.top_p == 0.95 &&
              preset.reasoning == ainiux::ReasoningSelection::named("high"),
          "preset generation fields are optional");

    const ainiux::ModelCapability* match = ainiux::config::resolve_model_capability(
        options.model_catalog, "qwen", "chat", "models/qwen3.6-coder");
    check(match != nullptr && match->id == "qwen-coder",
          "model regex matches the final component of a prefixed model id");
    check(!ainiux::config::model_regex_matches("^models/.*$", "models/qwen3.6-coder"),
          "model regex never matches a provider prefix");

    const ainiux::config::ReasoningSelectorData selector =
        ainiux::config::reasoning_selector_data(
            options.model_catalog, "qwen", "chat", "models/qwen3.6-coder");
    check(selector.values.size() == 3 &&
              selector.values.front().is_auto() &&
              selector.labels.front().find("provider default: medium") != std::string::npos &&
              selector.labels[1] == "low" && selector.labels[2] == "1024",
          "reasoning selector derives labels directly from compact values");
    const ainiux::config::ReasoningSelectorData unknown =
        ainiux::config::reasoning_selector_data(
            options.model_catalog, "qwen", "chat", "unknown-model");
    check(unknown.values.empty() && unknown.guidance.find("/reasoning VALUE") != std::string::npos &&
              unknown.guidance.find("models.conf") != std::string::npos,
          "unknown models receive direct-value and catalog guidance");

    ainiux::ReasoningSelection next;
    check(ainiux::config::next_reasoning_selection(
              options.model_catalog, "qwen", "chat", "qwen3.6-coder",
              ainiux::ReasoningSelection::automatic(), next) &&
              next == ainiux::ReasoningSelection::named("none"),
          "Qwen thinking toggle treats Auto as the default enabled state");
    check(ainiux::config::next_reasoning_selection(
              options.model_catalog, "qwen", "chat", "qwen3.6-coder", next, next) &&
              next == ainiux::ReasoningSelection::named("enabled"),
          "Qwen thinking toggle alternates from disabled to enabled");

    ainiux::ModelCatalog effort_catalog;
    ainiux::ModelCapability effort;
    effort.id = "effort";
    effort.model_regex = "^effort-model$";
    effort.reasoning_protocol = ainiux::ReasoningProtocol::OpenAiEffort;
    effort.reasoning_options = {
        ainiux::ReasoningSelection::named("min"),
        ainiux::ReasoningSelection::named("medium"),
        ainiux::ReasoningSelection::named("high"),
        ainiux::ReasoningSelection::named("xhigh"),
    };
    effort_catalog.models.push_back(std::move(effort));
    next = ainiux::ReasoningSelection::named("medium");
    check(ainiux::config::next_reasoning_selection(
              effort_catalog, "openai", "chat", "effort-model", next, next) &&
              next == ainiux::ReasoningSelection::named("high"),
          "reasoning shortcut advances through catalog order");
    check(ainiux::config::next_reasoning_selection(
              effort_catalog, "openai", "chat", "effort-model",
              ainiux::ReasoningSelection::named("xhigh"), next) &&
              next.is_auto(),
          "reasoning shortcut advances from the highest effort to Auto");
    check(ainiux::config::next_reasoning_selection(
              effort_catalog, "openai", "chat", "effort-model", next, next) &&
              next == ainiux::ReasoningSelection::named("min"),
          "reasoning shortcut cycles from Auto to the lowest effort");
    check(!ainiux::config::next_reasoning_selection(
              effort_catalog, "openai", "chat", "", next, next),
          "reasoning shortcut silently ignores a missing model");

    const auto check_off_choice = [&](const ainiux::ReasoningSelection& disabled,
                                      const std::string& label) {
        ainiux::ModelCatalog catalog;
        ainiux::ModelCapability model;
        model.id = label;
        model.model_regex = "^" + label + "$";
        model.reasoning_options = {disabled, ainiux::ReasoningSelection::named("high")};
        catalog.models.push_back(std::move(model));
        ainiux::ReasoningSelection selection = ainiux::ReasoningSelection::named("off");
        const ainiux::Error resolve = ainiux::config::resolve_reasoning_off(
            catalog, "any", "chat", label, selection);
        check(resolve.ok() && selection == disabled,
              "reasoning off resolves to catalog disable choice " + label);
    };
    check_off_choice(ainiux::ReasoningSelection::named("none"), "gpt-none");
    check_off_choice(ainiux::ReasoningSelection::token_budget(0), "anthropic-zero");
    check_off_choice(ainiux::ReasoningSelection::named("disabled"), "toggle-disabled");
    check_off_choice(ainiux::ReasoningSelection::named("no_think"), "hy3-no-think");

    ainiux::ReasoningSelection unavailable = ainiux::ReasoningSelection::named("off");
    const ainiux::Error unavailable_error = ainiux::config::resolve_reasoning_off(
        effort_catalog, "openai", "chat", "effort-model", unavailable);
    check(!unavailable_error.ok() && unavailable_error.message.find("min|medium|high|xhigh") != std::string::npos,
          "known models without a disable choice reject reasoning off and list choices");
    ainiux::ReasoningSelection unmatched = ainiux::ReasoningSelection::named("off");
    check(ainiux::config::resolve_reasoning_off(
              effort_catalog, "openai", "chat", "unmatched", unmatched).ok() &&
              unmatched == ainiux::ReasoningSelection::named("off"),
          "unmatched models retain semantic reasoning off");
}

void test_model_catalog_layering_and_validation() {
    ainiux::cli::Options options;
    const auto apply = [&](const std::string& text, const std::string& path) {
        const ainiux::config::ParseResult parsed = ainiux::config::parse(text, path);
        check(parsed.error.ok(), path + " parses");
        return parsed.error.ok()
                   ? ainiux::config::apply_models_document(parsed.document, options)
                   : parsed.error;
    };

    ainiux::Error err = apply(
        "[model]\n"
        "id = generic\nprovider = any\napi = any\nmodel = \"^same$\"\nvalue = low\n"
        "priority = 10\nreasoning_protocol = generic_thinking\n"
        "[model]\n"
        "id = specific\nprovider = openai\napi = chat\nmodel = \"^same$\"\nvalue = low\n"
        "priority = 10\nreasoning_protocol = openai_effort\n"
        "[model]\n"
        "id = merged\nprovider = openai\napi = any\nmodel = \"^merged$\"\n"
        "value = low|medium\nreasoning_protocol = openai_effort\n"
        "[preset]\nmodel_id = merged\npurpose = coding\ntemperature = 0.6\n",
        "bundled-models.conf");
    check(err.ok(), "bundled model layer applies");
    const ainiux::ModelCapability* match =
        ainiux::config::resolve_model_capability(options.model_catalog, "openai", "chat", "same");
    check(match != nullptr && match->id == "specific",
          "provider and API specificity resolves equal-priority overlaps");

    err = apply(
        "[model]\n"
        "id = later\nprovider = openai\napi = chat\nmodel = \"^same$\"\nvalue = low\n"
        "priority = 10\nreasoning_protocol = xai_effort\n"
        "[model]\n"
        "id = merged\nprovider = openai\napi = responses\nmodel = \"^merged-v2$\"\n"
        "value = low|high\npriority = 40\nreasoning_protocol = openai_effort\n"
        "temperature = unsupported\n"
        "[preset]\nmodel_id = merged\npurpose = coding\ntop_k = 7\n",
        "user-models.conf");
    check(err.ok(), "later model layer applies");
    match = ainiux::config::resolve_model_capability(
        options.model_catalog, "openai", "chat", "same");
    check(match != nullptr && match->id == "later",
          "later config layer resolves otherwise equal model definitions");
    check(options.model_catalog.models.size() == 4,
          "model blocks merge by id instead of accumulating duplicate ids");
    const ainiux::ModelCapability* merged = nullptr;
    for (const ainiux::ModelCapability& capability : options.model_catalog.models) {
        if (capability.id == "merged") merged = &capability;
    }
    check(merged != nullptr && merged->reasoning_options.size() == 2 &&
              merged->reasoning_options[1] == ainiux::ReasoningSelection::named("high"),
          "later model layers replace their compact reasoning values by model id");
    check(options.model_catalog.presets.size() == 1 &&
              !options.model_catalog.presets.front().temperature.has_value() &&
              options.model_catalog.presets.front().top_k == 7,
          "presets merge by model id and purpose");

    err = apply(
        "[preset]\nmodel_id = merged\npurpose = coding\nenabled = off\n"
        "[model]\nid = merged\nenabled = off\n",
        "remove-models.conf");
    check(err.ok() && options.model_catalog.models.size() == 3 &&
              options.model_catalog.presets.empty(),
          "enabled=false removes layered model catalog records");

    ainiux::config::ParseResult invalid = ainiux::config::parse(
        "[model]\nid = broken\nmodel = \"([\"\nvalue = low\nreasoning_protocol = openai_effort\n",
        "invalid-regex.conf");
    err = ainiux::config::apply_models_document(invalid.document, options);
    check(!err.ok() && err.code == ainiux::ErrorCode::Config &&
              err.message.find("invalid model regex") != std::string::npos,
          "invalid model regex is rejected during config loading");

    invalid = ainiux::config::parse(
        "[model]\nid = broken\nmodel = \".*\"\nvalue = low\nreasoning_protocol = arbitrary_json_path\n",
        "invalid-protocol.conf");
    err = ainiux::config::apply_models_document(invalid.document, options);
    check(!err.ok() && err.code == ainiux::ErrorCode::Config &&
              err.message.find("unknown reasoning protocol") != std::string::npos,
          "unregistered reasoning protocols are rejected during config loading");
}

void test_config_reads_models_template() {
    const ainiux::config::ParseResult parsed =
        ainiux::config::read_file("config/models.conf");
    check(parsed.error.ok(), "bundled models.conf parses");
    ainiux::cli::Options options;
    const ainiux::Error err =
        ainiux::config::apply_models_document(parsed.document, options);
    check(err.ok() && options.model_catalog.models.size() >= 20 &&
              !options.model_catalog.presets.empty(),
          "bundled models.conf passes schema validation and contains capabilities");
    bool muse = false;
    bool mimo_ladder = false;
    for (const ainiux::ModelCapability& capability : options.model_catalog.models) {
        muse = muse || capability.id.find("muse") != std::string::npos;
        if (capability.id.find("mimo") != std::string::npos) {
            for (const ainiux::ReasoningSelection& option : capability.reasoning_options) {
                mimo_ladder = mimo_ladder ||
                              (option.kind == ainiux::ReasoningSelectionKind::Named &&
                               option.value != "none" && option.value != "disabled" &&
                               option.value != "off" && option.value != "enabled");
            }
        }
    }
    check(!muse && !mimo_ladder,
          "bundled catalog omits Muse and an unverified MiMo effort ladder");

    const std::vector<std::pair<std::string, std::string>> routed_models = {
        {"groq", "another-router/google/GEMINI-3.1-FLASH-LITE-IMAGE"},
        {"together", "gateway/openai/GPT-5.6-LUNA"},
        {"custom_openai_chat", "x-ai/GROK-4.5"},
        {"deepinfra", "vendor/qwen/QWEN3.6-27B"},
        {"openrouter", "anthropic/CLAUDE-OPUS-4.8"},
    };
    for (const auto& routed : routed_models) {
        const ainiux::config::ReasoningSelectorData selector =
            ainiux::config::reasoning_selector_data(
                options.model_catalog, routed.first, "chat", routed.second);
        check(selector.guidance.empty() && selector.values.size() > 1,
              "provider-neutral family regex matches " + routed.second);
    }

    const ainiux::ModelCapability* llama = ainiux::config::resolve_model_capability(
        options.model_catalog, "groq", "chat", "meta/Meta-Llama-3.3-70B-Instruct");
    check(llama != nullptr && llama->id == "preset-llama-3",
          "case-insensitive Llama 3.x family rule covers routed sizes and variants");
    const ainiux::ModelCapability* gpt_oss_20b = ainiux::config::resolve_model_capability(
        options.model_catalog, "together", "chat", "openai/GPT-OSS-20B");
    const ainiux::ModelCapability* gpt_oss_120b = ainiux::config::resolve_model_capability(
        options.model_catalog, "deepinfra", "chat", "vendor/openai/gpt-oss-120b");
    check(gpt_oss_20b != nullptr && gpt_oss_20b->id == "openai-gpt-oss" &&
              gpt_oss_120b != nullptr && gpt_oss_120b->id == "openai-gpt-oss",
          "GPT OSS family rule covers both 20B and 120B through arbitrary prefixes");
    const ainiux::ModelCapability* deepseek_v4 =
        ainiux::config::resolve_model_capability(
            options.model_catalog, "deepseek", "chat", "deepseek-v4-pro");
    check(deepseek_v4 != nullptr &&
              deepseek_v4->context_window_tokens == 1000000,
          "DeepSeek V4 catalog record supplies its documented 1M context fallback");
    const ainiux::ModelCapability* deepseek_v4_dated =
        ainiux::config::resolve_model_capability(
            options.model_catalog, "openrouter", "chat",
            "deepseek/deepseek-v4-flash-0731");
    check(deepseek_v4_dated != nullptr && deepseek_v4_dated->id == "deepseek-v4" &&
              !deepseek_v4_dated->reasoning_options.empty(),
          "DeepSeek V4 family rule covers dated OpenRouter-style flash revisions");
    ainiux::ReasoningSelection deepseek_next;
    check(ainiux::config::next_reasoning_selection(
              options.model_catalog, "openrouter", "chat",
              "deepseek/deepseek-v4-flash-0731",
              ainiux::ReasoningSelection::automatic(),
              deepseek_next) &&
              deepseek_next == ainiux::ReasoningSelection::named("none"),
          "Ctrl+T / next_reasoning_selection works for dated DeepSeek V4 flash ids");

    const std::string valid_warning = ainiux::config::reasoning_catalog_warning(
        options.model_catalog,
        "groq",
        "chat",
        "vendor/DEEPSEEK-V4-FLASH",
        ainiux::ReasoningSelection::named("max"));
    const std::string typo_warning = ainiux::config::reasoning_catalog_warning(
        options.model_catalog,
        "groq",
        "chat",
        "vendor/DEEPSEEK-V4-FLASH",
        ainiux::ReasoningSelection::named("maxx"));
    check(valid_warning.empty() && typo_warning.find("maxx") != std::string::npos &&
              typo_warning.find("none|low|high|max") != std::string::npos &&
              typo_warning.find("provider may reject") != std::string::npos,
          "catalog warning distinguishes configured reasoning values from forward-compatible overrides");
    check(ainiux::config::reasoning_catalog_warning(
              options.model_catalog,
              "groq",
              "chat",
              "unknown-model",
              ainiux::ReasoningSelection::named("ultra")).empty() &&
              ainiux::config::reasoning_catalog_warning(
                  options.model_catalog,
                  "groq",
                  "chat",
                  "llama-3.2-3b",
                  ainiux::ReasoningSelection::named("ultra")).empty(),
          "catalog warning stays silent without a matched list of allowed values");

    const std::vector<std::string> near_misses = {
        "google/gemini-2.5-flash",
        "openai/gpt-4.1",
        "x-ai/grok-3",
        "qwen/qwen2.5-27b",
        "anthropic/claude-haiku-3.5",
        "meta/llama-4.0-70b",
        "openai/gpt-oss-200b",
    };
    for (const std::string& model : near_misses) {
        check(ainiux::config::resolve_model_capability(
                  options.model_catalog, "openrouter", "chat", model) == nullptr,
              "family regex rejects near miss " + model);
    }
}

void test_config_reads_common_template() {
    ainiux::config::ParseResult parsed = ainiux::config::read_file("config/ainiux.conf");
    check(parsed.error.ok(), "common config file parses");
    check(parsed.document.entries.size() == 72, "common config has every expected setting");
    ainiux::cli::Options highlight_options;
    ainiux::Error apply_error = ainiux::config::apply_document(parsed.document, highlight_options);
    check(apply_error.ok() && highlight_options.tui_highlight,
          "common config enables syntax highlighting by default");

    const ainiux::config::Entry* provider = parsed.document.find("provider");
    check(provider != nullptr && provider->value.is_string() && provider->value.string == "openai",
          "common config provider is stored as a string");
    check(provider != nullptr && provider->source.path == "config/ainiux.conf" && provider->source.line == 12,
          "common config entry retains source path and line");

    const ainiux::config::Entry* stream = parsed.document.find("generation", "stream");
    check(stream != nullptr && stream->value.is_boolean() && stream->value.boolean,
          "common config streaming value is a boolean");
    const ainiux::config::Entry* timeout = parsed.document.find("network.connect_timeout_seconds");
    check(timeout != nullptr && timeout->value.is_integer() && timeout->value.integer == 10,
          "common config timeout is an integer");
    const ainiux::config::Entry* model = parsed.document.find("model");
    check(model != nullptr && model->value.is_string() && model->value.string.empty(),
          "common config supports an empty bare string");

    ainiux::cli::Options options;
    ainiux::Error err = ainiux::config::apply_document(parsed.document, options);
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
              options.editor_text_align_width == ainiux::editor::kDefaultTextAlignWidth &&
              options.media_max_size_to_store_to_db == 65536 &&
              options.media_expiration_days == 7 && options.media_auto_expiration_days == 30 &&
              options.editor_ai_continue_prefix_max_chars ==
                  ainiux::editor::kDefaultAiContinuePrefixMaxChars &&
              options.editor_ai_continue_postfix_max_chars ==
                  ainiux::editor::kDefaultAiContinuePostfixMaxChars &&
              options.editor_ai_continue_prose_prefix_max_chars ==
                  ainiux::editor::kDefaultAiContinueProsePrefixMaxChars &&
              options.editor_ai_continue_prose_postfix_max_chars ==
                  ainiux::editor::kDefaultAiContinueProsePostfixMaxChars &&
              options.editor_ai_continue_max_tokens == ainiux::editor::kDefaultAiContinueMaxTokens &&
              options.max_parallel_agents == 2 &&
              options.security_review_batch_size == 200U * 1024U &&
              options.security_review_log_enabled &&
              options.security_review_log_keep_runs == 3 &&
              options.agent_history_backup_enabled &&
              options.agent_history_backup_max_bytes == 1024U * 1024U &&
              options.agent_history_backup_ttl_days == 7 &&
              options.agent_auto_compact &&
              options.agent_compact_strategy == CompactionStrategy::Smart &&
              !options.agent_show_command_output &&
              options.agent_max_response_bytes == 32L * 1024L * 1024L,
          "common config maps to the built-in runtime defaults");
    check(options.model_catalog.models.empty(),
          "common config leaves model capabilities to models.conf");
}

void test_config_rejects_invalid_input() {
    ainiux::config::ParseResult parsed = ainiux::config::parse("key = one\nkey = two\n", "duplicate.conf");
    check(!parsed.error.ok() && parsed.error.code == ainiux::ErrorCode::Config,
          "duplicate config key is rejected");
    check(parsed.error.message.find("duplicate.conf:2:1") != std::string::npos &&
              parsed.error.message.find("first defined at duplicate.conf:1:1") != std::string::npos,
          "duplicate config error reports both source locations");
    check(parsed.document.entries.empty(), "failed config parse returns no partially applied document");

    parsed = ainiux::config::parse("value = \"bad\\q\"\n", "escape.conf");
    check(!parsed.error.ok() && parsed.error.message.find("escape.conf:1:") != std::string::npos,
          "unsupported config string escape is rejected with location");

    parsed = ainiux::config::parse(
        "[agent]\ncompact_strategy = turbo\n", "compact-strategy.conf");
    ainiux::cli::Options compact_options;
    Error compact_error =
        ainiux::config::apply_document(parsed.document, compact_options);
    check(!compact_error.ok() &&
              compact_error.message.find("fast, smart, or summary") !=
                  std::string::npos,
          "agent compact_strategy rejects unknown values");

    parsed = ainiux::config::parse("number = 999999999999999999999999999\n", "overflow.conf");
    check(!parsed.error.ok() && parsed.error.message.find("signed 64-bit") != std::string::npos,
          "overflowing config integer is rejected");

    parsed = ainiux::config::parse("[bad section]\nkey = value\n", "section.conf");
    check(!parsed.error.ok() && parsed.error.message.find("invalid section name") != std::string::npos,
          "invalid config section name is rejected");

    const std::string invalid_utf8 = std::string("name = ") + static_cast<char>(0xC3) + "(\n";
    parsed = ainiux::config::parse(invalid_utf8, "utf8.conf");
    check(!parsed.error.ok() && parsed.error.message.find("utf8.conf:1:8") != std::string::npos &&
              parsed.error.message.find("invalid UTF-8") != std::string::npos,
          "invalid config UTF-8 is rejected at its byte column");
}

void test_config_schema_rejects_invalid_settings_transactionally() {
    ainiux::cli::Options options;
    options.tui_theme = "light";
    ainiux::config::ParseResult unknown =
        ainiux::config::parse("[tui]\ntheme = dark\ntypo = true\n", "unknown.conf");
    ainiux::Error err = ainiux::config::apply_document(unknown.document, options);
    check(!err.ok() && err.code == ainiux::ErrorCode::Config &&
              err.message.find("tui.typo") != std::string::npos,
          "config schema rejects unknown keys with the qualified name");
    check(options.tui_theme == "light", "invalid config does not partially change options");

    ainiux::config::ParseResult bad_media_days =
        ainiux::config::parse("[media]\nexpiration_days = -1\n", "media-days.conf");
    err = ainiux::config::apply_document(bad_media_days.document, options);
    check(!err.ok() && err.message.find("non-negative integer") != std::string::npos,
          "config schema rejects negative managed-media expiration days");

    ainiux::config::ParseResult bad_inline_limit = ainiux::config::parse(
        "[media]\nmax_size_to_store_to_db = -1\n", "media-inline-limit.conf");
    err = ainiux::config::apply_document(bad_inline_limit.document, options);
    check(!err.ok() && err.message.find("non-negative integer") != std::string::npos,
          "config schema rejects a negative inline Markdown threshold");

    ainiux::config::ParseResult wrong_type =
        ainiux::config::parse("[url_fetch]\nallow_private_addresses = yes\n", "type.conf");
    err = ainiux::config::apply_document(wrong_type.document, options);
    check(!err.ok() && err.message.find("expected on or off") != std::string::npos,
          "config schema reports the canonical boolean values");

    for (const std::string legacy : {"true", "false", "yes", "no", "1", "0", "enabled", "disabled"}) {
        ainiux::config::ParseResult legacy_bool = ainiux::config::parse(
            "[url_fetch]\nallow_private_addresses = " + legacy + "\n",
            "legacy-boolean.conf");
        err = ainiux::config::apply_document(legacy_bool.document, options);
        check(!err.ok() && err.message.find("expected on or off") != std::string::npos,
              "config boolean schema rejects legacy value " + legacy);
    }

    ainiux::config::ParseResult bad_version =
        ainiux::config::parse("config_version = 2\n", "version.conf");
    err = ainiux::config::apply_document(bad_version.document, options);
    check(!err.ok() && err.message.find("supported version is 1") != std::string::npos,
          "config schema rejects unsupported versions");

    ainiux::config::ParseResult bad_editor_limit =
        ainiux::config::parse("[editor]\nfile_size_limit = -2\n", "editor-limit.conf");
    err = ainiux::config::apply_document(bad_editor_limit.document, options);
    check(!err.ok() && err.message.find("expected -1 or a non-negative byte limit") != std::string::npos,
          "config schema rejects editor file limits below -1");

    ainiux::config::ParseResult bad_undo =
        ainiux::config::parse("[editor]\nundo_limit = -1\n", "editor-undo.conf");
    err = ainiux::config::apply_document(bad_undo.document, options);
    check(!err.ok() && err.message.find("non-negative integer") != std::string::npos,
          "config schema rejects negative editor undo limits");

    ainiux::config::ParseResult bad_tab_width =
        ainiux::config::parse("[editor]\ntab-width = 0\n", "editor-tab-width.conf");
    err = ainiux::config::apply_document(bad_tab_width.document, options);
    check(!err.ok() && err.message.find("1 through 32") != std::string::npos,
          "config schema rejects editor tab widths outside 1 through 32");

    ainiux::config::ParseResult bad_tab_style =
        ainiux::config::parse("[editor]\ntab-style = tabs\n", "editor-tab-style.conf");
    err = ainiux::config::apply_document(bad_tab_style.document, options);
    check(!err.ok() && err.message.find("spaces or tab") != std::string::npos,
          "config schema rejects unknown editor tab styles");

    ainiux::config::ParseResult bad_linebreak =
        ainiux::config::parse("[editor]\nlinebreak = native\n", "editor-linebreak.conf");
    err = ainiux::config::apply_document(bad_linebreak.document, options);
    check(!err.ok() && err.message.find("lf, cr, or crlf") != std::string::npos,
          "config schema rejects unknown editor linebreak modes");
}

void test_config_user_path_resolution() {
    ainiux::config::Environment environment{"relative", "/home/tester"};
    check(ainiux::config::user_config_path(environment) == "/home/tester/.config/ainiux/config.conf",
          "relative XDG_CONFIG_HOME falls back to HOME");
    check(ainiux::config::user_editor_commands_path(environment) ==
              "/home/tester/.config/ainiux/editor-commands.conf",
          "relative XDG_CONFIG_HOME falls back to HOME for editor commands");
    check(ainiux::config::user_themes_path(environment) == "/home/tester/.config/ainiux/themes.conf",
          "relative XDG_CONFIG_HOME falls back to HOME for themes");
    check(ainiux::config::bundled_config_paths().front() == "config/ainiux.conf",
          "installed defaults begin with the development bundled config path");
}

void test_themes_config() {
    ainiux::config::ParseResult parsed = ainiux::config::read_file("config/themes.conf");
    check(parsed.error.ok(), "themes.conf parses");
    ainiux::cli::Options options;
    ainiux::Error err = ainiux::config::apply_themes_document(parsed.document, options);
    check(err.ok(), "themes.conf applies");
    check(options.tui_themes.has("dark") && options.tui_themes.has("light") &&
              options.tui_themes.has("sepia"),
          "themes.conf defines built-in dark, light, and sepia themes");

    ainiux::config::ParseResult invalid = ainiux::config::parse("provider = openai\n", "bad-themes.conf");
    check(invalid.error.ok(), "invalid themes fixture parses");
    err = ainiux::config::apply_themes_document(invalid.document, options);
    check(!err.ok() && err.message.find("unknown themes setting") != std::string::npos,
          "themes.conf rejects unrelated settings");

    const std::string config_home =
        std::filesystem::absolute("build/config-theme-user").lexically_normal().string();
    std::filesystem::create_directories(config_home + "/ainiux");
    {
        std::ofstream user_themes(config_home + "/ainiux/themes.conf", std::ios::trunc);
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
    ainiux::config::Environment environment{config_home, "/nonexistent"};
    ainiux::config::LoadResult loaded = ainiux::config::load_automatic(ainiux::cli::Options{}, environment);
    check(loaded.error.ok(), "automatic loading with themes overrides succeeds");
    check(loaded.options.tui_themes.has("sepia") && loaded.options.tui_themes.has("dark"),
          "installed and user themes merge into the registry");
    const ainiux::tui::ThemePalette* dark = loaded.options.tui_themes.find("dark");
    check(dark != nullptr && dark->background.r == 0x10 && dark->background.g == 0x10 &&
              dark->background.b == 0x10,
          "user themes.conf overrides the built-in dark theme");

    ainiux::config::ParseResult config_override =
        ainiux::config::parse("[tui]\ntheme = sepia\n", "config-theme.conf");
    check(config_override.error.ok(), "config theme override parses");
    err = ainiux::config::apply_document(config_override.document, loaded.options);
    check(err.ok() && loaded.options.tui_theme == "sepia",
          "config.conf selects a custom theme by name");
}

void test_editor_commands_config() {
    ainiux::config::ParseResult parsed = ainiux::config::read_file("config/editor-commands.conf");
    check(parsed.error.ok(), "editor-commands.conf parses");
    ainiux::cli::Options options;
    ainiux::Error err = ainiux::config::apply_editor_commands_document(parsed.document, options);
    check(err.ok(), "editor-commands.conf applies");
    const ainiux::editor::EditorAssistCommand* spell =
        ainiux::editor::find_assist_command(options.editor_assist_config, "/spell");
    const ainiux::editor::EditorAssistCommand* continue_command =
        ainiux::editor::find_assist_command(options.editor_assist_config, "/continue");
    check(spell != nullptr && continue_command != nullptr,
          "editor-commands.conf defines built-in assist commands");
    const ainiux::editor::EditorAssistCommand* style_formal =
        ainiux::editor::find_assist_command(options.editor_assist_config, "/style-formal");
    const ainiux::editor::EditorAssistCommand* style_casual =
        ainiux::editor::find_assist_command(options.editor_assist_config, "/style-casual");
    const ainiux::editor::EditorAssistCommand* style_humor =
        ainiux::editor::find_assist_command(options.editor_assist_config, "/style-humor");
    const ainiux::editor::EditorAssistCommand* marketing =
        ainiux::editor::find_assist_command(options.editor_assist_config, "/marketing");
    check(style_formal != nullptr && style_formal->prompt.find("formal") != std::string::npos,
          "editor-commands.conf defines style-formal");
    check(style_casual != nullptr && style_casual->prompt.find("casual") != std::string::npos,
          "editor-commands.conf defines style-casual");
    check(style_humor != nullptr && style_humor->prompt.find("humoristic") != std::string::npos,
          "editor-commands.conf defines style-humor");
    check(marketing != nullptr && marketing->prompt.find("marketing materials") != std::string::npos,
          "editor-commands.conf defines marketing");
    check(options.editor_assist_config.behavior_rules.find("one-shot") != std::string::npos,
          "editor-commands.conf defines assist behavior rules");

    ainiux::config::ParseResult invalid = ainiux::config::parse("provider = openai\n", "bad-editor.conf");
    check(invalid.error.ok(), "invalid editor-commands fixture parses");
    err = ainiux::config::apply_editor_commands_document(invalid.document, options);
    check(!err.ok() && err.message.find("unknown editor-commands setting") != std::string::npos,
          "editor-commands.conf rejects unrelated settings");

    const std::string config_home =
        std::filesystem::absolute("build/config-editor-user").lexically_normal().string();
    std::filesystem::create_directories(config_home + "/ainiux");
    {
        std::ofstream user_commands(config_home + "/ainiux/editor-commands.conf", std::ios::trunc);
        check(user_commands.is_open(), "user editor-commands test file opens");
        user_commands << "[command]\nstring = /spell\nmodes = selection, all\n"
                         "prompt = \"User spell override\"\n";
        user_commands.close();
        check(user_commands.good(), "user editor-commands test file is written");
    }
    ainiux::config::Environment environment{config_home, "/nonexistent"};
    ainiux::config::LoadResult loaded = ainiux::config::load_automatic(ainiux::cli::Options{}, environment);
    check(loaded.error.ok(), "automatic loading with editor-commands overrides succeeds");
    const ainiux::editor::EditorAssistCommand* overridden_spell =
        ainiux::editor::find_assist_command(loaded.options.editor_assist_config, "/spell");
    check(overridden_spell != nullptr && overridden_spell->prompt == "User spell override",
          "user editor-commands.conf overrides installed editor commands");
    const ainiux::editor::EditorAssistCommand* kept_style =
        ainiux::editor::find_assist_command(loaded.options.editor_assist_config, "/style-formal");
    const ainiux::editor::EditorAssistCommand* kept_marketing =
        ainiux::editor::find_assist_command(loaded.options.editor_assist_config, "/marketing");
    check(kept_style != nullptr && kept_marketing != nullptr,
          "partial system/user editor-commands overrides keep bundled built-in commands");

    ainiux::config::ParseResult config_override = ainiux::config::parse(
        "[editor]\nassist_spell = \"Config spell override\"\n", "config-spell.conf");
    check(config_override.error.ok(), "config assist_spell override parses");
    err = ainiux::config::apply_document(config_override.document, loaded.options);
    overridden_spell = ainiux::editor::find_assist_command(loaded.options.editor_assist_config, "/spell");
    check(err.ok() && overridden_spell != nullptr &&
              overridden_spell->prompt == "Config spell override",
          "config.conf assist_spell overrides editor-commands.conf");

    ainiux::config::ParseResult defaults = ainiux::config::parse(
        "[command]\nstring = demo\nprompt = \"Demo prompt\"\n", "command-defaults.conf");
    options = ainiux::cli::Options{};
    err = ainiux::config::apply_document(defaults.document, options);
    const ainiux::editor::EditorAssistCommand* demo =
        ainiux::editor::find_assist_command(options.editor_assist_config, "/DEMO");
    check(err.ok() && demo != nullptr && demo->modes.size() == 4 &&
              demo->prompt == "Demo prompt",
          "missing command modes receive the standard four editor modes");
    const ainiux::config::ParseResult legacy = ainiux::config::parse(
        "[command]\nstring = /demo\nmodes = continue\nprompt = \"Legacy\"\n",
        "command-legacy.conf");
    options = ainiux::cli::Options{};
    err = ainiux::config::apply_document(legacy.document, options);
    demo = ainiux::editor::find_assist_command(options.editor_assist_config, "demo");
    check(err.ok() && demo != nullptr && demo->modes.size() == 1 &&
              demo->modes.front() == ainiux::editor::AssistCommandMode::Continue,
          "legacy slash-prefixed command strings and explicit modes remain authoritative");
}

void test_bundled_model_catalog_outside_source_directory() {
    const std::filesystem::path original_directory =
        std::filesystem::current_path();
    const std::filesystem::path isolated_directory =
        std::filesystem::absolute("build/config-isolated-cwd").lexically_normal();
    std::filesystem::create_directories(isolated_directory);
    std::filesystem::current_path(isolated_directory);

    const ainiux::config::Environment environment{
        (isolated_directory / "no-user-config").string(),
        (isolated_directory / "no-home").string(),
    };
    const ainiux::config::LoadResult loaded =
        ainiux::config::load_automatic(ainiux::cli::Options{}, environment, false);
    std::filesystem::current_path(original_directory);

    const ainiux::ModelCapability* deepseek_v4 =
        ainiux::config::resolve_model_capability(
            loaded.options.model_catalog, "deepseek", "chat",
            "deepseek/deepseek-v4-pro");
    check(loaded.error.ok() && deepseek_v4 != nullptr &&
              deepseek_v4->context_window_tokens == 1000000,
          "bundled catalog remains available outside the source directory and "
          "matches a routed DeepSeek V4 Pro name");
}

void test_config_empty_and_numeric_edge_cases() {
    ainiux::config::ParseResult parsed = ainiux::config::parse("", "empty.conf");
    check(parsed.error.ok() && parsed.document.entries.empty(),
          "empty config document parses successfully");

    parsed = ainiux::config::parse("tiny = 0.0000000012\nzero = 0\n", "numeric.conf");
    check(parsed.error.ok(), "config parser accepts tiny floats and zero integers");
    const ainiux::config::Entry* tiny = parsed.document.find("tiny");
    const ainiux::config::Entry* zero = parsed.document.find("zero");
    check(tiny != nullptr && tiny->value.is_float() && tiny->value.floating > 0.0 &&
              tiny->value.floating < 1e-8,
          "config parser preserves very small positive numbers");
    check(zero != nullptr && zero->value.is_integer() && zero->value.integer == 0,
          "config parser preserves zero integers");

    parsed = ainiux::config::parse("huge = -999999999999999999999\n", "overflow.conf");
    check(!parsed.error.ok() && parsed.error.message.find("signed 64-bit") != std::string::npos,
          "config parser rejects overflowing negative integers");

    parsed = ainiux::config::parse("greeting = \"\"\n", "empty-string.conf");
    check(parsed.error.ok(), "config parser accepts an empty quoted string");
    const ainiux::config::Entry* greeting = parsed.document.find("greeting");
    check(greeting != nullptr && greeting->value.is_string() && greeting->value.string.empty(),
          "config parser stores empty string values");
}

void test_config_code_index_size() {
    ainiux::config::ParseResult parsed = ainiux::config::parse(
        "[index]\nmax_source_code_file_size = 3M\n", "index.conf");
    ainiux::cli::Options options;
    const ainiux::Error error = ainiux::config::apply_document(parsed.document, options);
    check(parsed.error.ok() && error.ok() &&
              options.max_source_code_file_size == 3U * 1024U * 1024U,
          "index maximum source size accepts layered byte-size setting");
}

void test_config_security_review_settings() {
    ainiux::config::ParseResult parsed = ainiux::config::parse(
        "[agent]\nmax_parallel_agents = 4\nsecurity_review_batch_size = 200K\nsecurity_review_log_enabled = off\nsecurity_review_log_keep_runs = 9\n",
        "agent.conf");
    ainiux::cli::Options options;
    ainiux::Error error = ainiux::config::apply_document(parsed.document, options);
    check(parsed.error.ok() && error.ok() && options.max_parallel_agents == 4 &&
              options.security_review_batch_size == 204800 &&
              !options.security_review_log_enabled && options.security_review_log_keep_runs == 9,
          "agent config parses bounded concurrency and binary-K review size");

    parsed = ainiux::config::parse("[agent]\nmax_parallel_agents = 33\n", "agent-bad.conf");
    options = ainiux::cli::Options{};
    error = ainiux::config::apply_document(parsed.document, options);
    check(!error.ok(), "agent config rejects more than 32 parallel workers");

    parsed = ainiux::config::parse("[agent]\nsecurity_review_batch_size = 0\n", "agent-zero.conf");
    options = ainiux::cli::Options{};
    error = ainiux::config::apply_document(parsed.document, options);
    check(!error.ok(), "agent config rejects a zero review batch size");

    parsed = ainiux::config::parse("[agent]\nsecurity_review_log_keep_runs = 1001\n", "agent-keep.conf");
    options = ainiux::cli::Options{};
    error = ainiux::config::apply_document(parsed.document, options);
    check(!error.ok(), "security review log retention rejects values above 1000");
}

void test_config_agent_max_response_bytes() {
    check(ainiux::cli::Options{}.agent_max_response_bytes == 32L * 1024L * 1024L,
          "agent max response defaults to 32 MiB");

    ainiux::config::ParseResult parsed =
        ainiux::config::parse("[agent]\nmax_response_bytes = 64M\n", "agent-resp.conf");
    ainiux::cli::Options options;
    ainiux::Error error = ainiux::config::apply_document(parsed.document, options);
    check(parsed.error.ok() && error.ok() &&
              options.agent_max_response_bytes == 64L * 1024L * 1024L,
          "agent.max_response_bytes accepts binary-M sizes");

    parsed = ainiux::config::parse("[agent]\nmax_response_bytes = 0\n", "agent-resp-zero.conf");
    options = ainiux::cli::Options{};
    error = ainiux::config::apply_document(parsed.document, options);
    check(parsed.error.ok() && error.ok() && options.agent_max_response_bytes == 0,
          "agent.max_response_bytes 0 disables the HTTP body cap");

    parsed =
        ainiux::config::parse("[agent]\nmax_response_bytes = -1\n", "agent-resp-neg.conf");
    options = ainiux::cli::Options{};
    error = ainiux::config::apply_document(parsed.document, options);
    check(!error.ok(), "agent.max_response_bytes rejects negative sizes");
}

void test_agent_input_height_config() {
    ainiux::cli::Options options;
    check(options.agent_input_max_height_percent == 25,
          "agent input height defaults to 25 percent");
    for (int value : {10, 80}) {
        ainiux::config::ParseResult parsed = ainiux::config::parse(
            "[tui]\nagent_input_max_height_percent = " + std::to_string(value) + "\n",
            "agent-input.conf");
        ainiux::Error error = ainiux::config::apply_document(parsed.document, options);
        check(parsed.error.ok() && error.ok() &&
                  options.agent_input_max_height_percent == value,
              "agent input height accepts its inclusive boundary");
    }
    for (int value : {9, 81}) {
        ainiux::config::ParseResult parsed = ainiux::config::parse(
            "[tui]\nagent_input_max_height_percent = " + std::to_string(value) + "\n",
            "agent-input-invalid.conf");
        ainiux::Error error = ainiux::config::apply_document(parsed.document, options);
        check(!error.ok() && error.message.find("10 through 80") != std::string::npos,
              "agent input height rejects values outside 10 through 80 actionably");
    }
    check(options.agent_thinking_preview_max_chars == 100,
          "agent thinking preview defaults to 100 characters");
    for (int value : {0, 1000}) {
        ainiux::config::ParseResult parsed = ainiux::config::parse(
            "[tui]\nagent_thinking_preview_max_chars = " +
                std::to_string(value) + "\n",
            "agent-thinking.conf");
        ainiux::Error error =
            ainiux::config::apply_document(parsed.document, options);
        check(parsed.error.ok() && error.ok() &&
                  options.agent_thinking_preview_max_chars == value,
              "agent thinking preview accepts its inclusive boundaries");
    }
    ainiux::config::ParseResult parsed = ainiux::config::parse(
        "[tui]\nagent_thinking_preview_max_chars = 1001\n",
        "agent-thinking-invalid.conf");
    ainiux::Error error =
        ainiux::config::apply_document(parsed.document, options);
    check(!error.ok() &&
              error.message.find("0 through 1000") != std::string::npos,
          "agent thinking preview rejects values above 1000 actionably");
}

}  // namespace

void run_all() {
    test_config_applies_user_settings();
    test_config_applies_model_catalog();
    test_model_catalog_layering_and_validation();
    test_config_reads_models_template();
    test_bundled_model_catalog_outside_source_directory();
    test_config_empty_and_numeric_edge_cases();
    test_config_code_index_size();
    test_config_security_review_settings();
    test_config_agent_max_response_bytes();
    test_agent_input_height_config();
    test_config_file_read_errors();
    test_config_parses_supported_values();
    test_config_parses_multiline_strings();
    test_benchmark_prompt_configuration();
    test_config_reads_common_template();
    test_config_rejects_invalid_input();
    test_config_schema_rejects_invalid_settings_transactionally();
    test_config_user_path_resolution();
    test_themes_config();
    test_editor_commands_config();
}

}  // namespace ainiux::test::config
