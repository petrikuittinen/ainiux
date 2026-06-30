#include "provider/test_provider.hpp"
#include "support/test_support.hpp"
#include "cli/args.hpp"
#include "json/json.hpp"
#include "provider/provider.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <string>
#include <thread>
#include <vector>

namespace pkchat::test::provider {

namespace {

using pkchat::test::check;
using pkchat::test::read_fixture;

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

pkchat::Error run_stream_from_body(const std::string& body,
                                   pkchat::provider::ApiKind api_kind,
                                   pkchat::provider::ChatResult& result,
                                   std::string& streamed) {
    UniqueFd listen_fd(socket(AF_INET, SOCK_STREAM, 0));
    if (listen_fd.get() < 0) {
        return {pkchat::ErrorCode::Internal, "could not create test server socket"};
    }
    const int yes = 1;
    if (setsockopt(listen_fd.get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0) {
        return {pkchat::ErrorCode::Internal, "could not configure test server socket reuse"};
    }
    timeval timeout{};
    timeout.tv_sec = 5;
    if (setsockopt(listen_fd.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
        return {pkchat::ErrorCode::Internal, "could not configure test server socket timeout"};
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(listen_fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        return {pkchat::ErrorCode::Internal, "could not bind test server socket"};
    }
    if (listen(listen_fd.get(), 1) != 0) {
        return {pkchat::ErrorCode::Internal, "could not listen on test server socket"};
    }
    socklen_t length = sizeof(address);
    if (getsockname(listen_fd.get(), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        return {pkchat::ErrorCode::Internal, "could not inspect test server socket"};
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

    pkchat::provider::RequestContext context;
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

    pkchat::Error err = pkchat::provider::send_chat_messages(
        context,
        {{"user", "hello"}},
        [&](const std::string& delta) {
            streamed += delta;
            return pkchat::ok_error();
        },
        result);
    server.join();
    return err;
}

pkchat::Error run_chat_stream_from_body(const std::string& body,
                                        pkchat::provider::ChatResult& result,
                                        std::string& streamed) {
    return run_stream_from_body(body, pkchat::provider::ApiKind::ChatCompletions, result, streamed);
}

pkchat::Error run_responses_stream_from_body(const std::string& body,
                                             pkchat::provider::ChatResult& result,
                                             std::string& streamed) {
    return run_stream_from_body(body, pkchat::provider::ApiKind::Responses, result, streamed);
}

void test_chat_sse_accepts_cr_only_event_boundaries() {
    const std::string body =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\r\r"
        "data: {\"choices\":[{\"delta\":{\"content\":\" world\"}}]}\r\r"
        "data: [DONE]\r\r";
    pkchat::provider::ChatResult result;
    std::string streamed;
    const pkchat::Error err = run_chat_stream_from_body(body, result, streamed);
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
    pkchat::provider::ChatResult result;
    std::string streamed;
    const pkchat::Error err = run_chat_stream_from_body(body, result, streamed);
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
    pkchat::provider::ChatResult result;
    std::string streamed;
    const pkchat::Error err = run_chat_stream_from_body(body, result, streamed);
    check(err.ok(), "chat SSE parser streams OpenRouter reasoning.text details");
    check(result.content == "<think>Let me think</think>\n\nAnswer",
          "OpenRouter reasoning.text chunks concatenate without extra newlines");
}

void test_chat_sse_openrouter_reasoning_not_duplicated_with_details() {
    const std::string body =
        "data: {\"choices\":[{\"delta\":{\"reasoning\":\"shared\",\"reasoning_details\":[{\"type\":\"reasoning.text\",\"text\":\"shared\"}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"Done\"}}]}\n\n"
        "data: [DONE]\n\n";
    pkchat::provider::ChatResult result;
    std::string streamed;
    const pkchat::Error err = run_chat_stream_from_body(body, result, streamed);
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
    pkchat::provider::ChatResult result;
    std::string streamed;
    const pkchat::Error err = run_chat_stream_from_body(body, result, streamed);
    check(err.ok(), "chat SSE parser accepts OpenRouter summary and text reasoning details");
    check(result.content == "<think>High level\nDetailed trace</think>\n\nFinal",
          "OpenRouter reasoning.summary and reasoning.text are separated by a newline");
}

void test_chat_sse_reasoning_normalizes_crlf() {
    const std::string body =
        "data: {\"choices\":[{\"delta\":{\"reasoning\":\"line1\\r\\nline2\\rmore\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"OK\"}}]}\n\n"
        "data: [DONE]\n\n";
    pkchat::provider::ChatResult result;
    std::string streamed;
    const pkchat::Error err = run_chat_stream_from_body(body, result, streamed);
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
    pkchat::provider::ChatResult result;
    std::string streamed;
    const pkchat::Error err = run_chat_stream_from_body(body, result, streamed);
    check(err.ok(), "chat SSE parser accepts batched JSON data lines");
    check(result.content == "Hello world", "batched JSON data lines accumulate chat deltas");
    check(streamed == "Hello world", "batched JSON data lines forward chat deltas");
}

void test_chat_sse_accepts_concatenated_json_payloads() {
    const std::string body =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}{\"choices\":[{\"delta\":{\"content\":\" world\"}}]}\n\n"
        "data: [DONE]\n\n";
    pkchat::provider::ChatResult result;
    std::string streamed;
    const pkchat::Error err = run_chat_stream_from_body(body, result, streamed);
    check(err.ok(), "chat SSE parser accepts concatenated JSON payloads");
    check(result.content == "Hello world", "concatenated JSON payloads accumulate chat deltas");
    check(streamed == "Hello world", "concatenated JSON payloads forward chat deltas");
}

void test_chat_sse_accepts_concatenated_reasoning_payloads() {
    const std::string body =
        "data: {\"choices\":[{\"delta\":{\"reasoning\":\"1+1 is addition.\"}}]}"
        "{\"choices\":[{\"delta\":{\"content\":\"2\"}}]}\n\n"
        "data: [DONE]\n\n";
    pkchat::provider::ChatResult result;
    std::string streamed;
    const pkchat::Error err = run_chat_stream_from_body(body, result, streamed);
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
    pkchat::provider::ChatResult result;
    std::string streamed;
    const pkchat::Error err = run_chat_stream_from_body(body, result, streamed);
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
    pkchat::provider::ChatResult result;
    std::string streamed;
    const pkchat::Error err = run_chat_stream_from_body(body, result, streamed);
    check(err.ok(), "chat SSE parser accepts concatenated payloads with leaked SSE metadata");
    check(result.content == "Hello world", "leaked SSE metadata payloads accumulate content");
}

void test_chat_sse_concatenated_payload_split_ignores_content_braces() {
    const std::string body =
        "data: {\"choices\":[{\"delta\":{\"content\":\"answer with {braces} and [brackets]\"}}]}"
        "{\"choices\":[{\"delta\":{\"content\":\" done\"}}]}\n\n"
        "data: [DONE]\n\n";
    pkchat::provider::ChatResult result;
    std::string streamed;
    const pkchat::Error err = run_chat_stream_from_body(body, result, streamed);
    check(err.ok(), "chat SSE concatenated payload split ignores braces inside content strings");
    check(result.content == "answer with {braces} and [brackets] done",
          "content braces are preserved while concatenated payloads are split");
}

void test_chat_sse_accepts_json_payload_immediately_followed_by_done() {
    const std::string body =
        "data: {\"choices\":[{\"delta\":{\"content\":\"2\"}}]}[DONE]ignored-after-done\n\n";
    pkchat::provider::ChatResult result;
    std::string streamed;
    const pkchat::Error err = run_chat_stream_from_body(body, result, streamed);
    check(err.ok(), "chat SSE parser accepts JSON payload immediately followed by DONE");
    check(result.content == "2", "JSON plus DONE payload accumulates content");
    check(streamed == "2", "JSON plus DONE payload forwards content");
}

void test_responses_sse_accepts_concatenated_json_payloads() {
    const std::string body =
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"Hello\"}"
        "{\"type\":\"response.output_text.delta\",\"delta\":\" world\"}"
        "{\"type\":\"response.completed\",\"response\":{\"model\":\"mock-model\",\"usage\":{\"input_tokens\":1,\"output_tokens\":2,\"total_tokens\":3}}}\n\n";
    pkchat::provider::ChatResult result;
    std::string streamed;
    const pkchat::Error err = run_responses_stream_from_body(body, result, streamed);
    check(err.ok(), "Responses SSE parser accepts concatenated JSON payloads");
    check(result.content == "Hello world", "concatenated Responses payloads accumulate deltas");
    check(streamed == "Hello world", "concatenated Responses payloads forward deltas");
    check(result.total_tokens == 3, "concatenated Responses payloads preserve completion metadata");
}

void test_explicit_chat_url_does_not_require_base_when_model_set() {
    const char* argv[] = {"pkchat", "--chat-url", "https://example.test/custom/chat", "-m", "model", "-p", "hello", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(9, const_cast<char**>(argv));
    check(parsed.error.ok(), "explicit chat URL args parse");
    pkchat::provider::ContextResult ctx = pkchat::provider::build_context(parsed.options);
    check(ctx.error.ok(), "explicit chat URL context builds without base URL when model is set");
    check(ctx.context.chat_url == "https://example.test/custom/chat", "explicit chat URL is preserved");
}

void test_image_capability_detection() {
    pkchat::provider::RequestContext context;
    context.api_kind = pkchat::provider::ApiKind::ChatCompletions;
    context.profile.name = "custom_openai_chat";
    context.profile.capabilities.images = true;
    context.options.model = "Qwen3.5-35B-A3B";
    check(pkchat::provider::detected_capabilities_for(context).images,
          "Qwen3.5 model is detected as image capable");
    check(pkchat::provider::validate_image_input(context).ok(),
          "detected vision model accepts image input");

    context.options.model = "unknown-text-model";
    check(!pkchat::provider::validate_image_input(context).ok(),
          "unknown model requires an explicit image capability decision");
    context.options.image_capability = "allow";
    check(pkchat::provider::validate_image_input(context).ok(),
          "explicit image capability override allows a compatible unknown model");
    context.api_kind = pkchat::provider::ApiKind::Responses;
    check(!pkchat::provider::validate_image_input(context).ok(),
          "Responses image input remains rejected until its request schema is implemented");
}

void test_lmstudio_context() {
    const char* argv[] = {"pkchat", "--provider", "lmstudio", "--list-models"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(4, const_cast<char**>(argv));
    check(parsed.error.ok(), "lmstudio args parse");
    pkchat::provider::ContextResult ctx = pkchat::provider::build_context(parsed.options);
    check(ctx.error.ok(), "lmstudio context builds without key");
    check(ctx.context.profile.name == "lm_studio", "lmstudio alias normalized");
    check(ctx.context.base_url == "http://localhost:1234/v1", "lmstudio default base URL");
}

void test_lmstudio_shortcut_context() {
    const char* argv[] = {"pkchat", "lmstudio", "-i"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(3, const_cast<char**>(argv));
    check(parsed.error.ok(), "lmstudio shortcut args parse");
    pkchat::provider::ContextResult ctx = pkchat::provider::build_context(parsed.options);
    check(ctx.error.ok(), "lmstudio shortcut context builds without key or model");
    check(ctx.context.profile.name == "lm_studio", "lmstudio shortcut selects profile");
    check(ctx.context.base_url == "http://localhost:1234/v1", "lmstudio shortcut uses default base URL");
    check(ctx.context.options.model.empty(), "lmstudio shortcut does not require model");
}

void test_none_provider_allows_an_empty_endpoint() {
    const char* argv[] = {"pkchat", "--provider", "none", "--repl"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(4, const_cast<char**>(argv));
    check(parsed.error.ok(), "none provider parses without a positional endpoint");
    check(parsed.options.positional_url.empty(), "none provider keeps the omitted endpoint empty");
    check(pkchat::provider::validate_profile_name(parsed.options.provider).ok(),
          "none is a recognized provider name in standalone modes");

    pkchat::provider::ContextResult context = pkchat::provider::build_context(parsed.options);
    check(context.error.ok(), "none provider context builds without an endpoint");
    check(context.context.profile.name == "none" && context.context.profile.offline,
          "none resolves to the offline provider profile");
    check(context.context.base_url.empty() && context.context.chat_url.empty() &&
              context.context.responses_url.empty() && context.context.models_url.empty(),
          "none provider leaves every model endpoint empty");
    check(!pkchat::provider::capabilities_for(context.context).chat_completions &&
              !pkchat::provider::capabilities_for(context.context).model_listing,
          "none provider advertises no model capabilities");

    pkchat::provider::ModelsResult models;
    pkchat::Error err = pkchat::provider::list_models(context.context, models);
    check(err.code == pkchat::ErrorCode::UnsupportedFeature,
          "none provider rejects model listing before transport");

    pkchat::provider::ChatResult chat;
    err = pkchat::provider::send_chat_messages(
        context.context, {{"user", "hello"}},
        [](const std::string&) { return pkchat::ok_error(); }, chat);
    check(err.code == pkchat::ErrorCode::UnsupportedFeature,
          "none provider rejects chat before transport");

    const char* alias_argv[] = {"pkchat", "offline", "--repl"};
    pkchat::cli::ParseResult alias = pkchat::cli::parse_args(3, const_cast<char**>(alias_argv));
    pkchat::provider::ContextResult alias_context = pkchat::provider::build_context(alias.options);
    check(alias_context.error.ok() && alias_context.context.profile.name == "none",
          "offline positional alias resolves without an endpoint");

    const char* endpoint_argv[] = {
        "pkchat", "--provider", "none", "--base-url", "http://localhost:1234", "--repl"};
    pkchat::cli::ParseResult endpoint =
        pkchat::cli::parse_args(6, const_cast<char**>(endpoint_argv));
    pkchat::provider::ContextResult endpoint_context =
        pkchat::provider::build_context(endpoint.options);
    check(endpoint_context.error.code == pkchat::ErrorCode::BadArgs,
          "none provider rejects model endpoint overrides");
}

void test_openai_context_allows_missing_model() {
    const char* argv[] = {"pkchat", "--provider", "openai", "-p", "hello", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(7, const_cast<char**>(argv));
    check(parsed.error.ok(), "openai args without model parse");
    pkchat::provider::ContextResult ctx = pkchat::provider::build_context(parsed.options);
    check(ctx.error.ok(), "openai context builds without model so caller can discover one");
    check(ctx.context.options.model.empty(), "openai context keeps missing model empty before discovery");
}

void test_openrouter_shortcut_context() {
    const char* argv[] = {"pkchat", "openrouter", "-model", "provider/model", "-p", "hello", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(8, const_cast<char**>(argv));
    check(parsed.error.ok(), "openrouter shortcut args parse");
    pkchat::provider::ContextResult ctx = pkchat::provider::build_context(parsed.options);
    check(ctx.error.ok(), "openrouter shortcut context builds with auth header");
    check(ctx.context.profile.name == "openrouter", "openrouter shortcut selects profile");
    check(ctx.context.base_url == "https://openrouter.ai/api/v1", "openrouter shortcut uses standard base URL");
}

void test_provider_capabilities_and_responses_context() {
    const char* argv[] = {"pkchat", "--provider", "openai", "--api", "responses", "-p", "hello", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(9, const_cast<char**>(argv));
    check(parsed.error.ok(), "OpenAI Responses args parse");
    pkchat::provider::ContextResult ctx = pkchat::provider::build_context(parsed.options);
    check(ctx.error.ok(), "OpenAI Responses context builds");
    check(ctx.context.api_kind == pkchat::provider::ApiKind::Responses, "Responses API kind selected");
    check(pkchat::provider::active_request_url(ctx.context) == "https://api.openai.com/v1/responses",
          "OpenAI Responses endpoint selected");
    check(pkchat::provider::capabilities_for(ctx.context).responses_api, "OpenAI reports Responses capability");
    check(pkchat::provider::capabilities_for(ctx.context).chat_completions, "OpenAI reports Chat Completions capability");

    const char* shortcut_argv[] = {"pkchat", "--provider", "openai_responses", "-p", "hello", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult shortcut = pkchat::cli::parse_args(7, const_cast<char**>(shortcut_argv));
    check(shortcut.error.ok(), "openai_responses profile shortcut args parse");
    pkchat::provider::ContextResult shortcut_ctx = pkchat::provider::build_context(shortcut.options);
    check(shortcut_ctx.error.ok(), "openai_responses context builds");
    check(shortcut_ctx.context.profile.name == "openai", "openai_responses uses OpenAI profile");
    check(shortcut_ctx.context.api_kind == pkchat::provider::ApiKind::Responses, "openai_responses selects Responses API");
}

void test_provider_registry_resolves_added_profiles() {
    std::vector<pkchat::provider::Profile> profiles = pkchat::provider::built_in_profiles();
    check(profiles.size() >= 24, "provider registry includes offline and compatibility profiles");

    const char* grok_argv[] = {"pkchat", "grok", "--list-models", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult grok = pkchat::cli::parse_args(5, const_cast<char**>(grok_argv));
    check(grok.error.ok(), "grok alias args parse");
    pkchat::provider::ContextResult grok_ctx = pkchat::provider::build_context(grok.options);
    check(grok_ctx.error.ok(), "grok alias context builds");
    check(grok_ctx.context.profile.name == "xai", "grok alias resolves to xai");
    check(grok_ctx.context.base_url == "https://api.x.ai/v1", "xai base URL selected");

    const char* kimi_argv[] = {"pkchat", "--provider", "kimi", "--list-models", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult kimi = pkchat::cli::parse_args(6, const_cast<char**>(kimi_argv));
    check(kimi.error.ok(), "kimi alias args parse");
    pkchat::provider::ContextResult kimi_ctx = pkchat::provider::build_context(kimi.options);
    check(kimi_ctx.error.ok(), "kimi alias context builds");
    check(kimi_ctx.context.profile.name == "moonshot", "kimi alias resolves to moonshot");

    const char* llama_argv[] = {"pkchat", "llama.cpp", "--list-models"};
    pkchat::cli::ParseResult llama = pkchat::cli::parse_args(3, const_cast<char**>(llama_argv));
    check(llama.error.ok(), "llama.cpp alias args parse");
    pkchat::provider::ContextResult llama_ctx = pkchat::provider::build_context(llama.options);
    check(llama_ctx.error.ok(), "llama.cpp alias context builds");
    check(llama_ctx.context.profile.name == "llamacpp", "llama.cpp alias resolves to llamacpp");
    check(llama_ctx.context.profile.local_endpoint, "llamacpp is marked local");

    const char* vllm_argv[] = {"pkchat", "vllm", "--list-models"};
    pkchat::cli::ParseResult vllm = pkchat::cli::parse_args(3, const_cast<char**>(vllm_argv));
    check(vllm.error.ok(), "vllm shortcut args parse");
    pkchat::provider::ContextResult vllm_ctx = pkchat::provider::build_context(vllm.options);
    check(vllm_ctx.error.ok(), "vllm context builds");
    check(vllm_ctx.context.api_key == "token-abc123", "vllm uses configured dummy API key");

    const char* deepinfra_argv[] = {"pkchat", "--provider", "deepinfra", "--list-models", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult deepinfra = pkchat::cli::parse_args(6, const_cast<char**>(deepinfra_argv));
    check(deepinfra.error.ok(), "deepinfra args parse");
    pkchat::provider::ContextResult deepinfra_ctx = pkchat::provider::build_context(deepinfra.options);
    check(deepinfra_ctx.error.ok(), "deepinfra context builds");
    check(deepinfra_ctx.context.profile.key_envs.size() >= 2 && deepinfra_ctx.context.profile.key_envs[1] == "DEEPINFRA_TOKEN",
          "deepinfra registers alternate token env var");

    const char* zai_argv[] = {"pkchat", "--provider", "z.ai", "--list-models", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult zai = pkchat::cli::parse_args(6, const_cast<char**>(zai_argv));
    check(zai.error.ok(), "Z.AI alias args parse");
    pkchat::provider::ContextResult zai_ctx = pkchat::provider::build_context(zai.options);
    check(zai_ctx.error.ok(), "Z.AI alias context builds");
    check(zai_ctx.context.profile.name == "zai", "z.ai alias resolves to zai");
    check(zai_ctx.context.base_url == "https://api.z.ai/api/paas/v4", "Z.AI base URL selected");
    check(zai_ctx.context.chat_url == "https://api.z.ai/api/paas/v4/chat/completions",
          "Z.AI Chat Completions URL selected");
    pkchat::provider::ModelsResult zai_models;
    pkchat::Error zai_models_error = pkchat::provider::list_models(zai_ctx.context, zai_models);
    check(zai_models_error.code == pkchat::ErrorCode::UnsupportedFeature,
          "Z.AI rejects undocumented model listing before transport");

    const char* qwen_argv[] = {"pkchat", "qwen", "--list-models", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult qwen = pkchat::cli::parse_args(5, const_cast<char**>(qwen_argv));
    check(qwen.error.ok(), "Qwen shortcut args parse");
    pkchat::provider::ContextResult qwen_ctx = pkchat::provider::build_context(qwen.options);
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
    const char* unsupported_argv[] = {"pkchat", "--provider", "openrouter", "--api", "responses", "-p", "hello", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult unsupported = pkchat::cli::parse_args(9, const_cast<char**>(unsupported_argv));
    check(unsupported.error.ok(), "unsupported Responses args parse");
    pkchat::provider::ContextResult unsupported_ctx = pkchat::provider::build_context(unsupported.options);
    check(!unsupported_ctx.error.ok(), "chat-only provider rejects built-in Responses API");
    check(unsupported_ctx.error.code == pkchat::ErrorCode::UnsupportedFeature, "Responses rejection uses unsupported feature error");

    const char* override_argv[] = {"pkchat", "--provider", "openrouter", "--api", "responses", "--responses-url", "https://example.test/v1/responses", "-p", "hello", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult override = pkchat::cli::parse_args(11, const_cast<char**>(override_argv));
    check(override.error.ok(), "Responses override args parse");
    pkchat::provider::ContextResult override_ctx = pkchat::provider::build_context(override.options);
    check(override_ctx.error.ok(), "Responses override context builds");
    check(override_ctx.context.responses_url == "https://example.test/v1/responses", "Responses override endpoint selected");
    check(pkchat::provider::capabilities_for(override_ctx.context).responses_api, "Responses override reports capability");
}

void test_provider_unicode_request_serialization() {
    pkchat::provider::RequestContext context;
    context.options.model = "mock-model";
    context.options.stream = false;
    const std::string unicode = u8"مرحبا 你好 👨‍👩‍👧‍👦";
    std::vector<pkchat::provider::Message> messages = {{"user", unicode}};
    const std::string request = pkchat::provider::serialize_chat_request(context, messages);
    pkchat::json::ParseResult parsed = pkchat::json::parse(request);
    check(parsed.error.ok(), "Unicode chat request serializes to valid JSON");
    const pkchat::json::Value* content = parsed.value.get("messages");
    content = content == nullptr ? nullptr : content->at(0);
    content = content == nullptr ? nullptr : content->get("content");
    check(content != nullptr && content->is_string() && content->string == unicode,
          "Unicode chat request preserves Arabic, Chinese, and emoji message text");
}

}  // namespace

void run_all() {
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
    test_lmstudio_context();
    test_lmstudio_shortcut_context();
    test_none_provider_allows_an_empty_endpoint();
    test_openai_context_allows_missing_model();
    test_openrouter_shortcut_context();
    test_provider_unicode_request_serialization();
    test_provider_capabilities_and_responses_context();
    test_provider_registry_resolves_added_profiles();
    test_provider_responses_unsupported_and_override();
}

}  // namespace pkchat::test::provider
