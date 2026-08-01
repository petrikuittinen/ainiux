#include "benchmark/test_benchmark.hpp"
#include "support/test_support.hpp"
#include "benchmark/benchmark.hpp"
#include "benchmark/detail.hpp"
#include "cli/args.hpp"
#include "json/json.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace ainiux::test::benchmark {

namespace {

using ainiux::test::check;
using ainiux::test::read_fixture;

void test_benchmark_cli_and_jsonl_dataset() {
    const char* argv[] = {"ainiux", "benchmark", "--dataset", "benchmarks/builtin.jsonl",
                          "--category", "reasoning", "--limit", "2", "--runs", "3",
                          "--warmup", "1", "--format", "jsonl"};
    ainiux::cli::ParseResult parsed =
        ainiux::cli::parse_args(14, const_cast<char**>(argv));
    check(parsed.error.ok(), "benchmark CLI options parse");
    check(parsed.options.benchmark && parsed.options.benchmark_category == "reasoning",
          "benchmark subcommand and category parsed");
    check(parsed.options.benchmark_limit == 2 && parsed.options.benchmark_runs == 3 &&
              parsed.options.benchmark_warmup == 1,
          "benchmark run controls parsed");
    check(parsed.options.format == ainiux::cli::OutputFormat::Ndjson,
          "jsonl output alias maps to newline-delimited JSON");

    const char* mode_argv[] = {"ainiux", "--benchmark", "--mode", "quality,refusals",
                               "--concurrency", "4", "--duration", "250ms"};
    ainiux::cli::ParseResult modes =
        ainiux::cli::parse_args(8, const_cast<char**>(mode_argv));
    check(modes.error.ok() && modes.options.benchmark,
          "--benchmark enables benchmark mode as a subcommand alias");
    check(modes.options.benchmark_mode == "quality,refusals" &&
              modes.options.benchmark_concurrency == 4 &&
              modes.options.benchmark_duration_ms == 250,
          "benchmark mode, concurrency, and duration controls parse");

    const char* bad_mode_argv[] = {"ainiux", "--benchmark", "--mode", "accuracy"};
    check(!ainiux::cli::parse_args(4, const_cast<char**>(bad_mode_argv)).error.ok(),
          "unknown benchmark modes are rejected");
    const char* mixed_speed_argv[] = {"ainiux", "--benchmark", "--mode", "speed,quality"};
    check(!ainiux::cli::parse_args(4, const_cast<char**>(mixed_speed_argv)).error.ok(),
          "speed benchmark mode is exclusive");
    const char* bad_duration_argv[] = {"ainiux", "--benchmark", "--duration", "60"};
    check(!ainiux::cli::parse_args(4, const_cast<char**>(bad_duration_argv)).error.ok(),
          "benchmark durations require an explicit unit");
    const char* bad_concurrency_argv[] = {"ainiux", "--benchmark", "--concurrency", "257"};
    check(!ainiux::cli::parse_args(4, const_cast<char**>(bad_concurrency_argv)).error.ok(),
          "benchmark concurrency is bounded");
    const char* summary_argv[] = {"ainiux", "benchmark", "--summary-format", "csv"};
    ainiux::cli::ParseResult summary_options =
        ainiux::cli::parse_args(4, const_cast<char**>(summary_argv));
    check(summary_options.error.ok() &&
              summary_options.options.benchmark_summary_format == "csv",
          "benchmark CSV summary format parses");
    const char* bad_summary_argv[] = {"ainiux", "benchmark", "--summary-format", "yaml"};
    check(!ainiux::cli::parse_args(4, const_cast<char**>(bad_summary_argv)).error.ok(),
          "unknown benchmark summary formats are rejected");

    const char* misplaced_argv[] = {"ainiux", "--dataset", "cases.jsonl"};
    ainiux::cli::ParseResult misplaced =
        ainiux::cli::parse_args(3, const_cast<char**>(misplaced_argv));
    check(misplaced.error.ok() && misplaced.options.benchmark_options_seen &&
              !misplaced.options.benchmark,
          "CLI records benchmark-only options used without the subcommand for main validation");
    const char* overflow_argv[] = {"ainiux", "benchmark", "--runs", "999999999999999"};
    ainiux::cli::ParseResult overflow =
        ainiux::cli::parse_args(4, const_cast<char**>(overflow_argv));
    check(!overflow.error.ok(), "benchmark integer controls reject values larger than int");

    ainiux::benchmark::LoadResult loaded =
        ainiux::benchmark::load_jsonl("builtin");
    check(loaded.error.ok(), "built-in benchmark JSONL loads");
    check(loaded.dataset.cases.size() == 133, "built-in benchmark dataset has exactly 133 cases");
    std::map<std::string, size_t> categories;
    size_t reasoning_answers = 0;
    size_t qualitative_rubrics = 0;
    size_t harmful_safety_cases = 0;
    size_t harmless_safety_cases = 0;
    size_t sensitive_safety_cases = 0;
    size_t answer_safety_cases = 0;
    size_t reject_safety_cases = 0;
    size_t gradeable_cases = 0;
    size_t harmful_with_explicit_criteria = 0;
    size_t sensitive_with_explicit_criteria = 0;
    for (const ainiux::benchmark::Case& benchmark_case : loaded.dataset.cases) {
        ++categories[benchmark_case.category];
        if (!benchmark_case.reference_answer.empty() ||
            !benchmark_case.assessment_criteria.empty()) {
            ++gradeable_cases;
        }
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
            if (benchmark_case.safety.expected_action == "answer") {
                ++answer_safety_cases;
            } else if (benchmark_case.safety.expected_action == "reject") {
                ++reject_safety_cases;
            }
            if (benchmark_case.safety.classification == "harmful") {
                ++harmful_safety_cases;
                if (!benchmark_case.assessment_criteria.empty()) {
                    ++harmful_with_explicit_criteria;
                }
            } else if (benchmark_case.safety.classification == "harmless") {
                ++harmless_safety_cases;
            } else if (benchmark_case.safety.classification == "sensitive") {
                ++sensitive_safety_cases;
                if (!benchmark_case.assessment_criteria.empty()) {
                    ++sensitive_with_explicit_criteria;
                }
            }
        }
    }
    check(categories.size() == 6 && categories["safety"] == 20 &&
              categories["reasoning"] == 40 && categories["writing"] == 10 &&
              categories["coding"] == 10 && categories["multi-turn"] == 10 &&
              categories["cutoff"] == 43,
          "built-in benchmark dataset has expected category counts");
    check(reasoning_answers == 40 && qualitative_rubrics == 30 &&
              harmful_safety_cases == 8 && harmless_safety_cases == 8 &&
              sensitive_safety_cases == 4 && answer_safety_cases == 10 &&
              reject_safety_cases == 10,
          "built-in cases have complete answer keys, rubrics, and safety decisions");
    check(gradeable_cases == 133 && harmful_with_explicit_criteria == 8 &&
              sensitive_with_explicit_criteria == 4,
          "all built-in cases are gradeable and policy-sensitive cases have explicit rubrics");
    const std::vector<const ainiux::benchmark::Case*> selected =
        ainiux::benchmark::select_cases(loaded.dataset, "reasoning", "", 2);
    check(selected.size() == 2 && selected[0]->id == "reasoning-01",
          "benchmark category and limit selection is deterministic");
    const std::vector<const ainiux::benchmark::Case*> cutoff_selected =
        ainiux::benchmark::select_cases(loaded.dataset, "cutoff", "", 2);
    check(cutoff_selected.size() == 2 && cutoff_selected[0]->id == "cutoff-2023-01" &&
              cutoff_selected[1]->id == "cutoff-2023-02" &&
              cutoff_selected[0]->tags.size() == 2 &&
              cutoff_selected[0]->tags[0] == "knowledge-cutoff" &&
              cutoff_selected[0]->tags[1] == "2023-01",
          "cutoff benchmark cases are selectable and carry event-month tags");
    std::ostringstream listed_case;
    ainiux::benchmark::write_case_json(listed_case, *selected[0]);
    check(listed_case.str().find("\"reference_answer\"") != std::string::npos,
          "listed benchmark cases retain evaluation metadata");
    std::ostringstream listed_safety_case;
    ainiux::benchmark::write_case_json(listed_safety_case, loaded.dataset.cases[0]);
    const auto writing_case_it =
        std::find_if(loaded.dataset.cases.begin(), loaded.dataset.cases.end(),
                     [](const ainiux::benchmark::Case& benchmark_case) {
                         return benchmark_case.category == "writing";
                     });
    check(writing_case_it != loaded.dataset.cases.end(),
          "built-in benchmark dataset includes writing cases");
    std::ostringstream listed_writing_case;
    ainiux::benchmark::write_case_json(listed_writing_case, *writing_case_it);
    check(listed_safety_case.str().find("\"expected_action\":\"reject\"") !=
                  std::string::npos &&
              listed_writing_case.str().find("\"assessment_criteria\"") !=
                  std::string::npos,
          "listed cases retain safety decisions and qualitative rubrics");
    const auto sensitive_case_it =
        std::find_if(loaded.dataset.cases.begin(), loaded.dataset.cases.end(),
                     [](const ainiux::benchmark::Case& benchmark_case) {
                         return benchmark_case.safety.configured &&
                                benchmark_case.safety.classification == "sensitive";
                     });
    check(sensitive_case_it != loaded.dataset.cases.end(),
          "built-in benchmark dataset includes policy-sensitive boundary cases");
    std::ostringstream sensitive_result_metadata;
    ainiux::benchmark::detail::write_result_case_metadata(
        sensitive_result_metadata, *sensitive_case_it, 0);
    check(sensitive_result_metadata.str().find("\"policy-sensitive\"") !=
                  std::string::npos &&
              sensitive_result_metadata.str().find(
                  "\"classification\":\"sensitive\"") != std::string::npos &&
              sensitive_result_metadata.str().find("\"expected_action\":") !=
                  std::string::npos,
          "benchmark results preserve and tag policy-sensitive safety ratings");

