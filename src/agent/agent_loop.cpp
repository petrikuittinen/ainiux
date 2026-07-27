#include "agent/agent_loop.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>

#include "agent/tools.hpp"
#include "json/json.hpp"

namespace ainiux::agent {
namespace {

json::Value object_value() {
    json::Value value;
    value.type = json::Value::Type::Object;
    return value;
}

json::Value string_value(const std::string& text) {
    json::Value value;
    value.type = json::Value::Type::String;
    value.string = text;
    return value;
}

json::Value bool_value(bool boolean) {
    json::Value value;
    value.type = json::Value::Type::Bool;
    value.boolean = boolean;
    return value;
}

std::string lower_ascii(std::string text) {
    for (char& ch : text)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    return text;
}

bool message_has_status(const std::string& message, int status) {
    const std::string needle = "HTTP " + std::to_string(status);
    return message.find(needle) != std::string::npos;
}

bool tool_result_ok(const std::string& result_json) {
    const json::ParseResult parsed = json::parse(result_json);
    if (!parsed.error.ok() || !parsed.value.is_object()) return false;
    const json::Value* ok = parsed.value.get("ok");
    return ok != nullptr && ok->type == json::Value::Type::Bool && ok->boolean;
}

void rewrite_chat_tool_calls_arguments(json::Value& assistant,
                                       const std::vector<PreparedToolCall>& prepared) {
    json::Value* calls = nullptr;
    if (assistant.object.count("tool_calls")) calls = &assistant.object["tool_calls"];
    if (calls == nullptr || !calls->is_array()) return;
    for (json::Value& item : calls->array) {
        if (!item.is_object()) continue;
        const json::Value* id = item.get("id");
        if (id == nullptr || !id->is_string()) continue;
        for (const PreparedToolCall& call : prepared) {
            if (call.id != id->string) continue;
            json::Value* function = item.object.count("function") ? &item.object["function"] : nullptr;
            if (function == nullptr || !function->is_object()) break;
            function->object["arguments"] = string_value(call.history_arguments);
            if (!call.name.empty()) function->object["name"] = string_value(call.name);
            break;
        }
    }
}

void rewrite_responses_function_arguments(json::Value& item,
                                          const std::vector<PreparedToolCall>& prepared) {
    const json::Value* type = item.get("type");
    if (type == nullptr || !type->is_string() || type->string != "function_call") return;
    const json::Value* call_id = item.get("call_id");
    const json::Value* id = item.get("id");
    const std::string key = call_id != nullptr && call_id->is_string()
                                ? call_id->string
                                : (id != nullptr && id->is_string() ? id->string : std::string());
    if (key.empty()) return;
    for (const PreparedToolCall& call : prepared) {
        if (call.id != key) continue;
        item.object["arguments"] = string_value(call.history_arguments);
        if (!call.name.empty()) item.object["name"] = string_value(call.name);
        break;
    }
}

Error cancellable_sleep_ms(runtime::CancellationToken cancellation, int milliseconds) {
    const int ticks = std::max(1, (milliseconds + 49) / 50);
    for (int tick = 0; tick < ticks; ++tick) {
        if (cancellation.cancelled())
            return {ErrorCode::Cancelled, "agent transport retry cancelled during backoff"};
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return ok_error();
}

void append_text_message(provider::ToolConversation& conversation,
                         const std::string& role,
                         const std::string& content) {
    json::Value item = object_value();
    item.object["role"] = string_value(role);
    item.object["content"] = string_value(content);
    conversation.continuation_items_json.push_back(json::stringify(item));
}

std::string normalize_args_for_fingerprint(const PreparedToolCall& call) {
    if (call.arguments_invalid) return "{}";
    if (!call.parsed.normalized_arguments.empty()) return call.parsed.normalized_arguments;
    if (call.parsed.value.is_object()) return json::stringify(call.parsed.value);
    return call.history_arguments;
}

AgentRoundOutcome abort_outcome(AgentLoopState& state, const std::string& reason) {
    state.aborted = true;
    state.abort_reason = reason;
    AgentRoundOutcome outcome;
    outcome.kind = AgentRoundOutcome::Kind::Aborted;
    outcome.notice = reason;
    outcome.error = {ErrorCode::Cancelled, reason};
    return outcome;
}

bool tool_result_is_approval_denial(const std::string& result) {
    const json::ParseResult parsed = json::parse(result);
    if (!parsed.error.ok() || !parsed.value.is_object()) return false;
    const json::Value* error = parsed.value.get("error");
    if (error == nullptr || !error->is_object()) return false;
    const json::Value* code = error->get("code");
    const json::Value* message = error->get("message");
    const std::string code_text =
        code != nullptr && code->is_string() ? lower_ascii(code->string) : std::string();
    const std::string message_text =
        message != nullptr && message->is_string() ? lower_ascii(message->string) : std::string();
    if (code_text == "cancelled") return false;
    return message_text.find("approval") != std::string::npos &&
           (message_text.find("requires") != std::string::npos ||
            message_text.find("without user") != std::string::npos ||
            message_text.find("user selected no") != std::string::npos ||
            message_text.find("user denied") != std::string::npos);
}

AgentRoundOutcome execute_prepared_calls(AgentLoopState& state,
                                         const AgentLoopLimits& limits,
                                         const provider::RequestContext& context,
                                         provider::ToolConversation& conversation,
                                         std::vector<PreparedToolCall> prepared,
                                         ToolExecutor executor,
                                         runtime::CancellationToken cancellation) {
    AgentRoundOutcome outcome;
    outcome.prepared_calls = prepared;

    if (state.scripted_turns >= limits.max_scripted_turns) {
        const std::string deferred =
            cancelled_tool_result_json("Tool was not executed because the agent turn cap "
                                       "was reached. Continue the task to retry it.");
        outcome.tool_results.assign(prepared.size(), deferred);
        append_prepared_tool_results(context, conversation, prepared, outcome.tool_results);
        if (limits.interactive) {
            outcome.kind = AgentRoundOutcome::Kind::NeedsUserContinue;
            outcome.notice = "agent turn cap of " + std::to_string(limits.max_scripted_turns) +
                             " reached; continue?";
            return outcome;
        }
        state.aborted = true;
        state.abort_reason =
            "agent turn cap of " + std::to_string(limits.max_scripted_turns) + " exceeded";
        outcome.kind = AgentRoundOutcome::Kind::Aborted;
        outcome.notice = state.abort_reason;
        outcome.error = {ErrorCode::Cancelled, state.abort_reason};
        return outcome;
    }
    ++state.turn;
    ++state.scripted_turns;

    std::string soft_notice;
    if (track_identical_calls(state, prepared, limits, &soft_notice)) {
        pair_dangling_tool_calls(context, conversation, state);
        return abort_outcome(state, state.abort_reason);
    }

    // Record dangling ids before execution so cancel mid-tool can pair them.
    state.dangling_call_ids.clear();
    for (const PreparedToolCall& call : prepared) state.dangling_call_ids.push_back(call.id);

    std::vector<std::string> results;
    results.reserve(prepared.size());
    std::size_t failures = 0;
    bool approval_declined = false;
    for (const PreparedToolCall& call : prepared) {
        if (cancellation.cancelled()) {
            const std::string cancelled = cancelled_tool_result_json("Tool was not executed.");
            while (results.size() < prepared.size()) results.push_back(cancelled);
            append_prepared_tool_results(context, conversation, prepared, results);
            state.dangling_call_ids.clear();
            outcome.kind = AgentRoundOutcome::Kind::Error;
            outcome.error = {ErrorCode::Cancelled, "agent tool round cancelled"};
            outcome.tool_results = std::move(results);
            return outcome;
        }
        std::string result;
        if (approval_declined) {
            result = cancelled_tool_result_json(
                "Tool was not executed after the user declined approval.");
        } else if (call.arguments_invalid) {
            result = invalid_arguments_tool_result(
                call.name.empty() ? std::string("unknown") : call.name,
                call.parsed.error.ok() ? "tool arguments must be a JSON object"
                                       : call.parsed.error.message,
                call.original_arguments);
        } else if (!executor) {
            result = tool_error_result("internal", "no tool executor configured");
        } else {
            // Never automatically re-run a failed tool; one execution only.
            result = executor(call.name, call.original_arguments, cancellation);
        }
        if (!tool_result_ok(result)) ++failures;
        if (tool_result_is_approval_denial(result)) approval_declined = true;
        results.push_back(std::move(result));
    }

    append_prepared_tool_results(context, conversation, prepared, results);
    state.dangling_call_ids.clear();
    outcome.tool_results = results;

    if (approval_declined) {
        state.consecutive_all_failed_turns = 0;
        state.last_fingerprint.clear();
        state.identical_repeat_count = 0;
        state.soft_repeat_notice_pending = false;
        outcome.kind = AgentRoundOutcome::Kind::FinalText;
        outcome.notice = "Action cancelled by user.";
        outcome.error = ok_error();
        return outcome;
    }

    if (failures == prepared.size() && !prepared.empty()) {
        ++state.consecutive_all_failed_turns;
        // Identical-call soft/hard ladder owns repeated same-call failures.
        // Consecutive-failure abort applies when the model keeps failing with
        // changing calls (identical_repeat_count stays below the soft threshold).
        if (state.consecutive_all_failed_turns >= limits.consecutive_failure_turns &&
            state.identical_repeat_count < limits.soft_identical_repeats) {
            // Collect failures; lead with the most user-actionable policy reason
            // (e.g. outside-project path) rather than a less helpful follow-up
            // like "touch is not on the allowlist".
            struct FailureNote {
                std::string tool;
                std::string message;
                int priority = 0;  // higher = surface first
            };
            std::vector<FailureNote> notes;
            notes.reserve(prepared.size());
            auto priority_for = [](const std::string& message) -> int {
                const std::string lower = lower_ascii(message);
                if (lower.find("outside the project") != std::string::npos ||
                    lower.find("escapes workspace") != std::string::npos ||
                    (lower.find("~") != std::string::npos &&
                     lower.find("forbidden") != std::string::npos))
                    return 100;
                if (lower.find("approval") != std::string::npos ||
                    lower.find("headless") != std::string::npos)
                    return 80;
                if (lower.find("refusing") != std::string::npos ||
                    lower.find("policy") != std::string::npos)
                    return 60;
                if (lower.find("allowlist") != std::string::npos) return 20;
                return 40;
            };
            for (std::size_t i = 0; i < prepared.size(); ++i) {
                FailureNote note;
                note.tool = prepared[i].name;
                if (i < results.size()) {
                    const json::ParseResult parsed = json::parse(results[i]);
                    if (parsed.error.ok() && parsed.value.is_object()) {
                        const json::Value* err = parsed.value.get("error");
                        if (err != nullptr && err->is_object()) {
                            const json::Value* msg = err->get("message");
                            if (msg != nullptr && msg->is_string()) note.message = msg->string;
                        }
                    }
                }
                note.priority = priority_for(note.message);
                notes.push_back(std::move(note));
            }
            std::stable_sort(notes.begin(), notes.end(),
                             [](const FailureNote& a, const FailureNote& b) {
                                 return a.priority > b.priority;
                             });

            std::string summary;
            if (!notes.empty() && notes.front().priority >= 100) {
                // Primary user-facing explanation for path escapes.
                summary =
                    "Agent stopped: tools cannot create or access paths outside the project "
                    "directory. Use a path relative to the project root only "
                    "(not ~/…, $HOME, or absolute paths).";
                if (!notes.front().message.empty()) {
                    summary += " Detail: ";
                    const std::string& m = notes.front().message;
                    summary += m.size() > 140 ? m.substr(0, 137) + "..." : m;
                }
            } else {
                summary = "Agent stopped after " +
                          std::to_string(limits.consecutive_failure_turns) +
                          " consecutive turns where every tool call failed.";
                if (!notes.empty() && !notes.front().message.empty()) {
                    summary += " ";
                    summary += notes.front().tool;
                    summary += ": ";
                    const std::string& m = notes.front().message;
                    summary += m.size() > 160 ? m.substr(0, 157) + "..." : m;
                }
            }
            // Compact secondary list (other tools this turn).
            if (notes.size() > 1) {
                summary += " Also failed:";
                for (std::size_t i = 1; i < notes.size() && i < 4; ++i) {
                    summary += " ";
                    summary += notes[i].tool;
                    if (i + 1 < notes.size() && i + 1 < 4) summary += ",";
                }
            }
            return abort_outcome(state, summary);
        }
    } else {
        state.consecutive_all_failed_turns = 0;
    }

    if (state.soft_repeat_notice_pending) {
        state.soft_repeat_notice_pending = false;
        append_text_message(conversation, "user", identical_call_soft_notice_text());
        outcome.notice = identical_call_soft_notice_text();
    } else if (!soft_notice.empty()) {
        outcome.notice = soft_notice;
    }

    outcome.kind = AgentRoundOutcome::Kind::Continue;
    outcome.error = ok_error();
    return outcome;
}

}  // namespace

void reset_agent_loop_for_user_turn(AgentLoopState& state) {
    state.scripted_turns = 0;
    state.consecutive_all_failed_turns = 0;
    state.last_fingerprint.clear();
    state.identical_repeat_count = 0;
    state.soft_repeat_notice_pending = false;
    state.aborted = false;
    state.abort_reason.clear();
    state.dangling_call_ids.clear();
}

void append_conversation_text(provider::ToolConversation& conversation,
                              const std::string& role,
                              const std::string& content) {
    json::Value item;
    item.type = json::Value::Type::Object;
    item.object["role"].type = json::Value::Type::String;
    item.object["role"].string = role;
    item.object["content"].type = json::Value::Type::String;
    item.object["content"].string = content;
    conversation.continuation_items_json.push_back(json::stringify(item));
}

std::size_t append_request_only_context(
    provider::ToolConversation& conversation,
    const std::string& content) {
    append_conversation_text(conversation, "user", content);
    return conversation.continuation_items_json.empty()
               ? 0
               : conversation.continuation_items_json.size() - 1;
}

bool remove_request_only_context(provider::ToolConversation& conversation,
                                 std::size_t index) {
    if (index >= conversation.continuation_items_json.size()) return false;
    conversation.continuation_items_json.erase(
        conversation.continuation_items_json.begin() +
        static_cast<std::ptrdiff_t>(index));
    return true;
}

ToolProtocol default_tool_protocol(bool provider_supports_tool_calls) {
    return provider_supports_tool_calls ? ToolProtocol::Native : ToolProtocol::Xml;
}

bool is_immediate_fail_transport_error(const Error& error) {
    if (error.ok()) return false;
    if (error.code == ErrorCode::Auth || error.code == ErrorCode::BadArgs ||
        error.code == ErrorCode::BadUrl || error.code == ErrorCode::ProviderSchema ||
        error.code == ErrorCode::UnsupportedFeature || error.code == ErrorCode::Cancelled ||
        error.code == ErrorCode::Config || error.code == ErrorCode::JsonParse)
        return true;
    if (error.code == ErrorCode::HttpStatus) {
        return message_has_status(error.message, 400) || message_has_status(error.message, 401) ||
               message_has_status(error.message, 403) || message_has_status(error.message, 404);
    }
    if (error.code == ErrorCode::Auth) return true;
    const std::string lower = lower_ascii(error.message);
    if (lower.find("context length") != std::string::npos ||
        lower.find("context_length") != std::string::npos ||
        lower.find("maximum context") != std::string::npos ||
        lower.find("too many tokens") != std::string::npos)
        return true;
    return false;
}

bool is_retryable_transport_error(const Error& error) {
    if (error.ok() || is_immediate_fail_transport_error(error)) return false;
    if (error.code == ErrorCode::Timeout || error.code == ErrorCode::Connect ||
        error.code == ErrorCode::Dns || error.code == ErrorCode::Tls ||
        error.code == ErrorCode::RateLimit || error.code == ErrorCode::SseParse)
        return true;
    if (error.code == ErrorCode::HttpStatus) {
        return message_has_status(error.message, 429) || message_has_status(error.message, 500) ||
               message_has_status(error.message, 502) || message_has_status(error.message, 503) ||
               message_has_status(error.message, 529);
    }
    return false;
}

int transport_backoff_seconds(int zero_based_attempt) {
    if (zero_based_attempt <= 0) return 1;
    if (zero_based_attempt == 1) return 2;
    return 4;
}

Error send_tool_round_with_transport_retries(
    const provider::RequestContext& context,
    const provider::ToolConversation& conversation,
    const std::vector<provider::FunctionDefinition>& tools,
    provider::ToolRoundResult& result,
    runtime::CancellationToken cancellation,
    int transport_attempts,
    const provider::ToolRoundObserver* observer,
    const provider::ToolRoundContext& observation_context,
    std::function<void(const Error& error, int attempt, int backoff_seconds)> on_retry,
    provider::ReasoningDeltaCallback on_reasoning_delta) {
    if (transport_attempts < 1) transport_attempts = 1;
    Error error;
    for (int attempt = 0; attempt < transport_attempts; ++attempt) {
        result = provider::ToolRoundResult{};
        provider::ToolRoundContext attempt_context = observation_context;
        attempt_context.retry_attempt = static_cast<std::size_t>(attempt + 1);
        error = provider::send_tool_round(context, conversation, tools, result, cancellation,
                                          observer, attempt_context, on_reasoning_delta);
        if (error.ok()) return error;
        if (is_immediate_fail_transport_error(error)) return error;
        if (!is_retryable_transport_error(error)) return error;
        if (attempt + 1 >= transport_attempts) return error;
        const int backoff = transport_backoff_seconds(attempt);
        if (on_retry) on_retry(error, attempt + 1, backoff);
        Error wait = cancellable_sleep_ms(cancellation, backoff * 1000);
        if (!wait.ok()) return wait;
    }
    return error;
}

std::vector<PreparedToolCall> prepare_tool_calls(
    const std::vector<provider::ToolCall>& calls,
    const std::vector<std::string>& known_tool_names) {
    std::vector<PreparedToolCall> prepared;
    prepared.reserve(calls.size());
    for (const provider::ToolCall& call : calls) {
        PreparedToolCall item;
        item.id = call.id;
        item.original_arguments = call.arguments_json;
        item.name = call.name;
        if (!known_tool_names.empty()) {
            const std::string repaired = repair_tool_name(call.name, known_tool_names);
            if (!repaired.empty()) item.name = repaired;
        }
        item.parsed = parse_tool_arguments(call.arguments_json);
        if (!item.parsed.error.ok() || !item.parsed.value.is_object()) {
            item.arguments_invalid = true;
            item.history_arguments = "{}";
        } else {
            item.history_arguments = item.parsed.normalized_arguments.empty()
                                         ? json::stringify(item.parsed.value)
                                         : item.parsed.normalized_arguments;
            // Prefer compact canonical object form for history when normalized was the
            // source extract; keep a valid JSON object string always.
            if (item.history_arguments.empty()) item.history_arguments = "{}";
        }
        prepared.push_back(std::move(item));
    }
    return prepared;
}

std::string tool_call_fingerprint(const PreparedToolCall& call) {
    return call.name + "\n" + normalize_args_for_fingerprint(call);
}

Error sanitize_round_continuation_for_history(provider::ApiKind api_kind,
                                              provider::ToolRoundResult& round,
                                              const std::vector<PreparedToolCall>& prepared) {
    // Also keep in-memory tool_calls history-safe for any later serializer.
    for (provider::ToolCall& call : round.tool_calls) {
        for (const PreparedToolCall& prepared_call : prepared) {
            if (prepared_call.id != call.id) continue;
            call.name = prepared_call.name;
            call.arguments_json = prepared_call.history_arguments;
            break;
        }
    }
    for (std::string& encoded : round.continuation_items_json) {
        json::ParseResult parsed = json::parse(encoded);
        if (!parsed.error.ok() || !parsed.value.is_object())
            return {ErrorCode::Internal, "invalid tool continuation item while sanitizing history"};
        if (api_kind == provider::ApiKind::Responses) {
            rewrite_responses_function_arguments(parsed.value, prepared);
        } else {
            const json::Value* role = parsed.value.get("role");
            if (role != nullptr && role->is_string() && role->string == "assistant")
                rewrite_chat_tool_calls_arguments(parsed.value, prepared);
        }
        encoded = json::stringify(parsed.value);
    }
    return ok_error();
}

void append_prepared_tool_results(const provider::RequestContext& context,
                                  provider::ToolConversation& conversation,
                                  const std::vector<PreparedToolCall>& prepared,
                                  const std::vector<std::string>& result_json) {
    std::vector<provider::ToolCall> calls;
    calls.reserve(prepared.size());
    for (const PreparedToolCall& item : prepared) {
        provider::ToolCall call;
        call.id = item.id;
        call.name = item.name;
        call.arguments_json = item.history_arguments;
        calls.push_back(std::move(call));
    }
    provider::append_tool_results(context, calls, result_json, conversation);
}

void pair_dangling_tool_calls(const provider::RequestContext& context,
                              provider::ToolConversation& conversation,
                              AgentLoopState& state,
                              const std::string& message) {
    if (state.dangling_call_ids.empty()) return;
    std::vector<provider::ToolCall> calls;
    std::vector<std::string> results;
    calls.reserve(state.dangling_call_ids.size());
    results.reserve(state.dangling_call_ids.size());
    const std::string body = cancelled_tool_result_json(message);
    for (const std::string& id : state.dangling_call_ids) {
        provider::ToolCall call;
        call.id = id;
        call.name = "unknown";
        call.arguments_json = "{}";
        calls.push_back(std::move(call));
        results.push_back(body);
    }
    provider::append_tool_results(context, calls, results, conversation);
    state.dangling_call_ids.clear();
}

bool content_looks_like_xml_tool_markup(const std::string& text) {
    const std::string lower = lower_ascii(text);
    return lower.find("<tool_call>") != std::string::npos;
}

void note_native_channel_content(AgentLoopState& state,
                                 const std::string& assistant_text,
                                 std::string* downgrade_notice) {
    if (state.protocol != ToolProtocol::Native) return;
    if (content_looks_like_xml_tool_markup(assistant_text)) {
        ++state.native_xml_leak_strikes;
        if (state.native_xml_leak_strikes >= 2) {
            state.protocol = ToolProtocol::Xml;
            if (downgrade_notice != nullptr) {
                *downgrade_notice =
                    "native tool channel leaked <tool_call> markup twice; "
                    "downgrading this session to the XML tool protocol";
            }
        }
    } else {
        state.native_xml_leak_strikes = 0;
    }
}

bool track_identical_calls(AgentLoopState& state,
                           const std::vector<PreparedToolCall>& prepared,
                           const AgentLoopLimits& limits,
                           std::string* soft_notice) {
    if (prepared.empty()) {
        state.last_fingerprint.clear();
        state.identical_repeat_count = 0;
        return false;
    }
    // Fingerprint the whole parallel group in stable order.
    std::vector<std::string> parts;
    parts.reserve(prepared.size());
    for (const PreparedToolCall& call : prepared) parts.push_back(tool_call_fingerprint(call));
    std::sort(parts.begin(), parts.end());
    std::string fingerprint;
    for (const std::string& part : parts) {
        fingerprint += part;
        fingerprint.push_back('\x1e');
    }
    if (fingerprint == state.last_fingerprint) {
        ++state.identical_repeat_count;
    } else {
        state.last_fingerprint = fingerprint;
        state.identical_repeat_count = 1;
    }
    if (state.identical_repeat_count >= limits.hard_identical_repeats) {
        state.aborted = true;
        state.abort_reason =
            "identical tool call repeated " + std::to_string(limits.hard_identical_repeats) +
            " times (" + prepared.front().name + "); aborting agent task";
        return true;
    }
    if (state.identical_repeat_count == limits.soft_identical_repeats) {
        state.soft_repeat_notice_pending = true;
        if (soft_notice != nullptr) *soft_notice = identical_call_soft_notice_text();
    }
    return false;
}

std::string identical_call_soft_notice_text() {
    return "You have repeated the same call 3 times with the same result. "
           "Try a different approach or explain the blocker.";
}

std::string cancelled_tool_result_json(const std::string& message) {
    json::Value root = object_value();
    root.object["ok"] = bool_value(false);
    json::Value error = object_value();
    error.object["code"] = string_value("cancelled");
    error.object["message"] = string_value(message);
    root.object["error"] = std::move(error);
    root.object["data"] = object_value();
    root.object["warnings"] = json::Value{};
    root.object["warnings"].type = json::Value::Type::Array;
    root.object["truncated"] = bool_value(false);
    root.object["metadata"] = object_value();
    return json::stringify(root);
}

AgentRoundOutcome handle_agent_tool_round(
    AgentLoopState& state,
    const AgentLoopLimits& limits,
    const provider::RequestContext& context,
    provider::ToolConversation& conversation,
    provider::ToolRoundResult round,
    const std::vector<std::string>& known_tool_names,
    ToolExecutor executor,
    runtime::CancellationToken cancellation) {
    AgentRoundOutcome outcome;
    if (state.aborted) {
        outcome.kind = AgentRoundOutcome::Kind::Aborted;
        outcome.notice = state.abort_reason;
        outcome.error = {ErrorCode::Cancelled, state.abort_reason};
        return outcome;
    }

    std::string downgrade_notice;
    note_native_channel_content(state, round.content, &downgrade_notice);
    if (!downgrade_notice.empty()) {
        outcome.protocol_downgraded = true;
        outcome.notice = downgrade_notice;
    }

    // System prompt stays static for the session (prompt caching). Protocol
    // downgrades are communicated as a separate user notice, not by rewriting
    // the system message.
    if (!downgrade_notice.empty()) {
        append_text_message(
            conversation, "user",
            downgrade_notice +
                " From now on emit exactly one <tool_call><name>...</name>"
                "<args>{...}</args></tool_call> block per turn with one JSON object in <args>.");
    }

    if (round.tool_calls.empty()) {
        if (!round.content.empty() && content_looks_like_xml_tool_markup(round.content) &&
            state.protocol == ToolProtocol::Xml) {
            AgentRoundOutcome xml_outcome = handle_agent_xml_round(
                state, limits, context, conversation, round.content, known_tool_names,
                std::move(executor), cancellation);
            if (!downgrade_notice.empty()) {
                xml_outcome.protocol_downgraded = true;
                if (xml_outcome.notice.empty()) xml_outcome.notice = downgrade_notice;
                else xml_outcome.notice = downgrade_notice + "; " + xml_outcome.notice;
            }
            return xml_outcome;
        }
        ++state.turn;
        ++state.scripted_turns;
        // Append bare assistant text continuation if present.
        for (const std::string& item : round.continuation_items_json)
            conversation.continuation_items_json.push_back(item);
        outcome.kind = AgentRoundOutcome::Kind::FinalText;
        outcome.final_text = round.content;
        outcome.error = ok_error();
        if (!downgrade_notice.empty()) {
            outcome.protocol_downgraded = true;
            outcome.notice = downgrade_notice;
        }
        return outcome;
    }

    std::vector<PreparedToolCall> prepared = prepare_tool_calls(round.tool_calls, known_tool_names);
    Error sanitize_error =
        sanitize_round_continuation_for_history(context.api_kind, round, prepared);
    if (!sanitize_error.ok()) {
        outcome.kind = AgentRoundOutcome::Kind::Error;
        outcome.error = sanitize_error;
        return outcome;
    }
    for (const std::string& item : round.continuation_items_json)
        conversation.continuation_items_json.push_back(item);

    outcome = execute_prepared_calls(state, limits, context, conversation, std::move(prepared),
                                     std::move(executor), cancellation);
    if (outcome.protocol_downgraded == false && !downgrade_notice.empty()) {
        outcome.protocol_downgraded = true;
        if (outcome.notice.empty()) outcome.notice = downgrade_notice;
    }
    return outcome;
}

AgentRoundOutcome handle_agent_xml_round(
    AgentLoopState& state,
    const AgentLoopLimits& limits,
    const provider::RequestContext& context,
    provider::ToolConversation& conversation,
    const std::string& assistant_text,
    const std::vector<std::string>& known_tool_names,
    ToolExecutor executor,
    runtime::CancellationToken cancellation) {
    AgentRoundOutcome outcome;
    state.protocol = ToolProtocol::Xml;
    append_text_message(conversation, "assistant", assistant_text);

    const XmlToolCallParseResult xml = parse_xml_tool_call(assistant_text);
    if (!xml.error.ok()) {
        append_text_message(conversation, "user",
                            std::string("Tool channel error: ") + xml.error.message);
        outcome.kind = AgentRoundOutcome::Kind::Continue;
        outcome.notice = xml.error.message;
        outcome.error = ok_error();
        ++state.turn;
        ++state.scripted_turns;
        return outcome;
    }
    if (!xml.found) {
        ++state.turn;
        ++state.scripted_turns;
        outcome.kind = AgentRoundOutcome::Kind::FinalText;
        outcome.final_text = assistant_text;
        outcome.error = ok_error();
        return outcome;
    }

    provider::ToolCall synthetic;
    synthetic.id = "xml_call_" + std::to_string(state.turn + 1);
    synthetic.name = xml.name;
    synthetic.arguments_json = xml.arguments_text;
    std::vector<PreparedToolCall> prepared =
        prepare_tool_calls({synthetic}, known_tool_names);

    // XML results are also recorded as tool-role messages when the API is chat,
    // so later native upgrades (if any) stay history-safe. For Responses, the
    // same append_tool_results path is used with a synthetic call id.
    json::Value assistant = object_value();
    assistant.object["role"] = string_value("assistant");
    assistant.object["content"] = string_value(std::string());
    json::Value tool_calls;
    tool_calls.type = json::Value::Type::Array;
    json::Value item = object_value();
    item.object["id"] = string_value(prepared.front().id);
    item.object["type"] = string_value("function");
    json::Value function = object_value();
    function.object["name"] = string_value(prepared.front().name);
    function.object["arguments"] = string_value(prepared.front().history_arguments);
    item.object["function"] = std::move(function);
    tool_calls.array.push_back(std::move(item));
    if (context.api_kind != provider::ApiKind::Responses) {
        // Replace the plain assistant text already appended with a structured
        // tool-call form for Chat Completions history when possible.
        // Keep the textual assistant turn for XML fidelity and still emit tool results.
    }

    return execute_prepared_calls(state, limits, context, conversation, std::move(prepared),
                                  std::move(executor), cancellation);
}

}  // namespace ainiux::agent
