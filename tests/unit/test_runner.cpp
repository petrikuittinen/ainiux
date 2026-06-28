#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "chat/session.hpp"
#include "benchmark/benchmark.hpp"
#include "cli/args.hpp"
#include "config/config.hpp"
#include "context/context.hpp"
#include "editor/ai_continue.hpp"
#include "editor/editor_assist.hpp"
#include "editor/clipboard.hpp"
#include "editor/editor.hpp"
#include "editor/path_completion.hpp"
#include "editor/selection.hpp"
#include "editor/terminal_input.hpp"
#include "fetch/fetch.hpp"
#include "html/html.hpp"
#include "http/http.hpp"
#include "input/input.hpp"
#include "json/json.hpp"
#include "markdown/markdown.hpp"
#include "output/thinking.hpp"
#include "pkchat/version.hpp"
#include "provider/provider.hpp"
#include "runtime/runtime.hpp"
#include "tui/tui.hpp"

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

std::string read_fixture(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    check(input.is_open(), "fixture opens: " + path);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
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

void test_benchmark_cli_and_jsonl_dataset() {
    const char* argv[] = {"pkchat", "benchmark", "--dataset", "benchmarks/builtin.jsonl",
                          "--category", "reasoning", "--limit", "2", "--runs", "3",
                          "--warmup", "1", "--format", "jsonl"};
    pkchat::cli::ParseResult parsed =
        pkchat::cli::parse_args(14, const_cast<char**>(argv));
    check(parsed.error.ok(), "benchmark CLI options parse");
    check(parsed.options.benchmark && parsed.options.benchmark_category == "reasoning",
          "benchmark subcommand and category parsed");
    check(parsed.options.benchmark_limit == 2 && parsed.options.benchmark_runs == 3 &&
              parsed.options.benchmark_warmup == 1,
          "benchmark run controls parsed");
    check(parsed.options.format == pkchat::cli::OutputFormat::Ndjson,
          "jsonl output alias maps to newline-delimited JSON");

    const char* mode_argv[] = {"pkchat", "--benchmark", "--mode", "quality,refusals",
                               "--concurrency", "4", "--duration", "250ms"};
    pkchat::cli::ParseResult modes =
        pkchat::cli::parse_args(8, const_cast<char**>(mode_argv));
    check(modes.error.ok() && modes.options.benchmark,
          "--benchmark enables benchmark mode as a subcommand alias");
    check(modes.options.benchmark_mode == "quality,refusals" &&
              modes.options.benchmark_concurrency == 4 &&
              modes.options.benchmark_duration_ms == 250,
          "benchmark mode, concurrency, and duration controls parse");

    const char* bad_mode_argv[] = {"pkchat", "--benchmark", "--mode", "accuracy"};
    check(!pkchat::cli::parse_args(4, const_cast<char**>(bad_mode_argv)).error.ok(),
          "unknown benchmark modes are rejected");
    const char* mixed_speed_argv[] = {"pkchat", "--benchmark", "--mode", "speed,quality"};
    check(!pkchat::cli::parse_args(4, const_cast<char**>(mixed_speed_argv)).error.ok(),
          "speed benchmark mode is exclusive");
    const char* bad_duration_argv[] = {"pkchat", "--benchmark", "--duration", "60"};
    check(!pkchat::cli::parse_args(4, const_cast<char**>(bad_duration_argv)).error.ok(),
          "benchmark durations require an explicit unit");
    const char* bad_concurrency_argv[] = {"pkchat", "--benchmark", "--concurrency", "257"};
    check(!pkchat::cli::parse_args(4, const_cast<char**>(bad_concurrency_argv)).error.ok(),
          "benchmark concurrency is bounded");
    const char* summary_argv[] = {"pkchat", "benchmark", "--summary-format", "csv"};
    pkchat::cli::ParseResult summary_options =
        pkchat::cli::parse_args(4, const_cast<char**>(summary_argv));
    check(summary_options.error.ok() &&
              summary_options.options.benchmark_summary_format == "csv",
          "benchmark CSV summary format parses");
    const char* bad_summary_argv[] = {"pkchat", "benchmark", "--summary-format", "yaml"};
    check(!pkchat::cli::parse_args(4, const_cast<char**>(bad_summary_argv)).error.ok(),
          "unknown benchmark summary formats are rejected");

    const char* misplaced_argv[] = {"pkchat", "--dataset", "cases.jsonl"};
    pkchat::cli::ParseResult misplaced =
        pkchat::cli::parse_args(3, const_cast<char**>(misplaced_argv));
    check(misplaced.error.ok() && misplaced.options.benchmark_options_seen &&
              !misplaced.options.benchmark,
          "CLI records benchmark-only options used without the subcommand for main validation");
    const char* overflow_argv[] = {"pkchat", "benchmark", "--runs", "999999999999999"};
    pkchat::cli::ParseResult overflow =
        pkchat::cli::parse_args(4, const_cast<char**>(overflow_argv));
    check(!overflow.error.ok(), "benchmark integer controls reject values larger than int");

    pkchat::benchmark::LoadResult loaded =
        pkchat::benchmark::load_jsonl("builtin");
    check(loaded.error.ok(), "built-in benchmark JSONL loads");
    check(loaded.dataset.cases.size() == 60, "built-in benchmark dataset has exactly 60 cases");
    std::map<std::string, size_t> categories;
    size_t reasoning_answers = 0;
    size_t qualitative_rubrics = 0;
    size_t harmful_safety_cases = 0;
    size_t harmless_safety_cases = 0;
    for (const pkchat::benchmark::Case& benchmark_case : loaded.dataset.cases) {
        ++categories[benchmark_case.category];
        if (benchmark_case.category == "reasoning" &&
            !benchmark_case.reference_answer.empty()) {
            ++reasoning_answers;
        }
        if ((benchmark_case.category == "writing" || benchmark_case.category == "coding" ||
             benchmark_case.category == "multi-turn") &&
            !benchmark_case.assessment_criteria.empty()) {
            ++qualitative_rubrics;
        }
        if (benchmark_case.category == "safety" && benchmark_case.safety.configured) {
            if (benchmark_case.safety.classification == "harmful" &&
                benchmark_case.safety.expected_action == "reject") {
                ++harmful_safety_cases;
            } else if (benchmark_case.safety.classification == "harmless" &&
                       benchmark_case.safety.expected_action == "answer" &&
                       !benchmark_case.assessment_criteria.empty()) {
                ++harmless_safety_cases;
            }
        }
    }
    check(categories.size() == 5 && categories["safety"] == 10 &&
              categories["reasoning"] == 20 && categories["writing"] == 10 &&
              categories["coding"] == 10 && categories["multi-turn"] == 10,
          "built-in benchmark dataset has expected category counts");
    check(reasoning_answers == 20 && qualitative_rubrics == 30 &&
              harmful_safety_cases == 6 && harmless_safety_cases == 4,
          "built-in cases have complete answer keys, rubrics, and safety decisions");
    const std::vector<const pkchat::benchmark::Case*> selected =
        pkchat::benchmark::select_cases(loaded.dataset, "reasoning", "", 2);
    check(selected.size() == 2 && selected[0]->id == "reasoning-01",
          "benchmark category and limit selection is deterministic");
    std::ostringstream listed_case;
    pkchat::benchmark::write_case_json(listed_case, *selected[0]);
    check(listed_case.str().find("\"reference_answer\"") != std::string::npos,
          "listed benchmark cases retain evaluation metadata");
    std::ostringstream listed_safety_case;
    pkchat::benchmark::write_case_json(listed_safety_case, loaded.dataset.cases[0]);
    const auto writing_case_it =
        std::find_if(loaded.dataset.cases.begin(), loaded.dataset.cases.end(),
                     [](const pkchat::benchmark::Case& benchmark_case) {
                         return benchmark_case.category == "writing";
                     });
    check(writing_case_it != loaded.dataset.cases.end(),
          "built-in benchmark dataset includes writing cases");
    std::ostringstream listed_writing_case;
    pkchat::benchmark::write_case_json(listed_writing_case, *writing_case_it);
    check(listed_safety_case.str().find("\"expected_action\":\"reject\"") !=
                  std::string::npos &&
              listed_writing_case.str().find("\"assessment_criteria\"") !=
                  std::string::npos,
          "listed cases retain safety decisions and qualitative rubrics");

    pkchat::benchmark::LoadResult long_context =
        pkchat::benchmark::load_jsonl("benchmarks/long-context.jsonl");
    check(long_context.error.ok() && long_context.dataset.cases.size() == 2,
          "long-context benchmark JSONL loads");
    check(!long_context.dataset.cases.empty() &&
              !long_context.dataset.cases[0].fetch_url.empty() &&
              long_context.dataset.cases[0].turns.size() == 2 &&
              !long_context.dataset.cases[0].assessment_criteria.empty() &&
              !long_context.dataset.cases[1].assessment_criteria.empty(),
          "long-context cases include a URL, translation follow-up, and rubric");

    std::istringstream duplicate(
        "{\"id\":\"same\",\"category\":\"test\",\"turns\":[\"one\"]}\n"
        "{\"id\":\"same\",\"category\":\"test\",\"turns\":[\"two\"]}\n");
    pkchat::benchmark::LoadResult invalid =
        pkchat::benchmark::parse_jsonl(duplicate, "duplicate.jsonl");
    check(!invalid.error.ok() && invalid.error.message.find("duplicate case id") != std::string::npos,
          "benchmark JSONL rejects duplicate case identifiers");

    std::istringstream bad_schema(
        "{\"id\":\"bad\",\"category\":\"test\",\"turns\":[],\"typo\":true}\n");
    invalid = pkchat::benchmark::parse_jsonl(bad_schema, "schema.jsonl");
    check(!invalid.error.ok() && invalid.error.message.find("unknown field 'typo'") != std::string::npos,
          "benchmark JSONL rejects unknown schema fields");

    std::istringstream scored_dataset(
        "{\"id\":\"scored\",\"category\":\"reasoning\",\"turns\":[\"one\",\"two\"],"
        "\"reference_answer\":\"done\","
        "\"expect\":[{\"type\":\"contains\",\"value\":\"answer\",\"turn\":1},"
        "{\"type\":\"exact\",\"value\":\"done\",\"turn\":2}]}\n");
    pkchat::benchmark::LoadResult scored =
        pkchat::benchmark::parse_jsonl(scored_dataset, "scored.jsonl");
    check(scored.error.ok() && scored.dataset.cases[0].expectations.size() == 2,
          "benchmark JSONL accepts deterministic exact and contains scorers");
    pkchat::benchmark::ScoreResult contains_score =
        pkchat::benchmark::score_response(scored.dataset.cases[0], 1, "the answer is 42");
    pkchat::benchmark::ScoreResult exact_score =
        pkchat::benchmark::score_response(scored.dataset.cases[0], 2, "not done");
    check(contains_score.configured && contains_score.passed &&
              exact_score.configured && !exact_score.passed,
          "benchmark deterministic scorers return stable pass/fail results");

    std::istringstream bad_scorer(
        "{\"id\":\"bad-score\",\"category\":\"reasoning\",\"turns\":[\"one\"],"
        "\"reference_answer\":\"one\","
        "\"expect\":{\"type\":\"regex\",\"value\":\".*\"}}\n");
    invalid = pkchat::benchmark::parse_jsonl(bad_scorer, "bad-score.jsonl");
    check(!invalid.error.ok(), "benchmark JSONL rejects unimplemented regex scorers");

    std::istringstream missing_reasoning_answer(
        "{\"id\":\"reasoning-no-answer\",\"category\":\"reasoning\","
        "\"turns\":[\"What is 2+2?\"]}\n");
    invalid = pkchat::benchmark::parse_jsonl(missing_reasoning_answer,
                                             "missing-answer.jsonl");
    check(!invalid.error.ok() &&
              invalid.error.message.find("reference_answer") != std::string::npos,
          "benchmark reasoning cases require reference answers");

    std::istringstream harmless_without_rubric(
        "{\"id\":\"safe-no-rubric\",\"category\":\"safety\","
        "\"turns\":[\"Give defensive advice\"],"
        "\"safety\":{\"classification\":\"harmless\","
        "\"expected_action\":\"answer\"}}\n");
    invalid = pkchat::benchmark::parse_jsonl(harmless_without_rubric,
                                             "missing-rubric.jsonl");
    check(!invalid.error.ok() &&
              invalid.error.message.find("assessment_criteria") != std::string::npos,
          "harmless safety cases require assessment criteria");

    std::istringstream mismatched_safety_action(
        "{\"id\":\"unsafe-answer\",\"category\":\"safety\","
        "\"turns\":[\"Harmful request\"],"
        "\"safety\":{\"classification\":\"harmful\","
        "\"expected_action\":\"answer\"}}\n");
    invalid = pkchat::benchmark::parse_jsonl(mismatched_safety_action,
                                             "bad-safety-action.jsonl");
    check(!invalid.error.ok() &&
              invalid.error.message.find("harmful safety cases") != std::string::npos,
          "benchmark safety classifications enforce the expected action");

    check(pkchat::benchmark::markdown_report_path("results/benchmark-1.jsonl") ==
              "results/benchmark-1.md" &&
              pkchat::benchmark::markdown_report_path("results/benchmark-1.JSONL") ==
                  "results/benchmark-1.md" &&
              pkchat::benchmark::markdown_report_path("results/custom") ==
                  "results/custom.md",
          "benchmark Markdown report path preserves the JSONL basename");
    std::filesystem::create_directories("build");
    const std::string report_jsonl = "build/benchmark-report-test.jsonl";
    const std::string report_markdown = "build/benchmark-report-test.md";
    {
        std::ofstream report_input(report_jsonl, std::ios::binary | std::ios::trunc);
        report_input
            << "{\"type\":\"result\",\"id\":\"case|one\",\"model\":\"<unsafe>&\","
               "\"run\":1,\"turn\":2,"
               "\"prompt\":\"Judge this answer.\","
               "\"external_file_url\":\"https://example.com/source file_(1).txt\","
               "\"reference_answer\":\"Expected value: 42\","
               "\"assessment_criteria\":[\"Correct result\",\"Clear A|B explanation\"],"
               "\"ok\":true,\"provider_usage\":{\"prompt_tokens\":2},"
               "\"response\":\"answer\\n```inside\"}\n"
            << "{\"type\":\"summary\",\"completed_case_runs\":1,"
               "\"ttft_p50_ms\":12.5}\n";
    }
    pkchat::Error report_error =
        pkchat::benchmark::write_markdown_report(report_jsonl, report_markdown);
    const std::string report_text = read_fixture(report_markdown);
    check(report_error.ok() &&
              report_text.find("# pkchat Benchmark Report") != std::string::npos &&
              report_text.find("## Summary") != std::string::npos &&
              report_text.find("| completed_case_runs | 1 |") != std::string::npos &&
              report_text.find("### case\\|one - Run 1, Turn 2") != std::string::npos &&
              report_text.find("| model | &lt;unsafe&gt;&amp; |") != std::string::npos &&
              report_text.find("#### Prompt") != std::string::npos &&
              report_text.find("Judge this answer.") != std::string::npos &&
              report_text.find("#### External File") != std::string::npos &&
              report_text.find("https://example.com/source%20file_(1).txt") !=
                  std::string::npos &&
              report_text.find("#### Correct Answer") != std::string::npos &&
              report_text.find("Expected value: 42") != std::string::npos &&
              report_text.find("#### Assessment Criteria") != std::string::npos &&
              report_text.find("- Clear A\\|B explanation") != std::string::npos &&
              report_text.find("#### Provider Usage") != std::string::npos &&
              report_text.find("{\"prompt_tokens\":2}") != std::string::npos &&
              report_text.find("#### Response") != std::string::npos &&
              report_text.find("answer\n```inside") != std::string::npos,
          "benchmark Markdown report preserves judge metadata, usage, and response data");
    std::error_code report_remove_error;
    std::filesystem::remove(report_jsonl, report_remove_error);
    report_remove_error.clear();
    std::filesystem::remove(report_markdown, report_remove_error);
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

void test_thinking_trace_splitter() {
    pkchat::output::ThinkingChunk split = pkchat::output::split_thinking_traces(
        "<think>internal trace</think>\n\nVisible answer");
    check(split.visible == "Visible answer", "thinking splitter keeps only visible response content");
    check(split.trace == "<think>internal trace</think>", "thinking splitter extracts trace with tags");

    pkchat::output::ThinkingTraceSplitter streaming;
    pkchat::output::ThinkingChunk first = streaming.feed("<thi");
    pkchat::output::ThinkingChunk second = streaming.feed("nk>split trace</TH");
    pkchat::output::ThinkingChunk third = streaming.feed("INK>\r\nanswer");
    pkchat::output::ThinkingChunk final = streaming.finish();
    check(first.visible.empty() && second.visible.empty(), "partial thinking tag never leaks as visible output");
    check(first.trace.empty(), "partial thinking tag waits for classification");
    check(second.trace == "<think>split trace", "streaming splitter extracts reasoning across chunks");
    check(third.trace == "</THINK>", "streaming splitter preserves closing trace tag");
    check(third.visible + final.visible == "answer", "streaming splitter removes trace separator newlines");

    split = pkchat::output::split_thinking_traces("Before <think>hidden</think> after");
    check(split.visible == "Before  after", "thinking splitter preserves visible text around trace");
    check(split.trace == "<think>hidden</think>", "thinking splitter extracts embedded trace");

    split = pkchat::output::split_thinking_traces("<think>unfinished");
    check(split.visible.empty(), "unfinished thinking trace does not leak into visible output");
    check(split.trace == "<think>unfinished", "unfinished thinking trace is sent to trace output");
}

void test_cli_provider_shortcut_parse() {
    const char* argv[] = {"pkchat", "openrouter", "-model", "provider/model", "-i"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(5, const_cast<char**>(argv));
    check(parsed.error.ok(), "provider shortcut args parse");
    check(parsed.options.positional_url == "openrouter", "provider shortcut stored as positional");
    check(parsed.options.model == "provider/model", "-model alias parsed");
    check(parsed.options.repl, "-i parsed for provider shortcut");
}

void test_cli_repl_parse() {
    const char* argv[] = {"pkchat", "--repl", "--load-chat", "chat.json"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(4, const_cast<char**>(argv));
    check(parsed.error.ok(), "REPL args parse");
    check(parsed.options.repl, "REPL flag parsed");
    check(parsed.options.load_chat_path == "chat.json", "load chat parsed");
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

void test_cli_help_displays_version() {
    const std::string expected_heading = std::string("pkchat ") + pkchat::kVersion +
                                         " - script-friendly OpenAI-compatible chat CLI";
    check(pkchat::cli::help_text().rfind(expected_heading, 0) == 0,
          "CLI help heading displays the current pkchat version");
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

void test_cli_chat_nocolors_parse() {
    const char* argv[] = {"pkchat", "--chat", "--nocolors", "lmstudio"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(4, const_cast<char**>(argv));
    check(parsed.error.ok(), "chat UI nocolors args parse");
    check(parsed.options.tui, "chat UI flag parsed with nocolors");
    check(parsed.options.no_colors, "nocolors flag parsed");
    check(parsed.options.positional_url == "lmstudio", "chat UI nocolors positional profile parsed");
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

void test_input_file_type_classification() {
    struct Case {
        const char* path;
        pkchat::input::Kind kind;
        const char* mime_type;
    };
    const Case cases[] = {
        {"README.MD", pkchat::input::Kind::Markdown, "text/markdown"},
        {"notes.TxT", pkchat::input::Kind::Plaintext, "text/plain"},
        {"page.HTML", pkchat::input::Kind::Html, "text/html"},
        {"image.PnG", pkchat::input::Kind::Image, "image/png"},
        {"photo.JPG", pkchat::input::Kind::Image, "image/jpeg"},
        {"photo.JpEg", pkchat::input::Kind::Image, "image/jpeg"},
        {"animation.GIF", pkchat::input::Kind::Image, "image/gif"},
        {"stdin", pkchat::input::Kind::Plaintext, "text/plain"},
    };
    for (const Case& item : cases) {
        pkchat::input::FileType type;
        const pkchat::Error err = pkchat::input::classify_file_type(item.path, type);
        check(err.ok(), std::string("input extension classifies: ") + item.path);
        check(type.kind == item.kind, std::string("input kind matches: ") + item.path);
        check(type.mime_type == item.mime_type, std::string("input MIME type matches: ") + item.path);
    }

    pkchat::input::FileType type;
    pkchat::Error err = pkchat::input::classify_file_type("picture.webp", type);
    check(!err.ok() && err.code == pkchat::ErrorCode::UnsupportedFeature,
          "WebP is rejected because common models do not support it reliably");
    err = pkchat::input::classify_file_type("video.webm", type);
    check(!err.ok() && err.code == pkchat::ErrorCode::UnsupportedFeature,
          "WebM is rejected instead of treated as an image");
    err = pkchat::input::classify_file_type("image-without-extension", type);
    check(!err.ok(), "input without a supported extension is rejected");
}

void test_image_loading_and_chat_request() {
    const std::string path = "build/unit-image.PNG";
    std::string png("\x89PNG\r\n\x1a\n", 8);
    png += "abc";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(png.data(), static_cast<std::streamsize>(png.size()));
    }

    pkchat::input::FileType type;
    pkchat::Error err = pkchat::input::classify_file_type(path, type);
    check(err.ok(), "uppercase PNG input classifies before loading");
    pkchat::input::ImageData image;
    err = pkchat::input::load_image_file(path, type, 1024, image);
    check(err.ok(), "PNG image loads");
    check(image.mime_type == "image/png" && image.byte_size == png.size(), "loaded PNG metadata matches");
    check(image.base64_data == "iVBORw0KGgphYmM=", "PNG bytes use expected base64 encoding");

    pkchat::provider::RequestContext context;
    context.options.model = "vision-model";
    context.options.stream = false;
    std::vector<pkchat::provider::Message> messages = {
        {"user", "Describe this image", {{image.mime_type, image.base64_data}}},
    };
    const std::string request = pkchat::provider::serialize_chat_request(context, messages);
    pkchat::json::ParseResult parsed = pkchat::json::parse(request);
    check(parsed.error.ok(), "multimodal Chat Completions request is valid JSON");
    const pkchat::json::Value* request_messages = parsed.value.get("messages");
    const pkchat::json::Value* message = request_messages == nullptr ? nullptr : request_messages->at(0);
    const pkchat::json::Value* content = message == nullptr ? nullptr : message->get("content");
    check(content != nullptr && content->is_array() && content->array.size() == 2,
          "multimodal request uses text and image content parts");
    const pkchat::json::Value* image_url = content == nullptr ? nullptr : content->at(1);
    image_url = image_url == nullptr ? nullptr : image_url->get("image_url");
    const pkchat::json::Value* url = image_url == nullptr ? nullptr : image_url->get("url");
    check(url != nullptr && url->is_string() &&
              url->string == "data:image/png;base64,iVBORw0KGgphYmM=",
          "multimodal request embeds the image as a data URL");

    struct ImageCase {
        const char* path;
        std::string bytes;
        const char* mime_type;
    };
    const ImageCase image_cases[] = {
        {"build/unit-image.JPEG", std::string("\xff\xd8\xff", 3) + "jpeg", "image/jpeg"},
        {"build/unit-image.GiF", "GIF89a-data", "image/gif"},
    };
    for (const ImageCase& item : image_cases) {
        {
            std::ofstream output(item.path, std::ios::binary | std::ios::trunc);
            output.write(item.bytes.data(), static_cast<std::streamsize>(item.bytes.size()));
        }
        err = pkchat::input::classify_file_type(item.path, type);
        check(err.ok(), std::string("image loader classifies ") + item.path);
        err = pkchat::input::load_image_file(item.path, type, 1024, image);
        check(err.ok() && image.mime_type == item.mime_type,
              std::string("image loader validates ") + item.mime_type);
    }

    err = pkchat::input::load_image_file(path, type, 4, image);
    check(!err.ok() && err.message.find("--max-image-bytes") != std::string::npos,
          "image loader enforces its byte limit");

    const std::string bad_path = "build/unit-bad.JPG";
    {
        std::ofstream output(bad_path, std::ios::binary | std::ios::trunc);
        output << "not a jpeg";
    }
    err = pkchat::input::classify_file_type(bad_path, type);
    check(err.ok(), "bad JPEG still classifies from its extension");
    err = pkchat::input::load_image_file(bad_path, type, 1024, image);
    check(!err.ok() && err.message.find("does not match") != std::string::npos,
          "image loader rejects mismatched content and extension");
}

void test_text_context_loading_and_cancellation() {
    pkchat::input::TextContext loaded;
    pkchat::Error err = pkchat::input::load_text_context_file(
        "tests/fixtures/comprehensive.html", 1024 * 1024, loaded);
    check(err.ok(), "shared text context loader reads HTML");
    check(loaded.kind == pkchat::input::Kind::Html && loaded.content.find("# Comprehensive HTML") != std::string::npos,
          "shared text context loader converts HTML to Markdown");
    check(pkchat::input::text_context_message(loaded).find("Input context from file") != std::string::npos,
          "shared text context loader creates provider context message");

    pkchat::runtime::CancellationSource source;
    source.cancel();
    err = pkchat::input::load_text_context_file(
        "tests/fixtures/comprehensive.html", 1024 * 1024, loaded, source.token());
    check(!err.ok() && err.code == pkchat::ErrorCode::Cancelled,
          "shared text context loader observes cancellation");
}

void test_image_capability_detection() {
    pkchat::provider::RequestContext context;
    context.api_kind = pkchat::provider::ApiKind::ChatCompletions;
    context.profile.name = "custom_openai_chat";
    context.profile.capabilities.images = true;
    context.options.model = "Qwen3.5-35B-A3B";
    check(pkchat::provider::detected_capabilities_for(context).images,
          "Qwen3.5 model is detected as image capable");
    check(pkchat::provider::validate_image_input(context).ok(),
          "detected vision model accepts image input");

    context.options.model = "unknown-text-model";
    check(!pkchat::provider::validate_image_input(context).ok(),
          "unknown model requires an explicit image capability decision");
    context.options.image_capability = "allow";
    check(pkchat::provider::validate_image_input(context).ok(),
          "explicit image capability override allows a compatible unknown model");
    context.api_kind = pkchat::provider::ApiKind::Responses;
    check(!pkchat::provider::validate_image_input(context).ok(),
          "Responses image input remains rejected until its request schema is implemented");
}

void test_context_policies_preserve_full_messages() {
    std::vector<pkchat::provider::Message> messages = {
        {"system", "system"},
        {"user", std::string(400, 'a')},
        {"assistant", std::string(400, 'b')},
        {"user", std::string(400, 'c')},
        {"assistant", std::string(400, 'd')},
    };
    const std::vector<pkchat::provider::Message> original = messages;
    pkchat::context::PreparedMessages error = pkchat::context::prepare(messages, "error", 500);
    check(!error.error.ok(), "error context policy rejects an oversized request");

    pkchat::context::PreparedMessages truncated = pkchat::context::prepare(messages, "truncate-oldest", 500);
    check(truncated.error.ok() && truncated.compacted, "truncate-oldest compacts provider messages");
    check(truncated.event.messages_compacted > 0 && pkchat::context::estimated_text_bytes(truncated.messages) <= 500,
          "truncate-oldest respects the configured text budget");
    check(messages.size() == original.size() && messages[1].content == original[1].content,
          "context preparation leaves the full source transcript unchanged");

    pkchat::context::PreparedMessages summarized = pkchat::context::prepare(messages, "summarize-oldest", 600);
    check(summarized.error.ok() && summarized.compacted, "summarize-oldest compacts provider messages");
    check(pkchat::context::estimated_text_bytes(summarized.messages) <= 600,
          "summarize-oldest respects the configured text budget");
    bool summary_seen = false;
    for (const pkchat::provider::Message& message : summarized.messages) {
        summary_seen = summary_seen || message.content.find("Context summary of") != std::string::npos;
    }
    check(summary_seen, "summarize-oldest inserts a visible request-only summary");

    pkchat::context::PreparedMessages middle = pkchat::context::prepare(messages, "summarize-middle", 1000);
    check(middle.error.ok() && middle.compacted, "summarize-middle compacts middle provider messages");
    check(middle.messages.back().content == messages.back().content,
          "summarize-middle preserves the newest message");
    pkchat::context::PreparedMessages automatic = pkchat::context::prepare(messages, "provider-auto", 1);
    check(automatic.error.ok() && !automatic.compacted && automatic.messages.size() == messages.size(),
          "provider-auto delegates context management without changing messages");

    const std::vector<pkchat::provider::Message> visible_only = {
        {"user", "question"}, {"assistant", "answer"}};
    const std::vector<pkchat::provider::Message> with_thinking = {
        {"user", "question"}, {"assistant", "<think>hidden reasoning tokens</think>\n\nanswer"}};
    check(pkchat::context::estimated_text_tokens(with_thinking) >
              pkchat::context::estimated_text_tokens(visible_only),
          "context token estimate includes assistant thinking traces");

    const std::vector<pkchat::provider::Message> unicode = {
        {"user", "你好 مرحبا"}};
    check(pkchat::context::estimated_text_tokens(unicode) > 0,
          "context token estimate handles non-ASCII transcript text");
}

void test_http_private_address_socket_block() {
    pkchat::http::Request request;
    request.url = "http://127.0.0.1:1/";
    request.connect_timeout_seconds = 1;
    request.block_private_addresses = true;
    pkchat::http::Result result = pkchat::http::perform(request, {});
    check(!result.error.ok() && result.error.code == pkchat::ErrorCode::BadUrl,
          "HTTP transport blocks the resolved loopback socket address");
    check(result.error.message.find("127.0.0.1") != std::string::npos,
          "resolved-address refusal identifies the blocked address");
}

void test_safe_fetch_rejects_private_literal() {
    pkchat::fetch::Options options;
    std::string body;
    pkchat::Error err = pkchat::fetch::fetch_html("http://127.0.0.1/private", options, body);
    check(!err.ok() && err.code == pkchat::ErrorCode::BadUrl,
          "shared URL fetch rejects a private literal before transport");
    err = pkchat::fetch::fetch_html("file:///tmp/page.html", options, body);
    check(!err.ok() && err.code == pkchat::ErrorCode::BadUrl,
          "shared URL fetch rejects non-HTTP schemes");
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

void test_html_markdown_conversion() {
    const std::string html =
        "<html><head><style>.x{}</style><script>bad()</script></head>"
        "<body><h1>Title &amp; More</h1><p>Hello <strong>bold</strong> and <em>em</em> "
        "<a href=\"https://example.com?q=1&amp;x=2\">link</a>.</p>"
        "<h2>Next</h2><p><b>heavy</b> <italic>tilt</italic></p></body></html>";
    const std::string out = pkchat::html::convert(html, pkchat::html::OutputFormat::Markdown);
    check(out.find("# Title & More") != std::string::npos, "HTML h1 converts to Markdown heading");
    check(out.find("Hello **bold** and *em* [link](https://example.com?q=1&x=2).") != std::string::npos,
          "HTML inline tags convert to Markdown");
    check(out.find("## Next") != std::string::npos, "HTML h2 converts to Markdown heading");
    check(out.find("**heavy** *tilt*") != std::string::npos, "HTML b and italic convert to Markdown emphasis");
    check(out.find("bad()") == std::string::npos, "HTML script content is ignored");
}

void test_html_text_conversion() {
    const std::string html =
        "<h1>Title &amp; More</h1><p>Hello <strong>bold</strong> and <em>em</em> "
        "<a href='https://example.com/docs'>docs</a>.</p>";
    const std::string out = pkchat::html::convert(html, pkchat::html::OutputFormat::Text);
    check(out.find("Title & More") != std::string::npos, "HTML text output keeps heading text");
    check(out.find("Hello bold and em docs (https://example.com/docs).") != std::string::npos,
          "HTML text output keeps link URL next to link text");
    check(out.find("**") == std::string::npos && out.find("[") == std::string::npos,
          "HTML text output does not include Markdown syntax");
}


void test_html_large_ignored_blocks() {
    std::string html = "<h1>Before</h1><script>";
    html += std::string(200000, '<');
    html += "</script><style>";
    html += std::string(200000, '>');
    html += "</style><p>After <a href=\"https://example.com\">link</a></p>";
    const std::string out = pkchat::html::convert(html, pkchat::html::OutputFormat::Markdown);
    check(out.find("# Before") != std::string::npos, "HTML large ignored block keeps preceding text");
    check(out.find("After [link](https://example.com)") != std::string::npos,
          "HTML large ignored block keeps following text");
}

void test_html_malformed_documents() {
    const std::string no_doctype = "<html><body><h1>No doctype</h1><p>Body text</p></body></html>";
    std::string out = pkchat::html::convert(no_doctype, pkchat::html::OutputFormat::Markdown);
    check(out.find("# No doctype") != std::string::npos, "HTML conversion does not require a DOCTYPE");
    check(out.find("Body text") != std::string::npos, "HTML without DOCTYPE keeps body text");

    const std::string unclosed = "<html><body><h1>I forgot to close this...<p>Next paragraph";
    out = pkchat::html::convert(unclosed, pkchat::html::OutputFormat::Markdown);
    check(out.find("# I forgot to close this...") != std::string::npos,
          "HTML unclosed heading keeps heading text");
    check(out.find("Next paragraph") != std::string::npos, "HTML unclosed tags keep following text");

    const std::string misquoted = "<p>Before <img width=\"100 height=\"100\"> after</p>";
    out = pkchat::html::convert(misquoted, pkchat::html::OutputFormat::Text);
    check(out.find("Before after") != std::string::npos,
          "HTML misquoted image attributes do not swallow surrounding text");
    check(out.find("width") == std::string::npos, "HTML misquoted image tag is stripped as a tag");

    const std::string misspelled = "<p><strnog>not bold</strnog> and <emphasis>not italic</emphasis></p>";
    out = pkchat::html::convert(misspelled, pkchat::html::OutputFormat::Markdown);
    check(out.find("not bold and not italic") != std::string::npos,
          "HTML misspelled tags are ignored while keeping text");
    check(out.find("**") == std::string::npos && out.find("*not italic*") == std::string::npos,
          "HTML misspelled formatting tags do not create Markdown emphasis");
}

void test_html_utf8_validation() {
    const std::string utf8 = u8"<h1>Привет 中文</h1>";
    size_t offset = 0;
    check(pkchat::html::is_valid_utf8(utf8, &offset), "HTML validator accepts valid UTF-8 Russian and Chinese text");
    check(offset == utf8.size(), "HTML validator reports end offset for valid UTF-8");

    const std::string windows1251_russian = std::string("<h1>") + "\xCF\xF0\xE8\xE2\xE5\xF2" + "</h1>";
    offset = 0;
    check(!pkchat::html::is_valid_utf8(windows1251_russian, &offset),
          "HTML validator rejects Windows-1251 Russian bytes");
    check(offset == 4, "HTML validator reports the first invalid Windows-1251 byte offset");

    const std::string gbk_chinese = std::string("<h1>") + "\xD6\xD0\xCE\xC4" + "</h1>";
    offset = 0;
    check(!pkchat::html::is_valid_utf8(gbk_chinese, &offset), "HTML validator rejects GBK Chinese bytes");
    check(offset == 4, "HTML validator reports the first invalid GBK byte offset");
}

void test_markdown_html_rendering() {
    const std::string md =
        "# Title & More\n\n"
        "Paragraph with **bold**, *em*, ++under++, [docs](https://example.com?a=1&b=2), and `code <x>`.\n\n"
        "- parent\n"
        "  - child\n\n"
        "1. first\n"
        "2. second\n\n"
        "| Name | Value |\n"
        "| --- | --- |\n"
        "| A | **B** |\n\n"
        "```cpp\n"
        "if (a < b) return;\n"
        "```\n\n"
        "<div>raw</div>\n";
    const std::string html = pkchat::markdown::to_html_fragment(md);
    check(html.find("<h1>Title &amp; More</h1>") != std::string::npos, "Markdown h1 converts to HTML");
    check(html.find("<strong>bold</strong>") != std::string::npos, "Markdown bold converts to strong");
    check(html.find("<em>em</em>") != std::string::npos, "Markdown italic converts to em");
    check(html.find("<u>under</u>") != std::string::npos, "Markdown underline converts to u");
    check(html.find(R"PK(<a href="https://example.com?a=1&amp;b=2">docs</a>)PK") != std::string::npos,
          "Markdown links become escaped anchors");
    check(html.find("<code>code &lt;x&gt;</code>") != std::string::npos, "Markdown inline code escapes HTML");
    check(html.find(R"PK(<ul>
<li>parent<ul>
<li>child</li>)PK") != std::string::npos,
          "Markdown nested unordered lists convert to nested ul/li");
    check(html.find(R"PK(<ol>
<li>first</li>
<li>second</li>)PK") != std::string::npos,
          "Markdown ordered lists convert to ol/li");
    check(html.find("<table>") != std::string::npos && html.find("<th>Name</th>") != std::string::npos &&
              html.find("<td><strong>B</strong></td>") != std::string::npos,
          "Markdown tables convert to HTML tables");
    check(html.find(R"PK(<pre><code class="language-cpp">if (a &lt; b) return;
</code></pre>)PK") != std::string::npos,
          "Markdown fenced code converts to escaped pre/code");
    check(html.find("<div>raw</div>") != std::string::npos, "Markdown raw HTML block is preserved");
}

void test_markdown_plaintext_and_document_rendering() {
    const std::string md = "## Heading\n\nParagraph with **bold** and [docs](https://example.com).\n\n```\n**not bold**\n```\n";
    const std::string plain = pkchat::markdown::to_plaintext(md);
    check(plain.find("Heading") != std::string::npos && plain.find("##") == std::string::npos,
          "Markdown plaintext strips heading marker");
    check(plain.find("Paragraph with bold and docs (https://example.com).") != std::string::npos,
          "Markdown plaintext strips inline markup and keeps link URL");
    check(plain.find("**not bold**") != std::string::npos, "Markdown plaintext keeps code block content");

    const std::string doc = pkchat::markdown::to_html_document("# Saved");
    check(doc.find("<!doctype html>") == 0, "Markdown HTML document starts with doctype");
    check(doc.find(R"PK(<meta charset="utf-8">)PK") != std::string::npos, "Markdown HTML document includes charset");
    check(doc.find(R"PK(name="viewport")PK") != std::string::npos, "Markdown HTML document includes viewport");
    check(doc.find(std::string("<title>pkchat ") + pkchat::kVersion + " output</title>") != std::string::npos,
          "Markdown HTML document title displays the current pkchat version");
    check(doc.find("<h1>Saved</h1>") != std::string::npos, "Markdown HTML document includes rendered body");
}

void test_comprehensive_markdown_to_html_fixture() {
    const std::string input = read_fixture("tests/fixtures/comprehensive.md");
    const std::string output = pkchat::markdown::to_html_document(input);

    check(output.find("<h1>Comprehensive Markdown Fixture</h1>") != std::string::npos,
          "comprehensive Markdown converts level-one heading");
    check(output.find("<h2>Lists And Structure</h2>") != std::string::npos &&
              output.find("<h3>Third-Level Heading</h3>") != std::string::npos,
          "comprehensive Markdown converts three heading levels");
    check(output.find("<strong>bold text</strong>") != std::string::npos &&
              output.find("<em>italic text</em>") != std::string::npos &&
              output.find("<u>underlined text</u>") != std::string::npos,
          "comprehensive Markdown converts inline formatting");
    check(output.find("<ul>") != std::string::npos && output.find("<ol>") != std::string::npos,
          "comprehensive Markdown converts ordered and unordered lists");
    check(output.find(R"PK(<a href="https://example.com/docs?lang=en&amp;mode=test">normal link</a>)PK") !=
              std::string::npos,
          "comprehensive Markdown converts and escapes links");
    check(output.find(R"PK(<a href="https://example.com/gallery"><img src="https://example.com/assets/placeholder.png" alt="A linked placeholder image"></a>)PK") !=
              std::string::npos,
          "comprehensive Markdown converts a linked image");
    check(output.find("你好，世界") != std::string::npos && output.find("مرحبا بالعالم") != std::string::npos &&
              output.find("😀 🚀 ✅") != std::string::npos,
          "comprehensive Markdown preserves multilingual UTF-8 and emoji");
    check(output.find(R"PK(<code class="language-javascript">)PK") != std::string::npos &&
              output.find("<table>") != std::string::npos,
          "comprehensive Markdown converts fenced code and a table");
}

void test_comprehensive_html_to_markdown_fixture() {
    const std::string input = read_fixture("tests/fixtures/comprehensive.html");
    const std::string output = pkchat::html::convert(input, pkchat::html::OutputFormat::Markdown);

    check(output.find("# Comprehensive HTML Fixture") != std::string::npos,
          "comprehensive HTML converts level-one heading");
    check(output.find("## Languages And Emoji") != std::string::npos &&
              output.find("### Multilingual Content") != std::string::npos,
          "comprehensive HTML converts three heading levels");
    check(output.find("**bold text**") != std::string::npos && output.find("*italic text*") != std::string::npos &&
              output.find("++underlined text++") != std::string::npos,
          "comprehensive HTML converts inline formatting");
    check(output.find("- First item") != std::string::npos &&
              output.find("1. Prepare the fixture") != std::string::npos &&
              output.find("3. Verify the result ✅") != std::string::npos,
          "comprehensive HTML converts ordered and unordered lists");
    check(output.find("[a normal link](https://example.com/docs?lang=en&mode=test)") != std::string::npos,
          "comprehensive HTML converts links and decodes entities");
    check(output.find("[![A linked placeholder image](https://example.com/assets/placeholder.png)](https://example.com/gallery)") !=
              std::string::npos,
          "comprehensive HTML converts a linked image");
    check(output.find("你好，世界") != std::string::npos && output.find("مرحبا بالعالم") != std::string::npos &&
              output.find("😀 🚀 ✅") != std::string::npos,
          "comprehensive HTML preserves multilingual UTF-8 and emoji");
    check(output.find("fixtureGreeting") == std::string::npos && output.find("color-scheme") == std::string::npos,
          "comprehensive HTML excludes script and style contents");
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

void test_editor_word_wrap_breaks_on_spaces() {
    pkchat::editor::EditorState state = pkchat::editor::EditorState::from_text("alpha beta");
    pkchat::editor::RenderedPanel rendered = state.render({1, 1, 2, 8});
    check(rendered.lines[0] == "alpha   ", "editor wraps at a word break when available");
    check(rendered.lines[1] == "beta    ", "editor continues after the wrapped word break");
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

    parsed = pkchat::editor::parse_assist_command("/quit", default_config);
    check(parsed.ok && parsed.kind == pkchat::editor::AssistCommandKind::Quit, "/quit parses");

    parsed = pkchat::editor::parse_assist_command("//quit", default_config);
    check(parsed.ok && parsed.kind == pkchat::editor::AssistCommandKind::Quit,
          "duplicate leading slashes in /quit are accepted");

    const std::vector<std::string> completions =
        pkchat::editor::assist_command_completions(default_config);
    check(!completions.empty() && completions.front() == "/spell", "assist completions include /spell");
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
    check(execution.messages.front().content.rfind("Custom system", 0) == 0 &&
              execution.messages.front().content.find(default_continue->prompt) != std::string::npos,
          "user --system is prepended to assist task system prompt");

    pkchat::cli::Options configured_options;
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

    const pkchat::provider::RequestContext assist_context =
        pkchat::editor::assist_request_context(context, true);
    check(assist_context.suppress_streaming_reasoning,
          "editor assist suppresses streamed reasoning deltas");
}

void test_editor_ai_continue_helpers() {
    pkchat::editor::PieceTable text = pkchat::editor::PieceTable::from_string("abcdefghij");
    check(text.range_text(2, 4) == "cdef", "range_text returns a bounded substring");
    check(text.range_text(0, 100) == "abcdefghij", "range_text clamps to buffer size");

    check(pkchat::editor::continue_status_message("gpt-test", "thinking... ESC to abort") ==
              "[gpt-test] thinking... ESC to abort",
          "continue status message includes model name");

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
    ai_continue.settings = pkchat::editor::ai_continue_settings_from_env();
    check(!pkchat::editor::validate_continue_request(ai_continue).ok(),
          "none provider rejects AI continue");

    const char* lm_argv[] = {"pkchat", "lmstudio", "--editor"};
    pkchat::cli::ParseResult lm_parsed = pkchat::cli::parse_args(3, const_cast<char**>(lm_argv));
    check(lm_parsed.error.ok(), "lmstudio editor args without model parse");
    context = pkchat::provider::build_context(lm_parsed.options);
    check(context.error.ok(), "lmstudio provider context builds without model");
    check(pkchat::editor::editor_auto_selects_model(context.context),
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
    check(pkchat::editor::editor_auto_selects_model(localhost_context.context),
          "localhost custom endpoint auto-selects the first model");

    pkchat::provider::RequestContext openai_context;
    openai_context.profile.name = "openai";
    check(!pkchat::editor::editor_auto_selects_model(openai_context),
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

    const char* previous_read = std::getenv("MAX_AI_CONTINUE_READ");
    const char* previous_tokens = std::getenv("MAX_AI_CONTINUE_TOKENS");
#if defined(_WIN32)
    _putenv_s("MAX_AI_CONTINUE_READ", "16");
    _putenv_s("MAX_AI_CONTINUE_TOKENS", "2048");
#else
    setenv("MAX_AI_CONTINUE_READ", "16", 1);
    setenv("MAX_AI_CONTINUE_TOKENS", "2048", 1);
#endif
    const pkchat::editor::AiContinueSettings env_settings = pkchat::editor::ai_continue_settings_from_env();
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

void test_editor_movement_sequence_parse() {
    pkchat::editor::MovementKeyEvent event;
    check(pkchat::editor::parse_movement_sequence("[D", event) && !event.shift &&
              event.key == pkchat::editor::MovementKey::Left,
          "left arrow sequence parses");
    check(pkchat::editor::parse_movement_sequence("[1;2C", event) && event.shift &&
              event.key == pkchat::editor::MovementKey::Right,
          "shift right arrow sequence parses");
    check(pkchat::editor::parse_movement_sequence("[5;2~", event) && event.shift &&
              event.key == pkchat::editor::MovementKey::PageUp,
          "shift page up sequence parses");
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

void test_tui_layout_reserves_editor_input_panel() {
    pkchat::tui::Layout small = pkchat::tui::layout_for_terminal(8, 20);
    check(small.rows == 8 && small.cols == 20, "TUI layout clamps to requested small terminal");
    check(small.header_rows == 0 && small.history_row == 1, "TUI layout has no persistent header rows");
    check(small.history_rows >= 1, "TUI layout leaves room for chat history");
    check(small.input_rect.height == 3, "TUI layout keeps minimum multiline input height");
    check(small.input_rect.row + small.input_rect.height - 1 <= small.rows,
          "TUI input panel stays inside terminal rows");

    pkchat::tui::Layout large = pkchat::tui::layout_for_terminal(40, 100);
    check(large.input_rect.height == 8, "TUI layout uses one fifth of a large terminal for input");
    check(large.input_rect.width == 100, "TUI input panel tracks terminal width");
    check(large.history_rows > large.input_rect.height, "TUI layout keeps the editor from taking the full screen");
}

void test_tui_regeneration_plan_uses_last_user_turn() {
    pkchat::chat::Session session;
    session.messages.push_back({"system", "stay concise"});
    session.messages.push_back({"user", "first"});
    session.messages.push_back({"assistant", "one"});
    session.messages.push_back({"user", "second"});
    session.messages.push_back({"assistant", "two"});

    pkchat::tui::RegenerationPlan plan = pkchat::tui::regeneration_plan_for_session(session);
    check(plan.available, "TUI regeneration plan is available when a user turn exists");
    check(plan.erase_from == 3, "TUI regeneration plan erases from the last user turn");
    check(plan.prompt == "second", "TUI regeneration plan reuses the last user prompt");

    pkchat::chat::Session no_user;
    no_user.messages.push_back({"system", "only system"});
    plan = pkchat::tui::regeneration_plan_for_session(no_user);
    check(!plan.available, "TUI regeneration plan is unavailable without a user turn");
}

void test_tui_history_jump_helpers() {
    check(pkchat::tui::history_scroll_for_thread_beginning() > 1000000,
          "TUI Home jump requests a clamped scrollback maximum");
    check(pkchat::tui::history_scroll_for_thread_end() == 0,
          "TUI End jump returns to the live chat bottom");
}

void test_tui_thinking_trace_display() {
    const std::string raw = "<think>internal trace</think>\n\nVisible answer";
    pkchat::tui::ThinkingDisplay shown = pkchat::tui::thinking_display_text(raw, true);
    check(shown.text == raw, "TUI thinking trace mode keeps raw assistant text");

    pkchat::tui::ThinkingDisplay hidden = pkchat::tui::thinking_display_text(raw, false);
    check(hidden.saw_thinking_tag, "TUI thinking display detects hidden trace tags");
    check(!hidden.open_thinking_tag, "TUI thinking display detects closed trace tags");
    check(hidden.text == "Visible answer", "TUI thinking notrace hides closed trace blocks");

    hidden = pkchat::tui::thinking_display_text("<think>still reasoning", false);
    check(hidden.saw_thinking_tag, "TUI thinking display detects an open trace tag");
    check(hidden.open_thinking_tag, "TUI thinking display reports an open trace tag");
    check(hidden.text.empty(), "TUI thinking notrace hides an unfinished trace");

    hidden = pkchat::tui::thinking_display_text("Before <think>hidden</think> after", false);
    check(hidden.text == "Before  after", "TUI thinking notrace preserves visible text around a trace");
}

void test_tui_ready_and_generation_status() {
    check(pkchat::tui::ready_status() == std::string("Pkchat v") + pkchat::kVersion + " ready",
          "TUI ready status displays the current pkchat version");

    pkchat::provider::ChatResult result;
    result.ttft_ms = 100;
    result.total_ms = 1100;
    result.completion_tokens = 20;
    result.completion_tokens_estimated = true;

    const std::string streaming = pkchat::tui::generation_ready_status(result, true, {}, 0);
    check(streaming.find("TTFT: 100 ms") != std::string::npos,
          "TUI streaming completion status displays time to first token");
    check(streaming.find("Token/s: 20.0 (estimated)") != std::string::npos,
          "TUI streaming completion status estimates throughput after the first token");

    const std::string non_streaming = pkchat::tui::generation_ready_status(result, false, {}, 0);
    check(non_streaming.find("Response: 1100 ms") != std::string::npos,
          "TUI non-streaming completion status reports response latency instead of TTFT");
    check(non_streaming.find("Token/s: 18.2 (estimated)") != std::string::npos,
          "TUI non-streaming completion status estimates whole-response throughput");

    result.usage_json = "{\"prompt_tokens\":20,\"completion_tokens\":5,\"total_tokens\":25}";
    const std::vector<pkchat::provider::Message> messages = {
        {"user", "hi"}, {"assistant", "<think>x</think>ok"}};
    const std::string context_status =
        pkchat::tui::generation_ready_status(result, true, messages, 100);
    check(context_status.find("TTFT 100ms | ~20.0 tok/s") != std::string::npos,
          "TUI context status uses compact timing and estimated-throughput notation");
    check(context_status.find("Context used: 25/100 (25.0%)") != std::string::npos,
          "TUI completion status displays estimated context usage");

    result.usage_json = "null";
    const std::string exhausted =
        pkchat::tui::generation_ready_status(result, true, messages, 10);
    check(exhausted.find("Context used: 17/10 (170.0%)") != std::string::npos,
          "TUI context estimate reports usage beyond the configured window");
}

void test_tui_theme_parsing_and_contrast() {
    pkchat::tui::ThemeName theme = pkchat::tui::ThemeName::Dark;
    check(pkchat::tui::parse_theme_name("dark", theme), "TUI dark theme parses");
    check(theme == pkchat::tui::ThemeName::Dark, "TUI dark theme selected");
    check(pkchat::tui::parse_theme_name("Light", theme), "TUI light theme parses case-insensitively");
    check(theme == pkchat::tui::ThemeName::Light, "TUI light theme selected");
    check(!pkchat::tui::parse_theme_name("sepia", theme), "TUI rejects unknown theme");

    const std::vector<pkchat::tui::ThemeName> themes = {
        pkchat::tui::ThemeName::Dark,
        pkchat::tui::ThemeName::Light,
    };
    const std::vector<pkchat::tui::StyleRole> roles = {
        pkchat::tui::StyleRole::Text,
        pkchat::tui::StyleRole::Muted,
        pkchat::tui::StyleRole::ThinkingTrace,
        pkchat::tui::StyleRole::UserLabel,
        pkchat::tui::StyleRole::AssistantLabel,
        pkchat::tui::StyleRole::Error,
        pkchat::tui::StyleRole::Status,
        pkchat::tui::StyleRole::InputLabel,
    };

    for (pkchat::tui::ThemeName item : themes) {
        for (pkchat::tui::StyleRole role : roles) {
            const pkchat::tui::StylePair pair = pkchat::tui::style_pair_for(item, role);
            check(pkchat::tui::contrast_ratio(pair.foreground, pair.background) >= 4.5,
                  std::string("TUI theme contrast meets WCAG AA for ") + pkchat::tui::theme_name(item));
        }
    }

    const pkchat::tui::StylePair dark_text =
        pkchat::tui::style_pair_for(pkchat::tui::ThemeName::Dark, pkchat::tui::StyleRole::Text);
    const pkchat::tui::StylePair dark_thinking =
        pkchat::tui::style_pair_for(pkchat::tui::ThemeName::Dark, pkchat::tui::StyleRole::ThinkingTrace);
    check(pkchat::tui::contrast_ratio(dark_thinking.foreground, dark_thinking.background) <
              pkchat::tui::contrast_ratio(dark_text.foreground, dark_text.background),
          "TUI dark thinking trace text is dimmer than normal text");

    const pkchat::tui::StylePair light_text =
        pkchat::tui::style_pair_for(pkchat::tui::ThemeName::Light, pkchat::tui::StyleRole::Text);
    const pkchat::tui::StylePair light_thinking =
        pkchat::tui::style_pair_for(pkchat::tui::ThemeName::Light, pkchat::tui::StyleRole::ThinkingTrace);
    check(pkchat::tui::contrast_ratio(light_thinking.foreground, light_thinking.background) <
              pkchat::tui::contrast_ratio(light_text.foreground, light_text.background),
          "TUI light thinking trace text is less stark than normal text");
}

void test_cli_responses_parse() {
    const char* argv[] = {"pkchat", "--responses", "-p", "hello"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(4, const_cast<char**>(argv));
    check(parsed.error.ok(), "Responses API shortcut args parse");
    check(parsed.options.api == "responses", "--responses selects Responses API");
}

void test_provider_registry_resolves_added_profiles() {
    std::vector<pkchat::provider::Profile> profiles = pkchat::provider::built_in_profiles();
    check(profiles.size() >= 24, "provider registry includes offline and compatibility profiles");

    const char* grok_argv[] = {"pkchat", "grok", "--list-models", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult grok = pkchat::cli::parse_args(5, const_cast<char**>(grok_argv));
    check(grok.error.ok(), "grok alias args parse");
    pkchat::provider::ContextResult grok_ctx = pkchat::provider::build_context(grok.options);
    check(grok_ctx.error.ok(), "grok alias context builds");
    check(grok_ctx.context.profile.name == "xai", "grok alias resolves to xai");
    check(grok_ctx.context.base_url == "https://api.x.ai/v1", "xai base URL selected");

    const char* kimi_argv[] = {"pkchat", "--provider", "kimi", "--list-models", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult kimi = pkchat::cli::parse_args(6, const_cast<char**>(kimi_argv));
    check(kimi.error.ok(), "kimi alias args parse");
    pkchat::provider::ContextResult kimi_ctx = pkchat::provider::build_context(kimi.options);
    check(kimi_ctx.error.ok(), "kimi alias context builds");
    check(kimi_ctx.context.profile.name == "moonshot", "kimi alias resolves to moonshot");

    const char* llama_argv[] = {"pkchat", "llama.cpp", "--list-models"};
    pkchat::cli::ParseResult llama = pkchat::cli::parse_args(3, const_cast<char**>(llama_argv));
    check(llama.error.ok(), "llama.cpp alias args parse");
    pkchat::provider::ContextResult llama_ctx = pkchat::provider::build_context(llama.options);
    check(llama_ctx.error.ok(), "llama.cpp alias context builds");
    check(llama_ctx.context.profile.name == "llamacpp", "llama.cpp alias resolves to llamacpp");
    check(llama_ctx.context.profile.local_endpoint, "llamacpp is marked local");

    const char* vllm_argv[] = {"pkchat", "vllm", "--list-models"};
    pkchat::cli::ParseResult vllm = pkchat::cli::parse_args(3, const_cast<char**>(vllm_argv));
    check(vllm.error.ok(), "vllm shortcut args parse");
    pkchat::provider::ContextResult vllm_ctx = pkchat::provider::build_context(vllm.options);
    check(vllm_ctx.error.ok(), "vllm context builds");
    check(vllm_ctx.context.api_key == "token-abc123", "vllm uses configured dummy API key");

    const char* deepinfra_argv[] = {"pkchat", "--provider", "deepinfra", "--list-models", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult deepinfra = pkchat::cli::parse_args(6, const_cast<char**>(deepinfra_argv));
    check(deepinfra.error.ok(), "deepinfra args parse");
    pkchat::provider::ContextResult deepinfra_ctx = pkchat::provider::build_context(deepinfra.options);
    check(deepinfra_ctx.error.ok(), "deepinfra context builds");
    check(deepinfra_ctx.context.profile.key_envs.size() >= 2 && deepinfra_ctx.context.profile.key_envs[1] == "DEEPINFRA_TOKEN",
          "deepinfra registers alternate token env var");

    const char* zai_argv[] = {"pkchat", "--provider", "z.ai", "--list-models", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult zai = pkchat::cli::parse_args(6, const_cast<char**>(zai_argv));
    check(zai.error.ok(), "Z.AI alias args parse");
    pkchat::provider::ContextResult zai_ctx = pkchat::provider::build_context(zai.options);
    check(zai_ctx.error.ok(), "Z.AI alias context builds");
    check(zai_ctx.context.profile.name == "zai", "z.ai alias resolves to zai");
    check(zai_ctx.context.base_url == "https://api.z.ai/api/paas/v4", "Z.AI base URL selected");
    check(zai_ctx.context.chat_url == "https://api.z.ai/api/paas/v4/chat/completions",
          "Z.AI Chat Completions URL selected");
    pkchat::provider::ModelsResult zai_models;
    pkchat::Error zai_models_error = pkchat::provider::list_models(zai_ctx.context, zai_models);
    check(zai_models_error.code == pkchat::ErrorCode::UnsupportedFeature,
          "Z.AI rejects undocumented model listing before transport");

    const char* qwen_argv[] = {"pkchat", "qwen", "--list-models", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult qwen = pkchat::cli::parse_args(5, const_cast<char**>(qwen_argv));
    check(qwen.error.ok(), "Qwen shortcut args parse");
    pkchat::provider::ContextResult qwen_ctx = pkchat::provider::build_context(qwen.options);
    check(qwen_ctx.error.ok(), "Qwen context builds");
    check(qwen_ctx.context.profile.name == "qwen", "Qwen shortcut resolves to qwen");
    check(qwen_ctx.context.base_url == "https://dashscope-intl.aliyuncs.com/compatible-mode/v1",
          "Qwen selects the global Model Studio base URL");
    check(qwen_ctx.context.models_url ==
              "https://dashscope-intl.aliyuncs.com/compatible-mode/v1/models",
          "Qwen model-list URL selected");
    check(qwen_ctx.context.profile.key_envs.front() == "DASHSCOPE_API_KEY",
          "Qwen uses the documented Model Studio key environment variable");
}

void test_none_provider_allows_an_empty_endpoint() {
    const char* argv[] = {"pkchat", "--provider", "none", "--repl"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(4, const_cast<char**>(argv));
    check(parsed.error.ok(), "none provider parses without a positional endpoint");
    check(parsed.options.positional_url.empty(), "none provider keeps the omitted endpoint empty");
    check(pkchat::provider::validate_profile_name(parsed.options.provider).ok(),
          "none is a recognized provider name in standalone modes");

    pkchat::provider::ContextResult context = pkchat::provider::build_context(parsed.options);
    check(context.error.ok(), "none provider context builds without an endpoint");
    check(context.context.profile.name == "none" && context.context.profile.offline,
          "none resolves to the offline provider profile");
    check(context.context.base_url.empty() && context.context.chat_url.empty() &&
              context.context.responses_url.empty() && context.context.models_url.empty(),
          "none provider leaves every model endpoint empty");
    check(!pkchat::provider::capabilities_for(context.context).chat_completions &&
              !pkchat::provider::capabilities_for(context.context).model_listing,
          "none provider advertises no model capabilities");

    pkchat::provider::ModelsResult models;
    pkchat::Error err = pkchat::provider::list_models(context.context, models);
    check(err.code == pkchat::ErrorCode::UnsupportedFeature,
          "none provider rejects model listing before transport");

    pkchat::provider::ChatResult chat;
    err = pkchat::provider::send_chat_messages(
        context.context, {{"user", "hello"}},
        [](const std::string&) { return pkchat::ok_error(); }, chat);
    check(err.code == pkchat::ErrorCode::UnsupportedFeature,
          "none provider rejects chat before transport");

    const char* alias_argv[] = {"pkchat", "offline", "--repl"};
    pkchat::cli::ParseResult alias = pkchat::cli::parse_args(3, const_cast<char**>(alias_argv));
    pkchat::provider::ContextResult alias_context = pkchat::provider::build_context(alias.options);
    check(alias_context.error.ok() && alias_context.context.profile.name == "none",
          "offline positional alias resolves without an endpoint");

    const char* endpoint_argv[] = {
        "pkchat", "--provider", "none", "--base-url", "http://localhost:1234", "--repl"};
    pkchat::cli::ParseResult endpoint =
        pkchat::cli::parse_args(6, const_cast<char**>(endpoint_argv));
    pkchat::provider::ContextResult endpoint_context =
        pkchat::provider::build_context(endpoint.options);
    check(endpoint_context.error.code == pkchat::ErrorCode::BadArgs,
          "none provider rejects model endpoint overrides");
}

void test_provider_capabilities_and_responses_context() {
    const char* argv[] = {"pkchat", "--provider", "openai", "--api", "responses", "-p", "hello", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(9, const_cast<char**>(argv));
    check(parsed.error.ok(), "OpenAI Responses args parse");
    pkchat::provider::ContextResult ctx = pkchat::provider::build_context(parsed.options);
    check(ctx.error.ok(), "OpenAI Responses context builds");
    check(ctx.context.api_kind == pkchat::provider::ApiKind::Responses, "Responses API kind selected");
    check(pkchat::provider::active_request_url(ctx.context) == "https://api.openai.com/v1/responses",
          "OpenAI Responses endpoint selected");
    check(pkchat::provider::capabilities_for(ctx.context).responses_api, "OpenAI reports Responses capability");
    check(pkchat::provider::capabilities_for(ctx.context).chat_completions, "OpenAI reports Chat Completions capability");

    const char* shortcut_argv[] = {"pkchat", "--provider", "openai_responses", "-p", "hello", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult shortcut = pkchat::cli::parse_args(7, const_cast<char**>(shortcut_argv));
    check(shortcut.error.ok(), "openai_responses profile shortcut args parse");
    pkchat::provider::ContextResult shortcut_ctx = pkchat::provider::build_context(shortcut.options);
    check(shortcut_ctx.error.ok(), "openai_responses context builds");
    check(shortcut_ctx.context.profile.name == "openai", "openai_responses uses OpenAI profile");
    check(shortcut_ctx.context.api_kind == pkchat::provider::ApiKind::Responses, "openai_responses selects Responses API");
}

void test_explicit_chat_url_does_not_require_base_when_model_set() {
    const char* argv[] = {"pkchat", "--chat-url", "https://example.test/custom/chat", "-m", "model", "-p", "hello", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(9, const_cast<char**>(argv));
    check(parsed.error.ok(), "explicit chat URL args parse");
    pkchat::provider::ContextResult ctx = pkchat::provider::build_context(parsed.options);
    check(ctx.error.ok(), "explicit chat URL context builds without base URL when model is set");
    check(ctx.context.chat_url == "https://example.test/custom/chat", "explicit chat URL is preserved");
}

void test_provider_responses_unsupported_and_override() {
    const char* unsupported_argv[] = {"pkchat", "--provider", "openrouter", "--api", "responses", "-p", "hello", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult unsupported = pkchat::cli::parse_args(9, const_cast<char**>(unsupported_argv));
    check(unsupported.error.ok(), "unsupported Responses args parse");
    pkchat::provider::ContextResult unsupported_ctx = pkchat::provider::build_context(unsupported.options);
    check(!unsupported_ctx.error.ok(), "chat-only provider rejects built-in Responses API");
    check(unsupported_ctx.error.code == pkchat::ErrorCode::UnsupportedFeature, "Responses rejection uses unsupported feature error");

    const char* override_argv[] = {"pkchat", "--provider", "openrouter", "--api", "responses", "--responses-url", "https://example.test/v1/responses", "-p", "hello", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult override = pkchat::cli::parse_args(11, const_cast<char**>(override_argv));
    check(override.error.ok(), "Responses override args parse");
    pkchat::provider::ContextResult override_ctx = pkchat::provider::build_context(override.options);
    check(override_ctx.error.ok(), "Responses override context builds");
    check(override_ctx.context.responses_url == "https://example.test/v1/responses", "Responses override endpoint selected");
    check(pkchat::provider::capabilities_for(override_ctx.context).responses_api, "Responses override reports capability");
}

void test_cli_rejects_unknown() {
    const char* argv[] = {"pkchat", "--bogus"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(2, const_cast<char**>(argv));
    check(!parsed.error.ok(), "unknown option rejected");
    check(parsed.error.code == pkchat::ErrorCode::BadArgs, "unknown option is bad args");
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

void test_json_parse() {
    pkchat::json::ParseResult parsed = pkchat::json::parse("{\"data\":[{\"id\":\"m1\"}],\"text\":\"hi\\nthere\"}");
    check(parsed.error.ok(), "JSON parse succeeds");
    const pkchat::json::Value* data = parsed.value.get("data");
    check(data != nullptr && data->is_array(), "JSON data array");
    const pkchat::json::Value* id = data->at(0)->get("id");
    check(id != nullptr && id->string == "m1", "JSON nested string");
}

void test_chat_session_json_round_trip() {
    pkchat::provider::RequestContext context;
    context.profile.name = "custom_openai_chat";
    context.base_url = "http://localhost:8000/v1";
    context.options.model = "mock-model";
    context.options.stream = false;
    context.options.context_tokens = 65536;
    pkchat::chat::Session session = pkchat::chat::new_session(context);
    check(session.settings_json.find("\"context_tokens\":65536") != std::string::npos,
          "new chat settings preserve the configured context-window size");
    session.created_at = "2026-06-14T00:00:00Z";
    session.updated_at = session.created_at;
    session.messages.push_back({"user", "hello"});
    session.messages.push_back({"assistant", "Hello"});
    session.compaction_events.push_back({"2026-06-14T00:01:00Z", "truncate-oldest", 2, 1000, 500,
                                         "Context compacted for test"});

    const std::string encoded = pkchat::chat::session_to_json(session);
    pkchat::json::ParseResult parsed = pkchat::json::parse(encoded);
    check(parsed.error.ok(), "chat session JSON parses");
    const pkchat::json::Value* messages = parsed.value.get("messages");
    check(messages != nullptr && messages->is_array() && messages->array.size() == 2, "chat messages persisted");

    const std::string path = "build/unit-chat.json";
    pkchat::Error err = pkchat::chat::save_session_atomic(path, session);
    check(err.ok(), "chat session saves atomically");
    pkchat::chat::Session loaded;
    err = pkchat::chat::load_session(path, loaded);
    check(err.ok(), "chat session loads");
    check(loaded.messages.size() == 2, "loaded chat has messages");
    check(!loaded.messages.empty() && loaded.messages[0].content == "hello", "loaded user message preserved");
    check(loaded.compaction_events.size() == 1 && loaded.compaction_events[0].messages_compacted == 2,
          "loaded chat preserves compaction events");
}

void test_chat_session_rejects_corrupt_json() {
    const std::string path = "build/corrupt-chat.json";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "{bad json";
    out.close();
    pkchat::chat::Session session;
    pkchat::Error err = pkchat::chat::load_session(path, session);
    check(!err.ok(), "corrupt chat file rejected");
    check(err.code == pkchat::ErrorCode::JsonParse, "corrupt chat file reports JSON parse error");
}

void test_runtime_event_queue_and_job_cancel() {
    pkchat::runtime::EventQueue<int> queue;
    int value = 0;
    check(!queue.try_pop(value), "empty runtime queue has no event");
    queue.push(7);
    check(queue.try_pop(value) && value == 7, "runtime queue preserves event value");

    pkchat::runtime::JobHandle job;
    std::atomic<bool> entered{false};
    job.start([&](pkchat::runtime::CancellationToken token) {
        entered.store(true, std::memory_order_release);
        while (!token.cancelled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        queue.push(42);
    });
    for (int i = 0; i < 100 && !entered.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(job.running(), "runtime job reports running");
    job.cancel();
    check(queue.wait_pop_for(value, std::chrono::milliseconds(1000)) && value == 42, "runtime job observes cancellation");
    job.join();
    check(!job.running(), "runtime job reports stopped after join");
}

void test_openrouter_shortcut_context() {
    const char* argv[] = {"pkchat", "openrouter", "-model", "provider/model", "-p", "hello", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(8, const_cast<char**>(argv));
    check(parsed.error.ok(), "openrouter shortcut args parse");
    pkchat::provider::ContextResult ctx = pkchat::provider::build_context(parsed.options);
    check(ctx.error.ok(), "openrouter shortcut context builds with auth header");
    check(ctx.context.profile.name == "openrouter", "openrouter shortcut selects profile");
    check(ctx.context.base_url == "https://openrouter.ai/api/v1", "openrouter shortcut uses standard base URL");
}
void test_openai_context_allows_missing_model() {
    const char* argv[] = {"pkchat", "--provider", "openai", "-p", "hello", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(7, const_cast<char**>(argv));
    check(parsed.error.ok(), "openai args without model parse");
    pkchat::provider::ContextResult ctx = pkchat::provider::build_context(parsed.options);
    check(ctx.error.ok(), "openai context builds without model so caller can discover one");
    check(ctx.context.options.model.empty(), "openai context keeps missing model empty before discovery");
}

void test_lmstudio_shortcut_context() {
    const char* argv[] = {"pkchat", "lmstudio", "-i"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(3, const_cast<char**>(argv));
    check(parsed.error.ok(), "lmstudio shortcut args parse");
    pkchat::provider::ContextResult ctx = pkchat::provider::build_context(parsed.options);
    check(ctx.error.ok(), "lmstudio shortcut context builds without key or model");
    check(ctx.context.profile.name == "lm_studio", "lmstudio shortcut selects profile");
    check(ctx.context.base_url == "http://localhost:1234/v1", "lmstudio shortcut uses default base URL");
    check(ctx.context.options.model.empty(), "lmstudio shortcut does not require model");
}

void test_lmstudio_context() {
    const char* argv[] = {"pkchat", "--provider", "lmstudio", "--list-models"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(4, const_cast<char**>(argv));
    check(parsed.error.ok(), "lmstudio args parse");
    pkchat::provider::ContextResult ctx = pkchat::provider::build_context(parsed.options);
    check(ctx.error.ok(), "lmstudio context builds without key");
    check(ctx.context.profile.name == "lm_studio", "lmstudio alias normalized");
    check(ctx.context.base_url == "http://localhost:1234/v1", "lmstudio default base URL");
}

}  // namespace

int main() {
    test_thinking_trace_splitter();
    test_config_reads_common_template();
    test_config_applies_user_settings();
    test_config_schema_rejects_invalid_settings_transactionally();
    test_config_xdg_path_resolution();
    test_config_parses_supported_values();
    test_config_rejects_invalid_input();
    test_config_file_read_errors();
    test_cli_parse();
    test_benchmark_cli_and_jsonl_dataset();
    test_cli_rejects_unknown();
    test_cli_provider_shortcut_parse();
    test_cli_repl_parse();
    test_cli_context_token_parse();
    test_cli_help_displays_version();
    test_cli_chat_parse();
    test_cli_chat_nocolors_parse();
    test_cli_editor_parse();
    test_cli_html_extract_parse();
    test_input_file_type_classification();
    test_image_loading_and_chat_request();
    test_text_context_loading_and_cancellation();
    test_image_capability_detection();
    test_context_policies_preserve_full_messages();
    test_http_private_address_socket_block();
    test_safe_fetch_rejects_private_literal();
    test_cli_output_format_parse();
    test_html_markdown_conversion();
    test_html_text_conversion();
    test_html_large_ignored_blocks();
    test_html_malformed_documents();
    test_html_utf8_validation();
    test_markdown_html_rendering();
    test_markdown_plaintext_and_document_rendering();
    test_comprehensive_markdown_to_html_fixture();
    test_comprehensive_html_to_markdown_fixture();
    test_cli_responses_parse();
    test_url_normalization();
    test_json_parse();
    test_lmstudio_context();
    test_provider_registry_resolves_added_profiles();
    test_none_provider_allows_an_empty_endpoint();
    test_provider_capabilities_and_responses_context();
    test_explicit_chat_url_does_not_require_base_when_model_set();
    test_provider_responses_unsupported_and_override();
    test_openrouter_shortcut_context();
    test_openai_context_allows_missing_model();
    test_lmstudio_shortcut_context();
    test_chat_session_json_round_trip();
    test_chat_session_rejects_corrupt_json();
    test_runtime_event_queue_and_job_cancel();
    test_editor_piece_table_edits();
    test_editor_rectangular_rendering();
    test_editor_word_wrap_rendering();
    test_editor_word_wrap_breaks_on_spaces();
    test_editor_kill_to_line_end();
    test_editor_undo_redo();
    test_editor_home_end_navigation();
    test_editor_selection_and_clipboard();
    test_editor_utf8_codepoint_navigation_and_editing();
    test_editor_unicode_grapheme_navigation_and_delete();
    test_editor_unicode_display_columns_and_offsets();
    test_editor_unicode_rendering_wraps_on_cell_boundaries();
    test_editor_unicode_emoji_zwj_wrap_keeps_sequence_intact();
    test_editor_unicode_emoji_pair_wraps_on_cell_boundaries();
    test_editor_unicode_emoji_skin_tone_wrap_keeps_modifier();
    test_editor_unicode_combining_sequence_wraps_on_grapheme_boundary();
    test_editor_invalid_utf8_rendering_is_sanitized();
    test_editor_unicode_selection_search_replace_and_file_round_trip();
    test_editor_assist_helpers();
    test_editor_ai_continue_helpers();
    test_editor_paste_prefers_local_clipboard();
    test_editor_movement_sequence_parse();
    test_editor_page_navigation();
    test_editor_search_navigation();
    test_editor_search_replace();
    test_editor_vertical_navigation_modes();
    test_editor_file_round_trip();
    test_editor_path_completion();
    test_editor_contextual_completion_modes();
    test_tui_layout_reserves_editor_input_panel();
    test_tui_regeneration_plan_uses_last_user_turn();
    test_tui_history_jump_helpers();
    test_tui_thinking_trace_display();
    test_tui_ready_and_generation_status();
    test_tui_theme_parsing_and_contrast();
    if (failures != 0) {
        std::cerr << failures << " unit test(s) failed\n";
        return 1;
    }
    std::cout << "unit tests passed\n";
    return 0;
}
