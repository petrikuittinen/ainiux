#include "provider/test_provider.hpp"
#include "support/test_support.hpp"
#include "cli/args.hpp"
#include "json/json.hpp"
#include "provider/names.hpp"
#include "provider/provider.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

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

class UniqueFd {
   public:
    explicit UniqueFd(int fd = -1) : fd_(fd) {}
    ~UniqueFd() { reset(); }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    int get() const { return fd_; }
    int release() {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }
    void reset(int next = -1) {
        if (fd_ >= 0) {
            close(fd_);
        }
        fd_ = next;
    }

   private:
    int fd_ = -1;
};

bool send_all(int fd, const std::string& data) {
    size_t offset = 0;
    while (offset < data.size()) {
#ifdef MSG_NOSIGNAL
        const int flags = MSG_NOSIGNAL;
#else
        const int flags = 0;
#endif
        const ssize_t written = send(fd, data.data() + offset, data.size() - offset, flags);
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
    UniqueFd listen_fd(socket(AF_INET, SOCK_STREAM, 0));
    if (listen_fd.get() < 0) {
        return {ainiux::ErrorCode::Internal, "could not create test server socket"};
    }
    const int yes = 1;
    if (setsockopt(listen_fd.get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0) {
        return {ainiux::ErrorCode::Internal, "could not configure test server socket reuse"};
    }
    timeval timeout{};
    timeout.tv_sec = 5;
    if (setsockopt(listen_fd.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
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
    socklen_t length = sizeof(address);
    if (getsockname(listen_fd.get(), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        return {ainiux::ErrorCode::Internal, "could not inspect test server socket"};
    }
    const int port = ntohs(address.sin_port);
    const int server_fd = listen_fd.release();
    std::thread server([server_fd, body]() {
        UniqueFd scoped_listen(server_fd);
        UniqueFd client(accept(scoped_listen.get(), nullptr, nullptr));
        if (client.get() < 0) {
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
    UniqueFd listen_fd(socket(AF_INET, SOCK_STREAM, 0));
    if (listen_fd.get() < 0) {
        return {ainiux::ErrorCode::Internal, "could not create test server socket"};
    }
    const int yes = 1;
    if (setsockopt(listen_fd.get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0) {
        return {ainiux::ErrorCode::Internal, "could not configure test server socket reuse"};
    }
    timeval timeout{};
    timeout.tv_sec = 5;
    if (setsockopt(listen_fd.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
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
    socklen_t length = sizeof(address);
    if (getsockname(listen_fd.get(), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        return {ainiux::ErrorCode::Internal, "could not inspect test server socket"};
    }
    const int port = ntohs(address.sin_port);
    const int server_fd = listen_fd.release();
    std::thread server([server_fd, status, reason, content_type, body]() {
        UniqueFd scoped_listen(server_fd);
        UniqueFd client(accept(scoped_listen.get(), nullptr, nullptr));
        if (client.get() < 0) {
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
    check(ainiux::provider::profile_auto_selects_default_model(*lm_studio_profile, lm_studio_profile->base_url),
          "local lm_studio profile auto-selects a default model");
    check(!ainiux::provider::profile_auto_selects_default_model(*custom_profile, "https://api.example.com/v1"),
          "custom remote endpoint does not auto-select a default model");
    check(ainiux::provider::profile_auto_selects_default_model(*custom_profile, "http://localhost:8000/v1"),
          "custom loopback endpoint auto-selects a default model");
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
    check(ainiux::provider::context_window_for_model(models, "llama3") == 8192,
          "model context window lookup matches model id");

    ainiux::provider::RequestContext context;
    context.options.model = "llama3";
    ainiux::provider::apply_context_window_from_models(context, models);
    check(context.options.context_tokens == 8192,
          "context window is applied from model metadata when not configured explicitly");

    context.options.has_context_tokens = true;
    context.options.context_tokens = 0;
    ainiux::provider::apply_context_window_from_models(context, models);
    check(context.options.context_tokens == 0,
          "explicit context window configuration blocks model metadata fallback");

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
        const char* previous = std::getenv(name_.c_str());
        if (previous != nullptr) {
            previous_value_ = previous;
        }
        unsetenv(name_.c_str());
    }

    ~ScopedUnsetenv() {
        if (previous_value_.has_value()) {
            setenv(name_.c_str(), previous_value_->c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
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
    check(!ainiux::provider::defers_model_selection(context.context),
          "local custom URL defers model selection only for non-auto-select providers");
    check(!ainiux::provider::tui_defers_model_selection(context.context),
          "local custom URL chat UI auto-selects the default model");
    check(context.context.base_url.find("localhost:30000") != std::string::npos,
          "local URL chat UI keeps the requested endpoint");

    ainiux::cli::Options bare_chat;
    bare_chat.tui = true;
    ainiux::provider::apply_tui_startup_default(bare_chat);
    ainiux::provider::ContextResult offline = ainiux::provider::build_context(bare_chat);
    check(offline.error.ok(), "bare chat UI offline context builds");
    check(!ainiux::provider::tui_defers_model_selection(offline.context),
          "bare offline chat UI does not defer because provider selection comes first");

    const char* editor_argv[] = {"ainiux", "http://localhost:30000", "--editor"};
    ainiux::cli::ParseResult editor_parsed = ainiux::cli::parse_args(3, const_cast<char**>(editor_argv));
    check(editor_parsed.error.ok(), "local URL editor args parse");
    ainiux::provider::ContextResult editor_context = ainiux::provider::build_context(editor_parsed.options);
    check(editor_context.error.ok(), "local URL editor context builds");
    check(!ainiux::provider::defers_model_selection(editor_context.context),
          "local custom URL editor auto-selects the default model");
}

void test_tui_startup_provider_selection_helpers() {
    ainiux::cli::Options bare_chat;
    bare_chat.tui = true;
    check(ainiux::provider::tui_needs_startup_provider_selection(bare_chat),
          "bare chat UI requests startup provider selection");

    ainiux::provider::apply_tui_startup_default(bare_chat);
    check(bare_chat.provider == "none", "bare chat UI startup default uses offline provider until selection");

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
    check(ainiux::provider::profile_auto_selects_default_model(sglang_ctx.context.profile,
                                                               sglang_ctx.context.base_url),
          "sglang auto-selects a default model");

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
    ainiux::provider::RequestContext context;
    context.options.has_thinking_budget = true;
    context.options.thinking_budget = "high";

    context.profile.name = "openai";
    context.api_kind = ainiux::provider::ApiKind::ChatCompletions;
    ainiux::json::Value request = serialized_request_json(context);
    check_string_field(request,
                       "reasoning_effort",
                       "high",
                       "OpenAI Chat uses reasoning_effort for verbal thinking budgets");
    check(field(request, "enable_thinking") == nullptr && field(request, "thinking_budget") == nullptr,
          "OpenAI Chat does not receive generic thinking fields");

    context.api_kind = ainiux::provider::ApiKind::Responses;
    context.options.thinking_budget = "4096";
    request = serialized_request_json(context);
    const ainiux::json::Value* reasoning = field(request, "reasoning");
    check(reasoning != nullptr && reasoning->is_object(), "OpenAI Responses emits a reasoning object");
    check_string_field(reasoning,
                       "effort",
                       "medium",
                       "OpenAI Responses maps numeric budgets to reasoning effort");
    check(field(request, "thinking_budget") == nullptr,
          "OpenAI Responses does not receive generic thinking_budget");

    context = ainiux::provider::RequestContext{};
    context.profile.name = "openrouter";
    context.options.has_thinking_budget = true;
    context.options.thinking_budget = "2048";
    request = serialized_request_json(context);
    reasoning = field(request, "reasoning");
    check(reasoning != nullptr && reasoning->is_object(), "OpenRouter emits unified reasoning object");
    check_number_field(reasoning,
                       "max_tokens",
                       2048.0,
                       "OpenRouter preserves numeric thinking budgets as reasoning.max_tokens");
    check(field(request, "reasoning_effort") == nullptr,
          "OpenRouter does not receive top-level reasoning_effort");

    context.options.thinking_budget = "xhigh";
    request = serialized_request_json(context);
    reasoning = field(request, "reasoning");
    check_string_field(reasoning,
                       "effort",
                       "xhigh",
                       "OpenRouter preserves effort labels in reasoning.effort");

    context = ainiux::provider::RequestContext{};
    context.profile.name = "gemini";
    context.options.has_thinking_budget = true;
    context.options.thinking_budget = "8192";
    request = serialized_request_json(context);
    check_string_field(request,
                       "reasoning_effort",
                       "medium",
                       "Gemini maps numeric thinking budgets to documented OpenAI-compatible reasoning_effort");
    check(field(request, "extra_body") == nullptr,
          "Gemini default compatibility path does not mix reasoning_effort with native thinking_config");

    context.options.thinking_budget = "xhigh";
    request = serialized_request_json(context);
    check_string_field(request,
                       "reasoning_effort",
                       "high",
                       "Gemini maps over-high effort labels to its highest OpenAI-compatible effort");

    context = ainiux::provider::RequestContext{};
    context.profile.name = "anthropic";
    context.options.has_thinking_budget = true;
    context.options.thinking_budget = "2048";
    request = serialized_request_json(context);
    const ainiux::json::Value* thinking = field(request, "thinking");
    check_string_field(thinking,
                       "type",
                       "enabled",
                       "Anthropic Claude OpenAI compatibility uses thinking.type enabled");
    check_number_field(thinking,
                       "budget_tokens",
                       2048.0,
                       "Anthropic Claude preserves numeric thinking budgets as budget_tokens");
    check(field(request, "reasoning_effort") == nullptr && field(request, "enable_thinking") == nullptr,
          "Anthropic Claude does not receive OpenAI or generic thinking controls");

    context.options.thinking_budget = "high";
    request = serialized_request_json(context);
    thinking = field(request, "thinking");
    const ainiux::json::Value* output_config = field(request, "output_config");
    check_string_field(thinking,
                       "type",
                       "adaptive",
                       "Anthropic Claude maps verbal budgets to adaptive thinking");
    check_string_field(output_config,
                       "effort",
                       "high",
                       "Anthropic Claude carries verbal effort in output_config");

    context = ainiux::provider::RequestContext{};
    context.profile.name = "qwen";
    context.options.has_thinking_budget = true;
    context.options.thinking_budget = "high";
    request = serialized_request_json(context);
    check_bool_field(request, "enable_thinking", true, "Qwen thinking budget enables thinking");
    check_number_field(request,
                       "thinking_budget",
                       24576.0,
                       "Qwen maps verbal thinking budgets to documented token-budget scale");

    context.profile.name = "dashscope";
    context.options.thinking_budget = "1024";
    request = serialized_request_json(context);
    check_bool_field(request, "enable_thinking", true, "DashScope numeric budget enables thinking");
    check_number_field(request, "thinking_budget", 1024.0, "DashScope preserves numeric thinking_budget");

    context = ainiux::provider::RequestContext{};
    context.profile.name = "deepseek";
    context.options.has_enable_thinking = true;
    context.options.enable_thinking = false;
    request = serialized_request_json(context);
    thinking = field(request, "thinking");
    check_string_field(thinking,
                       "type",
                       "disabled",
                       "DeepSeek maps thinking off to thinking.type disabled");
    check(field(request, "reasoning_effort") == nullptr,
          "DeepSeek disabled thinking omits reasoning_effort");

    context.options.enable_thinking = true;
    context.options.has_thinking_budget = true;
    context.options.thinking_budget = "medium";
    request = serialized_request_json(context);
    thinking = field(request, "thinking");
    check_string_field(thinking,
                       "type",
                       "enabled",
                       "DeepSeek maps enabled thinking to thinking.type enabled");
    check_string_field(request,
                       "reasoning_effort",
                       "high",
                       "DeepSeek maps low/medium-compatible efforts to high");

    context = ainiux::provider::RequestContext{};
    context.profile.name = "moonshot";
    context.options.model = "kimi-k2.6";
    context.options.has_enable_thinking = true;
    context.options.enable_thinking = false;
    request = serialized_request_json(context);
    thinking = field(request, "thinking");
    check_string_field(thinking,
                       "type",
                       "disabled",
                       "Kimi K2.6 maps thinking off to thinking.type disabled");
    check(field(request, "reasoning_effort") == nullptr && field(request, "thinking_budget") == nullptr,
          "Kimi K2.6 does not receive unsupported effort or token-budget fields");

    context.options.model = "kimi-k2.7-code";
    context.options.enable_thinking = true;
    request = serialized_request_json(context);
    check(field(request, "thinking") == nullptr,
          "Kimi K2.7 code omits thinking parameter because the model is always thinking");

    context = ainiux::provider::RequestContext{};
    context.profile.name = "zai";
    context.options.model = "glm-5.2";
    context.options.has_thinking_budget = true;
    context.options.thinking_budget = "xhigh";
    request = serialized_request_json(context);
    thinking = field(request, "thinking");
    check_string_field(thinking,
                       "type",
                       "enabled",
                       "GLM-5.2 enables thinking when reasoning effort is requested");
    check_string_field(request,
                       "reasoning_effort",
                       "max",
                       "GLM-5.2 maps xhigh to max reasoning effort");

    context.options.thinking_budget = "none";
    request = serialized_request_json(context);
    thinking = field(request, "thinking");
    check_string_field(thinking,
                       "type",
                       "disabled",
                       "GLM-5.2 maps none to disabled thinking");
    check(field(request, "reasoning_effort") == nullptr,
          "GLM-5.2 does not send reasoning_effort when thinking is disabled");

    context = ainiux::provider::RequestContext{};
    context.profile.name = "xai";
    context.options.has_thinking_budget = true;
    context.options.thinking_budget = "xhigh";
    request = serialized_request_json(context);
    check_string_field(request,
                       "reasoning_effort",
                       "high",
                       "xAI maps xhigh to the highest documented Grok effort");

    context = ainiux::provider::RequestContext{};
    context.profile.name = "custom_openai_chat";
    context.options.has_enable_thinking = true;
    context.options.enable_thinking = true;
    context.options.has_thinking_budget = true;
    context.options.thinking_budget = "high";
    request = serialized_request_json(context);
    check_bool_field(request,
                     "enable_thinking",
                     true,
                     "custom OpenAI-compatible endpoints keep generic enable_thinking");
    check_string_field(request,
                       "thinking_budget",
                       "high",
                       "custom OpenAI-compatible endpoints preserve verbal thinking_budget");

    context.options.model = "deepseek-v4-pro";
    request = serialized_request_json(context);
    thinking = field(request, "thinking");
    check_string_field(thinking,
                       "type",
                       "enabled",
                       "custom endpoints with a DeepSeek V4 model use DeepSeek thinking fields");
    check_string_field(request,
                       "reasoning_effort",
                       "high",
                       "custom endpoints with a DeepSeek V4 model map verbal budgets to DeepSeek reasoning_effort");
    check(field(request, "enable_thinking") == nullptr && field(request, "thinking_budget") == nullptr,
          "custom DeepSeek V4 model detection suppresses generic thinking fields");
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

}  // namespace

void run_all() {
    test_http_status_errors_are_friendly();
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
    test_none_provider_allows_an_empty_endpoint();
    test_openai_context_allows_missing_model();
    test_openrouter_shortcut_context();
    test_provider_unicode_request_serialization();
    test_provider_capabilities_and_responses_context();
    test_provider_registry_resolves_added_profiles();
    test_provider_responses_unsupported_and_override();
    test_provider_reasoning_request_compatibility();
}

}  // namespace ainiux::test::provider
