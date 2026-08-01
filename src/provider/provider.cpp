#include "chat/settings.hpp"
#include "config/model_catalog.hpp"
#include "provider/names.hpp"
#include "provider/provider.hpp"

#include <chrono>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <set>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <utility>

#include "json/json.hpp"
#include "output/thinking.hpp"
#include "security/redact.hpp"

namespace ainiux::provider {

namespace {

std::uint64_t mix_session_bits(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

bool emits_openrouter_session_id(const RequestContext& context) {
    auto is_official_url = [](std::string value) {
        while (value.size() > 1 && value.back() == '/') value.pop_back();
        for (const char* official : {
                 "https://openrouter.ai/api/v1",
                 "https://eu.openrouter.ai/api/v1"}) {
            const std::string base = official;
            if (value == base ||
                (value.size() > base.size() &&
                 value.compare(0, base.size(), base) == 0 &&
                 value[base.size()] == '/'))
                return true;
        }
        return false;
    };
    return normalize_provider_key(context.profile.name) == "openrouter" &&
           !context.routing_session_id.empty() &&
           (is_official_url(context.base_url) ||
            is_official_url(context.chat_url) ||
            is_official_url(context.responses_url));
}

Capabilities profile_capabilities(bool requires_key,
                                  bool local,
                                  bool chat_completions,
                                  bool responses_api,
                                  bool model_listing) {
    Capabilities caps;
    caps.chat_completions = chat_completions;
    caps.responses_api = responses_api;
    caps.streaming = chat_completions || responses_api;
    caps.model_listing = model_listing;
    caps.usage_reporting = chat_completions || responses_api;
    caps.tool_calls = chat_completions || responses_api;
    caps.requires_bearer_key = requires_key;
    caps.optional_bearer_key = !requires_key;
    caps.custom_headers = true;
    caps.local_endpoint = local;
    return caps;
}

Profile make_profile(const std::string& name,
                     const std::vector<std::string>& aliases,
                     const std::string& base_url,
                     const std::string& chat_path,
                     const std::string& models_path,
                     const std::string& responses_path,
                     const std::vector<std::string>& key_envs,
                     bool requires_key,
                     bool local,
                     const std::string& dummy_api_key = "",
                     const std::string& warning = "") {
    Profile profile;
    profile.name = name;
    profile.aliases = aliases;
    profile.base_url = base_url;
    profile.chat_path = chat_path;
    profile.models_path = models_path;
    profile.responses_path = responses_path;
    profile.requires_bearer_key = requires_key;
    profile.local_endpoint = local;
    profile.key_envs = key_envs;
    profile.dummy_api_key = dummy_api_key;
    profile.compatibility_warning = warning;
    profile.capabilities = profile_capabilities(requires_key, local, !chat_path.empty(), !responses_path.empty(), !models_path.empty());
    profile.capabilities.images = local || name == names::kOpenAi || name == "openrouter" || name == "gemini" ||
                                  name == "xai" || name == "mistral" || name == "qwen" ||
                                  name == names::kCustomOpenAiChat;
    return profile;
}

Profile make_offline_profile() {
    Profile profile;
    profile.name = names::kNone;
    profile.aliases = {names::kOffline};
    profile.base_url.clear();
    profile.chat_path.clear();
    profile.responses_path.clear();
    profile.models_path.clear();
    profile.offline = true;
    profile.capabilities = {};
    return profile;
}

Profile with_credit_endpoint(Profile profile, const std::string& credit_url) {
    profile.credit_url = credit_url;
    profile.capabilities.credit_balance = !credit_url.empty();
    return profile;
}

const std::vector<Profile>& profile_registry() {
    static const std::vector<Profile> profiles = {
        make_offline_profile(),
        with_credit_endpoint(
            make_profile("openrouter", {}, "https://openrouter.ai/api/v1",
                         "/chat/completions", "/models", "",
                         {"OPENROUTER_API_KEY", "AINIUX_API_KEY"}, true, false),
            "https://openrouter.ai/api/v1/credits"),
        with_credit_endpoint(
            make_profile(names::kOpenAi,
                         {names::kOpenAiChat, names::kOpenAiResponses},
                         "https://api.openai.com/v1",
                         "/chat/completions",
                         "/models",
                         "/responses",
                         {"OPENAI_API_KEY", "AINIUX_API_KEY"},
                         true,
                         false),
            "https://api.openai.com/v1/dashboard/billing/credit_grants"),
        with_credit_endpoint(
            make_profile("deepseek", {}, "https://api.deepseek.com",
                         "/chat/completions", "/models", "",
                         {"DEEPSEEK_API_KEY", "AINIUX_API_KEY"}, true, false),
            "https://api.deepseek.com/user/balance"),
        make_profile("gemini", {}, "https://generativelanguage.googleapis.com/v1beta/openai", "/chat/completions", "/models", "", {"GEMINI_API_KEY", "AINIUX_API_KEY"}, true, false),
        make_profile("anthropic", {}, "https://api.anthropic.com/v1", "/chat/completions", "/models", "", {"ANTHROPIC_API_KEY", "AINIUX_API_KEY"}, true, false, "", "OpenAI compatibility layer is mainly for testing/comparison."),
        make_profile("xai", {"grok"}, "https://api.x.ai/v1", "/chat/completions", "/models", "", {"XAI_API_KEY", "AINIUX_API_KEY"}, true, false),
        make_profile("moonshot", {"kimi"}, "https://api.moonshot.ai/v1", "/chat/completions", "/models", "", {"MOONSHOT_API_KEY", "AINIUX_API_KEY"}, true, false),
        // Local OpenAI-compatible servers (after kimi): llama.cpp, LM Studio, Ollama, vLLM, SGLang.
        make_profile(names::kLlamacpp, {"llama_cpp", "llama.cpp"}, "http://localhost:8080/v1", "/chat/completions", "/models", "", {}, false, true),
        make_profile(names::kLmStudio,
                     {names::kLmStudioAlias},
                     "http://localhost:1234/v1",
                     "/chat/completions",
                     "/models",
                     "",
                     {"LMSTUDIO_API_KEY", "LM_STUDIO_API_KEY", "AINIUX_API_KEY"},
                     false,
                     true),
        make_profile(names::kOllama, {}, "http://localhost:11434/v1", "/chat/completions", "/models", "", {}, false, true),
        make_profile(names::kVllm, {}, "http://localhost:8000/v1", "/chat/completions", "/models", "", {}, false, true, "token-abc123"),
        make_profile(names::kSglang, {"sg_lang", "sg-lang"}, "http://localhost:30000/v1", "/chat/completions", "/models", "", {}, false, true),
        make_profile("groq", {}, "https://api.groq.com/openai/v1", "/chat/completions", "/models", "", {"GROQ_API_KEY", "AINIUX_API_KEY"}, true, false),
        make_profile("mistral", {}, "https://api.mistral.ai/v1", "/chat/completions", "/models", "", {"MISTRAL_API_KEY", "AINIUX_API_KEY"}, true, false),
        make_profile("together", {}, "https://api.together.ai/v1", "/chat/completions", "/models", "", {"TOGETHER_API_KEY", "AINIUX_API_KEY"}, true, false),
        make_profile("perplexity", {}, "https://api.perplexity.ai", "/chat/completions", "/models", "", {"PERPLEXITY_API_KEY", "AINIUX_API_KEY"}, true, false, "", "Perplexity canonical Sonar endpoint is /v1/sonar; /chat/completions is the OpenAI SDK-compatible alias."),
        make_profile("cerebras", {}, "https://api.cerebras.ai/v1", "/chat/completions", "/models", "", {"CEREBRAS_API_KEY", "AINIUX_API_KEY"}, true, false),
        make_profile("fireworks", {}, "https://api.fireworks.ai/inference/v1", "/chat/completions", "/models", "", {"FIREWORKS_API_KEY", "AINIUX_API_KEY"}, true, false),
        make_profile("deepinfra", {}, "https://api.deepinfra.com/v1/openai", "/chat/completions", "/models", "", {"DEEPINFRA_API_KEY", "DEEPINFRA_TOKEN", "AINIUX_API_KEY"}, true, false),
        make_profile("nvidia_nim", {}, "https://integrate.api.nvidia.com/v1", "/chat/completions", "/models", "", {"NVIDIA_NIM_API_KEY", "AINIUX_API_KEY"}, true, false),
        make_profile("zai", {"z.ai", "z_ai"}, "https://api.z.ai/api/paas/v4", "/chat/completions", "", "", {"ZAI_API_KEY", "AINIUX_API_KEY"}, true, false),
        make_profile("qwen", {"dashscope_intl"}, "https://dashscope-intl.aliyuncs.com/compatible-mode/v1", "/chat/completions", "/models", "", {"DASHSCOPE_API_KEY", "QWEN_API_KEY", "AINIUX_API_KEY"}, true, false),
        make_profile("dashscope", {}, "https://dashscope.aliyuncs.com/compatible-mode/v1", "/chat/completions", "/models", "", {"DASHSCOPE_API_KEY", "AINIUX_API_KEY"}, true, false),
        make_profile(names::kCustomOpenAiChat,
                     {names::kCustom},
                     "",
                     "/chat/completions",
                     "/models",
                     "/responses",
                     {"AINIUX_API_KEY"},
                     false,
                     false),
    };
    return profiles;
}

std::string provider_lookup_name(const std::string& requested) {
    const std::string name = normalize_provider_key(requested.empty() ? names::kOpenAi : requested);
    if (name == names::kOpenAiResponses) {
        return names::kOpenAi;
    }
    return name;
}

bool provider_requests_responses(const std::string& requested) {
    return normalize_provider_key(requested) == names::kOpenAiResponses;
}

bool find_profile(const std::string& requested, Profile& out) {
    const std::string name = provider_lookup_name(requested);
    for (const Profile& profile : profile_registry()) {
        if (normalize_provider_key(profile.name) == name) {
            out = profile;
            return true;
        }
        for (const std::string& alias : profile.aliases) {
            if (normalize_provider_key(alias) == name) {
                out = profile;
                return true;
            }
        }
    }
    return false;
}

bool is_provider_shortcut(const std::string& text) {
    Profile ignored;
    return find_profile(text, ignored);
}

bool has_authorization_header(const std::vector<std::string>& headers) {
    for (const std::string& header : headers) {
        const size_t colon = header.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        if (is_sensitive_header_name(ascii_trim(header.substr(0, colon))) &&
            normalize_provider_key(ascii_trim(header.substr(0, colon))) == "authorization") {
            return true;
        }
    }
    return false;
}

Error read_file(const std::string& path, std::string& out) {
    if (path == "-") {
        std::ostringstream ss;
        ss << std::cin.rdbuf();
        out = ss.str();
        return ok_error();
    }
    const std::string resolved = expand_user_path(path);
    std::ifstream file(resolved, std::ios::binary);
    if (!file) {
        return {ErrorCode::FileRead, "could not open file for reading: " + resolved};
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    if (file.bad()) {
        return {ErrorCode::FileRead, "could not read file: " + resolved};
    }
    out = ss.str();
    return ok_error();
}

std::string resolve_key(const cli::Options& options, const Profile& profile) {
    if (!options.key.empty()) {
        return options.key;
    }
    if (!options.key_env.empty()) {
        const char* value = std::getenv(options.key_env.c_str());
        return value == nullptr ? "" : std::string(value);
    }
    for (const std::string& env : profile.key_envs) {
        const char* value = std::getenv(env.c_str());
        if (value != nullptr && *value != '\0') {
            return std::string(value);
        }
    }
    return profile.dummy_api_key;
}

std::string join_url(const std::string& base, const std::string& suffix) {
    if (base.empty()) {
        return suffix;
    }
    if (!base.empty() && base.back() == '/') {
        return base.substr(0, base.size() - 1) + suffix;
    }
    return base + suffix;
}

Error validate_header(const std::string& header) {
    const size_t colon = header.find(':');
    if (colon == std::string::npos || colon == 0) {
        return {ErrorCode::BadArgs, "--header expects a value like \"Name: Value\""};
    }
    return ok_error();
}

std::string strip_thinking_blocks_for_request(const std::string& content);


std::string append_pair(std::string fields, const std::string& key, const std::string& value_json) {
    if (!fields.empty()) fields += ",";
    fields += json::quote(key) + ":" + value_json;
    return fields;
}

std::string reasoning_scalar_json(const ReasoningSelection& selection) {
    if (selection.kind == ReasoningSelectionKind::TokenBudget) {
        return std::to_string(selection.tokens);
    }
    return json::quote(selection.value);
}

ReasoningProtocol fallback_reasoning_protocol(const RequestContext& context) {
    const std::string profile = normalize_provider_key(context.profile.name);
    if (profile == names::kNone || context.profile.offline) return ReasoningProtocol::None;
    if (profile == names::kOpenAi) return ReasoningProtocol::OpenAiEffort;
    if (profile == "openrouter") return ReasoningProtocol::OpenRouter;
    if (profile == "anthropic") return ReasoningProtocol::AnthropicBudget;
    if (profile == "gemini") return ReasoningProtocol::GeminiEffort;
    if (profile == "moonshot") return ReasoningProtocol::KimiEffort;
    if (profile == "qwen" || profile == "dashscope") {
        return context.api_kind == ApiKind::Responses ? ReasoningProtocol::QwenResponses
                                                      : ReasoningProtocol::QwenChat;
    }
    if (profile == "deepseek") return ReasoningProtocol::DeepSeek;
    if (profile == "zai") return ReasoningProtocol::Zai;
    if (profile == "xai") return ReasoningProtocol::XaiEffort;
    if (profile.empty() || profile == names::kCustomOpenAiChat || context.profile.local_endpoint) {
        return ReasoningProtocol::GenericThinking;
    }
    return ReasoningProtocol::None;
}

const ModelCapability* matched_model_capability_impl(const RequestContext& context) {
    const std::string api = context.api_kind == ApiKind::Responses ? "responses" : "chat";
    return config::resolve_model_capability(context.options.model_catalog,
                                            context.profile.name,
                                            api,
                                            context.options.model);
}

ReasoningProtocol reasoning_protocol_for(const RequestContext& context) {
    // A router's wire format is determined by the transport even when the
    // matched catalog entry describes the model's native provider.
    if (normalize_provider_key(context.profile.name) == "openrouter") {
        return ReasoningProtocol::OpenRouter;
    }
    const ModelCapability* capability = matched_model_capability_impl(context);
    return capability == nullptr ? fallback_reasoning_protocol(context)
                                 : capability->reasoning_protocol;
}

std::string reasoning_effort_object(const ReasoningSelection& selection) {
    return "{\"effort\":" + reasoning_scalar_json(selection) + "}";
}

bool reasoning_selection_is_plain_enable(const ReasoningSelection& selection) {
    if (selection.kind != ReasoningSelectionKind::Named) return false;
    const std::string value = ascii_lower(selection.value);
    return value == "enabled" || value == "enable" || value == "on" ||
           value == "true" || value == "yes";
}

std::string reasoning_fields_json(const RequestContext& context) {
    const ReasoningSelection& selection = context.options.reasoning;
    if (selection.is_auto()) return {};
    const bool disabled = config::reasoning_selection_disables(selection);
    const bool semantic_off = selection.kind == ReasoningSelectionKind::Named &&
                              ascii_lower(selection.value) == "off";
    const ReasoningProtocol protocol = reasoning_protocol_for(context);
    ReasoningSelection wire_selection = selection;
    if (semantic_off) {
        if (protocol == ReasoningProtocol::AnthropicBudget) {
            wire_selection = ReasoningSelection::token_budget(0);
        } else if (protocol == ReasoningProtocol::Hy3Template) {
            wire_selection = ReasoningSelection::named("no_think");
        } else {
            wire_selection = ReasoningSelection::named("none");
        }
    }
    const std::string scalar = reasoning_scalar_json(wire_selection);
    std::string fields;
    switch (protocol) {
        case ReasoningProtocol::None:
            return {};
        case ReasoningProtocol::GenericThinking:
            fields = append_pair(fields, "enable_thinking", disabled ? "false" : "true");
            return append_pair(fields, "thinking_budget", disabled ? "0" : scalar);
        case ReasoningProtocol::OpenAiEffort:
            if (context.api_kind == ApiKind::Responses) {
                return append_pair(fields, "reasoning", reasoning_effort_object(wire_selection));
            }
            return append_pair(fields, "reasoning_effort", scalar);
        case ReasoningProtocol::OpenRouter:
            if (semantic_off) {
                return append_pair(fields, "reasoning", "{\"enabled\":false}");
            }
            if (selection.kind == ReasoningSelectionKind::TokenBudget) {
                return append_pair(fields, "reasoning", "{\"max_tokens\":" + scalar + "}");
            }
            return append_pair(fields, "reasoning", reasoning_effort_object(selection));
        case ReasoningProtocol::GeminiEffort:
        case ReasoningProtocol::KimiEffort:
            return append_pair(fields, "reasoning_effort", scalar);
        case ReasoningProtocol::XaiEffort:
            return append_pair(fields, "reasoning", reasoning_effort_object(selection));
        case ReasoningProtocol::GeminiThinkingLevel:
            return append_pair(fields,
                               "generation_config",
                               "{\"thinking_level\":" + scalar + "}");
        case ReasoningProtocol::GemmaThinkingLevel:
            fields = "\"chat_template_kwargs\":{\"enable_thinking\":";
            fields += disabled ? "false" : "true";
            fields += "}";
            return fields;
        case ReasoningProtocol::AnthropicBudget:
            if (disabled) return append_pair(fields, "thinking", "{\"type\":\"disabled\"}");
            return append_pair(fields,
                               "thinking",
                               "{\"type\":\"enabled\",\"budget_tokens\":" + scalar + "}");
        case ReasoningProtocol::ThinkingToggle:
            return append_pair(
                fields,
                "thinking",
                "{\"type\":" +
                    json::quote(disabled ? "disabled"
                                         : selection.kind == ReasoningSelectionKind::Named &&
                                                   selection.value != "enabled" &&
                                                   selection.value != "on"
                                               ? selection.value
                                               : "enabled") +
                    "}");
        case ReasoningProtocol::QwenChat:
            fields = "\"chat_template_kwargs\":{\"enable_thinking\":";
            fields += disabled ? "false" : "true";
            fields += "}";
            return fields;
        case ReasoningProtocol::QwenResponses:
        case ReasoningProtocol::MiniMaxResponses:
            return append_pair(fields, "reasoning", reasoning_effort_object(selection));
        case ReasoningProtocol::DeepSeek:
        case ReasoningProtocol::Zai:
            fields = append_pair(fields,
                                 "thinking",
                                 std::string("{\"type\":\"") +
                                     (disabled ? "disabled" : "enabled") + "\"}");
            if (!disabled) fields = append_pair(fields, "reasoning_effort", scalar);
            return fields;
        case ReasoningProtocol::NemotronTemplate:
            fields = "\"chat_template_kwargs\":{\"enable_thinking\":";
            fields += disabled ? "false" : "true";
            if (!disabled && !reasoning_selection_is_plain_enable(selection) &&
                (selection.kind == ReasoningSelectionKind::TokenBudget ||
                 selection.kind == ReasoningSelectionKind::Named)) {
                fields += ",\"reasoning_budget\":" + scalar;
            }
            fields += "}";
            return fields;
        case ReasoningProtocol::Hy3Template:
            return append_pair(fields,
                               "extra_body",
                               "{\"chat_template_kwargs\":{\"reasoning_effort\":" + scalar + "}}");
    }
    return {};
}

void append_sampling_fields(std::ostringstream& json, const cli::Options& o) {
    if (o.has_temperature) {
        json << ",\"temperature\":" << o.temperature;
    }
    if (o.has_top_p) {
        json << ",\"top_p\":" << o.top_p;
    }
    if (o.has_top_k) {
        json << ",\"top_k\":" << o.top_k;
    }
    if (o.has_min_p) {
        json << ",\"min_p\":" << o.min_p;
    }
    if (o.has_repeat_penalty) {
        json << ",\"repeat_penalty\":" << o.repeat_penalty;
    }
    if (o.has_presence_penalty) {
        json << ",\"presence_penalty\":" << o.presence_penalty;
    }
    if (o.has_max_output_tokens) {
        json << ",\"max_tokens\":" << o.max_output_tokens;
    }
}

std::string build_chat_request_json(const RequestContext& context, const std::vector<Message>& messages) {
    const cli::Options& o = context.options;
    std::ostringstream json;
    json << "{";
    if (!o.model.empty()) {
        json << "\"model\":" << json::quote(o.model) << ",";
    }
    if (emits_openrouter_session_id(context)) {
        json << "\"session_id\":" << json::quote(context.routing_session_id) << ",";
    }
    json << "\"messages\":[";
    for (size_t i = 0; i < messages.size(); ++i) {
        if (i != 0) {
            json << ",";
        }
        const std::string content = messages[i].role == "assistant"
                                        ? strip_thinking_blocks_for_request(messages[i].content)
                                        : messages[i].content;
        json << "{\"role\":" << json::quote(messages[i].role) << ",\"content\":";
        if (messages[i].images.empty()) {
            json << json::quote(content);
        } else {
            json << "[";
            bool wrote_part = false;
            if (!content.empty()) {
                json << "{\"type\":\"text\",\"text\":" << json::quote(content) << "}";
                wrote_part = true;
            }
            for (const ImageInput& image : messages[i].images) {
                if (wrote_part) {
                    json << ",";
                }
                const std::string data_url =
                    "data:" + image.mime_type + ";base64," + image.base64_data;
                json << "{\"type\":\"image_url\",\"image_url\":{\"url\":"
                     << json::quote(data_url) << "}}";
                wrote_part = true;
            }
            json << "]";
        }
        json << "}";
    }
    json << "],";
    json << "\"stream\":" << (o.stream ? "true" : "false");
    append_sampling_fields(json, o);
    const std::string reasoning_fields = reasoning_fields_json(context);
    if (!reasoning_fields.empty()) {
        json << "," << reasoning_fields;
    }
    json << "}";
    return json.str();
}

std::string responses_instructions_from_messages(const std::vector<Message>& messages) {
    std::string instructions;
    for (const Message& message : messages) {
        if (message.role != "system") {
            continue;
        }
        if (!instructions.empty()) {
            instructions += "\n\n";
        }
        instructions += message.content;
    }
    return instructions;
}

std::string build_responses_request_json(const RequestContext& context, const std::vector<Message>& messages) {
    const cli::Options& o = context.options;
    std::ostringstream out;
    out << "{";
    bool wrote = false;
    auto comma = [&]() {
        if (wrote) {
            out << ",";
        }
        wrote = true;
    };
    if (!o.model.empty()) {
        comma();
        out << "\"model\":" << json::quote(o.model);
    }
    if (emits_openrouter_session_id(context)) {
        comma();
        out << "\"session_id\":" << json::quote(context.routing_session_id);
    }
    const std::string instructions = responses_instructions_from_messages(messages);
    if (!instructions.empty()) {
        comma();
        out << "\"instructions\":" << json::quote(instructions);
    }
    comma();
    out << "\"input\":[";
    bool first_message = true;
    for (const Message& message : messages) {
        if (message.role == "system") {
            continue;
        }
        if (!first_message) {
            out << ",";
        }
        first_message = false;
        const std::string content = message.role == "assistant"
                                        ? strip_thinking_blocks_for_request(message.content)
                                        : message.content;
        if (message.role == "assistant") {
            out << "{\"type\":\"message\",\"role\":\"assistant\",\"status\":\"completed\",\"content\":[{\"type\":\"output_text\",\"text\":"
                << json::quote(content) << ",\"annotations\":[]}]}";
        } else {
            out << "{\"role\":\"user\",\"content\":" << json::quote(content) << "}";
        }
    }
    out << "]";
    comma();
    out << "\"stream\":" << (o.stream ? "true" : "false");
    comma();
    out << "\"store\":false";
    if (o.has_temperature) {
        comma();
        out << "\"temperature\":" << o.temperature;
    }
    if (o.has_top_p) {
        comma();
        out << "\"top_p\":" << o.top_p;
    }
    if (o.has_top_k) {
        comma();
        out << "\"top_k\":" << o.top_k;
    }
    if (o.has_min_p) {
        comma();
        out << "\"min_p\":" << o.min_p;
    }
    if (o.has_repeat_penalty) {
        comma();
        out << "\"repeat_penalty\":" << o.repeat_penalty;
    }
    if (o.has_presence_penalty) {
        comma();
        out << "\"presence_penalty\":" << o.presence_penalty;
    }
    if (o.has_max_output_tokens) {
        comma();
        out << "\"max_output_tokens\":" << o.max_output_tokens;
    }
    const std::string reasoning_fields = reasoning_fields_json(context);
    if (!reasoning_fields.empty()) {
        comma();
        out << reasoning_fields;
    }
    out << "}";
    return out.str();
}

std::string provider_error_message(const json::Value& root);

std::string http_status_summary(long status) {
    switch (status) {
        case 400:
            return "Bad request. The provider rejected the request format or parameters.";
        case 401:
            return "Authentication failed. Check that the API key is set for this provider.";
        case 402:
            return "Payment is required. Check the provider account balance, billing status, or quota.";
        case 403:
            return "Access forbidden. The API key may not have access to this model or endpoint.";
        case 404:
            return "Endpoint or model was not found. Check the base URL, endpoint path, and model name.";
        case 408:
            return "Request timed out. Try again later or use a smaller request.";
        case 409:
            return "Request conflict. The provider could not process this request in its current state.";
        case 413:
            return "Request is too large. Shorten the prompt, reduce attachments, or use a model with a larger context window.";
        case 415:
            return "Unsupported content type. Check whether this provider supports the requested input or attachment type.";
        case 422:
            return "Request was understood but rejected. Check model-specific options, tools, reasoning settings, and attachment support.";
        case 423:
            return "Resource is locked. Try again later.";
        case 425:
            return "Provider asked to retry later. Try the request again after a short wait.";
        case 429:
            return "Too many requests. Perhaps you have used your daily limits? Try again later or use a different model or provider.";
        case 500:
            return "Provider server error. Try again later or use a different model or provider.";
        case 502:
            return "Bad gateway from the provider. The upstream model service may be temporarily unavailable.";
        case 503:
            return "Provider service is unavailable or overloaded. Try again later or use a different model or provider.";
        case 504:
            return "Provider gateway timed out. Try again later, reduce the request size, or use a different model.";
        case 507:
            return "Provider ran out of storage for this request. Try again later or use a smaller request.";
        default:
            if (status >= 400 && status < 500) {
                return "Client request was rejected by the provider. Check the model, provider, API key, and request options.";
            }
            if (status >= 500 && status < 600) {
                return "Provider service failed while handling the request. Try again later or use a different provider.";
            }
            return "Unexpected HTTP status from the provider.";
    }
}

std::string compact_provider_detail(std::string text) {
    text = ascii_trim(std::move(text));
    for (char& ch : text) {
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            ch = ' ';
        }
    }
    if (text.size() > 500) {
        text = text.substr(0, 500) + "...";
    }
    return text;
}

std::string provider_body_detail(const std::string& body) {
    if (ascii_trim(body).empty()) {
        return "";
    }
    const json::ParseResult parsed = json::parse(body);
    if (parsed.error.ok()) {
        return compact_provider_detail(provider_error_message(parsed.value));
    }
    std::string text = compact_provider_detail(body);
    if (!text.empty() && text.front() != '{' && text.front() != '[') {
        return text;
    }
    return "";
}

Error http_status_error(const RequestContext& context, const http::Response& response, const std::string& url) {
    ErrorCode code = ErrorCode::HttpStatus;
    if (response.status == 401 || response.status == 403) {
        code = ErrorCode::Auth;
    } else if (response.status == 429) {
        code = ErrorCode::RateLimit;
    }
    std::string message = "HTTP " + std::to_string(response.status) + ": " +
                          http_status_summary(response.status) + "\nURL: " + url;
    const std::string provider_detail = provider_body_detail(response.body);
    if (!provider_detail.empty()) {
        message += "\nProvider message: " + provider_detail;
    }
    std::string suggestion;
    if (context.profile.local_endpoint && response.status == 0) {
        suggestion = "Start the local provider server and verify the port.";
    } else if (response.status == 404) {
        suggestion = "Check whether the server expects /v1, /api/v1, or an explicit --chat-url/--responses-url/--models-url.";
    } else if (response.status == 401 || response.status == 403) {
        suggestion = "Verify the API key, selected provider, account/project access, and model name.";
    } else if (response.status == 413) {
        suggestion = "Reduce prompt length, attached files, or max output tokens.";
    } else if (response.status == 422) {
        suggestion = "Try removing provider-specific options such as reasoning, tools, images, or unsupported sampling settings.";
    }
    if (!suggestion.empty()) {
        message += "\nSuggestion: " + suggestion;
    }
    return {code, message};
}

std::string provider_error_message(const json::Value& root) {
    auto message_from_error_object = [](const json::Value& err) -> std::string {
        if (err.is_string()) return err.string;
        if (!err.is_object()) return "";
        if (const json::Value* msg = err.get("message")) {
            if (msg->is_string() && !msg->string.empty()) return msg->string;
        }
        if (const json::Value* msg = err.get("detail")) {
            if (msg->is_string() && !msg->string.empty()) return msg->string;
        }
        return "";
    };

    const json::Value* err = root.get("error");
    if (err != nullptr) {
        std::string top = message_from_error_object(*err);
        // OpenRouter often wraps the real upstream (Gemini/Google) message as a
        // JSON string in error.metadata.raw. Prefer that when the top message is
        // the generic "Provider returned error".
        if (err->is_object()) {
            const json::Value* metadata = err->get("metadata");
            if (metadata != nullptr && metadata->is_object()) {
                const json::Value* raw = metadata->get("raw");
                if (raw != nullptr && raw->is_string() && !raw->string.empty()) {
                    const json::ParseResult nested = json::parse(raw->string);
                    if (nested.error.ok()) {
                        std::string nested_msg;
                        if (const json::Value* nested_err = nested.value.get("error"))
                            nested_msg = message_from_error_object(*nested_err);
                        if (nested_msg.empty()) nested_msg = message_from_error_object(nested.value);
                        if (!nested_msg.empty()) {
                            const std::string provider_name =
                                (metadata->get("provider_name") != nullptr &&
                                 metadata->get("provider_name")->is_string())
                                    ? metadata->get("provider_name")->string
                                    : std::string{};
                            if (!provider_name.empty())
                                return provider_name + ": " + nested_msg;
                            return nested_msg;
                        }
                    } else if (top.empty() || top == "Provider returned error") {
                        return compact_provider_detail(raw->string);
                    }
                }
            }
        }
        if (!top.empty()) return top;
    }
    if (const json::Value* msg = root.get("message")) {
        if (msg->is_string()) {
            return msg->string;
        }
    }
    if (const json::Value* msg = root.get("detail")) {
        if (msg->is_string()) {
            return msg->string;
        }
    }
    return "";
}

bool contains_think_tag(const std::string& text) {
    const std::string tag = "<think>";
    auto lower_ascii = [](char ch) {
        if (ch >= 'A' && ch <= 'Z') {
            return static_cast<char>(ch - 'A' + 'a');
        }
        return ch;
    };
    for (std::size_t pos = 0; pos + tag.size() <= text.size(); ++pos) {
        bool match = true;
        for (std::size_t i = 0; i < tag.size(); ++i) {
            if (lower_ascii(text[pos + i]) != tag[i]) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

std::string normalize_provider_text(std::string text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                out.push_back('\n');
                ++i;
            } else {
                out.push_back('\n');
            }
            continue;
        }
        out.push_back(ch);
    }
    return out;
}

std::string reasoning_detail_type(const json::Value& item) {
    const json::Value* type = item.get("type");
    return type != nullptr && type->is_string() ? type->string : std::string();
}

std::string reasoning_detail_text_from_item(const json::Value& item) {
    if (item.is_string()) {
        return item.string;
    }
    const std::string type = reasoning_detail_type(item);
    if (type == "reasoning.encrypted") {
        return "";
    }
    if (const json::Value* text = item.get("text")) {
        if (text->is_string()) {
            return text->string;
        }
    }
    if (const json::Value* summary = item.get("summary")) {
        if (summary->is_string()) {
            return summary->string;
        }
    }
    return "";
}

void append_reasoning_details_text(const json::Value& details, std::string& out) {
    if (!details.is_array()) {
        return;
    }
    std::string previous_type;
    for (const json::Value& item : details.array) {
        const std::string text = reasoning_detail_text_from_item(item);
        if (text.empty()) {
            continue;
        }
        const std::string type = reasoning_detail_type(item);
        const bool direct_concat = type == "reasoning.text";
        if (!out.empty()) {
            if (direct_concat && previous_type == "reasoning.text") {
                // OpenRouter streams reasoning.text fragments without separators.
            } else if (out.back() != '\n') {
                out.push_back('\n');
            }
        }
        out += text;
        previous_type = type;
    }
}

std::string reasoning_text_from_object(const json::Value& object) {
    std::string reasoning;
    if (const json::Value* value = object.get("reasoning_content")) {
        if (value->is_string()) {
            reasoning = value->string;
        }
    }
    if (reasoning.empty()) {
        if (const json::Value* value = object.get("reasoning")) {
            if (value->is_string()) {
                reasoning = value->string;
            }
        }
    }
    if (!reasoning.empty()) {
        return normalize_provider_text(std::move(reasoning));
    }
    if (const json::Value* details = object.get("reasoning_details")) {
        append_reasoning_details_text(*details, reasoning);
    }
    return normalize_provider_text(std::move(reasoning));
}

std::string content_with_reasoning_trace(const std::string& reasoning, const std::string& content) {
    if (reasoning.empty() || contains_think_tag(content)) {
        return content;
    }
    return "<think>" + reasoning + "</think>" + (content.empty() ? std::string() : std::string("\n\n") + content);
}

std::string strip_thinking_blocks_for_request(const std::string& content) {
    const std::string open_tag = "<think>";
    const std::string close_tag = "</think>";
    auto lower_ascii = [](char ch) {
        if (ch >= 'A' && ch <= 'Z') {
            return static_cast<char>(ch - 'A' + 'a');
        }
        return ch;
    };
    auto tag_at = [&](std::size_t pos, const std::string& tag) {
        if (pos + tag.size() > content.size()) {
            return false;
        }
        for (std::size_t i = 0; i < tag.size(); ++i) {
            if (lower_ascii(content[pos + i]) != tag[i]) {
                return false;
            }
        }
        return true;
    };
    auto find_tag = [&](const std::string& tag, std::size_t start) {
        for (std::size_t pos = start; pos + tag.size() <= content.size(); ++pos) {
            if (tag_at(pos, tag)) {
                return pos;
            }
        }
        return std::string::npos;
    };
    auto trim_outer_newlines = [](std::string& text) {
        while (!text.empty() && (text.front() == '\n' || text.front() == '\r')) {
            text.erase(text.begin());
        }
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
    };

    std::string out;
    bool in_thinking = false;
    std::size_t pos = 0;
    bool stripped = false;
    while (pos < content.size()) {
        if (!in_thinking) {
            const std::size_t open = find_tag(open_tag, pos);
            if (open == std::string::npos) {
                out.append(content, pos, std::string::npos);
                break;
            }
            out.append(content, pos, open - pos);
            pos = open + open_tag.size();
            in_thinking = true;
            stripped = true;
            continue;
        }
        const std::size_t close = find_tag(close_tag, pos);
        if (close == std::string::npos) {
            break;
        }
        pos = close + close_tag.size();
        in_thinking = false;
    }
    if (stripped) {
        trim_outer_newlines(out);
    }
    return out;
}

bool unwrap_model_object_key(const std::string& key) {
    return key == "meta" || key == "details";
}

bool parse_integral_string(const std::string& text, long long& out) {
    if (text.empty()) {
        return false;
    }
    std::size_t index = 0;
    try {
        out = std::stoll(text, &index);
    } catch (...) {
        return false;
    }
    return index == text.size();
}

std::string format_with_thousands(long long value) {
    const bool negative = value < 0;
    unsigned long long magnitude = static_cast<unsigned long long>(negative ? -value : value);
    std::string digits = std::to_string(magnitude);
    std::string formatted;
    formatted.reserve(digits.size() + digits.size() / 3 + 1);
    if (negative) {
        formatted.push_back('-');
    }
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i != 0 && (digits.size() - i) % 3 == 0) {
            formatted.push_back(',');
        }
        formatted.push_back(digits[i]);
    }
    return formatted;
}

std::string format_token_count_value(const std::string& raw) {
    long long tokens = 0;
    if (!parse_integral_string(raw, tokens) || tokens < 0) {
        return raw;
    }
    return format_with_thousands(tokens) + " tokens";
}

std::string format_parameter_count_value(const std::string& raw) {
    long long params = 0;
    if (!parse_integral_string(raw, params) || params < 0) {
        return raw;
    }
    const double billion = 1'000'000'000.0;
    const double million = 1'000'000.0;
    const double thousand = 1'000.0;
    std::ostringstream ss;
    ss << std::fixed;
    if (params >= static_cast<long long>(billion)) {
        ss << std::setprecision(1) << static_cast<double>(params) / billion << "B";
    } else if (params >= static_cast<long long>(million)) {
        ss << std::setprecision(1) << static_cast<double>(params) / million << "M";
    } else if (params >= static_cast<long long>(thousand)) {
        ss << std::setprecision(1) << static_cast<double>(params) / thousand << "K";
    } else {
        return format_with_thousands(params);
    }
    return ss.str();
}

std::string format_byte_size_value(const std::string& raw) {
    long long bytes = 0;
    if (!parse_integral_string(raw, bytes) || bytes < 0) {
        return raw;
    }
    const double gib = 1024.0 * 1024.0 * 1024.0;
    const double mib = 1024.0 * 1024.0;
    const double kib = 1024.0;
    std::ostringstream ss;
    ss << std::fixed;
    if (bytes >= static_cast<long long>(gib)) {
        ss << std::setprecision(2) << static_cast<double>(bytes) / gib << " GiB";
    } else if (bytes >= static_cast<long long>(mib)) {
        ss << std::setprecision(2) << static_cast<double>(bytes) / mib << " MiB";
    } else if (bytes >= static_cast<long long>(kib)) {
        ss << std::setprecision(1) << static_cast<double>(bytes) / kib << " KiB";
    } else {
        return format_with_thousands(bytes) + " B";
    }
    return ss.str();
}

std::string model_attribute_display(const json::Value& value) {
    switch (value.type) {
        case json::Value::Type::String:
            return value.string;
        case json::Value::Type::Bool:
            return value.boolean ? "true" : "false";
        case json::Value::Type::Number: {
            const double whole = std::floor(value.number);
            if (value.number == whole && whole >= 0.0 &&
                whole <= static_cast<double>(std::numeric_limits<long long>::max())) {
                return std::to_string(static_cast<long long>(whole));
            }
            std::ostringstream ss;
            ss << value.number;
            return ss.str();
        }
        default:
            return "";
    }
}

std::string format_model_created_value(const std::string& value) {
    if (value.empty()) {
        return value;
    }
    bool digits_only = true;
    for (char ch : value) {
        if (ch < '0' || ch > '9') {
            digits_only = false;
            break;
        }
    }
    if (!digits_only) {
        return value;
    }
    long long seconds = 0;
    try {
        seconds = std::stoll(value);
    } catch (...) {
        return value;
    }
    if (seconds <= 0) {
        return value;
    }
    const std::time_t timestamp = static_cast<std::time_t>(seconds);
    std::tm utc{};
    if (gmtime_r(&timestamp, &utc) == nullptr) {
        return value;
    }
    char buffer[32] = {};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S UTC", &utc) == 0) {
        return value;
    }
    return std::string(buffer) + " (" + value + ")";
}

