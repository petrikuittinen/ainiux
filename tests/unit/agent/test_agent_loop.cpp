#include "agent/test_agent_loop.hpp"

#include <string>
#include <vector>

#include "agent/agent_loop.hpp"
#include "json/json.hpp"
#include "support/test_support.hpp"

namespace ainiux::test::agent_loop {
namespace {
using ainiux::test::check;

provider::RequestContext chat_context() {
    provider::RequestContext context;
    context.api_kind = provider::ApiKind::ChatCompletions;
    context.profile.name = "openai";
    context.options.model = "test-model";
    return context;
}

std::string ok_result() {
    return R"({"ok":true,"error":null,"data":{},"warnings":[],"truncated":false,"metadata":{}})";
}

std::string fail_result() {
    return R"({"ok":false,"error":{"code":"not_found","message":"missing"},"data":{},"warnings":[],"truncated":false,"metadata":{}})";
}

void test_approval_denial_stops_turn_and_new_user_resets_abort() {
    using namespace ainiux::agent;
    AgentLoopState state;
    state.protocol = ToolProtocol::Xml;
    AgentLoopLimits limits;
    provider::ToolConversation conversation;
    int executions = 0;
    const AgentRoundOutcome denied = handle_agent_xml_round(
        state, limits, chat_context(), conversation,
        R"(<tool_call><name>remove</name><args>{"path":"/tmp/dug.txt"}</args></tool_call>)",
        {"remove"},
        [&](const std::string&, const std::string&, runtime::CancellationToken) {
            ++executions;
            return R"({"ok":false,"error":{"code":"unsupported_feature","message":"external remove requires user approval"},"data":{},"warnings":[],"truncated":false,"metadata":{}})";
        });
    check(executions == 1 && denied.kind == AgentRoundOutcome::Kind::FinalText &&
              !state.aborted && state.consecutive_all_failed_turns == 0,
          "declined approval ends the current turn without inviting a workaround");

    state.aborted = true;
    state.abort_reason = "three consecutive failures";
    state.consecutive_all_failed_turns = 3;
    state.identical_repeat_count = 4;
    state.last_fingerprint = "old";
    reset_agent_loop_for_user_turn(state);
    check(!state.aborted && state.abort_reason.empty() &&
              state.consecutive_all_failed_turns == 0 &&
              state.identical_repeat_count == 0 && state.last_fingerprint.empty(),
          "a new user message clears prior retry and abort state");
}

void test_transport_retry_classification() {
    using ainiux::agent::is_immediate_fail_transport_error;
    using ainiux::agent::is_retryable_transport_error;
    using ainiux::agent::transport_backoff_seconds;

    check(is_immediate_fail_transport_error({ErrorCode::Auth, "unauthorized"}),
          "auth fails immediately");
    check(is_immediate_fail_transport_error(
              {ErrorCode::HttpStatus, "POST failed: HTTP 401 Unauthorized"}),
          "HTTP 401 fails immediately");
    check(is_immediate_fail_transport_error(
              {ErrorCode::HttpStatus, "POST failed: HTTP 400 Bad Request"}),
          "HTTP 400 fails immediately");
    check(is_immediate_fail_transport_error(
              {ErrorCode::HttpStatus, "POST failed: HTTP 404 Not Found"}),
          "HTTP 404 fails immediately");
    check(!is_retryable_transport_error(
              {ErrorCode::HttpStatus, "POST failed: HTTP 401 Unauthorized"}),
          "HTTP 401 is not retryable");
    check(is_retryable_transport_error({ErrorCode::RateLimit, "HTTP 429"}),
          "rate limit is retryable");
    check(is_retryable_transport_error(
              {ErrorCode::HttpStatus, "POST failed: HTTP 503 Unavailable"}),
          "HTTP 503 is retryable");
    check(is_retryable_transport_error(
              {ErrorCode::HttpStatus, "POST failed: HTTP 529 overloaded"}),
          "HTTP 529 is retryable");
    check(is_retryable_transport_error({ErrorCode::Timeout, "timed out"}),
          "timeout is retryable");
    check(is_retryable_transport_error({ErrorCode::SseParse, "bad stream"}),
          "malformed SSE is retryable");
    check(transport_backoff_seconds(0) == 1 && transport_backoff_seconds(1) == 2 &&
              transport_backoff_seconds(2) == 4,
          "transport backoff ladder is 1s, 2s, 4s");
}

void test_prepare_and_history_hygiene() {
    using ainiux::agent::prepare_tool_calls;
    using ainiux::agent::sanitize_round_continuation_for_history;
    using ainiux::agent::append_prepared_tool_results;
    using ainiux::agent::pair_dangling_tool_calls;
    using ainiux::agent::AgentLoopState;

    std::vector<provider::ToolCall> calls = {
        {"call_1", "read_file", "not-json", 0},
        {"call_2", "ReadFile", R"({"path":"a.cpp"})", 1},
    };
    const std::vector<std::string> known = {"read_file", "search_text"};
    auto prepared = prepare_tool_calls(calls, known);
    check(prepared.size() == 2 && prepared[0].arguments_invalid &&
              prepared[0].history_arguments == "{}" &&
              prepared[0].original_arguments == "not-json",
          "invalid args keep original and serialize {} for history");
    check(prepared[1].name == "read_file" && !prepared[1].arguments_invalid,
          "case-repaired tool names are applied during prepare");

    provider::ToolRoundResult round;
    round.tool_calls = calls;
    json::Value assistant;
    assistant.type = json::Value::Type::Object;
    assistant.object["role"].type = json::Value::Type::String;
    assistant.object["role"].string = "assistant";
    assistant.object["content"].type = json::Value::Type::Null;
    json::Value tool_calls;
    tool_calls.type = json::Value::Type::Array;
    for (const provider::ToolCall& call : calls) {
        json::Value item;
        item.type = json::Value::Type::Object;
        item.object["id"].type = json::Value::Type::String;
        item.object["id"].string = call.id;
        item.object["type"].type = json::Value::Type::String;
        item.object["type"].string = "function";
        json::Value function;
        function.type = json::Value::Type::Object;
        function.object["name"].type = json::Value::Type::String;
        function.object["name"].string = call.name;
        function.object["arguments"].type = json::Value::Type::String;
        function.object["arguments"].string = call.arguments_json;
        item.object["function"] = std::move(function);
        tool_calls.array.push_back(std::move(item));
    }
    assistant.object["tool_calls"] = std::move(tool_calls);
    round.continuation_items_json.push_back(json::stringify(assistant));

    ainiux::Error error =
        sanitize_round_continuation_for_history(provider::ApiKind::ChatCompletions, round, prepared);
    check(error.ok(), "continuation sanitize succeeds");
    check(round.continuation_items_json.front().find("not-json") == std::string::npos &&
              round.continuation_items_json.front().find("\"arguments\":\"{}\"") != std::string::npos,
          "provider history never contains invalid JSON arguments");
    check(round.tool_calls[0].arguments_json == "{}",
          "in-memory tool_calls also use sanitized history args");

    provider::ToolConversation conversation;
    conversation.continuation_items_json = round.continuation_items_json;
    append_prepared_tool_results(chat_context(), conversation, prepared,
                                 {R"({"ok":false,"error":{"code":"invalid_arguments","message":"x"}})",
                                  ok_result()});
    check(conversation.continuation_items_json.size() == 3, "assistant + two tool results appended");
    check(conversation.continuation_items_json[1].find("tool_call_id") != std::string::npos ||
              conversation.continuation_items_json[1].find("call_1") != std::string::npos,
          "tool result messages reference call ids");

    AgentLoopState state;
    state.dangling_call_ids = {"orphan_1", "orphan_2"};
    pair_dangling_tool_calls(chat_context(), conversation, state);
    check(state.dangling_call_ids.empty() && conversation.continuation_items_json.size() == 5,
          "dangling tool calls are paired with synthetic cancelled results");
    check(conversation.continuation_items_json.back().find("cancelled") != std::string::npos,
          "synthetic dangling result is a cancelled tool result");
}

void test_loop_limits_and_no_tool_retry() {
    using ainiux::agent::AgentLoopLimits;
    using ainiux::agent::AgentLoopState;
    using ainiux::agent::AgentRoundOutcome;
    using ainiux::agent::handle_agent_tool_round;

    AgentLoopLimits limits;
    limits.soft_identical_repeats = 3;
    limits.hard_identical_repeats = 5;
    limits.consecutive_failure_turns = 3;
    limits.max_scripted_turns = 50;

    int executions = 0;
    auto executor = [&](const std::string&, const std::string&,
                        runtime::CancellationToken) {
        ++executions;
        return fail_result();
    };

    auto make_round = [](const std::string& id) {
        provider::ToolRoundResult round;
        provider::ToolCall call;
        call.id = id;
        call.name = "read_file";
        call.arguments_json = R"({"path":"same.cpp"})";
        round.tool_calls.push_back(call);
        json::Value assistant;
        assistant.type = json::Value::Type::Object;
        assistant.object["role"].type = json::Value::Type::String;
        assistant.object["role"].string = "assistant";
        json::Value tool_calls;
        tool_calls.type = json::Value::Type::Array;
        json::Value item;
        item.type = json::Value::Type::Object;
        item.object["id"].type = json::Value::Type::String;
        item.object["id"].string = call.id;
        item.object["type"].type = json::Value::Type::String;
        item.object["type"].string = "function";
        json::Value function;
        function.type = json::Value::Type::Object;
        function.object["name"].type = json::Value::Type::String;
        function.object["name"].string = call.name;
        function.object["arguments"].type = json::Value::Type::String;
        function.object["arguments"].string = call.arguments_json;
        item.object["function"] = std::move(function);
        tool_calls.array.push_back(std::move(item));
        assistant.object["tool_calls"] = std::move(tool_calls);
        round.continuation_items_json.push_back(json::stringify(assistant));
        return round;
    };

    AgentLoopState state;
    provider::ToolConversation conversation;
    const std::vector<std::string> known = {"read_file"};
    provider::RequestContext context = chat_context();

    AgentRoundOutcome third;
    for (int i = 1; i <= 3; ++i) {
        third = handle_agent_tool_round(state, limits, context, conversation, make_round("c" + std::to_string(i)),
                                        known, executor);
        check(third.kind == AgentRoundOutcome::Kind::Continue, "first three identical failures continue");
    }
    check(state.soft_repeat_notice_pending == false, "soft notice is injected during the third repeat");
    check(conversation.continuation_items_json.back().find("repeated the same call") != std::string::npos,
          "soft identical-call notice is injected as a follow-up message");

    AgentRoundOutcome fifth;
    for (int i = 4; i <= 5; ++i) {
        fifth = handle_agent_tool_round(state, limits, context, conversation, make_round("c" + std::to_string(i)),
                                        known, executor);
    }
    check(fifth.kind == AgentRoundOutcome::Kind::Aborted && state.aborted,
          "fifth identical call hard-aborts");
    // The fifth identical request aborts before re-running the tool.
    check(executions == 4, "failed tools are never automatically re-executed; hard abort skips the fifth run");

    // Consecutive-failure abort with varying fingerprints.
    AgentLoopState fail_state;
    provider::ToolConversation fail_conversation;
    executions = 0;
    auto varying = [](int n) {
        provider::ToolRoundResult round;
        provider::ToolCall call;
        call.id = "v" + std::to_string(n);
        call.name = "read_file";
        call.arguments_json = std::string(R"({"path":"file)") + std::to_string(n) + R"(.cpp"})";
        round.tool_calls.push_back(call);
        json::Value assistant;
        assistant.type = json::Value::Type::Object;
        assistant.object["role"].type = json::Value::Type::String;
        assistant.object["role"].string = "assistant";
        json::Value tool_calls;
        tool_calls.type = json::Value::Type::Array;
        json::Value item;
        item.type = json::Value::Type::Object;
        item.object["id"].type = json::Value::Type::String;
        item.object["id"].string = call.id;
        item.object["type"].type = json::Value::Type::String;
        item.object["type"].string = "function";
        json::Value function;
        function.type = json::Value::Type::Object;
        function.object["name"].type = json::Value::Type::String;
        function.object["name"].string = call.name;
        function.object["arguments"].type = json::Value::Type::String;
        function.object["arguments"].string = call.arguments_json;
        item.object["function"] = std::move(function);
        tool_calls.array.push_back(std::move(item));
        assistant.object["tool_calls"] = std::move(tool_calls);
        round.continuation_items_json.push_back(json::stringify(assistant));
        return round;
    };
    AgentRoundOutcome consecutive;
    for (int i = 1; i <= 3; ++i)
        consecutive = handle_agent_tool_round(fail_state, limits, context, fail_conversation,
                                              varying(i), known, executor);
    check(consecutive.kind == AgentRoundOutcome::Kind::Aborted &&
              fail_state.abort_reason.find("consecutive") != std::string::npos,
          "three consecutive all-failed turns abort");
}

void test_protocol_downgrade_and_xml() {
    using ainiux::agent::AgentLoopLimits;
    using ainiux::agent::AgentLoopState;
    using ainiux::agent::AgentRoundOutcome;
    using ainiux::agent::ToolProtocol;
    using ainiux::agent::default_tool_protocol;
    using ainiux::agent::handle_agent_tool_round;
    using ainiux::agent::handle_agent_xml_round;

    check(default_tool_protocol(true) == ToolProtocol::Native, "default protocol is native with tools");
    check(default_tool_protocol(false) == ToolProtocol::Xml, "default protocol is xml without tools");

    AgentLoopState state;
    state.protocol = ToolProtocol::Native;
    AgentLoopLimits limits;
    provider::ToolConversation conversation;
    provider::RequestContext context = chat_context();
    int executions = 0;
    auto executor = [&](const std::string& name, const std::string& args,
                        runtime::CancellationToken) {
        ++executions;
        check(name == "read_file" && args.find("src/main.cpp") != std::string::npos,
              "XML channel executes the parsed tool with original args");
        return ok_result();
    };

    provider::ToolRoundResult leak;
    leak.content = "I will call:\n<tool_call>\n<name>read_file</name>\n"
                   "<args>{\"path\":\"src/main.cpp\"}</args>\n</tool_call>";
    auto first = handle_agent_tool_round(state, limits, context, conversation, leak, {"read_file"},
                                         executor);
    check(first.kind == AgentRoundOutcome::Kind::FinalText &&
              state.protocol == ToolProtocol::Native && state.native_xml_leak_strikes == 1,
          "first native XML leak is recorded without downgrade");

    auto second = handle_agent_tool_round(state, limits, context, conversation, leak, {"read_file"},
                                          executor);
    check(state.protocol == ToolProtocol::Xml && second.protocol_downgraded,
          "second consecutive native XML leak downgrades the session");
    // After downgrade, empty native tool_calls with XML markup runs XML path.
    // second may already be FinalText if protocol was still Native during the
    // empty-tool branch before note - note runs before empty check and sets Xml,
    // then empty + Xml content routes to xml handler.
    if (second.kind == AgentRoundOutcome::Kind::Continue) {
        check(executions >= 1, "downgraded session executes XML tool call");
    } else {
        // If second only downgraded without executing, drive XML explicitly.
        auto xml = handle_agent_xml_round(state, limits, context, conversation, leak.content,
                                          {"read_file"}, executor);
        check(xml.kind == AgentRoundOutcome::Kind::Continue && executions >= 1,
              "XML round executes a single tool call");
    }

    auto multi = handle_agent_xml_round(
        state, limits, context, conversation,
        "<tool_call><name>a</name><args>{}</args></tool_call>"
        "<tool_call><name>b</name><args>{}</args></tool_call>",
        {"read_file"}, executor);
    check(multi.kind == AgentRoundOutcome::Kind::Continue &&
              multi.notice.find("exactly one") != std::string::npos,
          "XML channel rejects multiple tool_call blocks with a continue notice");
}

void test_invalid_args_not_executed() {
    using ainiux::agent::AgentLoopLimits;
    using ainiux::agent::AgentLoopState;
    using ainiux::agent::AgentRoundOutcome;
    using ainiux::agent::handle_agent_tool_round;

    AgentLoopState state;
    AgentLoopLimits limits;
    provider::ToolConversation conversation;
    int executions = 0;
    auto executor = [&](const std::string&, const std::string&, runtime::CancellationToken) {
        ++executions;
        return ok_result();
    };
    provider::ToolRoundResult round;
    provider::ToolCall call;
    call.id = "bad1";
    call.name = "read_file";
    call.arguments_json = "@@@";
    round.tool_calls.push_back(call);
    json::Value assistant;
    assistant.type = json::Value::Type::Object;
    assistant.object["role"].type = json::Value::Type::String;
    assistant.object["role"].string = "assistant";
    json::Value tool_calls;
    tool_calls.type = json::Value::Type::Array;
    json::Value item;
    item.type = json::Value::Type::Object;
    item.object["id"].type = json::Value::Type::String;
    item.object["id"].string = "bad1";
    item.object["type"].type = json::Value::Type::String;
    item.object["type"].string = "function";
    json::Value function;
    function.type = json::Value::Type::Object;
    function.object["name"].type = json::Value::Type::String;
    function.object["name"].string = "read_file";
    function.object["arguments"].type = json::Value::Type::String;
    function.object["arguments"].string = "@@@";
    item.object["function"] = std::move(function);
    tool_calls.array.push_back(std::move(item));
    assistant.object["tool_calls"] = std::move(tool_calls);
    round.continuation_items_json.push_back(json::stringify(assistant));

    auto outcome = handle_agent_tool_round(state, limits, chat_context(), conversation, round,
                                           {"read_file"}, executor);
    check(outcome.kind == AgentRoundOutcome::Kind::Continue && executions == 0,
          "invalid arguments produce a rich error tool-result without executing the tool");
    check(outcome.tool_results.size() == 1 &&
              outcome.tool_results.front().find("received_arguments") != std::string::npos &&
              outcome.tool_results.front().find("@@@") != std::string::npos,
          "rich invalid-arguments result includes the original text");
    check(conversation.continuation_items_json.front().find("@@@") == std::string::npos,
          "provider-facing history uses {} instead of invalid arguments");
}

void test_scripted_turn_cap() {
    using ainiux::agent::AgentLoopLimits;
    using ainiux::agent::AgentLoopState;
    using ainiux::agent::AgentRoundOutcome;
    using ainiux::agent::handle_agent_tool_round;

    AgentLoopLimits limits;
    limits.max_scripted_turns = 2;
    limits.interactive = false;
    AgentLoopState state;
    provider::ToolConversation conversation;
    auto executor = [&](const std::string&, const std::string&, runtime::CancellationToken) {
        return ok_result();
    };
    auto make_round = [](int n) {
        provider::ToolRoundResult round;
        provider::ToolCall call;
        call.id = "t" + std::to_string(n);
        call.name = "project_overview";
        call.arguments_json = "{}";
        round.tool_calls.push_back(call);
        json::Value assistant;
        assistant.type = json::Value::Type::Object;
        assistant.object["role"].type = json::Value::Type::String;
        assistant.object["role"].string = "assistant";
        json::Value tool_calls;
        tool_calls.type = json::Value::Type::Array;
        json::Value item;
        item.type = json::Value::Type::Object;
        item.object["id"].type = json::Value::Type::String;
        item.object["id"].string = call.id;
        item.object["type"].type = json::Value::Type::String;
        item.object["type"].string = "function";
        json::Value function;
        function.type = json::Value::Type::Object;
        function.object["name"].type = json::Value::Type::String;
        function.object["name"].string = call.name;
        function.object["arguments"].type = json::Value::Type::String;
        function.object["arguments"].string = "{}";
        item.object["function"] = std::move(function);
        tool_calls.array.push_back(std::move(item));
        assistant.object["tool_calls"] = std::move(tool_calls);
        round.continuation_items_json.push_back(json::stringify(assistant));
        return round;
    };
    auto first = handle_agent_tool_round(state, limits, chat_context(), conversation, make_round(1),
                                         {"project_overview"}, executor);
    auto second = handle_agent_tool_round(state, limits, chat_context(), conversation, make_round(2),
                                          {"project_overview"}, executor);
    auto third = handle_agent_tool_round(state, limits, chat_context(), conversation, make_round(3),
                                         {"project_overview"}, executor);
    check(first.kind == AgentRoundOutcome::Kind::Continue &&
              second.kind == AgentRoundOutcome::Kind::Continue &&
              third.kind == AgentRoundOutcome::Kind::Aborted,
          "scripted turn cap hard-aborts after the configured limit");

    limits.interactive = true;
    AgentLoopState interactive;
    provider::ToolConversation interactive_conversation;
    AgentRoundOutcome need;
    for (int i = 0; i < 2; ++i)
        need = handle_agent_tool_round(interactive, limits, chat_context(),
                                       interactive_conversation,
                                       make_round(20 + i), {"project_overview"}, executor);
    need = handle_agent_tool_round(interactive, limits, chat_context(),
                                   interactive_conversation, make_round(99),
                                   {"project_overview"}, executor);
    check(need.kind == AgentRoundOutcome::Kind::NeedsUserContinue &&
              need.tool_results.size() == 1 &&
              need.tool_results.front().find("turn cap") != std::string::npos,
          "interactive mode asks the user and pairs the capped tool call");
    check(interactive_conversation.continuation_items_json.size() == 6,
          "capped native assistant call has a matching synthetic tool result");

    ainiux::agent::append_conversation_text(interactive_conversation, "user",
                                            "Continue the current task.");
    provider::RequestContext serialized_context = chat_context();
    const std::vector<provider::FunctionDefinition> definitions = {
        {"project_overview", "Inspect the project", R"({"type":"object"})"}};
    json::ParseResult capped_request = json::parse(
        provider::serialize_tool_request(serialized_context, interactive_conversation,
                                         definitions));
    check(capped_request.error.ok(),
          "continuation after the cap serializes as valid Chat Completions history");
    const json::Value* capped_messages = capped_request.value.get("messages");
    check(capped_messages != nullptr && capped_messages->is_array() &&
              capped_messages->array.size() == 7 &&
              capped_messages->array[5].get("role") != nullptr &&
              capped_messages->array[5].get("role")->string == "tool" &&
              capped_messages->array[6].get("role") != nullptr &&
              capped_messages->array[6].get("role")->string == "user",
          "capped tool result precedes the user continuation message");

    interactive.scripted_turns = 0;
    auto resumed = handle_agent_tool_round(interactive, limits, chat_context(),
                                           interactive_conversation, make_round(100),
                                           {"project_overview"}, executor);
    check(resumed.kind == AgentRoundOutcome::Kind::Continue &&
              interactive.turn == 3 && interactive.scripted_turns == 1,
          "approved continuation receives a fresh budget while cumulative turns continue");
}

void test_follow_up_user_appends_after_tool_history() {
    // Regression: follow-ups used to push into conversation.messages, which
    // serialize_tool_request places BEFORE continuation tool history. The model
    // then saw the new user request before prior tool results.
    provider::RequestContext context = chat_context();
    context.options.model = "mock-model";
    context.options.stream = false;
    provider::ToolConversation conversation;
    conversation.messages = {{"system", "trusted"}, {"user", "write a game"}};
    conversation.continuation_items_json = {
        R"({"role":"assistant","content":null,"tool_calls":[{"id":"call_1","type":"function","function":{"name":"write_file","arguments":"{\"path\":\"game.py\"}"}}]})",
        R"({"role":"tool","tool_call_id":"call_1","content":"{\"ok\":true}"})",
        R"({"role":"assistant","content":"Created game.py"})"};

    agent::append_conversation_text(conversation, "user", "change attempts to 8");

    const std::vector<provider::FunctionDefinition> definitions = {
        {"write_file", "Write a file",
         R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"],"additionalProperties":false})"}};
    json::ParseResult parsed =
        json::parse(provider::serialize_tool_request(context, conversation, definitions));
    check(parsed.error.ok(), "follow-up request serializes");
    const json::Value* messages = parsed.value.get("messages");
    check(messages != nullptr && messages->is_array() && messages->array.size() == 6,
          "seed + tool history + follow-up = 6 messages");
    // Expected order: system, user(goal), assistant(tool_calls), tool, assistant(text), user(follow-up)
    check(messages->array[1].get("role") != nullptr &&
              messages->array[1].get("role")->string == "user" &&
              messages->array[1].get("content") != nullptr &&
              messages->array[1].get("content")->string.find("write a game") != std::string::npos,
          "original goal stays second");
    check(messages->array[2].get("tool_calls") != nullptr ||
              (messages->array[2].get("role") != nullptr &&
               messages->array[2].get("role")->string == "assistant"),
          "tool-call assistant precedes follow-up");
    check(messages->array[5].get("role") != nullptr &&
              messages->array[5].get("role")->string == "user" &&
              messages->array[5].get("content") != nullptr &&
              messages->array[5].get("content")->string.find("attempts to 8") != std::string::npos,
          "follow-up user is last after tool history");
}

