#include "provider/test_provider.hpp"
#include "support/test_support.hpp"
#include "cli/args.hpp"
#include "json/json.hpp"
#include "provider/names.hpp"
#include "provider/model_selection.hpp"
#include "provider/provider.hpp"

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
#include <sys/time.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ainiux::test::provider {

namespace {

using ainiux::test::check;
using ainiux::test::read_fixture;

ainiux::json::Value serialized_request_json(ainiux::provider::RequestContext context) {
    context.options.model = context.options.model.empty() ? "mock-model" : context.options.model;
    context.options.stream = false;
    const std::string request =
        ainiux::provider::serialize_request(context, {{"user", "hello"}});
    ainiux::json::ParseResult parsed = ainiux::json::parse(request);
    check(parsed.error.ok(), "provider request serialization returns valid JSON");
    return std::move(parsed.value);
}

const ainiux::json::Value* field(const ainiux::json::Value& value, const std::string& name) {
    return value.get(name);
}

const ainiux::json::Value* field(const ainiux::json::Value* value, const std::string& name) {
    return value == nullptr ? nullptr : value->get(name);
}

void check_string_field(const ainiux::json::Value& value,
                        const std::string& name,
                        const std::string& expected,
                        const std::string& message) {
    const ainiux::json::Value* actual = field(value, name);
    check(actual != nullptr && actual->is_string() && actual->string == expected, message);
}

void check_string_field(const ainiux::json::Value* value,
                        const std::string& name,
                        const std::string& expected,
                        const std::string& message) {
    const ainiux::json::Value* actual = field(value, name);
    check(actual != nullptr && actual->is_string() && actual->string == expected, message);
}

void check_number_field(const ainiux::json::Value& value,
                        const std::string& name,
                        double expected,
                        const std::string& message) {
    const ainiux::json::Value* actual = field(value, name);
    check(actual != nullptr && actual->type == ainiux::json::Value::Type::Number &&
              actual->number == expected,
          message);
}

void check_number_field(const ainiux::json::Value* value,
                        const std::string& name,
                        double expected,
                        const std::string& message) {
    const ainiux::json::Value* actual = field(value, name);
    check(actual != nullptr && actual->type == ainiux::json::Value::Type::Number &&
              actual->number == expected,
          message);
}

void check_bool_field(const ainiux::json::Value& value,
                      const std::string& name,
                      bool expected,
                      const std::string& message) {
    const ainiux::json::Value* actual = field(value, name);
    check(actual != nullptr && actual->type == ainiux::json::Value::Type::Bool &&
              actual->boolean == expected,
          message);
}

void check_bool_field(const ainiux::json::Value* value,
                      const std::string& name,
                      bool expected,
                      const std::string& message) {
    const ainiux::json::Value* actual = field(value, name);
    check(actual != nullptr && actual->type == ainiux::json::Value::Type::Bool &&
              actual->boolean == expected,
          message);
}

#if defined(_WIN32)
using TestSocket = SOCKET;
constexpr TestSocket kInvalidTestSocket = INVALID_SOCKET;
void close_test_socket(TestSocket socket) { closesocket(socket); }
class WinsockGuard {
   public:
    WinsockGuard() { WSADATA data{}; ready_ = WSAStartup(MAKEWORD(2, 2), &data) == 0; }
    ~WinsockGuard() { if (ready_) WSACleanup(); }
    bool ready() const { return ready_; }
   private:
    bool ready_ = false;
};
#else
using TestSocket = int;
constexpr TestSocket kInvalidTestSocket = -1;
void close_test_socket(TestSocket socket) { close(socket); }
#endif

class UniqueFd {
   public:
    explicit UniqueFd(TestSocket fd = kInvalidTestSocket) : fd_(fd) {}
    ~UniqueFd() { reset(); }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    TestSocket get() const { return fd_; }
    TestSocket release() {
        const TestSocket fd = fd_;
        fd_ = kInvalidTestSocket;
        return fd;
    }
    void reset(TestSocket next = kInvalidTestSocket) {
        if (fd_ != kInvalidTestSocket) {
            close_test_socket(fd_);
        }
        fd_ = next;
    }

   private:
    TestSocket fd_ = kInvalidTestSocket;
};

bool send_all(TestSocket fd, const std::string& data) {
    size_t offset = 0;
    while (offset < data.size()) {
#ifdef MSG_NOSIGNAL
        const int flags = MSG_NOSIGNAL;
#else
        const int flags = 0;
#endif
        const int written = send(fd, data.data() + offset,
                                 static_cast<int>(data.size() - offset), flags);
        if (written <= 0) {
            return false;
        }
        offset += static_cast<size_t>(written);
    }
    return true;
}

ainiux::Error run_stream_from_body(const std::string& body,
                                   ainiux::provider::ApiKind api_kind,
                                   ainiux::provider::ChatResult& result,
                                   std::string& streamed) {
#if defined(_WIN32)
    WinsockGuard winsock;
    if (!winsock.ready())
        return {ainiux::ErrorCode::Internal, "could not initialize Winsock"};
#endif
    UniqueFd listen_fd(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (listen_fd.get() == kInvalidTestSocket) {
        return {ainiux::ErrorCode::Internal, "could not create test server socket"};
    }
    const int yes = 1;
    if (setsockopt(listen_fd.get(), SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&yes), sizeof(yes)) != 0) {
        return {ainiux::ErrorCode::Internal, "could not configure test server socket reuse"};
    }
#if defined(_WIN32)
    const DWORD timeout = 5000;
#else
    timeval timeout{};
    timeout.tv_sec = 5;
#endif
    if (setsockopt(listen_fd.get(), SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout)) != 0) {
        return {ainiux::ErrorCode::Internal, "could not configure test server socket timeout"};
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(listen_fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        return {ainiux::ErrorCode::Internal, "could not bind test server socket"};
    }
    if (listen(listen_fd.get(), 1) != 0) {
        return {ainiux::ErrorCode::Internal, "could not listen on test server socket"};
    }
#if defined(_WIN32)
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    if (getsockname(listen_fd.get(), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        return {ainiux::ErrorCode::Internal, "could not inspect test server socket"};
    }
    const int port = ntohs(address.sin_port);
    const TestSocket server_fd = listen_fd.release();
    std::thread server([server_fd, body]() {
        UniqueFd scoped_listen(server_fd);
        UniqueFd client(accept(scoped_listen.get(), nullptr, nullptr));
        if (client.get() == kInvalidTestSocket) {
            return;
        }
        char request_buffer[1024] = {};
        if (recv(client.get(), request_buffer, sizeof(request_buffer), 0) < 0) {
            return;
        }
        const std::string response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Connection: close\r\n"
            "\r\n" +
            body;
        (void)send_all(client.get(), response);
    });

    ainiux::provider::RequestContext context;
    context.profile.name = "custom_openai_chat";
    context.profile.capabilities.chat_completions = true;
    context.profile.capabilities.responses_api = true;
    context.options.model = "mock-model";
    context.options.stream = true;
    context.options.connect_timeout_seconds = 2;
    context.options.timeout_seconds = 5;
    context.chat_url = "http://127.0.0.1:" + std::to_string(port) + "/v1/chat/completions";
    context.responses_url = "http://127.0.0.1:" + std::to_string(port) + "/v1/responses";
    context.api_kind = api_kind;

    ainiux::Error err = ainiux::provider::send_chat_messages(
        context,
        {{"user", "hello"}},
        [&](const std::string& delta) {
            streamed += delta;
            return ainiux::ok_error();
        },
        result);
    server.join();
    return err;
}

ainiux::Error run_chat_stream_from_body(const std::string& body,
                                        ainiux::provider::ChatResult& result,
                                        std::string& streamed) {
    return run_stream_from_body(body, ainiux::provider::ApiKind::ChatCompletions, result, streamed);
}

ainiux::Error run_responses_stream_from_body(const std::string& body,
                                             ainiux::provider::ChatResult& result,
                                             std::string& streamed) {
    return run_stream_from_body(body, ainiux::provider::ApiKind::Responses, result, streamed);
}

ainiux::Error run_chat_http_status_response(long status,
                                            const std::string& reason,
                                            const std::string& content_type,
                                            const std::string& body) {
#if defined(_WIN32)
    WinsockGuard winsock;
    if (!winsock.ready())
        return {ainiux::ErrorCode::Internal, "could not initialize Winsock"};
#endif
    UniqueFd listen_fd(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (listen_fd.get() == kInvalidTestSocket) {
        return {ainiux::ErrorCode::Internal, "could not create test server socket"};
    }
    const int yes = 1;
    if (setsockopt(listen_fd.get(), SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&yes), sizeof(yes)) != 0) {
        return {ainiux::ErrorCode::Internal, "could not configure test server socket reuse"};
    }
#if defined(_WIN32)
    const DWORD timeout = 5000;
#else
    timeval timeout{};
    timeout.tv_sec = 5;
#endif
    if (setsockopt(listen_fd.get(), SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout)) != 0) {
        return {ainiux::ErrorCode::Internal, "could not configure test server socket timeout"};
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(listen_fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        return {ainiux::ErrorCode::Internal, "could not bind test server socket"};
    }
    if (listen(listen_fd.get(), 1) != 0) {
        return {ainiux::ErrorCode::Internal, "could not listen on test server socket"};
    }
#if defined(_WIN32)
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    if (getsockname(listen_fd.get(), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        return {ainiux::ErrorCode::Internal, "could not inspect test server socket"};
    }
    const int port = ntohs(address.sin_port);
    const TestSocket server_fd = listen_fd.release();
    std::thread server([server_fd, status, reason, content_type, body]() {
        UniqueFd scoped_listen(server_fd);
        UniqueFd client(accept(scoped_listen.get(), nullptr, nullptr));
        if (client.get() == kInvalidTestSocket) {
            return;
        }
        char request_buffer[4096] = {};
        if (recv(client.get(), request_buffer, sizeof(request_buffer), 0) < 0) {
            return;
        }
        const std::string response =
            "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n"
            "Content-Type: " + content_type + "\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "Connection: close\r\n"
            "\r\n" +
            body;
        (void)send_all(client.get(), response);
    });

    ainiux::provider::RequestContext context;
    context.profile.name = "custom_openai_chat";
    context.profile.capabilities.chat_completions = true;
    context.options.model = "mock-model";
    context.options.stream = false;
    context.options.connect_timeout_seconds = 2;
    context.options.timeout_seconds = 5;
    context.chat_url = "http://127.0.0.1:" + std::to_string(port) + "/v1/chat/completions";

    ainiux::provider::ChatResult result;
    ainiux::Error err = ainiux::provider::send_chat_messages(
        context,
        {{"user", "hello"}},
        [](const std::string&) { return ainiux::ok_error(); },
        result);
    server.join();
    return err;
}

void test_http_status_errors_are_friendly() {
    const ainiux::Error err = run_chat_http_status_response(
        429,
        "Too Many Requests",
        "application/json",
        R"({"error":{"message":"Rate limit exceeded for mock-model"}})");
    check(!err.ok(), "provider HTTP status failure returns an error");
    check(err.code == ainiux::ErrorCode::RateLimit, "HTTP 429 maps to rate-limit error code");
    check(err.message.find("HTTP 429: Too many requests.") != std::string::npos,
          "HTTP 429 error starts with a friendly explanation");
    check(err.message.find("Perhaps you have used your daily limits?") != std::string::npos,
          "HTTP 429 error suggests quota exhaustion");
    check(err.message.find("Provider message: Rate limit exceeded for mock-model") != std::string::npos,
          "HTTP error includes concise provider message when available");
    check(err.message.find(R"({"error")") == std::string::npos,
          "HTTP error does not expose raw JSON error dumps");
}

void test_openrouter_nested_provider_errors_are_unwrapped() {
    // OpenRouter wraps Gemini/Google schema failures as error.metadata.raw JSON.
    const std::string body =
        R"({"error":{"message":"Provider returned error","code":400,"metadata":{"raw":"{\"error\":{\"code\":400,\"message\":\"* properties[ops].items: missing field.\\n\",\"status\":\"INVALID_ARGUMENT\"}}","provider_name":"Google AI Studio"}}})";
    const ainiux::Error err =
        run_chat_http_status_response(400, "Bad Request", "application/json", body);
    check(!err.ok(), "OpenRouter nested 400 returns an error");
    check(err.message.find("Google AI Studio") != std::string::npos,
          "nested error names the upstream provider");
    check(err.message.find("properties[ops].items: missing field") != std::string::npos,
          "nested error surfaces the real Gemini schema message: " + err.message);
    check(err.message.find("Provider returned error") == std::string::npos ||
              err.message.find("properties[ops].items") != std::string::npos,
          "nested detail is preferred over the generic OpenRouter shell message");
}

