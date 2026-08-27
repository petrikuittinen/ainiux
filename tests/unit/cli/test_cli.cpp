#include "cli/test_cli.hpp"
#include "support/test_support.hpp"
#include "cli/args.hpp"
#include "config/config.hpp"
#include "markdown/markdown.hpp"
#include "ainiux/version.hpp"
#include "editor/editor.hpp"
#include "provider/provider.hpp"
#include "tui/theme_registry.hpp"
#include <string>

namespace ainiux::test::cli {

namespace {

using ainiux::test::check;
using ainiux::test::read_fixture;

void test_cli_chat_nocolors_parse() {
    const char* argv[] = {"ainiux", "--chat", "--nocolors", "lmstudio"};
    ainiux::cli::ParseResult parsed = ainiux::cli::parse_args(4, const_cast<char**>(argv));
    check(parsed.error.ok(), "chat UI nocolors args parse");
    check(parsed.options.tui, "chat UI flag parsed with nocolors");
    check(parsed.options.no_colors, "nocolors flag parsed");
    check(parsed.options.positional_url == "lmstudio", "chat UI nocolors positional profile parsed");
}

void test_cli_theme_parse() {
    const char* light_argv[] = {"ainiux", "--chat", "--theme", "light"};
    ainiux::cli::ParseResult parsed =
        ainiux::cli::parse_args(4, const_cast<char**>(light_argv));
    check(parsed.error.ok(), "--theme light args parse");
    check(parsed.options.tui_theme == "light", "--theme light selects the light palette");
    check(!parsed.options.no_colors, "--theme light enables colors");

    const char* equals_argv[] = {"ainiux", "--editor", "--theme=dark"};
    parsed = ainiux::cli::parse_args(3, const_cast<char**>(equals_argv));
    check(parsed.error.ok(), "--theme=dark equals-form parses");
    check(parsed.options.tui_theme == "dark" && !parsed.options.no_colors,
          "--theme=dark selects dark and keeps colors on");

    const char* off_argv[] = {"ainiux", "--chat", "--theme", "off"};
    parsed = ainiux::cli::parse_args(4, const_cast<char**>(off_argv));
    check(parsed.error.ok(), "--theme off args parse");
    check(parsed.options.no_colors, "--theme off disables colors like /theme off");
    check(parsed.options.tui_theme == "dark",
          "--theme off keeps the previous palette name for later /theme");

    const char* off_case_argv[] = {"ainiux", "--theme", "OFF", "--editor"};
    parsed = ainiux::cli::parse_args(4, const_cast<char**>(off_case_argv));
    check(parsed.error.ok() && parsed.options.no_colors, "--theme OFF is case-insensitive");

    // First parse only has built-in dark/light; custom names are deferred until
    // the post-config reparse (which loads themes.conf).
    const char* deferred_argv[] = {"ainiux", "--chat", "--theme", "sepia"};
    parsed = ainiux::cli::parse_args(4, const_cast<char**>(deferred_argv));
    check(parsed.error.ok(), "first-pass --theme sepia is accepted before themes.conf");
    check(parsed.options.tui_theme == "sepia" && !parsed.options.no_colors,
          "first-pass keeps the raw theme name for reparse");

    ainiux::cli::Options with_themes;
    ainiux::config::ParseResult themes = ainiux::config::read_file("config/themes.conf");
    check(themes.error.ok(), "bundled themes.conf is readable for CLI reparse test");
    check(ainiux::config::apply_themes_document(themes.document, with_themes).ok(),
          "bundled themes.conf applies for CLI reparse test");
    check(with_themes.tui_themes.has("sepia"), "bundled themes include sepia");

    const char* reparse_argv[] = {"ainiux", "--chat", "--theme", "sepia"};
    parsed = ainiux::cli::parse_args(4, const_cast<char**>(reparse_argv), with_themes);
    check(parsed.error.ok(), "second-pass --theme sepia resolves after themes.conf");
    check(parsed.options.tui_theme == "sepia" && !parsed.options.no_colors,
          "second-pass selects the sepia palette");

    const char* unknown_argv[] = {"ainiux", "--theme", "not-a-real-theme"};
    parsed = ainiux::cli::parse_args(3, const_cast<char**>(unknown_argv), with_themes);
    check(!parsed.error.ok() && parsed.error.code == ainiux::ErrorCode::BadArgs,
          "unknown --theme is rejected once the full registry is loaded");
    check(parsed.error.message.find("unknown theme") != std::string::npos,
          "unknown --theme names the problem");

    const char* missing_argv[] = {"ainiux", "--theme"};
    parsed = ainiux::cli::parse_args(2, const_cast<char**>(missing_argv));
    check(!parsed.error.ok() && parsed.error.code == ainiux::ErrorCode::BadArgs,
          "--theme without a value is rejected");
}

void test_cli_color_mode_parse() {
    check(ainiux::cli::Options{}.color_mode == ainiux::tui::ColorModePreference::Auto,
          "color_mode defaults to auto");

    const char* auto_argv[] = {"ainiux", "--chat", "--color-mode", "auto"};
    ainiux::cli::ParseResult parsed =
        ainiux::cli::parse_args(4, const_cast<char**>(auto_argv));
    check(parsed.error.ok() && parsed.options.color_mode == ainiux::tui::ColorModePreference::Auto,
          "--color-mode auto parses");

    const char* tc_argv[] = {"ainiux", "--editor", "--color-mode=truecolor"};
    parsed = ainiux::cli::parse_args(3, const_cast<char**>(tc_argv));
    check(parsed.error.ok() &&
              parsed.options.color_mode == ainiux::tui::ColorModePreference::Truecolor,
          "--color-mode=truecolor equals-form parses");

    const char* c256_argv[] = {"ainiux", "--chat", "--color-mode", "256"};
    parsed = ainiux::cli::parse_args(4, const_cast<char**>(c256_argv));
    check(parsed.error.ok() &&
              parsed.options.color_mode == ainiux::tui::ColorModePreference::Ansi256,
          "--color-mode 256 parses");

    const char* c16_argv[] = {"ainiux", "--color-mode", "16", "--chat"};
    parsed = ainiux::cli::parse_args(4, const_cast<char**>(c16_argv));
    check(parsed.error.ok() &&
              parsed.options.color_mode == ainiux::tui::ColorModePreference::Ansi16,
          "--color-mode 16 parses");

    const char* bad_argv[] = {"ainiux", "--color-mode", "rainbow"};
    parsed = ainiux::cli::parse_args(3, const_cast<char**>(bad_argv));
    check(!parsed.error.ok() && parsed.error.code == ainiux::ErrorCode::BadArgs,
          "unknown --color-mode is rejected");

    const char* missing_argv[] = {"ainiux", "--color-mode"};
    parsed = ainiux::cli::parse_args(2, const_cast<char**>(missing_argv));
    check(!parsed.error.ok() && parsed.error.code == ainiux::ErrorCode::BadArgs,
          "--color-mode without a value is rejected");

    ainiux::config::ParseResult mode_config =
        ainiux::config::parse("[tui]\ncolor_mode = 256\n", "color-mode.conf");
    check(mode_config.error.ok(), "config color_mode parses");
    ainiux::cli::Options options;
    ainiux::Error err = ainiux::config::apply_document(mode_config.document, options);
    check(err.ok() && options.color_mode == ainiux::tui::ColorModePreference::Ansi256,
          "config [tui] color_mode = 256 applies");

    ainiux::config::ParseResult bad_config =
        ainiux::config::parse("[tui]\ncolor_mode = neon\n", "bad-color-mode.conf");
    check(bad_config.error.ok(), "invalid color_mode document still parses as TOML-alike");
    err = ainiux::config::apply_document(bad_config.document, options);
    check(!err.ok(), "invalid [tui] color_mode is rejected");
}

void test_cli_chat_parse() {
    const char* argv[] = {"ainiux", "--chat", "lmstudio"};
    ainiux::cli::ParseResult parsed = ainiux::cli::parse_args(3, const_cast<char**>(argv));
    check(parsed.error.ok(), "chat UI args parse");
    check(parsed.options.tui, "chat UI flag parsed");
    check(parsed.options.positional_url == "lmstudio", "chat UI positional profile parsed");

    const char* short_argv[] = {"ainiux", "-c", "lmstudio"};
    parsed = ainiux::cli::parse_args(3, const_cast<char**>(short_argv));
    check(parsed.error.ok(), "chat UI short -c args parse");
    check(parsed.options.tui, "chat UI short -c flag parsed");
    check(parsed.options.positional_url == "lmstudio", "chat UI short -c positional profile parsed");

    const char* alias_argv[] = {"ainiux", "--tui", "lmstudio"};
    parsed = ainiux::cli::parse_args(3, const_cast<char**>(alias_argv));
    check(parsed.error.ok(), "legacy TUI alias args parse");
    check(parsed.options.tui, "legacy TUI alias flag parsed");
}

void test_cli_context_token_parse() {
    const char* binary_argv[] = {"ainiux", "--chat", "--context", "64k"};
    ainiux::cli::ParseResult parsed = ainiux::cli::parse_args(4, const_cast<char**>(binary_argv));
    check(parsed.error.ok(), "binary-k context size parses");
    check(parsed.options.context_tokens == 65536, "64k context size equals 65536 tokens");
    check(parsed.options.has_context_tokens, "--context marks context window as explicitly configured");

    const char* million_argv[] = {"ainiux", "--chat", "--context", "1M"};
    parsed = ainiux::cli::parse_args(4, const_cast<char**>(million_argv));
    check(parsed.error.ok(), "decimal-M context size parses");
    check(parsed.options.context_tokens == 1000000, "1M context size equals one million tokens");

    const char* zero_argv[] = {"ainiux", "--context", "0"};
    parsed = ainiux::cli::parse_args(3, const_cast<char**>(zero_argv));
    check(!parsed.error.ok() && parsed.error.code == ainiux::ErrorCode::BadArgs,
          "zero context size is rejected");

    const char* suffix_argv[] = {"ainiux", "--context", "64kb"};
    parsed = ainiux::cli::parse_args(3, const_cast<char**>(suffix_argv));
    check(!parsed.error.ok() && parsed.error.code == ainiux::ErrorCode::BadArgs,
          "unsupported context size suffix is rejected");

    const char* overflow_argv[] = {"ainiux", "--context", "999999999999999999999M"};
    parsed = ainiux::cli::parse_args(3, const_cast<char**>(overflow_argv));
    check(!parsed.error.ok() && parsed.error.code == ainiux::ErrorCode::BadArgs,
          "overflowing context size is rejected");
}

void test_cli_editor_parse() {
    const char* argv[] = {"ainiux", "--editor", "notes.txt", "--output", "saved.txt"};
    ainiux::cli::ParseResult parsed = ainiux::cli::parse_args(5, const_cast<char**>(argv));
    check(parsed.error.ok(), "editor args parse");
    check(parsed.options.editor, "editor flag parsed");
    check(parsed.options.editor_path == "notes.txt", "editor file path parsed");
    check(parsed.options.positional_url.empty(), "editor file is not stored as positional URL");
    check(parsed.options.output_path == "saved.txt", "editor save-as output parsed");

    const char* short_argv[] = {"ainiux", "-e", "notes.txt", "--output", "saved.txt"};
    parsed = ainiux::cli::parse_args(5, const_cast<char**>(short_argv));
    check(parsed.error.ok(), "editor short -e args parse");
    check(parsed.options.editor, "editor short -e flag parsed");
    check(parsed.options.editor_path == "notes.txt", "editor short -e file path parsed");
    check(parsed.options.output_path == "saved.txt", "editor short -e save-as output parsed");

    const char* provider_argv[] = {"ainiux", "lmstudio", "--editor", "notes.txt"};
    parsed = ainiux::cli::parse_args(4, const_cast<char**>(provider_argv));
    check(parsed.error.ok(), "editor args with provider shortcut parse");
    check(parsed.options.positional_url == "lmstudio", "editor provider shortcut parsed");
    check(parsed.options.editor_path == "notes.txt", "editor file with provider shortcut parsed");

    const char* short_provider_argv[] = {"ainiux", "lmstudio", "-e", "notes.txt"};
    parsed = ainiux::cli::parse_args(4, const_cast<char**>(short_provider_argv));
    check(parsed.error.ok(), "editor short -e with provider shortcut parse");
    check(parsed.options.positional_url == "lmstudio", "editor short -e provider shortcut parsed");
    check(parsed.options.editor_path == "notes.txt", "editor short -e file with provider shortcut parsed");

    const char* url_argv[] = {"ainiux", "http://localhost:1234/v1", "--editor", "draft.md"};
    parsed = ainiux::cli::parse_args(4, const_cast<char**>(url_argv));
    check(parsed.error.ok(), "editor args with base URL parse");
    check(parsed.options.positional_url == "http://localhost:1234/v1", "editor base URL parsed");
    check(parsed.options.editor_path == "draft.md", "editor file with base URL parsed");

    const char* scratch_argv[] = {"ainiux", "--editor"};
    parsed = ainiux::cli::parse_args(2, const_cast<char**>(scratch_argv));
    check(parsed.error.ok(), "scratch editor args parse");
    check(parsed.options.editor, "scratch editor flag parsed");
    check(parsed.options.editor_path.empty(), "scratch editor has no file path");

    const char* short_scratch_argv[] = {"ainiux", "-e"};
    parsed = ainiux::cli::parse_args(2, const_cast<char**>(short_scratch_argv));
    check(parsed.error.ok(), "scratch editor short -e args parse");
    check(parsed.options.editor, "scratch editor short -e flag parsed");
    check(parsed.options.editor_path.empty(), "scratch editor short -e has no file path");

    const char* eq_argv[] = {"ainiux", "--editor=notes.txt"};
    parsed = ainiux::cli::parse_args(2, const_cast<char**>(eq_argv));
    check(parsed.error.ok(), "editor equals-form args parse");
    check(parsed.options.editor_path == "notes.txt", "editor equals-form file path parsed");

    const char* provider_file_argv[] = {"ainiux", "openrouter", "--editor", "openrouter"};
    parsed = ainiux::cli::parse_args(4, const_cast<char**>(provider_file_argv));
    check(parsed.error.ok(), "editor file named like provider shortcut parses");
    check(parsed.options.positional_url == "openrouter", "provider shortcut stays positional");
    check(parsed.options.editor_path == "openrouter", "editor path named like provider stays on --editor");

    const char* old_argv[] = {"ainiux", "--editor-continue-read", "1"};
    parsed = ainiux::cli::parse_args(3, const_cast<char**>(old_argv));
    check(!parsed.error.ok(), "removed --editor-continue-read option is rejected");
    const char* removed_continue_argv[] = {
        "ainiux", "--editor-continue-prefix-max-chars", "100"};
    parsed = ainiux::cli::parse_args(3, const_cast<char**>(removed_continue_argv));
    check(!parsed.error.ok(), "removed editor continue CLI options are rejected");

    const char* dired_argv[] = {"ainiux", "--dired"};
    parsed = ainiux::cli::parse_args(2, const_cast<char**>(dired_argv));
    check(parsed.error.ok(), "--dired without path parses");
    check(parsed.options.dired && parsed.options.editor, "--dired enables dired and editor");
    check(parsed.options.dired_path.empty(), "--dired without path leaves dired_path empty");

    const char* dired_path_argv[] = {"ainiux", "--dired", "src/"};
    parsed = ainiux::cli::parse_args(3, const_cast<char**>(dired_path_argv));
    check(parsed.error.ok(), "--dired with path parses");
    check(parsed.options.dired && parsed.options.editor, "--dired path enables dired and editor");
    check(parsed.options.dired_path == "src/", "--dired path stored");

    const char* dired_eq_argv[] = {"ainiux", "--dired=src/*.cpp"};
    parsed = ainiux::cli::parse_args(2, const_cast<char**>(dired_eq_argv));
    check(parsed.error.ok(), "--dired=glob parses");
    check(parsed.options.dired_path == "src/*.cpp", "--dired equals-form path stored");

    const char* dired_provider_argv[] = {"ainiux", "lmstudio", "--dired", "."};
    parsed = ainiux::cli::parse_args(4, const_cast<char**>(dired_provider_argv));
    check(parsed.error.ok(), "--dired with provider shortcut parses");
    check(parsed.options.positional_url == "lmstudio", "--dired keeps provider positional");
    check(parsed.options.dired_path == ".", "--dired path with provider parsed");

    const char* dired_short_argv[] = {"ainiux", "-d"};
    parsed = ainiux::cli::parse_args(2, const_cast<char**>(dired_short_argv));
    check(parsed.error.ok(), "-d without path parses");
    check(parsed.options.dired && parsed.options.editor, "-d enables dired and editor");
    check(parsed.options.dired_path.empty(), "-d without path leaves dired_path empty");

    const char* dired_short_path_argv[] = {"ainiux", "-d", "src/"};
    parsed = ainiux::cli::parse_args(3, const_cast<char**>(dired_short_path_argv));
    check(parsed.error.ok(), "-d with path parses");
    check(parsed.options.dired && parsed.options.editor, "-d path enables dired and editor");
    check(parsed.options.dired_path == "src/", "-d path stored");

    const char* dired_short_provider_argv[] = {"ainiux", "none", "-d", "."};
    parsed = ainiux::cli::parse_args(4, const_cast<char**>(dired_short_provider_argv));
    check(parsed.error.ok(), "-d with provider shortcut parses");
    check(parsed.options.positional_url == "none", "-d keeps provider positional");
    check(parsed.options.dired_path == ".", "-d path with provider parsed");
}

void test_cli_help_displays_version() {
    const std::string help = ainiux::cli::help_text();
    const std::string expected_heading =
        ainiux::app_version_label() + " - script-friendly OpenAI-compatible chat CLI";
    check(help.rfind(expected_heading, 0) == 0,
          "CLI help heading displays the current app version");
    check(help.find("ainiux -e notes.txt") != std::string::npos,
          "CLI help shows a short editor example without an explicit model");
    check(help.find("choose a model inside the editor with /model") != std::string::npos,
          "CLI help documents deferred editor model selection");
    check(help.find("ainiux lmstudio -m MODEL -r ") != std::string::npos,
          "CLI help shows a short one-shot agent -r example");
    check(help.find("-c, --chat") != std::string::npos,
          "CLI help documents -c short option for --chat");
    check(help.find("-e, --editor") != std::string::npos,
          "CLI help documents -e short option for --editor");
    check(help.find("-d, --dired") != std::string::npos,
          "CLI help documents -d short option for --dired");
    check(help.find("Mode:") != std::string::npos &&
              help.find("Prompt and generation:") != std::string::npos &&
              help.find("Provider and endpoint:") != std::string::npos &&
              help.find("Benchmark:") != std::string::npos,
          "CLI help groups options into logical sections");
    check(help.find("--editor-continue-prefix-max-chars") == std::string::npos &&
              help.find("--editor-continue-postfix-max-chars") == std::string::npos &&
              help.find("--editor-continue-prose-prefix-max-chars") == std::string::npos &&
              help.find("--editor-continue-prose-postfix-max-chars") == std::string::npos &&
              help.find("--editor-continue-max-tokens") == std::string::npos &&
              help.find("--editor-continue-read") == std::string::npos &&
              help.find("Editor AI continue:") == std::string::npos,
          "CLI help no longer documents removed editor continue options");
}

void test_cli_web_search_parse() {
    const char* argv[] = {"ainiux", "--search", "web scraping", "--web-search-provider", "duckduckgo",
                          "--max-web-search-results", "5", "-p", "summarize"};
    ainiux::cli::ParseResult parsed = ainiux::cli::parse_args(9, const_cast<char**>(argv));
    check(parsed.error.ok(), "web search args parse");
    check(parsed.options.search_query == "web scraping", "web search query parsed");
    check(parsed.options.web_search_provider == "duckduckgo", "web search provider parsed");
    check(parsed.options.max_web_search_results == 5, "web search max results parsed");
    check(parsed.options.max_web_search_results_explicit, "web search max results marked explicit");
    check(parsed.options.prompt == "summarize", "web search prompt parsed");
    check(parsed.options.builtin_web_search, "hosted web_search stays on by default");

    const char* disable[] = {"ainiux", "--no-builtin-web-search", "-p", "hi"};
    ainiux::cli::ParseResult disabled = ainiux::cli::parse_args(4, const_cast<char**>(disable));
    check(disabled.error.ok() && !disabled.options.builtin_web_search,
          "--no-builtin-web-search disables hosted web_search");
}

void test_cli_agent_max_response_bytes_parse() {
    check(ainiux::cli::Options{}.agent_max_response_bytes == 32L * 1024L * 1024L,
          "agent response cap defaults to 32 MiB");

    const char* argv[] = {"ainiux", "--max-agent-response-bytes", "64M", "-p", "hi"};
    ainiux::cli::ParseResult parsed = ainiux::cli::parse_args(5, const_cast<char**>(argv));
    check(parsed.error.ok(), "agent max response bytes parse");
    check(parsed.options.agent_max_response_bytes == 64L * 1024L * 1024L,
          "agent max response bytes accepts 64M");

    const char* unlimited[] = {"ainiux", "--max-agent-response-bytes", "0", "-p", "hi"};
    parsed = ainiux::cli::parse_args(5, const_cast<char**>(unlimited));
    check(parsed.error.ok() && parsed.options.agent_max_response_bytes == 0,
          "agent max response bytes 0 disables the cap");

    const char* bad[] = {"ainiux", "--max-agent-response-bytes", "-1", "-p", "hi"};
    parsed = ainiux::cli::parse_args(5, const_cast<char**>(bad));
    check(!parsed.error.ok(), "agent max response bytes rejects negatives");
}

void test_cli_html_extract_parse() {
    const char* argv[] = {"ainiux", "--fetch-url", "https://example.com/page", "--html-format", "markdown",
                          "--max-fetch-bytes", "123", "--allow-private-url-fetch", "--output", "page.md"};
    ainiux::cli::ParseResult parsed = ainiux::cli::parse_args(10, const_cast<char**>(argv));
    check(parsed.error.ok(), "HTML fetch args parse");
    check(parsed.options.fetch_url == "https://example.com/page", "HTML fetch URL parsed");
    check(parsed.options.html_format == "markdown", "HTML output format parsed");
    check(parsed.options.max_fetch_bytes == 123, "HTML max fetch bytes parsed");
    check(parsed.options.allow_private_url_fetch, "HTML private fetch override parsed");
    check(parsed.options.output_path == "page.md", "HTML output path parsed");

    const char* file_argv[] = {"ainiux", "--input", "page.html", "--output-format", "plaintext"};
    parsed = ainiux::cli::parse_args(5, const_cast<char**>(file_argv));
    check(parsed.error.ok(), "input file args parse");
    check(parsed.options.input_path == "page.html", "input file path parsed");
    check(parsed.options.output_format == ainiux::markdown::OutputFormat::Plaintext, "input plaintext output format parsed");
    check(parsed.options.rendered_output_format_explicit, "input rendered output format marked explicit");

    const char* legacy_file_argv[] = {"ainiux", "--html-file", "page.html", "--html-format", "text"};
    parsed = ainiux::cli::parse_args(5, const_cast<char**>(legacy_file_argv));
    check(parsed.error.ok(), "legacy HTML file args parse");
    check(parsed.options.html_file == "page.html", "legacy HTML file path parsed");
    check(parsed.options.html_format == "text", "legacy HTML text format parsed");

    const char* image_argv[] = {"ainiux", "--input", "PHOTO.JPEG", "--max-image-bytes", "4096", "-p", "describe"};
    parsed = ainiux::cli::parse_args(7, const_cast<char**>(image_argv));
    check(parsed.error.ok(), "image input args parse");
    check(parsed.options.input_path == "PHOTO.JPEG", "image input path parsed");
    check(parsed.options.max_image_bytes == 4096, "image byte limit parsed");

    const char* attach_argv[] = {"ainiux", "-p", "compare", "--attach", "one.md", "--attach", "two.txt",
                                 "--max-input-bytes", "8192"};
    parsed = ainiux::cli::parse_args(9, const_cast<char**>(attach_argv));
    check(parsed.error.ok(), "repeatable attachment args parse");
    check(parsed.options.attachment_paths.size() == 2, "two attachment paths parsed");
    check(parsed.options.attachment_paths[0] == "one.md" && parsed.options.attachment_paths[1] == "two.txt",
          "attachment path order is preserved");
    check(parsed.options.max_input_bytes == 8192, "text input byte limit parsed");

    const char* context_argv[] = {"ainiux", "-p", "hello", "--context-policy", "summarize-middle",
                                  "--max-context-bytes", "4096", "--image-capability", "allow"};
    parsed = ainiux::cli::parse_args(9, const_cast<char**>(context_argv));
    check(parsed.error.ok(), "context and image capability args parse");
    check(parsed.options.context_policy == "summarize-middle", "context policy parsed");
    check(parsed.options.max_context_bytes == 4096, "context byte limit parsed");
    check(parsed.options.image_capability == "allow", "image capability override parsed");
}

void test_cli_output_format_parse() {
    const char* argv[] = {"ainiux", "-p", "hello", "--output-format", "html", "--output", "answer.html"};
    ainiux::cli::ParseResult parsed = ainiux::cli::parse_args(7, const_cast<char**>(argv));
    check(parsed.error.ok(), "CLI output-format args parse");
    check(parsed.options.output_format == ainiux::markdown::OutputFormat::Html, "HTML output format parsed");
    check(parsed.options.output_format_explicit, "output-format explicit flag parsed");
    check(parsed.options.output_path == "answer.html", "output path parsed with output-format");

    const char* plain_argv[] = {"ainiux", "-p", "hello", "--output-format", "plaintext"};
    parsed = ainiux::cli::parse_args(5, const_cast<char**>(plain_argv));
    check(parsed.error.ok(), "CLI plaintext output-format args parse");
    check(parsed.options.output_format == ainiux::markdown::OutputFormat::Plaintext, "plaintext output format parsed");

    const char* json_argv[] = {"ainiux", "-p", "hello", "--output-format", "json"};
    parsed = ainiux::cli::parse_args(5, const_cast<char**>(json_argv));
    check(parsed.error.ok(), "CLI json output-format args parse");
    check(parsed.options.format == ainiux::cli::OutputFormat::Json, "json output-format maps to JSON format");
    check(!parsed.options.rendered_output_format_explicit, "json output-format is not a rendered text format");

    const char* jsond_argv[] = {"ainiux", "-p", "hello", "--output-format", "jsond"};
    parsed = ainiux::cli::parse_args(5, const_cast<char**>(jsond_argv));
    check(parsed.error.ok(), "CLI jsond output-format args parse");
    check(parsed.options.format == ainiux::cli::OutputFormat::Ndjson, "jsond output-format maps to NDJSON format");

    const char* bad_argv[] = {"ainiux", "-p", "hello", "--output-format", "pdf"};
    parsed = ainiux::cli::parse_args(5, const_cast<char**>(bad_argv));
    check(!parsed.error.ok(), "CLI rejects bad output-format");
}

void test_cli_parse() {
    const char* argv[] = {"ainiux", "http://localhost:8000", "-p", "hello", "--no-stream", "--format", "json", "-v", "--no-config", "--save-chat", "chat.json"};
    ainiux::cli::ParseResult parsed = ainiux::cli::parse_args(11, const_cast<char**>(argv));
    check(parsed.error.ok(), "CLI parse should succeed");
    check(parsed.options.positional_url == "http://localhost:8000", "positional URL parsed");
    check(parsed.options.prompt == "hello", "prompt parsed");
    check(!parsed.options.stream, "no-stream parsed");
    check(parsed.options.format == ainiux::cli::OutputFormat::Json, "json format parsed");
    check(parsed.options.verbose, "verbose parsed");
    check(parsed.options.no_config, "no-config parsed");
    check(parsed.options.save_chat_path == "chat.json", "save chat parsed");
}

void test_cli_provider_shortcut_parse() {
    const char* argv[] = {"ainiux", "openrouter", "-model", "provider/model", "-i"};
    ainiux::cli::ParseResult parsed = ainiux::cli::parse_args(5, const_cast<char**>(argv));
    check(parsed.error.ok(), "provider shortcut args parse");
    check(parsed.options.positional_url == "openrouter", "provider shortcut stored as positional");
    check(parsed.options.model == "provider/model", "-model alias parsed");
    check(parsed.options.repl, "-i parsed for provider shortcut");
}

void test_cli_rejects_unknown() {
    const char* argv[] = {"ainiux", "--bogus"};
    ainiux::cli::ParseResult parsed = ainiux::cli::parse_args(2, const_cast<char**>(argv));
    check(!parsed.error.ok(), "unknown option rejected");
    check(parsed.error.code == ainiux::ErrorCode::BadArgs, "unknown option is bad args");
}

void test_cli_repl_parse() {
    const char* argv[] = {"ainiux", "--repl", "--load-chat", "chat.json"};
    ainiux::cli::ParseResult parsed = ainiux::cli::parse_args(4, const_cast<char**>(argv));
    check(parsed.error.ok(), "REPL args parse");
    check(parsed.options.repl, "REPL flag parsed");
    check(parsed.options.load_chat_path == "chat.json", "load chat parsed");
}

void test_cli_responses_parse() {
    const char* argv[] = {"ainiux", "--responses", "-p", "hello"};
    ainiux::cli::ParseResult parsed = ainiux::cli::parse_args(4, const_cast<char**>(argv));
    check(parsed.error.ok(), "Responses API shortcut args parse");
    check(parsed.options.api == "responses", "--responses selects Responses API");
}

void test_cli_reasoning_parse() {
    const char* named_argv[] = {"ainiux", "--reasoning", "ultra", "-m", "model"};
    ainiux::cli::ParseResult parsed =
        ainiux::cli::parse_args(5, const_cast<char**>(named_argv));
    check(parsed.error.ok() &&
              parsed.options.reasoning == ainiux::ReasoningSelection::named("ultra") &&
              parsed.options.reasoning_explicit && parsed.options.reasoning_cli_explicit &&
              parsed.options.model_explicit,
          "--reasoning accepts an uncatalogued named value");

    const char* budget_argv[] = {"ainiux", "--reasoning", "8192"};
    parsed = ainiux::cli::parse_args(3, const_cast<char**>(budget_argv));
    check(parsed.error.ok() &&
              parsed.options.reasoning == ainiux::ReasoningSelection::token_budget(8192),
          "--reasoning accepts an exact non-negative token budget");

    const char* auto_argv[] = {"ainiux", "--reasoning", "auto"};
    parsed = ainiux::cli::parse_args(3, const_cast<char**>(auto_argv));
    check(parsed.error.ok() && parsed.options.reasoning.is_auto() &&
              parsed.options.reasoning_cli_explicit,
          "--reasoning auto explicitly clears an override");

    const char* invalid_argv[] = {"ainiux", "--reasoning", "not valid"};
    parsed = ainiux::cli::parse_args(3, const_cast<char**>(invalid_argv));
    check(!parsed.error.ok() && parsed.error.code == ainiux::ErrorCode::BadArgs,
          "--reasoning rejects values outside the bounded ASCII token syntax");

    const char* old_thinking_argv[] = {"ainiux", "--thinking", "on"};
    parsed = ainiux::cli::parse_args(3, const_cast<char**>(old_thinking_argv));
    check(!parsed.error.ok(), "retired --thinking is no longer a public option");
    const char* old_budget_argv[] = {"ainiux", "--thinking-budget", "4096"};
    parsed = ainiux::cli::parse_args(3, const_cast<char**>(old_budget_argv));
    check(!parsed.error.ok(), "retired --thinking-budget is no longer a public option");

    ainiux::cli::Options remembered;
    remembered.provider = "openai";
    remembered.model = "remembered-model";
    remembered.api = "chat";
    remembered.reasoning = ainiux::ReasoningSelection::named("high");
    const char* override_argv[] = {
        "ainiux", "--editor", "--provider", "qwen", "-m", "qwen3.6-plus",
        "--responses", "--reasoning", "low",
    };
    parsed = ainiux::cli::parse_args(
        9, const_cast<char**>(override_argv), remembered);
    check(parsed.error.ok() && parsed.options.provider == "qwen" &&
              parsed.options.model == "qwen3.6-plus" &&
              parsed.options.api == "responses" &&
              parsed.options.reasoning ==
                  ainiux::ReasoningSelection::named("low"),
          "explicit editor CLI selection overrides remembered model state");

    remembered.provider = "none";
    const char* direct_url_argv[] = {
        "ainiux", "http://127.0.0.1:18080", "--editor",
    };
    parsed = ainiux::cli::parse_args(
        3, const_cast<char**>(direct_url_argv), remembered);
    check(parsed.error.ok() && parsed.options.provider == "openai" &&
              parsed.options.positional_url == "http://127.0.0.1:18080",
          "direct endpoint overrides a remembered offline editor provider");
}

void test_cli_empty_and_unicode_edge_cases() {
    const char* argv[] = {"ainiux"};
    ainiux::cli::ParseResult parsed = ainiux::cli::parse_args(1, const_cast<char**>(argv));
    check(parsed.error.ok(), "CLI parse succeeds with only the program name");

    const char* empty_prompt_argv[] = {"ainiux", "-p", ""};
    parsed = ainiux::cli::parse_args(3, const_cast<char**>(empty_prompt_argv));
    check(parsed.error.ok() && parsed.options.prompt.empty(),
          "CLI parse accepts an empty prompt string");

    const std::string unicode_prompt = u8"مرحبا 你好 👨‍👩‍👧‍👦";
    const char* unicode_argv[] = {"ainiux", "-p", unicode_prompt.c_str()};
    parsed = ainiux::cli::parse_args(3, const_cast<char**>(unicode_argv));
    check(parsed.error.ok() && parsed.options.prompt == unicode_prompt,
          "CLI parse preserves Arabic, Chinese, and emoji prompt text");

    bool changed = false;
    ainiux::Error err;
    ainiux::provider::normalize_base_url("", &changed, err);
    check(!err.ok() && err.code == ainiux::ErrorCode::BadUrl,
          "base URL normalization rejects an empty URL");
}

void test_url_normalization() {
    bool changed = false;
    ainiux::Error err;
    std::string url = ainiux::provider::normalize_base_url("http://localhost:8000", &changed, err);
    check(err.ok(), "base URL without path is valid");
    check(changed, "base URL without path is changed");
    check(url == "http://localhost:8000/v1", "base URL appends /v1");
    url = ainiux::provider::normalize_base_url("http://localhost:8000/v1", &changed, err);
    check(err.ok(), "base URL with /v1 is valid");
    check(!changed, "base URL with /v1 unchanged");
    check(url == "http://localhost:8000/v1", "base URL with /v1 preserved");
    url = ainiux::provider::normalize_base_url("ftp://localhost", &changed, err);
    check(!err.ok() && err.code == ainiux::ErrorCode::BadUrl, "bad URL rejected");
}

void test_cli_code_index_parse() {
    const char* defaults[] = {"ainiux", "--index-code"};
    ainiux::cli::ParseResult parsed = ainiux::cli::parse_args(2, const_cast<char**>(defaults));
    check(parsed.error.ok() &&
              parsed.options.max_source_code_file_size == 10U * 1024U * 1024U,
          "code index CLI defaults to a 10 MiB source file limit");

    const char* argv[] = {"ainiux", "--index-code", "--print-index",
                          "--max-source-code-file-size", "2M", "--output", "index.md"};
    parsed = ainiux::cli::parse_args(7, const_cast<char**>(argv));
    check(parsed.error.ok() && parsed.options.index_code && parsed.options.print_index &&
              parsed.options.max_source_code_file_size == 2U * 1024U * 1024U &&
              parsed.options.output_path == "index.md",
          "code index CLI flags and byte-size override parse");
    check(ainiux::cli::validate_index_mode_arguments(7, const_cast<char**>(argv), parsed.options).ok(),
          "code index CLI accepts refresh-and-print options");

    const char* unrelated[] = {"ainiux", "--index-code", "--provider", "openai"};
    parsed = ainiux::cli::parse_args(4, const_cast<char**>(unrelated));
    check(parsed.error.ok() &&
              !ainiux::cli::validate_index_mode_arguments(4, const_cast<char**>(unrelated), parsed.options).ok(),
          "code index CLI rejects provider options");

    const char* bad_size[] = {"ainiux", "--index-code", "--max-source-code-file-size", "huge"};
    parsed = ainiux::cli::parse_args(4, const_cast<char**>(bad_size));
    check(!parsed.error.ok(), "code index CLI rejects invalid byte size");

    const char* clear[] = {"ainiux", "--clear-index"};
    parsed = ainiux::cli::parse_args(2, const_cast<char**>(clear));
    check(parsed.error.ok() && parsed.options.clear_index &&
              ainiux::cli::validate_index_mode_arguments(2, const_cast<char**>(clear), parsed.options).ok(),
          "clear-index parses as a standalone code index mode");

    const char* conflicting[] = {"ainiux", "--clear-index", "--index-code"};
    parsed = ainiux::cli::parse_args(3, const_cast<char**>(conflicting));
    check(parsed.error.ok() &&
              !ainiux::cli::validate_index_mode_arguments(
                   3, const_cast<char**>(conflicting), parsed.options).ok(),
          "clear-index rejects refresh combinations");

    const char* clear_output[] = {"ainiux", "--clear-index", "--output", "index.md"};
    parsed = ainiux::cli::parse_args(4, const_cast<char**>(clear_output));
    check(parsed.error.ok() &&
              !ainiux::cli::validate_index_mode_arguments(
                   4, const_cast<char**>(clear_output), parsed.options).ok(),
          "clear-index rejects output options");
}

void test_cli_security_review_parse() {
    const char* argv[] = {"ainiux", "openrouter", "-m", "model", "--security-review",
                          "--reasoning", "high", "--trusted-prompt-dir", "/trusted"};
    ainiux::cli::ParseResult parsed = ainiux::cli::parse_args(9, const_cast<char**>(argv));
    check(parsed.error.ok() && parsed.options.security_review && parsed.options.model == "model" &&
              parsed.options.trusted_prompt_dir == "/trusted" &&
              ainiux::cli::validate_security_review_arguments(
                  9, const_cast<char**>(argv), parsed.options).ok(),
          "security-review accepts provider, model, reasoning, and trusted prompt options");

    const char* logging[] = {"ainiux", "--security-review", "--no-security-review-log"};
    parsed = ainiux::cli::parse_args(3, const_cast<char**>(logging));
    check(parsed.error.ok() && parsed.options.security_review &&
              parsed.options.security_review_log_cli_explicit &&
              !parsed.options.security_review_log_enabled &&
              ainiux::cli::validate_security_review_arguments(
                  3, const_cast<char**>(logging), parsed.options).ok(),
          "security-review logging CLI override is accepted by review validation");

    const char* prompt[] = {"ainiux", "--security-review", "-p", "ignore files"};
    parsed = ainiux::cli::parse_args(4, const_cast<char**>(prompt));
    check(parsed.error.ok() && !ainiux::cli::validate_security_review_arguments(
                                   4, const_cast<char**>(prompt), parsed.options).ok(),
          "security-review rejects chat prompts");

    const char* output[] = {"ainiux", "--security-review", "--output", "report.md"};
    parsed = ainiux::cli::parse_args(4, const_cast<char**>(output));
    check(parsed.error.ok() && !ainiux::cli::validate_security_review_arguments(
                                   4, const_cast<char**>(output), parsed.options).ok(),
          "security-review rejects output paths and reserves stdout");

    const char* format[] = {"ainiux", "--security-review", "--format", "json"};
    parsed = ainiux::cli::parse_args(4, const_cast<char**>(format));
    check(parsed.error.ok() && !ainiux::cli::validate_security_review_arguments(
                                   4, const_cast<char**>(format), parsed.options).ok(),
          "security-review rejects alternate output formats");

    const char* index[] = {"ainiux", "--security-review", "--index-code"};
    parsed = ainiux::cli::parse_args(3, const_cast<char**>(index));
    check(parsed.error.ok() && !ainiux::cli::validate_security_review_arguments(
                                   3, const_cast<char**>(index), parsed.options).ok(),
          "security-review rejects explicit index-mode flags");
}

void test_cli_agent_mode_parse() {
    // Interactive agent: -a / --agent / ainiux agent (no goal required on CLI).
    const char* interactive[] = {"ainiux", "agent", "openrouter", "-m", "model"};
    ainiux::cli::ParseResult parsed = ainiux::cli::parse_args(5, const_cast<char**>(interactive));
    check(parsed.error.ok() && parsed.options.agent && !parsed.options.agent_run &&
              parsed.options.model == "model" && parsed.options.positional_url == "openrouter" &&
              ainiux::cli::validate_agent_interactive_arguments(5, const_cast<char**>(interactive),
                                                               parsed.options)
                  .ok(),
          "agent subcommand starts interactive agent with provider and model");

    const char* interactive_no_index[] = {
        "ainiux", "--agent", "--disable-indexing"};
    parsed = ainiux::cli::parse_args(
        3, const_cast<char**>(interactive_no_index));
    check(parsed.error.ok() && parsed.options.agent &&
              parsed.options.disable_indexing &&
              ainiux::cli::validate_disable_indexing_arguments(parsed.options)
                  .ok() &&
              ainiux::cli::validate_agent_interactive_arguments(
                  3, const_cast<char**>(interactive_no_index), parsed.options)
                  .ok(),
          "interactive agent accepts session-scoped indexing disable");

    const char* short_agent[] = {"ainiux", "-a", "lmstudio", "-m", "model"};
    parsed = ainiux::cli::parse_args(5, const_cast<char**>(short_agent));
    check(parsed.error.ok() && parsed.options.agent && !parsed.options.agent_run &&
              parsed.options.positional_url == "lmstudio" &&
              ainiux::cli::validate_agent_interactive_arguments(5, const_cast<char**>(short_agent),
                                                               parsed.options)
                  .ok(),
          "interactive agent short -a accepts provider shortcut");

    const char* flag[] = {"ainiux", "--agent", "-m", "model", "--no-agent-log"};
    parsed = ainiux::cli::parse_args(5, const_cast<char**>(flag));
    check(parsed.error.ok() && parsed.options.agent && parsed.options.agent_log_cli_explicit &&
              !parsed.options.agent_log_enabled &&
              ainiux::cli::validate_agent_interactive_arguments(5, const_cast<char**>(flag),
                                                               parsed.options)
                  .ok(),
          "agent flag accepts logging disable override");

    const char* system[] = {"ainiux", "--agent", "-m", "model", "-s", "override"};
    parsed = ainiux::cli::parse_args(6, const_cast<char**>(system));
    check(parsed.error.ok() && !ainiux::cli::validate_agent_interactive_arguments(
                                   6, const_cast<char**>(system), parsed.options)
                                   .ok(),
          "interactive agent rejects user system prompt overrides");

    const char* combined[] = {"ainiux", "--agent", "--security-review"};
    parsed = ainiux::cli::parse_args(3, const_cast<char**>(combined));
    check(parsed.error.ok() && !ainiux::cli::validate_agent_interactive_arguments(
                                   3, const_cast<char**>(combined), parsed.options)
                                   .ok(),
          "interactive agent rejects security-review combination");

    // One-shot agent: -r / --run / --run-file / ainiux run
    const char* run_flag[] = {"ainiux", "http://localhost:30000", "-m", "model", "-r",
                              "remove empty folders"};
    parsed = ainiux::cli::parse_args(6, const_cast<char**>(run_flag));
    check(parsed.error.ok() && parsed.options.agent_run && !parsed.options.agent &&
              parsed.options.prompt == "remove empty folders" &&
              parsed.options.positional_url == "http://localhost:30000" &&
              ainiux::cli::validate_agent_run_arguments(6, const_cast<char**>(run_flag),
                                                       parsed.options)
                  .ok(),
          "--run sets one-shot agent goal and accepts base URL");

    const char* run_sub[] = {"ainiux", "run", "openrouter", "-m", "model", "--run",
                             "add unit tests"};
    parsed = ainiux::cli::parse_args(7, const_cast<char**>(run_sub));
    check(parsed.error.ok() && parsed.options.agent_run && !parsed.options.agent &&
              parsed.options.prompt == "add unit tests" &&
              parsed.options.positional_url == "openrouter" &&
              ainiux::cli::validate_agent_run_arguments(7, const_cast<char**>(run_sub),
                                                       parsed.options)
                  .ok(),
          "ainiux run accepts provider shortcut with --run goal");

    const char* run_file[] = {"ainiux", "run", "lmstudio", "-m", "model", "--run-file", "goal.txt"};
    parsed = ainiux::cli::parse_args(7, const_cast<char**>(run_file));
    check(parsed.error.ok() && parsed.options.agent_run &&
              parsed.options.prompt_file == "goal.txt" &&
              parsed.options.positional_url == "lmstudio" &&
              ainiux::cli::validate_agent_run_arguments(7, const_cast<char**>(run_file),
                                                       parsed.options)
                  .ok(),
          "ainiux run accepts --run-file with provider name");

    const char* plan_sub[] = {"ainiux", "plan", "design the server", "--provider",
                              "openrouter", "-m", "model"};
    parsed = ainiux::cli::parse_args(7, const_cast<char**>(plan_sub));
    check(parsed.error.ok() && parsed.options.agent_run && parsed.options.agent_plan &&
              parsed.options.prompt == "design the server" &&
              parsed.options.provider == "openrouter" &&
              ainiux::cli::validate_agent_run_arguments(7, const_cast<char**>(plan_sub),
                                                       parsed.options)
                  .ok(),
          "ainiux plan consumes its quoted goal and named provider options");

    const char* plan_flag[] = {"ainiux", "openrouter", "--plan", "design the server",
                               "-m", "model"};
    parsed = ainiux::cli::parse_args(6, const_cast<char**>(plan_flag));
    check(parsed.error.ok() && parsed.options.agent_run && parsed.options.agent_plan &&
              parsed.options.prompt == "design the server" &&
              parsed.options.positional_url == "openrouter" &&
              ainiux::cli::validate_agent_run_arguments(6, const_cast<char**>(plan_flag),
                                                       parsed.options)
                  .ok(),
          "--plan accepts a positional provider");

    const char* plan_file[] = {"ainiux", "--plan-file", "goal.txt", "--provider",
                               "openai", "-m", "model"};
    parsed = ainiux::cli::parse_args(7, const_cast<char**>(plan_file));
    check(parsed.error.ok() && parsed.options.agent_run && parsed.options.agent_plan &&
              parsed.options.prompt_file == "goal.txt" &&
              ainiux::cli::validate_agent_run_arguments(7, const_cast<char**>(plan_file),
                                                       parsed.options)
                  .ok(),
          "--plan-file selects one-shot Plan mode");

    const char* mixed_task_modes[] = {"ainiux", "--run", "act goal", "--plan", "plan goal"};
    parsed = ainiux::cli::parse_args(5, const_cast<char**>(mixed_task_modes));
    check(parsed.error.ok() &&
              !ainiux::cli::validate_agent_run_arguments(
                   5, const_cast<char**>(mixed_task_modes), parsed.options)
                   .ok(),
          "one-shot Act and Plan entry forms cannot be combined");

    const char* run_no_index[] = {
        "ainiux", "--run", "goal", "--disable-indexing"};
    parsed =
        ainiux::cli::parse_args(4, const_cast<char**>(run_no_index));
    check(parsed.error.ok() && parsed.options.agent_run &&
              parsed.options.disable_indexing &&
              ainiux::cli::validate_disable_indexing_arguments(parsed.options)
                  .ok() &&
              ainiux::cli::validate_agent_run_arguments(
                  4, const_cast<char**>(run_no_index), parsed.options)
                  .ok(),
          "one-shot Run accepts indexing disable");

    const char* plan_no_index[] = {
        "ainiux", "--plan", "goal", "--disable-indexing"};
    parsed =
        ainiux::cli::parse_args(4, const_cast<char**>(plan_no_index));
    check(parsed.error.ok() && parsed.options.agent_plan &&
              parsed.options.disable_indexing &&
              ainiux::cli::validate_disable_indexing_arguments(parsed.options)
                  .ok(),
          "one-shot Plan accepts indexing disable");

    for (const std::vector<const char*>& rejected : {
             std::vector<const char*>{"ainiux", "--index-code",
                                      "--disable-indexing"},
             std::vector<const char*>{"ainiux", "--security-review",
                                      "--disable-indexing"},
             std::vector<const char*>{"ainiux", "--chat",
                                      "--disable-indexing"},
             std::vector<const char*>{"ainiux", "-p", "hello",
                                      "--disable-indexing"}}) {
        std::vector<char*> mutable_argv;
        for (const char* value : rejected)
            mutable_argv.push_back(const_cast<char*>(value));
        parsed = ainiux::cli::parse_args(
            static_cast<int>(mutable_argv.size()), mutable_argv.data());
        check(parsed.error.ok() &&
                  !ainiux::cli::validate_disable_indexing_arguments(
                       parsed.options)
                       .ok(),
              "--disable-indexing rejects unrelated mode");
    }

    const char* plan_positional_provider[] = {"ainiux", "plan", "goal", "openrouter"};
    parsed = ainiux::cli::parse_args(4, const_cast<char**>(plan_positional_provider));
    check(!parsed.error.ok(),
          "ainiux plan requires provider selection through named options");

    const char* missing[] = {"ainiux", "--run"};
    // --run without value should fail parse
    parsed = ainiux::cli::parse_args(2, const_cast<char**>(missing));
    check(!parsed.error.ok(), "--run without a goal value is a parse error");

    const char* no_goal[] = {"ainiux", "run", "-m", "model"};
    parsed = ainiux::cli::parse_args(4, const_cast<char**>(no_goal));
    check(parsed.error.ok() && parsed.options.agent_run &&
              !ainiux::cli::validate_agent_run_arguments(4, const_cast<char**>(no_goal),
                                                        parsed.options)
                   .ok(),
          "one-shot agent requires a goal via --run or --run-file");

    const char* with_p[] = {"ainiux", "-r", "goal", "-p", "other"};
    parsed = ainiux::cli::parse_args(5, const_cast<char**>(with_p));
    check(parsed.error.ok() && parsed.options.agent_run &&
              !ainiux::cli::validate_agent_run_arguments(5, const_cast<char**>(with_p),
                                                        parsed.options)
                   .ok(),
          "one-shot agent rejects combining --run with -p");

    const char* both_modes[] = {"ainiux", "--agent", "-r", "goal"};
    parsed = ainiux::cli::parse_args(4, const_cast<char**>(both_modes));
    check(parsed.error.ok() && parsed.options.agent && parsed.options.agent_run &&
              !ainiux::cli::validate_agent_run_arguments(4, const_cast<char**>(both_modes),
                                                        parsed.options)
                   .ok() &&
              !ainiux::cli::validate_agent_interactive_arguments(4, const_cast<char**>(both_modes),
                                                                parsed.options)
                   .ok(),
          "interactive --agent and one-shot --run cannot be combined");
}

}  // namespace

void test_cli_encoding_parse() {
    const char* argv[] = {"ainiux", "--input", "notes.txt", "--encoding", "cp1251"};
    ainiux::cli::ParseResult parsed = ainiux::cli::parse_args(5, const_cast<char**>(argv));
    check(parsed.error.ok() && parsed.options.input_encoding == "windows-1251",
          "--encoding cp1251 canonicalizes to windows-1251");
    const char* bad[] = {"ainiux", "--encoding", "gbk;rm"};
    parsed = ainiux::cli::parse_args(3, const_cast<char**>(bad));
    check(!parsed.error.ok() && parsed.error.code == ainiux::ErrorCode::UnsupportedFeature,
          "--encoding rejects unsafe names");
}

void test_cli_image_mode_parse() {
    const char* argv[] = {"ainiux",
                          "image",
                          "-p",
                          "a cat",
                          "--size",
                          "2k",
                          "--ar",
                          "16:9",
                          "--quality",
                          "high",
                          "--format",
                          "png",
                          "--attach",
                          "one.png",
                          "--attach",
                          "two.jpg",
                          "--output",
                          "out.png"};
    ainiux::cli::ParseResult parsed =
        ainiux::cli::parse_args(18, const_cast<char**>(argv));
    check(parsed.error.ok(), "image subcommand args parse");
    check(parsed.options.image, "image subcommand sets image mode");
    check(parsed.options.prompt == "a cat", "image prompt parsed");
    check(parsed.options.image_size == "2k", "image --size parsed");
    check(parsed.options.image_ar == "16:9", "image --ar parsed");
    check(parsed.options.image_quality == "high", "image --quality parsed");
    check(parsed.options.image_format == "png", "image --format png parsed");
    check(parsed.options.image_format_explicit, "image --format is explicit");
    check(parsed.options.attachment_paths.size() == 2, "image --attach is repeatable");
    check(parsed.options.output_path == "out.png", "image --output parsed");
    check(ainiux::cli::validate_image_mode_arguments(parsed.options).ok(),
          "valid image options pass validation");

    const char* debug_argv[] = {"ainiux", "--debug", "image", "-p", "a cube"};
    parsed = ainiux::cli::parse_args(5, const_cast<char**>(debug_argv));
    check(parsed.error.ok() && parsed.options.image && parsed.options.debug &&
              parsed.options.positional_url.empty() && parsed.options.prompt == "a cube",
          "ainiux --debug image is image mode, not a positional URL");

    const char* flag_argv[] = {"ainiux", "--image", "-p", "otter", "--format=webp", "--force"};
    parsed = ainiux::cli::parse_args(6, const_cast<char**>(flag_argv));
    check(parsed.error.ok() && parsed.options.image, "--image flag parses");
    check(parsed.options.image_format == "webp", "--format=webp in image mode");
    check(parsed.options.image_force, "--force parsed");

    const char* jpeg_argv[] = {"ainiux", "image", "-p", "x", "--format", "jpg"};
    parsed = ainiux::cli::parse_args(6, const_cast<char**>(jpeg_argv));
    check(parsed.error.ok() && parsed.options.image_format == "jpeg",
          "--format jpg normalizes to jpeg");

    const char* size_argv[] = {"ainiux", "image", "--size", "1536x1024", "--prompt", "x"};
    parsed = ainiux::cli::parse_args(6, const_cast<char**>(size_argv));
    check(parsed.error.ok() && parsed.options.image_size == "1536x1024",
          "WIDTHxHEIGHT --size parsed");

    const char* missing_prompt[] = {"ainiux", "image", "--size", "1k"};
    parsed = ainiux::cli::parse_args(4, const_cast<char**>(missing_prompt));
    check(parsed.error.ok(), "image without prompt still parses");
    check(!ainiux::cli::validate_image_mode_arguments(parsed.options).ok(),
          "image mode without prompt is rejected");

    const char* chat_format[] = {"ainiux", "image", "-p", "x", "--format", "json"};
    parsed = ainiux::cli::parse_args(6, const_cast<char**>(chat_format));
    check(parsed.error.ok(), "image --format json parses as chat format");
    check(!ainiux::cli::validate_image_mode_arguments(parsed.options).ok(),
          "image mode rejects chat --format json");

    const char* with_chat[] = {"ainiux", "image", "--chat", "-p", "x"};
    parsed = ainiux::cli::parse_args(5, const_cast<char**>(with_chat));
    check(parsed.error.ok(), "image+chat still parses");
    check(!ainiux::cli::validate_image_mode_arguments(parsed.options).ok(),
          "image mode rejects --chat");

    const char* size_only[] = {"ainiux", "-p", "hello", "--size", "1k"};
    parsed = ainiux::cli::parse_args(5, const_cast<char**>(size_only));
    check(parsed.error.ok(), "--size without image mode still parses");
    check(!ainiux::cli::validate_image_mode_arguments(parsed.options).ok(),
          "--size without image mode is rejected");

    const char* png_chat[] = {"ainiux", "-p", "hello", "--format", "png"};
    parsed = ainiux::cli::parse_args(5, const_cast<char**>(png_chat));
    check(parsed.error.ok() && parsed.options.image_format_explicit,
          "--format png is accepted at parse time");
    check(!ainiux::cli::validate_image_mode_arguments(parsed.options).ok(),
          "--format png without image mode is rejected");
}

void run_all() {
    test_cli_encoding_parse();
    test_cli_empty_and_unicode_edge_cases();
    test_cli_chat_nocolors_parse();
    test_cli_theme_parse();
    test_cli_color_mode_parse();
    test_cli_chat_parse();
    test_cli_context_token_parse();
    test_cli_code_index_parse();
    test_cli_security_review_parse();
    test_cli_agent_mode_parse();
    test_cli_editor_parse();
    test_cli_help_displays_version();
    test_cli_image_mode_parse();
    test_cli_web_search_parse();
    test_cli_agent_max_response_bytes_parse();
    test_cli_html_extract_parse();
    test_cli_output_format_parse();
    test_cli_parse();
    test_cli_provider_shortcut_parse();
    test_cli_rejects_unknown();
    test_cli_repl_parse();
    test_cli_responses_parse();
    test_cli_reasoning_parse();
    test_url_normalization();
}

}  // namespace ainiux::test::cli