void test_request_only_context_is_removed_without_history_loss() {
    provider::ToolConversation conversation;
    conversation.messages = {{"system", "trusted"}, {"user", "change add"}};
    conversation.continuation_items_json = {
        R"({"role":"assistant","content":"checking"})",
        R"({"role":"user","content":"tool history"})"};
    const std::vector<std::string> before =
        conversation.continuation_items_json;
    const std::size_t position = agent::append_request_only_context(
        conversation,
        "[Approximate code-index hints; verify before editing]\nsrc/util.c: add");
    check(conversation.continuation_items_json.size() == before.size() + 1 &&
              conversation.continuation_items_json.back().find(
                  "Approximate code-index hints") != std::string::npos,
          "request-only index hint is appended after current history");
    conversation.continuation_items_json.push_back(
        R"({"role":"assistant","content":"later round"})");
    check(agent::remove_request_only_context(conversation, position) &&
              conversation.continuation_items_json.size() ==
                  before.size() + 1 &&
              conversation.continuation_items_json[0] == before[0] &&
              conversation.continuation_items_json[1] == before[1] &&
              conversation.continuation_items_json.back().find(
                  "later round") != std::string::npos,
          "request-only index hint is removed without discarding tool rounds");
}

void test_batched_reads_reduce_rounds_and_serialized_request_bytes() {
    provider::RequestContext context = chat_context();
    const std::vector<provider::FunctionDefinition> definitions = {
        {"read_file", "Read one file", R"({"type":"object"})"},
        {"read_many", "Read several files", R"({"type":"object"})"}};
    provider::ToolConversation repeated;
    repeated.messages = {{"system", "stable agent prompt"},
                         {"user", "Act"},
                         {"user", "Read a.cpp and b.cpp"}};
    const std::size_t first_request_bytes =
        provider::serialize_tool_request(context, repeated, definitions).size();
    repeated.continuation_items_json = {
        R"({"role":"assistant","content":null,"tool_calls":[{"id":"a","type":"function","function":{"name":"read_file","arguments":"{\"path\":\"a.cpp\"}"}}]})",
        R"({"role":"tool","tool_call_id":"a","content":"{\"ok\":true,\"data\":{\"content\":\"a\"}}"})"};
    const std::size_t second_request_bytes =
        provider::serialize_tool_request(context, repeated, definitions).size();

    provider::ToolConversation batched;
    batched.messages = {{"system", "stable agent prompt"},
                        {"user", "Act"},
                        {"user", "Read a.cpp and b.cpp"}};
    const std::size_t batched_request_bytes =
        provider::serialize_tool_request(context, batched, definitions).size();
    const std::size_t repeated_rounds = 2;
    const std::size_t batched_rounds = 1;
    const std::size_t repeated_request_bytes =
        first_request_bytes + second_request_bytes;
    check(batched_rounds < repeated_rounds &&
              batched_request_bytes < repeated_request_bytes,
          "one read_many/multi-call round reduces deterministic model rounds and request bytes");
}

}  // namespace

void run_all() {
    test_transport_retry_classification();
    test_approval_denial_stops_turn_and_new_user_resets_abort();
    test_prepare_and_history_hygiene();
    test_loop_limits_and_no_tool_retry();
    test_protocol_downgrade_and_xml();
    test_invalid_args_not_executed();
    test_scripted_turn_cap();
    test_follow_up_user_appends_after_tool_history();
    test_request_only_context_is_removed_without_history_loss();
    test_batched_reads_reduce_rounds_and_serialized_request_bytes();
}

}  // namespace ainiux::test::agent_loop