void test_chat_sse_accepts_cr_only_event_boundaries() {
    const std::string body =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\r\r"
        "data: {\"choices\":[{\"delta\":{\"content\":\" world\"}}]}\r\r"
        "data: [DONE]\r\r";
    ainiux::provider::ChatResult result;
    std::string streamed;
    const ainiux::Error err = run_chat_stream_from_body(body, result, streamed);
    check(err.ok(), "chat SSE parser accepts CR-only event boundaries");
    check(result.content == "Hello world", "CR-only SSE stream accumulates chat deltas");
    check(streamed == "Hello world", "CR-only SSE stream forwards chat deltas");
}

void test_chat_sse_streaming_reasoning_content() {
    const std::string body =
        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"internal\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\" trace\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"Visible answer\"}}]}\n\n"
        "data: [DONE]\n\n";
    ainiux::provider::ChatResult result;
    std::string streamed;
    const ainiux::Error err = run_chat_stream_from_body(body, result, streamed);
    check(err.ok(), "chat SSE parser streams reasoning_content deltas");
    check(result.content == "<think>internal trace</think>\n\nVisible answer",
          "reasoning_content stream wraps thinking before visible content");
    check(streamed == result.content, "reasoning_content stream forwards wrapped output");
}

void test_chat_sse_openrouter_reasoning_details_text() {
    const std::string body =
        "data: {\"choices\":[{\"delta\":{\"reasoning_details\":[{\"type\":\"reasoning.text\",\"text\":\"Let\"}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"reasoning_details\":[{\"type\":\"reasoning.text\",\"text\":\" me\"}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"reasoning_details\":[{\"type\":\"reasoning.text\",\"text\":\" think\"}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"Answer\"}}]}\n\n"
        "data: [DONE]\n\n";
    ainiux::provider::ChatResult result;
    std::string streamed;
    const ainiux::Error err = run_chat_stream_from_body(body, result, streamed);
    check(err.ok(), "chat SSE parser streams OpenRouter reasoning.text details");
    check(result.content == "<think>Let me think</think>\n\nAnswer",
          "OpenRouter reasoning.text chunks concatenate without extra newlines");
}

void test_chat_sse_openrouter_reasoning_not_duplicated_with_details() {
    const std::string body =
        "data: {\"choices\":[{\"delta\":{\"reasoning\":\"shared\",\"reasoning_details\":[{\"type\":\"reasoning.text\",\"text\":\"shared\"}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"Done\"}}]}\n\n"
        "data: [DONE]\n\n";
    ainiux::provider::ChatResult result;
    std::string streamed;
    const ainiux::Error err = run_chat_stream_from_body(body, result, streamed);
    check(err.ok(), "chat SSE parser accepts OpenRouter reasoning and reasoning_details together");
    check(result.content == "<think>shared</think>\n\nDone",
          "OpenRouter reasoning string is preferred over duplicate reasoning_details text");
}

void test_chat_sse_openrouter_reasoning_summary_and_text() {
    const std::string body =
        "data: {\"choices\":[{\"delta\":{\"reasoning_details\":["
        "{\"type\":\"reasoning.summary\",\"summary\":\"High level\"},"
        "{\"type\":\"reasoning.text\",\"text\":\"Detailed trace\"}"
        "]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"Final\"}}]}\n\n"
        "data: [DONE]\n\n";
    ainiux::provider::ChatResult result;
    std::string streamed;
    const ainiux::Error err = run_chat_stream_from_body(body, result, streamed);
    check(err.ok(), "chat SSE parser accepts OpenRouter summary and text reasoning details");
    check(result.content == "<think>High level\nDetailed trace</think>\n\nFinal",
          "OpenRouter reasoning.summary and reasoning.text are separated by a newline");
}

void test_chat_sse_reasoning_normalizes_crlf() {
    const std::string body =
        "data: {\"choices\":[{\"delta\":{\"reasoning\":\"line1\\r\\nline2\\rmore\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"OK\"}}]}\n\n"
        "data: [DONE]\n\n";
    ainiux::provider::ChatResult result;
    std::string streamed;
    const ainiux::Error err = run_chat_stream_from_body(body, result, streamed);
    check(err.ok(), "chat SSE parser normalizes reasoning newlines");
    check(result.content == "<think>line1\nline2\nmore</think>\n\nOK",
          "reasoning text normalizes CRLF and lone CR to LF");
    check(streamed == result.content, "normalized reasoning is forwarded to stream consumers");
}

void test_chat_sse_accepts_batched_json_data_lines() {
    const std::string body =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\" world\"}}]}\n"
        "\n"
        "data: [DONE]\n\n";
    ainiux::provider::ChatResult result;
    std::string streamed;
    const ainiux::Error err = run_chat_stream_from_body(body, result, streamed);
    check(err.ok(), "chat SSE parser accepts batched JSON data lines");
    check(result.content == "Hello world", "batched JSON data lines accumulate chat deltas");
    check(streamed == "Hello world", "batched JSON data lines forward chat deltas");
}

void test_chat_sse_accepts_concatenated_json_payloads() {
    const std::string body =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}{\"choices\":[{\"delta\":{\"content\":\" world\"}}]}\n\n"
        "data: [DONE]\n\n";
    ainiux::provider::ChatResult result;
    std::string streamed;
    const ainiux::Error err = run_chat_stream_from_body(body, result, streamed);
    check(err.ok(), "chat SSE parser accepts concatenated JSON payloads");
    check(result.content == "Hello world", "concatenated JSON payloads accumulate chat deltas");
    check(streamed == "Hello world", "concatenated JSON payloads forward chat deltas");
}

void test_chat_sse_accepts_concatenated_reasoning_payloads() {
    const std::string body =
        "data: {\"choices\":[{\"delta\":{\"reasoning\":\"1+1 is addition.\"}}]}"
        "{\"choices\":[{\"delta\":{\"content\":\"2\"}}]}\n\n"
        "data: [DONE]\n\n";
    ainiux::provider::ChatResult result;
    std::string streamed;
    const ainiux::Error err = run_chat_stream_from_body(body, result, streamed);
    check(err.ok(), "chat SSE parser accepts concatenated reasoning JSON payloads");
    check(result.content == "<think>1+1 is addition.</think>\n\n2",
          "concatenated reasoning payloads preserve thinking and visible content");
    check(streamed == result.content, "concatenated reasoning payloads forward wrapped thinking output");
}

void test_chat_sse_accepts_concatenated_payloads_with_leaked_data_prefixes() {
    const std::string body =
        "data: {\"choices\":[{\"delta\":{\"reasoning\":\"thinking\"}}]}"
        "data: {\"choices\":[{\"delta\":{\"content\":\"done\"}}]}\n\n"
        "data: [DONE]\n\n";
    ainiux::provider::ChatResult result;
    std::string streamed;
    const ainiux::Error err = run_chat_stream_from_body(body, result, streamed);
    check(err.ok(), "chat SSE parser accepts concatenated payloads with leaked data prefixes");
    check(result.content == "<think>thinking</think>\n\ndone",
          "leaked data-prefix payloads preserve reasoning and content");
}

void test_chat_sse_accepts_concatenated_payloads_with_sse_metadata_prefixes() {
    const std::string body =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}"
        "event: message\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\" world\"}}]}\n\n"
        "data: [DONE]\n\n";
    ainiux::provider::ChatResult result;
    std::string streamed;
    const ainiux::Error err = run_chat_stream_from_body(body, result, streamed);
    check(err.ok(), "chat SSE parser accepts concatenated payloads with leaked SSE metadata");
    check(result.content == "Hello world", "leaked SSE metadata payloads accumulate content");
}

void test_chat_sse_concatenated_payload_split_ignores_content_braces() {
    const std::string body =
        "data: {\"choices\":[{\"delta\":{\"content\":\"answer with {braces} and [brackets]\"}}]}"
        "{\"choices\":[{\"delta\":{\"content\":\" done\"}}]}\n\n"
        "data: [DONE]\n\n";
    ainiux::provider::ChatResult result;
    std::string streamed;
    const ainiux::Error err = run_chat_stream_from_body(body, result, streamed);
    check(err.ok(), "chat SSE concatenated payload split ignores braces inside content strings");
    check(result.content == "answer with {braces} and [brackets] done",
          "content braces are preserved while concatenated payloads are split");
}

void test_chat_sse_accepts_json_payload_immediately_followed_by_done() {
    const std::string body =
        "data: {\"choices\":[{\"delta\":{\"content\":\"2\"}}]}[DONE]ignored-after-done\n\n";
    ainiux::provider::ChatResult result;
    std::string streamed;
    const ainiux::Error err = run_chat_stream_from_body(body, result, streamed);
    check(err.ok(), "chat SSE parser accepts JSON payload immediately followed by DONE");
    check(result.content == "2", "JSON plus DONE payload accumulates content");
    check(streamed == "2", "JSON plus DONE payload forwards content");
}

void test_responses_sse_accepts_concatenated_json_payloads() {
    const std::string body =
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"Hello\"}"
        "{\"type\":\"response.output_text.delta\",\"delta\":\" world\"}"
        "{\"type\":\"response.completed\",\"response\":{\"model\":\"mock-model\",\"usage\":{\"input_tokens\":1,\"output_tokens\":2,\"total_tokens\":3}}}\n\n";
    ainiux::provider::ChatResult result;
    std::string streamed;
    const ainiux::Error err = run_responses_stream_from_body(body, result, streamed);
    check(err.ok(), "Responses SSE parser accepts concatenated JSON payloads");
    check(result.content == "Hello world", "concatenated Responses payloads accumulate deltas");
    check(streamed == "Hello world", "concatenated Responses payloads forward deltas");
    check(result.total_tokens == 3, "concatenated Responses payloads preserve completion metadata");
}

void test_explicit_chat_url_does_not_require_base_when_model_set() {
    const char* argv[] = {"ainiux", "--chat-url", "https://example.test/custom/chat", "-m", "model", "-p", "hello", "--header", "Authorization: Bearer test"};
    ainiux::cli::ParseResult parsed = ainiux::cli::parse_args(9, const_cast<char**>(argv));
    check(parsed.error.ok(), "explicit chat URL args parse");
    ainiux::provider::ContextResult ctx = ainiux::provider::build_context(parsed.options);
    check(ctx.error.ok(), "explicit chat URL context builds without base URL when model is set");
    check(ctx.context.chat_url == "https://example.test/custom/chat", "explicit chat URL is preserved");
}

void test_image_capability_detection() {
    ainiux::provider::RequestContext context;
    context.api_kind = ainiux::provider::ApiKind::ChatCompletions;
    context.profile.name = "custom_openai_chat";
    context.profile.capabilities.images = true;
    context.options.model = "Qwen3.5-35B-A3B";
    check(ainiux::provider::detected_capabilities_for(context).images,
          "Qwen3.5 model is detected as image capable");
    check(ainiux::provider::validate_image_input(context).ok(),
          "detected vision model accepts image input");

    context.options.model = "unknown-text-model";
    check(!ainiux::provider::validate_image_input(context).ok(),
          "unknown model requires an explicit image capability decision");
    context.options.image_capability = "allow";
    check(ainiux::provider::validate_image_input(context).ok(),
          "explicit image capability override allows a compatible unknown model");
    context.api_kind = ainiux::provider::ApiKind::Responses;
    check(!ainiux::provider::validate_image_input(context).ok(),
          "Responses image input remains rejected until its request schema is implemented");
}

void test_provider_lookup_metadata() {
    check(ainiux::provider::normalize_provider_key("LM-Studio") == "lm_studio",
          "provider key normalization lowercases and maps dashes");
    check(ainiux::provider::canonical_profile_name("lmstudio") == ainiux::provider::names::kLmStudio,
          "canonical profile name resolves aliases");
    check(ainiux::provider::canonical_profile_name("custom") == ainiux::provider::names::kCustomOpenAiChat,
          "canonical profile name resolves custom alias");

    const std::vector<ainiux::provider::Profile> profiles = ainiux::provider::built_in_profiles();
    const ainiux::provider::Profile* custom_profile = nullptr;
    const ainiux::provider::Profile* lm_studio_profile = nullptr;
    for (const ainiux::provider::Profile& profile : profiles) {
        if (profile.name == ainiux::provider::names::kCustomOpenAiChat) {
            custom_profile = &profile;
        } else if (profile.name == ainiux::provider::names::kLmStudio) {
            lm_studio_profile = &profile;
        }
    }
    check(custom_profile != nullptr && !ainiux::provider::is_selectable_provider(*custom_profile),
          "custom OpenAI-compatible profile is not selectable in provider pickers");
    check(lm_studio_profile != nullptr && ainiux::provider::is_selectable_provider(*lm_studio_profile),
          "lm_studio profile is selectable in provider pickers");
}

void test_lmstudio_context() {
    const char* argv[] = {"ainiux", "--provider", "lmstudio", "--list-models"};
    ainiux::cli::ParseResult parsed = ainiux::cli::parse_args(4, const_cast<char**>(argv));
    check(parsed.error.ok(), "lmstudio args parse");
    ainiux::provider::ContextResult ctx = ainiux::provider::build_context(parsed.options);
    check(ctx.error.ok(), "lmstudio context builds without key");
    check(ctx.context.profile.name == "lm_studio", "lmstudio alias normalized");
    check(ctx.context.base_url == "http://localhost:1234/v1", "lmstudio default base URL");
}

