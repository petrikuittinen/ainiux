#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "agent/tool_args.hpp"
#include "common.hpp"
#include "provider/provider.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::agent {

// Channel used for model tool invocations in a session.
enum class ToolProtocol {
    Native,  // provider tool_calls / function_call items
    Xml,     // single <tool_call><name/><args/></tool_call> in assistant text
};

// Fixed limits from the v1.0 tool-call reliability plan.
struct AgentLoopLimits {
    std::size_t soft_identical_repeats = 3;
    std::size_t hard_identical_repeats = 5;
    std::size_t consecutive_failure_turns = 3;
    std::size_t max_scripted_turns = 50;
    int transport_attempts = 3;  // total attempts per request, not "extra" retries
    bool interactive = false;    // interactive: ask to continue at turn cap
};

struct AgentLoopState {
    ToolProtocol protocol = ToolProtocol::Native;
    std::size_t turn = 0;
    // Model tool rounds consumed since the most recent user message. Keep this
    // separate from cumulative turn so an approved continuation gets a fresh
    // budget without losing session/log numbering.
    std::size_t scripted_turns = 0;
    std::size_t consecutive_all_failed_turns = 0;
    std::size_t native_xml_leak_strikes = 0;
    std::string last_fingerprint;
    std::size_t identical_repeat_count = 0;
    bool soft_repeat_notice_pending = false;
    bool aborted = false;
    std::string abort_reason;
    // Call ids that still need a tool-result message in provider history.
    std::vector<std::string> dangling_call_ids;
};

// A new explicit user message starts a fresh retry/abort segment while retaining
// cumulative turn numbering and protocol health for the project session.
void reset_agent_loop_for_user_turn(AgentLoopState& state);

struct PreparedToolCall {
    std::string id;
    std::string name;
    std::string original_arguments;
    std::string history_arguments;  // "{}" when original was invalid
    ToolArgParseResult parsed;
    bool arguments_invalid = false;
};

struct AgentRoundOutcome {
    enum class Kind {
        Continue,           // tools ran (or notice injected); call the model again
        FinalText,          // assistant text without tool calls
        Aborted,            // hard stop (loop / consecutive fail / turn cap)
        NeedsUserContinue,  // interactive turn-cap: ask the user
        Error,              // transport or internal error
    };
    Kind kind = Kind::Error;
    std::string final_text;
    std::string notice;  // soft loop notice, protocol downgrade, or abort detail
    Error error;
    std::vector<std::string> tool_results;
    std::vector<PreparedToolCall> prepared_calls;
    bool protocol_downgraded = false;
};

// Default protocol: native when the provider/model path supports tools, else XML.
ToolProtocol default_tool_protocol(bool provider_supports_tool_calls);

// Transport retries: only timeout/connect/dns/rate-limit/5xx/malformed SSE.
// Immediate fail (0 retries): 400/401/403/404, auth, context length, bad args.
bool is_immediate_fail_transport_error(const Error& error);
bool is_retryable_transport_error(const Error& error);

// Backoff seconds for attempt index 0,1,2 -> 1,2,4 (caller may add jitter).
int transport_backoff_seconds(int zero_based_attempt);

// Send one tool round with the transport retry budget. Does not re-execute tools.
// Chat mode defaults are unaffected: this is only used by agent/session callers.
Error send_tool_round_with_transport_retries(
    const provider::RequestContext& context,
    const provider::ToolConversation& conversation,
    const std::vector<provider::FunctionDefinition>& tools,
    provider::ToolRoundResult& result,
    runtime::CancellationToken cancellation = runtime::CancellationToken(),
    int transport_attempts = 3,
    const provider::ToolRoundObserver* observer = nullptr,
    const provider::ToolRoundContext& observation_context = provider::ToolRoundContext{},
    std::function<void(const Error& error, int attempt, int backoff_seconds)> on_retry =
        {},
    provider::ReasoningDeltaCallback on_reasoning_delta = {});

