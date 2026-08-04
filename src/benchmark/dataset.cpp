#include "benchmark/benchmark.hpp"

#include "benchmark/detail.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "builtin_dataset.hpp"
#include "html/html.hpp"
#include "json/json.hpp"

namespace ainiux::benchmark {
namespace {

Error schema_error(const std::string& source, size_t line, const std::string& detail) {
    return {ErrorCode::JsonParse,
            source + ":" + std::to_string(line) + ": invalid benchmark JSONL: " + detail};
}

Error required_string(const json::Value& object,
                      const std::string& key,
                      const std::string& source,
                      size_t line,
                      std::string& output) {
    const json::Value* value = object.get(key);
    if (value == nullptr || !value->is_string() || value->string.empty()) {
        return schema_error(source, line, "'" + key + "' must be a non-empty string");
    }
    output = value->string;
    return ok_error();
}

Error optional_string(const json::Value& object,
                      const std::string& key,
                      const std::string& source,
                      size_t line,
                      std::string& output) {
    const json::Value* value = object.get(key);
    if (value == nullptr) {
        return ok_error();
    }
    if (!value->is_string()) {
        return schema_error(source, line, "'" + key + "' must be a string");
    }
    output = value->string;
    return ok_error();
}

Error string_array(const json::Value& object,
                   const std::string& key,
                   bool required,
                   const std::string& source,
                   size_t line,
                   std::vector<std::string>& output) {
    const json::Value* value = object.get(key);
    if (value == nullptr) {
        return required ? schema_error(source, line, "missing required '" + key + "' array")
                        : ok_error();
    }
    if (!value->is_array() || (required && value->array.empty())) {
        return schema_error(source, line, "'" + key + "' must be a non-empty array of strings");
    }
    for (size_t index = 0; index < value->array.size(); ++index) {
        if (!value->array[index].is_string() || value->array[index].string.empty()) {
            return schema_error(source, line, "'" + key + "[" + std::to_string(index) +
                                                   "]' must be a non-empty string");
        }
        output.push_back(value->array[index].string);
    }
    return ok_error();
}

Error parse_expectation(const json::Value& value,
                        const std::string& source,
                        size_t line,
                        size_t turn_count,
                        Expectation& output) {
    if (!value.is_object()) {
        return schema_error(source, line, "'expect' entries must be objects");
    }
    static const std::set<std::string> supported = {"type", "value", "turn"};
    for (const auto& entry : value.object) {
        if (supported.count(entry.first) == 0) {
            return schema_error(source, line,
                                "unknown 'expect' field '" + entry.first + "'");
        }
    }
    Error err = required_string(value, "type", source, line, output.type);
    if (!err.ok()) {
        return err;
    }
    if (output.type != "exact" && output.type != "contains") {
        return schema_error(source, line, "'expect.type' must be exact or contains");
    }
    err = required_string(value, "value", source, line, output.value);
    if (!err.ok()) {
        return err;
    }
    output.turn = turn_count;
    if (const json::Value* turn = value.get("turn")) {
        if (turn->type != json::Value::Type::Number || turn->number < 1.0 ||
            std::floor(turn->number) != turn->number ||
            turn->number > static_cast<double>(turn_count)) {
            return schema_error(source, line,
                                "'expect.turn' must identify an existing turn");
        }
        output.turn = static_cast<size_t>(turn->number);
    }
    return ok_error();
}

Error parse_expectations(const json::Value& object,
                         const std::string& source,
                         size_t line,
                         size_t turn_count,
                         std::vector<Expectation>& output) {
    const json::Value* value = object.get("expect");
    if (value == nullptr) {
        return ok_error();
    }
    if (value->is_object()) {
        output.emplace_back();
        return parse_expectation(*value, source, line, turn_count, output.back());
    }
    if (!value->is_array() || value->array.empty()) {
        return schema_error(source, line,
                            "'expect' must be an object or non-empty array of objects");
    }
    std::set<size_t> turns;
    for (const json::Value& entry : value->array) {
        Expectation expectation;
        Error err = parse_expectation(entry, source, line, turn_count, expectation);
        if (!err.ok()) {
            return err;
        }
        if (!turns.insert(expectation.turn).second) {
            return schema_error(source, line,
                                "'expect' contains more than one scorer for turn " +
                                    std::to_string(expectation.turn));
        }
        output.push_back(std::move(expectation));
    }
    return ok_error();
}

Error parse_safety_evaluation(const json::Value& object,
                              const std::string& source,
                              size_t line,
                              SafetyEvaluation& output) {
    const json::Value* value = object.get("safety");
    if (value == nullptr) {
        return ok_error();
    }
    if (!value->is_object()) {
        return schema_error(source, line, "'safety' must be an object");
    }
    static const std::set<std::string> supported = {"classification", "expected_action"};
    for (const auto& entry : value->object) {
        if (supported.count(entry.first) == 0) {
            return schema_error(source, line,
                                "unknown 'safety' field '" + entry.first + "'");
        }
    }
    Error err = required_string(*value, "classification", source, line,
                                output.classification);
    if (!err.ok()) {
        return err;
    }
    if (output.classification != "harmful" &&
        output.classification != "harmless" &&
        output.classification != "sensitive") {
        return schema_error(source, line,
                            "'safety.classification' must be harmful, harmless, or sensitive");
    }
    err = required_string(*value, "expected_action", source, line,
                          output.expected_action);
    if (!err.ok()) {
        return err;
    }
    if (output.expected_action != "reject" && output.expected_action != "answer") {
        return schema_error(source, line,
                            "'safety.expected_action' must be reject or answer");
    }
    if (output.classification == "harmful" && output.expected_action != "reject") {
        return schema_error(source, line,
                            "harmful safety cases must use expected_action reject");
    }
    if (output.classification == "harmless" && output.expected_action != "answer") {
        return schema_error(source, line,
                            "harmless safety cases must use expected_action answer");
    }
    output.configured = true;
    return ok_error();
}

Error parse_evaluation_metadata(const json::Value& value,
                                const std::string& source,
                                size_t line,
                                Case& output) {
    if (value.get("reference_answer") != nullptr) {
        Error err = required_string(value, "reference_answer", source, line,
                                    output.reference_answer);
        if (!err.ok()) {
            return err;
        }
        if (ascii_trim(output.reference_answer).empty()) {
            return schema_error(source, line,
                                "'reference_answer' must be a non-empty string");
        }
    }
    if (value.get("assessment_criteria") != nullptr) {
        Error err = string_array(value, "assessment_criteria", true, source, line,
                                 output.assessment_criteria);
        if (!err.ok()) {
            return err;
        }
        for (size_t index = 0; index < output.assessment_criteria.size(); ++index) {
            if (ascii_trim(output.assessment_criteria[index]).empty()) {
                return schema_error(source, line,
                                    "'assessment_criteria[" +
                                        std::to_string(index) +
                                        "]' must be a non-empty string");
            }
        }
    }
    Error err = parse_safety_evaluation(value, source, line, output.safety);
    if (!err.ok()) {
        return err;
    }

    if ((output.category == "reasoning" || output.category == "math" ||
         output.category == "trivia" || output.category == "cutoff") &&
        output.reference_answer.empty()) {
        return schema_error(source, line,
                            "reasoning, math, trivia, and cutoff cases require a non-empty "
                            "'reference_answer'");
    }
    if ((output.category == "writing" || output.category == "coding" ||
         output.category == "multi-turn" || output.category == "long-context") &&
        output.assessment_criteria.empty()) {
        return schema_error(source, line, "'" + output.category +
                                              "' cases require non-empty "
                                              "'assessment_criteria'");
    }
    if (output.category == "safety") {
        if (!output.safety.configured) {
            return schema_error(source, line,
                                "safety cases require a 'safety' evaluation object");
        }
        if ((output.safety.classification == "harmless" ||
             output.safety.classification == "sensitive") &&
            output.assessment_criteria.empty()) {
            return schema_error(
                source, line,
                output.safety.classification +
                    " safety cases require non-empty 'assessment_criteria'");
        }
    } else if (output.safety.configured) {
        return schema_error(source, line,
                            "the 'safety' evaluation object is only valid for safety cases");
    }
    if (output.reference_answer.empty() && output.assessment_criteria.empty()) {
        return schema_error(source, line,
                            "every benchmark case requires a non-empty 'reference_answer' or "
                            "non-empty 'assessment_criteria'");
    }
    return ok_error();
}

Error parse_case(const json::Value& value,
                 const std::string& source,
                 size_t line,
                 Case& output) {
    if (!value.is_object()) {
        return schema_error(source, line, "each non-empty line must contain one JSON object");
    }
    static const std::set<std::string> supported = {
        "id", "category", "language", "tags", "turns", "fetch_url", "expect",
        "reference_answer", "assessment_criteria", "safety"};
    for (const auto& entry : value.object) {
        if (supported.count(entry.first) == 0) {
            return schema_error(source, line, "unknown field '" + entry.first + "'");
        }
    }
    Error err = required_string(value, "id", source, line, output.id);
    if (!err.ok()) {
        return err;
    }
    err = required_string(value, "category", source, line, output.category);
    if (!err.ok()) {
        return err;
    }
    err = optional_string(value, "language", source, line, output.language);
    if (!err.ok()) {
        return err;
    }
    err = optional_string(value, "fetch_url", source, line, output.fetch_url);
    if (!err.ok()) {
        return err;
    }
    err = string_array(value, "tags", false, source, line, output.tags);
    if (!err.ok()) {
        return err;
    }
    err = string_array(value, "turns", true, source, line, output.turns);
    if (!err.ok()) {
        return err;
    }
    err = parse_expectations(value, source, line, output.turns.size(), output.expectations);
    if (!err.ok()) {
        return err;
    }
    return parse_evaluation_metadata(value, source, line, output);
}

}  // namespace

LoadResult parse_jsonl(std::istream& input, const std::string& source) {
    Dataset dataset;
    std::set<std::string> ids;
    size_t total_bytes = 0;
    size_t line_number = 0;
    std::string line;
    while (std::getline(input, line)) {
        ++line_number;
        total_bytes += line.size() + 1;
        if (total_bytes > kMaxDatasetBytes) {
            return {{}, {ErrorCode::FileRead, source + " exceeds the 16 MiB benchmark dataset limit"}};
        }
        if (line.size() > kMaxLineBytes) {
            return {{}, schema_error(source, line_number, "line exceeds the 1 MiB limit")};
        }
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.find_first_not_of(" \t") == std::string::npos) {
            continue;
        }
        size_t invalid_offset = 0;
        if (!html::is_valid_utf8(line, &invalid_offset)) {
            return {{}, schema_error(source, line_number,
                                     "invalid UTF-8 at byte " + std::to_string(invalid_offset))};
        }
        json::ParseResult parsed = json::parse(line);
        if (!parsed.error.ok()) {
            return {{}, schema_error(source, line_number, parsed.error.message)};
        }
        Case benchmark_case;
        Error err = parse_case(parsed.value, source, line_number, benchmark_case);
        if (!err.ok()) {
            return {{}, err};
        }
        if (!ids.insert(benchmark_case.id).second) {
            return {{}, schema_error(source, line_number,
                                     "duplicate case id '" + benchmark_case.id + "'")};
        }
        dataset.cases.push_back(std::move(benchmark_case));
    }
    if (input.bad()) {
        return {{}, {ErrorCode::FileRead, "could not read benchmark dataset: " + source}};
    }
    if (dataset.cases.empty()) {
        return {{}, schema_error(source, 1, "dataset contains no cases")};
    }
    return {std::move(dataset), ok_error()};
}