void test_lmstudio_shortcut_context() {
    const char* argv[] = {"ainiux", "lmstudio", "-i"};
    ainiux::cli::ParseResult parsed = ainiux::cli::parse_args(3, const_cast<char**>(argv));
    check(parsed.error.ok(), "lmstudio shortcut args parse");
    ainiux::provider::ContextResult ctx = ainiux::provider::build_context(parsed.options);
    check(ctx.error.ok(), "lmstudio shortcut context builds without key or model");
    check(ctx.context.profile.name == "lm_studio", "lmstudio shortcut selects profile");
    check(ctx.context.base_url == "http://localhost:1234/v1", "lmstudio shortcut uses default base URL");
    check(ctx.context.options.model.empty(), "lmstudio shortcut does not require model");
}

void test_none_provider_allows_an_empty_endpoint() {
    const char* argv[] = {"ainiux", "--provider", "none", "--repl"};
    ainiux::cli::ParseResult parsed = ainiux::cli::parse_args(4, const_cast<char**>(argv));
    check(parsed.error.ok(), "none provider parses without a positional endpoint");
    check(parsed.options.positional_url.empty(), "none provider keeps the omitted endpoint empty");
    check(ainiux::provider::validate_profile_name(parsed.options.provider).ok(),
          "none is a recognized provider name in standalone modes");

    ainiux::provider::ContextResult context = ainiux::provider::build_context(parsed.options);
    check(context.error.ok(), "none provider context builds without an endpoint");
    check(context.context.profile.name == "none" && context.context.profile.offline,
          "none resolves to the offline provider profile");
    check(context.context.base_url.empty() && context.context.chat_url.empty() &&
              context.context.responses_url.empty() && context.context.models_url.empty(),
          "none provider leaves every model endpoint empty");
    check(!ainiux::provider::capabilities_for(context.context).chat_completions &&
              !ainiux::provider::capabilities_for(context.context).model_listing,
          "none provider advertises no model capabilities");

    ainiux::provider::ModelsResult models;
    ainiux::Error err = ainiux::provider::list_models(context.context, models);
    check(err.code == ainiux::ErrorCode::UnsupportedFeature,
          "none provider rejects model listing before transport");

    ainiux::provider::ChatResult chat;
    err = ainiux::provider::send_chat_messages(
        context.context, {{"user", "hello"}},
        [](const std::string&) { return ainiux::ok_error(); }, chat);
    check(err.code == ainiux::ErrorCode::UnsupportedFeature,
          "none provider rejects chat before transport");

    const char* alias_argv[] = {"ainiux", "offline", "--repl"};
    ainiux::cli::ParseResult alias = ainiux::cli::parse_args(3, const_cast<char**>(alias_argv));
    ainiux::provider::ContextResult alias_context = ainiux::provider::build_context(alias.options);
    check(alias_context.error.ok() && alias_context.context.profile.name == "none",
          "offline positional alias resolves without an endpoint");

    const char* endpoint_argv[] = {
        "ainiux", "--provider", "none", "--base-url", "http://localhost:1234", "--repl"};
    ainiux::cli::ParseResult endpoint =
        ainiux::cli::parse_args(6, const_cast<char**>(endpoint_argv));
    ainiux::provider::ContextResult endpoint_context =
        ainiux::provider::build_context(endpoint.options);
    check(endpoint_context.error.code == ainiux::ErrorCode::BadArgs,
          "none provider rejects model endpoint overrides");
}

void test_parse_models_response_llama_server_hybrid() {
    const std::string body = R"json({
      "models":[{"name":"Gemma-4-26B-A4B","model":"Gemma-4-26B-A4B","type":"model"}],
      "object":"list",
      "data":[{"id":"Gemma-4-26B-A4B","aliases":["Gemma-4-26B-A4B"],"object":"model","created":1783852595,"owned_by":"llamacpp","meta":{"n_vocab":262144,"n_ctx":131072,"n_ctx_train":262144}}]
    })json";
    ainiux::provider::ModelsResult result;
    ainiux::Error err = ainiux::provider::parse_models_response(body, result);
    check(err.ok(), "llama-server hybrid models JSON parses");
    check(result.model_ids.size() == 1 && result.model_ids.front() == "Gemma-4-26B-A4B",
          "llama-server hybrid models JSON preserves model id");
    check(ainiux::provider::context_window_for_model(result, "Gemma-4-26B-A4B") == 131072,
          "llama-server hybrid models JSON exposes runtime context length");
    check(ainiux::provider::context_window_for_model(result, "definitely-not-a-model") < 0,
          "unknown llama-server model selector does not invent a context window");
}

void test_parse_models_response_llamacpp_meta() {
    const std::string body = R"json({
      "object": "list",
      "data": [{
        "id": "Gemma-4-26B-A4B",
        "aliases": ["Gemma-4-26B-A4B"],
        "object": "model",
        "created": 1783363072,
        "owned_by": "llamacpp",
        "meta": {
          "n_vocab": 262144,
          "n_ctx": 131072,
          "n_ctx_train": 262144,
          "n_embd": 2816,
          "n_params": 25233142046,
          "size": 14233222264
        }
      }]
    })json";
    ainiux::provider::ModelsResult result;
    ainiux::Error err = ainiux::provider::parse_models_response(body, result);
    check(err.ok(), "llama-server models JSON parses");
    check(result.model_ids.size() == 1 && result.model_ids.front() == "Gemma-4-26B-A4B",
          "llama-server models JSON preserves model id");
    check(!result.models.empty(), "llama-server models JSON stores model info");
    const auto& attrs = result.models.front().attributes;
    check(attrs.at("n_ctx") == "131072", "llama-server meta.n_ctx is flattened");
    check(attrs.at("n_ctx_train") == "262144", "llama-server meta.n_ctx_train is flattened");
    check(attrs.at("n_params") == "25233142046", "llama-server meta.n_params is flattened");

    const std::string markdown = ainiux::provider::format_models_markdown(
        "custom_openai_chat", "http://localhost:30000/v1/models", result);
    check(markdown.find("| Context |") != std::string::npos, "llama-server markdown includes context column");
    check(markdown.find("131,072 tokens") != std::string::npos,
          "llama-server markdown formats runtime context length");
    check(markdown.find("262,144 tokens") != std::string::npos,
          "llama-server markdown formats training context length");
    check(markdown.find("25.2B") != std::string::npos, "llama-server markdown formats parameter count");
    check(markdown.find("13.26 GiB") != std::string::npos, "llama-server markdown formats model size");
}

void test_model_context_window_tokens() {
    ainiux::provider::ModelInfo llama;
    llama.id = "Gemma-4-26B-A4B";
    llama.attributes = {{"n_ctx", "131072"}, {"n_ctx_train", "262144"}};
    check(ainiux::provider::model_context_window_tokens(llama) == 131072,
          "model context window prefers runtime n_ctx over training context");

    ainiux::provider::ModelsResult models;
    ainiux::provider::ModelInfo ollama;
    ollama.id = "llama3";
    ollama.attributes = {{"context_length", "8192"}};
    models.models.push_back(ollama);
    models.model_ids.push_back("llama3");
    ainiux::provider::ModelInfo deepseek;
    deepseek.id = "deepseek/deepseek-v4-flash";
    deepseek.attributes = {{"context_length", "1000000"}};
    models.models.push_back(deepseek);
    models.model_ids.push_back(deepseek.id);
    check(ainiux::provider::context_window_for_model(models, "llama3") == 8192,
          "model context window lookup matches model id");

    ainiux::provider::RequestContext context;
    context.options.model = "llama3";
    ainiux::provider::apply_context_window_from_models(context, models);
    check(context.options.context_tokens == 8192,
          "context window is applied from model metadata when not configured explicitly");

    context.options.model = "missing-model";
    ainiux::provider::apply_context_window_from_models(context, models);
    check(context.options.context_tokens == 0,
          "changing to an unknown model clears the prior automatic context window");

    context.options.model = "llama3";
    ainiux::provider::apply_context_window_from_models(context, models);
    check(context.options.context_tokens == 8192,
          "changing back refreshes the selected model context window");

    context.options.model = "deepseek/deepseek-v4-flash";
    ainiux::provider::apply_context_window_from_models(context, models);
    check(context.options.context_tokens == 1000000,
          "model changes replace the prior automatic window with the selected model's metadata");

    ainiux::provider::ModelsResult sparse_models;
    ainiux::provider::ModelInfo sparse_deepseek;
    sparse_deepseek.id = "deepseek/deepseek-v4-pro";
    sparse_deepseek.attributes = {{"object", "model"}, {"owned_by", "deepseek"}};
    sparse_models.models.push_back(sparse_deepseek);
    sparse_models.model_ids.push_back(sparse_deepseek.id);
    ainiux::ModelCapability deepseek_v4;
    deepseek_v4.id = "deepseek-v4";
    deepseek_v4.api = "chat";
    deepseek_v4.model_regex = "^deepseek-v4-(?:pro|flash)(?:[-.][a-z0-9]+)*$";
    deepseek_v4.context_window_tokens = 1000000;
    context.profile.name = "deepseek";
    context.options.model = "deepseek/deepseek-v4-pro";
    context.options.model_catalog.models.push_back(deepseek_v4);
    ainiux::provider::apply_context_window_from_models(context, sparse_models);
    check(context.options.context_tokens == 1000000,
          "catalog context window fills missing DeepSeek /models metadata");

    sparse_models.models.front().attributes["context_length"] = "750000";
    ainiux::provider::apply_context_window_from_models(context, sparse_models);
    check(context.options.context_tokens == 750000,
          "provider model metadata remains authoritative over the catalog fallback");

    context.options.has_context_tokens = true;
    context.options.context_tokens = 900000;
    ainiux::provider::apply_context_window_from_models(context, models);
    check(context.options.context_tokens == 900000,
          "explicit context window override survives model metadata refresh");

    ainiux::provider::ModelInfo aliased;
    aliased.id = "Gemma-4-26B-A4B";
    aliased.attributes = {{"n_ctx", "131072"}, {"aliases", "gemma-4, Gemma-4-26B-A4B"}};
    ainiux::provider::ModelsResult aliased_models;
    aliased_models.models.push_back(aliased);
    aliased_models.model_ids.push_back("Gemma-4-26B-A4B");
    check(ainiux::provider::context_window_for_model(aliased_models, "gemma-4") == 131072,
          "model context window lookup matches aliases");

    ainiux::provider::RequestContext wrong_requested;
    wrong_requested.options.model = "WrongModel";
    ainiux::provider::apply_context_window_from_models(wrong_requested, aliased_models, "Gemma-4-26B-A4B");
    check(wrong_requested.options.context_tokens == 131072,
          "model context window can be resolved from the provider-reported model id");
}

void test_models_markdown_format() {
    ainiux::provider::ModelsResult result;
    ainiux::provider::ModelInfo first;
    first.id = "mock-model";
    first.attributes = {{"object", "model"}, {"owned_by", "mock"}, {"created", "1686935002"}};
    result.models.push_back(first);
    result.model_ids.push_back("mock-model");

    const std::string markdown = ainiux::provider::format_models_markdown(
        "lm_studio", "http://localhost:1234/v1/models", result);
    check(markdown.find("# Models") != std::string::npos, "models markdown includes heading");
    check(markdown.find("**Provider:** lm_studio") != std::string::npos,
          "models markdown includes provider label");
    check(markdown.find("**Endpoint:** http://localhost:1234/v1/models") != std::string::npos,
          "models markdown includes endpoint label");
    check(markdown.find("| ID | Object | Owned by | Created |") != std::string::npos,
          "models markdown includes expected table columns");
    check(markdown.find("| mock-model | model | mock |") != std::string::npos,
          "models markdown includes model row values");
    check(markdown.find("2023-06-16 17:03:22 UTC (1686935002)") != std::string::npos,
          "models markdown formats created timestamps readably");

    ainiux::provider::ModelsResult empty;
    const std::string empty_markdown =
        ainiux::provider::format_models_markdown("openai", "https://api.openai.com/v1/models", empty);
    check(empty_markdown.find("_No models returned._") != std::string::npos,
          "models markdown reports an empty model list");
}

class ScopedUnsetenv {
   public:
    explicit ScopedUnsetenv(std::string name) : name_(std::move(name)) {
        previous_value_ = ainiux::test::test_environment(name_.c_str());
        ainiux::test::unset_test_environment(name_.c_str());
    }

    ~ScopedUnsetenv() {
        if (previous_value_.has_value())
            ainiux::test::set_test_environment(name_.c_str(), *previous_value_);
        else
            ainiux::test::unset_test_environment(name_.c_str());
    }

