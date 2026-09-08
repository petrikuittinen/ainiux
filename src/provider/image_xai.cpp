#include "provider/image_xai.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>

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

std::string join_url(const std::string& base, const std::string& suffix) {
    if (base.empty()) return suffix;
    if (base.back() == '/' && !suffix.empty() && suffix.front() == '/') {
        return base.substr(0, base.size() - 1) + suffix;
    }
    if (base.back() != '/' && (suffix.empty() || suffix.front() != '/')) return base + "/" + suffix;
    return base + suffix;
}

bool has_authorization_header(const std::vector<std::string>& headers) {
    for (const std::string& header : headers) {
        const size_t colon = header.find(':');
        if (colon == std::string::npos || colon == 0) continue;
        if (ascii_lower(header.substr(0, colon)) == "authorization") return true;
    }
    return false;
}

json::Value json_string_value(const std::string& text) {
    json::Value value;
    value.type = json::Value::Type::String;
    value.string = text;
    return value;
}

json::Value json_number_value(double number) {
    json::Value value;
    value.type = json::Value::Type::Number;
    value.number = number;
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

json::Value image_ref(const ImageInput& image) {
    json::Value ref = json_object_value();
    ref.object["url"] = json_string_value("data:" + image.mime_type + ";base64," + image.base64_data);
    ref.object["type"] = json_string_value("image_url");
    return ref;
}

std::string json_string_field(const json::Value* value) {
    if (value != nullptr && value->is_string()) return value->string;
    return {};
}

long long json_int_field(const json::Value* value) {
    if (value == nullptr) return 0;
    if (value->type == json::Value::Type::Number) return static_cast<long long>(value->number);
    return 0;
}

Error xai_image_http_error(const http::Response& response, const std::string& url) {
    ErrorCode code = ErrorCode::HttpStatus;
    if (response.status == 401 || response.status == 403) code = ErrorCode::Auth;
    else if (response.status == 429) code = ErrorCode::RateLimit;
    std::string summary = "Unexpected HTTP status from xAI Imagine.";
    if (response.status == 400 || response.status == 422) {
        summary = "Bad request. xAI rejected the image prompt, size, quality, or attachments.";
    } else if (response.status == 401) {
        summary = "Authentication failed. Check XAI_API_KEY or GROK_API_KEY.";
    } else if (response.status == 403) {
        summary = "Access forbidden. The xAI key may not have Imagine access.";
    } else if (response.status == 404) {
        summary = "xAI Imagine model was not found. Check -m/--model.";
    } else if (response.status == 429) {
        summary = "Too many requests. Try again later.";
    } else if (response.status >= 500 && response.status < 600) {
        summary = "xAI failed while generating the image. Try again later.";
    }
    std::string message = "HTTP " + std::to_string(response.status) + ": " + summary + "\nURL: " + url;
    const json::ParseResult parsed = json::parse(response.body);
    std::string detail;
    if (parsed.error.ok()) {
        if (const json::Value* error = parsed.value.get("error")) {
            if (error->is_string()) detail = error->string;
            else if (error->is_object()) {
                detail = json_string_field(error->get("message"));
                if (detail.empty()) detail = json_string_field(error->get("detail"));
            }
        }
        if (detail.empty()) detail = json_string_field(parsed.value.get("message"));
    }
    if (detail.empty()) detail = compact_detail(response.body);
    if (!detail.empty()) message += "\nProvider message: " + detail;
    if (response.status == 401 || response.status == 403) {
        message += "\nSuggestion: Set XAI_API_KEY or GROK_API_KEY and pass --provider xai.";
    }
    return {code, message};
}

}  // namespace

Error serialize_xai_imagine_request(const ImageGenerateRequest& request, std::string& body) {
    body.clear();
    json::Value root = json_object_value();
    const std::string model =
        request.capability.api_model.empty() ? request.model : request.capability.api_model;
    root.object["model"] = json_string_value(model);
    root.object["prompt"] = json_string_value(request.prompt);
    root.object["n"] = json_number_value(1);
    root.object["response_format"] = json_string_value("b64_json");
    if (!request.size.empty()) root.object["resolution"] = json_string_value(request.size);
    if (!request.aspect.empty()) root.object["aspect_ratio"] = json_string_value(request.aspect);
    if (!request.quality.empty()) root.object["quality"] = json_string_value(request.quality);
    if (request.images.size() == 1) {
        root.object["image"] = image_ref(request.images.front());
    } else if (request.images.size() > 1) {
        json::Value images = json_array_value();
        for (const ImageInput& image : request.images) images.array.push_back(image_ref(image));
        root.object["images"] = std::move(images);
    }
    body = json::stringify(root);
    return ok_error();
}