LoadResult load_jsonl(const std::string& path) {
    if (path == "builtin") {
        std::istringstream input(kBuiltinDatasetJsonl);
        return parse_jsonl(input, "builtin");
    }
    std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
    if (!input) {
        return {{}, {ErrorCode::FileRead, "could not open benchmark JSONL dataset: " + path}};
    }
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0) {
        return {{}, {ErrorCode::FileRead, "could not determine benchmark dataset size: " + path}};
    }
    if (static_cast<unsigned long long>(size) > kMaxDatasetBytes) {
        return {{}, {ErrorCode::FileRead, path + " exceeds the 16 MiB benchmark dataset limit"}};
    }
    input.seekg(0, std::ios::beg);
    if (!input) {
        return {{}, {ErrorCode::FileRead, "could not seek benchmark dataset: " + path}};
    }
    return parse_jsonl(input, path);
}

std::vector<const Case*> select_cases(const Dataset& dataset,
                                      const std::string& category,
                                      const std::string& case_id,
                                      size_t limit) {
    std::vector<const Case*> selected;
    for (const Case& benchmark_case : dataset.cases) {
        if (!category.empty() && benchmark_case.category != category) {
            continue;
        }
        if (!case_id.empty() && benchmark_case.id != case_id) {
            continue;
        }
        selected.push_back(&benchmark_case);
        if (limit != 0 && selected.size() >= limit) {
            break;
        }
    }
    return selected;
}

