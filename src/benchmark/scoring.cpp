#include "benchmark/benchmark.hpp"

namespace ainiux::benchmark {

ScoreResult score_response(const Case& benchmark_case,
                           size_t turn,
                           const std::string& response) {
    for (const Expectation& expectation : benchmark_case.expectations) {
        if (expectation.turn != turn) {
            continue;
        }
        ScoreResult result;
        result.configured = true;
        result.method = expectation.type;
        if (expectation.type == "exact") {
            result.passed = response == expectation.value;
        } else if (expectation.type == "contains") {
            result.passed = response.find(expectation.value) != std::string::npos;
        }
        return result;
    }
    return {};
}

}  // namespace ainiux::benchmark