#include "benchmark/benchmark.hpp"

#include "benchmark/detail.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>

#include "context/context.hpp"
#include "fetch/fetch.hpp"
#include "json/json.hpp"
#include "output/thinking.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::benchmark {
namespace {

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
    long long provider_prompt_tokens = 0;
    size_t provider_prompt_token_samples = 0;
    long long provider_total_tokens = 0;
    size_t provider_total_token_samples = 0;
    size_t scored_turns = 0;
    size_t passed_turns = 0;
    std::vector<double> ttft_ms;
    std::vector<double> total_ms;
    std::vector<double> decode_ms;
    std::vector<double> decode_token_rates;
    std::vector<double> wall_token_rates;
};

enum class RunOutcome { Completed, Failed, Cancelled };

struct ProgressSnapshot {
    size_t finished = 0;
    size_t completed = 0;
    size_t failed = 0;
    size_t cancelled = 0;
};

struct ProgressCounts {
    std::mutex mutex;
    ProgressSnapshot values;
};

void record_outcome(ProgressCounts& counts, RunOutcome outcome) {
    std::lock_guard<std::mutex> lock(counts.mutex);
    if (outcome == RunOutcome::Completed) {
        ++counts.values.completed;
    } else if (outcome == RunOutcome::Failed) {
        ++counts.values.failed;
    } else {
        ++counts.values.cancelled;
    }
    ++counts.values.finished;
}

ProgressSnapshot progress_snapshot(ProgressCounts& counts) {
    std::lock_guard<std::mutex> lock(counts.mutex);
    return counts.values;
}

void write_progress_counts(std::ostream& status, const ProgressSnapshot& counts) {
    status << "completed " << counts.completed
           << ", failed " << counts.failed
           << ", cancelled " << counts.cancelled;
}

void write_modes_text(std::ostream& status, const std::vector<std::string>& modes) {
    for (size_t index = 0; index < modes.size(); ++index) {
        if (index != 0) {
            status << ",";
        }
        status << modes[index];
    }
}

class CancellationMonitor {
   public:
    explicit CancellationMonitor(std::function<bool()> requested)
        : requested_(std::move(requested)) {
        if (!requested_) {
            return;
        }
        thread_ = std::thread([this] {
            std::unique_lock<std::mutex> lock(mutex_);
            while (!stop_) {
                lock.unlock();
                if (requested_()) {
                    interrupted_.store(true, std::memory_order_release);
                    source_.cancel();
                    return;
                }
                lock.lock();
                cv_.wait_for(lock, std::chrono::milliseconds(20), [this] { return stop_; });
            }
        });
    }

    ~CancellationMonitor() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_one();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    CancellationMonitor(const CancellationMonitor&) = delete;
    CancellationMonitor& operator=(const CancellationMonitor&) = delete;

    runtime::CancellationToken token() const { return source_.token(); }
    void cancel() const { source_.cancel(); }
    bool interrupted() const {
        return interrupted_.load(std::memory_order_acquire) ||
               (requested_ && requested_());
    }

   private:
    std::function<bool()> requested_;
    runtime::CancellationSource source_;
    std::atomic<bool> interrupted_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread thread_;
};

