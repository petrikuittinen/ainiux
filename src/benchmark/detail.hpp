#pragma once

#include <iosfwd>
#include <string>
#include <vector>

#include "cli/args.hpp"
#include "fetch/fetch.hpp"
#include "provider/provider.hpp"

namespace pkchat::benchmark {

struct Case;

namespace detail {

fetch::Options fetch_options_for(const cli::Options& options);
void write_string_array(std::ostream& output, const std::vector<std::string>& values);
void write_result_tags(std::ostream& output, const Case& benchmark_case);
void write_result_case_metadata(std::ostream& output,
                                const Case& benchmark_case,
                                size_t turn_index);
std::vector<std::string> split_modes(const std::string& text);
bool contains_mode(const std::vector<std::string>& modes, const std::string& wanted);
long long saturating_add(long long left, long long right);
void write_json_metric(std::ostream& output, double value);
void write_json_integer_or_null(std::ostream& output, long long value);
double wall_tokens_per_second(const provider::ChatResult& result);
size_t worker_count_for(size_t tasks, int requested);

}  // namespace detail
}  // namespace pkchat::benchmark