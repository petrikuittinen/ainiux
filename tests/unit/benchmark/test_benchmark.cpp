#include "benchmark/test_benchmark.hpp"
#include "support/test_support.hpp"
#include "benchmark/benchmark.hpp"
#include "cli/args.hpp"
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace pkchat::test::benchmark {

namespace {

using pkchat::test::check;
using pkchat::test::read_fixture;

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
    check(loaded.dataset.cases.size() == 103, "built-in benchmark dataset has exactly 103 cases");
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
    check(categories.size() == 6 && categories["safety"] == 10 &&
              categories["reasoning"] == 20 && categories["writing"] == 10 &&
              categories["coding"] == 10 && categories["multi-turn"] == 10 &&
              categories["cutoff"] == 43,
          "built-in benchmark dataset has expected category counts");
    check(reasoning_answers == 20 && qualitative_rubrics == 30 &&
              harmful_safety_cases == 6 && harmless_safety_cases == 4,
          "built-in cases have complete answer keys, rubrics, and safety decisions");
    const std::vector<const pkchat::benchmark::Case*> selected =
        pkchat::benchmark::select_cases(loaded.dataset, "reasoning", "", 2);
    check(selected.size() == 2 && selected[0]->id == "reasoning-01",
          "benchmark category and limit selection is deterministic");
    const std::vector<const pkchat::benchmark::Case*> cutoff_selected =
        pkchat::benchmark::select_cases(loaded.dataset, "cutoff", "", 2);
    check(cutoff_selected.size() == 2 && cutoff_selected[0]->id == "cutoff-2023-01" &&
              cutoff_selected[1]->id == "cutoff-2023-02" &&
              cutoff_selected[0]->tags.size() == 2 &&
              cutoff_selected[0]->tags[0] == "knowledge-cutoff" &&
              cutoff_selected[0]->tags[1] == "2023-01",
          "cutoff benchmark cases are selectable and carry event-month tags");
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

    std::istringstream missing_cutoff_answer(
        "{\"id\":\"cutoff-no-answer\",\"category\":\"cutoff\","
        "\"turns\":[\"What happened in 2024?\"]}\n");
    invalid = pkchat::benchmark::parse_jsonl(missing_cutoff_answer,
                                             "missing-cutoff.jsonl");
    check(!invalid.error.ok() &&
              invalid.error.message.find("reference_answer") != std::string::npos,
          "benchmark cutoff cases require reference answers");

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

void test_benchmark_dataset_io_and_scoring_edge_cases() {
    pkchat::benchmark::LoadResult missing =
        pkchat::benchmark::load_jsonl("build/missing-benchmark-dataset.jsonl");
    check(!missing.error.ok() && missing.error.code == pkchat::ErrorCode::FileRead,
          "missing benchmark dataset reports a file-read error");

    std::istringstream empty_stream;
    pkchat::benchmark::LoadResult empty = pkchat::benchmark::parse_jsonl(empty_stream, "empty.jsonl");
    check(!empty.error.ok() && empty.error.message.find("no cases") != std::string::npos,
          "empty benchmark JSONL stream is rejected");

    std::istringstream scored_dataset(
        "{\"id\":\"empty-response\",\"category\":\"reasoning\",\"turns\":[\"one\"],"
        "\"reference_answer\":\"done\","
        "\"expect\":[{\"type\":\"contains\",\"value\":\"answer\",\"turn\":1}]}\n");
    pkchat::benchmark::LoadResult scored =
        pkchat::benchmark::parse_jsonl(scored_dataset, "scored.jsonl");
    check(scored.error.ok(), "benchmark scorer fixture parses");
    pkchat::benchmark::ScoreResult empty_response =
        pkchat::benchmark::score_response(scored.dataset.cases[0], 1, "");
    check(empty_response.configured && !empty_response.passed,
          "contains scorer fails on an empty response");
    pkchat::benchmark::Case exact_case;
    exact_case.id = "exact-zero";
    exact_case.category = "reasoning";
    exact_case.turns = {"0"};
    exact_case.expectations = {{"exact", "0", 1}};
    pkchat::benchmark::ScoreResult exact_zero =
        pkchat::benchmark::score_response(exact_case, 1, "0");
    check(exact_zero.configured && exact_zero.passed,
          "exact scorer accepts a zero-valued response");
}

}  // namespace

void run_all() {
    test_benchmark_cli_and_jsonl_dataset();
    test_benchmark_dataset_io_and_scoring_edge_cases();
}

}  // namespace pkchat::test::benchmark