void flatten_model_value(const std::string& key, const json::Value& value, std::map<std::string, std::string>& out) {
    if (value.is_object()) {
        const bool unwrap = unwrap_model_object_key(key);
        for (const auto& entry : value.object) {
            if (unwrap) {
                flatten_model_value(entry.first, entry.second, out);
            } else if (key.empty()) {
                flatten_model_value(entry.first, entry.second, out);
            } else {
                flatten_model_value(key + "." + entry.first, entry.second, out);
            }
        }
        return;
    }
    if (value.is_array()) {
        if (key.empty()) {
            return;
        }
        std::vector<std::string> parts;
        for (const json::Value& item : value.array) {
            if (item.is_string()) {
                if (!item.string.empty()) {
                    parts.push_back(item.string);
                }
            } else {
                const std::string display = model_attribute_display(item);
                if (!display.empty()) {
                    parts.push_back(display);
                }
            }
        }
        if (!parts.empty()) {
            std::ostringstream joined;
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if (i != 0) {
                    joined << ", ";
                }
                joined << parts[i];
            }
            out.emplace(key, joined.str());
        }
        return;
    }
    const std::string display = model_attribute_display(value);
    if (!display.empty() && !key.empty()) {
        out.emplace(key, display);
    }
}

std::string format_model_attribute_value(const std::string& key, const std::string& raw) {
    if (key == "created") {
        return format_model_created_value(raw);
    }
    if (key == "n_ctx" || key == "n_ctx_train" || key == "context_length" || key == "max_model_len" ||
        key == "max_context_length" || key == "max_tokens") {
        return format_token_count_value(raw);
    }
    if (key == "n_params" || key == "parameter_count") {
        return format_parameter_count_value(raw);
    }
    if (key == "size") {
        return format_byte_size_value(raw);
    }
    if (key == "n_vocab" || key == "n_embd") {
        long long value = 0;
        if (parse_integral_string(raw, value) && value >= 0) {
            return format_with_thousands(value);
        }
    }
    return raw;
}