// Normalize arguments and tool names; mark invalid args for history hygiene.
std::vector<PreparedToolCall> prepare_tool_calls(
    const std::vector<provider::ToolCall>& calls,
    const std::vector<std::string>& known_tool_names);

// Fingerprint for identical-call loop detection (name + normalized args JSON).
std::string tool_call_fingerprint(const PreparedToolCall& call);

// Rewrite assistant/function_call continuation items so invalid arguments become "{}".
// Mutates round.continuation_items_json. Keeps original args only in PreparedToolCall.
Error sanitize_round_continuation_for_history(provider::ApiKind api_kind,
                                              provider::ToolRoundResult& round,
                                              const std::vector<PreparedToolCall>& prepared);

// Append a role/content message onto continuation_items_json (after tool history).
// Follow-up user turns must use this — not conversation.messages — so they serialize
// after assistant/tool items rather than between the seed goal and tool history.
void append_conversation_text(provider::ToolConversation& conversation,
                              const std::string& role,
                              const std::string& content);
// Request-only context is serialized like a continuation message, but callers
// remove it after the active user turn so it never accumulates in history.
std::size_t append_request_only_context(
    provider::ToolConversation& conversation,
    const std::string& content);
bool remove_request_only_context(provider::ToolConversation& conversation,
                                 std::size_t index);

// Append assistant continuation (already sanitized) then tool results.
void append_prepared_tool_results(const provider::RequestContext& context,
                                  provider::ToolConversation& conversation,
                                  const std::vector<PreparedToolCall>& prepared,
                                  const std::vector<std::string>& result_json);

// On cancel/abort: pair any dangling call ids with a synthetic cancelled result.
void pair_dangling_tool_calls(const provider::RequestContext& context,
                              provider::ToolConversation& conversation,
                              AgentLoopState& state,
                              const std::string& message = "Tool was not executed.");

// Detect leaked <tool_call> markup on the native channel. Two consecutive turns
// downgrade the session to XML and set outcome.protocol_downgraded.
bool content_looks_like_xml_tool_markup(const std::string& text);
void note_native_channel_content(AgentLoopState& state,
                                 const std::string& assistant_text,
                                 std::string* downgrade_notice);

// Soft/hard identical-call tracking. Returns true if hard-aborted.
bool track_identical_calls(AgentLoopState& state,
                           const std::vector<PreparedToolCall>& prepared,
                           const AgentLoopLimits& limits,
                           std::string* soft_notice);

// Soft system/user notice text for repeated identical calls.
std::string identical_call_soft_notice_text();

// Synthetic cancelled / invalid-args tool result bodies.
std::string cancelled_tool_result_json(const std::string& message = "Tool was not executed.");

// Execute one model round: prepare calls, history hygiene, run tools (never auto
// re-run on failure), update loop state, append results.
// executor is not called when arguments are invalid (rich error is returned instead).
using ToolExecutor = std::function<std::string(const std::string& name,
                                               const std::string& arguments_json,
                                               runtime::CancellationToken cancellation)>;

AgentRoundOutcome handle_agent_tool_round(
    AgentLoopState& state,
    const AgentLoopLimits& limits,
    const provider::RequestContext& context,
    provider::ToolConversation& conversation,
    provider::ToolRoundResult round,
    const std::vector<std::string>& known_tool_names,
    ToolExecutor executor,
    runtime::CancellationToken cancellation = runtime::CancellationToken());

// XML channel: parse one tool call from assistant text and run the same path.
AgentRoundOutcome handle_agent_xml_round(
    AgentLoopState& state,
    const AgentLoopLimits& limits,
    const provider::RequestContext& context,
    provider::ToolConversation& conversation,
    const std::string& assistant_text,
    const std::vector<std::string>& known_tool_names,
    ToolExecutor executor,
    runtime::CancellationToken cancellation = runtime::CancellationToken());

}  // namespace ainiux::agent