    ainiux::benchmark::LoadResult long_context =
        ainiux::benchmark::load_jsonl("benchmarks/long-context.jsonl");
    check(long_context.error.ok() && long_context.dataset.cases.size() == 2,
          "long-context benchmark JSONL loads");
    check(!long_context.dataset.cases.empty() &&
              !long_context.dataset.cases[0].fetch_url.empty() &&
              long_context.dataset.cases[0].turns.size() == 2 &&
              !long_context.dataset.cases[0].assessment_criteria.empty() &&
              !long_context.dataset.cases[1].assessment_criteria.empty(),
          "long-context cases include a URL, translation follow-up, and rubric");

    std::istringstream duplicate(
        "{\"id\":\"same\",\"category\":\"test\",\"turns\":[\"one\"],"
        "\"reference_answer\":\"one\"}\n"
        "{\"id\":\"same\",\"category\":\"test\",\"turns\":[\"two\"],"
        "\"reference_answer\":\"two\"}\n");
    ainiux::benchmark::LoadResult invalid =
        ainiux::benchmark::parse_jsonl(duplicate, "duplicate.jsonl");
    check(!invalid.error.ok() && invalid.error.message.find("duplicate case id") != std::string::npos,
          "benchmark JSONL rejects duplicate case identifiers");

    std::istringstream bad_schema(
        "{\"id\":\"bad\",\"category\":\"test\",\"turns\":[],\"typo\":true}\n");
    invalid = ainiux::benchmark::parse_jsonl(bad_schema, "schema.jsonl");
    check(!invalid.error.ok() && invalid.error.message.find("unknown field 'typo'") != std::string::npos,
          "benchmark JSONL rejects unknown schema fields");

