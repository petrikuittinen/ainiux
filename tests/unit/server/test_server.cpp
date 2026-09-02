#include "server/test_server.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>

#include "cli/args.hpp"
#include "json/json.hpp"
#include "server/auth.hpp"
#include "server/chat_service.hpp"
#include "server/http_parser.hpp"
#include "server/event_broker.hpp"
#include "server/job_registry.hpp"
#include "server/job_service.hpp"
#include "server/listener.hpp"
#include "server/mcp_adapter.hpp"
#include "server/router.hpp"
#include "server/session_hub.hpp"
#include "server/server.hpp"
#include "server/tls.hpp"
#include "server/workspace_service.hpp"
#include "support/test_support.hpp"

namespace ainiux::test::server_control {
namespace {

using ainiux::test::check;
using namespace ainiux::server;
namespace http = ainiux::server::http;

std::string request_text(const std::string& path = "/ainiux/v1/health",
                         const std::string& token = "controller") {
    return "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " +
           token + "\r\n\r\n";
}

http::Request parsed_request(const std::string& text) {
    http::Parser parser;
    check(parser.feed(text) == http::ParseState::Complete, "server test request parses");
    return parser.request();
}

bool wait_terminal(const std::shared_ptr<Job>& job) {
    for (int i = 0; i < 200; ++i) {
        if (job->terminal()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return job->terminal();
}

void test_fragmented_parser_and_pipeline() {
    const std::string first = request_text();
    const std::string second = request_text("/ainiux/v1/status");
    http::Parser parser;
    for (std::size_t i = 0; i + 1 < first.size(); ++i) {
        check(parser.feed(std::string_view(first.data() + i, 1)) == http::ParseState::NeedMore,
              "fragmented HTTP request waits for all bytes");
    }
    check(parser.feed(std::string_view(first.data() + first.size() - 1, 1)) ==
              http::ParseState::Complete,
          "fragmented HTTP request completes on final byte");
    check(parser.request().path == "/ainiux/v1/health" && parser.request().body.empty(),
          "strict parser preserves normalized path and empty body");

    http::Parser pipelined;
    check(pipelined.feed(first + second) == http::ParseState::Complete,
          "parser completes the first pipelined request");
    check(pipelined.take_remaining() == second,
          "parser returns unconsumed pipelined bytes exactly");
}

void expect_parse_failure(const std::string& text, int status, const char* label) {
    http::Parser parser;
    check(parser.feed(text) == http::ParseState::Failed && parser.error().status == status, label);
}

void test_strict_framing_and_limits() {
    expect_parse_failure("GET /ainiux/v1/health HTTP/1.1\nHost: localhost\n\n", 400,
                         "bare-LF HTTP is rejected");
    expect_parse_failure("GET /ainiux/v1/%2e%2e/x HTTP/1.1\r\nHost: localhost\r\n\r\n", 400,
                         "encoded ambiguous path is rejected");
    expect_parse_failure("GET /../x HTTP/1.1\r\nHost: localhost\r\n\r\n", 400,
                         "path traversal is rejected");
    expect_parse_failure("POST /ainiux/v1/x HTTP/1.1\r\nHost: localhost\r\n"
                         "Content-Length: 0\r\nContent-Length: 0\r\n\r\n", 400,
                         "duplicate Content-Length is rejected");
    expect_parse_failure("POST /ainiux/v1/x HTTP/1.1\r\nHost: localhost\r\n"
                         "Transfer-Encoding: chunked\r\n\r\n", 501,
                         "transfer encoding is rejected");
    expect_parse_failure("GET / HTTP/1.1\r\nHost: localhost\r\n folded\r\n\r\n", 400,
                         "obsolete folded headers are rejected");
    expect_parse_failure("GET / HTTP/1.1\r\n\r\n", 400,
                         "missing HTTP/1.1 Host is rejected");

    http::Parser small(3);
    check(small.feed("POST /x HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\n\r\n") ==
              http::ParseState::Failed && small.error().status == 413,
          "route body limit is enforced before buffering the body");

    std::string headers = "GET / HTTP/1.1\r\nHost: localhost\r\n";
    for (int i = 0; i < 100; ++i) headers += "X-" + std::to_string(i) + ": y\r\n";
    headers += "\r\n";
    expect_parse_failure(headers, 431, "header count includes Host and is bounded at 100");
}

void test_auth_and_routes() {
    check(constant_time_equal("same", "same") && !constant_time_equal("same", "samf") &&
              !constant_time_equal("same", "same-longer"),
          "constant-time credential comparison handles equality and unequal lengths");
    AuthConfig config{"controller", "mcp-token"};
    http::Request full = parsed_request(request_text());
    check(authenticate(full, config) == AuthScope::FullControl,
          "full-control credential authenticates control route");
    http::Request wrong_scope = parsed_request(request_text("/ainiux/v1/status", "mcp-token"));
    check(authenticate(wrong_scope, config) == AuthScope::None,
          "MCP-only credential cannot authenticate control API");
    http::Request mcp = parsed_request(request_text("/mcp", "mcp-token"));
    check(authenticate(mcp, config) == AuthScope::McpOnly,
          "MCP-only credential authenticates only the MCP path");
    http::Request full_on_mcp = parsed_request(request_text("/mcp", "controller"));
    check(authenticate(full_on_mcp, config) == AuthScope::None,
          "full-control credential is not reused as an MCP credential");

    std::atomic<std::size_t> active{2};
    PublicStatus status{8766, 64, 128, &active};
    Response health = route_request(full, config, status);
    check(health.status == 200 && health.body == "{\"status\":\"ok\"}",
          "authenticated health is minimal");
    Response denied = route_request(wrong_scope, config, status);
    check(denied.status == 401 && denied.body.find("mcp-token") == std::string::npos,
          "wrong-scope response is unauthorized and does not expose credentials");
    http::Request status_request = parsed_request(request_text("/ainiux/v1/status"));
    Response status_response = route_request(status_request, config, status);
    check(status_response.status == 200 &&
              status_response.body.find("\"active\":2") != std::string::npos &&
              status_response.body.find("127.0.0.1") != std::string::npos,
          "status reports bounded public listener state");
    http::Request capabilities = parsed_request(request_text("/ainiux/v1/capabilities"));
    Response capability_response = route_request(capabilities, config, status);
    check(capability_response.status == 200 &&
              capability_response.body.find("\"mcp\":true") != std::string::npos &&
              capability_response.body.find("controller") == std::string::npos,
          "capabilities advertise the MCP adapter without exposing a secret");

    http::Request bad_host = full;
    bad_host.headers["host"] = "example.com";
    check(route_request(bad_host, config, status).status == 421,
          "non-loopback Host is rejected");
    http::Request bad_origin = full;
    bad_origin.headers["origin"] = "https://example.com";
    check(route_request(bad_origin, config, status).status == 403,
          "cross-origin request is rejected");

    PublicStatus remote_status = status;
    remote_status.bind_address = "192.0.2.10";
    remote_status.remote = true;
    remote_status.tls = true;
    http::Request remote = full;
    remote.headers["host"] = "192.0.2.10:8766";
    remote.headers["origin"] = "https://192.0.2.10:8766";
    check(route_request(remote, config, remote_status).status == 200,
          "remote TLS requests accept only the configured host and same HTTPS origin");
    remote.headers["origin"] = "http://192.0.2.10:8766";
    check(route_request(remote, config, remote_status).status == 403,
          "remote TLS requests reject a downgraded Origin");
    remote.headers["origin"] = "https://192.0.2.10:8766";
    remote.headers["host"] = "192.0.2.10:9999";
    check(route_request(remote, config, remote_status).status == 421,
          "remote requests reject a Host with the wrong listener port");
}

void test_event_replay_is_ordered_and_bounded() {
    EventBroker broker("job_events", 3, 4096);
    for (int i = 0; i < 5; ++i) {
        broker.publish("progress", "{\"step\":" + std::to_string(i) + "}");
    }
    const ReplayBatch expired = broker.replay_after(1);
    check(expired.expired, "event replay explicitly expires an evicted cursor");
    const ReplayBatch retained = broker.replay_after(2);
    check(!retained.expired && retained.events.size() == 3 &&
              retained.events[0].id == 3 && retained.events[2].id == 5,
          "event replay remains ordered by monotonic ID inside the bounded window");
    broker.close();
    check(broker.wait_after(5, std::chrono::milliseconds(1)).closed,
          "closing an event broker wakes subscribers");
}

void test_job_registry_idempotency_lane_and_cancellation() {
    JobRegistry registry(8, 2);
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool release = false;
    JobWork blocked = [&](runtime::CancellationToken token, JobEvents) {
        std::unique_lock<std::mutex> lock(gate_mutex);
        gate_cv.wait_for(lock, std::chrono::seconds(1), [&] { return release || token.cancelled(); });
        return JobOutcome{token.cancelled() ? Error{ErrorCode::Cancelled, "cancelled"}
                                            : ok_error(),
                          "{\"ok\":true}"};
    };
    SubmitResult first = registry.submit("run", "{\"goal\":\"one\"}", "same-key",
                                         JobClass::Agent, blocked);
    SubmitResult replay = registry.submit("run", "{\"goal\":\"one\"}", "same-key",
                                          JobClass::Agent, blocked);
    SubmitResult changed = registry.submit("run", "{\"goal\":\"two\"}", "same-key",
                                           JobClass::Agent, blocked);
    SubmitResult lane = registry.submit("plan", "{\"goal\":\"plan\"}", "",
                                        JobClass::Agent, blocked);
    check(first.status == SubmitStatus::Created && replay.status == SubmitStatus::Existing &&
              replay.job == first.job,
          "same idempotency operation and payload return the original job");
    check(changed.status == SubmitStatus::IdempotencyConflict,
          "an idempotency key reused with different input is a typed conflict");
    check(lane.status == SubmitStatus::AgentConflict &&
              lane.conflicting_job_id == first.job->id,
          "run and plan share one non-queued workspace agent lane");
    std::shared_ptr<Job> cancelled;
    check(registry.cancel(first.job->id, cancelled) && registry.cancel(first.job->id, cancelled),
          "job cancellation is idempotent");
    gate_cv.notify_all();
    check(wait_terminal(first.job) && first.job->state() == wire::JobState::Cancelled,
          "cancellation reaches a terminal cancelled state");
    registry.shutdown();
}

void test_provider_job_concurrency_cap() {
    JobRegistry registry(12, 2);
    std::atomic<int> active{0};
    std::atomic<int> maximum{0};
    std::atomic<bool> release{false};
    std::vector<std::shared_ptr<Job>> jobs;
    for (int i = 0; i < 6; ++i) {
        JobWork work = [&](runtime::CancellationToken token, JobEvents) {
            const int now = active.fetch_add(1) + 1;
            int seen = maximum.load();
            while (now > seen && !maximum.compare_exchange_weak(seen, now)) {}
            while (!release.load() && !token.cancelled()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            active.fetch_sub(1);
            return JobOutcome{token.cancelled() ? Error{ErrorCode::Cancelled, "cancelled"}
                                                : ok_error(), "{}"};
        };
        SubmitResult submitted = registry.submit("chat", "{\"n\":" + std::to_string(i) + "}",
                                                 "", JobClass::Provider, work);
        jobs.push_back(submitted.job);
    }
    for (int i = 0; i < 100 && maximum.load() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(maximum.load() == 2, "provider chat/image work obeys the global concurrency cap");
    release.store(true);
    for (const auto& job : jobs) check(wait_terminal(job), "provider job reaches terminal state");
    registry.shutdown();
}

void test_terminal_retention_eviction_releases_workers_safely() {
    JobRegistry registry(1, 1);
    for (int i = 0; i < 6; ++i) {
        SubmitResult submitted = registry.submit(
            "chat", "{\"iteration\":" + std::to_string(i) + "}", "",
            JobClass::Provider,
            [](runtime::CancellationToken, JobEvents) {
                return JobOutcome{ok_error(), "{\"done\":true}"};
            });
        check(submitted.status == SubmitStatus::Created && wait_terminal(submitted.job),
              "terminal retention evicts and joins prior workers safely");
    }
    check(registry.size() == 1, "terminal job retention stays within --max-jobs");
    registry.shutdown();
}

void test_job_routes_and_sse() {
    cli::Options options;
    options.provider = "none";
    JobService jobs(options, ".", 8);
    AuthConfig auth{"controller", {}};
    std::atomic<std::size_t> active{1};
    PublicStatus status{8766, 64, 8, &active, &jobs};
    const std::string body = "{\"provider\":\"none\",\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}";
    const std::string prefix = "POST /ainiux/v1/jobs/chat HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                               "Authorization: Bearer controller\r\nContent-Type: application/json\r\n"
                               "Idempotency-Key: route-key\r\nContent-Length: ";
    http::Request request = parsed_request(prefix + std::to_string(body.size()) + "\r\n\r\n" + body);
    Response created = route_request(request, auth, status);
    Response existing = route_request(request, auth, status);
    check(created.status == 202 && existing.status == 200 &&
              existing.body.find("\"existing\":true") != std::string::npos,
          "authenticated JSON submission creates and idempotently reuses a job resource");

    const json::ParseResult creation = json::parse(created.body);
    const json::Value* job_value = creation.value.get("job");
    const json::Value* id_value = job_value == nullptr ? nullptr : job_value->get("id");
    check(id_value != nullptr && id_value->is_string(), "job submission returns a stable job ID");
    const std::string job_id = id_value == nullptr ? std::string() : id_value->string;
    http::Request events = parsed_request(request_text("/ainiux/v1/jobs/" + job_id + "/events"));
    Response stream = route_request(events, auth, status);
    std::string records;
    check(stream.status == 200 && stream.streaming && stream.stream_body,
          "job event route upgrades to an SSE response");
    if (stream.stream_body) {
        stream.stream_body([&](std::string_view part) {
            records.append(part.data(), part.size());
            return true;
        });
    }
    check(records.find("id: 1\n") != std::string::npos &&
              records.find("event: queued\n") != std::string::npos,
          "SSE returns retained events with ordered IDs and stable envelopes");
    jobs.shutdown();
}

std::string mcp_meta(bool tasks = false) {
    return "\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\","
           "\"io.modelcontextprotocol/clientInfo\":{\"name\":\"test-client\",\"version\":\"1\"},"
           "\"io.modelcontextprotocol/clientCapabilities\":{" +
           std::string(tasks ? "\"extensions\":{\"io.modelcontextprotocol/tasks\":{}}" : "") +
           "}}";
}

http::Request mcp_request(const std::string& method,
                         const std::string& params,
                         const std::string& name = {},
                         const std::string& token = "mcp-token") {
    const std::string body = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":" +
                             json::quote(method) + ",\"params\":{" + params + "}}";
    std::string request = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " +
                          token + "\r\nContent-Type: application/json\r\nAccept: application/json, text/event-stream\r\n"
                          "MCP-Protocol-Version: 2026-07-28\r\nMcp-Method: " + method + "\r\n";
    if (!name.empty()) request += "Mcp-Name: " + name + "\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    return parsed_request(request);
}

void test_mcp_stateless_adapter_and_tasks() {
    cli::Options options;
    options.provider = "none";
    JobService jobs(options, ".", 8);
    McpAdapter adapter(&jobs, 8);
    AuthConfig auth{"controller", "mcp-token"};
    std::atomic<std::size_t> active{0};
    PublicStatus status{8766, 64, 8, &active, &jobs, &adapter};

    Response discover = route_request(mcp_request("server/discover", mcp_meta()), auth, status);
    check(discover.status == 200 && discover.body.find("\"supportedVersions\":[\"2026-07-28\"]") != std::string::npos &&
              discover.body.find("io.modelcontextprotocol/tasks") != std::string::npos,
          "MCP discovery returns the supported stateless version and task extension");

    Response list = route_request(mcp_request("tools/list", mcp_meta()), auth, status);
    check(list.status == 200 && list.body.find("ainiux_chat") != std::string::npos &&
              list.body.find("ainiux_job_cancel") != std::string::npos &&
              list.body.find("\"ttlMs\":300000") != std::string::npos,
          "MCP tools/list returns a deterministic cacheable tool catalog");

    http::Request mismatched = mcp_request("tools/list", mcp_meta());
    mismatched.headers["mcp-method"] = "tools/call";
    Response mismatch = route_request(mismatched, auth, status);
    check(mismatch.status == 400 && mismatch.body.find("-32020") != std::string::npos,
          "MCP rejects a standard routing header that disagrees with the JSON-RPC body");

    const std::string call_params = mcp_meta(true) +
        ",\"name\":\"ainiux_chat\",\"arguments\":{\"provider\":\"none\","
        "\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}";
    Response call = route_request(mcp_request("tools/call", call_params, "ainiux_chat"), auth, status);
    const json::ParseResult call_json = json::parse(call.body);
    const json::Value* task_result = call_json.value.get("result");
    const json::Value* task_id = task_result == nullptr ? nullptr : task_result->get("taskId");
    check(call.status == 200 && task_id != nullptr && task_id->is_string() &&
              task_id->string.rfind("mcp_task_", 0) == 0,
          "task-enabled tools/call returns an opaque MCP task handle");
    if (task_id != nullptr && task_id->is_string()) {
        const std::string task = task_id->string;
        Response get = route_request(mcp_request("tasks/get", mcp_meta() +
                                                   ",\"taskId\":" + json::quote(task), task), auth, status);
        check(get.status == 200 && get.body.find("\"taskId\":" + json::quote(task)) != std::string::npos,
              "tasks/get resolves the task handle through the shared job registry");
        const std::string job_params = mcp_meta() + ",\"name\":\"ainiux_job_get\",\"arguments\":{" +
                                        "\"job_id\":" + json::quote(task) + "}";
        Response job_get = route_request(mcp_request("tools/call", job_params, "ainiux_job_get"), auth, status);
        check(job_get.status == 200 && job_get.body.find("\"structuredContent\"") != std::string::npos,
              "MCP job_get exposes a structured snapshot without raw server paths");
        Response cancel = route_request(mcp_request("tasks/cancel", mcp_meta() +
                                                       ",\"taskId\":" + json::quote(task), task), auth, status);
        check(cancel.status == 200 && cancel.body.find("\"resultType\":\"complete\"") != std::string::npos,
              "tasks/cancel acknowledges an MCP task through JobRegistry cancellation");
    }
    check(route_request(mcp_request("tools/list", mcp_meta(), {}, "controller"), auth, status).status == 401,
          "full-control credentials cannot be used at the MCP endpoint");
    jobs.shutdown();
}

http::Request session_request(const std::string& method,
                              const std::string& path,
                              const std::string& body = "{}") {
    std::string request = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                          "Authorization: Bearer controller\r\n";
    if (method == "POST") request += "Content-Type: application/json\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    return parsed_request(request);
}

void test_interactive_sessions_are_bounded_and_replayable() {
    namespace fs = std::filesystem;
    const fs::path workspace = fs::temp_directory_path() / "ainiux-session-hub-test";
    std::error_code cleanup_error;
    fs::remove_all(workspace, cleanup_error);
    fs::create_directories(workspace, cleanup_error);
    check(!cleanup_error, "session test creates an isolated workspace");

    cli::Options options;
    options.provider = "none";
    SessionHub hub(options, workspace.u8string(), 1);
    AuthConfig auth{"controller", {}};
    std::atomic<std::size_t> active{0};
    PublicStatus status{8766, 64, 8, &active, nullptr};
    status.sessions = &hub;
    status.max_sessions = 1;

    const std::string create_body =
        "{\"kind\":\"agent\",\"provider\":\"none\",\"permission_mode\":\"confirm\",\"task_mode\":\"act\"}";
    Response created = route_request(session_request("POST", "/ainiux/v1/sessions/agent", create_body),
                                     auth, status);
    const json::ParseResult created_json = json::parse(created.body);
    const json::Value* session_value = created_json.value.get("session");
    const json::Value* id_value = session_value == nullptr ? nullptr : session_value->get("id");
    check(created.status == 202 && id_value != nullptr && id_value->is_string(),
          "interactive agent session creation returns an opaque session ID");
    if (id_value == nullptr || !id_value->is_string()) {
        hub.shutdown();
        fs::remove_all(workspace, cleanup_error);
        return;
    }
    const std::string id = id_value->string;
    std::shared_ptr<InteractiveSession> session = hub.find(id);
    for (int i = 0; i < 200 && session->snapshot_json().find("\"status\":\"ready\"") == std::string::npos; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(session->snapshot_json().find("\"status\":\"ready\"") != std::string::npos,
          "session preparation completes asynchronously without blocking creation");

    Response listed = route_request(session_request("GET", "/ainiux/v1/sessions", ""), auth, status);
    check(listed.status == 200 && listed.body.find(id) != std::string::npos,
          "session listing exposes state without workspace paths");
    const ReplayBatch replay = session->events().replay_after(0);
    check(!replay.events.empty() && replay.events.front().session_id == id &&
              replay.events.back().type == "ready",
          "session events retain ordered creation and readiness state");

    Response turn = route_request(session_request(
        "POST", "/ainiux/v1/sessions/" + id + "/turns", "{\"text\":\"hello\"}"), auth, status);
    const json::ParseResult turn_json = json::parse(turn.body);
    const json::Value* turn_id_value = turn_json.value.get("turn_id");
    check(turn.status == 202 && turn_id_value != nullptr && turn_id_value->is_string(),
          "ready interactive sessions accept asynchronous turns");
    if (turn_id_value != nullptr && turn_id_value->is_string()) {
        const std::string turn_id = turn_id_value->string;
        Response conflict = route_request(session_request(
            "POST", "/ainiux/v1/sessions/" + id + "/turns", "{\"text\":\"second\"}"), auth, status);
        check(conflict.status == 409, "a session rejects a concurrent turn instead of racing controllers");
        Response cancelled = route_request(session_request(
            "POST", "/ainiux/v1/sessions/" + id + "/turns/" + turn_id + "/cancel", ""), auth, status);
        check(cancelled.status == 200, "interactive turn cancellation is explicit and idempotent at the session boundary");
    }
    for (int i = 0; i < 200 && session->snapshot_json().find("\"turn_id\":null") == std::string::npos; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    Response over_capacity = route_request(session_request("POST", "/ainiux/v1/sessions", create_body),
                                            auth, status);
    check(over_capacity.status == 429, "interactive sessions obey the configured bounded capacity");
    Response deleted = route_request(session_request("DELETE", "/ainiux/v1/sessions/" + id, ""), auth, status);
    check(deleted.status == 200 && hub.size() == 0, "deleting a session cancels work and releases its retained state");
    hub.shutdown();
    fs::remove_all(workspace, cleanup_error);
}

void test_read_only_workspace_routes_are_contained_and_bounded() {
    namespace fs = std::filesystem;
    const fs::path workspace = fs::temp_directory_path() / "ainiux-workspace-service-test";
    const fs::path outside = fs::temp_directory_path() / "ainiux-workspace-service-outside";
    std::error_code cleanup_error;
    fs::remove_all(workspace, cleanup_error);
    fs::remove_all(outside, cleanup_error);
    fs::create_directories(workspace / "nested", cleanup_error);
    fs::create_directories(workspace / ".ainiux-pr", cleanup_error);
    fs::create_directories(outside, cleanup_error);
    check(!cleanup_error, "workspace service creates isolated test directories");
    {
        std::ofstream(workspace / "visible.txt", std::ios::binary) << "visible text\n";
        std::ofstream(workspace / "nested" / "inside.md", std::ios::binary) << "nested text\n";
        std::ofstream(workspace / ".ainiux-pr" / "private.json", std::ios::binary) << "private";
        std::ofstream(workspace / "config.conf", std::ios::binary) << "provider_secret = hidden\n";
        std::ofstream(workspace / "secret.txt", std::ios::binary) << "do not expose\n";
        std::ofstream(outside / "outside.txt", std::ios::binary) << "outside\n";
    }
#if !defined(_WIN32)
    fs::create_symlink(outside / "outside.txt", workspace / "escape", cleanup_error);
#endif
    std::ofstream large(workspace / "large.txt", std::ios::binary);
    large << std::string(1024U * 1024U + 1U, 'x');
    large.close();

    WorkspaceService service(workspace.u8string());
    std::string body;
    check(service.list("", body).ok() && body.find("visible.txt") != std::string::npos &&
              body.find("nested") != std::string::npos &&
              body.find("private.json") == std::string::npos &&
              body.find("config.conf") == std::string::npos &&
              body.find("secret.txt") == std::string::npos &&
              body.find(workspace.u8string()) == std::string::npos,
          "dired route lists safe relative entries without private state or absolute paths");
    check(service.read("visible.txt", body).ok() && body.find("visible text") != std::string::npos &&
              body.find(workspace.u8string()) == std::string::npos,
          "file route returns bounded content using a workspace-relative path");
    check(!service.read("../outside.txt", body).ok() &&
              !service.read("nested/../visible.txt", body).ok() &&
              !service.read("large.txt", body).ok(),
          "file route rejects traversal and oversized reads");
#if !defined(_WIN32)
    check(!service.read("escape", body).ok(),
          "file route rejects symlink targets even when they point outside the workspace");
#endif

    AuthConfig auth{"controller", {}};
    std::atomic<std::size_t> active{0};
    PublicStatus status{8766, 64, 8, &active};
    status.workspace = &service;
    Response listed = route_request(
        parsed_request(request_text("/ainiux/v1/dired?path=nested")), auth, status);
    check(listed.status == 200 && listed.body.find("inside.md") != std::string::npos,
          "authenticated dired endpoint accepts one relative path query");
    Response file = route_request(
        parsed_request(request_text("/ainiux/v1/files?path=visible.txt")), auth, status);
    check(file.status == 200 && file.body.find("visible text") != std::string::npos,
          "authenticated files endpoint returns a selected workspace file");
    Response bad_query = route_request(
        parsed_request(request_text("/ainiux/v1/files?path=visible.txt&other=x")), auth, status);
    check(bad_query.status == 400, "workspace routes reject unrecognized query parameters");
    Response review = route_request(
        parsed_request(request_text("/ainiux/v1/workspace/review")), auth, status);
    check(review.status == 200 && review.body.find("nested/inside.md") != std::string::npos &&
              review.body.find("private.json") == std::string::npos &&
              review.body.find(workspace.u8string()) == std::string::npos,
          "workspace review is recursive, bounded, and excludes private paths");

    fs::remove_all(workspace, cleanup_error);
    fs::remove_all(outside, cleanup_error);
}

void test_revision_safe_chat_thread_routes() {
    namespace fs = std::filesystem;
    const fs::path directory = fs::temp_directory_path() / "ainiux-chat-service-test";
    const fs::path database = directory / "ainiux.db";
    std::error_code cleanup_error;
    fs::remove_all(directory, cleanup_error);
    fs::create_directories(directory, cleanup_error);
    check(!cleanup_error, "chat service creates an isolated database directory");

    ChatService service(database.u8string());
    AuthConfig auth{"controller", {}};
    std::atomic<std::size_t> active{0};
    PublicStatus status{8766, 64, 8, &active};
    status.chat_threads = &service;

    Response created = route_request(session_request(
        "POST", "/ainiux/v1/chat/threads",
        "{\"revision\":0,\"name\":\"Remote chat\",\"provider\":\"openai\",\"model\":\"model-a\"}"),
        auth, status);
    const json::ParseResult created_json = json::parse(created.body);
    const json::Value* created_thread = created_json.value.get("thread");
    const json::Value* id_value = created_thread == nullptr ? nullptr : created_thread->get("id");
    const json::Value* revision_value =
        created_thread == nullptr ? nullptr : created_thread->get("revision");
    check(created.status == 201 && id_value != nullptr &&
              id_value->type == json::Value::Type::Number && revision_value != nullptr &&
              revision_value->number == 1,
          "chat thread creation requires revision zero and returns revision one");
    if (id_value == nullptr || id_value->type != json::Value::Type::Number) {
        fs::remove_all(directory, cleanup_error);
        return;
    }
    const long long thread_id = static_cast<long long>(id_value->number);
    const std::string thread_path = "/ainiux/v1/chat/threads/" + std::to_string(thread_id);

    Response listed = route_request(session_request("GET", "/ainiux/v1/chat/threads", ""),
                                    auth, status);
    check(listed.status == 200 && listed.body.find("Remote chat") != std::string::npos &&
              listed.body.find(database.u8string()) == std::string::npos,
          "chat thread listing exposes bounded summaries without the database path");

    Response appended = route_request(session_request(
        "POST", thread_path + "/messages",
        "{\"revision\":1,\"messages\":[{\"role\":\"user\",\"content\":\"hello\"},"
        "{\"role\":\"assistant\",\"content\":\"hi\"}]}"), auth, status);
    check(appended.status == 200 && appended.body.find("\"revision\":2") != std::string::npos &&
              appended.body.find("\"message_count\":2") != std::string::npos,
          "message append atomically advances the observed thread revision");

    Response stale = route_request(session_request(
        "POST", thread_path + "/messages",
        "{\"revision\":1,\"messages\":[{\"role\":\"user\",\"content\":\"stale write\"}]}"),
        auth, status);
    check(stale.status == 409 && stale.body.find("revision_conflict") != std::string::npos &&
              stale.body.find("\"current_revision\":2") != std::string::npos,
          "stale chat clients receive a conflict with the current revision");

    Response loaded = route_request(session_request("GET", thread_path, ""), auth, status);
    check(loaded.status == 200 && loaded.body.find("hello") != std::string::npos &&
              loaded.body.find("stale write") == std::string::npos &&
              loaded.body.find("\"revision\":2") != std::string::npos,
          "thread loading returns the committed transcript and rejects stale content");

    {
        chat::SqliteStore tui_store;
        Error error = tui_store.open(database.u8string());
        chat::Session session;
        if (error.ok()) error = tui_store.load_session(thread_id, session);
        if (error.ok()) {
            std::vector<provider::TextAttachment> attachments;
            for (int index = 0; index < 65; ++index) {
                provider::TextAttachment attachment;
                attachment.markdown_content =
                    "private attachment body " + std::to_string(index);
                attachment.display_name = "notes-" + std::to_string(index) + ".md";
                attachment.source_ref =
                    "/private/source/notes-" + std::to_string(index) + ".md";
                attachment.byte_size =
                    static_cast<long long>(attachment.markdown_content.size());
                attachments.push_back(std::move(attachment));
            }
            session.messages.push_back({"user", "from TUI", {}, std::move(attachments)});
            error = tui_store.save_session(session);
        }
        check(error.ok() && session.revision == 3,
              "ordinary TUI persistence advances the same optimistic revision");
    }
    loaded = route_request(session_request("GET", thread_path, ""), auth, status);
    check(loaded.status == 200 && loaded.body.find("\"revision\":3") != std::string::npos &&
              loaded.body.find("notes-0.md") != std::string::npos &&
              loaded.body.find("notes-64.md") == std::string::npos &&
              loaded.body.find("\"attachments_truncated\":true") != std::string::npos &&
              loaded.body.find("/private/source") == std::string::npos &&
              loaded.body.find("private attachment body") == std::string::npos,
          "remote loads bound attachment metadata without reading source paths or payloads");
    Response stale_after_tui = route_request(session_request(
        "POST", thread_path + "/messages",
        "{\"revision\":2,\"messages\":[{\"role\":\"user\",\"content\":\"raced\"}]}"),
        auth, status);
    check(stale_after_tui.status == 409 &&
              stale_after_tui.body.find("\"current_revision\":3") != std::string::npos,
          "API revisions detect saves made by another chat-store connection");

    chat::Session stale_tui_snapshot;
    {
        chat::SqliteStore store;
        Error error = store.open(database.u8string());
        if (error.ok()) error = store.load_session(thread_id, stale_tui_snapshot);
        check(error.ok() && stale_tui_snapshot.revision == 3,
              "stale full-save fixture observes the current thread revision");
    }
    Response remote_wins = route_request(session_request(
        "POST", thread_path + "/messages",
        "{\"revision\":3,\"messages\":[{\"role\":\"assistant\",\"content\":\"remote wins\"}]}"),
        auth, status);
    Error stale_save_error;
    {
        chat::SqliteStore store;
        stale_save_error = store.open(database.u8string());
        stale_tui_snapshot.messages.push_back({"user", "stale TUI overwrite"});
        if (stale_save_error.ok()) stale_save_error = store.save_session(stale_tui_snapshot);
    }
    loaded = route_request(session_request("GET", thread_path, ""), auth, status);
    check(remote_wins.status == 200 && stale_save_error.code == ErrorCode::FileLock &&
              loaded.body.find("remote wins") != std::string::npos &&
              loaded.body.find("stale TUI overwrite") == std::string::npos &&
              loaded.body.find("\"revision\":4") != std::string::npos,
          "a stale full TUI save cannot overwrite a newer API append");

    Response invalid_attachment = route_request(session_request(
        "POST", thread_path + "/messages",
        "{\"revision\":3,\"messages\":[{\"role\":\"user\",\"content\":\"x\","
        "\"attachments\":[{\"path\":\"/private/file\"}]}]}"), auth, status);
    check(invalid_attachment.status == 400,
          "remote message input rejects unrestricted attachment fields");

    long long large_thread_id = 0;
    {
        chat::SqliteStore store;
        Error error = store.open(database.u8string());
        chat::Session large_session;
        large_session.name = "Large remote thread";
        for (int index = 0; index < 520; ++index) {
            large_session.messages.push_back(
                {"user", "bounded-message-" + std::to_string(index)});
        }
        if (error.ok()) error = store.save_session(large_session);
        large_thread_id = large_session.thread_id;
        check(error.ok(), "large chat fixture persists through the ordinary store");
    }
    Response bounded = route_request(session_request(
        "GET", "/ainiux/v1/chat/threads/" + std::to_string(large_thread_id), ""), auth, status);
    check(bounded.status == 200 &&
              bounded.body.find("\"message_count\":520") != std::string::npos &&
              bounded.body.find("\"messages_truncated\":true") != std::string::npos &&
              bounded.body.find("bounded-message-519") != std::string::npos &&
              bounded.body.find("bounded-message-0\"") == std::string::npos,
          "remote thread loads retain the newest 512 messages and report truncation");

    const fs::path blocked_parent = directory / "not-a-directory";
    std::ofstream(blocked_parent, std::ios::binary) << "file";
    ChatService failed((blocked_parent / "ainiux.db").u8string());
    status.chat_threads = &failed;
    Response failure = route_request(session_request("GET", "/ainiux/v1/chat/threads", ""),
                                     auth, status);
    check(failure.status == 500 &&
              failure.body.find(blocked_parent.u8string()) == std::string::npos,
          "chat database failures do not expose server-side paths");

    fs::remove_all(directory, cleanup_error);
}

void test_job_errors_hide_server_side_paths() {
    cli::Options options;
    options.provider = "none";
    options.key_file = "/private/provider/credential.txt";
    JobService jobs(options, ".", 2);
    const std::string body =
        "{\"provider\":\"none\",\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}";
    ServiceSubmitResult submitted = jobs.submit("chat", body, "");
    check(submitted.validation_error.ok() && submitted.submission.job &&
              wait_terminal(submitted.submission.job),
          "server-side provider configuration failure completes the job");
    const std::string snapshot = submitted.submission.job->snapshot_json();
    check(snapshot.find("/private/provider") == std::string::npos &&
              snapshot.find("server-side file") != std::string::npos,
          "job errors do not expose configured local credential paths");
    jobs.shutdown();
}

void test_server_cli_contract() {
    const char* argv[] = {"ainiux", "server", "--workspace", ".", "--bind", "127.0.0.1",
                          "--port", "9001", "--max-connections", "7", "--max-jobs", "9",
                          "--max-sessions", "5"};
    cli::ParseResult parsed = cli::parse_args(
        static_cast<int>(sizeof(argv) / sizeof(argv[0])), const_cast<char**>(argv));
    check(parsed.error.ok() && parsed.options.server && parsed.options.port == 9001 &&
              parsed.options.bind_address == "127.0.0.1" &&
              parsed.options.max_connections == 7 && parsed.options.max_jobs == 9 &&
              parsed.options.max_sessions == 5,
          "server subcommand and bounded options parse");
    check(validate_server_options(parsed.options).ok(), "standalone server options validate");
    const char* combined[] = {"ainiux", "server", "-p", "hello"};
    parsed = cli::parse_args(4, const_cast<char**>(combined));
    check(parsed.error.ok() && !validate_server_options(parsed.options).ok(),
          "server mode rejects prompt combinations");
    const char* misplaced[] = {"ainiux", "-p", "hello", "--port", "9001"};
    parsed = cli::parse_args(5, const_cast<char**>(misplaced));
    check(parsed.error.ok() && !validate_server_options(parsed.options).ok(),
          "server-only options are rejected outside server mode");
    const char* stdin_key[] = {"ainiux", "server", "--key-stdin"};
    parsed = cli::parse_args(3, const_cast<char**>(stdin_key));
    check(parsed.error.ok() && !validate_server_options(parsed.options).ok(),
          "server mode rejects provider credentials read from process stdin");

    const char* unsafe_remote[] = {"ainiux", "server", "--bind", "0.0.0.0"};
    parsed = cli::parse_args(4, const_cast<char**>(unsafe_remote));
    check(parsed.error.ok() && !validate_server_options(parsed.options).ok(),
          "non-loopback server binding fails closed without TLS or explicit plaintext opt-in");
    const char* explicit_plain[] = {"ainiux", "server", "--bind", "0.0.0.0",
                                    "--insecure-plain-bind"};
    parsed = cli::parse_args(5, const_cast<char**>(explicit_plain));
    check(parsed.error.ok() && parsed.options.insecure_plain_bind &&
              validate_server_options(parsed.options).ok(),
          "non-loopback plaintext binding requires an explicit unsafe opt-in");
    const char* incomplete_tls[] = {"ainiux", "server", "--tls-cert", "cert.pem"};
    parsed = cli::parse_args(4, const_cast<char**>(incomplete_tls));
    check(parsed.error.ok() && !validate_server_options(parsed.options).ok(),
          "server TLS requires a certificate and private key pair");

    cli::Options base;
    base.provider = "none";
    SessionHub remote_sessions(base, ".", 1, false);
    const SessionCreateResult denied_yolo = remote_sessions.create(
        "{\"kind\":\"agent\",\"provider\":\"none\",\"permission_mode\":\"yolo\"}");
    check(!denied_yolo.error.ok() && denied_yolo.error.code == ErrorCode::UnsupportedFeature,
          "remote session policy denies an explicit Yolo request without startup opt-in");
    remote_sessions.shutdown();
}

#if !defined(_WIN32)
int open_loopback(unsigned short port) {
    const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) return -1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(socket_fd);
        return -1;
    }
    return socket_fd;
}

std::string raw_request(unsigned short port, const std::string& request) {
    const int socket_fd = open_loopback(port);
    if (socket_fd < 0) return {};
    std::size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t count = ::send(socket_fd, request.data() + sent, request.size() - sent, 0);
        if (count <= 0) break;
        sent += static_cast<std::size_t>(count);
    }
    (void)::shutdown(socket_fd, SHUT_WR);
    std::string response;
    char buffer[2048];
    for (;;) {
        const ssize_t count = ::recv(socket_fd, buffer, sizeof(buffer), 0);
        if (count <= 0) break;
        response.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(socket_fd);
    return response;
}

std::size_t append_curl_body(char* data, std::size_t size, std::size_t count, void* userdata) {
    static_cast<std::string*>(userdata)->append(data, size * count);
    return size * count;
}

void test_tls_listener_lifecycle() {
    if (!TlsContext::available()) return;
    Listener listener;
    ListenerConfig config;
    config.port = 0;
    config.max_connections = 2;
    config.auth.full_control_secret = "controller";
    config.tls_cert_file = "tests/fixtures/server_tls_cert.pem";
    config.tls_key_file = "tests/fixtures/server_tls_key.pem";
    check(listener.start(config).ok() && listener.tls_enabled(),
          "listener loads a matching PEM certificate and private key through RAII TLS state");
    std::atomic<bool> stop{false};
    Error serve_error;
    std::thread server_thread([&] { serve_error = listener.serve_until([&] { return stop.load(); }); });

    CURL* curl = curl_easy_init();
    std::string body;
    long status = 0;
    curl_slist* raw_headers = nullptr;
    raw_headers = curl_slist_append(raw_headers, "Authorization: Bearer controller");
    const std::string origin = "Origin: https://127.0.0.1:" + std::to_string(listener.port());
    raw_headers = curl_slist_append(raw_headers, origin.c_str());
    const std::string url = "https://127.0.0.1:" + std::to_string(listener.port()) +
                            "/ainiux/v1/health";
    check(curl != nullptr && raw_headers != nullptr, "TLS client test resources allocate");
    if (curl != nullptr && raw_headers != nullptr) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, raw_headers);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_curl_body);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        const CURLcode result = curl_easy_perform(curl);
        (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        check(result == CURLE_OK && status == 200 && body == "{\"status\":\"ok\"}",
              "TLS listener completes an authenticated HTTPS request with strict Origin");
    }
    if (raw_headers != nullptr) curl_slist_free_all(raw_headers);
    if (curl != nullptr) curl_easy_cleanup(curl);
    stop.store(true);
    server_thread.join();
    check(serve_error.ok(), "TLS listener shuts down cleanly and releases connection state");

    Listener mismatched;
    config.tls_cert_file = "tests/fixtures/server_tls_key.pem";
    check(!mismatched.start(config).ok(), "listener rejects an invalid certificate file");
}

void test_loopback_listener_lifecycle() {
    Listener listener;
    ListenerConfig config;
    config.port = 0;
    config.max_connections = 2;
    config.auth.full_control_secret = "controller";
    check(listener.start(config).ok() && listener.port() != 0,
          "listener binds an ephemeral loopback port");
    std::atomic<bool> stop{false};
    Error serve_error;
    std::thread server_thread([&] { serve_error = listener.serve_until([&] { return stop.load(); }); });
    const std::string response = raw_request(listener.port(), request_text());
    check(response.find("HTTP/1.1 200 OK") == 0 &&
              response.find("X-Content-Type-Options: nosniff") != std::string::npos &&
              response.find("Content-Security-Policy: default-src 'none'") != std::string::npos,
          "raw loopback request reaches authenticated health with hardened headers");

    const int idle_client = open_loopback(listener.port());
    for (int i = 0; i < 50 && listener.active_connections() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(idle_client >= 0 && listener.active_connections() == 1,
          "listener tracks an idle active connection");
    stop.store(true);
    server_thread.join();
    if (idle_client >= 0) ::close(idle_client);
    check(serve_error.ok() && listener.active_connections() == 0,
          "listener shutdown joins workers and releases active connections");

    Listener capped;
    config.max_connections = 1;
    check(capped.start(config).ok(), "connection-cap listener starts");
    stop.store(false);
    std::thread capped_thread([&] { serve_error = capped.serve_until([&] { return stop.load(); }); });
    const int held = open_loopback(capped.port());
    for (int i = 0; i < 50 && capped.active_connections() != 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const std::string busy = raw_request(capped.port(), request_text());
    check(held >= 0 && busy.find("HTTP/1.1 429 Too Many Requests") == 0,
          "simultaneous connection cap rejects excess clients");
    if (held >= 0) ::close(held);
    stop.store(true);
    capped_thread.join();
}
#else
void test_tls_listener_lifecycle() {}
void test_loopback_listener_lifecycle() {
    // Native Windows socket lifecycle is compiled into the product target; the
    // platform integration gate exercises it with the packaged executable.
}
#endif

}  // namespace

void run_all() {
    test_fragmented_parser_and_pipeline();
    test_strict_framing_and_limits();
    test_auth_and_routes();
    test_event_replay_is_ordered_and_bounded();
    test_job_registry_idempotency_lane_and_cancellation();
    test_provider_job_concurrency_cap();
    test_terminal_retention_eviction_releases_workers_safely();
    test_job_routes_and_sse();
    test_mcp_stateless_adapter_and_tasks();
    test_interactive_sessions_are_bounded_and_replayable();
    test_read_only_workspace_routes_are_contained_and_bounded();
    test_revision_safe_chat_thread_routes();
    test_job_errors_hide_server_side_paths();
    test_server_cli_contract();
    test_loopback_listener_lifecycle();
    test_tls_listener_lifecycle();
}

}  // namespace ainiux::test::server_control
