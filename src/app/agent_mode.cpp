#include "app/app.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

#include "agent/session_runtime.hpp"
#include "security/redact.hpp"

namespace ainiux::app {
namespace {

volatile std::sig_atomic_t g_agent_interrupt = 0;

void agent_signal_handler(int) { g_agent_interrupt = 1; }

class AgentSignalGuard {
   public:
    AgentSignalGuard() { g_agent_interrupt = 0; previous_ = std::signal(SIGINT, agent_signal_handler); }
    ~AgentSignalGuard() {
        if (previous_ != SIG_ERR) std::signal(SIGINT, previous_);
    }
    AgentSignalGuard(const AgentSignalGuard&) = delete;
    AgentSignalGuard& operator=(const AgentSignalGuard&) = delete;

   private:
    using Handler = void (*)(int);
    Handler previous_ = SIG_ERR;
};

}  // namespace

AgentGoalResult run_agent_goal(provider::RequestContext context,
                               const std::string& goal_text,
                               runtime::CancellationToken cancellation,
                               std::function<bool()> interrupted,
                               bool write_final_to_stdout,
                               std::function<void(const std::string& status_line)> on_progress) {
    AgentGoalResult result;
    const std::string goal = ascii_trim(goal_text);
    if (goal.empty()) {
        result.error = {ErrorCode::BadArgs, "agent goal is empty; pass -r/--run TEXT or --run-file PATH"};
        return result;
    }

    agent::SessionRuntimeOptions options;
    options.workspace = ".";
    options.allow_mutations = true;
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
    options.compact_limit = context.options.agent_compact_limit;
    options.show_command_output = context.options.agent_show_command_output;
    options.on_progress = std::move(on_progress);

    agent::AgentSessionRuntime runtime;
    Error error = runtime.prepare(context, cancellation, interrupted, options);
    if (!error.ok()) {
        result.error = error;
        return result;
    }

    agent::SessionTurnResult turn =
        runtime.run_user_turn(context, goal, cancellation, interrupted);
    result.error = turn.error;
    result.final_text = turn.final_text;
    result.turns = turn.session_turns;
    result.tool_calls = turn.session_tool_calls;

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
    return result;
}

int run_agent_mode(provider::RequestContext context) {
    AgentSignalGuard signal_guard;
    runtime::CancellationSource cancellation;
    std::atomic<bool> finished{false};
    std::thread interrupt_monitor([&] {
        while (!finished.load(std::memory_order_acquire)) {
            if (g_agent_interrupt != 0) {
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
    AgentGoalResult result =
        run_agent_goal(std::move(context), goal, cancellation.token(),
                       [] { return g_agent_interrupt != 0; }, true, {});
    if (!result.error.ok()) {
        print_error(result.error);
        return exit_code_for(result.error.code);
    }
    return 0;
}

}  // namespace ainiux::app