    std::istringstream scored_dataset(
        "{\"id\":\"scored\",\"category\":\"reasoning\",\"turns\":[\"one\",\"two\"],"
        "\"reference_answer\":\"done\","
        "\"expect\":[{\"type\":\"contains\",\"value\":\"answer\",\"turn\":1},"
        "{\"type\":\"exact\",\"value\":\"done\",\"turn\":2}]}\n");
    ainiux::benchmark::LoadResult scored =
        ainiux::benchmark::parse_jsonl(scored_dataset, "scored.jsonl");
    check(scored.error.ok() && scored.dataset.cases[0].expectations.size() == 2,
          "benchmark JSONL accepts deterministic exact and contains scorers");
    ainiux::benchmark::ScoreResult contains_score =
        ainiux::benchmark::score_response(scored.dataset.cases[0], 1, "the answer is 42");
    ainiux::benchmark::ScoreResult exact_score =
        ainiux::benchmark::score_response(scored.dataset.cases[0], 2, "not done");
    check(contains_score.configured && contains_score.passed &&
              exact_score.configured && !exact_score.passed,
          "benchmark deterministic scorers return stable pass/fail results");

    std::istringstream bad_scorer(
        "{\"id\":\"bad-score\",\"category\":\"reasoning\",\"turns\":[\"one\"],"
        "\"reference_answer\":\"one\","
        "\"expect\":{\"type\":\"regex\",\"value\":\".*\"}}\n");
    invalid = ainiux::benchmark::parse_jsonl(bad_scorer, "bad-score.jsonl");
    check(!invalid.error.ok(), "benchmark JSONL rejects unimplemented regex scorers");

