#include "app/app.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "agent/agent_loop.hpp"
#include "agent/index/index.hpp"
#include "agent/prompts.hpp"
#include "agent/review_log.hpp"
#include "agent/tools.hpp"
#include "json/json.hpp"
#include "security/redact.hpp"

namespace ainiux::app {
namespace {

volatile std::sig_atomic_t g_agent_interrupt = 0;

json::Value log_object() {
    json::Value value;
    value.type = json::Value::Type::Object;
    return value;
}
json::Value log_string(const std::string& text) {
    json::Value value;
    value.type = json::Value::Type::String;
    value.string = text;
    return value;
}
json::Value log_number(double number) {
    json::Value value;
    value.type = json::Value::Type::Number;
    value.number = number;
    return value;
}
json::Value log_bool(bool boolean) {
    json::Value value;
    value.type = json::Value::Type::Bool;
    value.boolean = boolean;
    return value;
}

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

std::vector<std::string> configured_secrets(const provider::RequestContext& context) {
    std::vector<std::string> secrets;
    if (!context.api_key.empty()) secrets.push_back(context.api_key);
    if (!context.options.key.empty()) secrets.push_back(context.options.key);
    for (const std::string& header : context.headers) {
        const std::size_t colon = header.find(':');
        if (colon == std::string::npos) continue;
        if (is_sensitive_header_name(ascii_trim(header.substr(0, colon)))) {
            const std::string value = ascii_trim(header.substr(colon + 1));
            if (!value.empty()) secrets.push_back(value);
        }
    }
    std::sort(secrets.begin(), secrets.end());
    secrets.erase(std::unique(secrets.begin(), secrets.end()), secrets.end());
    return secrets;
}

std::vector<std::string> known_tool_names(const agent::ReadToolRegistry& tools) {
    std::vector<std::string> names;
    for (const provider::FunctionDefinition& definition : tools.definitions())
        names.push_back(definition.name);
    return names;
}

}  // namespace

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

    const std::vector<std::string> secrets = configured_secrets(context);
    const auto started = std::chrono::steady_clock::now();
    const std::string goal = ascii_trim(context.options.prompt);
    if (goal.empty()) {
        const Error error{ErrorCode::BadArgs, "agent mode requires a non-empty user goal"};
        print_error(error);
        return exit_code_for(error.code);
    }

    std::unique_ptr<agent::ReviewLogger> logger;
    if (context.options.agent_log_enabled) {
        Error log_error;
        logger = agent::ReviewLogger::create(
            ".", context.options.security_review_log_keep_runs, secrets,
            [&](const std::string& warning) { std::cerr << warning << "\n"; }, log_error, "agent");
        if (!logger) {
            std::cerr << "AGENT LOGGING DISABLED: " << redact_secrets(log_error.message, secrets)
                      << "; the agent will continue\n";
        } else if (!context.options.quiet) {
            std::cerr << "Agent diagnostic log (live): " << logger->partial_path() << "\n"
                      << "  tail -f that path while the agent runs; finalized as "
                      << logger->final_path() << " on completion\n";
        }
    }