std::string markdown_table_cell(const std::string& text) {
    std::string output;
    output.reserve(text.size());
    for (char ch : text) {
        if (ch == '|') {
            output += "\\|";
        } else if (ch == '\n') {
            output += "<br>";
        } else if (ch != '\r') {
            output.push_back(ch);
        }
    }
    return output;
}

std::string model_attribute_heading(const std::string& key) {
    if (key == "id") {
        return "ID";
    }
    if (key == "owned_by") {
        return "Owned by";
    }
    if (key == "created") {
        return "Created";
    }
    if (key == "n_ctx" || key == "context_length" || key == "max_model_len" || key == "max_context_length") {
        return "Context";
    }
    if (key == "n_ctx_train") {
        return "Train context";
    }
    if (key == "n_params" || key == "parameter_count") {
        return "Parameters";
    }
    if (key == "n_vocab") {
        return "Vocab";
    }
    if (key == "n_embd") {
        return "Embedding dim";
    }
    if (key == "size") {
        return "Model size";
    }
    if (key == "aliases") {
        return "Aliases";
    }
    if (key == "capabilities") {
        return "Capabilities";
    }
    if (key == "architecture") {
        return "Architecture";
    }
    if (key == "modality") {
        return "Modality";
    }
    std::string heading;
    heading.reserve(key.size());
    bool capitalize_next = true;
    for (char ch : key) {
        if (ch == '_') {
            heading.push_back(' ');
            capitalize_next = true;
            continue;
        }
        if (capitalize_next && ch >= 'a' && ch <= 'z') {
            heading.push_back(static_cast<char>(ch - 'a' + 'A'));
            capitalize_next = false;
            continue;
        }
        heading.push_back(ch);
        capitalize_next = false;
    }
    return heading;
}

void append_parsed_model(ModelInfo info, ModelsResult& result) {
    if (info.id.empty()) {
        return;
    }
    result.model_ids.push_back(info.id);
    result.models.push_back(std::move(info));
}

void append_models_from_array(const json::Value& array, ModelsResult& result) {
    for (const json::Value& item : array.array) {
        if (!item.is_object()) {
            continue;
        }
        ModelInfo info;
        if (const json::Value* id = item.get("id")) {
            if (id->is_string()) {
                info.id = id->string;
            }
        }
        if (info.id.empty()) {
            if (const json::Value* name = item.get("name")) {
                if (name->is_string()) {
                    info.id = name->string;
                }
            }
        }
        if (info.id.empty()) {
            if (const json::Value* model = item.get("model")) {
                if (model->is_string()) {
                    info.id = model->string;
                }
            }
        }
        if (info.id.empty()) {
            continue;
        }
        for (const auto& entry : item.object) {
            if (entry.first == "id") {
                continue;
            }
            flatten_model_value(entry.first, entry.second, info.attributes);
        }
        append_parsed_model(std::move(info), result);
    }
}

Error parse_models_json(const std::string& body, ModelsResult& result) {
    json::ParseResult parsed = json::parse(body);
    if (!parsed.error.ok()) {
        return parsed.error;
    }
    if (const std::string provider_msg = provider_error_message(parsed.value); !provider_msg.empty()) {
        return {ErrorCode::ProviderSchema, "provider error: " + provider_msg};
    }
    result.models.clear();
    result.model_ids.clear();
    if (const json::Value* data = parsed.value.get("data")) {
        if (data->is_array()) {
            append_models_from_array(*data, result);
        }
    }
    if (result.models.empty()) {
        if (const json::Value* models = parsed.value.get("models")) {
            if (models->is_array()) {
                append_models_from_array(*models, result);
            }
        }
    }
    if (result.models.empty()) {
        return {ErrorCode::ProviderSchema, "models response did not contain any models"};
    }
    return ok_error();
}

long long usage_token_value(const json::Value& usage, const std::string& name) {
    const json::Value* value = usage.get(name);
    if (value == nullptr || value->type != json::Value::Type::Number || value->number < 0.0 ||
        value->number >= static_cast<double>(std::numeric_limits<long long>::max())) {
        return -1;
    }
    return static_cast<long long>(value->number);
}

long long nested_usage_token_value(const json::Value& usage,
                                   const std::string& object_name,
                                   const std::string& name) {
    const json::Value* object = usage.get(object_name);
    return object != nullptr && object->is_object()
               ? usage_token_value(*object, name)
               : -1;
}

void parse_usage(const json::Value& usage, ChatResult& result) {
    if (!usage.is_object()) {
        return;
    }
    result.usage_json = json::stringify(usage);
    result.prompt_tokens = usage_token_value(usage, "prompt_tokens");
    if (result.prompt_tokens < 0) {
        result.prompt_tokens = usage_token_value(usage, "input_tokens");
    }
    result.cache_read_tokens = usage_token_value(usage, "prompt_cache_hit_tokens");
    result.fresh_prompt_tokens = usage_token_value(usage, "prompt_cache_miss_tokens");
    result.cache_write_tokens = usage_token_value(usage, "cache_write_tokens");
    for (const char* details_name : {"prompt_tokens_details", "input_tokens_details"}) {
        if (result.cache_read_tokens < 0)
            result.cache_read_tokens =
                nested_usage_token_value(usage, details_name, "cached_tokens");
        if (result.cache_write_tokens < 0)
            result.cache_write_tokens =
                nested_usage_token_value(usage, details_name, "cache_write_tokens");
    }
    if (result.fresh_prompt_tokens < 0 && result.prompt_tokens >= 0) {
        result.fresh_prompt_tokens =
            result.cache_read_tokens >= 0 && result.cache_read_tokens <= result.prompt_tokens
                ? result.prompt_tokens - result.cache_read_tokens
                : result.cache_read_tokens < 0 ? result.prompt_tokens : -1;
    }
    const long long completion_tokens = [&] {
        const long long chat_tokens = usage_token_value(usage, "completion_tokens");
        return chat_tokens >= 0 ? chat_tokens : usage_token_value(usage, "output_tokens");
    }();
    if (completion_tokens >= 0) {
        result.completion_tokens = completion_tokens;
        result.completion_tokens_estimated = false;
    }
    result.total_tokens = usage_token_value(usage, "total_tokens");
    if (result.total_tokens < 0 && result.prompt_tokens >= 0 && completion_tokens >= 0 &&
        result.prompt_tokens <= std::numeric_limits<long long>::max() - completion_tokens) {
        result.total_tokens = result.prompt_tokens + completion_tokens;
    }
}

Error parse_chat_json(const std::string& body, ChatResult& result) {
    json::ParseResult parsed = json::parse(body);
    if (!parsed.error.ok()) {
        return parsed.error;
    }
    if (const std::string provider_msg = provider_error_message(parsed.value); !provider_msg.empty()) {
        return {ErrorCode::ProviderSchema, "provider error: " + provider_msg};
    }
    if (const json::Value* model = parsed.value.get("model")) {
        if (model->is_string()) {
            result.model = model->string;
        }
    }
    const json::Value* choices = parsed.value.get("choices");
    if (choices == nullptr || !choices->is_array() || choices->array.empty()) {
        return {ErrorCode::ProviderSchema, "chat response did not contain choices[0]"};
    }
    const json::Value* message = choices->array[0].get("message");
    if (message == nullptr) {
        return {ErrorCode::ProviderSchema, "chat response did not contain choices[0].message"};
    }
    const std::string reasoning = reasoning_text_from_object(*message);
    const json::Value* content = message->get("content");
    std::string content_text;
    if (content != nullptr && content->is_string()) {
        content_text = normalize_provider_text(content->string);
    } else if (reasoning.empty()) {
        return {ErrorCode::ProviderSchema, "chat response did not contain choices[0].message.content"};
    }
    result.content = content_with_reasoning_trace(reasoning, content_text);
    if (const json::Value* usage = parsed.value.get("usage")) {
        parse_usage(*usage, result);
    }
    return ok_error();
}

void append_text_with_separator(std::string& out, const std::string& text) {
    if (text.empty()) {
        return;
    }
    if (!out.empty()) {
        out.push_back('\n');
    }
    out += text;
}