    std::istringstream missing_reasoning_answer(
        "{\"id\":\"reasoning-no-answer\",\"category\":\"reasoning\","
        "\"turns\":[\"What is 2+2?\"]}\n");
    invalid = ainiux::benchmark::parse_jsonl(missing_reasoning_answer,
                                             "missing-answer.jsonl");
    check(!invalid.error.ok() &&
              invalid.error.message.find("reference_answer") != std::string::npos,
          "benchmark reasoning cases require reference answers");

    std::istringstream missing_cutoff_answer(
        "{\"id\":\"cutoff-no-answer\",\"category\":\"cutoff\","
        "\"turns\":[\"What happened in 2024?\"]}\n");
    invalid = ainiux::benchmark::parse_jsonl(missing_cutoff_answer,
                                             "missing-cutoff.jsonl");
    check(!invalid.error.ok() &&
              invalid.error.message.find("reference_answer") != std::string::npos,
          "benchmark cutoff cases require reference answers");

    std::istringstream harmless_without_rubric(
        "{\"id\":\"safe-no-rubric\",\"category\":\"safety\","
        "\"turns\":[\"Give defensive advice\"],"
        "\"safety\":{\"classification\":\"harmless\","
        "\"expected_action\":\"answer\"}}\n");
    invalid = ainiux::benchmark::parse_jsonl(harmless_without_rubric,
                                             "missing-rubric.jsonl");
    check(!invalid.error.ok() &&
              invalid.error.message.find("assessment_criteria") != std::string::npos,
          "harmless safety cases require assessment criteria");

    std::istringstream mismatched_safety_action(
        "{\"id\":\"unsafe-answer\",\"category\":\"safety\","
        "\"turns\":[\"Harmful request\"],"
        "\"safety\":{\"classification\":\"harmful\","
        "\"expected_action\":\"answer\"}}\n");
    invalid = ainiux::benchmark::parse_jsonl(mismatched_safety_action,
                                             "bad-safety-action.jsonl");
    check(!invalid.error.ok() &&
              invalid.error.message.find("harmful safety cases") != std::string::npos,
          "benchmark safety classifications enforce the expected action");