double percentile(std::vector<double> samples, double percentile_value) {
    if (samples.empty()) {
        return -1.0;
    }
    std::sort(samples.begin(), samples.end());
    const size_t rank = static_cast<size_t>(
        std::ceil(percentile_value * static_cast<double>(samples.size())));
    return samples[std::max<size_t>(1, rank) - 1];
}

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
                    detail::write_string_array(output, modes);
                    detail::write_result_case_metadata(output, benchmark_case, turn_index);
                    output << ",\"run\":" << run_number
                           << ",\"turn\":" << (turn_index + 1)
                           << ",\"ok\":false,\"cancelled\":" << (cancelled ? "true" : "false")
                           << ",\"http_status\":" << result.http_status
                           << ",\"time_to_first_byte_ms\":";
                    detail::write_json_integer_or_null(output, result.time_to_first_byte_ms);
                    output << ",\"first_body_ms\":";
                    detail::write_json_integer_or_null(output, result.first_body_ms);
                    output
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
            const ::ainiux::output::ThinkingChunk separated =
                ::ainiux::output::split_thinking_traces(result.content);
            const double token_rate = provider::tokens_per_second(
                result, request_context.options.stream);
            const double wall_token_rate = detail::wall_tokens_per_second(result);
            const long long decode_ms = request_context.options.stream && result.ttft_ms >= 0
                                            ? std::max<long long>(0, result.total_ms - result.ttft_ms)
                                            : -1;
            const ScoreResult score = score_response(benchmark_case, turn_index + 1,
                                                     separated.visible);
            {
                std::lock_guard<std::mutex> lock(output_mutex);
                output << "{\"type\":\"result\",\"id\":" << json::quote(benchmark_case.id)
                       << ",\"category\":" << json::quote(benchmark_case.category)
                       << ",\"language\":" << json::quote(benchmark_case.language)
                       << ",\"modes\":";
                detail::write_string_array(output, modes);
                detail::write_result_case_metadata(output, benchmark_case, turn_index);
                output << ",\"model\":" << json::quote(result.model.empty()
                                                                ? request_context.options.model
                                                                : result.model)
                       << ",\"provider\":" << json::quote(request_context.profile.name)
                       << ",\"run\":" << run_number
                       << ",\"turn\":" << (turn_index + 1)
                       << ",\"ok\":true,\"estimated_prompt_tokens\":" << prompt_tokens
                       << ",\"provider_prompt_tokens\":";
                detail::write_json_integer_or_null(output, result.prompt_tokens);
                output << ",\"prompt_token_count_source\":"
                       << json::quote(result.prompt_tokens >= 0
                                          ? "provider_reported"
                                          : "estimated")
                       << ",\"ttft_ms\":" << result.ttft_ms
                       << ",\"ttft_source\":"
                       << json::quote(request_context.options.stream
                                          ? "first_content_delta"
                                          : "response_latency")
                       << ",\"total_ms\":" << result.total_ms
                       << ",\"decode_ms\":";
                detail::write_json_integer_or_null(output, decode_ms);
                output << ",\"dns_ms\":";
                detail::write_json_integer_or_null(output, result.dns_ms);
                output << ",\"connect_ms\":";
                detail::write_json_integer_or_null(output, result.connect_ms);
                output << ",\"tls_ms\":";
                detail::write_json_integer_or_null(output, result.tls_ms);
                output << ",\"time_to_first_byte_ms\":";
                detail::write_json_integer_or_null(output, result.time_to_first_byte_ms);
                output << ",\"first_body_ms\":";
                detail::write_json_integer_or_null(output, result.first_body_ms);
                output << ",\"http_status\":" << result.http_status
                       << ",\"completion_tokens\":" << result.completion_tokens
                       << ",\"provider_total_tokens\":";
                detail::write_json_integer_or_null(output, result.total_tokens);
                output
                       << ",\"estimated_total_tokens\":"
                       << detail::saturating_add(prompt_tokens, result.completion_tokens)
                       << ",\"total_tokens\":"
                       << (result.total_tokens >= 0
                               ? result.total_tokens
                               : detail::saturating_add(prompt_tokens, result.completion_tokens))
                       << ",\"total_token_count_source\":"
                       << json::quote(result.total_tokens >= 0
                                          ? "provider_reported"
                                          : "estimated")
                       << ",\"token_count_source\":"
                       << json::quote(result.completion_tokens_estimated
                                          ? "estimated"
                                          : "provider_reported")
                       << ",\"tokens_per_second\":" << std::fixed << std::setprecision(3)
                       << token_rate
                       << ",\"tokens_per_second_decode\":";
                detail::write_json_metric(output, request_context.options.stream ? token_rate : -1.0);
                output << ",\"tokens_per_second_wall\":";
                detail::write_json_metric(output, wall_token_rate);
                output << ",\"provider_usage\":"
                       << (result.usage_json.empty() ? "null" : result.usage_json)
                       << ",\"score\":";
                if (score.configured) {
                    output << (score.passed ? "1" : "0");
                } else {
                    output << "null";
                }
                output << ",\"score_method\":"
                       << (score.configured ? json::quote(score.method) : "null")
                       << ",\"thinking_trace_present\":"
                       << (separated.trace.empty() ? "false" : "true")
                       << ",\"response\":" << json::quote(separated.visible) << "}\n";
            }
            std::lock_guard<std::mutex> lock(statistics_mutex);
            ++statistics.completed_turns;
            statistics.estimated_prompt_tokens =
                detail::saturating_add(statistics.estimated_prompt_tokens, prompt_tokens);
            statistics.completion_tokens =
                detail::saturating_add(statistics.completion_tokens, result.completion_tokens);
            statistics.token_rate_sum += token_rate;
            statistics.total_ms.push_back(static_cast<double>(result.total_ms));
            statistics.wall_token_rates.push_back(wall_token_rate);
            if (request_context.options.stream && result.ttft_ms >= 0) {
                statistics.ttft_sum_ms =
                    detail::saturating_add(statistics.ttft_sum_ms, result.ttft_ms);
                ++statistics.ttft_samples;
                statistics.ttft_ms.push_back(static_cast<double>(result.ttft_ms));
                statistics.decode_ms.push_back(static_cast<double>(decode_ms));
                statistics.decode_token_rates.push_back(token_rate);
            }
            if (result.prompt_tokens >= 0) {
                statistics.provider_prompt_tokens = detail::saturating_add(
                    statistics.provider_prompt_tokens, result.prompt_tokens);
                ++statistics.provider_prompt_token_samples;
            }
            if (result.total_tokens >= 0) {
                statistics.provider_total_tokens = detail::saturating_add(
                    statistics.provider_total_tokens, result.total_tokens);
                ++statistics.provider_total_token_samples;
            }
            if (score.configured) {
                ++statistics.scored_turns;
                if (score.passed) {
                    ++statistics.passed_turns;
                }
            }
        }
    }
    if (!warmup) {
        std::lock_guard<std::mutex> lock(statistics_mutex);
        ++statistics.completed_case_runs;
    }
    return RunOutcome::Completed;
}

}  // namespace