void append_responses_content_text(const json::Value& content, std::string& out) {
    if (content.is_string()) {
        append_text_with_separator(out, content.string);
        return;
    }
    if (!content.is_array()) {
        return;
    }
    for (const json::Value& item : content.array) {
        if (item.is_string()) {
            append_text_with_separator(out, item.string);
            continue;
        }
        const json::Value* text = item.get("text");
        if (text != nullptr && text->is_string()) {
            append_text_with_separator(out, text->string);
        }
    }
}

void append_responses_reasoning_text(const json::Value& item, std::string& reasoning) {
    if (const json::Value* summary = item.get("summary")) {
        append_reasoning_details_text(*summary, reasoning);
    }
    if (const json::Value* content = item.get("content")) {
        append_reasoning_details_text(*content, reasoning);
    }
    if (const json::Value* text = item.get("text")) {
        if (text->is_string()) {
            if (!reasoning.empty() && reasoning.back() != '\n') {
                reasoning.push_back('\n');
            }
            reasoning += text->string;
        }
    }
}

Error parse_responses_json_value(const json::Value& root, ChatResult& result, bool require_content) {
    if (const std::string provider_msg = provider_error_message(root); !provider_msg.empty()) {
        return {ErrorCode::ProviderSchema, "provider error: " + provider_msg};
    }
    if (const json::Value* model = root.get("model")) {
        if (model->is_string()) {
            result.model = model->string;
        }
    }
    std::string content_text;
    std::string reasoning;
    if (const json::Value* output_text = root.get("output_text")) {
        if (output_text->is_string()) {
            content_text = output_text->string;
        }
    }
    if (const json::Value* output = root.get("output")) {
        if (output->is_array()) {
            for (const json::Value& item : output->array) {
                const json::Value* type = item.get("type");
                const bool is_reasoning = type != nullptr && type->is_string() && type->string == "reasoning";
                if (is_reasoning) {
                    append_responses_reasoning_text(item, reasoning);
                    continue;
                }
                if (const json::Value* content = item.get("content")) {
                    append_responses_content_text(*content, content_text);
                }
            }
        }
    }
    if (!content_text.empty() || !reasoning.empty()) {
        result.content = content_with_reasoning_trace(reasoning, content_text);
    } else if (require_content) {
        return {ErrorCode::ProviderSchema, "Responses API response did not contain output text"};
    }
    if (const json::Value* usage = root.get("usage")) {
        parse_usage(*usage, result);
    }
    return ok_error();
}

Error parse_responses_json(const std::string& body, ChatResult& result) {
    json::ParseResult parsed = json::parse(body);
    if (!parsed.error.ok()) {
        return parsed.error;
    }
    return parse_responses_json_value(parsed.value, result, true);
}

long long estimate_completion_tokens(const std::string& text) {
    long long tokens = 0;
    bool in_ascii_word = false;
    for (unsigned char ch : text) {
        if (std::isalnum(ch) || ch == 95) {
            if (!in_ascii_word) {
                ++tokens;
                in_ascii_word = true;
            }
            continue;
        }
        in_ascii_word = false;
        if (!std::isspace(ch) && ch < 128) {
            ++tokens;
        }
    }
    if (tokens == 0 && !text.empty()) {
        tokens = 1;
    }
    return tokens;
}

bool choice_stream_finished(const json::Value& choice) {
    const json::Value* finish_reason = choice.get("finish_reason");
    if (finish_reason == nullptr || finish_reason->is_null()) {
        return false;
    }
    if (!finish_reason->is_string()) {
        return false;
    }
    return !finish_reason->string.empty();
}

bool find_sse_event_boundary(const std::string& buffer,
                             size_t start,
                             size_t& event_end,
                             size_t& next_pos) {
    size_t line_start = start;
    while (line_start < buffer.size()) {
        size_t line_end = line_start;
        while (line_end < buffer.size() && buffer[line_end] != '\n' && buffer[line_end] != '\r') {
            ++line_end;
        }
        if (line_end == buffer.size()) {
            return false;
        }

        size_t after_line = line_end + 1;
        if (buffer[line_end] == '\r' && after_line < buffer.size() && buffer[after_line] == '\n') {
            ++after_line;
        }
        if (line_end == line_start) {
            event_end = line_start;
            next_pos = after_line;
            return true;
        }
        line_start = after_line;
    }
    return false;
}

std::vector<std::string> collect_sse_data_lines(const std::string& event) {
    std::vector<std::string> data_lines;
    size_t pos = 0;
    while (pos < event.size()) {
        size_t line_end = pos;
        while (line_end < event.size() && event[line_end] != '\n' && event[line_end] != '\r') {
            ++line_end;
        }
        const std::string line = event.substr(pos, line_end - pos);
        if (!line.empty() && line[0] != ':' && line.rfind("data:", 0) == 0) {
            std::string value = line.substr(5);
            if (!value.empty() && value.front() == ' ') {
                value.erase(value.begin());
            }
            data_lines.push_back(std::move(value));
        }
        if (line_end == event.size()) {
            break;
        }
        pos = line_end + 1;
        if (event[line_end] == '\r' && pos < event.size() && event[pos] == '\n') {
            ++pos;
        }
    }
    return data_lines;
}

std::string join_sse_data_lines(const std::vector<std::string>& data_lines) {
    std::string data;
    for (const std::string& line : data_lines) {
        if (!data.empty()) {
            data.push_back('\n');
        }
        data += line;
    }
    return data;
}

bool is_ascii_ws(char ch) {
    return ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t';
}

void skip_ascii_ws(const std::string& text, size_t& pos) {
    while (pos < text.size() && (is_ascii_ws(text[pos]) || text[pos] == '\x1e')) {
        ++pos;
    }
}

bool starts_with_at(const std::string& text, size_t pos, const std::string& prefix) {
    return pos <= text.size() && text.compare(pos, prefix.size(), prefix) == 0;
}

std::string sanitized_payload_preview(const std::string& text, size_t pos, size_t limit) {
    std::ostringstream out;
    size_t emitted = 0;
    for (size_t i = pos; i < text.size() && emitted < limit; ++i, ++emitted) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        switch (ch) {
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '"':
                out << "\\\"";
                break;
            default:
                if (ch < 0x20 || ch == 0x7f) {
                    out << "\\x" << std::hex << std::setw(2) << std::setfill('0')
                        << static_cast<int>(ch) << std::dec << std::setfill(' ');
                } else {
                    out << static_cast<char>(ch);
                }
                break;
        }
    }
    if (pos + emitted < text.size()) {
        out << "...";
    }
    return out.str();
}

void skip_to_next_line(const std::string& text, size_t& pos) {
    while (pos < text.size() && text[pos] != '\n' && text[pos] != '\r') {
        ++pos;
    }
    if (pos < text.size()) {
        ++pos;
        if (pos < text.size() && text[pos - 1] == '\r' && text[pos] == '\n') {
            ++pos;
        }
    }
}

bool skip_leaked_sse_field_prefix(const std::string& text, size_t& pos) {
    if (starts_with_at(text, pos, "data:")) {
        pos += 5;
        if (pos < text.size() && text[pos] == ' ') {
            ++pos;
        }
        return true;
    }
    if (starts_with_at(text, pos, "event:") ||
        starts_with_at(text, pos, "id:") ||
        starts_with_at(text, pos, "retry:") ||
        starts_with_at(text, pos, ":")) {
        skip_to_next_line(text, pos);
        return true;
    }
    return false;
}

void skip_sse_payload_separators(const std::string& text, size_t& pos) {
    while (true) {
        skip_ascii_ws(text, pos);
        if (!skip_leaked_sse_field_prefix(text, pos)) {
            return;
        }
    }
}

bool split_concatenated_sse_payloads(const std::string& data,
                                     std::vector<std::string>& payloads,
                                     size_t& failure_pos) {
    payloads.clear();
    size_t pos = 0;
    while (true) {
        skip_sse_payload_separators(data, pos);
        if (pos >= data.size()) {
            failure_pos = pos;
            break;
        }
        if (starts_with_at(data, pos, "[DONE]")) {
            payloads.emplace_back("[DONE]");
            failure_pos = pos + 6;
            return true;
        }

        const json::ParsePrefixResult parsed = json::parse_prefix(data, pos);
        if (!parsed.error.ok() || parsed.consumed <= pos) {
            failure_pos = parsed.consumed > 0 ? parsed.consumed : pos;
            payloads.clear();
            return false;
        }
        payloads.push_back(data.substr(pos, parsed.consumed - pos));
        pos = parsed.consumed;
    }
    return !payloads.empty();
}

std::string sse_payload_tail_context(const std::string& data) {
    json::ParsePrefixResult parsed = json::parse_prefix(data);
    size_t pos = parsed.error.ok() ? parsed.consumed : 0;
    skip_sse_payload_separators(data, pos);
    if (pos >= data.size()) {
        pos = parsed.error.ok() ? parsed.consumed : 0;
    }
    return "; SSE payload tail near byte " + std::to_string(pos) + ": \"" +
           sanitized_payload_preview(data, pos, 96) + "\"";
}

Error sse_json_parse_error(const std::string& message, const std::string& data) {
    return {ErrorCode::SseParse,
            message + "; SSE payload bytes: " + std::to_string(data.size()) +
                "; SSE payload head: \"" + sanitized_payload_preview(data, 0, 220) + "\"" +
                sse_payload_tail_context(data)};
}

class SseParser {
   public:
    explicit SseParser(bool suppress_streaming_reasoning = false)
        : suppress_streaming_reasoning_(suppress_streaming_reasoning) {}

    Error feed(const std::string& chunk, const DeltaCallback& on_delta, ChatResult& result, bool& done) {
        buffer_ += chunk;
        size_t pos = 0;
        while (true) {
            size_t event_end = 0;
            size_t next_pos = 0;
            if (!find_sse_event_boundary(buffer_, pos, event_end, next_pos)) {
                buffer_.erase(0, pos);
                return ok_error();
            }
            const std::string event = buffer_.substr(pos, event_end - pos);
            pos = next_pos;
            Error err = process_event(event, on_delta, result, done);
            if (!err.ok() || done) {
                buffer_.erase(0, pos);
                return err;
            }
        }
    }

    Error finish(const DeltaCallback& on_delta, ChatResult& result, bool& done) {
        if (!buffer_.empty()) {
            std::string event = buffer_;
            buffer_.clear();
            Error err = process_event(event, on_delta, result, done);
            if (!err.ok()) {
                return err;
            }
        }
        if (!done) {
            return close_reasoning(on_delta, result.content, false);
        }
        return ok_error();
    }

   private:
    std::string buffer_;
    bool reasoning_open_ = false;
    bool suppress_streaming_reasoning_ = false;

    Error emit_text(const std::string& text,
                    const DeltaCallback& on_delta,
                    std::string& accumulated,
                    bool forward_to_callback) {
        if (text.empty()) {
            return ok_error();
        }
        accumulated += text;
        if (forward_to_callback) {
            return on_delta(text);
        }
        return ok_error();
    }

    Error emit_reasoning(const std::string& text, const DeltaCallback& on_delta, std::string& accumulated) {
        if (text.empty()) {
            return ok_error();
        }
        const bool forward = !suppress_streaming_reasoning_;
        if (!reasoning_open_) {
            Error err = emit_text("<think>", on_delta, accumulated, forward);
            if (!err.ok()) {
                return err;
            }
            reasoning_open_ = true;
        }
        return emit_text(text, on_delta, accumulated, forward);
    }

    Error close_reasoning(const DeltaCallback& on_delta, std::string& accumulated, bool before_content) {
        if (!reasoning_open_) {
            return ok_error();
        }
        reasoning_open_ = false;
        return emit_text(before_content ? "</think>\n\n" : "</think>",
                         on_delta,
                         accumulated,
                         !suppress_streaming_reasoning_);
    }

    Error process_event(const std::string& event,
                        const DeltaCallback& on_delta,
                        ChatResult& result,
                        bool& done) {
        const std::vector<std::string> data_lines = collect_sse_data_lines(event);
        if (data_lines.empty()) {
            return ok_error();
        }
        const std::string data = join_sse_data_lines(data_lines);
        return process_payload(data, on_delta, result, done);
    }

    Error process_payload(const std::string& data,
                          const DeltaCallback& on_delta,
                          ChatResult& result,
                          bool& done) {
        std::vector<std::string> payloads;
        size_t failure_pos = 0;
        if (split_concatenated_sse_payloads(data, payloads, failure_pos)) {
            Error err = ok_error();
            for (const std::string& payload : payloads) {
                err = process_data(payload, on_delta, result, done);
                if (!err.ok() && err.code == ErrorCode::SseParse) {
                    std::vector<std::string> nested_payloads;
                    size_t nested_failure = 0;
                    if (split_concatenated_sse_payloads(payload, nested_payloads, nested_failure) &&
                        !(nested_payloads.size() == 1 && nested_payloads.front() == payload)) {
                        err = ok_error();
                        for (const std::string& nested : nested_payloads) {
                            err = process_data(nested, on_delta, result, done);
                            if (!err.ok() || done) {
                                return err;
                            }
                        }
                    }
                }
                if (!err.ok() || done) {
                    return err;
                }
            }
            return ok_error();
        }

        Error err = process_data(data, on_delta, result, done);
        if (!err.ok() && err.code == ErrorCode::SseParse) {
            if (data.size() > failure_pos) {
                err.message += "; SSE payload tail near byte " + std::to_string(failure_pos) + ": \"" +
                               sanitized_payload_preview(data, failure_pos, 96) + "\"";
            } else {
                err.message += sse_payload_tail_context(data);
            }
        }
        return err;
    }

    Error process_data(const std::string& data,
                       const DeltaCallback& on_delta,
                       ChatResult& result,
                       bool& done) {
        if (data.empty()) {
            return ok_error();
        }
        if (data == "[DONE]") {
            Error err = close_reasoning(on_delta, result.content, false);
            if (!err.ok()) {
                return err;
            }
            done = true;
            return ok_error();
        }
        json::ParseResult parsed = json::parse(data);
        if (!parsed.error.ok()) {
            return sse_json_parse_error(parsed.error.message, data);
        }
        if (const std::string provider_msg = provider_error_message(parsed.value); !provider_msg.empty()) {
            return {ErrorCode::ProviderSchema, "provider error: " + provider_msg};
        }
        if (const json::Value* model = parsed.value.get("model"); model != nullptr && model->is_string()) {
            result.model = model->string;
        }
        if (const json::Value* usage = parsed.value.get("usage")) {
            parse_usage(*usage, result);
        }
        const json::Value* choices = parsed.value.get("choices");
        if (choices == nullptr || !choices->is_array() || choices->array.empty()) {
            return ok_error();
        }
        const json::Value& choice = choices->array[0];
        const json::Value* delta = choice.get("delta");
        if (delta != nullptr) {
            const std::string reasoning = reasoning_text_from_object(*delta);
            Error err = emit_reasoning(reasoning, on_delta, result.content);
            if (!err.ok()) {
                return err;
            }
            const json::Value* content = delta->get("content");
            if (content != nullptr && content->is_string() && !content->string.empty()) {
                const std::string content_text = normalize_provider_text(content->string);
                const bool content_has_think_tag = contains_think_tag(content_text);
                err = close_reasoning(on_delta, result.content, !content_has_think_tag);
                if (!err.ok()) {
                    return err;
                }
                err = emit_text(content_text, on_delta, result.content, true);
                if (!err.ok()) {
                    return err;
                }
            }
        }
        if (choice_stream_finished(choice)) {
            Error err = close_reasoning(on_delta, result.content, false);
            if (!err.ok()) {
                return err;
            }
            done = true;
        }
        return ok_error();
    }
};

class ResponsesSseParser {
   public:
    explicit ResponsesSseParser(bool suppress_streaming_reasoning = false)
        : suppress_streaming_reasoning_(suppress_streaming_reasoning) {}

    Error feed(const std::string& chunk, const DeltaCallback& on_delta, ChatResult& result, bool& done) {
        buffer_ += chunk;
        size_t pos = 0;
        while (true) {
            size_t event_end = 0;
            size_t next_pos = 0;
            if (!find_sse_event_boundary(buffer_, pos, event_end, next_pos)) {
                buffer_.erase(0, pos);
                return ok_error();
            }
            const std::string event = buffer_.substr(pos, event_end - pos);
            pos = next_pos;
            Error err = process_event(event, on_delta, result, done);
            if (!err.ok() || done) {
                buffer_.erase(0, pos);
                return err;
            }
        }
    }

    Error finish(const DeltaCallback& on_delta, ChatResult& result, bool& done) {
        if (!buffer_.empty()) {
            std::string event = buffer_;
            buffer_.clear();
            Error err = process_event(event, on_delta, result, done);
            if (!err.ok()) {
                return err;
            }
        }
        return close_reasoning(on_delta, result, false);
    }

