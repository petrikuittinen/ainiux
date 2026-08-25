#include "provider/image_gemini.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>
#include <utility>

#include "config/image_catalog.hpp"
#include "input/input.hpp"
#include "json/json.hpp"

namespace ainiux::provider {
namespace {

std::string compact_detail(std::string text) {
    text = ascii_trim(std::move(text));
    for (char& ch : text) {
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
    }
    if (text.size() > 500) text = text.substr(0, 500) + "...";
    return text;
}

std::string json_string_field(const json::Value* value) {
    if (value != nullptr && value->is_string()) return value->string;
    return {};
}

long long json_int_field(const json::Value* value) {
    if (value == nullptr) return 0;
    if (value->type == json::Value::Type::Number) {
        return static_cast<long long>(value->number);
    }
    if (value->is_string()) {
        try {
            return std::stoll(value->string);
        } catch (...) {
            return 0;
        }
    }
    return 0;
}

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

std::string join_url(const std::string& base, const std::string& suffix) {
    if (base.empty()) return suffix;
    if (base.back() == '/' && !suffix.empty() && suffix.front() == '/') {
        return base.substr(0, base.size() - 1) + suffix;
    }
    if (base.back() != '/' && (suffix.empty() || suffix.front() != '/')) return base + "/" + suffix;
    return base + suffix;
}

bool ends_with_ignore_case(const std::string& text, const std::string& suffix) {
    if (text.size() < suffix.size()) return false;
    return ascii_lower(text.substr(text.size() - suffix.size())) == ascii_lower(suffix);
}

bool looks_like_wh(const std::string& text) {
    const std::string lower = ascii_lower(ascii_trim(text));
    const size_t sep = lower.find('x');
    if (sep == std::string::npos || sep == 0 || sep + 1 >= lower.size()) return false;
    for (size_t i = 0; i < lower.size(); ++i) {
        if (i == sep) continue;
        if (std::isdigit(static_cast<unsigned char>(lower[i])) == 0) return false;
    }
    return true;
}

std::string gemini_mime_type(const std::string& format) {
    const std::string lower = ascii_lower(ascii_trim(format));
    if (lower.empty() || lower == "auto") return {};
    if (lower.rfind("image/", 0) == 0) return lower;
    if (lower == "png") return "image/png";
    if (lower == "jpeg" || lower == "jpg") return "image/jpeg";
    if (lower == "webp") return "image/webp";
    return {};
}

std::string format_from_mime(const std::string& mime) {
    const std::string lower = ascii_lower(ascii_trim(mime));
    if (lower == "image/png") return "png";
    if (lower == "image/jpeg" || lower == "image/jpg") return "jpeg";
    if (lower == "image/webp") return "webp";
    return {};
}

bool valid_gemini_model(const std::string& api_model) {
    if (api_model.empty() || api_model.front() == '/' || api_model.back() == '/') return false;
    for (char ch : api_model) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (!(std::isalnum(uch) != 0 || ch == '_' || ch == '-' || ch == '.')) return false;
    }
    return true;
}

bool has_header_name(const std::vector<std::string>& headers, const std::string& name) {
    const std::string want = ascii_lower(name);
    for (const std::string& header : headers) {
        const size_t colon = header.find(':');
        if (colon == std::string::npos || colon == 0) continue;
        if (ascii_lower(header.substr(0, colon)) == want) return true;
    }
    return false;
}

http::Request make_gemini_http(const RequestContext& context,
                               const std::string& url,
                               runtime::CancellationToken cancellation) {
    http::Request req;
    req.method = "POST";
    req.url = url;
    req.headers = context.headers;
    req.headers.emplace_back("Content-Type: application/json");
    if (!context.api_key.empty() && !has_header_name(req.headers, "x-goog-api-key")) {
        req.headers.emplace_back("x-goog-api-key: " + context.api_key);
    }
    req.connect_timeout_seconds = context.options.connect_timeout_seconds;
    req.timeout_seconds = context.options.timeout_seconds;
    req.proxy = context.options.proxy;
    req.insecure_tls = context.options.insecure_tls;
    req.trace = context.options.trace_http;
    req.cancellation = cancellation;
    return req;
}

std::string gemini_error_detail(const json::Value& root) {
    std::string message;
    if (const json::Value* errors = root.get("errors")) {
        if (errors->is_array()) {
            for (const json::Value& item : errors->array) {
                std::string part = json_string_field(item.get("message"));
                if (part.empty() && item.is_string()) part = item.string;
                if (part.empty()) continue;
                if (!message.empty()) message += "; ";
                message += part;
            }
        }
    }
    if (message.empty()) {
        if (const json::Value* error = root.get("error")) {
            if (error->is_string()) message = error->string;
            else if (error->is_object()) {
                message = json_string_field(error->get("message"));
            }
        }
    }
    return compact_detail(message);
}

bool gemini_status_failed(const std::string& status) {
    const std::string lower = ascii_lower(status);
    return lower == "failed" || lower == "cancelled" || lower == "canceled" ||
           lower == "incomplete" || lower == "budget_exceeded" ||
           lower == "requires_action" || lower == "in_progress" || lower == "queued";
}

Error gemini_http_status_error(const http::Response& response, const std::string& url) {
    ErrorCode code = ErrorCode::HttpStatus;
    if (response.status == 401 || response.status == 403) code = ErrorCode::Auth;
    else if (response.status == 429) code = ErrorCode::RateLimit;

    std::string summary = "Unexpected HTTP status from Gemini.";
    if (response.status == 400) {
        summary = "Bad request. Gemini rejected the image prompt, size, format, or attachments.";
    } else if (response.status == 401) {
        summary = "Authentication failed. Check GEMINI_API_KEY.";
    } else if (response.status == 403) {
        summary = "Access forbidden. The Gemini key may not have access to this image model.";
    } else if (response.status == 404) {
        summary = "Gemini Interactions endpoint or model was not found. Check -m/--model.";
    } else if (response.status == 429) {
        summary = "Too many requests. Try again later or use a cheaper Gemini image model.";
    } else if (response.status >= 500 && response.status < 600) {
        summary = "Gemini failed while generating the image. Try again later.";
    }

    std::string message = "HTTP " + std::to_string(response.status) + ": " + summary + "\nURL: " + url;
    const json::ParseResult parsed = json::parse(response.body);
    std::string detail;
    if (parsed.error.ok()) detail = gemini_error_detail(parsed.value);
    if (detail.empty()) detail = compact_detail(response.body);
    if (!detail.empty()) message += "\nProvider message: " + detail;
    if (response.status == 401 || response.status == 403) {
        message += "\nSuggestion: Set GEMINI_API_KEY and pass --provider gemini.";
    }
    return {code, message};
}

void consider_image_block(const json::Value& block, std::string& data, std::string& mime) {
    if (!block.is_object()) return;
    if (ascii_lower(json_string_field(block.get("type"))) != "image") return;
    const std::string candidate = json_string_field(block.get("data"));
    if (candidate.empty()) return;
    data = candidate;
    mime = json_string_field(block.get("mime_type"));
}

void consider_content(const json::Value* content, std::string& data, std::string& mime) {
    if (content == nullptr) return;
    if (content->is_array()) {
        for (const json::Value& item : content->array) {
            consider_image_block(item, data, mime);
        }
        return;
    }
    consider_image_block(*content, data, mime);
}

}  // namespace