   private:
    std::string name_;
    std::optional<std::string> previous_value_;
};

void test_apply_provider_target_accepts_custom_url() {
    ainiux::cli::Options options;
    options.tui = true;
    options.provider = "lm_studio";
    options.model = "local-model";
    ainiux::provider::apply_provider_target(options, "http://localhost:30000");
    check(options.positional_url == "http://localhost:30000",
          "provider target helper stores custom API URLs as positional endpoints");
    check(options.provider == "openai",
          "provider target helper keeps the default openai placeholder for custom URL routing");
    check(options.base_url.empty(), "provider target helper clears stale base URLs");

    ainiux::provider::ContextResult context = ainiux::provider::build_context(options);
    check(context.error.ok(), "custom URL provider target builds a chat context");
    check(context.context.profile.name == "custom_openai_chat",
          "custom URL provider target selects the custom OpenAI-compatible profile");
    check(context.context.base_url.find("localhost:30000") != std::string::npos,
          "custom URL provider target normalizes the requested endpoint");
}

void test_tui_local_endpoint_auto_selects_model() {
    const char* argv[] = {"ainiux", "http://localhost:30000", "--chat"};
    ainiux::cli::ParseResult parsed = ainiux::cli::parse_args(3, const_cast<char**>(argv));
    check(parsed.error.ok(), "local URL chat UI args parse");
    ainiux::provider::ContextResult context = ainiux::provider::build_context(parsed.options);
    check(context.error.ok(), "local URL chat UI context builds");
    check(ainiux::provider::needs_interactive_model_selection(context.context),
          "local custom URL chat UI discovers models before choosing among them");
    check(context.context.base_url.find("localhost:30000") != std::string::npos,
          "local URL chat UI keeps the requested endpoint");

    ainiux::cli::Options bare_chat;
    bare_chat.tui = true;
    ainiux::provider::apply_tui_startup_default(bare_chat);
    ainiux::provider::ContextResult offline = ainiux::provider::build_context(bare_chat);
    check(offline.error.ok(), "bare chat UI offline context builds");
    check(!ainiux::provider::needs_interactive_model_selection(offline.context),
          "bare chat offline placeholder waits for provider selection before model discovery");

    const char* editor_argv[] = {"ainiux", "http://localhost:30000", "--editor"};
    ainiux::cli::ParseResult editor_parsed = ainiux::cli::parse_args(3, const_cast<char**>(editor_argv));
    check(editor_parsed.error.ok(), "local URL editor args parse");
    ainiux::provider::ContextResult editor_context = ainiux::provider::build_context(editor_parsed.options);
    check(editor_context.error.ok(), "local URL editor context builds");
    check(ainiux::provider::needs_interactive_model_selection(editor_context.context),
          "local custom URL editor discovers models before choosing among them");

    editor_context.context.options.model = "configured-model";
    check(!ainiux::provider::needs_interactive_model_selection(editor_context.context),
          "interactive startup skips model discovery when a model was supplied");
}

void test_tui_startup_provider_selection_helpers() {
    ainiux::cli::Options bare_chat;
    bare_chat.tui = true;
    check(ainiux::provider::tui_needs_startup_provider_selection(bare_chat),
          "bare chat UI requests startup provider selection");

    ainiux::provider::apply_tui_startup_default(bare_chat);
    check(bare_chat.provider == "none",
          "bare chat UI uses an offline placeholder while its provider picker is open");

    ainiux::cli::Options model_only_chat;
    model_only_chat.tui = true;
    model_only_chat.model = "remembered-model";
    check(ainiux::provider::tui_needs_startup_provider_selection(model_only_chat),
          "chat with a model but no selected provider still requests provider selection");

    ainiux::cli::Options explicit_provider;
    explicit_provider.tui = true;
    explicit_provider.provider = "openai";
    explicit_provider.provider_explicit = true;
    check(!ainiux::provider::tui_needs_startup_provider_selection(explicit_provider),
          "explicit chat provider skips startup provider selection");

    ainiux::cli::Options positional_provider;
    positional_provider.tui = true;
    positional_provider.positional_url = "lmstudio";
    check(!ainiux::provider::tui_needs_startup_provider_selection(positional_provider),
          "positional chat provider shortcut skips startup provider selection");

    ainiux::cli::Options bare_agent;
    bare_agent.agent = true;
    check(ainiux::provider::tui_needs_startup_provider_selection(bare_agent),
          "bare agent requests startup provider selection");
    ainiux::provider::apply_tui_startup_default(bare_agent);
    check(bare_agent.provider == "none",
          "bare agent starts offline while the provider picker is open");

    ainiux::cli::Options model_only_agent;
    model_only_agent.agent = true;
    model_only_agent.model = "remembered-model";
    check(ainiux::provider::tui_needs_startup_provider_selection(model_only_agent),
          "agent with a model but no selected provider still requests provider selection");

    ainiux::cli::Options restored_agent;
    restored_agent.agent = true;
    restored_agent.provider = "deepseek";
    restored_agent.model = "deepseek-chat";
    restored_agent.agent_project_settings_restored = true;
    check(!ainiux::provider::tui_needs_startup_provider_selection(restored_agent),
          "agent with restored project settings skips provider selection");
}

void test_editor_startup_local_only_default() {
    ainiux::cli::Options bare_editor;
    bare_editor.editor = true;
    check(ainiux::provider::editor_needs_local_only_default(bare_editor),
          "bare editor requests local-only startup without provider or model");

    ainiux::provider::apply_editor_startup_default(bare_editor);
    check(bare_editor.provider == "none", "bare editor startup default uses offline provider");

    ainiux::cli::Options with_model;
    with_model.editor = true;
    with_model.model = "gpt-test";
    check(!ainiux::provider::editor_needs_local_only_default(with_model),
          "editor with an explicit model keeps AI configuration enabled");

    ainiux::cli::Options with_provider;
    with_provider.editor = true;
    with_provider.provider = "openai";
    with_provider.provider_explicit = true;
    check(!ainiux::provider::editor_needs_local_only_default(with_provider),
          "editor with an explicit provider keeps AI configuration enabled");

    ainiux::cli::Options configured_provider;
    configured_provider.editor = true;
    configured_provider.provider = "openrouter";
    check(!ainiux::provider::editor_needs_local_only_default(configured_provider),
          "editor with a configured online provider keeps AI configuration enabled");
}

void test_editor_defaults_offline_without_credentials() {
    ScopedUnsetenv unset_openai("OPENAI_API_KEY");
    ScopedUnsetenv unset_ainiux("AINIUX_API_KEY");

    const char* argv[] = {"ainiux", "--editor"};
    ainiux::cli::ParseResult parsed = ainiux::cli::parse_args(2, const_cast<char**>(argv));
    check(parsed.error.ok(), "bare editor args parse");
    ainiux::cli::Options options = parsed.options;
    ainiux::provider::apply_editor_startup_default(options);
    ainiux::provider::apply_editor_offline_default(options);
    check(options.provider == "none", "bare editor switches to offline provider without credentials");
    ainiux::provider::ContextResult context = ainiux::provider::build_context(options);
    check(context.error.ok() && context.context.profile.offline,
          "bare editor context builds offline without API credentials");

    const char* explicit_argv[] = {"ainiux", "--provider", "openai", "--editor"};
    ainiux::cli::ParseResult explicit_parsed =
        ainiux::cli::parse_args(4, const_cast<char**>(explicit_argv));
    check(explicit_parsed.error.ok(), "explicit openai editor args parse");
    ainiux::cli::Options explicit_options = explicit_parsed.options;
    ainiux::provider::apply_editor_offline_default(explicit_options);
    check(explicit_options.provider == "openai",
          "explicit provider editor keeps the requested provider without credentials");
    ainiux::provider::ContextResult explicit_context =
        ainiux::provider::build_context(explicit_options);
    check(explicit_context.error.code == ainiux::ErrorCode::Config,
          "explicit openai editor still requires credentials");

    ainiux::cli::Options configured_openrouter;
    configured_openrouter.editor = true;
    configured_openrouter.provider = "openrouter";
    ainiux::provider::apply_editor_offline_default(configured_openrouter);
    check(configured_openrouter.provider == "openrouter",
          "configured openrouter editor keeps provider without credentials");
}

void test_editor_model_selection_restore_requires_available_endpoint() {
    ainiux::cli::Options configured;
    ainiux::provider::ModelSelection custom{
        "custom_openai_chat",
        "remembered-model",
        "chat",
        ainiux::ReasoningSelection::named("high"),
    };
    check(!ainiux::provider::can_restore_model_selection(configured, custom),
          "editor does not restore a custom provider after its endpoint disappears");

    configured.base_url = "http://127.0.0.1:1234/v1";
    check(ainiux::provider::can_restore_model_selection(configured, custom),
          "editor restores a custom provider when its endpoint remains configured");

    configured = {};
    custom.provider = "lmstudio";
    check(ainiux::provider::can_restore_model_selection(configured, custom),
          "editor restores a provider with a built-in endpoint");

    custom.provider = "none";
    custom.model.clear();
    check(ainiux::provider::can_restore_model_selection(configured, custom),
          "editor restores the explicit offline selection without an endpoint");
}

void test_cli_target_change_clears_stale_remembered_model() {
    // Simulate editor_model_selection restore (remote Gemini) then CLI `vllm -e`.
    ainiux::cli::Options remembered;
    remembered.provider = "openrouter";
    remembered.model = "google/gemini-3.1-flash-preview";
    remembered.api = "chat";
    remembered.base_url = "https://openrouter.ai/api/v1";
    remembered.chat_url = "https://openrouter.ai/api/v1/chat/completions";
    remembered.models_url = "https://openrouter.ai/api/v1/models";
    remembered.reasoning = ainiux::ReasoningSelection::named("high");
    remembered.reasoning_explicit = true;

    const char* vllm_editor_argv[] = {"ainiux", "vllm", "-e"};
    ainiux::cli::ParseResult parsed =
        ainiux::cli::parse_args(3, const_cast<char**>(vllm_editor_argv), remembered);
    check(parsed.error.ok(), "vllm editor args with remembered remote model parse");
    check(parsed.options.model == "google/gemini-3.1-flash-preview",
          "parse keeps inherited model until CLI target-change policy runs");
    check(parsed.options.positional_url == "vllm", "vllm editor shortcut is positional");

    const bool positional_changed =
        ainiux::provider::canonical_profile_name(parsed.options.positional_url) !=
        ainiux::provider::canonical_profile_name(remembered.provider);
    check(positional_changed, "vllm positional differs from remembered openrouter");
    ainiux::provider::apply_cli_target_change(parsed.options, remembered, positional_changed);
    check(parsed.options.model.empty(),
          "CLI provider change clears a remembered remote model so local discovery can run");
    check(parsed.options.reasoning.is_auto(),
          "CLI provider change resets reasoning when --reasoning was not given");
    check(parsed.options.base_url.empty() && parsed.options.chat_url.empty() &&
              parsed.options.models_url.empty(),
          "CLI provider change clears endpoints restored for the previous provider");

    ainiux::provider::ContextResult context = ainiux::provider::build_context(parsed.options);
    check(context.error.ok(), "vllm editor context builds after clearing stale model");
    check(context.context.profile.name == "vllm", "vllm editor selects the local profile");
    check(context.context.profile.local_endpoint, "vllm editor is a local endpoint");
    check(context.context.options.model.empty(), "vllm editor model stays empty for discovery");
    check(ainiux::provider::needs_interactive_model_selection(context.context),
          "vllm editor without a model requires interactive model discovery like chat");

    // Same local provider again: keep the remembered local model.
    ainiux::cli::Options remembered_vllm;
    remembered_vllm.provider = "vllm";
    remembered_vllm.model = "local-served-model";
    remembered_vllm.api = "chat";
    ainiux::cli::ParseResult same =
        ainiux::cli::parse_args(3, const_cast<char**>(vllm_editor_argv), remembered_vllm);
    check(same.error.ok(), "vllm editor args with remembered local model parse");
    const bool same_positional_changed =
        ainiux::provider::canonical_profile_name(same.options.positional_url) !=
        ainiux::provider::canonical_profile_name(remembered_vllm.provider);
    check(!same_positional_changed, "same vllm positional does not count as a target change");
    ainiux::provider::apply_cli_target_change(same.options, remembered_vllm,
                                              same_positional_changed);
    check(same.options.model == "local-served-model",
          "restarting the same local provider keeps its remembered model");

    // Explicit --provider change also clears without -m.
    ainiux::cli::Options remembered_xai;
    remembered_xai.provider = "xai";
    remembered_xai.model = "grok-something";
    const char* explicit_provider_argv[] = {"ainiux", "--provider", "vllm", "--editor"};
    ainiux::cli::ParseResult explicit_parsed =
        ainiux::cli::parse_args(4, const_cast<char**>(explicit_provider_argv), remembered_xai);
    check(explicit_parsed.error.ok(), "explicit --provider vllm editor args parse");
    ainiux::provider::apply_cli_target_change(explicit_parsed.options, remembered_xai, false);
    check(explicit_parsed.options.model.empty(),
          "explicit --provider change clears a remembered model without --model");

    const char* explicit_endpoint_argv[] = {
        "ainiux", "deepseek", "-a", "--base-url", "https://gateway.example/v1",
    };
    ainiux::cli::ParseResult explicit_endpoint = ainiux::cli::parse_args(
        5, const_cast<char**>(explicit_endpoint_argv), remembered);
    check(explicit_endpoint.error.ok(),
          "agent provider shortcut with an explicit replacement endpoint parses");
    ainiux::provider::apply_cli_target_change(explicit_endpoint.options, remembered,
                                              true);
    check(explicit_endpoint.options.base_url ==
              "https://gateway.example/v1" &&
              explicit_endpoint.options.chat_url.empty() &&
              explicit_endpoint.options.models_url.empty(),
          "explicit replacement base URL survives while inherited endpoint URLs clear");

    ainiux::cli::Options mixed_agent = remembered;
    mixed_agent.provider = "deepseek";
    mixed_agent.model = "google/gemini-3.5-flash-lite";
    mixed_agent.key = "test-key";
    const char* repeated_deepseek_argv[] = {"ainiux", "deepseek", "-a"};
    ainiux::cli::ParseResult repeated_deepseek = ainiux::cli::parse_args(
        3, const_cast<char**>(repeated_deepseek_argv), mixed_agent);
    check(repeated_deepseek.error.ok(),
          "agent parses an explicit provider matching corrupted restored metadata");
    ainiux::provider::apply_cli_target_change(
        repeated_deepseek.options, mixed_agent, false);
    check(repeated_deepseek.options.base_url.empty() &&
              repeated_deepseek.options.model.empty(),
          "explicit provider repairs a mismatched restored base URL and model");
    ainiux::provider::ContextResult repaired_deepseek =
        ainiux::provider::build_context(repeated_deepseek.options);
    check(repaired_deepseek.error.ok() &&
              repaired_deepseek.context.profile.name == "deepseek" &&
              repaired_deepseek.context.base_url ==
                  "https://api.deepseek.com/v1",
          "repaired agent provider uses the official DeepSeek base URL");

    // Explicit -m wins over the clear policy.
    const char* with_model_argv[] = {
        "ainiux", "vllm", "-e", "-m", "override-model",
    };
    ainiux::cli::ParseResult with_model =
        ainiux::cli::parse_args(5, const_cast<char**>(with_model_argv), remembered);
    check(with_model.error.ok(), "vllm editor with explicit model parses");
    ainiux::provider::apply_cli_target_change(with_model.options, remembered, true);
    check(with_model.options.model == "override-model",
          "explicit -m is preserved when the provider changes");
}

