#include "config/test_config.hpp"
#include "support/test_support.hpp"
#include "cli/args.hpp"
#include "config/config.hpp"
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
          "tui.highlight accepts the on/off compatibility form");
    highlight_config = ainiux::config::parse("[tui]\nhighlight = true\n", "highlight-bool.conf");
    err = ainiux::config::apply_document(highlight_config.document, options);
    check(err.ok() && options.tui_highlight,
          "tui.highlight accepts boolean values");

    ainiux::config::ParseResult editor_config =
        ainiux::config::parse("[editor]\nundo_limit = 7\nhuge_file_size_warning = 2048\nfile_size_limit = -1\n"
                              "tab-width = 8\ntab-style = tab\nlinebreak = crlf\n"
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
    const char* layered_argv[] = {
        "ainiux", "--editor-continue-postfix-max-chars", "40",
        "--editor-continue-prose-postfix-max-chars", "400"};
    const ainiux::cli::ParseResult layered_cli =
        ainiux::cli::parse_args(5, const_cast<char**>(layered_argv), layered);
    check(layered_cli.error.ok() &&
              layered_cli.options.editor_ai_continue_prefix_max_chars == 30 &&
              layered_cli.options.editor_ai_continue_postfix_max_chars == 40 &&
              layered_cli.options.editor_ai_continue_prose_prefix_max_chars == 300 &&
              layered_cli.options.editor_ai_continue_prose_postfix_max_chars == 400,
          "continuation settings follow system then user then CLI precedence");

    ainiux::config::ParseResult insert_config = ainiux::config::parse(
        "[input]\nauto-convert-html-to-md = no\n", "insert.conf");
    check(insert_config.error.ok(), "insert conversion config fixture parses");
    err = ainiux::config::apply_document(insert_config.document, options);
    check(err.ok() && !options.auto_convert_html_to_markdown,
          "input auto-convert-html-to-md accepts no");

    options.editor_assist_config = ainiux::editor::default_editor_assist_config();
    ainiux::config::ParseResult assist_prompt_config = ainiux::config::parse(
        "[editor]\nassist_fact = \"Custom fact prompt\"\n", "assist-fact.conf");
    check(assist_prompt_config.error.ok(), "editor assist fact config parses");
    err = ainiux::config::apply_document(assist_prompt_config.document, options);
    const ainiux::editor::EditorAssistCommand* fact_command =
        ainiux::editor::find_assist_command(options.editor_assist_config, "/fact");
    check(err.ok() && fact_command != nullptr && fact_command->prompt == "Custom fact prompt",
          "editor assist fact config applies");

    const std::string system_home =
        std::filesystem::absolute("build/config-system").lexically_normal().string();
    std::filesystem::create_directories(system_home + "/ainiux");
    {
        std::ofstream system_config(system_home + "/ainiux/config.conf", std::ios::trunc);
        check(system_config.is_open(), "system config test file opens");
        system_config << "[url_fetch]\nallow_private_addresses = false\n"
                         "[tui]\ntheme = light\nthinking_traces = false\n";
        system_config.close();
        check(system_config.good(), "system config test file is written");
    }
    const std::string config_home =
        std::filesystem::absolute("tests/fixtures/config-home").lexically_normal().string();
    ainiux::config::Environment environment{config_home, system_home, "/nonexistent"};
    ainiux::config::LoadResult loaded = ainiux::config::load_automatic(ainiux::cli::Options{}, environment);
    check(loaded.error.ok(), "automatic user config loading succeeds");
    check(loaded.loaded_paths.size() == 5 &&
              loaded.loaded_paths[0].find("benchmarks.conf") != std::string::npos &&
              loaded.loaded_paths[1].find("themes.conf") != std::string::npos &&
              loaded.loaded_paths[2].find("editor-commands.conf") != std::string::npos &&
              loaded.loaded_paths[3] == system_home + "/ainiux/config.conf" &&
              loaded.loaded_paths[4] == config_home + "/ainiux/config.conf",
          "automatic loading applies bundled prompt/UI files before system and user config");
    check(loaded.options.tui_themes.has("dark") && loaded.options.tui_themes.has("light") &&
              loaded.options.tui_themes.has("sepia"),
          "automatic loading includes built-in themes");
    const ainiux::editor::EditorAssistCommand* loaded_spell =
        ainiux::editor::find_assist_command(loaded.options.editor_assist_config, "/spell");
    check(loaded_spell != nullptr && loaded_spell->prompt.find("spelling") != std::string::npos,
          "automatic loading includes built-in editor assist commands");
    const std::array<const char*, 32> added_assist_commands = {
        "expand",      "shorten",   "summarize", "simplify",  "variations",
        "checklist",   "table",     "keypoints", "sentiment", "quiz",
        "questions",   "risk",      "entities",  "brainstorm", "outline",
        "hooks",       "title",     "explain",   "transliterate", "readability",
        "speech",      "fiction",   "blog",      "article",   "joke",
        "roast",       "grumpyman", "Trump",
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
    const std::array<const char*, 12> multiline_assist_commands = {
        "speech", "fiction", "blog", "article", "joke", "roast", "grumpyman", "Trump",
        "fix", "refactor", "tests", "plan",
    };
    for (const char* command_name : multiline_assist_commands) {
        const ainiux::editor::EditorAssistCommand* command =
            ainiux::editor::find_assist_command(loaded.options.editor_assist_config, command_name);
        check(command != nullptr && command->prompt.find('\n') != std::string::npos,
              std::string("complex AI command preserves its multiline prompt: ") + command_name);
    }
    check(loaded.options.allow_private_url_fetch && loaded.options.show_thinking_traces &&
              loaded.options.tui_theme == "dark",
          "user settings partially override automatic system settings");

    ainiux::config::LoadResult system_only =
        ainiux::config::load_automatic(ainiux::cli::Options{}, environment, false);
    check(system_only.error.ok() && !system_only.options.allow_private_url_fetch &&
              !system_only.options.show_thinking_traces && system_only.options.tui_theme == "light",
          "disabling user config retains the automatic system config");
    check(system_only.loaded_paths.size() == 4 &&
              system_only.loaded_paths[0].find("benchmarks.conf") != std::string::npos &&
              system_only.loaded_paths[1].find("themes.conf") != std::string::npos &&
              system_only.loaded_paths[2].find("editor-commands.conf") != std::string::npos &&
              system_only.loaded_paths[3] == system_home + "/ainiux/config.conf",
          "disabling user config still loads bundled prompts/UI files and system config");
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
        "enabled = false\r\n"
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
          "config false boolean parsed");
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
    const std::string system_root = root + "/system";
    const std::string user_root = root + "/user";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(system_root + "/ainiux");
    std::filesystem::create_directories(user_root + "/ainiux");
    {
        std::ofstream file(system_root + "/ainiux/benchmarks.conf");
        file << "[grading]\nsystem_prompt = \"automatic system prompt\"\n";
    }
    {
        std::ofstream file(user_root + "/ainiux/benchmarks.conf");
        file << "[grading]\ncase_prompt = \"automatic user "
                "{{benchmark_case_json}} prompt\"\n";
    }
    const ainiux::config::Environment environment{user_root, system_root,
                                                   "/nonexistent"};
    ainiux::config::LoadResult automatic = ainiux::config::load_automatic(
        ainiux::cli::Options{}, environment, true);
    check(automatic.error.ok() &&
              automatic.options.benchmark_grading_prompts.system_prompt ==
                  "automatic system prompt" &&
              automatic.options.benchmark_grading_prompts.case_prompt ==
                  "automatic user {{benchmark_case_json}} prompt",
          "automatic benchmark prompts load bundled then system then user overrides");
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
          "--no-config skips user benchmark prompts but retains bundled and system prompts");
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
    const char* previous_override = std::getenv("AINIUX_BENCHMARKS");
    const bool had_previous_override = previous_override != nullptr;
    const std::string saved_override =
        previous_override == nullptr ? std::string() : previous_override;
    setenv("AINIUX_BENCHMARKS", override_path.c_str(), 1);
    ainiux::config::LoadResult overridden = ainiux::config::load_automatic(
        ainiux::cli::Options{}, {"", "/nonexistent", "/nonexistent"}, false);
    if (had_previous_override) {
        setenv("AINIUX_BENCHMARKS", saved_override.c_str(), 1);
    } else {
        unsetenv("AINIUX_BENCHMARKS");
    }
    check(overridden.error.ok() &&
              overridden.options.benchmark_grading_prompts.system_prompt ==
                  "environment system" &&
              overridden.options.benchmark_grading_prompts.case_prompt ==
                  "environment {{benchmark_case_json}} case",
          "AINIUX_BENCHMARKS overrides the bundled benchmark prompt path");
}