std::string gemini_interactions_url(const RequestContext& context) {
    std::string base = context.base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (ends_with_ignore_case(base, "/openai")) {
        base.resize(base.size() - 7);
        while (!base.empty() && base.back() == '/') base.pop_back();
    }
    return join_url(base, "/interactions");
}

Error serialize_gemini_request(const ImageGenerateRequest& request, std::string& body) {
    body.clear();
    const ImageCapability& capability = request.capability;
    const std::string model =
        capability.api_model.empty() ? request.model : capability.api_model;
    if (!valid_gemini_model(model)) {
        return {ErrorCode::BadArgs,
                "Gemini image models require api_model (for example gemini-3.1-flash-image)"};
    }
    if (request.prompt.empty()) {
        return {ErrorCode::BadArgs, "image generation requires a prompt"};
    }

    json::Value input = json_array_value();
    json::Value text = json_object_value();
    text.object["type"] = json_string_value("text");
    text.object["text"] = json_string_value(request.prompt);
    input.array.push_back(std::move(text));
    for (const ImageInput& image : request.images) {
        json::Value part = json_object_value();
        part.object["type"] = json_string_value("image");
        part.object["mime_type"] =
            json_string_value(image.mime_type.empty() ? std::string("image/png") : image.mime_type);
        part.object["data"] = json_string_value(image.base64_data);
        input.array.push_back(std::move(part));
    }

    json::Value response_format = json_object_value();
    response_format.object["type"] = json_string_value("image");
    if (!request.size.empty() && !capability.size_field.empty() && !looks_like_wh(request.size)) {
        response_format.object[capability.size_field] = json_string_value(request.size);
    }
    if (!request.aspect.empty() && !capability.aspect_field.empty()) {
        response_format.object[capability.aspect_field] = json_string_value(request.aspect);
    }
    const std::string mime = gemini_mime_type(request.output_format);
    if (!mime.empty() && !capability.format_field.empty()) {
        response_format.object[capability.format_field] = json_string_value(mime);
    }

    json::Value root = json_object_value();
    root.object["model"] = json_string_value(model);
    root.object["input"] = std::move(input);
    root.object["response_format"] = std::move(response_format);
    root.object["store"] = json_bool_value(false);
    body = json::stringify(root);
    return ok_error();
}

