#include "provider/provider.hpp"

#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

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
    if (o.has_temperature) {
        json << ",\"temperature\":" << o.temperature;
    }
    if (o.has_top_p) {
        json << ",\"top_p\":" << o.top_p;
    }
    if (o.has_max_output_tokens) {
        json << ",\"max_tokens\":" << o.max_output_tokens;
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
    if (o.has_max_output_tokens) {
        comma();
        out << "\"max_output_tokens\":" << o.max_output_tokens;
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

void append_reasoning_details_text(const json::Value& details, std::string& out) {
    if (!details.is_array()) {
        return;
    }
    for (const json::Value& item : details.array) {
        std::string text;
        if (item.is_string()) {
            text = item.string;
        } else if (const json::Value* value = item.get("text")) {
            if (value->is_string()) {
                text = value->string;
            }
        }
        if (text.empty()) {
            continue;
        }
        if (!out.empty() && out.back() != '\n') {
            out.push_back('\n');
        }
        out += text;
    }
}

std::string reasoning_text_from_object(const json::Value& object) {
    std::string reasoning;
    if (const json::Value* value = object.get("reasoning_content")) {
        if (value->is_string()) {
            reasoning += value->string;
        }
    }
    if (reasoning.empty()) {
        if (const json::Value* value = object.get("reasoning")) {
            if (value->is_string()) {
                reasoning += value->string;
            }
        }
    }
    if (const json::Value* details = object.get("reasoning_details")) {
        append_reasoning_details_text(*details, reasoning);
    }
    return reasoning;
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
        content_text = content->string;
    } else if (reasoning.empty()) {
        return {ErrorCode::ProviderSchema, "chat response did not contain choices[0].message.content"};
    }
    result.content = content_with_reasoning_trace(reasoning, content_text);
    if (const json::Value* usage = parsed.value.get("usage")) {
        result.usage_json = json_value_to_string(*usage);
        if (const json::Value* completion_tokens = usage->get("completion_tokens")) {
            if (completion_tokens->type == json::Value::Type::Number && completion_tokens->number >= 0.0) {
                result.completion_tokens = static_cast<long long>(completion_tokens->number);
                result.completion_tokens_estimated = false;
            }
        }
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
        result.usage_json = json_value_to_string(*usage);
        if (const json::Value* output_tokens = usage->get("output_tokens")) {
            if (output_tokens->type == json::Value::Type::Number && output_tokens->number >= 0.0) {
                result.completion_tokens = static_cast<long long>(output_tokens->number);
                result.completion_tokens_estimated = false;
            }
        }
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

class SseParser {
   public:
    Error feed(const std::string& chunk, const DeltaCallback& on_delta, std::string& accumulated, bool& done) {
        buffer_ += chunk;
        size_t pos = 0;
        while (true) {
            size_t sep = buffer_.find("\n\n", pos);
            size_t sep_len = 2;
            const size_t crlf_sep = buffer_.find("\r\n\r\n", pos);
            if (crlf_sep != std::string::npos && (sep == std::string::npos || crlf_sep < sep)) {
                sep = crlf_sep;
                sep_len = 4;
            }
            if (sep == std::string::npos) {
                buffer_.erase(0, pos);
                return ok_error();
            }
            const std::string event = buffer_.substr(pos, sep - pos);
            pos = sep + sep_len;
            Error err = process_event(event, on_delta, accumulated, done);
            if (!err.ok() || done) {
                buffer_.erase(0, pos);
                return err;
            }
        }
    }

    Error finish(const DeltaCallback& on_delta, std::string& accumulated, bool& done) {
        if (!buffer_.empty()) {
            std::string event = buffer_;
            buffer_.clear();
            Error err = process_event(event, on_delta, accumulated, done);
            if (!err.ok()) {
                return err;
            }
        }
        if (!done) {
            return close_reasoning(on_delta, accumulated, false);
        }
        return ok_error();
    }

   private:
    std::string buffer_;
    bool reasoning_open_ = false;

    Error emit_text(const std::string& text, const DeltaCallback& on_delta, std::string& accumulated) {
        if (text.empty()) {
            return ok_error();
        }
        accumulated += text;
        return on_delta(text);
    }

    Error emit_reasoning(const std::string& text, const DeltaCallback& on_delta, std::string& accumulated) {
        if (text.empty()) {
            return ok_error();
        }
        if (!reasoning_open_) {
            Error err = emit_text("<think>", on_delta, accumulated);
            if (!err.ok()) {
                return err;
            }
            reasoning_open_ = true;
        }
        return emit_text(text, on_delta, accumulated);
    }

    Error close_reasoning(const DeltaCallback& on_delta, std::string& accumulated, bool before_content) {
        if (!reasoning_open_) {
            return ok_error();
        }
        reasoning_open_ = false;
        return emit_text(before_content ? "</think>\n\n" : "</think>", on_delta, accumulated);
    }

    Error process_event(const std::string& event,
                        const DeltaCallback& on_delta,
                        std::string& accumulated,
                        bool& done) {
        std::istringstream lines(event);
        std::string line;
        std::string data;
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty() || line[0] == ':') {
                continue;
            }
            if (line.rfind("data:", 0) == 0) {
                std::string value = line.substr(5);
                if (!value.empty() && value.front() == ' ') {
                    value.erase(value.begin());
                }
                if (!data.empty()) {
                    data.push_back('\n');
                }
                data += value;
            }
        }
        if (data.empty()) {
            return ok_error();
        }
        if (data == "[DONE]") {
            Error err = close_reasoning(on_delta, accumulated, false);
            if (!err.ok()) {
                return err;
            }
            done = true;
            return ok_error();
        }
        json::ParseResult parsed = json::parse(data);
        if (!parsed.error.ok()) {
            return {ErrorCode::SseParse, parsed.error.message};
        }
        if (const std::string provider_msg = provider_error_message(parsed.value); !provider_msg.empty()) {
            return {ErrorCode::ProviderSchema, "provider error: " + provider_msg};
        }
        const json::Value* choices = parsed.value.get("choices");
        if (choices == nullptr || !choices->is_array() || choices->array.empty()) {
            return ok_error();
        }
        const json::Value* delta = choices->array[0].get("delta");
        if (delta == nullptr) {
            return ok_error();
        }
        const std::string reasoning = reasoning_text_from_object(*delta);
        Error err = emit_reasoning(reasoning, on_delta, accumulated);
        if (!err.ok()) {
            return err;
        }
        const json::Value* content = delta->get("content");
        if (content != nullptr && content->is_string() && !content->string.empty()) {
            err = close_reasoning(on_delta, accumulated, true);
            if (!err.ok()) {
                return err;
            }
            return emit_text(content->string, on_delta, accumulated);
        }
        return ok_error();
    }
};

class ResponsesSseParser {
   public:
    Error feed(const std::string& chunk, const DeltaCallback& on_delta, ChatResult& result, bool& done) {
        buffer_ += chunk;
        size_t pos = 0;
        while (true) {
            size_t sep = buffer_.find("\n\n", pos);
            size_t sep_len = 2;
            const size_t crlf_sep = buffer_.find("\r\n\r\n", pos);
            if (crlf_sep != std::string::npos && (sep == std::string::npos || crlf_sep < sep)) {
                sep = crlf_sep;
                sep_len = 4;
            }
            if (sep == std::string::npos) {
                buffer_.erase(0, pos);
                return ok_error();
            }
            const std::string event = buffer_.substr(pos, sep - pos);
            pos = sep + sep_len;
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

    Error emit_text(const std::string& text, const DeltaCallback& on_delta, ChatResult& result) {
        if (text.empty()) {
            return ok_error();
        }
        result.content += text;
        return on_delta(text);
    }

    Error emit_reasoning(const std::string& text, const DeltaCallback& on_delta, ChatResult& result) {
        if (text.empty()) {
            return ok_error();
        }
        if (!reasoning_open_) {
            Error err = emit_text("<think>", on_delta, result);
            if (!err.ok()) {
                return err;
            }
            reasoning_open_ = true;
        }
        return emit_text(text, on_delta, result);
    }

    Error close_reasoning(const DeltaCallback& on_delta, ChatResult& result, bool before_content) {
        if (!reasoning_open_) {
            return ok_error();
        }
        reasoning_open_ = false;
        return emit_text(before_content ? "</think>\n\n" : "</think>", on_delta, result);
    }

    static bool ends_with(const std::string& text, const std::string& suffix) {
        return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    Error process_event(const std::string& event,
                        const DeltaCallback& on_delta,
                        ChatResult& result,
                        bool& done) {
        std::istringstream lines(event);
        std::string line;
        std::string data;
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty() || line[0] == ':') {
                continue;
            }
            if (line.rfind("data:", 0) == 0) {
                std::string value = line.substr(5);
                if (!value.empty() && value.front() == ' ') {
                    value.erase(value.begin());
                }
                if (!data.empty()) {
                    data.push_back('\n');
                }
                data += value;
            }
        }
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
            return {ErrorCode::SseParse, parsed.error.message};
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
            return emit_text(delta->string, on_delta, result);
        }
        return ok_error();
    }
};

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
    if (!options.list_models && !options.repl && !options.tui && trim_ascii(options.prompt).empty()) {
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
    req.body = context.api_kind == ApiKind::Responses ? build_responses_request_json(context, messages)
                                                      : serialize_chat_request(context, messages);
    SseParser chat_parser;
    ResponsesSseParser responses_parser;
    bool done = false;
    if (context.options.stream) {
        req.on_body = [&](const std::string& chunk) -> Error {
            if (context.api_kind == ApiKind::Responses) {
                return responses_parser.feed(chunk, timed_delta, result, done);
            }
            return chat_parser.feed(chunk, timed_delta, result.content, done);
        };
    }

    const http::Result http_result = http::perform(req, {context.api_key});
    if (!http_result.error.ok()) {
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
                        : chat_parser.finish(timed_delta, result.content, done);
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

}  // namespace pkchat::provider
