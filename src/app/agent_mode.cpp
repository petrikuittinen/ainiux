#include "app/app.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

#include "agent/session_runtime.hpp"
#include "app/index_progress.hpp"
#include "fetch/fetch.hpp"
#include "search/search.hpp"
#include "input/input.hpp"
#include "security/redact.hpp"
#include "runtime/interrupt.hpp"

namespace ainiux::app {
std::string format_agent_run_metrics(const AgentGoalResult& result) {
    std::ostringstream out;
    out << "Agent metrics: tool calls " << result.tool_calls << " ("
        << result.failed_tool_calls << " failed), input "
        << result.token_usage.input_tokens << " tokens";
    if (result.token_usage.input_estimated) out << " (estimated)";
    out << ", output " << result.token_usage.output_tokens << " tokens";
    if (result.token_usage.output_estimated) out << " (estimated)";
    out << ", time " << std::fixed << std::setprecision(2)
        << static_cast<double>(std::max(0LL, result.elapsed_ms)) / 1000.0 << " s";
    return out.str();
}

AgentGoalResult run_agent_goal(provider::RequestContext context,
                               const std::string& goal_text,
                               runtime::CancellationToken cancellation,
                               std::function<bool()> interrupted,
                               bool write_final_to_stdout,
                               std::function<void(const std::string& status_line)> on_progress) {
    const auto started = std::chrono::steady_clock::now();
    AgentGoalResult result;
    if (context.routing_session_id.empty())
        context.routing_session_id = provider::new_routing_session_id();
    const std::string goal = ascii_trim(goal_text);
    if (goal.empty()) {
        result.error = {ErrorCode::BadArgs, "agent goal is empty; pass -r/--run TEXT or --run-file PATH"};
        result.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
        return result;
    }

    agent::SessionRuntimeOptions options;
    options.workspace = ".";
    options.task_mode = context.options.agent_plan ? agent::AgentTaskMode::Plan
                                                   : agent::AgentTaskMode::Act;
    options.allow_network = true;
    options.interactive = !write_final_to_stdout;
    options.enable_session_db = true;
    options.enable_agent_log = context.options.agent_log_enabled;
    options.security_review_log_keep_runs = context.options.security_review_log_keep_runs;
    options.trusted_prompt_dir = context.options.trusted_prompt_dir;
    options.max_source_code_file_size = context.options.max_source_code_file_size;
    options.history_backup.enabled = context.options.agent_history_backup_enabled;
    options.history_backup.max_bytes = context.options.agent_history_backup_max_bytes;
    options.history_backup.ttl_days = context.options.agent_history_backup_ttl_days;
    options.auto_compact = context.options.agent_auto_compact;
    options.compact_strategy = context.options.agent_compact_strategy;
    options.compact_limit = context.options.agent_compact_limit;
    options.max_agent_turns = context.options.agent_max_turns;
    options.index_mode =
        context.options.disable_indexing
            ? agent::SessionRuntimeOptions::IndexMode::Disabled
            : agent::SessionRuntimeOptions::IndexMode::UseExistingLazy;
    IndexProgressPrinter index_progress(
        !context.options.quiet &&
        options.index_mode !=
            agent::SessionRuntimeOptions::IndexMode::Disabled);
    options.on_index_progress =
        [&index_progress](const agent::index::Progress& update) {
            index_progress.update(update);
        };
    options.show_command_output = context.options.agent_show_command_output;
    options.fetch_options.connect_timeout_seconds = context.options.connect_timeout_seconds;
    options.fetch_options.timeout_seconds =
        context.options.timeout_seconds > 0 ? context.options.timeout_seconds : 30;
    options.fetch_options.max_bytes = context.options.max_fetch_bytes;
    options.fetch_options.proxy = context.options.proxy;
    options.fetch_options.insecure_tls = context.options.insecure_tls;
    options.fetch_options.trace_http = context.options.trace_http;
    options.fetch_options.allow_private = context.options.allow_private_url_fetch;
    options.search_options = search::options_for(context.options);
    options.on_progress = std::move(on_progress);

    agent::AgentSessionRuntime runtime;
    Error error = runtime.prepare(context, cancellation, interrupted, options);
    index_progress.finish();
    if (!error.ok()) {
        result.error = error;
        result.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
        return result;
    }
    runtime.begin_background_index_freshness();

    agent::AgentSessionRuntime::UserTurnPayload turn_payload;
    turn_payload.text = goal;
    for (const std::string& path : context.options.attachment_paths) {
        if (path.empty()) continue;
        input::FileType type;
        Error type_err = input::classify_file_type(path, type);
        if (!type_err.ok()) {
            result.error = type_err;
            result.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - started)
                                    .count();
            return result;
        }
        if (type.kind == input::Kind::Image) {
            const std::size_t limit =
                context.options.max_image_bytes > 0
                    ? static_cast<std::size_t>(context.options.max_image_bytes)
                    : 20U * 1024U * 1024U;
            input::ImageData loaded;
            Error load_err =
                input::load_image_file(path, type, limit, loaded, cancellation);
            if (!load_err.ok()) {
                result.error = load_err;
                result.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - started)
                                        .count();
                return result;
            }
            provider::ImageInput image{loaded.mime_type, std::move(loaded.base64_data)};
            image.display_name = path;
            image.source_ref = path;
            image.byte_size = static_cast<long long>(loaded.byte_size);
            turn_payload.images.push_back(std::move(image));
            if (!context.options.quiet) {
                std::cerr << "Attached image for agent turn: " << path << " ("
                          << loaded.mime_type << ", " << loaded.byte_size << " bytes)\n";
            }
        } else {
            // Text-like: fold a short notice into the goal (bounded).
            LoadedDocument document;
            Error doc_err =
                load_text_context_file(context.options, path, "--attach", document);
            if (!doc_err.ok()) {
                result.error = doc_err;
                result.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - started)
                                        .count();
                return result;
            }
            turn_payload.text +=
                "\n\n----- attached: " + path + " -----\n" + document.converted;
            if (!context.options.quiet) {
                std::cerr << "Attached context for agent turn: " << path << "\n";
            }
        }
    }

    agent::SessionTurnResult turn =
        runtime.run_user_turn(context, std::move(turn_payload), cancellation, interrupted);
    result.error = turn.error;
    result.final_text = turn.final_text;
    result.turns = turn.session_turns;
    result.tool_calls = turn.session_tool_calls;
    result.failed_tool_calls = turn.session_failed_tool_calls;
    result.token_usage = turn.token_usage;

    if (write_final_to_stdout && turn.error.ok() && !turn.needs_user_continue) {
        std::cout << result.final_text;
        if (!result.final_text.empty() && result.final_text.back() != '\n') std::cout << '\n';
    }

    std::string status = "success";
    if (!turn.error.ok()) {
        if (turn.error.code == ErrorCode::Cancelled)
            status = "cancelled";
        else if (turn.error.message.find("abort") != std::string::npos)
            status = "aborted";
        else
            status = "error";
    } else if (turn.needs_user_continue) {
        // One-shot --run treats needs-continue as a soft success with notice text.
        status = "success";
    }
    Error finish_error = runtime.finish_session(
        status, result.final_text,
        turn.error.ok() ? std::string{} : error_code_name(turn.error.code),
        turn.error.ok() ? std::string{} : turn.error.message);
    if (!finish_error.ok() && !context.options.quiet)
        std::cerr << "Agent warning: could not finish session DB: " << finish_error.message
                  << "\n";
    result.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();
    return result;
}

int run_agent_mode(provider::RequestContext context) {
    runtime::InterruptGuard interrupt_guard;
    runtime::CancellationSource cancellation;
    std::atomic<bool> finished{false};
    std::thread interrupt_monitor([&] {
        while (!finished.load(std::memory_order_acquire)) {
            if (interrupt_guard.interrupted()) {
                cancellation.cancel();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    });
    struct MonitorJoin {
        std::atomic<bool>& finished;
        std::thread& thread;
        ~MonitorJoin() {
            finished.store(true, std::memory_order_release);
            if (thread.joinable()) thread.join();
        }
    } monitor_join{finished, interrupt_monitor};

    const std::string goal = ascii_trim(context.options.prompt);
    const bool quiet = context.options.quiet;
    AgentGoalResult result =
        run_agent_goal(std::move(context), goal, cancellation.token(),
                       [&interrupt_guard] { return interrupt_guard.interrupted(); }, true, {});
    if (!result.error.ok()) {
        print_error(result.error);
        if (!quiet) std::cerr << format_agent_run_metrics(result) << "\n";
        return exit_code_for(result.error.code);
    }
    if (!quiet) std::cerr << format_agent_run_metrics(result) << "\n";
    return 0;
}

}  // namespace ainiux::app