Error parse_xai_imagine_response(const std::string& body, ImageGenerateResult& result) {
    const json::ParseResult parsed = json::parse(body);
    if (!parsed.error.ok()) {
        return {ErrorCode::JsonParse, "xAI Imagine response is not valid JSON: " + parsed.error.message};
    }
    if (const json::Value* error = parsed.value.get("error")) {
        std::string detail;
        if (error->is_string()) detail = error->string;
        else if (error->is_object()) detail = json_string_field(error->get("message"));
        if (!detail.empty()) {
            return {ErrorCode::ProviderSchema, "xAI Imagine error: " + compact_detail(detail)};
        }
    }
    const json::Value* data = parsed.value.get("data");
    if (data == nullptr || !data->is_array() || data->array.empty()) {
        return {ErrorCode::ProviderSchema, "xAI Imagine response did not contain data[0].b64_json"};
    }
    const json::Value* first = data->at(0);
    if (first == nullptr || !first->is_object()) {
        return {ErrorCode::ProviderSchema, "xAI Imagine response did not contain data[0].b64_json"};
    }
    const bool moderated = first->get("respect_moderation") &&
        first->get("respect_moderation")->type == json::Value::Type::Bool &&
        !first->get("respect_moderation")->boolean;
    const json::Value* b64 = first->get("b64_json");
    if (b64 == nullptr || !b64->is_string() || b64->string.empty()) {
        if (moderated) return {ErrorCode::ProviderSchema, "xAI Imagine blocked the image by moderation"};
        return {ErrorCode::ProviderSchema, "xAI Imagine response did not contain data[0].b64_json"};
    }
    std::string encoded = b64->string;
    encoded.erase(std::remove_if(encoded.begin(), encoded.end(),
                                 [](unsigned char ch) { return std::isspace(ch) != 0; }),
                  encoded.end());
    Error decode_error = input::decode_base64(encoded, result.bytes);
    if (!decode_error.ok()) return {ErrorCode::ProviderSchema, "xAI Imagine b64_json is malformed"};
    const std::string mime = json_string_field(first->get("mime_type"));
    if (mime == "image/jpeg") result.output_format = "jpeg";
    else if (mime == "image/webp") result.output_format = "webp";
    else if (mime == "image/png") result.output_format = "png";
    if (const json::Value* usage = parsed.value.get("usage")) {
        result.total_tokens = json_int_field(usage->get("total_tokens"));
        result.input_tokens = json_int_field(usage->get("input_tokens"));
        result.output_tokens = json_int_field(usage->get("output_tokens"));
    }
    return ok_error();
}

Error generate_xai_imagine_image(const RequestContext& context,
                                 const ImageGenerateRequest& request,
                                 ImageGenerateResult& result,
                                 runtime::CancellationToken cancellation) {
    if (cancellation.cancelled()) {
        return {ErrorCode::Cancelled, "image generation cancelled: before request"};
    }
    std::string body;
    Error serialize_error = serialize_xai_imagine_request(request, body);
    if (!serialize_error.ok()) return serialize_error;
    const bool edits = !request.images.empty();
    const std::string url = join_url(context.base_url, edits ? "/images/edits" : "/images/generations");
    http::Request req;
    req.method = "POST";
    req.url = url;
    req.headers = context.headers;
    req.headers.emplace_back("Content-Type: application/json");
    if (!context.api_key.empty() && !has_authorization_header(req.headers)) {
        req.headers.emplace_back("Authorization: Bearer " + context.api_key);
    }
    req.connect_timeout_seconds = context.options.connect_timeout_seconds;
    req.timeout_seconds = context.options.timeout_seconds;
    req.proxy = context.options.proxy;
    req.insecure_tls = context.options.insecure_tls;
    req.trace = context.options.trace_http;
    req.cancellation = cancellation;
    req.body = std::move(body);
    req.retry_post_on_send_error = false;
    const auto started = std::chrono::steady_clock::now();
    const http::Result http_result = http::perform(req, {context.api_key});
    result.total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - started)
                          .count();
    if (!http_result.error.ok()) return http_result.error;
    if (http_result.response.status < 200 || http_result.response.status >= 300) {
        return xai_image_http_error(http_result.response, url);
    }
    return parse_xai_imagine_response(http_result.response.body, result);
}

}  // namespace ainiux::provider