Error parse_gemini_interaction(const std::string& body, ImageGenerateResult& result) {
    result.bytes.clear();
    result.output_format.clear();
    result.input_tokens = 0;
    result.output_tokens = 0;
    result.total_tokens = 0;

    const json::ParseResult parsed = json::parse(body);
    if (!parsed.error.ok()) {
        return {ErrorCode::JsonParse,
                "Gemini Interactions response is not valid JSON: " + parsed.error.message};
    }

    const std::string status = json_string_field(parsed.value.get("status"));
    const std::string detail = gemini_error_detail(parsed.value);
    if (gemini_status_failed(status)) {
        std::string message = "Gemini image generation failed";
        if (!status.empty()) message += " (status " + status + ")";
        if (!detail.empty()) message += ": " + detail;
        return {ErrorCode::ProviderSchema, message};
    }

    std::string encoded;
    std::string mime;
    if (const json::Value* steps = parsed.value.get("steps")) {
        if (steps->is_array()) {
            for (const json::Value& step : steps->array) {
                if (!step.is_object()) continue;
                if (ascii_lower(json_string_field(step.get("type"))) == "thought") continue;
                consider_content(step.get("content"), encoded, mime);
            }
        }
    }
    if (encoded.empty()) {
        if (const json::Value* output_image = parsed.value.get("output_image")) {
            consider_image_block(*output_image, encoded, mime);
            if (encoded.empty()) encoded = json_string_field(output_image->get("data"));
            if (mime.empty()) mime = json_string_field(output_image->get("mime_type"));
        }
    }
    if (encoded.empty()) {
        return {ErrorCode::ProviderSchema,
                "Gemini Interactions response did not contain an image in steps or output_image"};
    }

    encoded.erase(std::remove_if(encoded.begin(), encoded.end(),
                                 [](unsigned char ch) { return std::isspace(ch) != 0; }),
                  encoded.end());
    Error decode_error = input::decode_base64(encoded, result.bytes);
    encoded.clear();
    encoded.shrink_to_fit();
    if (!decode_error.ok()) {
        return {ErrorCode::ProviderSchema, "Gemini image data is malformed base64"};
    }
    if (result.bytes.empty()) {
        return {ErrorCode::ProviderSchema, "Gemini image decoded to an empty file"};
    }
    result.output_format = format_from_mime(mime);

    if (const json::Value* usage = parsed.value.get("usage")) {
        result.input_tokens = json_int_field(usage->get("total_input_tokens"));
        result.output_tokens = json_int_field(usage->get("total_output_tokens"));
        result.total_tokens = json_int_field(usage->get("total_tokens"));
    }
    return ok_error();
}

Error generate_gemini_image(const RequestContext& context,
                            const ImageGenerateRequest& request,
                            ImageGenerateResult& result,
                            runtime::CancellationToken cancellation) {
    if (cancellation.cancelled()) {
        return {ErrorCode::Cancelled, "image generation cancelled: before request"};
    }
    if (request.prompt.empty()) {
        return {ErrorCode::BadArgs, "image generation requires a prompt"};
    }

    std::string body;
    Error serialize_error = serialize_gemini_request(request, body);
    if (!serialize_error.ok()) return serialize_error;

    const std::string url = gemini_interactions_url(context);
    if (url.empty() || url == "/interactions") {
        return {ErrorCode::BadUrl, "no base URL configured; pass BASE_URL or --base-url"};
    }

    http::Request req = make_gemini_http(context, url, cancellation);
    req.body = std::move(body);

    const auto started = std::chrono::steady_clock::now();
    const http::Result http_result = http::perform(req, {context.api_key});
    result.total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - started)
                          .count();
    if (!http_result.error.ok()) return http_result.error;
    if (http_result.response.status < 200 || http_result.response.status >= 300) {
        return gemini_http_status_error(http_result.response, url);
    }
    return parse_gemini_interaction(http_result.response.body, result);
}

}  // namespace ainiux::provider
