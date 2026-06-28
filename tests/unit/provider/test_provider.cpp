#include "provider/test_provider.hpp"
#include "support/test_support.hpp"
#include "cli/args.hpp"
#include "json/json.hpp"
#include "provider/provider.hpp"
#include <string>
#include <vector>

namespace pkchat::test::provider {

namespace {

using pkchat::test::check;
using pkchat::test::read_fixture;

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