    auto finish_log = [&](const Error& final_error, const std::string& final_text,
                          std::size_t turns, std::size_t tool_calls) {
        if (!logger) return;
        json::Value fields = log_object();
        fields.object["duration_ms"] = log_number(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                       std::chrono::steady_clock::now() - started)
                                                       .count());
        fields.object["exit_code"] = log_number(exit_code_for(final_error.code));
        fields.object["error_code"] = log_string(error_code_name(final_error.code));
        fields.object["error_message"] = log_string(final_error.message);
        fields.object["turns"] = log_number(turns);
        fields.object["tool_calls"] = log_number(tool_calls);
        fields.object["final_text_bytes"] = log_number(final_text.size());
        logger->finish(std::move(fields), final_error.ok() ? "success" : "failure");
        if (!context.options.quiet) {
            std::error_code exists_error;
            if (std::filesystem::exists(logger->final_path(), exists_error) && !exists_error)
                std::cerr << "Agent diagnostic log (final): " << logger->final_path() << "\n";
        }
    };

    if (logger) {
        json::Value fields = log_object();
        fields.object["workspace"] = log_string(".");
        fields.object["provider"] = log_string(context.profile.name);
        fields.object["model"] = log_string(context.options.model);
        fields.object["api"] =
            log_string(context.api_kind == provider::ApiKind::Responses ? "responses" : "chat");
        fields.object["goal"] = agent::ReviewLogger::payload(goal);
        fields.object["streaming"] = log_bool(context.options.stream);
        logger->event("run_start", {"run"}, std::move(fields), "success");
    }

    agent::index::Options index_options;
    index_options.workspace = ".";
    index_options.max_source_code_file_size = context.options.max_source_code_file_size;
    index_options.cancellation = cancellation.token();
    index_options.interrupted = [] { return g_agent_interrupt != 0; };
    agent::index::RefreshStats index_stats;
    Error error = agent::index::refresh(index_options, index_stats);
    if (logger) {
        json::Value fields = log_object();
        fields.object["discovered"] = log_number(index_stats.discovered);
        fields.object["indexed"] = log_number(index_stats.indexed);
        fields.object["unchanged"] = log_number(index_stats.unchanged);
        fields.object["skipped"] = log_number(index_stats.skipped);
        if (!error.ok()) {
            fields.object["error_code"] = log_string(error_code_name(error.code));
            fields.object["error_message"] = log_string(error.message);
        }
        logger->event("index_result", {"index"}, std::move(fields),
                      error.ok() ? "success" : "failure");
    }
    if (!error.ok()) {
        finish_log(error, "", 0, 0);
        print_error(error);
        return exit_code_for(error.code);
    }
    if (!context.options.quiet) {
        for (const std::string& diagnostic : index_stats.diagnostics)
            std::cerr << "Index warning: " << redact_secrets(diagnostic, secrets) << "\n";
        std::cerr << "Code index refreshed: " << index_stats.discovered << " eligible, "
                  << index_stats.indexed << " indexed, " << index_stats.unchanged
                  << " unchanged, " << index_stats.skipped << " skipped.\n";
    }

    agent::index::Snapshot snapshot;
    error = agent::index::load_snapshot(index_options, snapshot);
    if (!error.ok()) {
        finish_log(error, "", 0, 0);
        print_error(error);
        return exit_code_for(error.code);
    }

    agent::ReadToolRegistry tools;
    error = agent::ReadToolRegistry::create(index_options, std::move(snapshot), secrets, tools);
    if (!error.ok()) {
        finish_log(error, "", 0, 0);
        print_error(error);
        return exit_code_for(error.code);
    }

    agent::TrustedPrompts prompts;
    error = agent::load_trusted_prompts(context.options.trusted_prompt_dir, prompts);
    if (!error.ok()) {
        finish_log(error, "", 0, 0);
        print_error(error);
        return exit_code_for(error.code);
    }

    const bool supports_tools = provider::capabilities_for(context).tool_calls;
    if (!supports_tools) {
        error = {ErrorCode::UnsupportedFeature,
                 "agent mode requires a provider/API path with native function calling"};
        finish_log(error, "", 0, 0);
        print_error(error);
        return exit_code_for(error.code);
    }

    agent::AgentLoopState state;
    state.protocol = agent::default_tool_protocol(supports_tools);
    agent::AgentLoopLimits limits;
    limits.interactive = false;
    limits.max_scripted_turns = 50;

    provider::ToolConversation conversation;
    agent::seed_agent_conversation(conversation, prompts, state.protocol, goal);
    if (!context.options.quiet) {
        std::cerr << "Agent goal: " << redact_secrets(goal, secrets) << "\n"
                  << "Using " << context.profile.name << "/" << context.options.model
                  << " with protocol "
                  << (state.protocol == agent::ToolProtocol::Xml ? "xml" : "native") << ".\n";
    }

    const std::vector<std::string> known = known_tool_names(tools);
    const std::vector<provider::FunctionDefinition> definitions = tools.definitions();
    std::size_t total_tool_calls = 0;
    std::string final_text;

    auto executor = [&](const std::string& name, const std::string& arguments_json,
                        runtime::CancellationToken token) {
        return tools.execute(name, arguments_json, token);
    };

    for (;;) {
        if (cancellation.token().cancelled() || g_agent_interrupt != 0) {
            agent::pair_dangling_tool_calls(context, conversation, state);
            error = {ErrorCode::Cancelled, "agent run cancelled"};
            finish_log(error, final_text, state.turn, total_tool_calls);
            print_error(error);
            return exit_code_for(error.code);
        }

        provider::ToolRoundResult round;
        agent::ReviewLogContext log_context("agent");
        log_context.round = state.turn + 1;
        log_context.cumulative_tool_calls = total_tool_calls;
        provider::ToolRoundObserver observer;
        const provider::ToolRoundObserver* observer_pointer = nullptr;
        if (logger) {
            observer = logger->tool_round_observer();
            observer_pointer = &observer;
        }

        error = agent::send_tool_round_with_transport_retries(
            context, conversation, definitions, round, cancellation.token(),
            limits.transport_attempts, observer_pointer,
            [&]() {
                provider::ToolRoundContext ctx;
                ctx.stage = log_context.stage;
                ctx.round = log_context.round;
                ctx.cumulative_tool_calls = log_context.cumulative_tool_calls;
                return ctx;
            }(),
            [&](const Error& retry_error, int attempt, int backoff_seconds) {
                if (!logger) return;
                json::Value fields = log_object();
                fields.object["error_code"] = log_string(error_code_name(retry_error.code));
                fields.object["error_message"] = log_string(retry_error.message);
                fields.object["attempt"] = log_number(attempt);
                fields.object["backoff_ms"] = log_number(backoff_seconds * 1000);
                logger->event("retry_scheduled", log_context, std::move(fields), "failure");
            });
        if (!error.ok()) {
            agent::pair_dangling_tool_calls(context, conversation, state);
            finish_log(error, final_text, state.turn, total_tool_calls);
            print_error(error);
            return exit_code_for(error.code);
        }

        total_tool_calls += round.tool_calls.size();
        agent::AgentRoundOutcome outcome = agent::handle_agent_tool_round(
            state, limits, context, conversation, std::move(round), known, executor,
            cancellation.token());

        if (logger) {
            json::Value fields = log_object();
            fields.object["outcome"] = log_string(
                outcome.kind == agent::AgentRoundOutcome::Kind::Continue       ? "continue"
                : outcome.kind == agent::AgentRoundOutcome::Kind::FinalText    ? "final_text"
                : outcome.kind == agent::AgentRoundOutcome::Kind::Aborted      ? "aborted"
                : outcome.kind == agent::AgentRoundOutcome::Kind::NeedsUserContinue
                    ? "needs_user_continue"
                    : "error");
            fields.object["tool_result_count"] = log_number(outcome.tool_results.size());
            fields.object["protocol"] =
                log_string(state.protocol == agent::ToolProtocol::Xml ? "xml" : "native");
            if (!outcome.notice.empty())
                fields.object["notice"] = agent::ReviewLogger::payload(outcome.notice);
            if (!outcome.final_text.empty())
                fields.object["final_text"] = agent::ReviewLogger::payload(outcome.final_text);
            if (!outcome.error.ok()) {
                fields.object["error_code"] = log_string(error_code_name(outcome.error.code));
                fields.object["error_message"] = log_string(outcome.error.message);
            }
            logger->event("agent_round", log_context, std::move(fields),
                          outcome.error.ok() ||
                                  outcome.kind == agent::AgentRoundOutcome::Kind::Continue ||
                                  outcome.kind == agent::AgentRoundOutcome::Kind::FinalText
                              ? "success"
                              : "failure");
            for (std::size_t i = 0; i < outcome.prepared_calls.size(); ++i) {
                json::Value tool_fields = log_object();
                tool_fields.object["call_id"] = log_string(outcome.prepared_calls[i].id);
                tool_fields.object["tool_name"] = log_string(outcome.prepared_calls[i].name);
                tool_fields.object["arguments"] =
                    agent::ReviewLogger::payload(outcome.prepared_calls[i].original_arguments);
                if (i < outcome.tool_results.size())
                    tool_fields.object["result"] =
                        agent::ReviewLogger::payload(outcome.tool_results[i]);
                logger->event("tool_result", log_context, std::move(tool_fields), "success");
            }
        }

        if (!outcome.notice.empty() && !context.options.quiet)
            std::cerr << "Agent notice: " << redact_secrets(outcome.notice, secrets) << "\n";

        if (outcome.kind == agent::AgentRoundOutcome::Kind::Continue) continue;

        if (outcome.kind == agent::AgentRoundOutcome::Kind::FinalText) {
            final_text = outcome.final_text;
            std::cout << final_text;
            if (!final_text.empty() && final_text.back() != '\n') std::cout << '\n';
            finish_log(ok_error(), final_text, state.turn, total_tool_calls);
            return 0;
        }

        agent::pair_dangling_tool_calls(context, conversation, state);
        error = outcome.error.ok()
                    ? Error{ErrorCode::Cancelled,
                            outcome.notice.empty() ? "agent run aborted" : outcome.notice}
                    : outcome.error;
        finish_log(error, final_text, state.turn, total_tool_calls);
        print_error(error);
        return exit_code_for(error.code);
    }
}

}  // namespace ainiux::app
