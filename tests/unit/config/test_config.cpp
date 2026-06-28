#include "config/test_config.hpp"
#include "support/test_support.hpp"
#include "cli/args.hpp"
#include "config/config.hpp"
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
    check(options.tui_theme == "dark", "user config selects the dark theme");
    check(options.show_thinking_traces, "user config shows thinking traces by default");

    pkchat::config::ParseResult editor_config =
        pkchat::config::parse("[editor]\nundo_limit = 7\nhuge_file_size_warning = 2048\nfile_size_limit = -1\n",
                              "editor.conf");
    check(editor_config.error.ok(), "editor config fixture parses");
    err = pkchat::config::apply_document(editor_config.document, options);
    check(err.ok() && options.editor_undo_limit == 7 &&
              options.editor_huge_file_size_warning == 2048 &&
              options.editor_file_size_limit == -1,
          "editor config settings apply");

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
    check(loaded.loaded_paths.size() == 2 &&
              loaded.loaded_paths[0] == system_home + "/pkchat/config.conf" &&
              loaded.loaded_paths[1] == config_home + "/pkchat/config.conf",
          "automatic loading applies the system file before the XDG user file");
    check(loaded.options.allow_private_url_fetch && loaded.options.show_thinking_traces &&
              loaded.options.tui_theme == "dark",
          "user settings partially override automatic system settings");

    pkchat::config::LoadResult system_only =
        pkchat::config::load_automatic(pkchat::cli::Options{}, environment, false);
    check(system_only.error.ok() && !system_only.options.allow_private_url_fetch &&
              !system_only.options.show_thinking_traces && system_only.options.tui_theme == "light",
          "disabling user config retains the automatic system config");
    check(system_only.loaded_paths.size() == 1 &&
              system_only.loaded_paths[0] == system_home + "/pkchat/config.conf" &&
              system_only.diagnostics.back().state == pkchat::config::ConfigFileState::Skipped,
          "disabled user config is reported as skipped");

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

void test_config_reads_common_template() {
    pkchat::config::ParseResult parsed = pkchat::config::read_file("config/pkchat.conf");
    check(parsed.error.ok(), "common config file parses");
    check(parsed.document.entries.size() == 31, "common config has every expected setting");

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
              options.editor_undo_limit == 5 &&
              options.editor_huge_file_size_warning == 1073741824LL &&
              options.editor_file_size_limit == -1,
          "common config maps to the built-in runtime defaults");
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
}

void test_config_xdg_path_resolution() {
    pkchat::config::Environment environment{"relative", "/high:relative:/low", "/home/tester"};
    check(pkchat::config::user_config_path(environment) == "/home/tester/.config/pkchat/config.conf",
          "relative XDG_CONFIG_HOME falls back to HOME");
    const std::vector<std::string> system = pkchat::config::system_config_paths(environment);
    check(system.size() == 2 && system[0] == "/low/pkchat/config.conf" &&
              system[1] == "/high/pkchat/config.conf",
          "system config directories load in reverse order and ignore relative entries");
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
    test_config_empty_and_numeric_edge_cases();
    test_config_file_read_errors();
    test_config_parses_supported_values();
    test_config_reads_common_template();
    test_config_rejects_invalid_input();
    test_config_schema_rejects_invalid_settings_transactionally();
    test_config_xdg_path_resolution();
}

}  // namespace pkchat::test::config
