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

Profile profile_for(const std::string& requested) {
    const std::string name = lower_alias(requested.empty() ? "openai" : requested);
    if (name == "lm_studio" || name == "lmstudio") {
        return {"lm_studio",
                "http://localhost:1234/v1",
                false,
                true,
                {"LMSTUDIO_API_KEY", "LM_STUDIO_API_KEY", "PKCHAT_API_KEY"}};
    }
    if (name == "openrouter") {
        return {"openrouter",
                "https://openrouter.ai/api/v1",
                true,
                false,
                {"OPENROUTER_API_KEY", "PKCHAT_API_KEY"}};
    }
    if (name == "custom_openai_chat" || name == "custom") {
        return {"custom_openai_chat", "", false, false, {"PKCHAT_API_KEY"}};
    }
    return {"openai", "https://api.openai.com/v1", true, false, {"OPENAI_API_KEY", "PKCHAT_API_KEY"}};
}

bool is_provider_shortcut(const std::string& text) {
    const std::string name = lower_alias(text);
    return name == "openai" || name == "openrouter" || name == "lm_studio" || name == "lmstudio";
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
    return "";
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
        json << "{\"role\":" << json::quote(messages[i].role)
             << ",\"content\":" << json::quote(messages[i].content) << "}";
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
        suggestion = " Suggestion: check whether the server expects /v1, /api/v1, or an explicit --chat-url/--models-url.";
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
    const json::Value* content = message->get("content");
    if (content == nullptr || !content->is_string()) {
        return {ErrorCode::ProviderSchema, "chat response did not contain choices[0].message.content"};
    }
    result.content = content->string;
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
        if (buffer_.empty()) {
            return ok_error();
        }
        std::string event = buffer_;
        buffer_.clear();
        return process_event(event, on_delta, accumulated, done);
    }

   private:
    std::string buffer_;

    static Error process_event(const std::string& event,
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
        const json::Value* content = delta == nullptr ? nullptr : delta->get("content");
        if (content != nullptr && content->is_string() && !content->string.empty()) {
            accumulated += content->string;
            return on_delta(content->string);
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

    Profile profile = profile_for(options.provider);
    std::string base = options.base_url;
    if (!options.positional_url.empty() && is_provider_shortcut(options.positional_url)) {
        profile = profile_for(options.positional_url);
        options.provider = profile.name;
    } else if (base.empty() && !options.positional_url.empty()) {
        base = options.positional_url;
        if (input_options.provider == "openai") {
            profile = profile_for("custom_openai_chat");
            options.provider = profile.name;
        }
    }
    if (base.empty()) {
        base = profile.base_url;
    }
    if (base.empty() && options.chat_url.empty() && options.models_url.empty()) {
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
    if (changed && !options.quiet) {
        std::cerr << "Using base URL: " << base << "\n";
    }

    for (const std::string& header : options.headers) {
        err = validate_header(header);
        if (!err.ok()) {
            return {{}, err};
        }
    }

    std::string key = resolve_key(options, profile);
    if (profile.requires_bearer_key && key.empty() && !has_authorization_header(options.headers)) {
        return {{}, {ErrorCode::Config, "provider " + profile.name + " requires an API key; set " +
                                      (profile.key_envs.empty() ? "PKCHAT_API_KEY" : profile.key_envs[0]) +
                                      " or use --key-env/--key-file/--key-stdin"}};
    }
    RequestContext context;
    context.options = options;
    context.profile = profile;
    context.base_url = base;
    context.chat_url = options.chat_url.empty() ? join_url(base, "/chat/completions") : options.chat_url;
    context.models_url = options.models_url.empty() ? join_url(base, "/models") : options.models_url;
    context.api_key = key;
    context.headers = options.headers;
    return {context, ok_error()};
}

Error list_models(const RequestContext& context, ModelsResult& result, runtime::CancellationToken cancellation) {
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
    if (cancellation.cancelled()) {
        return {ErrorCode::Cancelled, "chat request cancelled before it started"};
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

    http::Request req = base_http_request(context, "POST", context.chat_url, cancellation);
    req.body = build_chat_request_json(context, messages);
    SseParser parser;
    bool done = false;
    if (context.options.stream) {
        req.on_body = [&](const std::string& chunk) -> Error {
            return parser.feed(chunk, timed_delta, result.content, done);
        };
    }

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
        return http_status_error(context, http_result.response, context.chat_url);
    }
    result.model = context.options.model;
    if (context.options.stream) {
        Error err = parser.finish(timed_delta, result.content, done);
        if (!err.ok()) {
            return err;
        }
    } else {
        Error err = parse_chat_json(http_result.response.body, result);
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