void test_config_applies_model_settings() {
    ainiux::config::ParseResult parsed = ainiux::config::parse(
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

    ainiux::cli::Options options;
    ainiux::Error err = ainiux::config::apply_document(parsed.document, options);
    check(err.ok() && options.model_settings.size() == 1, "model-setting config applies");
    const ainiux::ModelSetting& setting = options.model_settings.front();
    check(setting.model == "Qwen3.6-*" && setting.purpose == "coding" &&
              setting.default_system_prompt.empty() && setting.temperature == 0.6 && setting.top_k == 20 &&
              setting.top_p == 0.95 && setting.min_p == 0.0 && setting.repeat_penalty == 1.0 &&
              setting.presence_penalty == 0.0,
          "model-setting values are stored");

    ainiux::config::ParseResult override_config = ainiux::config::parse(
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
    err = ainiux::config::apply_document(override_config.document, options);
    check(err.ok() && options.model_settings.size() == 2, "model-setting merge keeps other entries");
    check(options.model_settings[0].model == "Qwen3.6-*" && options.model_settings[0].temperature == 0.4 &&
              options.model_settings[0].top_k == 10 && options.model_settings[1].model == "Gemma-4-31B",
          "model-setting merge replaces matching model and purpose in place");
}

void test_config_model_setting_thinking_budget() {
    ainiux::config::ParseResult verbal = ainiux::config::parse(
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
    ainiux::cli::Options options;
    ainiux::Error err = ainiux::config::apply_document(verbal.document, options);
    check(err.ok() && options.model_settings.size() == 1 &&
              options.model_settings.front().thinking_budget == "high",
          "model-setting config accepts verbal thinking_budget");

    ainiux::config::ParseResult numeric = ainiux::config::parse(
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
    err = ainiux::config::apply_document(numeric.document, options);
    check(err.ok() && options.model_settings.size() == 2 &&
              options.model_settings.back().thinking_budget == "8192",
          "model-setting config accepts token thinking_budget");
}

void test_config_reads_common_template() {
    ainiux::config::ParseResult parsed = ainiux::config::read_file("config/ainiux.conf");
    check(parsed.error.ok(), "common config file parses");
    check(parsed.document.entries.size() == 161, "common config has every expected setting");
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
              options.editor_ai_continue_prefix_max_chars ==
                  ainiux::editor::kDefaultAiContinuePrefixMaxChars &&
              options.editor_ai_continue_postfix_max_chars ==
                  ainiux::editor::kDefaultAiContinuePostfixMaxChars &&
              options.editor_ai_continue_prose_prefix_max_chars ==
                  ainiux::editor::kDefaultAiContinueProsePrefixMaxChars &&
              options.editor_ai_continue_prose_postfix_max_chars ==
                  ainiux::editor::kDefaultAiContinueProsePostfixMaxChars &&
              options.editor_ai_continue_max_tokens == ainiux::editor::kDefaultAiContinueMaxTokens,
          "common config maps to the built-in runtime defaults");
    check(options.model_settings.size() == 12, "common config includes model-setting presets");
    check(options.model_settings.front().model == "Qwen3.6-*" &&
              options.model_settings.front().purpose == "creative" &&
              options.model_settings.front().temperature == 1.0,
          "common config model-setting presets preserve order and values");
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

    ainiux::config::ParseResult wrong_type =
        ainiux::config::parse("[url_fetch]\nallow_private_addresses = yes\n", "type.conf");
    err = ainiux::config::apply_document(wrong_type.document, options);
    check(!err.ok() && err.message.find("expected boolean, got string") != std::string::npos,
          "config schema reports expected and actual types");

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

void test_config_xdg_path_resolution() {
    ainiux::config::Environment environment{"relative", "/high:relative:/low", "/home/tester"};
    check(ainiux::config::user_config_path(environment) == "/home/tester/.config/ainiux/config.conf",
          "relative XDG_CONFIG_HOME falls back to HOME");
    check(ainiux::config::user_editor_commands_path(environment) ==
              "/home/tester/.config/ainiux/editor-commands.conf",
          "relative XDG_CONFIG_HOME falls back to HOME for editor commands");
    check(ainiux::config::user_themes_path(environment) == "/home/tester/.config/ainiux/themes.conf",
          "relative XDG_CONFIG_HOME falls back to HOME for themes");
    const std::vector<std::string> system = ainiux::config::system_config_paths(environment);
    check(system.size() == 2 && system[0] == "/low/ainiux/config.conf" &&
              system[1] == "/high/ainiux/config.conf",
          "system config directories load in reverse order and ignore relative entries");
    const std::vector<std::string> editor_commands =
        ainiux::config::system_editor_commands_paths(environment);
    check(editor_commands.size() == 2 &&
              editor_commands[0] == "/low/ainiux/editor-commands.conf" &&
              editor_commands[1] == "/high/ainiux/editor-commands.conf",
          "system editor-commands directories load in reverse order and ignore relative entries");
    const std::vector<std::string> themes = ainiux::config::system_themes_paths(environment);
    check(themes.size() == 2 && themes[0] == "/low/ainiux/themes.conf" &&
              themes[1] == "/high/ainiux/themes.conf",
          "system themes directories load in reverse order and ignore relative entries");
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

    const std::string system_home =
        std::filesystem::absolute("build/config-theme-system").lexically_normal().string();
    std::filesystem::create_directories(system_home + "/ainiux");
    {
        std::ofstream system_themes(system_home + "/ainiux/themes.conf", std::ios::trunc);
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
    ainiux::config::Environment environment{config_home, system_home, "/nonexistent"};
    ainiux::config::LoadResult loaded = ainiux::config::load_automatic(ainiux::cli::Options{}, environment);
    check(loaded.error.ok(), "automatic loading with themes overrides succeeds");
    check(loaded.options.tui_themes.has("sepia") && loaded.options.tui_themes.has("dark"),
          "system and user themes merge into the registry");
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
    check(options.editor_assist_config.behavior_rules.find("one-shot") != std::string::npos,
          "editor-commands.conf defines assist behavior rules");

    ainiux::config::ParseResult invalid = ainiux::config::parse("provider = openai\n", "bad-editor.conf");
    check(invalid.error.ok(), "invalid editor-commands fixture parses");
    err = ainiux::config::apply_editor_commands_document(invalid.document, options);
    check(!err.ok() && err.message.find("unknown editor-commands setting") != std::string::npos,
          "editor-commands.conf rejects unrelated settings");

    const std::string system_home =
        std::filesystem::absolute("build/config-editor-system").lexically_normal().string();
    std::filesystem::create_directories(system_home + "/ainiux");
    {
        std::ofstream system_commands(system_home + "/ainiux/editor-commands.conf", std::ios::trunc);
        check(system_commands.is_open(), "system editor-commands test file opens");
        system_commands << "[command]\nstring = /spell\nmodes = selection, all\n"
                           "prompt = \"System spell override\"\n";
        system_commands.close();
        check(system_commands.good(), "system editor-commands test file is written");
    }
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
    ainiux::config::Environment environment{config_home, system_home, "/nonexistent"};
    ainiux::config::LoadResult loaded = ainiux::config::load_automatic(ainiux::cli::Options{}, environment);
    check(loaded.error.ok(), "automatic loading with editor-commands overrides succeeds");
    const ainiux::editor::EditorAssistCommand* overridden_spell =
        ainiux::editor::find_assist_command(loaded.options.editor_assist_config, "/spell");
    check(overridden_spell != nullptr && overridden_spell->prompt == "User spell override",
          "user editor-commands.conf overrides system editor-commands.conf");

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

}  // namespace

void run_all() {
    test_config_applies_user_settings();
    test_config_applies_model_settings();
    test_config_model_setting_thinking_budget();
    test_config_empty_and_numeric_edge_cases();
    test_config_file_read_errors();
    test_config_parses_supported_values();
    test_config_parses_multiline_strings();
    test_benchmark_prompt_configuration();
    test_config_reads_common_template();
    test_config_rejects_invalid_input();
    test_config_schema_rejects_invalid_settings_transactionally();
    test_config_xdg_path_resolution();
    test_themes_config();
    test_editor_commands_config();
}

}  // namespace ainiux::test::config