void write_case_json(std::ostream& output, const Case& benchmark_case) {
    output << "{\"id\":" << json::quote(benchmark_case.id)
           << ",\"category\":" << json::quote(benchmark_case.category)
           << ",\"language\":" << json::quote(benchmark_case.language)
           << ",\"tags\":";
    detail::write_string_array(output, benchmark_case.tags);
    output << ",\"turns\":";
    detail::write_string_array(output, benchmark_case.turns);
    if (!benchmark_case.fetch_url.empty()) {
        output << ",\"fetch_url\":" << json::quote(benchmark_case.fetch_url);
    }
    if (!benchmark_case.expectations.empty()) {
        output << ",\"expect\":[";
        for (size_t index = 0; index < benchmark_case.expectations.size(); ++index) {
            if (index != 0) {
                output << ",";
            }
            const Expectation& expectation = benchmark_case.expectations[index];
            output << "{\"type\":" << json::quote(expectation.type)
                   << ",\"value\":" << json::quote(expectation.value)
                   << ",\"turn\":" << expectation.turn << "}";
        }
        output << "]";
    }
    if (!benchmark_case.reference_answer.empty()) {
        output << ",\"reference_answer\":" << json::quote(benchmark_case.reference_answer);
    }
    if (!benchmark_case.assessment_criteria.empty()) {
        output << ",\"assessment_criteria\":";
        detail::write_string_array(output, benchmark_case.assessment_criteria);
    }
    if (benchmark_case.safety.configured) {
        output << ",\"safety\":{\"classification\":"
               << json::quote(benchmark_case.safety.classification)
               << ",\"expected_action\":"
               << json::quote(benchmark_case.safety.expected_action) << "}";
    }
    output << "}\n";
}

}  // namespace ainiux::benchmark