   private:
    std::string buffer_;
    bool reasoning_open_ = false;
    bool suppress_streaming_reasoning_ = false;

    Error emit_text(const std::string& text,
                    const DeltaCallback& on_delta,
                    ChatResult& result,
                    bool forward_to_callback) {
        if (text.empty()) {
            return ok_error();
        }
        result.content += text;
        if (forward_to_callback) {
            return on_delta(text);
        }
        return ok_error();
    }

    Error emit_reasoning(const std::string& text, const DeltaCallback& on_delta, ChatResult& result) {
        if (text.empty()) {
            return ok_error();
        }
        const bool forward = !suppress_streaming_reasoning_;
        if (!reasoning_open_) {
            Error err = emit_text("<think>", on_delta, result, forward);
            if (!err.ok()) {
                return err;
            }
            reasoning_open_ = true;
        }
        return emit_text(text, on_delta, result, forward);
    }

    Error close_reasoning(const DeltaCallback& on_delta, ChatResult& result, bool before_content) {
        if (!reasoning_open_) {
            return ok_error();
        }
        reasoning_open_ = false;
        return emit_text(before_content ? "</think>\n\n" : "</think>",
                         on_delta,
                         result,
                         !suppress_streaming_reasoning_);
    }

    static bool ends_with(const std::string& text, const std::string& suffix) {
        return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    Error process_event(const std::string& event,
                        const DeltaCallback& on_delta,
                        ChatResult& result,
                        bool& done) {
        const std::vector<std::string> data_lines = collect_sse_data_lines(event);
        if (data_lines.empty()) {
            return ok_error();
        }
        const std::string data = join_sse_data_lines(data_lines);
        return process_payload(data, on_delta, result, done);
    }

    Error process_payload(const std::string& data,
                          const DeltaCallback& on_delta,
                          ChatResult& result,
                          bool& done) {
        std::vector<std::string> payloads;
        size_t failure_pos = 0;
        if (split_concatenated_sse_payloads(data, payloads, failure_pos)) {
            Error err = ok_error();
            for (const std::string& payload : payloads) {
                err = process_data(payload, on_delta, result, done);
                if (!err.ok() && err.code == ErrorCode::SseParse) {
                    std::vector<std::string> nested_payloads;
                    size_t nested_failure = 0;
                    if (split_concatenated_sse_payloads(payload, nested_payloads, nested_failure) &&
                        !(nested_payloads.size() == 1 && nested_payloads.front() == payload)) {
                        err = ok_error();
                        for (const std::string& nested : nested_payloads) {
                            err = process_data(nested, on_delta, result, done);
                            if (!err.ok() || done) {
                                return err;
                            }
                        }
                    }
                }
                if (!err.ok() || done) {
                    return err;
                }
            }
            return ok_error();
        }

        Error err = process_data(data, on_delta, result, done);
        if (!err.ok() && err.code == ErrorCode::SseParse) {
            if (data.size() > failure_pos) {
                err.message += "; SSE payload tail near byte " + std::to_string(failure_pos) + ": \"" +
                               sanitized_payload_preview(data, failure_pos, 96) + "\"";
            } else {
                err.message += sse_payload_tail_context(data);
            }
        }
        return err;
    }

    Error process_data(const std::string& data,
                       const DeltaCallback& on_delta,
                       ChatResult& result,
                       bool& done) {
        if (data.empty()) {
            return ok_error();
        }
        if (data == "[DONE]") {
            Error err = close_reasoning(on_delta, result, false);
            if (!err.ok()) {
                return err;
            }
            done = true;
            return ok_error();
        }
        json::ParseResult parsed = json::parse(data);
        if (!parsed.error.ok()) {
            return sse_json_parse_error(parsed.error.message, data);
        }
        if (const std::string provider_msg = provider_error_message(parsed.value); !provider_msg.empty()) {
            return {ErrorCode::ProviderSchema, "provider error: " + provider_msg};
        }
        std::string type;
        if (const json::Value* value = parsed.value.get("type")) {
            if (value->is_string()) {
                type = value->string;
            }
        }
        if (type == "response.completed") {
            Error err = close_reasoning(on_delta, result, false);
            if (!err.ok()) {
                return err;
            }
            if (const json::Value* response = parsed.value.get("response")) {
                ChatResult metadata;
                err = parse_responses_json_value(*response, metadata, false);
                if (!err.ok()) {
                    return err;
                }
                if (!metadata.model.empty()) {
                    result.model = metadata.model;
                }
                if (!metadata.usage_json.empty() && metadata.usage_json != "null") {
                    result.usage_json = metadata.usage_json;
                }
                result.prompt_tokens = metadata.prompt_tokens;
                result.total_tokens = metadata.total_tokens;
                if (metadata.completion_tokens > 0) {
                    result.completion_tokens = metadata.completion_tokens;
                    result.completion_tokens_estimated = metadata.completion_tokens_estimated;
                }
            }
            done = true;
            return ok_error();
        }
        if (type == "response.failed" || type == "response.incomplete") {
            return {ErrorCode::ProviderSchema, "Responses API stream ended with " + type};
        }
        const json::Value* delta = parsed.value.get("delta");
        if (delta == nullptr || !delta->is_string()) {
            return ok_error();
        }
        if (ends_with(type, "reasoning_summary_text.delta") || ends_with(type, "reasoning_text.delta")) {
            return emit_reasoning(delta->string, on_delta, result);
        }
        if (ends_with(type, "output_text.delta") || type == "response.text.delta") {
            Error err = close_reasoning(on_delta, result, true);
            if (!err.ok()) {
                return err;
            }
            return emit_text(delta->string, on_delta, result, true);
        }
        return ok_error();
    }
};

Error stream_body_chunk(SseParser& chat_parser,
                        ResponsesSseParser& responses_parser,
                        ApiKind api_kind,
                        const std::string& chunk,
                        const DeltaCallback& on_delta,
                        ChatResult& result,
                        bool& done) {
    Error err = api_kind == ApiKind::Responses
                    ? responses_parser.feed(chunk, on_delta, result, done)
                    : chat_parser.feed(chunk, on_delta, result, done);
    if (!err.ok()) {
        return err;
    }
    if (done) {
        return {ErrorCode::StreamComplete, "chat completion stream finished"};
    }
    return ok_error();
}

std::vector<std::string> build_headers(const RequestContext& context) {
    std::vector<std::string> headers = context.headers;
    headers.emplace_back("Content-Type: application/json");
    if (!context.api_key.empty() && !has_authorization_header(headers)) {
        headers.emplace_back("Authorization: Bearer " + context.api_key);
    }
    return headers;
}

http::Request base_http_request(const RequestContext& context,
                                const std::string& method,
                                const std::string& url,
                                runtime::CancellationToken cancellation) {
    http::Request req;
    req.method = method;
    req.url = url;
    req.headers = build_headers(context);
    req.connect_timeout_seconds = context.options.connect_timeout_seconds;
    req.timeout_seconds = context.options.timeout_seconds;
    req.proxy = context.options.proxy;
    req.insecure_tls = context.options.insecure_tls;
    req.trace = context.options.trace_http;
    req.cancellation = cancellation;
    return req;
}

}  // namespace

const ModelCapability* matched_model_capability(const RequestContext& context) {
    return matched_model_capability_impl(context);
}

std::string reasoning_temperature_advisory(const RequestContext& context) {
    return config::temperature_advisory(matched_model_capability_impl(context),
                                        context.options.reasoning,
                                        context.options.has_temperature);
}

std::string normalize_base_url(const std::string& url, bool* changed, Error& error) {
    error = ok_error();
    if (changed != nullptr) {
        *changed = false;
    }
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        error = {ErrorCode::BadUrl, "URL must start with http:// or https://: " + url};
        return "";
    }
    std::string out = url;
    while (out.size() > 1 && out.back() == '/') {
        out.pop_back();
    }
    const size_t scheme = out.find("://");
    const size_t path = out.find('/', scheme == std::string::npos ? 0 : scheme + 3);
    if (path == std::string::npos) {
        if (changed != nullptr) {
            *changed = true;
        }
        return out + "/v1";
    }
    const std::string path_part = out.substr(path);
    if (path_part == "/") {
        if (changed != nullptr) {
            *changed = true;
        }
        return out.substr(0, path) + "/v1";
    }
    return out;
}

ContextResult build_context(const cli::Options& input_options) {
    cli::Options options = input_options;
    if (!options.prompt_file.empty()) {
        Error err = read_file(options.prompt_file, options.prompt);
        if (!err.ok()) {
            return {{}, err};
        }
    }
    if (!options.system_file.empty()) {
        Error err = read_file(options.system_file, options.system);
        if (!err.ok()) {
            return {{}, err};
        }
    }
    if (options.key_stdin) {
        std::ostringstream ss;
        ss << std::cin.rdbuf();
        options.key = ascii_trim(ss.str());
    }
    if (!options.key_file.empty()) {
        Error err = read_file(options.key_file, options.key);
        if (!err.ok()) {
            return {{}, err};
        }
        options.key = ascii_trim(options.key);
    }
    if (!options.list_models && !options.repl && !options.tui && !options.editor &&
        !options.benchmark && !options.grade && !options.security_review && !options.agent &&
        !options.agent_run && ascii_trim(options.prompt).empty()) {
        return {{},
                {ErrorCode::BadArgs,
                 "prompt is empty; use -p/--prompt, --prompt-file, --run/--run-file, "
                 "--plan/--plan-file, or --repl"}};
    }

    if (provider_requests_responses(options.provider)) {
        options.api = "responses";
    }
    ApiKind api_kind = options.api == "responses" ? ApiKind::Responses : ApiKind::ChatCompletions;

    Profile profile;
    if (!find_profile(options.provider, profile)) {
        return {{}, {ErrorCode::BadArgs, "unknown provider profile: " + options.provider}};
    }

    std::string base = options.base_url;
    if (!options.positional_url.empty() && is_provider_shortcut(options.positional_url)) {
        if (provider_requests_responses(options.positional_url)) {
            options.api = "responses";
            api_kind = ApiKind::Responses;
        }
        if (!find_profile(options.positional_url, profile)) {
            return {{}, {ErrorCode::BadArgs, "unknown provider profile: " + options.positional_url}};
        }
        options.provider = profile.name;
    } else if (base.empty() && !options.positional_url.empty()) {
        base = options.positional_url;
        if (input_options.provider == "openai") {
            if (!find_profile(names::kCustomOpenAiChat, profile)) {
                return {{}, {ErrorCode::Internal,
                            std::string(names::kCustomOpenAiChat) +
                                " profile is missing from the provider registry"}};
            }
            options.provider = profile.name;
        }
    } else {
        options.provider = profile.name;
    }
    Error reasoning_error = config::resolve_reasoning_off(
        options.model_catalog,
        options.provider,
        api_kind == ApiKind::Responses ? "responses" : "chat",
        options.model,
        options.reasoning);
    if (!reasoning_error.ok()) return {{}, reasoning_error};
    if (base.empty()) {
        base = profile.base_url;
    }
    if (profile.offline &&
        (!base.empty() || !options.chat_url.empty() || !options.models_url.empty() ||
         !options.responses_url.empty())) {
        return {{}, {ErrorCode::BadArgs,
                     "provider none cannot be combined with a model API endpoint; choose an OpenAI-compatible "
                     "provider to use --base-url, --chat-url, --models-url, or --responses-url"}};
    }
    const bool may_need_models = options.list_models || options.model.empty();
    const bool needs_base_for_models = may_need_models && options.models_url.empty();
    const bool needs_base_for_chat = api_kind == ApiKind::ChatCompletions && options.chat_url.empty();
    const bool needs_base_for_responses = api_kind == ApiKind::Responses && options.responses_url.empty();
    if (!profile.offline && base.empty() &&
        (needs_base_for_models || needs_base_for_chat || needs_base_for_responses)) {
        return {{}, {ErrorCode::BadUrl, "no base URL configured; pass BASE_URL or --base-url"}};
    }

    bool changed = false;
    Error err;
    if (!base.empty()) {
        base = normalize_base_url(base, &changed, err);
        if (!err.ok()) {
            return {{}, err};
        }
    }
    // Full-screen Chat/Agent alternate-screen UIs must not write status to
    // stderr: it lands under the input cursor and looks like editor content.
    if (changed && !options.quiet && !options.repl && !options.tui &&
        !options.agent) {
        std::cerr << "Using base URL: " << base << "\n";
    }

    for (const std::string& header : options.headers) {
        err = validate_header(header);
        if (!err.ok()) {
            return {{}, err};
        }
    }

    if (!profile.offline && api_kind == ApiKind::ChatCompletions &&
        !profile.capabilities.chat_completions && options.chat_url.empty()) {
        return {{}, {ErrorCode::UnsupportedFeature, "provider " + profile.name + " does not define a Chat Completions endpoint"}};
    }
    if (!profile.offline && api_kind == ApiKind::Responses &&
        !profile.capabilities.responses_api && options.responses_url.empty()) {
        return {{}, {ErrorCode::UnsupportedFeature, "provider " + profile.name + " does not define a built-in Responses API endpoint; use --responses-url or --api chat"}};
    }

    std::string key = resolve_key(options, profile);
    if (profile.requires_bearer_key && key.empty() && !has_authorization_header(options.headers)) {
        return {{}, {ErrorCode::Config, "provider " + profile.name + " requires an API key; set " +
                                      (profile.key_envs.empty() ? "AINIUX_API_KEY" : profile.key_envs[0]) +
                                      " or use --key-env/--key-file/--key-stdin"}};
    }
    if (!profile.compatibility_warning.empty() && !options.quiet) {
        std::cerr << "Provider warning: " << profile.compatibility_warning << "\n";
    }

    RequestContext context;
    context.options = options;
    context.profile = profile;
    context.base_url = base;
    context.chat_url = options.chat_url.empty() ? join_url(base, profile.chat_path) : options.chat_url;
    context.responses_url = options.responses_url.empty() ? join_url(base, profile.responses_path) : options.responses_url;
    context.models_url = options.models_url.empty() ? join_url(base, profile.models_path) : options.models_url;
    context.api_key = key;
    context.headers = options.headers;
    context.api_kind = api_kind;
    if (!options.responses_url.empty()) {
        context.profile.capabilities.responses_api = true;
    }
    return {context, ok_error()};
}

bool editor_has_configured_model_endpoint(const cli::Options& options) {
    if (options.provider_explicit) {
        return true;
    }
    if (!options.positional_url.empty() || !options.base_url.empty() || !options.chat_url.empty() ||
        !options.models_url.empty() || !options.responses_url.empty()) {
        return true;
    }
    Profile profile;
    return find_profile(options.provider, profile) && !profile.offline;
}

bool tui_needs_startup_provider_selection(const cli::Options& options) {
    // Shared by full-screen Chat and interactive Agent (same provider picker UX).
    const bool fullscreen_interactive = options.tui || options.agent;
    return fullscreen_interactive && !options.agent_project_settings_restored &&
           !options.provider_explicit && options.positional_url.empty() &&
           options.base_url.empty() && options.chat_url.empty() && options.models_url.empty() &&
           options.responses_url.empty();
}

bool looks_like_api_url(const std::string& text) {
    return text.rfind("http://", 0) == 0 || text.rfind("https://", 0) == 0;
}

void apply_provider_target(cli::Options& options, const std::string& target) {
    options.positional_url.clear();
    options.base_url.clear();
    options.chat_url.clear();
    options.models_url.clear();
    options.responses_url.clear();
    if (looks_like_api_url(target)) {
        options.positional_url = target;
        options.provider = "openai";
        return;
    }
    options.provider = target;
}

void apply_tui_startup_default(cli::Options& options) {
    if (!tui_needs_startup_provider_selection(options)) {
        return;
    }
    options.provider = "none";
}

bool editor_needs_local_only_default(const cli::Options& options) {
    if (!options.editor || options.provider_explicit || !options.positional_url.empty() ||
        !options.base_url.empty() || !options.chat_url.empty() || !options.models_url.empty() ||
        !options.responses_url.empty() || !options.model.empty()) {
        return false;
    }
    if (options.provider != "openai") {
        Profile profile;
        if (find_profile(options.provider, profile) && !profile.offline) {
            return false;
        }
    }
    return true;
}

void apply_editor_startup_default(cli::Options& options) {
    if (!editor_needs_local_only_default(options)) {
        return;
    }
    options.provider = "none";
}

void apply_editor_offline_default(cli::Options& options) {
    if (!options.editor || editor_has_configured_model_endpoint(options)) {
        return;
    }
    Profile profile;
    if (!find_profile(options.provider, profile) || !profile.requires_bearer_key) {
        return;
    }
    if (!resolve_key(options, profile).empty() || has_authorization_header(options.headers)) {
        return;
    }
    options.provider = "none";
}