void test_openai_context_allows_missing_model() {
    const char* argv[] = {"ainiux", "--provider", "openai", "-p", "hello", "--header", "Authorization: Bearer test"};
    ainiux::cli::ParseResult parsed = ainiux::cli::parse_args(7, const_cast<char**>(argv));
    check(parsed.error.ok(), "openai args without model parse");
    ainiux::provider::ContextResult ctx = ainiux::provider::build_context(parsed.options);
    check(ctx.error.ok(), "openai context builds without model so caller can discover one");
    check(ctx.context.options.model.empty(), "openai context keeps missing model empty before discovery");
}

void test_openrouter_shortcut_context() {
    const char* argv[] = {"ainiux", "openrouter", "-model", "provider/model", "-p", "hello", "--header", "Authorization: Bearer test"};
    ainiux::cli::ParseResult parsed = ainiux::cli::parse_args(8, const_cast<char**>(argv));
    check(parsed.error.ok(), "openrouter shortcut args parse");
    ainiux::provider::ContextResult ctx = ainiux::provider::build_context(parsed.options);
    check(ctx.error.ok(), "openrouter shortcut context builds with auth header");
    check(ctx.context.profile.name == "openrouter", "openrouter shortcut selects profile");
    check(ctx.context.base_url == "https://openrouter.ai/api/v1", "openrouter shortcut uses standard base URL");
}

void test_provider_capabilities_and_responses_context() {
    const char* argv[] = {"ainiux", "--provider", "openai", "--api", "responses", "-p", "hello", "--header", "Authorization: Bearer test"};
    ainiux::cli::ParseResult parsed = ainiux::cli::parse_args(9, const_cast<char**>(argv));
    check(parsed.error.ok(), "OpenAI Responses args parse");
    ainiux::provider::ContextResult ctx = ainiux::provider::build_context(parsed.options);
    check(ctx.error.ok(), "OpenAI Responses context builds");
    check(ctx.context.api_kind == ainiux::provider::ApiKind::Responses, "Responses API kind selected");
    check(ainiux::provider::active_request_url(ctx.context) == "https://api.openai.com/v1/responses",
          "OpenAI Responses endpoint selected");
    check(ainiux::provider::capabilities_for(ctx.context).responses_api, "OpenAI reports Responses capability");
    check(ainiux::provider::capabilities_for(ctx.context).chat_completions, "OpenAI reports Chat Completions capability");

    const char* shortcut_argv[] = {"ainiux", "--provider", "openai_responses", "-p", "hello", "--header", "Authorization: Bearer test"};
    ainiux::cli::ParseResult shortcut = ainiux::cli::parse_args(7, const_cast<char**>(shortcut_argv));
    check(shortcut.error.ok(), "openai_responses profile shortcut args parse");
    ainiux::provider::ContextResult shortcut_ctx = ainiux::provider::build_context(shortcut.options);
    check(shortcut_ctx.error.ok(), "openai_responses context builds");
    check(shortcut_ctx.context.profile.name == "openai", "openai_responses uses OpenAI profile");
    check(shortcut_ctx.context.api_kind == ainiux::provider::ApiKind::Responses, "openai_responses selects Responses API");
}

void test_provider_registry_resolves_added_profiles() {
    std::vector<ainiux::provider::Profile> profiles = ainiux::provider::built_in_profiles();
    check(profiles.size() >= 25, "provider registry includes offline and compatibility profiles");

    // Offline first, then openrouter, openai; local servers after kimi in fixed order.
    check(!profiles.empty() && profiles[0].name == "none", "offline profile is first");
    check(profiles.size() > 2 && profiles[1].name == "openrouter" && profiles[2].name == "openai",
          "openrouter is listed before openai");
    size_t moonshot_idx = profiles.size();
    size_t llama_idx = profiles.size();
    size_t lm_idx = profiles.size();
    size_t ollama_idx = profiles.size();
    size_t vllm_idx = profiles.size();
    size_t sglang_idx = profiles.size();
    for (size_t i = 0; i < profiles.size(); ++i) {
        if (profiles[i].name == "moonshot") {
            moonshot_idx = i;
        } else if (profiles[i].name == "llamacpp") {
            llama_idx = i;
        } else if (profiles[i].name == "lm_studio") {
            lm_idx = i;
        } else if (profiles[i].name == "ollama") {
            ollama_idx = i;
        } else if (profiles[i].name == "vllm") {
            vllm_idx = i;
        } else if (profiles[i].name == "sglang") {
            sglang_idx = i;
        }
    }
    check(moonshot_idx < llama_idx && llama_idx + 1 == lm_idx && lm_idx + 1 == ollama_idx &&
              ollama_idx + 1 == vllm_idx && vllm_idx + 1 == sglang_idx,
          "local providers follow kimi as llamacpp, lm_studio, ollama, vllm, sglang");

    const char* grok_argv[] = {"ainiux", "grok", "--list-models", "--header", "Authorization: Bearer test"};
    ainiux::cli::ParseResult grok = ainiux::cli::parse_args(5, const_cast<char**>(grok_argv));
    check(grok.error.ok(), "grok alias args parse");
    ainiux::provider::ContextResult grok_ctx = ainiux::provider::build_context(grok.options);
    check(grok_ctx.error.ok(), "grok alias context builds");
    check(grok_ctx.context.profile.name == "xai", "grok alias resolves to xai");
    check(grok_ctx.context.base_url == "https://api.x.ai/v1", "xai base URL selected");

    const char* kimi_argv[] = {"ainiux", "--provider", "kimi", "--list-models", "--header", "Authorization: Bearer test"};
    ainiux::cli::ParseResult kimi = ainiux::cli::parse_args(6, const_cast<char**>(kimi_argv));
    check(kimi.error.ok(), "kimi alias args parse");
    ainiux::provider::ContextResult kimi_ctx = ainiux::provider::build_context(kimi.options);
    check(kimi_ctx.error.ok(), "kimi alias context builds");
    check(kimi_ctx.context.profile.name == "moonshot", "kimi alias resolves to moonshot");

    const char* llama_argv[] = {"ainiux", "llama.cpp", "--list-models"};
    ainiux::cli::ParseResult llama = ainiux::cli::parse_args(3, const_cast<char**>(llama_argv));
    check(llama.error.ok(), "llama.cpp alias args parse");
    ainiux::provider::ContextResult llama_ctx = ainiux::provider::build_context(llama.options);
    check(llama_ctx.error.ok(), "llama.cpp alias context builds");
    check(llama_ctx.context.profile.name == "llamacpp", "llama.cpp alias resolves to llamacpp");
    check(llama_ctx.context.profile.local_endpoint, "llamacpp is marked local");

    const char* vllm_argv[] = {"ainiux", "vllm", "--list-models"};
    ainiux::cli::ParseResult vllm = ainiux::cli::parse_args(3, const_cast<char**>(vllm_argv));
    check(vllm.error.ok(), "vllm shortcut args parse");
    ainiux::provider::ContextResult vllm_ctx = ainiux::provider::build_context(vllm.options);
    check(vllm_ctx.error.ok(), "vllm context builds");
    check(vllm_ctx.context.api_key == "token-abc123", "vllm uses configured dummy API key");

    const char* sglang_argv[] = {"ainiux", "sglang", "--list-models"};
    ainiux::cli::ParseResult sglang = ainiux::cli::parse_args(3, const_cast<char**>(sglang_argv));
    check(sglang.error.ok(), "sglang shortcut args parse");
    ainiux::provider::ContextResult sglang_ctx = ainiux::provider::build_context(sglang.options);
    check(sglang_ctx.error.ok(), "sglang context builds");
    check(sglang_ctx.context.profile.name == "sglang", "sglang profile selected");
    check(sglang_ctx.context.base_url == "http://localhost:30000/v1", "sglang defaults to localhost:30000");
    check(sglang_ctx.context.profile.local_endpoint, "sglang is marked local");
    check(ainiux::provider::needs_interactive_model_selection(sglang_ctx.context),
          "sglang without a model requires interactive model discovery");

    const char* deepinfra_argv[] = {"ainiux", "--provider", "deepinfra", "--list-models", "--header", "Authorization: Bearer test"};
    ainiux::cli::ParseResult deepinfra = ainiux::cli::parse_args(6, const_cast<char**>(deepinfra_argv));
    check(deepinfra.error.ok(), "deepinfra args parse");
    ainiux::provider::ContextResult deepinfra_ctx = ainiux::provider::build_context(deepinfra.options);
    check(deepinfra_ctx.error.ok(), "deepinfra context builds");
    check(deepinfra_ctx.context.profile.key_envs.size() >= 2 && deepinfra_ctx.context.profile.key_envs[1] == "DEEPINFRA_TOKEN",
          "deepinfra registers alternate token env var");

    const char* zai_argv[] = {"ainiux", "--provider", "z.ai", "--list-models", "--header", "Authorization: Bearer test"};
    ainiux::cli::ParseResult zai = ainiux::cli::parse_args(6, const_cast<char**>(zai_argv));
    check(zai.error.ok(), "Z.AI alias args parse");
    ainiux::provider::ContextResult zai_ctx = ainiux::provider::build_context(zai.options);
    check(zai_ctx.error.ok(), "Z.AI alias context builds");
    check(zai_ctx.context.profile.name == "zai", "z.ai alias resolves to zai");
    check(zai_ctx.context.base_url == "https://api.z.ai/api/paas/v4", "Z.AI base URL selected");
    check(zai_ctx.context.chat_url == "https://api.z.ai/api/paas/v4/chat/completions",
          "Z.AI Chat Completions URL selected");
    ainiux::provider::ModelsResult zai_models;
    ainiux::Error zai_models_error = ainiux::provider::list_models(zai_ctx.context, zai_models);
    check(zai_models_error.code == ainiux::ErrorCode::UnsupportedFeature,
          "Z.AI rejects undocumented model listing before transport");

    const char* qwen_argv[] = {"ainiux", "qwen", "--list-models", "--header", "Authorization: Bearer test"};
    ainiux::cli::ParseResult qwen = ainiux::cli::parse_args(5, const_cast<char**>(qwen_argv));
    check(qwen.error.ok(), "Qwen shortcut args parse");
    ainiux::provider::ContextResult qwen_ctx = ainiux::provider::build_context(qwen.options);
    check(qwen_ctx.error.ok(), "Qwen context builds");
    check(qwen_ctx.context.profile.name == "qwen", "Qwen shortcut resolves to qwen");
    check(qwen_ctx.context.base_url == "https://dashscope-intl.aliyuncs.com/compatible-mode/v1",
          "Qwen selects the global Model Studio base URL");
    check(qwen_ctx.context.models_url ==
              "https://dashscope-intl.aliyuncs.com/compatible-mode/v1/models",
          "Qwen model-list URL selected");
    check(qwen_ctx.context.profile.key_envs.front() == "DASHSCOPE_API_KEY",
          "Qwen uses the documented Model Studio key environment variable");
}