Error run(const provider::RequestContext& context,
          const std::vector<const Case*>& cases,
          const cli::Options& options,
          std::ostream& output,
          std::ostream& status,
          const std::function<bool()>& interrupt_requested) {
    if (cases.empty()) {
        return {ErrorCode::BadArgs, "benchmark selection matched no dataset cases"};
    }
    const std::vector<std::string> modes = detail::split_modes(options.benchmark_mode);
    const bool speed_mode = detail::contains_mode(modes, "speed");
    if (speed_mode && modes.size() != 1) {
        return {ErrorCode::BadArgs, "speed benchmark mode cannot be combined with other modes"};
    }
    CancellationMonitor cancellation(interrupt_requested);
    provider::RequestContext request_context = context;
    if (request_context.profile.offline) {
        return {ErrorCode::UnsupportedFeature,
                "benchmark execution requires a model provider; use --validate-dataset or --list-cases offline"};
    }
    if (request_context.options.model.empty()) {
        provider::ModelsResult models;
        Error err = provider::list_models(request_context, models, cancellation.token());
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
                                          detail::fetch_options_for(options), prepared.fetched_context,
                                          cancellation.token());
            if (!err.ok()) {
                return err;
            }
        }
        prepared_cases.push_back(std::move(prepared));
    }

    Statistics statistics;
    std::mutex output_mutex;
    std::mutex statistics_mutex;
    std::mutex status_mutex;

    const size_t case_count = prepared_cases.size();
    if (static_cast<size_t>(options.benchmark_warmup) >
            std::numeric_limits<size_t>::max() / case_count ||
        (!speed_mode && static_cast<size_t>(options.benchmark_runs) >
                            std::numeric_limits<size_t>::max() / case_count)) {
        return {ErrorCode::BadArgs, "benchmark run count is too large for the selected dataset"};
    }

    if (!options.quiet) {
        status << "Benchmark started: modes ";
        write_modes_text(status, modes);
        status << "; provider " << request_context.profile.name
               << "; model " << request_context.options.model
               << "; cases " << case_count
               << "; concurrency " << options.benchmark_concurrency << "\n";
    }

    auto run_finite_batch = [&](size_t repetitions, bool warmup) {
        const size_t total_tasks = repetitions * prepared_cases.size();
        if (total_tasks == 0) {
            return;
        }
        std::atomic<size_t> next_task{0};
        ProgressCounts progress;
        size_t last_reported = 0;
        std::vector<std::thread> workers;
        const size_t worker_count = detail::worker_count_for(total_tasks, options.benchmark_concurrency);
        if (!options.quiet) {
            std::lock_guard<std::mutex> lock(status_mutex);
            status << (warmup ? "Warm-up" : "Benchmark") << " progress: 0/"
                   << total_tasks << " runs (0%)\n";
        }
        workers.reserve(worker_count);
        for (size_t worker = 0; worker < worker_count; ++worker) {
            workers.emplace_back([&] {
                for (;;) {
                    if (cancellation.token().cancelled()) {
                        break;
                    }
                    const size_t task = next_task.fetch_add(1, std::memory_order_relaxed);
                    if (task >= total_tasks || cancellation.token().cancelled()) {
                        break;
                    }
                    const size_t case_index = task % prepared_cases.size();
                    const size_t run_number = task / prepared_cases.size() + 1;
                    const RunOutcome outcome = run_case(
                        request_context, prepared_cases[case_index], run_number, warmup,
                        modes, cancellation.token(), output, output_mutex, statistics,
                        statistics_mutex);
                    record_outcome(progress, outcome);
                    if (!options.quiet) {
                        std::lock_guard<std::mutex> lock(status_mutex);
                        const ProgressSnapshot progress_now = progress_snapshot(progress);
                        const size_t reporting_interval =
                            total_tasks / 20 + (total_tasks % 20 == 0 ? 0 : 1);
                        if (progress_now.finished == total_tasks || last_reported == 0 ||
                            progress_now.finished - last_reported >= reporting_interval) {
                            last_reported = progress_now.finished;
                            status << (warmup ? "Warm-up" : "Benchmark") << " progress: "
                                   << progress_now.finished << "/" << total_tasks << " runs ("
                                   << static_cast<unsigned>(
                                          static_cast<long double>(progress_now.finished) *
                                          100.0L / static_cast<long double>(total_tasks))
                                   << "%; ";
                            write_progress_counts(status, progress_now);
                            status << ")\n";
                        }
                    }
                }
            });
        }
        for (std::thread& worker : workers) {
            worker.join();
        }
    };

    run_finite_batch(static_cast<size_t>(options.benchmark_warmup), true);
    const auto measured_start = std::chrono::steady_clock::now();
    if (speed_mode && !cancellation.token().cancelled()) {
        const auto deadline = measured_start +
                              std::chrono::milliseconds(options.benchmark_duration_ms);
        std::atomic<size_t> next_task{0};
        ProgressCounts progress;
        std::mutex timer_mutex;
        std::condition_variable timer_cv;
        bool stop_timer = false;
        if (!options.quiet) {
            status << "Speed progress: 0/" << options.benchmark_duration_ms
                   << " ms (0%; 0 runs finished)\n";
        }
        std::thread timer([&] {
            std::unique_lock<std::mutex> lock(timer_mutex);
            const auto reporting_interval = std::chrono::milliseconds(
                std::max<long long>(100, std::min<long long>(1000,
                    options.benchmark_duration_ms / 10)));
            while (!stop_timer) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) {
                    cancellation.cancel();
                    break;
                }
                const auto wake_at = std::min(deadline, now + reporting_interval);
                if (timer_cv.wait_until(lock, wake_at, [&] { return stop_timer; })) {
                    break;
                }
                const auto current = std::chrono::steady_clock::now();
                if (current >= deadline) {
                    cancellation.cancel();
                    break;
                }
                if (!options.quiet) {
                    const long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        current - measured_start).count();
                    lock.unlock();
                    {
                        std::lock_guard<std::mutex> status_lock(status_mutex);
                        status << "Speed progress: " << elapsed << "/"
                               << options.benchmark_duration_ms << " ms ("
                               << (elapsed * 100 / options.benchmark_duration_ms) << "%; ";
                        write_progress_counts(status, progress_snapshot(progress));
                        status << ")\n";
                    }
                    lock.lock();
                }
            }
        });
        std::vector<std::thread> workers;
        workers.reserve(static_cast<size_t>(options.benchmark_concurrency));
        for (int worker = 0; worker < options.benchmark_concurrency; ++worker) {
            workers.emplace_back([&] {
                while (!cancellation.token().cancelled() &&
                       std::chrono::steady_clock::now() < deadline) {
                    const size_t task = next_task.fetch_add(1, std::memory_order_relaxed);
                    const size_t case_index = task % prepared_cases.size();
                    const size_t run_number = task / prepared_cases.size() + 1;
                    const RunOutcome outcome = run_case(
                        request_context, prepared_cases[case_index], run_number, false, modes,
                        cancellation.token(), output, output_mutex, statistics,
                        statistics_mutex);
                    record_outcome(progress, outcome);
                    if (outcome == RunOutcome::Failed) {
                        cancellation.cancel();
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
        if (!options.quiet) {
            const long long elapsed = std::min<long long>(
                options.benchmark_duration_ms,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - measured_start).count());
            std::lock_guard<std::mutex> lock(status_mutex);
            status << "Speed progress: " << elapsed << "/"
                   << options.benchmark_duration_ms << " ms ("
                   << (elapsed * 100 / options.benchmark_duration_ms) << "%; ";
            write_progress_counts(status, progress_snapshot(progress));
            status << ")\n";
        }
    } else if (!cancellation.token().cancelled()) {
        run_finite_batch(static_cast<size_t>(options.benchmark_runs), false);
    }
    const auto measured_end = std::chrono::steady_clock::now();
    const long long elapsed_ms = std::max<long long>(
        1, std::chrono::duration_cast<std::chrono::milliseconds>(measured_end - measured_start)
               .count());

    const double average_ttft = statistics.ttft_samples == 0
                                    ? -1.0
                                    : static_cast<double>(statistics.ttft_sum_ms) /
                                          static_cast<double>(statistics.ttft_samples);
    const double average_token_rate = statistics.completed_turns == 0
                                          ? 0.0
                                          : statistics.token_rate_sum /
                                                static_cast<double>(statistics.completed_turns);
    const double aggregate_token_rate = static_cast<double>(statistics.completion_tokens) *
                                        1000.0 / static_cast<double>(elapsed_ms);
    const double score_percentage = statistics.scored_turns == 0
                                        ? -1.0
                                        : static_cast<double>(statistics.passed_turns) * 100.0 /
                                              static_cast<double>(statistics.scored_turns);
    const bool interrupted = cancellation.interrupted();
    const bool evaluation_mode = detail::contains_mode(modes, "quality") ||
                                 detail::contains_mode(modes, "refusals");
    const char* scoring = statistics.scored_turns != 0
                              ? "scored"
                              : (evaluation_mode ? "not_configured" : "not_applicable");
    const double ttft_p50 = percentile(statistics.ttft_ms, 0.50);
    const double ttft_p90 = percentile(statistics.ttft_ms, 0.90);
    const double ttft_p99 = percentile(statistics.ttft_ms, 0.99);
    const double total_p50 = percentile(statistics.total_ms, 0.50);
    const double total_p90 = percentile(statistics.total_ms, 0.90);
    const double total_p99 = percentile(statistics.total_ms, 0.99);
    const double decode_p50 = percentile(statistics.decode_ms, 0.50);
    const double decode_p90 = percentile(statistics.decode_ms, 0.90);
    const double decode_p99 = percentile(statistics.decode_ms, 0.99);
    const double decode_rate_p50 = percentile(statistics.decode_token_rates, 0.50);
    const double decode_rate_p90 = percentile(statistics.decode_token_rates, 0.90);
    const double decode_rate_p99 = percentile(statistics.decode_token_rates, 0.99);
    const double wall_rate_p50 = percentile(statistics.wall_token_rates, 0.50);
    const double wall_rate_p90 = percentile(statistics.wall_token_rates, 0.90);
    const double wall_rate_p99 = percentile(statistics.wall_token_rates, 0.99);
    output << "{\"type\":\"summary\",\"selected_cases\":" << cases.size()
           << ",\"modes\":";
    detail::write_string_array(output, modes);
    output << ",\"concurrency\":" << options.benchmark_concurrency
           << ",\"duration_ms\":" << (speed_mode ? options.benchmark_duration_ms : 0)
           << ",\"elapsed_ms\":" << elapsed_ms
           << ",\"runs_per_case\":" << options.benchmark_runs
           << ",\"warmup_runs\":" << options.benchmark_warmup
           << ",\"interrupted\":" << (interrupted ? "true" : "false")
           << ",\"provider\":" << json::quote(request_context.profile.name)
           << ",\"model\":" << json::quote(request_context.options.model)
           << ",\"base_url\":" << json::quote(request_context.base_url)
           << ",\"stream\":" << (request_context.options.stream ? "true" : "false")
           << ",\"completed_case_runs\":" << statistics.completed_case_runs
           << ",\"failed_case_runs\":" << statistics.failed_case_runs
           << ",\"cancelled_case_runs\":" << statistics.cancelled_case_runs
           << ",\"completed_turns\":" << statistics.completed_turns
           << ",\"estimated_prompt_tokens\":" << statistics.estimated_prompt_tokens
           << ",\"completion_tokens\":" << statistics.completion_tokens
           << ",\"provider_prompt_tokens\":" << statistics.provider_prompt_tokens
           << ",\"provider_prompt_token_samples\":"
           << statistics.provider_prompt_token_samples
           << ",\"provider_total_tokens\":" << statistics.provider_total_tokens
           << ",\"provider_total_token_samples\":"
           << statistics.provider_total_token_samples
           << ",\"estimated_total_tokens\":"
           << detail::saturating_add(statistics.estimated_prompt_tokens, statistics.completion_tokens)
           << ",\"average_ttft_ms\":";
    detail::write_json_metric(output, average_ttft);
    output << ",\"ttft_p50_ms\":";
    detail::write_json_metric(output, ttft_p50);
    output << ",\"ttft_p90_ms\":";
    detail::write_json_metric(output, ttft_p90);
    output << ",\"ttft_p99_ms\":";
    detail::write_json_metric(output, ttft_p99);
    output << ",\"total_latency_p50_ms\":";
    detail::write_json_metric(output, total_p50);
    output << ",\"total_latency_p90_ms\":";
    detail::write_json_metric(output, total_p90);
    output << ",\"total_latency_p99_ms\":";
    detail::write_json_metric(output, total_p99);
    output << ",\"decode_latency_p50_ms\":";
    detail::write_json_metric(output, decode_p50);
    output << ",\"decode_latency_p90_ms\":";
    detail::write_json_metric(output, decode_p90);
    output << ",\"decode_latency_p99_ms\":";
    detail::write_json_metric(output, decode_p99);
    output
           << ",\"average_tokens_per_second\":" << average_token_rate
           << ",\"aggregate_tokens_per_second\":" << aggregate_token_rate
           << ",\"decode_tokens_per_second_p50\":";
    detail::write_json_metric(output, decode_rate_p50);
    output << ",\"decode_tokens_per_second_p90\":";
    detail::write_json_metric(output, decode_rate_p90);
    output << ",\"decode_tokens_per_second_p99\":";
    detail::write_json_metric(output, decode_rate_p99);
    output << ",\"wall_tokens_per_second_p50\":";
    detail::write_json_metric(output, wall_rate_p50);
    output << ",\"wall_tokens_per_second_p90\":";
    detail::write_json_metric(output, wall_rate_p90);
    output << ",\"wall_tokens_per_second_p99\":";
    detail::write_json_metric(output, wall_rate_p99);
    output << ",\"scoring\":" << json::quote(scoring)
           << ",\"scored_turns\":" << statistics.scored_turns
           << ",\"passed_turns\":" << statistics.passed_turns
           << ",\"failed_score_turns\":"
           << (statistics.scored_turns - statistics.passed_turns)
           << ",\"score_percentage\":";
    detail::write_json_metric(output, score_percentage);
    output
           << "}\n";
    if (!output) {
        return {ErrorCode::FileWrite, "could not write benchmark JSONL output"};
    }
    if (!options.quiet) {
        const auto metric_text = [](double value) {
            if (value < 0.0) {
                return std::string("n/a");
            }
            std::ostringstream text;
            text << std::fixed << std::setprecision(3) << value;
            return text.str();
        };
        std::vector<std::pair<std::string, std::string>> rows = {
            {"provider", request_context.profile.name},
            {"model", request_context.options.model},
            {"base_url", request_context.base_url},
            {"stream", request_context.options.stream ? "true" : "false"},
            {"elapsed_ms", std::to_string(elapsed_ms)},
            {"completed_runs", std::to_string(statistics.completed_case_runs)},
            {"failed_runs", std::to_string(statistics.failed_case_runs)},
            {"cancelled_runs", std::to_string(statistics.cancelled_case_runs)},
            {"interrupted", interrupted ? "true" : "false"},
            {"estimated_prompt_tokens", std::to_string(statistics.estimated_prompt_tokens)},
            {"completion_tokens", std::to_string(statistics.completion_tokens)},
            {"provider_prompt_tokens", std::to_string(statistics.provider_prompt_tokens)},
            {"provider_prompt_token_samples",
             std::to_string(statistics.provider_prompt_token_samples)},
            {"provider_total_tokens", std::to_string(statistics.provider_total_tokens)},
            {"provider_total_token_samples",
             std::to_string(statistics.provider_total_token_samples)},
            {"estimated_total_tokens", std::to_string(detail::saturating_add(
                                           statistics.estimated_prompt_tokens,
                                           statistics.completion_tokens))},
            {"average_ttft_ms", metric_text(average_ttft)},
            {"ttft_p50_ms", metric_text(ttft_p50)},
            {"ttft_p90_ms", metric_text(ttft_p90)},
            {"ttft_p99_ms", metric_text(ttft_p99)},
            {"total_latency_p50_ms", metric_text(total_p50)},
            {"total_latency_p90_ms", metric_text(total_p90)},
            {"total_latency_p99_ms", metric_text(total_p99)},
            {"decode_latency_p50_ms", metric_text(decode_p50)},
            {"decode_latency_p90_ms", metric_text(decode_p90)},
            {"decode_latency_p99_ms", metric_text(decode_p99)},
            {"average_tokens_per_second", metric_text(average_token_rate)},
            {"aggregate_tokens_per_second", metric_text(aggregate_token_rate)},
            {"decode_tokens_per_second_p50", metric_text(decode_rate_p50)},
            {"decode_tokens_per_second_p90", metric_text(decode_rate_p90)},
            {"decode_tokens_per_second_p99", metric_text(decode_rate_p99)},
            {"wall_tokens_per_second_p50", metric_text(wall_rate_p50)},
            {"wall_tokens_per_second_p90", metric_text(wall_rate_p90)},
            {"wall_tokens_per_second_p99", metric_text(wall_rate_p99)},
            {"scoring", scoring},
            {"scored_turns", std::to_string(statistics.scored_turns)},
            {"passed_turns", std::to_string(statistics.passed_turns)},
            {"score_percentage", metric_text(score_percentage)},
        };
        if (options.benchmark_summary_format == "csv") {
            const auto csv_field = [](const std::string& value) {
                if (value.find_first_of(",\"\r\n") == std::string::npos) {
                    return value;
                }
                std::string escaped = "\"";
                for (char ch : value) {
                    if (ch == '\"') {
                        escaped += "\"\"";
                    } else {
                        escaped.push_back(ch);
                    }
                }
                escaped.push_back('\"');
                return escaped;
            };
            status << "metric,value\n";
            for (const auto& row : rows) {
                status << csv_field(row.first) << "," << csv_field(row.second) << "\n";
            }
        } else {
            status << "Benchmark summary:\n"
                   << "  Metric                                Value\n"
                   << "  ------------------------------------  ----------------\n";
            for (const auto& row : rows) {
                std::string label = row.first;
                std::replace(label.begin(), label.end(), '_', ' ');
                if (!label.empty()) {
                    label.front() = static_cast<char>(
                        std::toupper(static_cast<unsigned char>(label.front())));
                }
                if (label.rfind("Ttft", 0) == 0) {
                    label.replace(0, 4, "TTFT");
                }
                if (label == "Base url") {
                    label = "Base URL";
                }
                status << "  " << std::left << std::setw(36) << label << "  "
                       << row.second << "\n";
            }
            status << std::right;
        }
    }
    if (interrupted) {
        return {ErrorCode::Cancelled, "benchmark cancelled by Ctrl+C"};
    }
    return statistics.failed_case_runs == 0
               ? ok_error()
               : Error{ErrorCode::ProviderSchema,
                       std::to_string(statistics.failed_case_runs) +
                           " benchmark case run(s) failed"};
}

}  // namespace ainiux::benchmark