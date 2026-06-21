#include "benchmark/benchmark.hpp"

#include <fstream>
#include <iomanip>
#include <istream>
#include <limits>
#include <set>
#include <sstream>

#include "fetch/fetch.hpp"
#include "html/html.hpp"
#include "json/json.hpp"
#include "output/thinking.hpp"
#include "builtin_dataset.hpp"

namespace pkchat::benchmark {
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

Error parse_case(const json::Value& value,
                 const std::string& source,
                 size_t line,
                 Case& output) {
    if (!value.is_object()) {
        return schema_error(source, line, "each non-empty line must contain one JSON object");
    }
    static const std::set<std::string> supported = {
        "id", "category", "language", "tags", "turns", "fetch_url"};
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
    return string_array(value, "turns", true, source, line, output.turns);
}

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
    std::ifstream input(path, std::ios::binary);
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
    write_string_array(output, benchmark_case.tags);
    output << ",\"turns\":";
    write_string_array(output, benchmark_case.turns);
    if (!benchmark_case.fetch_url.empty()) {
        output << ",\"fetch_url\":" << json::quote(benchmark_case.fetch_url);
    }
    output << "}\n";
}

Error run(const provider::RequestContext& context,
          const std::vector<const Case*>& cases,
          const cli::Options& options,
          std::ostream& output,
          std::ostream& status) {
    if (cases.empty()) {
        return {ErrorCode::BadArgs, "benchmark selection matched no dataset cases"};
    }
    provider::RequestContext request_context = context;
    if (request_context.profile.offline) {
        return {ErrorCode::UnsupportedFeature,
                "benchmark execution requires a model provider; use --validate-dataset or --list-cases offline"};
    }
    if (request_context.options.model.empty()) {
        provider::ModelsResult models;
        Error err = provider::list_models(request_context, models);
        if (!err.ok()) {
            return err;
        }
        if (!models.model_ids.empty()) {
            request_context.options.model = models.model_ids.front();
        }
    }
    if (request_context.options.model.empty()) {
        return {ErrorCode::BadArgs, "benchmark requires --model when the provider does not list a model"};
    }

    size_t completed_cases = 0;
    size_t failed_cases = 0;
    for (const Case* benchmark_case : cases) {
        std::string fetched_context;
        if (!benchmark_case->fetch_url.empty()) {
            if (!options.quiet) {
                status << "Fetching benchmark context: " << benchmark_case->fetch_url << "\n";
            }
            Error err = fetch::fetch_text(benchmark_case->fetch_url,
                                          fetch_options_for(options), fetched_context);
            if (!err.ok()) {
                return err;
            }
        }
        for (int run_index = -options.benchmark_warmup;
             run_index < options.benchmark_runs;
             ++run_index) {
            const bool warmup = run_index < 0;
            std::vector<provider::Message> messages;
            if (!fetched_context.empty()) {
                messages.push_back({"user", "Reference text fetched from " + benchmark_case->fetch_url +
                                               ":\n\n" + fetched_context});
            }
            bool failed = false;
            for (size_t turn_index = 0; turn_index < benchmark_case->turns.size(); ++turn_index) {
                messages.push_back({"user", benchmark_case->turns[turn_index]});
                provider::ChatResult result;
                Error err = provider::send_chat_messages(
                    request_context, messages,
                    [](const std::string&) { return ok_error(); }, result);
                if (!err.ok()) {
                    failed = true;
                    if (!warmup) {
                        output << "{\"type\":\"result\",\"id\":" << json::quote(benchmark_case->id)
                               << ",\"category\":" << json::quote(benchmark_case->category)
                               << ",\"run\":" << (run_index + 1)
                               << ",\"turn\":" << (turn_index + 1)
                               << ",\"ok\":false,\"error_code\":"
                               << json::quote(error_code_name(err.code))
                               << ",\"error\":" << json::quote(err.message) << "}\n";
                    }
                    break;
                }
                messages.push_back({"assistant", result.content});
                if (!warmup) {
                    const ::pkchat::output::ThinkingChunk separated =
                        ::pkchat::output::split_thinking_traces(result.content);
                    output << "{\"type\":\"result\",\"id\":" << json::quote(benchmark_case->id)
                           << ",\"category\":" << json::quote(benchmark_case->category)
                           << ",\"language\":" << json::quote(benchmark_case->language)
                           << ",\"model\":" << json::quote(result.model.empty()
                                                                    ? request_context.options.model
                                                                    : result.model)
                           << ",\"provider\":" << json::quote(request_context.profile.name)
                           << ",\"run\":" << (run_index + 1)
                           << ",\"turn\":" << (turn_index + 1)
                           << ",\"ok\":true,\"ttft_ms\":" << result.ttft_ms
                           << ",\"total_ms\":" << result.total_ms
                           << ",\"completion_tokens\":" << result.completion_tokens
                           << ",\"token_count_source\":"
                           << json::quote(result.completion_tokens_estimated ? "estimated" : "provider_reported")
                           << ",\"tokens_per_second\":" << std::fixed << std::setprecision(3)
                           << provider::tokens_per_second(result, request_context.options.stream)
                           << ",\"thinking_trace_present\":"
                           << (separated.trace.empty() ? "false" : "true")
                           << ",\"response\":" << json::quote(separated.visible) << "}\n";
                }
            }
            if (!warmup) {
                if (failed) {
                    ++failed_cases;
                } else {
                    ++completed_cases;
                }
            }
        }
    }
    output << "{\"type\":\"summary\",\"selected_cases\":" << cases.size()
           << ",\"runs_per_case\":" << options.benchmark_runs
           << ",\"warmup_runs\":" << options.benchmark_warmup
           << ",\"completed_case_runs\":" << completed_cases
           << ",\"failed_case_runs\":" << failed_cases << "}\n";
    return failed_cases == 0
               ? ok_error()
               : Error{ErrorCode::ProviderSchema,
                       std::to_string(failed_cases) + " benchmark case run(s) failed"};
}

}  // namespace pkchat::benchmark