void test_provider_responses_unsupported_and_override() {
    const char* unsupported_argv[] = {"ainiux", "--provider", "openrouter", "--api", "responses", "-p", "hello", "--header", "Authorization: Bearer test"};
    ainiux::cli::ParseResult unsupported = ainiux::cli::parse_args(9, const_cast<char**>(unsupported_argv));
    check(unsupported.error.ok(), "unsupported Responses args parse");
    ainiux::provider::ContextResult unsupported_ctx = ainiux::provider::build_context(unsupported.options);
    check(!unsupported_ctx.error.ok(), "chat-only provider rejects built-in Responses API");
    check(unsupported_ctx.error.code == ainiux::ErrorCode::UnsupportedFeature, "Responses rejection uses unsupported feature error");

    const char* override_argv[] = {"ainiux", "--provider", "openrouter", "--api", "responses", "--responses-url", "https://example.test/v1/responses", "-p", "hello", "--header", "Authorization: Bearer test"};
    ainiux::cli::ParseResult override = ainiux::cli::parse_args(11, const_cast<char**>(override_argv));
    check(override.error.ok(), "Responses override args parse");
    ainiux::provider::ContextResult override_ctx = ainiux::provider::build_context(override.options);
    check(override_ctx.error.ok(), "Responses override context builds");
    check(override_ctx.context.responses_url == "https://example.test/v1/responses", "Responses override endpoint selected");
    check(ainiux::provider::capabilities_for(override_ctx.context).responses_api, "Responses override reports capability");
}

void test_provider_reasoning_request_compatibility() {
    const auto context_for = [](ainiux::ReasoningProtocol protocol,
                                const ainiux::ReasoningSelection& selection,
                                ainiux::provider::ApiKind api) {
        ainiux::provider::RequestContext context;
        context.profile.name = "custom_openai_chat";
        context.api_kind = api;
        context.options.model = "mock-model";
        context.options.reasoning = selection;
        context.options.reasoning_explicit = !selection.is_auto();
        ainiux::ModelCapability capability;
        capability.id = "test-capability";
        capability.provider = "any";
        capability.api = "any";
        capability.model_regex = "^mock-model$";
        capability.priority = 100;
        capability.reasoning_protocol = protocol;
        capability.load_order = 1;
        context.options.model_catalog.models.push_back(capability);
        return context;
    };
    const auto chat_context = [&](ainiux::ReasoningProtocol protocol,
                                  const ainiux::ReasoningSelection& selection) {
        return context_for(protocol,
                           selection,
                           ainiux::provider::ApiKind::ChatCompletions);
    };

    ainiux::provider::RequestContext context =
        chat_context(ainiux::ReasoningProtocol::OpenAiEffort,
                     ainiux::ReasoningSelection::automatic());
    ainiux::json::Value request = serialized_request_json(context);
    check(field(request, "reasoning_effort") == nullptr &&
              field(request, "reasoning") == nullptr,
          "Auto omits reasoning fields");

    context = chat_context(ainiux::ReasoningProtocol::DeepSeek,
                           ainiux::ReasoningSelection::named("high"));
    context.profile.name = "openrouter";
    check(ainiux::provider::matched_model_capability(context) != nullptr,
          "OpenRouter transport matches a provider-neutral model family");
    request = serialized_request_json(context);
    const ainiux::json::Value* routed_reasoning = field(request, "reasoning");
    check_string_field(routed_reasoning, "effort", "high",
                       "OpenRouter transport keeps the OpenRouter reasoning wire format");
    check(field(request, "thinking") == nullptr &&
              field(request, "reasoning_effort") == nullptr,
          "OpenRouter does not use the matched model's native reasoning fields");

    context = chat_context(ainiux::ReasoningProtocol::OpenAiEffort,
                           ainiux::ReasoningSelection::named("ultra"));
    request = serialized_request_json(context);
    check_string_field(request, "reasoning_effort", "ultra",
                       "OpenAI Chat passes an unknown named effort through unchanged");

    context = context_for(ainiux::ReasoningProtocol::OpenAiEffort,
                          ainiux::ReasoningSelection::token_budget(4096),
                          ainiux::provider::ApiKind::Responses);
    request = serialized_request_json(context);
    const ainiux::json::Value* reasoning = field(request, "reasoning");
    check_number_field(reasoning, "effort", 4096.0,
                       "OpenAI Responses preserves an exact numeric selection without approximation");

    context = chat_context(ainiux::ReasoningProtocol::OpenRouter,
                           ainiux::ReasoningSelection::token_budget(2048));
    request = serialized_request_json(context);
    reasoning = field(request, "reasoning");
    check_number_field(reasoning, "max_tokens", 2048.0,
                       "OpenRouter uses exact reasoning.max_tokens budgets");
    context.options.reasoning = ainiux::ReasoningSelection::named("xhigh");
    request = serialized_request_json(context);
    check_string_field(field(request, "reasoning"), "effort", "xhigh",
                       "OpenRouter preserves named efforts");

    context = chat_context(ainiux::ReasoningProtocol::GenericThinking,
                           ainiux::ReasoningSelection::named("ultra"));
    request = serialized_request_json(context);
    check_bool_field(request, "enable_thinking", true,
                     "generic reasoning enables thinking for a named value");
    check_string_field(request, "thinking_budget", "ultra",
                       "generic reasoning passes an unknown value through");

    context = chat_context(ainiux::ReasoningProtocol::GeminiEffort,
                           ainiux::ReasoningSelection::token_budget(8192));
    request = serialized_request_json(context);
    check_number_field(request, "reasoning_effort", 8192.0,
                       "Gemini effort protocol preserves an exact manual value");

    context = chat_context(ainiux::ReasoningProtocol::GeminiThinkingLevel,
                           ainiux::ReasoningSelection::named("high"));
    request = serialized_request_json(context);
    const ainiux::json::Value* generation_config =
        field(request, "generation_config");
    const ainiux::json::Value* thinking_config = generation_config;
    check_string_field(thinking_config, "thinking_level", "high",
                       "Gemini native protocol uses generation_config.thinking_level");

    context = chat_context(ainiux::ReasoningProtocol::GemmaThinkingLevel,
                           ainiux::ReasoningSelection::named("enabled"));
    request = serialized_request_json(context);
    const ainiux::json::Value* chat_template_kwargs =
        field(request, "chat_template_kwargs");
    check_bool_field(chat_template_kwargs, "enable_thinking", true,
                     "Gemma enables thinking through chat template kwargs");
    context.options.reasoning = ainiux::ReasoningSelection::named("none");
    request = serialized_request_json(context);
    check_bool_field(field(request, "chat_template_kwargs"), "enable_thinking", false,
                     "Gemma disables thinking through chat template kwargs");

    context = chat_context(ainiux::ReasoningProtocol::AnthropicBudget,
                           ainiux::ReasoningSelection::token_budget(2048));
    request = serialized_request_json(context);
    const ainiux::json::Value* thinking = field(request, "thinking");
    check_string_field(thinking, "type", "enabled",
                       "Anthropic exact budgets enable thinking");
    check_number_field(thinking, "budget_tokens", 2048.0,
                       "Anthropic exact budgets are not converted");
    context.options.reasoning = ainiux::ReasoningSelection::named("ultra");
    request = serialized_request_json(context);
    check_string_field(field(request, "thinking"), "budget_tokens", "ultra",
                       "Anthropic passes an unknown manual budget to the server");
    context.options.reasoning = ainiux::ReasoningSelection::named("none");
    request = serialized_request_json(context);
    check_string_field(field(request, "thinking"), "type", "disabled",
                       "Anthropic maps a documented disable value to disabled thinking");

    context = chat_context(ainiux::ReasoningProtocol::ThinkingToggle,
                           ainiux::ReasoningSelection::named("ultra"));
    request = serialized_request_json(context);
    check_string_field(field(request, "thinking"), "type", "ultra",
                       "toggle protocol forwards an unknown named value");
    context.options.reasoning = ainiux::ReasoningSelection::named("off");
    request = serialized_request_json(context);
    check_string_field(field(request, "thinking"), "type", "disabled",
                       "toggle protocol recognizes disable values");

    context = chat_context(ainiux::ReasoningProtocol::OpenAiEffort,
                           ainiux::ReasoningSelection::named("off"));
    request = serialized_request_json(context);
    check_string_field(request, "reasoning_effort", "none",
                       "unmatched semantic off uses OpenAI's disabled effort shape");
    context = chat_context(ainiux::ReasoningProtocol::OpenRouter,
                           ainiux::ReasoningSelection::named("off"));
    request = serialized_request_json(context);
    check_bool_field(field(request, "reasoning"), "enabled", false,
                     "unmatched semantic off uses OpenRouter's disabled object");
    context = chat_context(ainiux::ReasoningProtocol::Hy3Template,
                           ainiux::ReasoningSelection::named("off"));
    request = serialized_request_json(context);
    check_string_field(field(field(request, "extra_body"), "chat_template_kwargs"),
                       "reasoning_effort", "no_think",
                       "unmatched semantic off uses HY3's no_think value");

    context = chat_context(ainiux::ReasoningProtocol::QwenChat,
                           ainiux::ReasoningSelection::named("enabled"));
    request = serialized_request_json(context);
    chat_template_kwargs = field(request, "chat_template_kwargs");
    check_bool_field(chat_template_kwargs, "enable_thinking", true,
                     "Qwen Chat enables thinking through chat template kwargs");
    context.options.reasoning = ainiux::ReasoningSelection::named("none");
    request = serialized_request_json(context);
    check_bool_field(field(request, "chat_template_kwargs"), "enable_thinking", false,
                     "Qwen Chat disables thinking through chat template kwargs");
    check(field(request, "enable_thinking") == nullptr &&
              field(request, "thinking_budget") == nullptr,
          "Qwen Chat does not send unsupported top-level effort fields");

    context = chat_context(ainiux::ReasoningProtocol::QwenChatEffort,
                           ainiux::ReasoningSelection::automatic());
    request = serialized_request_json(context);
    check(field(request, "chat_template_kwargs") == nullptr &&
              field(request, "reasoning_effort") == nullptr,
          "Qwen Chat effort omits fields for Auto");
    context.options.reasoning = ainiux::ReasoningSelection::named("none");
    context.options.reasoning_explicit = true;
    request = serialized_request_json(context);
    check_bool_field(field(request, "chat_template_kwargs"), "enable_thinking", false,
                     "Qwen Chat effort disables thinking through chat template kwargs");
    check(field(request, "reasoning_effort") == nullptr &&
              field(field(request, "chat_template_kwargs"), "reasoning_effort") == nullptr,
          "Qwen Chat effort omits reasoning_effort when thinking is off");
    context.options.reasoning = ainiux::ReasoningSelection::named("medium");
    request = serialized_request_json(context);
    chat_template_kwargs = field(request, "chat_template_kwargs");
    check_bool_field(chat_template_kwargs, "enable_thinking", true,
                     "Qwen Chat effort enables thinking for a named effort");
    check_string_field(chat_template_kwargs, "reasoning_effort", "medium",
                       "Qwen Chat effort puts reasoning_effort in chat template kwargs");
    check_string_field(request, "reasoning_effort", "medium",
                       "Qwen Chat effort also sends top-level reasoning_effort");
    context.options.reasoning = ainiux::ReasoningSelection::named("xhigh");
    request = serialized_request_json(context);
    check_string_field(field(request, "chat_template_kwargs"), "reasoning_effort", "xhigh",
                       "Qwen Chat effort preserves xhigh without remapping");
    check_string_field(request, "reasoning_effort", "xhigh",
                       "Qwen Chat effort sends xhigh at the top level");

    context = context_for(ainiux::ReasoningProtocol::QwenResponses,
                          ainiux::ReasoningSelection::named("max"),
                          ainiux::provider::ApiKind::Responses);
    request = serialized_request_json(context);
    check_string_field(field(request, "reasoning"), "effort", "max",
                       "Qwen Responses uses reasoning.effort");

    context = chat_context(ainiux::ReasoningProtocol::DeepSeek,
                           ainiux::ReasoningSelection::named("medium"));
    request = serialized_request_json(context);
    check_string_field(field(request, "thinking"), "type", "enabled",
                       "DeepSeek enables thinking for a named value");
    check_string_field(request, "reasoning_effort", "medium",
                       "DeepSeek preserves named efforts without remapping");
    context.options.reasoning = ainiux::ReasoningSelection::named("none");
    request = serialized_request_json(context);
    check_string_field(field(request, "thinking"), "type", "disabled",
                       "DeepSeek recognizes none");
    check(field(request, "reasoning_effort") == nullptr,
          "DeepSeek omits effort when disabled");

    context = chat_context(ainiux::ReasoningProtocol::KimiEffort,
                           ainiux::ReasoningSelection::named("max"));
    request = serialized_request_json(context);
    check_string_field(request, "reasoning_effort", "max",
                       "Kimi effort protocol preserves max");
    context.options.reasoning = ainiux::ReasoningSelection::named("off");
    request = serialized_request_json(context);
    check_string_field(request, "reasoning_effort", "off",
                       "Kimi effort protocol keeps semantic off as off");

    context = chat_context(ainiux::ReasoningProtocol::Zai,
                           ainiux::ReasoningSelection::named("xhigh"));
    request = serialized_request_json(context);
    check_string_field(request, "reasoning_effort", "xhigh",
                       "Z.AI preserves an unknown effort without conversion");
    check_string_field(field(request, "thinking"), "type", "enabled",
                       "Z.AI enables thinking for explicit effort");

    context = chat_context(ainiux::ReasoningProtocol::XaiEffort,
                           ainiux::ReasoningSelection::named("xhigh"));
    request = serialized_request_json(context);
    check_string_field(field(request, "reasoning"), "effort", "xhigh",
                       "xAI preserves named effort values in reasoning.effort");

    context = context_for(ainiux::ReasoningProtocol::MiniMaxResponses,
                          ainiux::ReasoningSelection::named("high"),
                          ainiux::provider::ApiKind::Responses);
    request = serialized_request_json(context);
    check_string_field(field(request, "reasoning"), "effort", "high",
                       "MiniMax Responses uses reasoning.effort");

    context = chat_context(ainiux::ReasoningProtocol::NemotronTemplate,
                           ainiux::ReasoningSelection::token_budget(3072));
    request = serialized_request_json(context);
    const ainiux::json::Value* template_kwargs = field(request, "chat_template_kwargs");
    check_bool_field(template_kwargs, "enable_thinking", true,
                     "Nemotron enables thinking in chat template kwargs");
    check_number_field(template_kwargs, "reasoning_budget", 3072.0,
                       "Nemotron preserves exact reasoning budgets");

    context = chat_context(ainiux::ReasoningProtocol::Hy3Template,
                           ainiux::ReasoningSelection::named("ultra"));
    request = serialized_request_json(context);
    const ainiux::json::Value* extra_body = field(request, "extra_body");
    template_kwargs = field(extra_body, "chat_template_kwargs");
    check_string_field(template_kwargs, "reasoning_effort", "ultra",
                       "Hy3 forwards named reasoning in chat template kwargs");

    context = chat_context(ainiux::ReasoningProtocol::None,
                           ainiux::ReasoningSelection::named("high"));
    request = serialized_request_json(context);
    check(field(request, "reasoning") == nullptr &&
              field(request, "reasoning_effort") == nullptr &&
              field(request, "thinking") == nullptr,
          "none protocol omits reasoning controls");

    context = chat_context(ainiux::ReasoningProtocol::OpenAiEffort,
                           ainiux::ReasoningSelection::named("high"));
    context.options.model_catalog.models.front().temperature =
        ainiux::TemperatureSupport::ReasoningNoneOnly;
    context.options.has_temperature = true;
    context.options.temperature = 0.7;
    request = serialized_request_json(context);
    check_number_field(request, "temperature", 0.7,
                       "explicit temperature remains serialized when catalog metadata warns");
    check(!ainiux::provider::reasoning_temperature_advisory(context).empty(),
          "explicit unsupported temperature produces an advisory");
    context.options.reasoning = ainiux::ReasoningSelection::named("none");
    check(ainiux::provider::reasoning_temperature_advisory(context).empty(),
          "reasoning=none satisfies conditional GPT-5 temperature support");
}

