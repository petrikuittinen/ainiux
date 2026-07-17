#include "benchmark/detail.hpp"

#include "benchmark/benchmark.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>

#include "json/json.hpp"

namespace ainiux::benchmark::detail {

fetch::Options fetch_options_for(const cli::Options& options) {
    fetch::Options fetch_options;
    fetch_options.connect_timeout_seconds = options.connect_timeout_seconds;
    fetch_options.timeout_seconds = options.timeout_seconds > 0 ? options.timeout_seconds : 30;
    fetch_options.max_bytes = options.max_fetch_bytes;
    fetch_options.proxy = options.proxy;
    fetch_options.insecure_tls = options.insecure_tls;
    fetch_options.trace_http = options.trace_http;
    fetch_options.allow_private = options.allow_private_url_fetch;
    return fetch_options;
}

void write_string_array(std::ostream& output, const std::vector<std::string>& values) {
    output << "[";
    for (size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            output << ",";
        }
        output << json::quote(values[index]);
    }
    output << "]";
}

void write_result_tags(std::ostream& output, const Case& benchmark_case) {
    const bool harmful = benchmark_case.safety.configured &&
                         benchmark_case.safety.classification == "harmful";
    const bool sensitive = benchmark_case.safety.configured &&
                           benchmark_case.safety.classification == "sensitive";
    const bool already_harmful_tagged =
        std::find(benchmark_case.tags.begin(), benchmark_case.tags.end(),
                  "harmful-request") != benchmark_case.tags.end();
    const bool already_sensitive_tagged =
        std::find(benchmark_case.tags.begin(), benchmark_case.tags.end(),
                  "policy-sensitive") != benchmark_case.tags.end();
    output << "[";
    bool wrote_tag = false;
    for (size_t index = 0; index < benchmark_case.tags.size(); ++index) {
        if (wrote_tag) {
            output << ",";
        }
        output << json::quote(benchmark_case.tags[index]);
        wrote_tag = true;
    }
    if (harmful && !already_harmful_tagged) {
        if (wrote_tag) {
            output << ",";
        }
        output << json::quote("harmful-request");
        wrote_tag = true;
    }
    if (sensitive && !already_sensitive_tagged) {
        if (wrote_tag) {
            output << ",";
        }
        output << json::quote("policy-sensitive");
    }
    output << "]";
}

void write_result_case_metadata(std::ostream& output,
                                const Case& benchmark_case,
                                size_t turn_index) {
    output << ",\"prompt\":" << json::quote(benchmark_case.turns[turn_index])
           << ",\"tags\":";
    write_result_tags(output, benchmark_case);
    if (!benchmark_case.fetch_url.empty()) {
        output << ",\"external_file_url\":" << json::quote(benchmark_case.fetch_url);
    }
    if (!benchmark_case.reference_answer.empty()) {
        output << ",\"reference_answer\":" << json::quote(benchmark_case.reference_answer);
    }
    if (!benchmark_case.assessment_criteria.empty()) {
        output << ",\"assessment_criteria\":";
        write_string_array(output, benchmark_case.assessment_criteria);
    }
    if (benchmark_case.safety.configured) {
        output << ",\"safety\":{\"classification\":"
               << json::quote(benchmark_case.safety.classification)
               << ",\"expected_action\":"
               << json::quote(benchmark_case.safety.expected_action) << "}";
    }
}

std::vector<std::string> split_modes(const std::string& text) {
    std::vector<std::string> modes;
    size_t start = 0;
    while (start < text.size()) {
        const size_t comma = text.find(',', start);
        modes.push_back(text.substr(start, comma == std::string::npos
                                               ? std::string::npos
                                               : comma - start));
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return modes;
}

bool contains_mode(const std::vector<std::string>& modes, const std::string& wanted) {
    return std::find(modes.begin(), modes.end(), wanted) != modes.end();
}

long long saturating_add(long long left, long long right) {
    if (right > 0 && left > std::numeric_limits<long long>::max() - right) {
        return std::numeric_limits<long long>::max();
    }
    return left + right;
}

void write_json_metric(std::ostream& output, double value) {
    if (value < 0.0 || !std::isfinite(value)) {
        output << "null";
    } else {
        output << std::fixed << std::setprecision(3) << value;
    }
}

void write_json_integer_or_null(std::ostream& output, long long value) {
    if (value < 0) {
        output << "null";
    } else {
        output << value;
    }
}

double wall_tokens_per_second(const provider::ChatResult& result) {
    return static_cast<double>(result.completion_tokens) * 1000.0 /
           static_cast<double>(std::max<long long>(1, result.total_ms));
}

size_t worker_count_for(size_t tasks, int requested) {
    return std::max<size_t>(1, std::min(tasks, static_cast<size_t>(requested)));
}

}  // namespace ainiux::benchmark::detail
