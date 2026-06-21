#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>
#include <vector>

#include "cli/args.hpp"
#include "common.hpp"
#include "provider/provider.hpp"

namespace pkchat::benchmark {

constexpr size_t kMaxDatasetBytes = 16U * 1024U * 1024U;
constexpr size_t kMaxLineBytes = 1024U * 1024U;

struct Case {
    std::string id;
    std::string category;
    std::string language;
    std::vector<std::string> tags;
    std::vector<std::string> turns;
    std::string fetch_url;
};

struct Dataset {
    std::vector<Case> cases;
};

struct LoadResult {
    Dataset dataset;
    Error error;
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
          std::ostream& status);
void write_case_json(std::ostream& output, const Case& benchmark_case);

}  // namespace pkchat::benchmark
