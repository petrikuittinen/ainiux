#pragma once

#include <cstddef>
#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

#include "cli/args.hpp"
#include "common.hpp"
#include "provider/provider.hpp"

namespace pkchat::benchmark {

constexpr size_t kMaxDatasetBytes = 16U * 1024U * 1024U;
constexpr size_t kMaxLineBytes = 1024U * 1024U;

struct Expectation {
    std::string type;
    std::string value;
    size_t turn = 0;
};

struct SafetyEvaluation {
    bool configured = false;
    std::string classification;
    std::string expected_action;
};

struct Case {
    std::string id;
    std::string category;
    std::string language;
    std::vector<std::string> tags;
    std::vector<std::string> turns;
    std::string fetch_url;
    std::vector<Expectation> expectations;
    std::string reference_answer;
    std::vector<std::string> assessment_criteria;
    SafetyEvaluation safety;
};

struct ScoreResult {
    bool configured = false;
    bool passed = false;
    std::string method;
};

struct Dataset {
    std::vector<Case> cases;
};

struct LoadResult {
    Dataset dataset;
    Error error;
};

struct GradeCriterionFinding {
    size_t index = 0;
    std::string verdict;
    std::string reason;
};

struct JudgeGrade {
    int score = 0;
    std::string verdict;
    std::string rationale;
    std::vector<GradeCriterionFinding> criteria;
};

LoadResult parse_jsonl(std::istream& input, const std::string& source);
LoadResult load_jsonl(const std::string& path);
std::vector<const Case*> select_cases(const Dataset& dataset,
                                      const std::string& category,
                                      const std::string& case_id,
                                      size_t limit);
Error run(const provider::RequestContext& context,
          const std::vector<const Case*>& cases,
          const cli::Options& options,
          std::ostream& output,
          std::ostream& status,
          const std::function<bool()>& interrupt_requested = {});
void write_case_json(std::ostream& output, const Case& benchmark_case);
std::string markdown_report_path(const std::string& jsonl_path);
Error write_markdown_report(const std::string& jsonl_path,
                            const std::string& markdown_path);
Error find_grade_input(const cli::Options& options, std::string& path);
Error render_grading_case_prompt(const cli::BenchmarkGradingPrompts& prompts,
                                 const std::string& benchmark_case_json,
                                 std::string& prompt);
Error parse_judge_grade(const std::string& response,
                        size_t criterion_count,
                        JudgeGrade& grade);
Error grade(const provider::RequestContext& context,
            const std::string& source_path,
            const cli::Options& options,
            std::ostream& output,
            std::ostream& status,
            const std::function<bool()>& interrupt_requested = {});
ScoreResult score_response(const Case& benchmark_case,
                           size_t turn,
                           const std::string& response);

}  // namespace pkchat::benchmark
