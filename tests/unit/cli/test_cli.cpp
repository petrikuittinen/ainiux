#include "cli/test_cli.hpp"
#include "support/test_support.hpp"
#include "cli/args.hpp"
#include "markdown/markdown.hpp"
#include "pkchat/version.hpp"
#include "provider/provider.hpp"
#include <string>

namespace pkchat::test::cli {

namespace {

using pkchat::test::check;
using pkchat::test::read_fixture;

void test_cli_chat_nocolors_parse() {
    const char* argv[] = {"pkchat", "--chat", "--nocolors", "lmstudio"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(4, const_cast<char**>(argv));
    check(parsed.error.ok(), "chat UI nocolors args parse");
    check(parsed.options.tui, "chat UI flag parsed with nocolors");
    check(parsed.options.no_colors, "nocolors flag parsed");
    check(parsed.options.positional_url == "lmstudio", "chat UI nocolors positional profile parsed");
}

void test_cli_chat_parse() {
    const char* argv[] = {"pkchat", "--chat", "lmstudio"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(3, const_cast<char**>(argv));
    check(parsed.error.ok(), "chat UI args parse");
    check(parsed.options.tui, "chat UI flag parsed");
    check(parsed.options.positional_url == "lmstudio", "chat UI positional profile parsed");

    const char* alias_argv[] = {"pkchat", "--tui", "lmstudio"};
    parsed = pkchat::cli::parse_args(3, const_cast<char**>(alias_argv));
    check(parsed.error.ok(), "legacy TUI alias args parse");
    check(parsed.options.tui, "legacy TUI alias flag parsed");
}

void test_cli_context_token_parse() {
    const char* binary_argv[] = {"pkchat", "--chat", "--context", "64k"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(4, const_cast<char**>(binary_argv));
    check(parsed.error.ok(), "binary-k context size parses");
    check(parsed.options.context_tokens == 65536, "64k context size equals 65536 tokens");

    const char* million_argv[] = {"pkchat", "--chat", "--context", "1M"};
    parsed = pkchat::cli::parse_args(4, const_cast<char**>(million_argv));
    check(parsed.error.ok(), "decimal-M context size parses");
    check(parsed.options.context_tokens == 1000000, "1M context size equals one million tokens");

    const char* zero_argv[] = {"pkchat", "--context", "0"};
    parsed = pkchat::cli::parse_args(3, const_cast<char**>(zero_argv));
    check(!parsed.error.ok() && parsed.error.code == pkchat::ErrorCode::BadArgs,
          "zero context size is rejected");

    const char* suffix_argv[] = {"pkchat", "--context", "64kb"};
    parsed = pkchat::cli::parse_args(3, const_cast<char**>(suffix_argv));
    check(!parsed.error.ok() && parsed.error.code == pkchat::ErrorCode::BadArgs,
          "unsupported context size suffix is rejected");

    const char* overflow_argv[] = {"pkchat", "--context", "999999999999999999999M"};
    parsed = pkchat::cli::parse_args(3, const_cast<char**>(overflow_argv));
    check(!parsed.error.ok() && parsed.error.code == pkchat::ErrorCode::BadArgs,
          "overflowing context size is rejected");
}

void test_cli_editor_parse() {
    const char* argv[] = {"pkchat", "--editor", "notes.txt", "--output", "saved.txt"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(5, const_cast<char**>(argv));
    check(parsed.error.ok(), "editor args parse");
    check(parsed.options.editor, "editor flag parsed");
    check(parsed.options.editor_path == "notes.txt", "editor file path parsed");
    check(parsed.options.positional_url.empty(), "editor file is not stored as positional URL");
    check(parsed.options.output_path == "saved.txt", "editor save-as output parsed");

    const char* provider_argv[] = {"pkchat", "lmstudio", "--editor", "notes.txt"};
    parsed = pkchat::cli::parse_args(4, const_cast<char**>(provider_argv));
    check(parsed.error.ok(), "editor args with provider shortcut parse");
    check(parsed.options.positional_url == "lmstudio", "editor provider shortcut parsed");
    check(parsed.options.editor_path == "notes.txt", "editor file with provider shortcut parsed");

    const char* url_argv[] = {"pkchat", "http://localhost:1234/v1", "--editor", "draft.md"};
    parsed = pkchat::cli::parse_args(4, const_cast<char**>(url_argv));
    check(parsed.error.ok(), "editor args with base URL parse");
    check(parsed.options.positional_url == "http://localhost:1234/v1", "editor base URL parsed");
    check(parsed.options.editor_path == "draft.md", "editor file with base URL parsed");

    const char* scratch_argv[] = {"pkchat", "--editor"};
    parsed = pkchat::cli::parse_args(2, const_cast<char**>(scratch_argv));
    check(parsed.error.ok(), "scratch editor args parse");
    check(parsed.options.editor, "scratch editor flag parsed");
    check(parsed.options.editor_path.empty(), "scratch editor has no file path");

    const char* eq_argv[] = {"pkchat", "--editor=notes.txt"};
    parsed = pkchat::cli::parse_args(2, const_cast<char**>(eq_argv));
    check(parsed.error.ok(), "editor equals-form args parse");
    check(parsed.options.editor_path == "notes.txt", "editor equals-form file path parsed");

    const char* provider_file_argv[] = {"pkchat", "openrouter", "--editor", "openrouter"};
    parsed = pkchat::cli::parse_args(4, const_cast<char**>(provider_file_argv));
    check(parsed.error.ok(), "editor file named like provider shortcut parses");
    check(parsed.options.positional_url == "openrouter", "provider shortcut stays positional");
    check(parsed.options.editor_path == "openrouter", "editor path named like provider stays on --editor");
}

void test_cli_help_displays_version() {
    const std::string expected_heading = std::string("pkchat ") + pkchat::kVersion +
                                         " - script-friendly OpenAI-compatible chat CLI";
    check(pkchat::cli::help_text().rfind(expected_heading, 0) == 0,
          "CLI help heading displays the current pkchat version");
}

void test_cli_html_extract_parse() {
    const char* argv[] = {"pkchat", "--fetch-url", "https://example.com/page", "--html-format", "markdown",
                          "--max-fetch-bytes", "123", "--allow-private-url-fetch", "--output", "page.md"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(10, const_cast<char**>(argv));
    check(parsed.error.ok(), "HTML fetch args parse");
    check(parsed.options.fetch_url == "https://example.com/page", "HTML fetch URL parsed");
    check(parsed.options.html_format == "markdown", "HTML output format parsed");
    check(parsed.options.max_fetch_bytes == 123, "HTML max fetch bytes parsed");
    check(parsed.options.allow_private_url_fetch, "HTML private fetch override parsed");
    check(parsed.options.output_path == "page.md", "HTML output path parsed");

    const char* file_argv[] = {"pkchat", "--input", "page.html", "--output-format", "plaintext"};
    parsed = pkchat::cli::parse_args(5, const_cast<char**>(file_argv));
    check(parsed.error.ok(), "input file args parse");
    check(parsed.options.input_path == "page.html", "input file path parsed");
    check(parsed.options.output_format == pkchat::markdown::OutputFormat::Plaintext, "input plaintext output format parsed");
    check(parsed.options.rendered_output_format_explicit, "input rendered output format marked explicit");

    const char* legacy_file_argv[] = {"pkchat", "--html-file", "page.html", "--html-format", "text"};
    parsed = pkchat::cli::parse_args(5, const_cast<char**>(legacy_file_argv));
    check(parsed.error.ok(), "legacy HTML file args parse");
    check(parsed.options.html_file == "page.html", "legacy HTML file path parsed");
    check(parsed.options.html_format == "text", "legacy HTML text format parsed");

    const char* image_argv[] = {"pkchat", "--input", "PHOTO.JPEG", "--max-image-bytes", "4096", "-p", "describe"};
    parsed = pkchat::cli::parse_args(7, const_cast<char**>(image_argv));
    check(parsed.error.ok(), "image input args parse");
    check(parsed.options.input_path == "PHOTO.JPEG", "image input path parsed");
    check(parsed.options.max_image_bytes == 4096, "image byte limit parsed");

    const char* attach_argv[] = {"pkchat", "-p", "compare", "--attach", "one.md", "--attach", "two.txt",
                                 "--max-input-bytes", "8192"};
    parsed = pkchat::cli::parse_args(9, const_cast<char**>(attach_argv));
    check(parsed.error.ok(), "repeatable attachment args parse");
    check(parsed.options.attachment_paths.size() == 2, "two attachment paths parsed");
    check(parsed.options.attachment_paths[0] == "one.md" && parsed.options.attachment_paths[1] == "two.txt",
          "attachment path order is preserved");
    check(parsed.options.max_input_bytes == 8192, "text input byte limit parsed");

    const char* context_argv[] = {"pkchat", "-p", "hello", "--context-policy", "summarize-middle",
                                  "--max-context-bytes", "4096", "--image-capability", "allow"};
    parsed = pkchat::cli::parse_args(9, const_cast<char**>(context_argv));
    check(parsed.error.ok(), "context and image capability args parse");
    check(parsed.options.context_policy == "summarize-middle", "context policy parsed");
    check(parsed.options.max_context_bytes == 4096, "context byte limit parsed");
    check(parsed.options.image_capability == "allow", "image capability override parsed");
}

void test_cli_output_format_parse() {
    const char* argv[] = {"pkchat", "-p", "hello", "--output-format", "html", "--output", "answer.html"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(7, const_cast<char**>(argv));
    check(parsed.error.ok(), "CLI output-format args parse");
    check(parsed.options.output_format == pkchat::markdown::OutputFormat::Html, "HTML output format parsed");
    check(parsed.options.output_format_explicit, "output-format explicit flag parsed");
    check(parsed.options.output_path == "answer.html", "output path parsed with output-format");

    const char* plain_argv[] = {"pkchat", "-p", "hello", "--output-format", "plaintext"};
    parsed = pkchat::cli::parse_args(5, const_cast<char**>(plain_argv));
    check(parsed.error.ok(), "CLI plaintext output-format args parse");
    check(parsed.options.output_format == pkchat::markdown::OutputFormat::Plaintext, "plaintext output format parsed");

    const char* json_argv[] = {"pkchat", "-p", "hello", "--output-format", "json"};
    parsed = pkchat::cli::parse_args(5, const_cast<char**>(json_argv));
    check(parsed.error.ok(), "CLI json output-format args parse");
    check(parsed.options.format == pkchat::cli::OutputFormat::Json, "json output-format maps to JSON format");
    check(!parsed.options.rendered_output_format_explicit, "json output-format is not a rendered text format");

    const char* jsond_argv[] = {"pkchat", "-p", "hello", "--output-format", "jsond"};
    parsed = pkchat::cli::parse_args(5, const_cast<char**>(jsond_argv));
    check(parsed.error.ok(), "CLI jsond output-format args parse");
    check(parsed.options.format == pkchat::cli::OutputFormat::Ndjson, "jsond output-format maps to NDJSON format");

    const char* bad_argv[] = {"pkchat", "-p", "hello", "--output-format", "pdf"};
    parsed = pkchat::cli::parse_args(5, const_cast<char**>(bad_argv));
    check(!parsed.error.ok(), "CLI rejects bad output-format");
}

void test_cli_parse() {
    const char* argv[] = {"pkchat", "http://localhost:8000", "-p", "hello", "--no-stream", "--format", "json", "-v", "--no-config", "--save-chat", "chat.json"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(11, const_cast<char**>(argv));
    check(parsed.error.ok(), "CLI parse should succeed");
    check(parsed.options.positional_url == "http://localhost:8000", "positional URL parsed");
    check(parsed.options.prompt == "hello", "prompt parsed");
    check(!parsed.options.stream, "no-stream parsed");
    check(parsed.options.format == pkchat::cli::OutputFormat::Json, "json format parsed");
    check(parsed.options.verbose, "verbose parsed");
    check(parsed.options.no_config, "no-config parsed");
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

void test_cli_rejects_unknown() {
    const char* argv[] = {"pkchat", "--bogus"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(2, const_cast<char**>(argv));
    check(!parsed.error.ok(), "unknown option rejected");
    check(parsed.error.code == pkchat::ErrorCode::BadArgs, "unknown option is bad args");
}

void test_cli_repl_parse() {
    const char* argv[] = {"pkchat", "--repl", "--load-chat", "chat.json"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(4, const_cast<char**>(argv));
    check(parsed.error.ok(), "REPL args parse");
    check(parsed.options.repl, "REPL flag parsed");
    check(parsed.options.load_chat_path == "chat.json", "load chat parsed");
}

void test_cli_responses_parse() {
    const char* argv[] = {"pkchat", "--responses", "-p", "hello"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(4, const_cast<char**>(argv));
    check(parsed.error.ok(), "Responses API shortcut args parse");
    check(parsed.options.api == "responses", "--responses selects Responses API");
}

void test_cli_empty_and_unicode_edge_cases() {
    const char* argv[] = {"pkchat"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(1, const_cast<char**>(argv));
    check(parsed.error.ok(), "CLI parse succeeds with only the program name");

    const char* empty_prompt_argv[] = {"pkchat", "-p", ""};
    parsed = pkchat::cli::parse_args(3, const_cast<char**>(empty_prompt_argv));
    check(parsed.error.ok() && parsed.options.prompt.empty(),
          "CLI parse accepts an empty prompt string");

    const std::string unicode_prompt = u8"مرحبا 你好 👨‍👩‍👧‍👦";
    const char* unicode_argv[] = {"pkchat", "-p", unicode_prompt.c_str()};
    parsed = pkchat::cli::parse_args(3, const_cast<char**>(unicode_argv));
    check(parsed.error.ok() && parsed.options.prompt == unicode_prompt,
          "CLI parse preserves Arabic, Chinese, and emoji prompt text");

    bool changed = false;
    pkchat::Error err;
    pkchat::provider::normalize_base_url("", &changed, err);
    check(!err.ok() && err.code == pkchat::ErrorCode::BadUrl,
          "base URL normalization rejects an empty URL");
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

}  // namespace

void run_all() {
    test_cli_empty_and_unicode_edge_cases();
    test_cli_chat_nocolors_parse();
    test_cli_chat_parse();
    test_cli_context_token_parse();
    test_cli_editor_parse();
    test_cli_help_displays_version();
    test_cli_html_extract_parse();
    test_cli_output_format_parse();
    test_cli_parse();
    test_cli_provider_shortcut_parse();
    test_cli_rejects_unknown();
    test_cli_repl_parse();
    test_cli_responses_parse();
    test_url_normalization();
}

}  // namespace pkchat::test::cli