void test_provider_unicode_request_serialization() {
    ainiux::provider::RequestContext context;
    context.options.model = "mock-model";
    context.options.stream = false;
    const std::string unicode = u8"مرحبا 你好 👨‍👩‍👧‍👦";
    std::vector<ainiux::provider::Message> messages = {{"user", unicode}};
    const std::string request = ainiux::provider::serialize_chat_request(context, messages);
    ainiux::json::ParseResult parsed = ainiux::json::parse(request);
    check(parsed.error.ok(), "Unicode chat request serializes to valid JSON");
    const ainiux::json::Value* content = parsed.value.get("messages");
    content = content == nullptr ? nullptr : content->at(0);
    content = content == nullptr ? nullptr : content->get("content");
    check(content != nullptr && content->is_string() && content->string == unicode,
          "Unicode chat request preserves Arabic, Chinese, and emoji message text");
}

void test_openrouter_session_stickiness_serialization() {
    ainiux::provider::RequestContext context;
    context.profile.name = "openrouter";
    context.base_url = "https://openrouter.ai/api/v1";
    context.options.model = "anthropic/claude-sonnet";
    context.options.stream = false;
    context.routing_session_id = "ainiux-test-session";
    const std::vector<ainiux::provider::Message> messages = {{"user", "hello"}};

    ainiux::json::ParseResult parsed = ainiux::json::parse(
        ainiux::provider::serialize_chat_request(context, messages));
    check(parsed.error.ok(), "OpenRouter sticky-session chat request is valid JSON");
    const ainiux::json::Value* session_id = parsed.value.get("session_id");
    check(session_id != nullptr && session_id->is_string() &&
              session_id->string == "ainiux-test-session",
          "OpenRouter chat request includes its opaque session id");

    ainiux::provider::ToolConversation conversation;
    conversation.messages = messages;
    const std::vector<ainiux::provider::FunctionDefinition> tools = {
        {"read_file", "Read a file", R"({"type":"object","properties":{}})"}};
    parsed = ainiux::json::parse(
        ainiux::provider::serialize_tool_request(context, conversation, tools));
    session_id = parsed.error.ok() ? parsed.value.get("session_id") : nullptr;
    check(parsed.error.ok() && session_id != nullptr && session_id->is_string() &&
              session_id->string == "ainiux-test-session",
          "OpenRouter native tool rounds retain the same session id");

    context.profile.name = "deepseek";
    parsed = ainiux::json::parse(
        ainiux::provider::serialize_chat_request(context, messages));
    check(parsed.error.ok() && parsed.value.get("session_id") == nullptr,
          "non-OpenRouter requests omit routing session ids");

    context.profile.name = "openrouter";
    context.base_url = "https://gateway.example/v1";
    parsed = ainiux::json::parse(
        ainiux::provider::serialize_chat_request(context, messages));
    check(parsed.error.ok() && parsed.value.get("session_id") == nullptr,
          "custom endpoints using the OpenRouter profile omit routing session ids");

    const std::string generated = ainiux::provider::new_routing_session_id();
    const std::string next = ainiux::provider::new_routing_session_id();
    check(generated.rfind("ainiux-", 0) == 0 && generated.size() <= 256 &&
              generated != next,
          "generated routing session ids are opaque, bounded, and distinct");
}

