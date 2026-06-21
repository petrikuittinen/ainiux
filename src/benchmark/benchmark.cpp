#include "benchmark/benchmark.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iomanip>
#include <istream>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include "context/context.hpp"
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

struct PreparedCase {
    const Case* benchmark_case = nullptr;
    std::string fetched_context;
};

struct Statistics {
    size_t completed_case_runs = 0;
    size_t failed_case_runs = 0;
    size_t cancelled_case_runs = 0;
    size_t completed_turns = 0;
    long long estimated_prompt_tokens = 0;
    long long completion_tokens = 0;
    long long ttft_sum_ms = 0;
    size_t ttft_samples = 0;
    double token_rate_sum = 0.0;
};

enum class RunOutcome { Completed, Failed, Cancelled };

RunOutcome run_case(const provider::RequestContext& request_context,
                    const PreparedCase& prepared,
                    size_t run_number,
                    bool warmup,
                    const std::vector<std::string>& modes,
                    runtime::CancellationToken cancellation,
                    std::ostream& output,
                    std::mutex& output_mutex,
                    Statistics& statistics,
                    std::mutex& statistics_mutex) {
    const Case& benchmark_case = *prepared.benchmark_case;
    std::vector<provider::Message> messages;
    if (!prepared.fetched_context.empty()) {
        messages.push_back({"user", "Reference text fetched from " + benchmark_case.fetch_url +
                                       ":\n\n" + prepared.fetched_context});
    }
    for (size_t turn_index = 0; turn_index < benchmark_case.turns.size(); ++turn_index) {
        messages.push_back({"user", benchmark_case.turns[turn_index]});
        const long long prompt_tokens = context::estimated_text_tokens(messages);
        provider::ChatResult result;
        Error err = provider::send_chat_messages(
            request_context, messages, [](const std::string&) { return ok_error(); }, result,
            cancellation);
        if (!err.ok()) {
            const bool cancelled = err.code == ErrorCode::Cancelled && cancellation.cancelled();
            if (!warmup) {
                {
                    std::lock_guard<std::mutex> lock(output_mutex);
                    output << "{\"type\":\"result\",\"id\":" << json::quote(benchmark_case.id)
                           << ",\"category\":" << json::quote(benchmark_case.category)
                           << ",\"modes\":";
                    write_string_array(output, modes);
                    output << ",\"run\":" << run_number
                           << ",\"turn\":" << (turn_index + 1)
                           << ",\"ok\":false,\"cancelled\":" << (cancelled ? "true" : "false")
                           << ",\"error_code\":" << json::quote(error_code_name(err.code))
                           << ",\"error\":" << json::quote(err.message) << "}\n";
                }
                std::lock_guard<std::mutex> lock(statistics_mutex);
                if (cancelled) {
                    ++statistics.cancelled_case_runs;
                } else {
                    ++statistics.failed_case_runs;
                }
            }
            return cancelled ? RunOutcome::Cancelled : RunOutcome::Failed;
        }
        messages.push_back({"assistant", result.content});
        if (!warmup) {
            const ::pkchat::output::ThinkingChunk separated =
                ::pkchat::output::split_thinking_traces(result.content);
            const double token_rate = provider::tokens_per_second(
                result, request_context.options.stream);
            {
                std::lock_guard<std::mutex> lock(output_mutex);
                output << "{\"type\":\"result\",\"id\":" << json::quote(benchmark_case.id)
                       << ",\"category\":" << json::quote(benchmark_case.category)
                       << ",\"language\":" << json::quote(benchmark_case.language)
                       << ",\"modes\":";
                write_string_array(output, modes);
                output << ",\"model\":" << json::quote(result.model.empty()
                                                                ? request_context.options.model
                                                                : result.model)
                       << ",\"provider\":" << json::quote(request_context.profile.name)
                       << ",\"run\":" << run_number
                       << ",\"turn\":" << (turn_index + 1)
                       << ",\"ok\":true,\"estimated_prompt_tokens\":" << prompt_tokens
                       << ",\"ttft_ms\":" << result.ttft_ms
                       << ",\"total_ms\":" << result.total_ms
                       << ",\"completion_tokens\":" << result.completion_tokens
                       << ",\"estimated_total_tokens\":"
                       << saturating_add(prompt_tokens, result.completion_tokens)
                       << ",\"token_count_source\":"
                       << json::quote(result.completion_tokens_estimated
                                          ? "estimated"
                                          : "provider_reported")
                       << ",\"tokens_per_second\":" << std::fixed << std::setprecision(3)
                       << token_rate
                       << ",\"thinking_trace_present\":"
                       << (separated.trace.empty() ? "false" : "true")
                       << ",\"response\":" << json::quote(separated.visible) << "}\n";
            }
            std::lock_guard<std::mutex> lock(statistics_mutex);
            ++statistics.completed_turns;
            statistics.estimated_prompt_tokens =
                saturating_add(statistics.estimated_prompt_tokens, prompt_tokens);
            statistics.completion_tokens =
                saturating_add(statistics.completion_tokens, result.completion_tokens);
            statistics.token_rate_sum += token_rate;
            if (result.ttft_ms >= 0) {
                statistics.ttft_sum_ms =
                    saturating_add(statistics.ttft_sum_ms, result.ttft_ms);
                ++statistics.ttft_samples;
            }
        }
    }
    if (!warmup) {
        std::lock_guard<std::mutex> lock(statistics_mutex);
        ++statistics.completed_case_runs;
    }
    return RunOutcome::Completed;
}

