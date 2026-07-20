#pragma once

#include <cstddef>
#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

#include "agent/prompts.hpp"
#include "agent/tools.hpp"
#include "common.hpp"
#include "provider/provider.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::agent {

class ReviewLogger;

struct Finding {
    std::string id;
    std::string title;
    std::string severity;
    std::string confidence;
    std::string category;
    std::string cwe;
    std::string path;
    std::size_t line_start = 1;
    std::size_t line_end = 1;
    std::string impact;
    std::string remediation;
    std::string evidence;
    bool coordinator = false;
};

struct FileCoverage {
    std::string path;
    std::string status;
    std::string detail;
};

struct ReviewReport {
    std::string workspace;
    std::string provider;
    std::string model;
    std::string api;
    std::string reasoning;
    long long index_updated_at = 0;
    long long reviewed_at = 0;
    std::size_t batch_size = 0;
    std::size_t parallel_agents = 0;
    std::size_t logical_batches = 0;
    std::vector<index::LanguageTotal> languages;
    std::vector<Finding> findings;
    std::vector<FileCoverage> coverage;
    std::vector<std::string> errors;
    bool complete = true;
};

using ProgressCallback = std::function<void(const std::string&)>;

struct ReviewChunkPlan {
    std::size_t byte_start = 0;
    std::size_t byte_end = 0;
    std::size_t line_start = 1;
    std::size_t line_end = 1;
};

std::vector<std::vector<std::string>> plan_review_batches(const index::Snapshot& snapshot,
                                                          std::size_t batch_size);
std::vector<ReviewChunkPlan> plan_review_chunks(const std::string& source,
                                                std::size_t batch_size);
Error parse_review_worker_output(const std::string& text,
                                 const index::Snapshot& snapshot,
                                 const std::vector<std::string>& expected_coverage,
                                 std::vector<Finding>& findings);

Error run_review(const provider::RequestContext& context,
                 const TrustedPrompts& prompts,
                 const ReadToolRegistry& tools,
                 std::size_t batch_size,
                 std::size_t max_parallel_agents,
                 runtime::CancellationToken cancellation,
                 ProgressCallback progress,
                 ReviewReport& report,
                 ReviewLogger* logger = nullptr);

Error render_review_markdown(const ReviewReport& report, std::ostream& output);

}  // namespace ainiux::agent
