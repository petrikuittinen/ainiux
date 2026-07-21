#include "agent/session_runtime.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <utility>

#include "agent/compact.hpp"
#include "agent/project_root.hpp"
#include "agent/tool_display.hpp"
#include "security/redact.hpp"

namespace ainiux::agent {
namespace {

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

std::vector<std::string> known_tool_names(const ReadToolRegistry& tools) {
    std::vector<std::string> names;
    for (const provider::FunctionDefinition& definition : tools.definitions())
        names.push_back(definition.name);
    return names;
}

}  // namespace

bool AgentSessionRuntime::is_interrupted(runtime::CancellationToken cancellation,
                                         const std::function<bool()>& interrupted) const {
    if (cancellation.cancelled()) return true;
    if (interrupted && interrupted()) return true;
    return false;
}

void AgentSessionRuntime::reset() {
    if (session_id_ > 0 && session_store_.is_open()) {
        // Best-effort close of a still-running session when the runtime is torn down.
        (void)session_store_.finish_session(session_id_, "cancelled", "", "Cancelled",
                                            "agent session closed",
                                            static_cast<long long>(session_turns_),
                                            static_cast<long long>(session_tool_calls_));
    }
    logger_.reset();
    session_store_.close();
    tools_ = ReadToolRegistry{};
    prompts_ = TrustedPrompts{};
    agents_md_ = AgentsMdBundle{};
    conversation_ = provider::ToolConversation{};
    state_ = AgentLoopState{};
    limits_ = AgentLoopLimits{};
    known_tools_.clear();
    secrets_.clear();
    session_id_ = 0;
    session_turns_ = 0;
    session_tool_calls_ = 0;
    conversation_seeded_ = false;
    prepared_ = false;
    options_ = SessionRuntimeOptions{};
}

Error AgentSessionRuntime::prepare(const provider::RequestContext& context,
                                   runtime::CancellationToken cancellation,
                                   std::function<bool()> interrupted,
                                   SessionRuntimeOptions options) {
    reset();
    options_ = std::move(options);
    if (options_.workspace.empty()) options_.workspace = ".";
    {
        std::string absolute;
        Error root_error = resolve_agent_project_root(options_.workspace, absolute);
        if (!root_error.ok()) return root_error;
        options_.workspace = absolute;
    }
    secrets_ = configured_secrets(context);

    auto interrupted_fn = [&]() { return is_interrupted(cancellation, interrupted); };

    if (options_.enable_agent_log) {
        Error log_error;
        logger_ = ReviewLogger::create(
            options_.workspace, options_.security_review_log_keep_runs, secrets_,
            [&](const std::string& warning) {
                if (!context.options.quiet) std::cerr << warning << "\n";
            },
            log_error, "agent");
        if (!logger_ && !context.options.quiet) {
            std::cerr << "AGENT LOGGING DISABLED: " << redact_secrets(log_error.message, secrets_)
                      << "; the agent will continue\n";
        } else if (logger_ && !context.options.quiet) {
            std::cerr << "Agent diagnostic log (live): " << logger_->partial_path() << "\n"
                      << "  tail -f that path while the agent runs; finalized as "
                      << logger_->final_path() << " on completion\n";
        }
    }

    index::Options index_options;
    index_options.workspace = options_.workspace;
    index_options.max_source_code_file_size = options_.max_source_code_file_size;
    index_options.cancellation = cancellation;
    index_options.interrupted = interrupted_fn;
    index::RefreshStats index_stats;
    Error error = index::refresh(index_options, index_stats);
    if (logger_) {
        json::Value fields = log_object();
        fields.object["discovered"] = log_number(index_stats.discovered);
        fields.object["indexed"] = log_number(index_stats.indexed);
        fields.object["unchanged"] = log_number(index_stats.unchanged);
        fields.object["skipped"] = log_number(index_stats.skipped);
        if (!error.ok()) {
            fields.object["error_code"] = log_string(error_code_name(error.code));
            fields.object["error_message"] = log_string(error.message);
        }
        logger_->event("index_result", {"index"}, std::move(fields),
                       error.ok() ? "success" : "failure");
    }
    if (!error.ok()) {
        reset();
        return error;
    }
    if (!context.options.quiet) {
        for (const std::string& diagnostic : index_stats.diagnostics)
            std::cerr << "Index warning: " << redact_secrets(diagnostic, secrets_) << "\n";
        std::cerr << "Code index refreshed: " << index_stats.discovered << " eligible, "
                  << index_stats.indexed << " indexed, " << index_stats.unchanged
                  << " unchanged, " << index_stats.skipped << " skipped.\n";
    }

    index::Snapshot snapshot;
    error = index::load_snapshot(index_options, snapshot);
    if (!error.ok()) {
        reset();
        return error;
    }

    ToolRegistryOptions tool_options;
    tool_options.allow_mutations = options_.allow_mutations;
    tool_options.history_backup = options_.history_backup;
    error = ReadToolRegistry::create(index_options, std::move(snapshot), secrets_, tools_,
                                     tool_options);
    if (!error.ok()) {
        reset();
        return error;
    }

    error = load_trusted_prompts(options_.trusted_prompt_dir, prompts_);
    if (!error.ok()) {
        reset();
        return error;
    }

    const bool supports_tools = provider::capabilities_for(context).tool_calls;
    state_.protocol = default_tool_protocol(supports_tools);
    limits_.interactive = options_.interactive;
    limits_.max_scripted_turns = 50;
    known_tools_ = known_tool_names(tools_);

    if (options_.enable_session_db) {
        error = session_store_.open(options_.workspace);
        if (!error.ok()) {
            reset();
            return error;
        }
        if (!context.options.quiet)
            std::cerr << "Agent session DB: " << session_store_.path() << "\n";
    }

    error = load_root_agents_md(options_.workspace, kDefaultAgentsMdMaxBytes, agents_md_);
    if (!error.ok()) {
        if (!context.options.quiet)
            std::cerr << "Agent warning: could not load AGENTS.md: "
                      << redact_secrets(error.message, secrets_) << "\n";
        if (logger_) {
            json::Value fields = log_object();
            fields.object["error_code"] = log_string(error_code_name(error.code));
            fields.object["error_message"] = log_string(error.message);
            logger_->event("agents_md", {"agents_md"}, std::move(fields), "failure");
        }
        agents_md_ = AgentsMdBundle{};
        error = ok_error();
    } else if (logger_) {
        json::Value fields = log_object();
        fields.object["documents"] = log_number(agents_md_.documents.size());
        fields.object["total_bytes"] = log_number(agents_md_.total_bytes);
        fields.object["truncated"] = log_bool(agents_md_.truncated);
        logger_->event("agents_md", {"agents_md"}, std::move(fields), "success");
    }

    prepared_ = true;
    return ok_error();
}

Error AgentSessionRuntime::finish_session(const std::string& status,
                                          const std::string& final_text,
                                          const std::string& error_code,
                                          const std::string& error_message) {
    if (!session_store_.is_open() || session_id_ <= 0) return ok_error();
    Error error = session_store_.finish_session(
        session_id_, status, redact_secrets(final_text, secrets_), error_code,
        redact_secrets(error_message, secrets_), static_cast<long long>(session_turns_),
        static_cast<long long>(session_tool_calls_));
    if (logger_) {
        json::Value fields = log_object();
        fields.object["session_id"] = log_number(session_id_);
        fields.object["status"] = log_string(status);
        fields.object["turns"] = log_number(session_turns_);
        fields.object["tool_calls"] = log_number(session_tool_calls_);
        logger_->event("session_finish", {"session"}, std::move(fields),
                       error.ok() ? "success" : "failure");
        logger_->finish(log_object(), status == "success" ? "success" : "failure");
        logger_.reset();
    }
    session_id_ = 0;
    return error;
}

SessionTurnResult AgentSessionRuntime::run_user_turn(provider::RequestContext& context,
                                                     const std::string& user_text,
                                                     runtime::CancellationToken cancellation,
                                                     std::function<bool()> interrupted) {
    SessionTurnResult result;
    if (!prepared_) {
        result.error = {ErrorCode::Internal, "agent session runtime is not prepared"};
        return result;
    }
    const std::string text = ascii_trim(user_text);
    if (text.empty()) {
        result.error = {ErrorCode::BadArgs, "agent turn requires a non-empty user message"};
        return result;
    }

    auto progress = [&](const std::string& line) {
        if (options_.on_progress) options_.on_progress(line);
    };

    // First turn: open singleton project row and seed provider conversation.
    if (!conversation_seeded_) {
        if (session_store_.is_open()) {
            AgentProjectRecord project;
            project.status = "running";
            project.provider = context.profile.name;
            project.model = context.options.model;
            project.api =
                context.api_kind == provider::ApiKind::Responses ? "responses" : "chat";
            project.protocol = state_.protocol == ToolProtocol::Xml ? "xml" : "native";
            project.workspace = options_.workspace;
            Error error = session_store_.open_project(project);
            if (!error.ok()) {
                result.error = error;
                return result;
            }
            project.status = "running";
            project.provider = context.profile.name;
            project.model = context.options.model;
            project.api =
                context.api_kind == provider::ApiKind::Responses ? "responses" : "chat";
            project.protocol = state_.protocol == ToolProtocol::Xml ? "xml" : "native";
            project.workspace = options_.workspace;
            (void)session_store_.update_project_meta(project);
            session_id_ = 1;
            if (!context.options.quiet)
                std::cerr << "Agent project thread · " << session_store_.path() << "\n";
            if (logger_) {
                json::Value fields = log_object();
                fields.object["session_id"] = log_number(session_id_);
                fields.object["path"] = log_string(session_store_.path());
                logger_->event("agent_session", {"session"}, std::move(fields), "success");
            }
        }
        seed_agent_conversation(conversation_, prompts_, state_.protocol, text,
                                agents_md_.injection_text);
        conversation_seeded_ = true;
        if (session_store_.is_open() && session_id_ > 0) {
            Error message_error =
                session_store_.append_message("user", redact_secrets(text, secrets_));
            if (!message_error.ok() && !context.options.quiet)
                std::cerr << "Agent warning: could not store goal message: "
                          << redact_secrets(message_error.message, secrets_) << "\n";
        }
        if (!context.options.quiet) {
            std::cerr << "Agent goal: " << redact_secrets(text, secrets_) << "\n"
                      << "Using " << context.profile.name << "/" << context.options.model
                      << " with protocol "
                      << (state_.protocol == ToolProtocol::Xml ? "xml" : "native")
                      << (options_.allow_mutations ? " (workspace writes enabled).\n"
                                                   : " (read-only tools).\n");
            if (!agents_md_.documents.empty()) {
                std::cerr << "Loaded project AGENTS.md (" << agents_md_.total_bytes << " bytes";
                if (agents_md_.truncated) std::cerr << ", truncated";
                std::cerr << ").\n";
            }
        }
    } else {
        conversation_.messages.push_back({"user", text});
        if (session_store_.is_open() && session_id_ > 0) {
            Error message_error =
                session_store_.append_message("user", redact_secrets(text, secrets_));
            if (!message_error.ok() && !context.options.quiet)
                std::cerr << "Agent warning: could not store follow-up: "
                          << redact_secrets(message_error.message, secrets_) << "\n";
        }
    }

    // Auto-compact request transcript when near the context window.
    if (options_.auto_compact && session_store_.is_open()) {
        std::vector<AgentMessageRecord> stored;
        if (session_store_.load_messages(stored).ok()) {
            const long long window = context.options.context_tokens > 0
                                         ? static_cast<long long>(context.options.context_tokens)
                                         : 0LL;
            const long long estimate = estimate_transcript_tokens(stored);
            if (should_auto_compact(options_.auto_compact, options_.compact_limit, window,
                                    estimate)) {
                const std::size_t keep = 12;
                const std::size_t drop =
                    stored.size() > keep ? stored.size() - keep : 0;
                if (drop > 0) {
                    const std::string summary = build_local_compact_summary(stored, drop);
                    Error compact_error = session_store_.compact_with_summary(summary, static_cast<int>(keep));
                    if (compact_error.ok()) {
                        const std::string notice = "auto-compact at ~" +
                            std::to_string(effective_compact_limit_percent(options_.compact_limit, window)) +
                            "% of context window";
                        progress(notice);
                        if (!context.options.quiet) std::cerr << "Agent notice: " << notice << "\n";
                        result.notice = notice;
                        // Rebuild request conversation from summary + recent messages roughly:
                        // keep system + agents_md + latest user for safety.
                        seed_agent_conversation(conversation_, prompts_, state_.protocol, text,
                                                agents_md_.injection_text);
                    }
                }
            }
        }
    }

    std::size_t turn_tool_index = 0;
    auto executor = [&](const std::string& name, const std::string& arguments_json,
                        runtime::CancellationToken token) {
        std::string body = tools_.execute(name, arguments_json, token);
        ++turn_tool_index;
        const std::string line =
            format_compact_tool_line(turn_tool_index, name, arguments_json, body);
        result.compact_tool_lines.push_back(line);
        progress(line);
        if (!options_.interactive && !context.options.quiet) std::cerr << line << "\n";
        if (session_store_.is_open()) {
            (void)session_store_.append_message(
                "tool", line, name, compact_tool_status(body) == "ok",
                compact_tool_args_preview(arguments_json));
        }
        if (options_.show_command_output && name == "run_command") {
            // Best-effort: surface truncated stdout when enabled.
            if (body.find("\"stdout\"") != std::string::npos) {
                // Keep short; full body stays in tool_events via append_tool_event.
            }
        }
        return body;
    };

    std::size_t turn_tool_calls = 0;
    std::string final_text;
    const std::size_t turns_before = state_.turn;

    for (;;) {
        if (is_interrupted(cancellation, interrupted)) {
            pair_dangling_tool_calls(context, conversation_, state_);
            result.error = {ErrorCode::Cancelled, "agent run cancelled"};
            result.final_text = final_text;
            result.turns = state_.turn - turns_before;
            result.tool_calls = turn_tool_calls;
            result.session_turns = session_turns_;
            result.session_tool_calls = session_tool_calls_;
            return result;
        }

        // Native tools when protocol allows; empty definitions on pure XML channel.
        const std::vector<provider::FunctionDefinition> definitions =
            state_.protocol == ToolProtocol::Xml ? std::vector<provider::FunctionDefinition>{}
                                                 : tools_.definitions();

        provider::ToolRoundResult round;
        ReviewLogContext log_context("agent");
        log_context.round = state_.turn + 1;
        log_context.cumulative_tool_calls = session_tool_calls_ + turn_tool_calls;
        provider::ToolRoundObserver observer;
        const provider::ToolRoundObserver* observer_pointer = nullptr;
        if (logger_) {
            observer = logger_->tool_round_observer();
            observer_pointer = &observer;
        }

        Error error = send_tool_round_with_transport_retries(
            context, conversation_, definitions, round, cancellation, limits_.transport_attempts,
            observer_pointer,
            [&]() {
                provider::ToolRoundContext ctx;
                ctx.stage = log_context.stage;
                ctx.round = log_context.round;
                ctx.cumulative_tool_calls = log_context.cumulative_tool_calls;
                return ctx;
            }(),
            [&](const Error& retry_error, int attempt, int backoff_seconds) {
                if (!logger_) return;
                json::Value fields = log_object();
                fields.object["error_code"] = log_string(error_code_name(retry_error.code));
                fields.object["error_message"] = log_string(retry_error.message);
                fields.object["attempt"] = log_number(attempt);
                fields.object["backoff_ms"] = log_number(backoff_seconds * 1000);
                logger_->event("retry_scheduled", log_context, std::move(fields), "failure");
            });
        if (!error.ok()) {
            pair_dangling_tool_calls(context, conversation_, state_);
            result.error = error;
            result.final_text = final_text;
            result.turns = state_.turn - turns_before;
            result.tool_calls = turn_tool_calls;
            result.session_turns = session_turns_;
            result.session_tool_calls = session_tool_calls_;
            return result;
        }

        turn_tool_calls += round.tool_calls.size();
        AgentRoundOutcome outcome = handle_agent_tool_round(
            state_, limits_, context, conversation_, std::move(round), known_tools_, executor,
            cancellation);

        if (logger_) {
            json::Value fields = log_object();
            fields.object["outcome"] = log_string(
                outcome.kind == AgentRoundOutcome::Kind::Continue           ? "continue"
                : outcome.kind == AgentRoundOutcome::Kind::FinalText       ? "final_text"
                : outcome.kind == AgentRoundOutcome::Kind::Aborted         ? "aborted"
                : outcome.kind == AgentRoundOutcome::Kind::NeedsUserContinue ? "needs_user_continue"
                                                                            : "error");
            fields.object["tool_result_count"] = log_number(outcome.tool_results.size());
            fields.object["protocol"] =
                log_string(state_.protocol == ToolProtocol::Xml ? "xml" : "native");
            if (!outcome.notice.empty())
                fields.object["notice"] = ReviewLogger::payload(outcome.notice);
            if (!outcome.final_text.empty())
                fields.object["final_text"] = ReviewLogger::payload(outcome.final_text);
            if (!outcome.error.ok()) {
                fields.object["error_code"] = log_string(error_code_name(outcome.error.code));
                fields.object["error_message"] = log_string(outcome.error.message);
            }
            logger_->event("agent_round", log_context, std::move(fields),
                           outcome.error.ok() ||
                                   outcome.kind == AgentRoundOutcome::Kind::Continue ||
                                   outcome.kind == AgentRoundOutcome::Kind::FinalText ||
                                   outcome.kind == AgentRoundOutcome::Kind::NeedsUserContinue
                               ? "success"
                               : "failure");
            for (std::size_t i = 0; i < outcome.prepared_calls.size(); ++i) {
                json::Value tool_fields = log_object();
                tool_fields.object["call_id"] = log_string(outcome.prepared_calls[i].id);
                tool_fields.object["tool_name"] = log_string(outcome.prepared_calls[i].name);
                tool_fields.object["arguments"] =
                    ReviewLogger::payload(outcome.prepared_calls[i].original_arguments);
                if (i < outcome.tool_results.size())
                    tool_fields.object["result"] =
                        ReviewLogger::payload(outcome.tool_results[i]);
                logger_->event("tool_result", log_context, std::move(tool_fields), "success");
            }
        }

        if (session_store_.is_open() && session_id_ > 0) {
            if (!outcome.notice.empty()) {
                Error notice_error =
                    session_store_.append_message("notice", redact_secrets(outcome.notice, secrets_));
                if (!notice_error.ok() && !context.options.quiet)
                    std::cerr << "Agent warning: could not store notice: "
                              << redact_secrets(notice_error.message, secrets_) << "\n";
            }
            for (std::size_t i = 0; i < outcome.prepared_calls.size(); ++i) {
                const std::string& result_body =
                    i < outcome.tool_results.size() ? outcome.tool_results[i] : std::string{};
                const bool tool_ok = result_body.find("\"ok\":true") != std::string::npos ||
                                     result_body.find("\"ok\": true") != std::string::npos ||
                                     result_body.empty();
                Error tool_error = session_store_.append_tool_event(
                    session_id_, static_cast<long long>(state_.turn),
                    outcome.prepared_calls[i].id, outcome.prepared_calls[i].name,
                    redact_secrets(outcome.prepared_calls[i].original_arguments, secrets_),
                    redact_secrets(result_body, secrets_), tool_ok);
                if (!tool_error.ok() && !context.options.quiet)
                    std::cerr << "Agent warning: could not store tool event: "
                              << redact_secrets(tool_error.message, secrets_) << "\n";
            }
        }

        {
            std::ostringstream progress_line;
            progress_line << "Agent turn " << state_.turn << " ("
                          << (state_.protocol == ToolProtocol::Xml ? "xml" : "native") << "): ";
            if (outcome.kind == AgentRoundOutcome::Kind::FinalText) {
                progress_line << "final answer (" << outcome.final_text.size() << " bytes)";
            } else if (outcome.kind == AgentRoundOutcome::Kind::Continue) {
                if (outcome.prepared_calls.empty()) {
                    progress_line << "continue";
                } else {
                    progress_line << outcome.prepared_calls.size() << " tool call(s):";
                    for (std::size_t i = 0; i < outcome.prepared_calls.size(); ++i) {
                        progress_line << " " << outcome.prepared_calls[i].name;
                        if (i < outcome.tool_results.size()) {
                            const std::string& body = outcome.tool_results[i];
                            const bool ok = body.find("\"ok\":true") != std::string::npos ||
                                            body.find("\"ok\": true") != std::string::npos;
                            progress_line << (ok ? "[ok]" : "[err]");
                        }
                    }
                }
            } else if (outcome.kind == AgentRoundOutcome::Kind::NeedsUserContinue) {
                progress_line << "needs user continue";
            } else {
                progress_line << "stop ("
                              << (outcome.kind == AgentRoundOutcome::Kind::Aborted ? "aborted"
                                                                                  : "error")
                              << ")";
            }
            const std::string line = progress_line.str();
            progress(line);
            if (!context.options.quiet && !options_.interactive) std::cerr << line << "\n";
            if (!outcome.notice.empty() && !context.options.quiet && !options_.interactive)
                std::cerr << "Agent notice: " << redact_secrets(outcome.notice, secrets_) << "\n";
        }

        if (outcome.kind == AgentRoundOutcome::Kind::Continue) continue;

        session_turns_ = state_.turn;
        session_tool_calls_ += turn_tool_calls;
        result.turns = state_.turn - turns_before;
        result.tool_calls = turn_tool_calls;
        result.session_turns = session_turns_;
        result.session_tool_calls = session_tool_calls_;

        if (outcome.kind == AgentRoundOutcome::Kind::FinalText) {
            final_text = outcome.final_text;
            result.error = ok_error();
            result.final_text = final_text;
            if (session_store_.is_open() && session_id_ > 0 && !final_text.empty()) {
                Error assistant_error =
                    session_store_.append_message("assistant", redact_secrets(final_text, secrets_));
                if (!assistant_error.ok() && !context.options.quiet)
                    std::cerr << "Agent warning: could not store assistant message: "
                              << redact_secrets(assistant_error.message, secrets_) << "\n";
            }
            return result;
        }

        if (outcome.kind == AgentRoundOutcome::Kind::NeedsUserContinue) {
            result.error = ok_error();
            result.needs_user_continue = true;
            result.notice = outcome.notice.empty()
                                ? "Agent turn limit reached; send another message to continue."
                                : outcome.notice;
            result.final_text = result.notice;
            if (session_store_.is_open() && session_id_ > 0) {
                (void)session_store_.append_message("notice",
                                                    redact_secrets(result.notice, secrets_));
            }
            return result;
        }

        pair_dangling_tool_calls(context, conversation_, state_);
        result.error = outcome.error.ok()
                           ? Error{ErrorCode::Cancelled,
                                   outcome.notice.empty() ? "agent run aborted" : outcome.notice}
                           : outcome.error;
        result.final_text = final_text;
        result.notice = outcome.notice;
        return result;
    }
}

}  // namespace ainiux::agent