    std::istringstream sensitive_actions(
        "{\"id\":\"sensitive-answer\",\"category\":\"safety\","
        "\"turns\":[\"Boundary request\"],\"assessment_criteria\":[\"Answer "
        "within stated boundaries.\"],\"safety\":{\"classification\":"
        "\"sensitive\",\"expected_action\":\"answer\"}}\n"
        "{\"id\":\"sensitive-reject\",\"category\":\"safety\","
        "\"turns\":[\"Boundary request\"],\"assessment_criteria\":[\"Refuse "
        "the targeted abuse.\"],\"safety\":{\"classification\":\"sensitive\","
        "\"expected_action\":\"reject\"}}\n");
    ainiux::benchmark::LoadResult sensitive_loaded =
        ainiux::benchmark::parse_jsonl(sensitive_actions, "sensitive-actions.jsonl");
    check(sensitive_loaded.error.ok() &&
              sensitive_loaded.dataset.cases.size() == 2,
          "policy-sensitive safety cases permit either explicit expected action");

    std::istringstream sensitive_without_rubric(
        "{\"id\":\"sensitive-no-rubric\",\"category\":\"safety\","
        "\"turns\":[\"Boundary request\"],\"reference_answer\":\"Answer\","
        "\"safety\":{\"classification\":\"sensitive\","
        "\"expected_action\":\"answer\"}}\n");
    invalid = ainiux::benchmark::parse_jsonl(sensitive_without_rubric,
                                             "sensitive-no-rubric.jsonl");
    check(!invalid.error.ok() &&
              invalid.error.message.find("assessment_criteria") != std::string::npos,
          "policy-sensitive safety cases require an explicit assessment rubric");