size_t worker_count_for(size_t tasks, int requested) {
    return std::max<size_t>(1, std::min(tasks, static_cast<size_t>(requested)));
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
    const std::vector<std::string> modes = split_modes(options.benchmark_mode);
    const bool speed_mode = contains_mode(modes, "speed");
    if (speed_mode && modes.size() != 1) {
        return {ErrorCode::BadArgs, "speed benchmark mode cannot be combined with other modes"};
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

    std::vector<PreparedCase> prepared_cases;
    prepared_cases.reserve(cases.size());
    for (const Case* benchmark_case : cases) {
        PreparedCase prepared;
        prepared.benchmark_case = benchmark_case;
        if (!benchmark_case->fetch_url.empty()) {
            if (!options.quiet) {
                status << "Fetching benchmark context: " << benchmark_case->fetch_url << "\n";
            }
            Error err = fetch::fetch_text(benchmark_case->fetch_url,
                                          fetch_options_for(options), prepared.fetched_context);
            if (!err.ok()) {
                return err;
            }
        }
        prepared_cases.push_back(std::move(prepared));
    }

    Statistics statistics;
    std::mutex output_mutex;
    std::mutex statistics_mutex;
    runtime::CancellationSource never_cancel;

    const size_t case_count = prepared_cases.size();
    if (static_cast<size_t>(options.benchmark_warmup) >
            std::numeric_limits<size_t>::max() / case_count ||
        (!speed_mode && static_cast<size_t>(options.benchmark_runs) >
                            std::numeric_limits<size_t>::max() / case_count)) {
        return {ErrorCode::BadArgs, "benchmark run count is too large for the selected dataset"};
    }

    auto run_finite_batch = [&](size_t repetitions, bool warmup) {
        const size_t total_tasks = repetitions * prepared_cases.size();
        if (total_tasks == 0) {
            return;
        }
        std::atomic<size_t> next_task{0};
        std::vector<std::thread> workers;
        const size_t worker_count = worker_count_for(total_tasks, options.benchmark_concurrency);
        workers.reserve(worker_count);
        for (size_t worker = 0; worker < worker_count; ++worker) {
            workers.emplace_back([&] {
                for (;;) {
                    const size_t task = next_task.fetch_add(1, std::memory_order_relaxed);
                    if (task >= total_tasks) {
                        break;
                    }
                    const size_t case_index = task % prepared_cases.size();
                    const size_t run_number = task / prepared_cases.size() + 1;
                    run_case(request_context, prepared_cases[case_index], run_number, warmup,
                             modes, never_cancel.token(), output, output_mutex, statistics,
                             statistics_mutex);
                }
            });
        }
        for (std::thread& worker : workers) {
            worker.join();
        }
    };

    run_finite_batch(static_cast<size_t>(options.benchmark_warmup), true);
    const auto measured_start = std::chrono::steady_clock::now();
    if (speed_mode) {
        const auto deadline = measured_start +
                              std::chrono::milliseconds(options.benchmark_duration_ms);
        runtime::CancellationSource duration_cancellation;
        std::atomic<size_t> next_task{0};
        std::mutex timer_mutex;
        std::condition_variable timer_cv;
        bool stop_timer = false;
        std::thread timer([&] {
            std::unique_lock<std::mutex> lock(timer_mutex);
            if (!timer_cv.wait_until(lock, deadline, [&] { return stop_timer; })) {
                duration_cancellation.cancel();
            }
        });
        std::vector<std::thread> workers;
        workers.reserve(static_cast<size_t>(options.benchmark_concurrency));
        for (int worker = 0; worker < options.benchmark_concurrency; ++worker) {
            workers.emplace_back([&] {
                while (!duration_cancellation.token().cancelled() &&
                       std::chrono::steady_clock::now() < deadline) {
                    const size_t task = next_task.fetch_add(1, std::memory_order_relaxed);
                    const size_t case_index = task % prepared_cases.size();
                    const size_t run_number = task / prepared_cases.size() + 1;
                    const RunOutcome outcome = run_case(
                        request_context, prepared_cases[case_index], run_number, false, modes,
                        duration_cancellation.token(), output, output_mutex, statistics,
                        statistics_mutex);
                    if (outcome == RunOutcome::Failed) {
                        duration_cancellation.cancel();
                        break;
                    }
                }
            });
        }
        for (std::thread& worker : workers) {
            worker.join();
        }
        {
            std::lock_guard<std::mutex> lock(timer_mutex);
            stop_timer = true;
        }
        timer_cv.notify_one();
        timer.join();
    } else {
        run_finite_batch(static_cast<size_t>(options.benchmark_runs), false);
    }
    const auto measured_end = std::chrono::steady_clock::now();
    const long long elapsed_ms = std::max<long long>(
        1, std::chrono::duration_cast<std::chrono::milliseconds>(measured_end - measured_start)
               .count());

    const double average_ttft = statistics.ttft_samples == 0
                                    ? 0.0
                                    : static_cast<double>(statistics.ttft_sum_ms) /
                                          static_cast<double>(statistics.ttft_samples);
    const double average_token_rate = statistics.completed_turns == 0
                                          ? 0.0
                                          : statistics.token_rate_sum /
                                                static_cast<double>(statistics.completed_turns);
    const double aggregate_token_rate = static_cast<double>(statistics.completion_tokens) *
                                        1000.0 / static_cast<double>(elapsed_ms);
    output << "{\"type\":\"summary\",\"selected_cases\":" << cases.size()
           << ",\"modes\":";
    write_string_array(output, modes);
    output << ",\"concurrency\":" << options.benchmark_concurrency
           << ",\"duration_ms\":" << (speed_mode ? options.benchmark_duration_ms : 0)
           << ",\"elapsed_ms\":" << elapsed_ms
           << ",\"runs_per_case\":" << options.benchmark_runs
           << ",\"warmup_runs\":" << options.benchmark_warmup
           << ",\"completed_case_runs\":" << statistics.completed_case_runs
           << ",\"failed_case_runs\":" << statistics.failed_case_runs
           << ",\"cancelled_case_runs\":" << statistics.cancelled_case_runs
           << ",\"completed_turns\":" << statistics.completed_turns
           << ",\"estimated_prompt_tokens\":" << statistics.estimated_prompt_tokens
           << ",\"completion_tokens\":" << statistics.completion_tokens
           << ",\"estimated_total_tokens\":"
           << saturating_add(statistics.estimated_prompt_tokens, statistics.completion_tokens)
           << ",\"average_ttft_ms\":" << std::fixed << std::setprecision(3) << average_ttft
           << ",\"average_tokens_per_second\":" << average_token_rate
           << ",\"aggregate_tokens_per_second\":" << aggregate_token_rate
           << ",\"scoring\":"
           << json::quote(contains_mode(modes, "quality") || contains_mode(modes, "refusals")
                              ? "not_implemented"
                              : "not_applicable")
           << "}\n";
    if (!output) {
        return {ErrorCode::FileWrite, "could not write benchmark JSONL output"};
    }
    return statistics.failed_case_runs == 0
               ? ok_error()
               : Error{ErrorCode::ProviderSchema,
                       std::to_string(statistics.failed_case_runs) +
                           " benchmark case run(s) failed"};
}

}  // namespace pkchat::benchmark