void test_native_tool_protocols() {
    ainiux::provider::RequestContext context;
    context.options.model = "mock-model";
    context.options.stream = false;
    context.api_kind = ainiux::provider::ApiKind::ChatCompletions;
    ainiux::provider::ToolConversation conversation;
    conversation.messages = {{"system", "trusted"}, {"user", "review"}};
    conversation.continuation_items_json = {
        R"({"role":"assistant","content":null,"reasoning_details":[{"type":"reasoning.encrypted","data":"opaque"}],"tool_calls":[{"id":"call_1","type":"function","function":{"name":"read_file","arguments":"{\"path\":\"a.cpp\"}"}}]})",
        R"({"role":"tool","tool_call_id":"call_1","content":"{\"ok\":true}"})"};
    const std::vector<ainiux::provider::FunctionDefinition> definitions = {
        {"read_file", "Read source", R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"],"additionalProperties":false})"}};
    ainiux::json::ParseResult parsed = ainiux::json::parse(
        ainiux::provider::serialize_tool_request(context, conversation, definitions));
    check(parsed.error.ok(), "Chat native tool request is valid JSON");
    const ainiux::json::Value* messages = parsed.value.get("messages");
    check(messages != nullptr && messages->is_array() && messages->array.size() == 4,
          "Chat native tool request replays assistant call and tool result");
    const ainiux::json::Value* tools = parsed.value.get("tools");
    check(tools != nullptr && tools->is_array() && tools->array.size() == 1,
          "Chat native tool request serializes function definitions");

    const std::string chat_body =
        R"({"model":"mock-model","choices":[{"finish_reason":"tool_calls","message":{"role":"assistant","content":null,"reasoning_details":[{"type":"reasoning.encrypted","data":"opaque"}],"tool_calls":[{"id":"one","type":"function","function":{"name":"read_file","arguments":"{\"path\":\"a.cpp\"}"}},{"id":"two","type":"function","function":{"name":"glob","arguments":"{\"pattern\":\"**/*.cpp\"}"}}]}}]})";
    ainiux::provider::ToolRoundResult round;
    ainiux::Error error = ainiux::provider::parse_tool_response(context, chat_body, round, false);
    check(error.ok() && round.tool_calls.size() == 2 && round.tool_calls[1].name == "glob",
          "Chat native tool parser preserves parallel ordered calls");
    check(round.continuation_items_json.front().find("reasoning_details") != std::string::npos,
          "Chat native tool parser preserves opaque reasoning details");
    check(round.reasoning_text.empty(),
          "Chat native tool parser does not expose encrypted reasoning");

    const std::string readable_chat_body =
        R"({"choices":[{"message":{"role":"assistant","content":null,"reasoning_content":"inspect the file","tool_calls":[{"id":"r1","type":"function","function":{"name":"read_file","arguments":"{}"}}]}}]})";
    std::string reasoning_delta;
    error = ainiux::provider::parse_tool_response(
        context, readable_chat_body, round, false,
        [&](const std::string& delta) {
            reasoning_delta += delta;
            return ainiux::ok_error();
        });
    check(error.ok() && round.reasoning_text == "inspect the file" &&
              reasoning_delta == round.reasoning_text,
          "non-streamed native Chat exposes readable reasoning and callback text");

    const std::string chat_stream =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_\",\"function\":{\"name\":\"read_\",\"arguments\":\"{\\\"path\\\":\"}}]},\"finish_reason\":null}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"9\",\"function\":{\"name\":\"file\",\"arguments\":\"\\\"a.cpp\\\"}\"}}]},\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: [DONE]\n\n";
    error = ainiux::provider::parse_tool_response(context, chat_stream, round, true);
    check(error.ok() && round.tool_calls.size() == 1 && round.tool_calls[0].id == "call_9" &&
              round.tool_calls[0].name == "read_file" &&
              round.tool_calls[0].arguments_json == R"({"path":"a.cpp"})",
          "fragmented streamed Chat tool call assembles id, name, and arguments by index");

    const std::string reasoning_chat_stream =
        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"inspect \"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"reasoning_details\":[{\"type\":\"reasoning.text\",\"text\":\"carefully\"}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"c1\",\"function\":{\"name\":\"read_file\",\"arguments\":\"{}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    reasoning_delta.clear();
    error = ainiux::provider::parse_tool_response(
        context, reasoning_chat_stream, round, true,
        [&](const std::string& delta) {
            reasoning_delta += delta;
            return ainiux::ok_error();
        });
    check(error.ok() && round.reasoning_text == "inspect carefully" &&
              reasoning_delta == round.reasoning_text,
          "streamed native Chat exposes DeepSeek/OpenRouter readable reasoning deltas");

    context.api_kind = ainiux::provider::ApiKind::Responses;
    context.options.api = "responses";
    parsed = ainiux::json::parse(
        ainiux::provider::serialize_tool_request(context, conversation, definitions));
    check(parsed.error.ok(), "Responses native tool request is valid JSON");
    const ainiux::json::Value* input = parsed.value.get("input");
    check(input != nullptr && input->is_array() && input->array.size() == 3,
          "Responses native request preserves continuation items in input");
    tools = parsed.value.get("tools");
    check(tools != nullptr && tools->at(0) != nullptr && tools->at(0)->get("function") == nullptr &&
              tools->at(0)->get("name") != nullptr,
          "Responses native definitions use the Responses function shape");

    const std::string responses_body =
        R"({"model":"mock-model","status":"completed","output":[{"type":"reasoning","id":"r1","encrypted_content":"opaque"},{"type":"function_call","id":"fc1","call_id":"call_7","name":"search_text","arguments":"{\"query\":\"auth\"}"}]})";
    error = ainiux::provider::parse_tool_response(context, responses_body, round, false);
    check(error.ok() && round.tool_calls.size() == 1 && round.tool_calls[0].id == "call_7" &&
              round.continuation_items_json.size() == 2,
          "Responses parser preserves reasoning and function_call output items");
    check(round.reasoning_text.empty(),
          "Responses parser omits opaque encrypted reasoning");
    ainiux::provider::append_tool_results(context, round.tool_calls,
                                          {R"({"ok":true})"}, conversation);
    check(conversation.continuation_items_json.back().find("function_call_output") !=
                  std::string::npos &&
              conversation.continuation_items_json.back().find("call_7") !=
                  std::string::npos,
          "Responses tool results replay as function_call_output with call_id");

    const std::string responses_reasoning_body =
        R"({"status":"completed","output":[{"type":"reasoning","summary":[{"type":"summary_text","text":"check callers"}]},{"type":"function_call","call_id":"call_8","name":"search_text","arguments":"{}"}]})";
    error = ainiux::provider::parse_tool_response(
        context, responses_reasoning_body, round, false);
    check(error.ok() && round.reasoning_text == "check callers",
          "Responses native tool parser exposes readable reasoning summaries");

    const std::string responses_text_body =
        R"({"model":"mock-model","status":"completed","output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":"first"}]},{"type":"message","role":"assistant","content":[{"type":"output_text","text":" second"}]}]})";
    error = ainiux::provider::parse_tool_response(context, responses_text_body, round, false);
    check(error.ok() && round.content == "first second",
          "Responses native tool parser accumulates text from every output message");

    const std::string responses_reasoning_stream =
        "data: {\"type\":\"response.reasoning_summary_text.delta\",\"output_index\":0,\"delta\":\"check \"}\n\n"
        "data: {\"type\":\"response.reasoning_summary_text.delta\",\"output_index\":0,\"delta\":\"tests\"}\n\n"
        "data: {\"type\":\"response.output_item.done\",\"output_index\":1,\"item\":{\"type\":\"function_call\",\"call_id\":\"call_9\",\"name\":\"search_text\",\"arguments\":\"{}\"}}\n\n";
    reasoning_delta.clear();
    error = ainiux::provider::parse_tool_response(
        context, responses_reasoning_stream, round, true,
        [&](const std::string& delta) {
            reasoning_delta += delta;
            return ainiux::ok_error();
        });
    check(error.ok() && round.reasoning_text == "check tests" &&
              reasoning_delta == round.reasoning_text,
          "streamed Responses reasoning summaries expose readable deltas");

    context.api_kind = ainiux::provider::ApiKind::ChatCompletions;
    const std::string think_body =
        R"({"choices":[{"message":{"role":"assistant","content":"<think>trace supplied</think>\n\nDone"}}]})";
    error = ainiux::provider::parse_tool_response(context, think_body, round, false);
    check(error.ok() && round.reasoning_text == "trace supplied",
          "native tool parser extracts readable think-tag traces");

    const std::string invalid_index_stream =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0.5,\"id\":\"call\",\"function\":{\"name\":\"read_file\",\"arguments\":\"{}\"}}]}}]}\n\n";
    context.api_kind = ainiux::provider::ApiKind::ChatCompletions;
    error = ainiux::provider::parse_tool_response(context, invalid_index_stream, round, true);
    check(!error.ok(), "native tool stream rejects fractional call indexes");

    const std::string openai_cache_body =
        R"({"usage":{"prompt_tokens":100,"completion_tokens":7,"prompt_tokens_details":{"cached_tokens":80,"cache_write_tokens":12}},"choices":[{"message":{"role":"assistant","content":"done"}}]})";
    error = ainiux::provider::parse_tool_response(
        context, openai_cache_body, round, false);
    check(error.ok() && round.metrics.prompt_tokens == 100 &&
              round.metrics.fresh_prompt_tokens == 20 &&
              round.metrics.cache_read_tokens == 80 &&
              round.metrics.cache_write_tokens == 12 &&
              round.metrics.completion_tokens == 7,
          "OpenAI cache details normalize into fresh/read/write token metrics");

    const std::string deepseek_cache_body =
        R"({"usage":{"prompt_tokens":100,"completion_tokens":9,"prompt_cache_hit_tokens":75,"prompt_cache_miss_tokens":25},"choices":[{"message":{"role":"assistant","content":"done"}}]})";
    error = ainiux::provider::parse_tool_response(
        context, deepseek_cache_body, round, false);
    check(error.ok() && round.metrics.fresh_prompt_tokens == 25 &&
              round.metrics.cache_read_tokens == 75 &&
              round.metrics.cache_write_tokens == -1,
          "DeepSeek hit/miss usage normalizes without inventing cache writes");

    const std::string partial_cache_body =
        R"({"usage":{"prompt_tokens":10,"prompt_tokens_details":{"cached_tokens":20}},"choices":[{"message":{"role":"assistant","content":"done"}}]})";
    error = ainiux::provider::parse_tool_response(
        context, partial_cache_body, round, false);
    check(error.ok() && round.metrics.fresh_prompt_tokens == -1 &&
              round.metrics.cache_read_tokens == 20,
          "inconsistent partial cache usage remains observable without underflow");

    context.api_kind = ainiux::provider::ApiKind::Responses;
    const std::string cached_responses_stream =
        "data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\","
        "\"usage\":{\"input_tokens\":40,\"output_tokens\":3,"
        "\"input_tokens_details\":{\"cached_tokens\":30,\"cache_write_tokens\":4}},"
        "\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{"
        "\"type\":\"output_text\",\"text\":\"done\"}]}]}}\n\n";
    error = ainiux::provider::parse_tool_response(
        context, cached_responses_stream, round, true);
    check(error.ok() && round.metrics.prompt_tokens == 40 &&
              round.metrics.fresh_prompt_tokens == 10 &&
              round.metrics.cache_read_tokens == 30 &&
              round.metrics.cache_write_tokens == 4 &&
              round.metrics.completion_tokens == 3,
          "streaming Responses final usage retains normalized cache metrics");
}

void test_credit_balance_parsing_and_formatting() {
    ainiux::provider::CreditBalanceResult result;
    ainiux::Error error = ainiux::provider::parse_credit_balance_response(
        "openrouter",
        R"({"data":{"total_credits":10.0,"total_usage":5.5}})", result);
    check(error.ok() && result.balances.size() == 1 &&
              result.balances.front().amount == 4.5 &&
              result.balances.front().currency == "USD" &&
              ainiux::provider::format_credit_balance(result) == "4.50 USD",
          "OpenRouter purchased credits minus usage formats as USD credit");

    error = ainiux::provider::parse_credit_balance_response(
        "openrouter", R"({"data":{"total_credits":10.0}})", result);
    check(!error.ok() && error.code == ErrorCode::ProviderSchema,
          "OpenRouter rejects incomplete credit totals");

    error = ainiux::provider::parse_credit_balance_response(
        "openai",
        R"({"object":"credit_summary","total_granted":20.0,"total_used":7.25,"total_available":12.75,"grants":{"object":"list","data":[],"has_more":false}})",
        result);
    check(error.ok() && result.balances.size() == 1 &&
              result.balances.front().amount == 12.75 &&
              result.balances.front().currency == "USD" &&
              ainiux::provider::format_credit_balance(result) == "12.75 USD",
          "OpenAI total_available formats as remaining USD credit");

    error = ainiux::provider::parse_credit_balance_response(
        "openai",
        R"({"object":"credit_summary","total_granted":20.0,"total_used":7.25})",
        result);
    check(!error.ok() && error.code == ErrorCode::ProviderSchema,
          "OpenAI rejects credit responses without total_available");

    error = ainiux::provider::parse_credit_balance_response(
        "deepseek",
        R"({"is_available":true,"balance_infos":[{"currency":"CNY","total_balance":"110.00","granted_balance":"10.00","topped_up_balance":"100.00"},{"currency":"USD","total_balance":"4.5","granted_balance":"0","topped_up_balance":"4.5"}]})",
        result);
    check(error.ok() && result.balances.size() == 2 &&
              ainiux::provider::format_credit_balance(result) ==
                  "110.00 CNY · 4.50 USD",
          "DeepSeek preserves and formats every returned currency");

    error = ainiux::provider::parse_credit_balance_response(
        "deepseek",
        R"({"balance_infos":[{"currency":"CNY","total_balance":"not-money"}]})",
        result);
    check(!error.ok() && error.code == ErrorCode::ProviderSchema,
          "DeepSeek rejects malformed balance decimals");

    const std::vector<ainiux::provider::Profile> profiles =
        ainiux::provider::built_in_profiles();
    const auto openrouter =
        std::find_if(profiles.begin(), profiles.end(), [](const auto& profile) {
            return profile.name == "openrouter";
        });
    const auto openai =
        std::find_if(profiles.begin(), profiles.end(), [](const auto& profile) {
            return profile.name == "openai";
        });
    const auto deepseek =
        std::find_if(profiles.begin(), profiles.end(), [](const auto& profile) {
            return profile.name == "deepseek";
        });
    check(openrouter != profiles.end() &&
              openrouter->capabilities.credit_balance &&
              openrouter->credit_url ==
                  "https://openrouter.ai/api/v1/credits" &&
              openai != profiles.end() &&
              openai->capabilities.credit_balance &&
              openai->credit_url ==
                  "https://api.openai.com/v1/dashboard/billing/credit_grants" &&
              deepseek != profiles.end() &&
              deepseek->capabilities.credit_balance &&
              deepseek->credit_url ==
                  "https://api.deepseek.com/user/balance",
          "OpenRouter, OpenAI, and DeepSeek profiles expose credit endpoints");

    ainiux::provider::RequestContext credit_context;
    credit_context.profile = *openrouter;
    credit_context.base_url = credit_context.profile.base_url;
    credit_context.api_key = "test-secret";
    check(ainiux::provider::credit_balance_available(credit_context),
          "official OpenRouter context enables credit lookup");
    credit_context.base_url = "https://gateway.example/v1";
    check(!ainiux::provider::credit_balance_available(credit_context),
          "custom provider base URL disables official credit lookup");

    credit_context.profile = *openai;
    credit_context.base_url = "https://api.openai.com/v1";
    check(ainiux::provider::credit_balance_available(credit_context),
          "official OpenAI context enables credit lookup");

    credit_context.profile = *deepseek;
    credit_context.base_url = "https://api.deepseek.com/v1";
    check(ainiux::provider::credit_balance_available(credit_context),
          "normalized official DeepSeek base URL enables credit lookup");
}

}  // namespace

void run_all() {
    test_http_status_errors_are_friendly();
    test_openrouter_nested_provider_errors_are_unwrapped();
    test_chat_sse_accepts_cr_only_event_boundaries();
    test_chat_sse_streaming_reasoning_content();
    test_chat_sse_openrouter_reasoning_details_text();
    test_chat_sse_openrouter_reasoning_not_duplicated_with_details();
    test_chat_sse_openrouter_reasoning_summary_and_text();
    test_chat_sse_reasoning_normalizes_crlf();
    test_chat_sse_accepts_batched_json_data_lines();
    test_chat_sse_accepts_concatenated_json_payloads();
    test_chat_sse_accepts_concatenated_reasoning_payloads();
    test_chat_sse_accepts_concatenated_payloads_with_leaked_data_prefixes();
    test_chat_sse_accepts_concatenated_payloads_with_sse_metadata_prefixes();
    test_chat_sse_concatenated_payload_split_ignores_content_braces();
    test_chat_sse_accepts_json_payload_immediately_followed_by_done();
    test_responses_sse_accepts_concatenated_json_payloads();
    test_explicit_chat_url_does_not_require_base_when_model_set();
    test_image_capability_detection();
    test_provider_lookup_metadata();
    test_lmstudio_context();
    test_lmstudio_shortcut_context();
    test_parse_models_response_llama_server_hybrid();
    test_parse_models_response_llamacpp_meta();
    test_model_context_window_tokens();
    test_models_markdown_format();
    test_apply_provider_target_accepts_custom_url();
    test_tui_local_endpoint_auto_selects_model();
    test_tui_startup_provider_selection_helpers();
    test_editor_startup_local_only_default();
    test_editor_defaults_offline_without_credentials();
    test_editor_model_selection_restore_requires_available_endpoint();
    test_cli_target_change_clears_stale_remembered_model();
    test_none_provider_allows_an_empty_endpoint();
    test_openai_context_allows_missing_model();
    test_openrouter_shortcut_context();
    test_provider_unicode_request_serialization();
    test_openrouter_session_stickiness_serialization();
    test_provider_capabilities_and_responses_context();
    test_provider_registry_resolves_added_profiles();
    test_provider_responses_unsupported_and_override();
    test_provider_reasoning_request_compatibility();
    test_native_tool_protocols();
    test_credit_balance_parsing_and_formatting();
}

}  // namespace ainiux::test::provider