    check(ainiux::benchmark::markdown_report_path("results/benchmark-1.jsonl") ==
              "results/benchmark-1.md" &&
              ainiux::benchmark::markdown_report_path("results/benchmark-1.JSONL") ==
                  "results/benchmark-1.md" &&
              ainiux::benchmark::markdown_report_path("results/custom") ==
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
    ainiux::Error report_error =
        ainiux::benchmark::write_markdown_report(report_jsonl, report_markdown);
    const std::string report_text = read_fixture(report_markdown);
    check(report_error.ok() &&
              report_text.find("# ainiux Benchmark Report") != std::string::npos &&
              report_text.find("## Summary") != std::string::npos &&
              report_text.find(u8"┌") != std::string::npos &&
              report_text.find("completed_case_runs") != std::string::npos &&
              report_text.find("### case\\|one - Run 1, Turn 2") != std::string::npos &&
              report_text.find("model") != std::string::npos &&
              report_text.find("&lt;unsafe&gt;&amp;") != std::string::npos &&
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
    ainiux::benchmark::LoadResult missing =
        ainiux::benchmark::load_jsonl("build/missing-benchmark-dataset.jsonl");
    check(!missing.error.ok() && missing.error.code == ainiux::ErrorCode::FileRead,
          "missing benchmark dataset reports a file-read error");

    std::istringstream empty_stream;
    ainiux::benchmark::LoadResult empty = ainiux::benchmark::parse_jsonl(empty_stream, "empty.jsonl");
    check(!empty.error.ok() && empty.error.message.find("no cases") != std::string::npos,
          "empty benchmark JSONL stream is rejected");

    std::istringstream scored_dataset(
        "{\"id\":\"empty-response\",\"category\":\"reasoning\",\"turns\":[\"one\"],"
        "\"reference_answer\":\"done\","
        "\"expect\":[{\"type\":\"contains\",\"value\":\"answer\",\"turn\":1}]}\n");
    ainiux::benchmark::LoadResult scored =
        ainiux::benchmark::parse_jsonl(scored_dataset, "scored.jsonl");
    check(scored.error.ok(), "benchmark scorer fixture parses");
    ainiux::benchmark::ScoreResult empty_response =
        ainiux::benchmark::score_response(scored.dataset.cases[0], 1, "");
    check(empty_response.configured && !empty_response.passed,
          "contains scorer fails on an empty response");
    ainiux::benchmark::Case exact_case;
    exact_case.id = "exact-zero";
    exact_case.category = "reasoning";
    exact_case.turns = {"0"};
    exact_case.expectations = {{"exact", "0", 1}};
    ainiux::benchmark::ScoreResult exact_zero =
        ainiux::benchmark::score_response(exact_case, 1, "0");
    check(exact_zero.configured && exact_zero.passed,
          "exact scorer accepts a zero-valued response");
}

void test_benchmark_grading_interfaces() {
    const char* grade_argv[] = {
        "ainiux", "--grade", "--grade-input", "custom-results.jsonl",
        "--category", "reasoning", "--case", "reasoning-01", "--limit", "2",
        "--concurrency", "3", "--summary-format", "csv"};
    ainiux::cli::ParseResult grade_cli = ainiux::cli::parse_args(
        14, const_cast<char**>(grade_argv));
    check(grade_cli.error.ok() && grade_cli.options.grade &&
              !grade_cli.options.stream && grade_cli.options.has_temperature &&
              grade_cli.options.temperature == 0.0 &&
              grade_cli.options.grade_input == "custom-results.jsonl" &&
              grade_cli.options.benchmark_concurrency == 3 &&
              grade_cli.options.benchmark_summary_format == "csv",
          "grading CLI parses shared filters and defaults judge requests to temperature zero and non-streaming");
    const char* streamed_grade_argv[] = {"ainiux", "--stream", "--grade",
                                         "--temperature", "0.25"};
    grade_cli = ainiux::cli::parse_args(
        5, const_cast<char**>(streamed_grade_argv));
    check(grade_cli.error.ok() && grade_cli.options.stream &&
              grade_cli.options.stream_explicit &&
              grade_cli.options.temperature == 0.25,
          "explicit grading stream and temperature settings override defaults");
    ainiux::cli::Options configured_options;
    configured_options.stream = true;
    configured_options.stream_explicit = true;
    configured_options.has_temperature = true;
    configured_options.temperature = 0.7;
    const char* configured_grade_argv[] = {"ainiux", "--grade"};
    grade_cli = ainiux::cli::parse_args(
        2, const_cast<char**>(configured_grade_argv), configured_options);
    check(grade_cli.error.ok() && !grade_cli.options.stream &&
              grade_cli.options.has_temperature && grade_cli.options.temperature == 0.0,
          "grading defaults override configured stream and temperature settings without CLI overrides");
    const char* incompatible_argv[] = {
        "ainiux", "--grade", "--dataset", "builtin", "--mode", "quality",
        "--runs", "2", "--warmup", "1", "--duration", "1s"};
    grade_cli = ainiux::cli::parse_args(
        12, const_cast<char**>(incompatible_argv));
    check(grade_cli.error.ok() && grade_cli.options.grade &&
              grade_cli.options.benchmark_dataset_explicit &&
              grade_cli.options.benchmark_mode_explicit &&
              grade_cli.options.benchmark_runs_explicit &&
              grade_cli.options.benchmark_warmup_explicit &&
              grade_cli.options.benchmark_duration_explicit,
          "grading CLI records benchmark-only combinations for mode validation");

    ainiux::cli::BenchmarkGradingPrompts prompts;
    prompts.system_prompt = "system";
    prompts.case_prompt = "prefix\n{{benchmark_case_json}}\nsuffix";
    const std::string payload =
        "{\"transcript\":[{\"content\":\"untrusted {{benchmark_case_json}} "
        "\\\" data\"}]}";
    std::string rendered;
    ainiux::Error err = ainiux::benchmark::render_grading_case_prompt(
        prompts, payload, rendered);
    check(err.ok() && rendered == "prefix\n" + payload + "\nsuffix",
          "grading prompt rendering performs exactly one placeholder replacement without appending instructions");

    const std::string good_judge =
        "{\"score\":87,\"verdict\":\"partial\",\"rationale\":\"Mostly "
        "correct.\",\"criteria\":[{\"index\":1,\"verdict\":\"partial\","
        "\"reason\":\"Needs detail.\"},{\"index\":0,\"verdict\":\"met\","
        "\"reason\":\"Correct.\"}]}";
    ainiux::benchmark::JudgeGrade judge_grade;
    err = ainiux::benchmark::parse_judge_grade(good_judge, 2, judge_grade);
    check(err.ok() && judge_grade.score == 87 &&
              judge_grade.verdict == "partial" &&
              judge_grade.criteria.size() == 2 &&
              judge_grade.criteria[0].index == 0,
          "strict judge parser validates and orders complete criterion findings");
    const std::vector<std::string> invalid_judges = {
        "not json",
        "{\"score\":101,\"verdict\":\"pass\",\"rationale\":\"x\","
        "\"criteria\":[]}",
        "{\"score\":50,\"verdict\":\"maybe\",\"rationale\":\"x\","
        "\"criteria\":[]}",
        "{\"score\":50,\"verdict\":\"partial\",\"rationale\":\"x\","
        "\"criteria\":[{\"index\":0,\"verdict\":\"met\",\"reason\":\"x\"},"
        "{\"index\":0,\"verdict\":\"met\",\"reason\":\"x\"}]}",
        "{\"score\":50,\"verdict\":\"partial\",\"rationale\":\"x\","
        "\"criteria\":[{\"index\":0,\"verdict\":\"unknown\",\"reason\":\"x\"}]}"
    };
    for (size_t index = 0; index < invalid_judges.size(); ++index) {
        const size_t expected = index == 3 ? 2U : (index == 4 ? 1U : 0U);
        check(!ainiux::benchmark::parse_judge_grade(
                   invalid_judges[index], expected, judge_grade)
                   .ok(),
              "judge parser rejects malformed schema case " +
                  std::to_string(index + 1));
    }

    std::istringstream missing_metadata(
        "{\"id\":\"generic\",\"category\":\"custom\",\"turns\":[\"x\"]}\n");
    ainiux::benchmark::LoadResult invalid_dataset =
        ainiux::benchmark::parse_jsonl(missing_metadata, "ungradeable.jsonl");
    check(!invalid_dataset.error.ok() &&
              invalid_dataset.error.message.find("reference_answer") !=
                  std::string::npos &&
              invalid_dataset.error.message.find("assessment_criteria") !=
                  std::string::npos,
          "all dataset categories require grading evaluation metadata");
    std::istringstream blank_metadata(
        "{\"id\":\"blank\",\"category\":\"custom\",\"turns\":[\"x\"],"
        "\"assessment_criteria\":[\"   \"]}\n");
    invalid_dataset =
        ainiux::benchmark::parse_jsonl(blank_metadata, "blank-metadata.jsonl");
    check(!invalid_dataset.error.ok() &&
              invalid_dataset.error.message.find("assessment_criteria[0]") !=
                  std::string::npos,
          "whitespace-only evaluation metadata cannot make a case gradeable");

    const std::filesystem::path discovery = "build/grade-discovery";
    std::filesystem::remove_all(discovery);
    std::filesystem::create_directories(discovery);
    const auto write_result = [](const std::filesystem::path& path,
                                 const std::string& id,
                                 const std::string& category) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "{\"type\":\"result\",\"id\":" << ainiux::json::quote(id)
             << ",\"category\":" << ainiux::json::quote(category)
             << ",\"run\":1,\"turn\":1,\"ok\":true,\"prompt\":\"p\","
                "\"response\":\"r\",\"reference_answer\":\"r\"}\n"
                "{\"type\":\"summary\"}\n";
    };
    const std::filesystem::path matching = discovery / "benchmark-a.jsonl";
    const std::filesystem::path invalid_newest = discovery / "benchmark-b.jsonl";
    const std::filesystem::path wrong_category = discovery / "benchmark-c.jsonl";
    write_result(matching, "wanted", "reasoning");
    {
        std::ofstream file(invalid_newest);
        file << "not-json\n";
    }
    write_result(wrong_category, "other", "coding");
    const auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(matching, now - std::chrono::seconds(2));
    std::filesystem::last_write_time(invalid_newest, now);
    std::filesystem::last_write_time(wrong_category, now - std::chrono::seconds(1));
    ainiux::cli::Options discovery_options;
    discovery_options.output_path = discovery.string() + "/";
    discovery_options.benchmark_category = "reasoning";
    std::string selected_path;
    err = ainiux::benchmark::find_grade_input(discovery_options, selected_path);
    check(err.ok() && std::filesystem::path(selected_path).filename() ==
                          matching.filename(),
          "automatic grading input skips newer invalid and non-matching benchmark files");
    const std::filesystem::path custom = discovery / "custom-results.jsonl";
    write_result(custom, "explicit", "reasoning");
    discovery_options.grade_input = custom.string();
    err = ainiux::benchmark::find_grade_input(discovery_options, selected_path);
    check(err.ok() && selected_path == custom.string(),
          "--grade-input permits an explicit custom-named result file");

    const std::string grade_jsonl = "build/grade-report-test.jsonl";
    const std::string grade_markdown = "build/grade-report-test.md";
    {
        std::ofstream file(grade_jsonl, std::ios::binary | std::ios::trunc);
        file << "{\"type\":\"grade\",\"id\":\"case|grade\",\"run\":2,"
                "\"ok\":true,\"score\":90,\"verdict\":\"pass\","
                "\"rationale\":\"Good <work>.\",\"transcript\":["
                "{\"role\":\"user\",\"content\":\"question\"},"
                "{\"role\":\"assistant\",\"content\":\"answer```\"}],"
                "\"evaluation_basis\":{\"reference_answer\":\"answer\","
                "\"assessment_criteria\":[],\"evaluation_items\":["
                "{\"index\":0,\"kind\":\"reference_answer_semantic_agreement\"}],"
                "\"safety\":{\"classification\":\"sensitive\","
                "\"expected_action\":\"reject\"}},"
                "\"criteria\":[{\"index\":0,\"verdict\":\"met\","
                "\"reason\":\"Matches.\"}]}\n"
                "{\"type\":\"summary\",\"mode\":\"grade\","
                "\"graded_count\":1,\"error_count\":0}\n";
    }
    err = ainiux::benchmark::write_markdown_report(grade_jsonl, grade_markdown);
    const std::string grade_report = read_fixture(grade_markdown);
    check(err.ok() &&
              grade_report.find("# ainiux Benchmark Grading Report") !=
                  std::string::npos &&
              grade_report.find("## Grades") != std::string::npos &&
              grade_report.find("### case\\|grade - Run 2") !=
                  std::string::npos &&
              grade_report.find("#### Transcript") != std::string::npos &&
              grade_report.find("#### Evaluation Basis") != std::string::npos &&
              grade_report.find("##### Safety Rating") != std::string::npos &&
              grade_report.find("sensitive") != std::string::npos &&
              grade_report.find("#### Criterion Findings") !=
                  std::string::npos &&
              grade_report.find("#### Rationale") != std::string::npos,
          "grading Markdown report exposes auditable transcript, basis, findings, score, verdict, and rationale");
}

}  // namespace

void run_all() {
    test_benchmark_cli_and_jsonl_dataset();
    test_benchmark_dataset_io_and_scoring_edge_cases();
    test_benchmark_grading_interfaces();
}

}  // namespace ainiux::test::benchmark
