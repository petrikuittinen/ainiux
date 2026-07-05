#include "chat/settings.hpp"
#include "provider/provider.hpp"

#include <chrono>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <utility>

#include "json/json.hpp"
#include "security/redact.hpp"

namespace pkchat::provider {

namespace {

std::string trim_ascii(std::string text) {
    auto is_ws = [](unsigned char ch) { return ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t'; };
    while (!text.empty() && is_ws(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
    }
    while (!text.empty() && is_ws(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    return text;
}

std::string lower_alias(std::string text) {
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        } else if (ch == '-') {
            ch = '_';
        }
    }
    return text;
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
    profile.capabilities.images = name == "openai" || name == "openrouter" || name == "gemini" ||
                                  name == "xai" || name == "mistral" || name == "lm_studio" ||
                                  name == "ollama" || name == "vllm" || name == "llamacpp" ||
                                  name == "qwen" || name == "custom_openai_chat";
    return profile;
}

Profile make_offline_profile() {
    Profile profile;
    profile.name = "none";
    profile.aliases = {"offline"};
    profile.base_url.clear();
    profile.chat_path.clear();
    profile.responses_path.clear();
    profile.models_path.clear();
    profile.offline = true;
    profile.capabilities = {};
    return profile;
}

const std::vector<Profile>& profile_registry() {
    static const std::vector<Profile> profiles = {
        make_offline_profile(),
        make_profile("openai", {"openai_chat", "openai_responses"}, "https://api.openai.com/v1", "/chat/completions", "/models", "/responses", {"OPENAI_API_KEY", "PKCHAT_API_KEY"}, true, false),
        make_profile("openrouter", {}, "https://openrouter.ai/api/v1", "/chat/completions", "/models", "", {"OPENROUTER_API_KEY", "PKCHAT_API_KEY"}, true, false),
        make_profile("deepseek", {}, "https://api.deepseek.com", "/chat/completions", "/models", "", {"DEEPSEEK_API_KEY", "PKCHAT_API_KEY"}, true, false),
        make_profile("gemini", {}, "https://generativelanguage.googleapis.com/v1beta/openai", "/chat/completions", "/models", "", {"GEMINI_API_KEY", "PKCHAT_API_KEY"}, true, false),
        make_profile("anthropic", {}, "https://api.anthropic.com/v1", "/chat/completions", "/models", "", {"ANTHROPIC_API_KEY", "PKCHAT_API_KEY"}, true, false, "", "OpenAI compatibility layer is mainly for testing/comparison."),
        make_profile("xai", {"grok"}, "https://api.x.ai/v1", "/chat/completions", "/models", "", {"XAI_API_KEY", "PKCHAT_API_KEY"}, true, false),
        make_profile("moonshot", {"kimi"}, "https://api.moonshot.ai/v1", "/chat/completions", "/models", "", {"MOONSHOT_API_KEY", "PKCHAT_API_KEY"}, true, false),
        make_profile("groq", {}, "https://api.groq.com/openai/v1", "/chat/completions", "/models", "", {"GROQ_API_KEY", "PKCHAT_API_KEY"}, true, false),
        make_profile("mistral", {}, "https://api.mistral.ai/v1", "/chat/completions", "/models", "", {"MISTRAL_API_KEY", "PKCHAT_API_KEY"}, true, false),
        make_profile("together", {}, "https://api.together.ai/v1", "/chat/completions", "/models", "", {"TOGETHER_API_KEY", "PKCHAT_API_KEY"}, true, false),
        make_profile("perplexity", {}, "https://api.perplexity.ai", "/chat/completions", "/models", "", {"PERPLEXITY_API_KEY", "PKCHAT_API_KEY"}, true, false, "", "Perplexity canonical Sonar endpoint is /v1/sonar; /chat/completions is the OpenAI SDK-compatible alias."),
        make_profile("cerebras", {}, "https://api.cerebras.ai/v1", "/chat/completions", "/models", "", {"CEREBRAS_API_KEY", "PKCHAT_API_KEY"}, true, false),
        make_profile("fireworks", {}, "https://api.fireworks.ai/inference/v1", "/chat/completions", "/models", "", {"FIREWORKS_API_KEY", "PKCHAT_API_KEY"}, true, false),
        make_profile("deepinfra", {}, "https://api.deepinfra.com/v1/openai", "/chat/completions", "/models", "", {"DEEPINFRA_API_KEY", "DEEPINFRA_TOKEN", "PKCHAT_API_KEY"}, true, false),
        make_profile("nvidia_nim", {}, "https://integrate.api.nvidia.com/v1", "/chat/completions", "/models", "", {"NVIDIA_NIM_API_KEY", "PKCHAT_API_KEY"}, true, false),
        make_profile("zai", {"z.ai", "z_ai"}, "https://api.z.ai/api/paas/v4", "/chat/completions", "", "", {"ZAI_API_KEY", "PKCHAT_API_KEY"}, true, false),
        make_profile("qwen", {"dashscope_intl"}, "https://dashscope-intl.aliyuncs.com/compatible-mode/v1", "/chat/completions", "/models", "", {"DASHSCOPE_API_KEY", "QWEN_API_KEY", "PKCHAT_API_KEY"}, true, false),
        make_profile("dashscope", {}, "https://dashscope.aliyuncs.com/compatible-mode/v1", "/chat/completions", "/models", "", {"DASHSCOPE_API_KEY", "PKCHAT_API_KEY"}, true, false),
        make_profile("lm_studio", {"lmstudio"}, "http://localhost:1234/v1", "/chat/completions", "/models", "", {"LMSTUDIO_API_KEY", "LM_STUDIO_API_KEY", "PKCHAT_API_KEY"}, false, true),
        make_profile("ollama", {}, "http://localhost:11434/v1", "/chat/completions", "/models", "", {}, false, true),
        make_profile("vllm", {}, "http://localhost:8000/v1", "/chat/completions", "/models", "", {}, false, true, "token-abc123"),
        make_profile("llamacpp", {"llama_cpp", "llama.cpp"}, "http://localhost:8080/v1", "/chat/completions", "/models", "", {}, false, true),
        make_profile("custom_openai_chat", {"custom"}, "", "/chat/completions", "/models", "/responses", {"PKCHAT_API_KEY"}, false, false),
    };
    return profiles;
}

std::string provider_lookup_name(const std::string& requested) {
    const std::string name = lower_alias(requested.empty() ? "openai" : requested);
    if (name == "openai_responses") {
        return "openai";
    }
    return name;
}

bool provider_requests_responses(const std::string& requested) {
    return lower_alias(requested) == "openai_responses";
}

bool find_profile(const std::string& requested, Profile& out) {
    const std::string name = provider_lookup_name(requested);
    for (const Profile& profile : profile_registry()) {
        if (lower_alias(profile.name) == name) {
            out = profile;
            return true;
        }
        for (const std::string& alias : profile.aliases) {
            if (lower_alias(alias) == name) {
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
        if (is_sensitive_header_name(trim_ascii(header.substr(0, colon))) &&
            lower_alias(trim_ascii(header.substr(0, colon))) == "authorization") {
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
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {ErrorCode::FileRead, "could not open file for reading: " + path};
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    if (file.bad()) {
        return {ErrorCode::FileRead, "could not read file: " + path};
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

enum class ReasoningWireFormat {
    None,
    GenericThinking,
    OpenAiChatEffort,
    OpenAiResponsesReasoning,
    OpenRouterReasoning,
    GeminiOpenAi,
    QwenThinking,
    DeepSeekThinking,
    XaiReasoningEffort,
};

std::string lower_ascii_copy(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

long long parse_reasoning_token_budget(const std::string& value) {
    if (!chat::thinking_budget_is_token_count(value)) {
        return -1;
    }
    try {
        return std::stoll(value);
    } catch (const std::exception&) {
        return -1;
    }
}

std::string normalize_reasoning_effort(std::string value) {
    value = lower_ascii_copy(trim_ascii(std::move(value)));
    if (value == "off" || value == "false" || value == "disabled" || value == "disable" ||
        value == "no") {
        return "none";
    }
    if (value == "on" || value == "true" || value == "enabled" || value == "enable" ||
        value == "yes") {
        return "medium";
    }
    return value;
}

std::string effort_from_token_budget(long long tokens) {
    if (tokens <= 0) {
        return "none";
    }
    if (tokens <= 1024) {
        return "low";
    }
    if (tokens <= 8192) {
        return "medium";
    }
    if (tokens <= 24576) {
        return "high";
    }
    return "xhigh";
}

long long token_budget_from_effort(const std::string& effort) {
    if (effort == "none") {
        return 0;
    }
    if (effort == "minimal" || effort == "low") {
        return 1024;
    }
    if (effort == "medium") {
        return 8192;
    }
    if (effort == "high") {
        return 24576;
    }
    if (effort == "xhigh" || effort == "max") {
        return 32768;
    }
    return -1;
}

std::string reasoning_effort_from_options(const cli::Options& o) {
    if (o.has_thinking_budget) {
        const long long tokens = parse_reasoning_token_budget(o.thinking_budget);
        if (tokens >= 0) {
            return effort_from_token_budget(tokens);
        }
        return normalize_reasoning_effort(o.thinking_budget);
    }
    if (o.has_enable_thinking) {
        return o.enable_thinking ? "medium" : "none";
    }
    return "";
}

ReasoningWireFormat reasoning_wire_format_for(const RequestContext& context) {
    const std::string profile = lower_alias(context.profile.name);
    if (profile == "none" || context.profile.offline) {
        return ReasoningWireFormat::None;
    }
    if (profile == "openai") {
        return context.api_kind == ApiKind::Responses ? ReasoningWireFormat::OpenAiResponsesReasoning
                                                      : ReasoningWireFormat::OpenAiChatEffort;
    }
    if (profile == "openrouter") {
        return ReasoningWireFormat::OpenRouterReasoning;
    }
    if (profile == "gemini") {
        return ReasoningWireFormat::GeminiOpenAi;
    }
    if (profile == "qwen" || profile == "dashscope") {
        return ReasoningWireFormat::QwenThinking;
    }
    if (profile == "deepseek") {
        return ReasoningWireFormat::DeepSeekThinking;
    }
    if (profile == "xai") {
        return ReasoningWireFormat::XaiReasoningEffort;
    }
    if (profile.empty() || profile == "custom_openai_chat" || profile == "lm_studio" ||
        profile == "ollama" || profile == "vllm" || profile == "llamacpp") {
        return ReasoningWireFormat::GenericThinking;
    }
    return ReasoningWireFormat::None;
}

std::string openai_effort_value(std::string effort) {
    if (effort == "max") {
        return "xhigh";
    }
    return effort;
}

std::string gemini_effort_value(std::string effort) {
    if (effort == "xhigh" || effort == "max") {
        return "high";
    }
    return effort;
}

std::string xai_effort_value(std::string effort) {
    if (effort == "minimal") {
        return "low";
    }
    if (effort == "xhigh" || effort == "max") {
        return "high";
    }
    return effort;
}

std::string deepseek_effort_value(std::string effort) {
    if (effort == "xhigh" || effort == "max") {
        return "max";
    }
    if (effort == "none") {
        return "none";
    }
    return "high";
}

std::string append_pair(std::string fields, const std::string& key, const std::string& value_json) {
    if (!fields.empty()) {
        fields += ",";
    }
    fields += json::quote(key);
    fields += ":";
    fields += value_json;
    return fields;
}

std::string qwen_reasoning_fields_json(const cli::Options& o) {
    std::string fields;
    const std::string effort = reasoning_effort_from_options(o);
    const long long explicit_tokens =
        o.has_thinking_budget ? parse_reasoning_token_budget(o.thinking_budget) : -1;

    if (o.has_enable_thinking || o.has_thinking_budget) {
        bool enabled = true;
        if (o.has_enable_thinking) {
            enabled = o.enable_thinking;
        }
        if (effort == "none" || explicit_tokens == 0) {
            enabled = false;
        }
        fields = append_pair(fields, "enable_thinking", enabled ? "true" : "false");
    }

    if (explicit_tokens > 0) {
        fields = append_pair(fields, "thinking_budget", std::to_string(explicit_tokens));
    } else if (explicit_tokens < 0 && !effort.empty()) {
        const long long mapped_tokens = token_budget_from_effort(effort);
        if (mapped_tokens > 0) {
            fields = append_pair(fields, "thinking_budget", std::to_string(mapped_tokens));
        }
    }
    return fields;
}

std::string generic_reasoning_fields_json(const cli::Options& o) {
    std::string fields;
    if (o.has_enable_thinking) {
        fields = append_pair(fields, "enable_thinking", o.enable_thinking ? "true" : "false");
    }
    if (o.has_thinking_budget) {
        if (chat::thinking_budget_is_token_count(o.thinking_budget)) {
            fields = append_pair(fields, "thinking_budget", o.thinking_budget);
        } else {
            fields = append_pair(fields, "thinking_budget", json::quote(o.thinking_budget));
        }
    }
    return fields;
}

std::string reasoning_fields_json(const RequestContext& context) {
    const cli::Options& o = context.options;
    if (!o.has_enable_thinking && !o.has_thinking_budget) {
        return "";
    }

    const ReasoningWireFormat format = reasoning_wire_format_for(context);
    const std::string effort = reasoning_effort_from_options(o);
    const long long tokens =
        o.has_thinking_budget ? parse_reasoning_token_budget(o.thinking_budget) : -1;
    std::string fields;

    switch (format) {
        case ReasoningWireFormat::None:
            return "";
        case ReasoningWireFormat::GenericThinking:
            return generic_reasoning_fields_json(o);
        case ReasoningWireFormat::OpenAiChatEffort:
            if (!effort.empty()) {
                fields = append_pair(fields, "reasoning_effort", json::quote(openai_effort_value(effort)));
            }
            return fields;
        case ReasoningWireFormat::OpenAiResponsesReasoning:
            if (!effort.empty()) {
                fields = append_pair(fields,
                                     "reasoning",
                                     "{\"effort\":" + json::quote(openai_effort_value(effort)) + "}");
            }
            return fields;
        case ReasoningWireFormat::OpenRouterReasoning:
            if (tokens == 0 || effort == "none") {
                fields = append_pair(fields, "reasoning", "{\"effort\":\"none\"}");
            } else if (tokens > 0) {
                fields = append_pair(fields, "reasoning", "{\"max_tokens\":" + std::to_string(tokens) + "}");
            } else if (!effort.empty()) {
                fields = append_pair(fields, "reasoning", "{\"effort\":" + json::quote(effort) + "}");
            } else if (o.has_enable_thinking && o.enable_thinking) {
                fields = append_pair(fields, "reasoning", "{\"enabled\":true}");
            }
            return fields;
        case ReasoningWireFormat::GeminiOpenAi:
            if (tokens > 0) {
                fields = append_pair(fields,
                                     "extra_body",
                                     "{\"google\":{\"thinking_config\":{\"thinking_budget\":" +
                                         std::to_string(tokens) + "}}}");
            } else if (!effort.empty()) {
                fields =
                    append_pair(fields, "reasoning_effort", json::quote(gemini_effort_value(effort)));
            }
            return fields;
        case ReasoningWireFormat::QwenThinking:
            return qwen_reasoning_fields_json(o);
        case ReasoningWireFormat::DeepSeekThinking: {
            if (effort == "none" || tokens == 0 || (o.has_enable_thinking && !o.enable_thinking)) {
                fields = append_pair(fields, "thinking", "{\"type\":\"disabled\"}");
                return fields;
            }
            fields = append_pair(fields, "thinking", "{\"type\":\"enabled\"}");
            if (!effort.empty()) {
                fields = append_pair(fields,
                                     "reasoning_effort",
                                     json::quote(deepseek_effort_value(effort)));
            }
            return fields;
        }
        case ReasoningWireFormat::XaiReasoningEffort:
            if (!effort.empty()) {
                fields = append_pair(fields, "reasoning_effort", json::quote(xai_effort_value(effort)));
            }
            return fields;
    }
    return "";
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

Error http_status_error(const RequestContext& context, const http::Response& response, const std::string& url) {
    ErrorCode code = ErrorCode::HttpStatus;
    if (response.status == 401 || response.status == 403) {
        code = ErrorCode::Auth;
    } else if (response.status == 429) {
        code = ErrorCode::RateLimit;
    }
    std::string body = response.body;
    if (body.size() > 2000) {
        body = body.substr(0, 2000) + "...";
    }
    std::string suggestion;
    if (context.profile.local_endpoint && response.status == 0) {
        suggestion = " Suggestion: start the local provider server and verify the port.";
    } else if (response.status == 404) {
        suggestion = " Suggestion: check whether the server expects /v1, /api/v1, or an explicit --chat-url/--responses-url/--models-url.";
    }
    return {code,
            "HTTP " + std::to_string(response.status) + " from " + url + ": " + body + suggestion};
}

std::string json_value_to_string(const json::Value& value) {
    std::ostringstream out;
    switch (value.type) {
        case json::Value::Type::Null:
            return "null";
        case json::Value::Type::Bool:
            return value.boolean ? "true" : "false";
        case json::Value::Type::Number:
            out << std::setprecision(17) << value.number;
            return out.str();
        case json::Value::Type::String:
            return json::quote(value.string);
        case json::Value::Type::Array:
            out << "[";
            for (size_t i = 0; i < value.array.size(); ++i) {
                if (i != 0) {
                    out << ",";
                }
                out << json_value_to_string(value.array[i]);
            }
            out << "]";
            return out.str();
        case json::Value::Type::Object:
            out << "{";
            for (auto it = value.object.begin(); it != value.object.end(); ++it) {
                if (it != value.object.begin()) {
                    out << ",";
                }
                out << json::quote(it->first) << ":" << json_value_to_string(it->second);
            }
            out << "}";
            return out.str();
    }
    return "null";
}

std::string provider_error_message(const json::Value& root) {
    const json::Value* err = root.get("error");
    if (err == nullptr) {
        return "";
    }
    if (err->is_string()) {
        return err->string;
    }
    if (const json::Value* msg = err->get("message")) {
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

Error parse_models_json(const std::string& body, ModelsResult& result) {
    json::ParseResult parsed = json::parse(body);
    if (!parsed.error.ok()) {
        return parsed.error;
    }
    if (const std::string provider_msg = provider_error_message(parsed.value); !provider_msg.empty()) {
        return {ErrorCode::ProviderSchema, "provider error: " + provider_msg};
    }
    const json::Value* data = parsed.value.get("data");
    if (data == nullptr || !data->is_array()) {
        return {ErrorCode::ProviderSchema, "models response did not contain a data array"};
    }
    for (const json::Value& item : data->array) {
        const json::Value* id = item.get("id");
        if (id != nullptr && id->is_string()) {
            result.model_ids.push_back(id->string);
        }
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

void parse_usage(const json::Value& usage, ChatResult& result) {
    if (!usage.is_object()) {
        return;
    }
    result.usage_json = json_value_to_string(usage);
    result.prompt_tokens = usage_token_value(usage, "prompt_tokens");
    if (result.prompt_tokens < 0) {
        result.prompt_tokens = usage_token_value(usage, "input_tokens");
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
        options.key = trim_ascii(ss.str());
    }
    if (!options.key_file.empty()) {
        Error err = read_file(options.key_file, options.key);
        if (!err.ok()) {
            return {{}, err};
        }
        options.key = trim_ascii(options.key);
    }
    if (!options.list_models && !options.repl && !options.tui && !options.editor && !options.benchmark &&
        trim_ascii(options.prompt).empty()) {
        return {{}, {ErrorCode::BadArgs, "prompt is empty; use -p/--prompt, --prompt-file, or --repl"}};
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
            if (!find_profile("custom_openai_chat", profile)) {
                return {{}, {ErrorCode::Internal, "custom_openai_chat profile is missing from the provider registry"}};
            }
            options.provider = profile.name;
        }
    } else {
        options.provider = profile.name;
    }
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
    if (changed && !options.quiet && !options.repl) {
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
                                      (profile.key_envs.empty() ? "PKCHAT_API_KEY" : profile.key_envs[0]) +
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

    const std::string model = lower_alias(context.options.model);
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

std::string display_name_for_profile(const std::string& profile_name) {
    for (const Profile& profile : built_in_profiles()) {
        if (profile.name == profile_name) {
            if (!profile.aliases.empty()) {
                return profile.aliases.front();
            }
            return profile.name;
        }
    }
    return profile_name;
}

}  // namespace pkchat::provider