std::string format_models_markdown(const std::string& provider_name,
                                   const std::string& models_url,
                                   const ModelsResult& result) {
    std::ostringstream out;
    out << "# Models\n\n";
    out << "**Provider:** " << markdown_table_cell(provider_name) << "\n";
    if (!models_url.empty()) {
        out << "**Endpoint:** " << markdown_table_cell(models_url) << "\n";
    }
    out << "\n";
    if (result.models.empty()) {
        out << "_No models returned._\n";
        return out.str();
    }

    static const std::vector<std::string> preferred_columns = {
        "object",
        "owned_by",
        "n_ctx",
        "context_length",
        "max_model_len",
        "max_context_length",
        "n_ctx_train",
        "n_params",
        "parameter_count",
        "size",
        "n_vocab",
        "n_embd",
        "capabilities",
        "aliases",
        "architecture",
        "modality",
        "created",
        "parent",
        "root",
    };
    std::set<std::string> attribute_columns;
    for (const ModelInfo& model : result.models) {
        for (const auto& entry : model.attributes) {
            attribute_columns.insert(entry.first);
        }
    }
    std::vector<std::string> columns;
    columns.reserve(1 + attribute_columns.size());
    columns.push_back("id");
    for (const std::string& key : preferred_columns) {
        if (attribute_columns.erase(key) > 0) {
            columns.push_back(key);
        }
    }
    for (const std::string& key : attribute_columns) {
        columns.push_back(key);
    }

    out << "|";
    for (const std::string& column : columns) {
        out << " " << model_attribute_heading(column) << " |";
    }
    out << "\n|";
    for (size_t i = 0; i < columns.size(); ++i) {
        out << " --- |";
    }
    out << "\n";

    for (const ModelInfo& model : result.models) {
        out << "|";
        for (const std::string& column : columns) {
            std::string value;
            if (column == "id") {
                value = model.id;
            } else {
                const auto it = model.attributes.find(column);
                if (it != model.attributes.end()) {
                    value = format_model_attribute_value(column, it->second);
                }
            }
            out << " " << markdown_table_cell(value) << " |";
        }
        out << "\n";
    }
    return out.str();
}

std::string normalize_provider_key(std::string text) {
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        } else if (ch == '-') {
            ch = '_';
        }
    }
    return text;
}

std::string canonical_profile_name(const std::string& name) {
    Profile profile;
    if (find_profile(name, profile)) {
        return profile.name;
    }
    return normalize_provider_key(name.empty() ? names::kOpenAi : name);
}

bool is_selectable_provider(const Profile& profile) {
    return !profile.offline && profile.name != names::kCustomOpenAiChat;
}

bool needs_interactive_model_selection(const RequestContext& context) {
    return !context.profile.offline && context.options.model.empty();
}

std::vector<Profile> built_in_profiles() {
    return profile_registry();
}

Error validate_profile_name(const std::string& name) {
    Profile profile;
    if (!find_profile(name, profile)) {
        return {ErrorCode::BadArgs, "unknown provider profile: " + name};
    }
    return ok_error();
}

const Capabilities& capabilities_for(const RequestContext& context) {
    return context.profile.capabilities;
}

bool credit_balance_available(const RequestContext& context) {
    auto without_trailing_slash = [](std::string value) {
        while (value.size() > 1 && value.back() == '/') value.pop_back();
        return value;
    };
    Error normalization_error;
    const std::string official_base =
        normalize_base_url(context.profile.base_url, nullptr, normalization_error);
    return context.profile.capabilities.credit_balance &&
           !context.profile.credit_url.empty() &&
           !context.api_key.empty() &&
           normalization_error.ok() &&
           without_trailing_slash(context.base_url) ==
               without_trailing_slash(official_base);
}

Capabilities detected_capabilities_for(const RequestContext& context) {
    Capabilities caps = capabilities_for(context);
    if (context.api_kind != ApiKind::ChatCompletions || context.options.image_capability == "deny") {
        caps.images = false;
        return caps;
    }
    if (context.options.image_capability == "allow") {
        caps.images = true;
        return caps;
    }
    if (!caps.images) {
        return caps;
    }

    const std::string model = normalize_provider_key(context.options.model);
    const bool known_vision = model.find("gpt_4o") != std::string::npos ||
                              model.find("gpt_4.1") != std::string::npos ||
                              model.find("gpt_5") != std::string::npos ||
                              model.find("vision") != std::string::npos ||
                              model.find("llava") != std::string::npos ||
                              model.find("pixtral") != std::string::npos ||
                              model.find("gemini") != std::string::npos ||
                              model.find("minicpm_v") != std::string::npos ||
                              (model.find("qwen") != std::string::npos &&
                               (model.find("vl") != std::string::npos || model.find("3.5") != std::string::npos ||
                                model.find("3.6") != std::string::npos));
    caps.images = known_vision;
    return caps;
}

Error validate_image_input(const RequestContext& context) {
    if (context.api_kind != ApiKind::ChatCompletions) {
        return {ErrorCode::UnsupportedFeature,
                "image input currently supports Chat Completions only; use --api chat"};
    }
    if (context.options.image_capability == "deny") {
        return {ErrorCode::UnsupportedFeature, "image input is disabled by --image-capability deny"};
    }
    if (!context.profile.capabilities.images && context.options.image_capability != "allow") {
        return {ErrorCode::UnsupportedFeature,
                "provider profile " + context.profile.name +
                    " does not advertise image input; use a vision-capable provider or explicitly pass "
                    "--image-capability allow for a compatible custom endpoint"};
    }
    if (!detected_capabilities_for(context).images) {
        return {ErrorCode::UnsupportedFeature,
                "model " + context.options.model +
                    " is not recognized as image-capable. Select a vision model or pass "
                    "--image-capability allow after verifying endpoint support"};
    }
    return ok_error();
}

std::string active_request_url(const RequestContext& context) {
    return context.api_kind == ApiKind::Responses ? context.responses_url : context.chat_url;
}

double tokens_per_second(const ChatResult& result, bool stream) {
    long long denominator_ms = result.total_ms;
    if (stream && result.ttft_ms >= 0 && result.total_ms > result.ttft_ms) {
        denominator_ms = result.total_ms - result.ttft_ms;
    }
    if (denominator_ms <= 0) {
        denominator_ms = 1;
    }
    return static_cast<double>(result.completion_tokens) * 1000.0 /
           static_cast<double>(denominator_ms);
}

long long reported_total_tokens(const ChatResult& result) {
    if (result.total_tokens >= 0) {
        return result.total_tokens;
    }
    if (result.usage_json.empty() || result.usage_json == "null") {
        return -1;
    }
    const json::ParseResult parsed = json::parse(result.usage_json);
    if (!parsed.error.ok() || !parsed.value.is_object()) {
        return -1;
    }
    auto token_value = [&](const std::string& name) -> long long {
        const json::Value* value = parsed.value.get(name);
        if (value == nullptr || value->type != json::Value::Type::Number || value->number < 0.0 ||
            value->number >= static_cast<double>(std::numeric_limits<long long>::max())) {
            return -1;
        }
        return static_cast<long long>(value->number);
    };
    const long long total = token_value("total_tokens");
    if (total >= 0) {
        return total;
    }
    long long input = token_value("prompt_tokens");
    if (input < 0) {
        input = token_value("input_tokens");
    }
    long long output = token_value("completion_tokens");
    if (output < 0) {
        output = token_value("output_tokens");
    }
    if (input < 0 || output < 0 || input > std::numeric_limits<long long>::max() - output) {
        return -1;
    }
    return input + output;
}

std::string serialize_request(const RequestContext& context, const std::vector<Message>& messages) {
    return context.api_kind == ApiKind::Responses ? build_responses_request_json(context, messages)
                                                  : build_chat_request_json(context, messages);
}

std::string serialize_chat_request(const RequestContext& context, const std::vector<Message>& messages) {
    return build_chat_request_json(context, messages);
}

bool model_selector_matches_value(const std::string& selector, const std::string& candidate) {
    if (selector.empty() || candidate.empty()) {
        return false;
    }
    if (selector == candidate) {
        return true;
    }
    return ascii_lower(selector) == ascii_lower(candidate);
}

