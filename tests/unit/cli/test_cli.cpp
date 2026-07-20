#include "cli/test_cli.hpp"
#include "support/test_support.hpp"
#include "cli/args.hpp"
#include "markdown/markdown.hpp"
#include "ainiux/version.hpp"
#include "editor/editor.hpp"
#include "provider/provider.hpp"
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

    const char* continue_argv[] = {"ainiux", "--editor",
                                   "--editor-continue-prefix-max-chars", "0",
                                   "--editor-continue-postfix-max-chars", "17",
                                   "--editor-continue-prose-prefix-max-chars", "23",
                                   "--editor-continue-prose-postfix-max-chars", "0",
                                   "--editor-continue-max-tokens", "2048"};
    parsed = ainiux::cli::parse_args(12, const_cast<char**>(continue_argv));
    check(parsed.error.ok(), "editor continue settings args parse");
    check(parsed.options.editor_ai_continue_prefix_max_chars == 0,
          "editor continue prefix accepts zero to disable prefix context");
    check(parsed.options.editor_ai_continue_postfix_max_chars == 17,
          "editor continue postfix character limit parses");
    check(parsed.options.editor_ai_continue_prose_prefix_max_chars == 23,
          "editor prose continue prefix character limit parses");
    check(parsed.options.editor_ai_continue_prose_postfix_max_chars == 0,
          "editor prose continue postfix accepts zero to disable postfix context");
    check(parsed.options.editor_ai_continue_max_tokens == 2048,
          "editor continue max tokens parsed");

    const char* old_argv[] = {"ainiux", "--editor-continue-read", "1"};
    parsed = ainiux::cli::parse_args(3, const_cast<char**>(old_argv));
    check(!parsed.error.ok(), "removed --editor-continue-read option is rejected");
    const char* overflow_argv[] = {
        "ainiux", "--editor-continue-prefix-max-chars", "999999999999999999999999999999"};
    parsed = ainiux::cli::parse_args(3, const_cast<char**>(overflow_argv));
    check(!parsed.error.ok() && parsed.error.message.find("too large") != std::string::npos,
          "overflowing editor continuation character limit is rejected");
    const char* prose_overflow_argv[] = {
        "ainiux", "--editor-continue-prose-postfix-max-chars",
        "999999999999999999999999999999"};
    parsed = ainiux::cli::parse_args(3, const_cast<char**>(prose_overflow_argv));
    check(!parsed.error.ok() && parsed.error.message.find("too large") != std::string::npos,
          "overflowing prose continuation character limit is rejected");
}

void test_cli_help_displays_version() {
    const std::string help = ainiux::cli::help_text();
    const std::string expected_heading =
        ainiux::app_version_label() + " - script-friendly OpenAI-compatible chat CLI";
    check(help.rfind(expected_heading, 0) == 0,
          "CLI help heading displays the current app version");
    check(help.find("openrouter --editor") != std::string::npos,
          "CLI help documents openrouter editor startup without model");
    check(help.find("choose a model inside the editor with /model") != std::string::npos,
          "CLI help documents deferred editor model selection");
    check(help.find("-c, --chat") != std::string::npos,
          "CLI help documents -c short option for --chat");
    check(help.find("-e, --editor") != std::string::npos,
          "CLI help documents -e short option for --editor");
    check(help.find("Mode:") != std::string::npos &&
              help.find("Prompt and generation:") != std::string::npos &&
              help.find("Provider and endpoint:") != std::string::npos &&
              help.find("Benchmark:") != std::string::npos,
          "CLI help groups options into logical sections");
    check(help.find("--editor-continue-prefix-max-chars") != std::string::npos &&
              help.find("--editor-continue-postfix-max-chars") != std::string::npos &&
              help.find("--editor-continue-prose-prefix-max-chars") != std::string::npos &&
              help.find("--editor-continue-prose-postfix-max-chars") != std::string::npos &&
              help.find("--editor-continue-read") == std::string::npos,
          "CLI help documents only the new editor continue context settings");
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
    const char* argv[] = {"ainiux", "agent", "openrouter", "-m", "model", "-p", "summarize the project"};
    ainiux::cli::ParseResult parsed = ainiux::cli::parse_args(7, const_cast<char**>(argv));
    check(parsed.error.ok() && parsed.options.agent && parsed.options.model == "model" &&
              parsed.options.prompt == "summarize the project" &&
              ainiux::cli::validate_agent_mode_arguments(7, const_cast<char**>(argv), parsed.options)
                  .ok(),
          "agent subcommand accepts provider, model, and goal prompt");

    const char* flag[] = {"ainiux", "--agent", "-m", "model", "-p", "goal", "--no-agent-log"};
    parsed = ainiux::cli::parse_args(7, const_cast<char**>(flag));
    check(parsed.error.ok() && parsed.options.agent && parsed.options.agent_log_cli_explicit &&
              !parsed.options.agent_log_enabled &&
              ainiux::cli::validate_agent_mode_arguments(7, const_cast<char**>(flag), parsed.options)
                  .ok(),
          "agent flag accepts logging disable override");

    const char* missing[] = {"ainiux", "--agent", "-m", "model"};
    parsed = ainiux::cli::parse_args(4, const_cast<char**>(missing));
    check(parsed.error.ok() && !ainiux::cli::validate_agent_mode_arguments(
                                   4, const_cast<char**>(missing), parsed.options).ok(),
          "agent mode requires a goal prompt");

    const char* system[] = {"ainiux", "--agent", "-m", "model", "-p", "goal", "-s", "override"};
    parsed = ainiux::cli::parse_args(8, const_cast<char**>(system));
    check(parsed.error.ok() && !ainiux::cli::validate_agent_mode_arguments(
                                   8, const_cast<char**>(system), parsed.options).ok(),
          "agent mode rejects user system prompt overrides");

    const char* combined[] = {"ainiux", "--agent", "--security-review", "-p", "goal"};
    parsed = ainiux::cli::parse_args(5, const_cast<char**>(combined));
    check(parsed.error.ok() && !ainiux::cli::validate_agent_mode_arguments(
                                   5, const_cast<char**>(combined), parsed.options).ok(),
          "agent mode rejects security-review combination");
}

}  // namespace

void run_all() {
    test_cli_empty_and_unicode_edge_cases();
    test_cli_chat_nocolors_parse();
    test_cli_chat_parse();
    test_cli_context_token_parse();
    test_cli_code_index_parse();
    test_cli_security_review_parse();
    test_cli_agent_mode_parse();
    test_cli_editor_parse();
    test_cli_help_displays_version();
    test_cli_web_search_parse();
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