bool model_selector_matches_aliases(const std::string& selector, const std::string& aliases) {
    if (selector.empty() || aliases.empty()) {
        return false;
    }
    std::size_t start = 0;
    while (start <= aliases.size()) {
        const std::size_t comma = aliases.find(',', start);
        const std::size_t end = comma == std::string::npos ? aliases.size() : comma;
        std::string alias = aliases.substr(start, end - start);
        while (!alias.empty() && alias.front() == ' ') {
            alias.erase(alias.begin());
        }
        while (!alias.empty() && alias.back() == ' ') {
            alias.pop_back();
        }
        if (model_selector_matches_value(selector, alias)) {
            return true;
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return false;
}

bool model_matches_selector(const ModelInfo& model, const std::string& selector) {
    if (model_selector_matches_value(selector, model.id)) {
        return true;
    }
    const auto aliases = model.attributes.find("aliases");
    if (aliases != model.attributes.end() && model_selector_matches_aliases(selector, aliases->second)) {
        return true;
    }
    static const char* alternate_keys[] = {"name", "model"};
    for (const char* key : alternate_keys) {
        const auto it = model.attributes.find(key);
        if (it != model.attributes.end() && model_selector_matches_value(selector, it->second)) {
            return true;
        }
    }
    return false;
}

long long model_attribute_tokens(const std::map<std::string, std::string>& attributes, const std::string& key) {
    const auto it = attributes.find(key);
    if (it == attributes.end()) {
        return -1;
    }
    long long value = 0;
    if (!parse_integral_string(it->second, value) || value <= 0) {
        return -1;
    }
    return value;
}

long long model_context_window_tokens(const ModelInfo& model) {
    static const char* keys[] = {"n_ctx",
                                 "context_length",
                                 "max_model_len",
                                 "max_context_length",
                                 "n_ctx_train",
                                 "max_tokens"};
    for (const char* key : keys) {
        const long long value = model_attribute_tokens(model.attributes, key);
        if (value > 0) {
            return value;
        }
    }
    return -1;
}

long long context_window_for_model(const ModelsResult& models, const std::string& model_selector) {
    if (model_selector.empty()) {
        return -1;
    }
    for (const ModelInfo& model : models.models) {
        if (model_matches_selector(model, model_selector)) {
            return model_context_window_tokens(model);
        }
    }
    return -1;
}

void apply_context_window_from_models(RequestContext& context,
                                      const ModelsResult& models,
                                      const std::string& model_selector) {
    if (context.options.has_context_tokens) {
        return;
    }
    // An automatically discovered value belongs only to the model for which it
    // was discovered. Clear it before every lookup so a missing model/attribute
    // never inherits the previous model's window.
    context.options.context_tokens = 0;
    std::vector<std::string> selectors;
    if (!model_selector.empty()) {
        selectors.push_back(model_selector);
    }
    if (!context.options.model.empty() &&
        (selectors.empty() || selectors.front() != context.options.model)) {
        selectors.push_back(context.options.model);
    }
    for (const std::string& selector : selectors) {
        const long long window = context_window_for_model(models, selector);
        if (window > 0) {
            context.options.context_tokens = window;
            return;
        }
    }
    const std::string api =
        context.api_kind == ApiKind::Responses ? "responses" : "chat";
    for (const std::string& selector : selectors) {
        const ModelCapability* capability =
            config::resolve_model_capability(context.options.model_catalog,
                                             context.profile.name,
                                             api,
                                             selector);
        if (capability != nullptr &&
            capability->context_window_tokens.has_value() &&
            *capability->context_window_tokens > 0) {
            context.options.context_tokens =
                *capability->context_window_tokens;
            return;
        }
    }
}

Error resolve_context_window(RequestContext& context, const std::string& model_selector) {
    if (context.profile.offline || context.options.has_context_tokens) {
        return ok_error();
    }
    context.options.context_tokens = 0;
    if (model_selector.empty() && context.options.model.empty()) {
        return ok_error();
    }
    ModelsResult models;
    const Error err = list_models(context, models);
    if (!err.ok()) {
        return ok_error();
    }
    apply_context_window_from_models(context, models, model_selector);
    return ok_error();
}

Error parse_models_response(const std::string& body, ModelsResult& result) {
    return parse_models_json(body, result);
}

namespace {

bool valid_currency_code(const std::string& currency) {
    if (currency.size() < 3 || currency.size() > 8) return false;
    for (const unsigned char ch : currency) {
        if (!std::isalnum(ch) && ch != '-' && ch != '_') return false;
    }
    return true;
}

Error parse_decimal_string(const std::string& text, double& value) {
    if (text.empty()) return {ErrorCode::ProviderSchema, "balance amount is empty"};
    char* end = nullptr;
    errno = 0;
    value = std::strtod(text.c_str(), &end);
    if (errno != 0 || end == text.c_str() || *end != '\0' || !std::isfinite(value))
        return {ErrorCode::ProviderSchema, "balance amount is not a finite decimal"};
    return ok_error();
}

}  // namespace

Error parse_credit_balance_response(const std::string& provider_name,
                                    const std::string& body,
                                    CreditBalanceResult& result) {
    result = {};
    const json::ParseResult parsed = json::parse(body);
    if (!parsed.error.ok())
        return {ErrorCode::JsonParse,
                provider_name + " credit response is not valid JSON: " +
                    parsed.error.message};
    if (!parsed.value.is_object())
        return {ErrorCode::ProviderSchema,
                provider_name + " credit response must be a JSON object"};

    const std::string canonical = normalize_provider_key(provider_name);
    if (canonical == "openrouter") {
        const json::Value* data = parsed.value.get("data");
        const json::Value* total =
            data != nullptr && data->is_object() ? data->get("total_credits") : nullptr;
        const json::Value* usage =
            data != nullptr && data->is_object() ? data->get("total_usage") : nullptr;
        if (total == nullptr || total->type != json::Value::Type::Number ||
            !std::isfinite(total->number) || usage == nullptr ||
            usage->type != json::Value::Type::Number ||
            !std::isfinite(usage->number))
            return {ErrorCode::ProviderSchema,
                    "OpenRouter credit response requires numeric "
                    "data.total_credits and data.total_usage"};
        result.balances.push_back({total->number - usage->number, "USD"});
        return ok_error();
    }
    if (canonical == names::kOpenAi) {
        const json::Value* available = parsed.value.get("total_available");
        if (available == nullptr ||
            available->type != json::Value::Type::Number ||
            !std::isfinite(available->number))
            return {ErrorCode::ProviderSchema,
                    "OpenAI credit response requires numeric total_available"};
        result.balances.push_back({available->number, "USD"});
        return ok_error();
    }
    if (canonical == "deepseek") {
        const json::Value* infos = parsed.value.get("balance_infos");
        if (infos == nullptr || !infos->is_array())
            return {ErrorCode::ProviderSchema,
                    "DeepSeek credit response is missing balance_infos array"};
        for (const json::Value& item : infos->array) {
            if (!item.is_object())
                return {ErrorCode::ProviderSchema,
                        "DeepSeek balance_infos entries must be objects"};
            const json::Value* currency = item.get("currency");
            const json::Value* total = item.get("total_balance");
            if (currency == nullptr || !currency->is_string() ||
                !valid_currency_code(currency->string) ||
                total == nullptr || !total->is_string())
                return {ErrorCode::ProviderSchema,
                        "DeepSeek balance entry requires currency and total_balance strings"};
            double amount = 0.0;
            Error decimal_error = parse_decimal_string(total->string, amount);
            if (!decimal_error.ok())
                return {decimal_error.code,
                        "DeepSeek " + currency->string + " " + decimal_error.message};
            std::string normalized_currency = currency->string;
            for (char& ch : normalized_currency)
                ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            result.balances.push_back({amount, normalized_currency});
        }
        return ok_error();
    }
    return {ErrorCode::UnsupportedFeature,
            "provider " + provider_name + " does not support credit balance queries"};
}

std::string format_credit_balance(const CreditBalanceResult& result) {
    std::ostringstream out;
    for (std::size_t index = 0; index < result.balances.size(); ++index) {
        if (index > 0) out << " · ";
        out << std::fixed << std::setprecision(2) << result.balances[index].amount
            << " " << (result.balances[index].currency.empty()
                            ? "USD"
                            : result.balances[index].currency);
    }
    return out.str();
}

Error list_models(const RequestContext& context, ModelsResult& result, runtime::CancellationToken cancellation) {
    if (context.profile.offline) {
        return {ErrorCode::UnsupportedFeature,
                "provider none disables model listing; select an OpenAI-compatible provider first"};
    }
    if (!context.profile.capabilities.model_listing || context.models_url.empty()) {
        return {ErrorCode::UnsupportedFeature,
                "provider " + context.profile.name +
                    " does not define a model-list endpoint; pass --model MODEL using a model supported by the provider"};
    }
    http::Request req = base_http_request(context, "GET", context.models_url, cancellation);
    const http::Result http_result = http::perform(req, {context.api_key});
    if (!http_result.error.ok()) {
        if (context.profile.local_endpoint && http_result.error.code == ErrorCode::Connect) {
            return {ErrorCode::Connect,
                    http_result.error.message +
                        "\nSuggestion: start LM Studio's local server or override the URL with --base-url."};
        }
        return http_result.error;
    }
    if (http_result.response.status < 200 || http_result.response.status >= 300) {
        return http_status_error(context, http_result.response, context.models_url);
    }
    return parse_models_json(http_result.response.body, result);
}

Error get_credit_balance(const RequestContext& context,
                         CreditBalanceResult& result,
                         runtime::CancellationToken cancellation) {
    result = {};
    if (!context.profile.capabilities.credit_balance ||
        context.profile.credit_url.empty()) {
        return {ErrorCode::UnsupportedFeature,
                "provider " + context.profile.name +
                    " does not define a credit-balance endpoint"};
    }
    if (context.api_key.empty())
        return {ErrorCode::Auth,
                "provider " + context.profile.name +
                    " credit balance requires an API key"};
    if (!credit_balance_available(context))
        return {ErrorCode::UnsupportedFeature,
                "credit balance is disabled when provider " +
                    context.profile.name + " uses a custom base URL"};
    http::Request request =
        base_http_request(context, "GET", context.profile.credit_url, cancellation);
    request.max_body_bytes = 65536;
    const http::Result fetched = http::perform(request, {context.api_key});
    if (!fetched.error.ok()) return fetched.error;
    if (fetched.response.status < 200 || fetched.response.status >= 300)
        return http_status_error(context, fetched.response,
                                 context.profile.credit_url);
    return parse_credit_balance_response(context.profile.name,
                                         fetched.response.body, result);
}

Error send_chat_messages(const RequestContext& context,
                         const std::vector<Message>& messages,
                         DeltaCallback on_delta,
                         ChatResult& result,
                         runtime::CancellationToken cancellation) {
    if (context.profile.offline) {
        return {ErrorCode::UnsupportedFeature,
                "provider none disables AI/model requests; select an OpenAI-compatible provider first"};
    }
    if (cancellation.cancelled()) {
        return {ErrorCode::Cancelled, "chat request cancelled before it started"};
    }
    for (const Message& message : messages) {
        if (!message.images.empty()) {
            Error image_error = validate_image_input(context);
            if (!image_error.ok()) {
                return image_error;
            }
            for (const ImageInput& image : message.images) {
                if (image.base64_data.empty()) {
                    return {ErrorCode::FileRead,
                            "image attachment data is unavailable" +
                                (image.display_name.empty() ? std::string()
                                                            : ": " + image.display_name)};
                }
            }
            break;
        }
    }
    auto start = std::chrono::steady_clock::now();
    bool saw_first_delta = false;
    auto timed_delta = [&](const std::string& delta) -> Error {
        if (cancellation.cancelled()) {
            return {ErrorCode::Cancelled, "chat request cancelled while streaming"};
        }
        if (!saw_first_delta && !delta.empty()) {
            saw_first_delta = true;
            const auto first = std::chrono::steady_clock::now();
            result.ttft_ms = std::chrono::duration_cast<std::chrono::milliseconds>(first - start).count();
        }
        return on_delta(delta);
    };

    const std::string url = active_request_url(context);
    http::Request req = base_http_request(context, "POST", url, cancellation);
    req.body = serialize_request(context, messages);
    SseParser chat_parser(context.suppress_streaming_reasoning);
    ResponsesSseParser responses_parser(context.suppress_streaming_reasoning);
    bool done = false;
    if (context.options.stream) {
        req.on_body = [&](const std::string& chunk) -> Error {
            return stream_body_chunk(chat_parser,
                                     responses_parser,
                                     context.api_kind,
                                     chunk,
                                     timed_delta,
                                     result,
                                     done);
        };
    }

    const http::Result http_result = http::perform(req, {context.api_key});
    result.http_status = http_result.response.status;
    result.dns_ms = http_result.response.dns_ms;
    result.connect_ms = http_result.response.connect_ms;
    result.tls_ms = http_result.response.tls_ms;
    result.time_to_first_byte_ms = http_result.response.time_to_first_byte_ms;
    result.first_body_ms = http_result.response.first_body_ms;
    if (!http_result.error.ok() && http_result.error.code != ErrorCode::StreamComplete) {
        if (context.profile.local_endpoint && http_result.error.code == ErrorCode::Connect) {
            return {ErrorCode::Connect,
                    http_result.error.message +
                        "\nSuggestion: start the local provider server or override the URL with --base-url."};
        }
        return http_result.error;
    }
    if (http_result.response.status < 200 || http_result.response.status >= 300) {
        return http_status_error(context, http_result.response, url);
    }
    result.model = context.options.model;
    if (context.options.stream) {
        Error err = context.api_kind == ApiKind::Responses
                        ? responses_parser.finish(timed_delta, result, done)
                        : chat_parser.finish(timed_delta, result, done);
        if (!err.ok()) {
            return err;
        }
    } else {
        Error err = context.api_kind == ApiKind::Responses ? parse_responses_json(http_result.response.body, result)
                                                           : parse_chat_json(http_result.response.body, result);
        if (!err.ok()) {
            return err;
        }
        const auto first = std::chrono::steady_clock::now();
        result.ttft_ms = std::chrono::duration_cast<std::chrono::milliseconds>(first - start).count();
    }
    auto end = std::chrono::steady_clock::now();
    result.total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    if (result.completion_tokens <= 0) {
        result.completion_tokens = estimate_completion_tokens(result.content);
        result.completion_tokens_estimated = true;
    }
    return ok_error();
}

Error send_chat(const RequestContext& context, DeltaCallback on_delta, ChatResult& result, runtime::CancellationToken cancellation) {
    std::vector<Message> messages;
    if (!context.options.system.empty()) {
        messages.push_back({"system", context.options.system});
    }
    messages.push_back({"user", context.options.prompt});
    return send_chat_messages(context, messages, on_delta, result, cancellation);
}

namespace {

json::Value json_string_value(const std::string& text) {
    json::Value value;
    value.type = json::Value::Type::String;
    value.string = text;
    return value;
}

json::Value json_bool_value(bool boolean) {
    json::Value value;
    value.type = json::Value::Type::Bool;
    value.boolean = boolean;
    return value;
}

json::Value json_object_value() {
    json::Value value;
    value.type = json::Value::Type::Object;
    return value;
}

json::Value json_array_value() {
    json::Value value;
    value.type = json::Value::Type::Array;
    return value;
}

Error parse_continuation_item(const std::string& encoded, json::Value& value) {
    json::ParseResult parsed = json::parse(encoded);
    if (!parsed.error.ok() || !parsed.value.is_object()) {
        return {ErrorCode::Internal, "invalid native tool continuation item"};
    }
    value = std::move(parsed.value);
    return ok_error();
}

std::string response_item_text(const json::Value& item) {
    std::string text;
    if (const json::Value* content = item.get("content"); content != nullptr && content->is_array()) {
        for (const json::Value& part : content->array) {
            const json::Value* part_text = part.get("text");
            if (part_text != nullptr && part_text->is_string()) text += part_text->string;
        }
    }
    return text;
}

std::string thinking_trace_text(const std::string& content) {
    output::ThinkingChunk split = output::split_thinking_traces(content);
    std::string trace = std::move(split.trace);
    const std::string open = "<think>";
    const std::string close = "</think>";
    std::string readable;
    std::size_t position = 0;
    while (position < trace.size()) {
        const std::size_t begin = trace.find(open, position);
        if (begin == std::string::npos) break;
        const std::size_t end = trace.find(close, begin + open.size());
        const std::size_t text_end = end == std::string::npos ? trace.size() : end;
        if (!readable.empty() && readable.back() != '\n') readable.push_back('\n');
        readable.append(trace, begin + open.size(), text_end - begin - open.size());
        if (end == std::string::npos) break;
        position = end + close.size();
    }
    return normalize_provider_text(std::move(readable));
}

Error parse_chat_tool_root(const json::Value& root, ToolRoundResult& result) {
    if (const std::string provider_msg = provider_error_message(root); !provider_msg.empty())
        return {ErrorCode::ProviderSchema, "provider error: " + provider_msg};
    if (const json::Value* model = root.get("model"); model != nullptr && model->is_string())
        result.metrics.model = model->string;
    if (const json::Value* usage = root.get("usage")) parse_usage(*usage, result.metrics);
    const json::Value* choices = root.get("choices");
    if (choices == nullptr || !choices->is_array() || choices->array.empty())
        return {ErrorCode::ProviderSchema, "native-tool chat response did not contain choices[0]"};
    const json::Value& choice = choices->array.front();
    const json::Value* message = choice.get("message");
    if (message == nullptr || !message->is_object())
        return {ErrorCode::ProviderSchema, "native-tool chat response did not contain choices[0].message"};
    result.reasoning_text = reasoning_text_from_object(*message);
    if (const json::Value* content = message->get("content"); content != nullptr && content->is_string())
        result.content = normalize_provider_text(content->string);
    if (result.reasoning_text.empty())
        result.reasoning_text = thinking_trace_text(result.content);
    if (const json::Value* calls = message->get("tool_calls"); calls != nullptr) {
        if (!calls->is_array()) return {ErrorCode::ProviderSchema, "assistant tool_calls must be an array"};
        for (std::size_t index = 0; index < calls->array.size(); ++index) {
            const json::Value& item = calls->array[index];
            const json::Value* id = item.get("id");
            const json::Value* function = item.get("function");
            const json::Value* name = function == nullptr ? nullptr : function->get("name");
            const json::Value* arguments = function == nullptr ? nullptr : function->get("arguments");
            if (id == nullptr || !id->is_string() || name == nullptr || !name->is_string() ||
                arguments == nullptr || !arguments->is_string())
                return {ErrorCode::ProviderSchema, "assistant tool call is missing id, name, or string arguments"};
            result.tool_calls.push_back({id->string, name->string, arguments->string, index});
        }
    }
    if (const json::Value* finish = choice.get("finish_reason");
        finish != nullptr && finish->is_string() && finish->string == "length") result.truncated = true;
    result.continuation_items_json.push_back(json::stringify(*message));
    if (result.content.empty() && result.tool_calls.empty() && !result.truncated)
        return {ErrorCode::ProviderSchema, "native-tool chat response contained neither text nor tool calls"};
    return ok_error();
}

Error parse_responses_tool_root(const json::Value& root, ToolRoundResult& result) {
    if (const std::string provider_msg = provider_error_message(root); !provider_msg.empty())
        return {ErrorCode::ProviderSchema, "provider error: " + provider_msg};
    if (const json::Value* model = root.get("model"); model != nullptr && model->is_string())
        result.metrics.model = model->string;
    if (const json::Value* usage = root.get("usage")) parse_usage(*usage, result.metrics);
    if (const json::Value* status = root.get("status");
        status != nullptr && status->is_string() && status->string == "incomplete") result.truncated = true;
    const json::Value* output_text = root.get("output_text");
    const bool has_output_text = output_text != nullptr && output_text->is_string();
    if (has_output_text) result.content = output_text->string;
    const json::Value* output = root.get("output");
    if (output == nullptr || !output->is_array()) {
        if (!result.content.empty()) return ok_error();
        return {ErrorCode::ProviderSchema, "native-tool Responses result did not contain output items"};
    }
    for (std::size_t index = 0; index < output->array.size(); ++index) {
        const json::Value& item = output->array[index];
        result.continuation_items_json.push_back(json::stringify(item));
        const json::Value* type = item.get("type");
        if (type != nullptr && type->is_string() && type->string == "reasoning")
            append_responses_reasoning_text(item, result.reasoning_text);
        if (type != nullptr && type->is_string() && type->string == "function_call") {
            const json::Value* call_id = item.get("call_id");
            const json::Value* id = item.get("id");
            const json::Value* name = item.get("name");
            const json::Value* arguments = item.get("arguments");
            const std::string resolved_id = call_id != nullptr && call_id->is_string()
                                                ? call_id->string
                                                : id != nullptr && id->is_string() ? id->string : std::string();
            if (resolved_id.empty() || name == nullptr || !name->is_string() ||
                arguments == nullptr || !arguments->is_string())
                return {ErrorCode::ProviderSchema, "Responses function_call is missing call_id, name, or arguments"};
            result.tool_calls.push_back({resolved_id, name->string, arguments->string, index});
        } else if (!has_output_text) {
            result.content += response_item_text(item);
        }
    }
    result.reasoning_text = normalize_provider_text(std::move(result.reasoning_text));
    if (result.reasoning_text.empty())
        result.reasoning_text = thinking_trace_text(result.content);
    if (result.content.empty() && result.tool_calls.empty() && !result.truncated)
        return {ErrorCode::ProviderSchema, "native-tool Responses result contained neither text nor calls"};
    return ok_error();
}

Error parse_chat_tool_stream(const std::string& body,
                             ToolRoundResult& result,
                             const ReasoningDeltaCallback& on_reasoning_delta) {
    std::map<std::size_t, ToolCall> calls;
    json::Value preserved = json_object_value();
    auto process = [&](const std::string& data) -> Error {
        if (data.empty() || data == "[DONE]") return ok_error();
        json::ParseResult parsed = json::parse(data);
        if (!parsed.error.ok()) return {ErrorCode::SseParse, parsed.error.message};
        if (const std::string message = provider_error_message(parsed.value); !message.empty())
            return {ErrorCode::ProviderSchema, "provider error: " + message};
        if (const json::Value* model = parsed.value.get("model"); model != nullptr && model->is_string())
            result.metrics.model = model->string;
        if (const json::Value* usage = parsed.value.get("usage")) parse_usage(*usage, result.metrics);
        const json::Value* choices = parsed.value.get("choices");
        if (choices == nullptr || !choices->is_array() || choices->array.empty()) return ok_error();
        const json::Value& choice = choices->array.front();
        if (const json::Value* finish = choice.get("finish_reason");
            finish != nullptr && finish->is_string() && finish->string == "length") result.truncated = true;
        const json::Value* delta = choice.get("delta");
        if (delta == nullptr || !delta->is_object()) return ok_error();
        const std::string reasoning_delta = reasoning_text_from_object(*delta);
        if (!reasoning_delta.empty()) {
            result.reasoning_text += reasoning_delta;
            if (on_reasoning_delta) {
                Error callback_error = on_reasoning_delta(reasoning_delta);
                if (!callback_error.ok()) return callback_error;
            }
        }
        if (const json::Value* content = delta->get("content"); content != nullptr && content->is_string())
            result.content += normalize_provider_text(content->string);
        for (const auto& field : delta->object) {
            if (field.first == "role" || field.first == "content" || field.first == "tool_calls") continue;
            auto existing = preserved.object.find(field.first);
            if (field.second.is_string()) {
                if (existing == preserved.object.end()) preserved.object[field.first] = field.second;
                else if (existing->second.is_string()) existing->second.string += field.second.string;
            } else if (field.second.is_array()) {
                if (existing == preserved.object.end()) preserved.object[field.first] = field.second;
                else if (existing->second.is_array())
                    existing->second.array.insert(existing->second.array.end(), field.second.array.begin(), field.second.array.end());
            } else {
                preserved.object[field.first] = field.second;
            }
        }
        const json::Value* tool_calls = delta->get("tool_calls");
        if (tool_calls == nullptr) return ok_error();
        if (!tool_calls->is_array()) return {ErrorCode::ProviderSchema, "streamed tool_calls must be an array"};
        for (const json::Value& item : tool_calls->array) {
            const json::Value* index_value = item.get("index");
            if (index_value == nullptr || index_value->type != json::Value::Type::Number ||
                !std::isfinite(index_value->number) || index_value->number < 0 ||
                index_value->number > 1000000 ||
                index_value->number !=
                    static_cast<double>(static_cast<std::size_t>(index_value->number)))
                return {ErrorCode::ProviderSchema, "streamed tool call is missing a valid index"};
            const std::size_t index = static_cast<std::size_t>(index_value->number);
            ToolCall& call = calls[index];
            call.index = index;
            if (const json::Value* id = item.get("id"); id != nullptr && id->is_string()) call.id += id->string;
            if (const json::Value* function = item.get("function"); function != nullptr) {
                if (const json::Value* name = function->get("name"); name != nullptr && name->is_string()) call.name += name->string;
                if (const json::Value* arguments = function->get("arguments"); arguments != nullptr && arguments->is_string())
                    call.arguments_json += arguments->string;
            }
        }
        return ok_error();
    };
    std::size_t pos = 0;
    while (pos < body.size()) {
        std::size_t end = 0;
        std::size_t next = 0;
        if (!find_sse_event_boundary(body, pos, end, next)) { end = body.size(); next = body.size(); }
        const std::vector<std::string> lines = collect_sse_data_lines(body.substr(pos, end - pos));
        if (!lines.empty()) {
            Error error = process(join_sse_data_lines(lines));
            if (!error.ok()) return error;
        }
        pos = next;
    }
    json::Value assistant = std::move(preserved);
    assistant.object["role"] = json_string_value("assistant");
    if (!result.content.empty()) assistant.object["content"] = json_string_value(result.content);
    else assistant.object["content"] = json::Value{};
    if (!calls.empty()) {
        json::Value array = json_array_value();
        for (auto& entry : calls) {
            ToolCall call = std::move(entry.second);
            if (call.id.empty() || call.name.empty())
                return {ErrorCode::ProviderSchema, "stream ended with an incomplete native tool call"};
            json::Value item = json_object_value();
            item.object["id"] = json_string_value(call.id);
            item.object["type"] = json_string_value("function");
            json::Value function = json_object_value();
            function.object["name"] = json_string_value(call.name);
            function.object["arguments"] = json_string_value(call.arguments_json);
            item.object["function"] = std::move(function);
            array.array.push_back(std::move(item));
            result.tool_calls.push_back(std::move(call));
        }
        assistant.object["tool_calls"] = std::move(array);
    }
    result.continuation_items_json.push_back(json::stringify(assistant));
    if (result.reasoning_text.empty())
        result.reasoning_text = thinking_trace_text(result.content);
    if (result.content.empty() && result.tool_calls.empty() && !result.truncated)
        return {ErrorCode::ProviderSchema, "native-tool stream contained neither text nor calls"};
    return ok_error();
}

Error parse_responses_tool_stream(const std::string& body,
                                  ToolRoundResult& result,
                                  const ReasoningDeltaCallback& on_reasoning_delta) {
    std::map<std::size_t, json::Value> items;
    std::map<std::size_t, std::string> argument_fragments;
    json::Value completed_response;
    bool completed = false;
    auto process = [&](const std::string& data) -> Error {
        if (data.empty() || data == "[DONE]") return ok_error();
        json::ParseResult parsed = json::parse(data);
        if (!parsed.error.ok()) return {ErrorCode::SseParse, parsed.error.message};
        const json::Value* type = parsed.value.get("type");
        const std::string event = type != nullptr && type->is_string() ? type->string : std::string();
        if (event == "response.completed" || event == "response.incomplete") {
            if (const json::Value* response = parsed.value.get("response"); response != nullptr && response->is_object()) {
                completed_response = *response;
                completed = true;
            }
            if (event == "response.incomplete") result.truncated = true;
            return ok_error();
        }
        const json::Value* output_index = parsed.value.get("output_index");
        std::size_t index = 0;
        if (output_index != nullptr) {
            if (output_index->type != json::Value::Type::Number ||
                !std::isfinite(output_index->number) || output_index->number < 0 ||
                output_index->number > 1000000 ||
                output_index->number !=
                    static_cast<double>(static_cast<std::size_t>(output_index->number))) {
                return {ErrorCode::ProviderSchema,
                        "Responses stream event contains an invalid output_index"};
            }
            index = static_cast<std::size_t>(output_index->number);
        }
        if (event == "response.output_item.added" || event == "response.output_item.done") {
            if (const json::Value* item = parsed.value.get("item"); item != nullptr && item->is_object()) items[index] = *item;
        } else if (event == "response.function_call_arguments.delta") {
            if (const json::Value* delta = parsed.value.get("delta"); delta != nullptr && delta->is_string())
                argument_fragments[index] += delta->string;
        } else if (event == "response.output_text.delta") {
            if (const json::Value* delta = parsed.value.get("delta"); delta != nullptr && delta->is_string())
                result.content += delta->string;
        } else if (event == "response.reasoning_summary_text.delta" ||
                   event == "response.reasoning_text.delta") {
            if (const json::Value* delta = parsed.value.get("delta");
                delta != nullptr && delta->is_string()) {
                result.reasoning_text += delta->string;
                if (on_reasoning_delta) {
                    Error callback_error = on_reasoning_delta(delta->string);
                    if (!callback_error.ok()) return callback_error;
                }
            }
        } else if (event == "error") {
            return {ErrorCode::ProviderSchema, "Responses stream reported an error"};
        }
        return ok_error();
    };
    std::size_t pos = 0;
    while (pos < body.size()) {
        std::size_t end = 0;
        std::size_t next = 0;
        if (!find_sse_event_boundary(body, pos, end, next)) { end = body.size(); next = body.size(); }
        const std::vector<std::string> lines = collect_sse_data_lines(body.substr(pos, end - pos));
        if (!lines.empty()) {
            Error error = process(join_sse_data_lines(lines));
            if (!error.ok()) return error;
        }
        pos = next;
    }
    if (completed) {
        ToolRoundResult completed_result;
        Error error = parse_responses_tool_root(completed_response, completed_result);
        if (!error.ok()) return error;
        if (!result.reasoning_text.empty())
            completed_result.reasoning_text = result.reasoning_text;
        result = std::move(completed_result);
        return ok_error();
    }
    json::Value root = json_object_value();
    json::Value output = json_array_value();
    for (auto& entry : items) {
        json::Value item = std::move(entry.second);
        const json::Value* type = item.get("type");
        if (type != nullptr && type->is_string() && type->string == "function_call" &&
            item.get("arguments") == nullptr)
            item.object["arguments"] = json_string_value(argument_fragments[entry.first]);
        output.array.push_back(std::move(item));
    }
    root.object["output"] = std::move(output);
    if (!result.content.empty()) root.object["output_text"] = json_string_value(result.content);
    ToolRoundResult parsed_result;
    Error error = parse_responses_tool_root(root, parsed_result);
    if (!error.ok()) return error;
    if (!result.reasoning_text.empty()) parsed_result.reasoning_text = result.reasoning_text;
    result = std::move(parsed_result);
    return ok_error();
}

}  // namespace

std::string new_routing_session_id() {
    static std::atomic<std::uint64_t> sequence{0};
    const std::uint64_t serial = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    const std::uint64_t ticks = static_cast<std::uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const std::uint64_t first = mix_session_bits(ticks ^ serial);
    const std::uint64_t second =
        mix_session_bits(first ^ (serial * 0xd1342543de82ef95ULL));
    std::ostringstream out;
    out << "ainiux-" << std::hex << std::setfill('0')
        << std::setw(16) << first << std::setw(16) << second;
    return out.str();
}

std::string serialize_tool_request(const RequestContext& context,
                                   const ToolConversation& conversation,
                                   const std::vector<FunctionDefinition>& tools) {
    json::ParseResult parsed = json::parse(serialize_request(context, conversation.messages));
    if (!parsed.error.ok() || !parsed.value.is_object()) return "{}";
    json::Value& root = parsed.value;
    const std::string item_key = context.api_kind == ApiKind::Responses ? "input" : "messages";
    json::Value* items = &root.object[item_key];
    for (const std::string& encoded : conversation.continuation_items_json) {
        json::Value item;
        if (!parse_continuation_item(encoded, item).ok()) return "{}";
        items->array.push_back(std::move(item));
    }
    json::Value definitions = json_array_value();
    for (const FunctionDefinition& definition : tools) {
        json::ParseResult schema = json::parse(definition.parameters_json);
        if (!schema.error.ok() || !schema.value.is_object()) return "{}";
        json::Value item = json_object_value();
        item.object["type"] = json_string_value("function");
        if (context.api_kind == ApiKind::Responses) {
            item.object["name"] = json_string_value(definition.name);
            item.object["description"] = json_string_value(definition.description);
            item.object["parameters"] = std::move(schema.value);
            item.object["strict"] = json_bool_value(true);
        } else {
            json::Value function = json_object_value();
            function.object["name"] = json_string_value(definition.name);
            function.object["description"] = json_string_value(definition.description);
            function.object["parameters"] = std::move(schema.value);
            item.object["function"] = std::move(function);
        }
        definitions.array.push_back(std::move(item));
    }
    root.object["tools"] = std::move(definitions);
    root.object["tool_choice"] = json_string_value("auto");
    return json::stringify(root);
}

Error parse_tool_response(const RequestContext& context,
                          const std::string& body,
                          ToolRoundResult& result,
                          bool streaming,
                          ReasoningDeltaCallback on_reasoning_delta) {
    result = ToolRoundResult{};
    if (streaming)
        return context.api_kind == ApiKind::Responses
                   ? parse_responses_tool_stream(body, result, on_reasoning_delta)
                   : parse_chat_tool_stream(body, result, on_reasoning_delta);
    json::ParseResult parsed = json::parse(body);
    if (!parsed.error.ok()) return parsed.error;
    Error error = context.api_kind == ApiKind::Responses
                      ? parse_responses_tool_root(parsed.value, result)
                      : parse_chat_tool_root(parsed.value, result);
    if (error.ok() && on_reasoning_delta && !result.reasoning_text.empty())
        return on_reasoning_delta(result.reasoning_text);
    return error;
}

void append_tool_results(const RequestContext& context,
                         const std::vector<ToolCall>& calls,
                         const std::vector<std::string>& result_json,
                         ToolConversation& conversation) {
    const std::size_t count = std::min(calls.size(), result_json.size());
    for (std::size_t index = 0; index < count; ++index) {
        json::Value item = json_object_value();
        if (context.api_kind == ApiKind::Responses) {
            item.object["type"] = json_string_value("function_call_output");
            item.object["call_id"] = json_string_value(calls[index].id);
            item.object["output"] = json_string_value(result_json[index]);
        } else {
            item.object["role"] = json_string_value("tool");
            item.object["tool_call_id"] = json_string_value(calls[index].id);
            item.object["content"] = json_string_value(result_json[index]);
        }
        conversation.continuation_items_json.push_back(json::stringify(item));
    }
}

Error send_tool_round(const RequestContext& context,
                      const ToolConversation& conversation,
                      const std::vector<FunctionDefinition>& tools,
                      ToolRoundResult& result,
                      runtime::CancellationToken cancellation,
                      const ToolRoundObserver* observer,
                      const ToolRoundContext& observation_context,
                      ReasoningDeltaCallback on_reasoning_delta) {
    Error precondition_error;
    if (context.profile.offline)
        precondition_error = {ErrorCode::UnsupportedFeature, "provider none disables native tool requests"};
    else if (tools.empty())
        precondition_error = {ErrorCode::BadArgs, "native tool request requires at least one function definition"};
    else if (cancellation.cancelled())
        precondition_error = {ErrorCode::Cancelled, "native tool request cancelled before it started"};
    if (precondition_error.ok()) {
        // Same image capability gates as ordinary chat when seed messages carry images.
        for (const Message& message : conversation.messages) {
            if (message.images.empty()) continue;
            precondition_error = validate_image_input(context);
            if (!precondition_error.ok()) break;
            for (const ImageInput& image : message.images) {
                if (image.base64_data.empty()) {
                    precondition_error = {
                        ErrorCode::FileRead,
                        "image attachment data is unavailable" +
                            (image.display_name.empty() ? std::string()
                                                        : ": " + image.display_name)};
                    break;
                }
            }
            break;
        }
    }
    if (!precondition_error.ok()) {
        if (observer != nullptr && observer->on_response)
            observer->on_response(observation_context, http::Response{}, result, precondition_error);
        return precondition_error;
    }
    http::Request request = base_http_request(context, "POST", active_request_url(context), cancellation);
    request.body = serialize_tool_request(context, conversation, tools);
    request.max_body_bytes = 8L * 1024L * 1024L;
    std::vector<std::string> header_names;
    header_names.reserve(request.headers.size());
    for (const std::string& header : request.headers) {
        const std::size_t colon = header.find(':');
        header_names.push_back(ascii_trim(header.substr(0, colon)));
    }
    const Error serialization_error = request.body == "{}"
        ? Error{ErrorCode::Internal, "could not serialize native tool request"} : ok_error();
    if (observer != nullptr && observer->on_request)
        observer->on_request(observation_context, request.url, header_names,
                             request.body, serialization_error);
    if (!serialization_error.ok()) {
        if (observer != nullptr && observer->on_response)
            observer->on_response(observation_context, http::Response{}, result,
                                  serialization_error);
        return serialization_error;
    }
    std::string reasoning_stream_buffer;
    output::ThinkingTraceSplitter thinking_splitter;
    if (context.options.stream && on_reasoning_delta) {
        request.on_body = [&](const std::string& chunk) -> Error {
            reasoning_stream_buffer += chunk;
            std::size_t position = 0;
            for (;;) {
                std::size_t end = 0;
                std::size_t next = 0;
                if (!find_sse_event_boundary(reasoning_stream_buffer, position, end, next))
                    break;
                const std::vector<std::string> lines =
                    collect_sse_data_lines(reasoning_stream_buffer.substr(position,
                                                                          end - position));
                position = next;
                if (lines.empty()) continue;
                const std::string data = join_sse_data_lines(lines);
                if (data.empty() || data == "[DONE]") continue;
                json::ParseResult parsed = json::parse(data);
                if (!parsed.error.ok()) continue;  // Final parser reports malformed SSE.
                std::string delta;
                if (context.api_kind == ApiKind::Responses) {
                    const json::Value* type = parsed.value.get("type");
                    const std::string event =
                        type != nullptr && type->is_string() ? type->string : std::string();
                    if (event == "response.reasoning_summary_text.delta" ||
                        event == "response.reasoning_text.delta") {
                        if (const json::Value* value = parsed.value.get("delta");
                            value != nullptr && value->is_string())
                            delta = value->string;
                    }
                } else {
                    const json::Value* choices = parsed.value.get("choices");
                    if (choices != nullptr && choices->is_array() &&
                        !choices->array.empty()) {
                        const json::Value* value = choices->array.front().get("delta");
                        if (value != nullptr && value->is_object()) {
                            delta = reasoning_text_from_object(*value);
                            if (delta.empty()) {
                                if (const json::Value* content = value->get("content");
                                    content != nullptr && content->is_string()) {
                                    output::ThinkingChunk thinking =
                                        thinking_splitter.feed(content->string);
                                    delta = std::move(thinking.trace);
                                    for (const std::string& tag :
                                         {std::string("<think>"),
                                          std::string("</think>")}) {
                                        std::size_t tag_position = 0;
                                        while ((tag_position = delta.find(tag, tag_position)) !=
                                               std::string::npos)
                                            delta.erase(tag_position, tag.size());
                                    }
                                }
                            }
                        }
                    }
                }
                if (!delta.empty()) {
                    Error callback_error = on_reasoning_delta(delta);
                    if (!callback_error.ok()) return callback_error;
                }
            }
            if (position > 0) reasoning_stream_buffer.erase(0, position);
            return ok_error();
        };
    }
    const auto started = std::chrono::steady_clock::now();
    const http::Result response = http::perform(request, {context.api_key});
    Error error = response.error;
    if (error.ok() && (response.response.status < 200 || response.response.status >= 300))
        error = http_status_error(context, response.response, request.url);
    if (error.ok())
        error = parse_tool_response(
            context, response.response.body, result, context.options.stream,
            context.options.stream ? ReasoningDeltaCallback{} : on_reasoning_delta);
    result.metrics.http_status = response.response.status;
    result.metrics.total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - started).count();
    result.metrics.dns_ms = response.response.dns_ms;
    result.metrics.connect_ms = response.response.connect_ms;
    result.metrics.tls_ms = response.response.tls_ms;
    result.metrics.time_to_first_byte_ms = response.response.time_to_first_byte_ms;
    result.metrics.first_body_ms = response.response.first_body_ms;
    if (observer != nullptr && observer->on_response)
        observer->on_response(observation_context, response.response, result, error);
    return error;
}

std::string display_name_for_profile(const std::string& profile_name) {
    if (profile_name.empty()) return {};
    if (looks_like_api_url(profile_name)) return names::kCustom;
    Profile profile;
    if (!find_profile(profile_name, profile)) {
        return names::kCustom;
    }
    if (profile.name == names::kCustomOpenAiChat) return names::kCustom;
    std::string preferred = profile.name;
    for (const std::string& alias : profile.aliases) {
        if (alias.size() < preferred.size()) preferred = alias;
    }
    return preferred;
}

}  // namespace ainiux::provider
