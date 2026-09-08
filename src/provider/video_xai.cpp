#include "provider/video.hpp"

#include <chrono>
#include <thread>

#include "input/input.hpp"
#include "json/json.hpp"
#include "security/redact.hpp"

namespace ainiux::provider {
namespace {

constexpr int kPollSliceMs = 100;
constexpr int kPollIntervalMs = 1000;

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
    if (base.back() == '/') return base.substr(0, base.size() - 1) + suffix;
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

http::Request make_xai_http(const RequestContext& context, const std::string& method,
                            const std::string& url, runtime::CancellationToken cancellation) {
    http::Request req;
    req.method = method;
    req.url = url;
    req.headers = context.headers;
    if (!context.api_key.empty() && !has_authorization_header(req.headers)) {
        req.headers.emplace_back("Authorization: Bearer " + context.api_key);
    }
    req.connect_timeout_seconds = context.options.connect_timeout_seconds;
    req.timeout_seconds = context.options.timeout_seconds > 0 ? context.options.timeout_seconds : 300;
    req.proxy = context.options.proxy;
    req.insecure_tls = context.options.insecure_tls;
    req.trace = context.options.trace_http;
    req.cancellation = cancellation;
    return req;
}

json::Value json_string_value(const std::string& text) {
    json::Value value;
    value.type = json::Value::Type::String;
    value.string = text;
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

json::Value coerce_number(const json::Value& value) {
    if (!value.is_string() || value.string.empty()) return value;
    const std::string& text = value.string;
    std::size_t index = text[0] == '-' ? 1 : 0;
    if (index >= text.size()) return value;
    for (std::size_t i = index; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') return value;
    }
    json::Value number;
    number.type = json::Value::Type::Number;
    number.number = std::stod(text);
    return number;
}

std::string json_string_field(const json::Value* value) {
    if (value != nullptr && value->is_string()) return value->string;
    return {};
}

std::string wire_model(const VideoGenerateRequest& request) {
    std::string model =
        request.capability.api_model.empty() ? request.model : request.capability.api_model;
    const std::size_t slash = model.find('/');
    if (slash != std::string::npos) model.erase(slash);
    return model;
}

Error media_data_uri(const VideoInput& media, std::string& uri) {
    if (!media.remote_url.empty() && media.remote_url.rfind("data:", 0) == 0) {
        uri = media.remote_url;
        return ok_error();
    }
    if (!media.bytes || media.bytes->empty()) {
        return {ErrorCode::Internal, "xAI Imagine video input data is unavailable"};
    }
    uri = "data:" + media.mime_type + ";base64," + input::encode_base64(*media.bytes);
    return ok_error();
}

json::Value image_url_object(const std::string& uri) {
    json::Value image = json_object_value();
    image.object["url"] = json_string_value(uri);
    return image;
}

Error xai_video_http_error(const http::Response& response, const std::string& url) {
    ErrorCode code = ErrorCode::HttpStatus;
    if (response.status == 401 || response.status == 403) code = ErrorCode::Auth;
    else if (response.status == 429) code = ErrorCode::RateLimit;
    std::string summary = "Unexpected HTTP status from xAI Imagine video.";
    if (response.status == 400 || response.status == 422) {
        summary = "Bad request. xAI rejected the video prompt, settings, or attachments.";
    } else if (response.status == 401) {
        summary = "Authentication failed. Check XAI_API_KEY or GROK_API_KEY.";
    } else if (response.status == 403) {
        summary = "Access forbidden. The xAI key may not have Imagine video access.";
    } else if (response.status == 404) {
        summary = "xAI Imagine video model or request was not found. Check -m/--model.";
    } else if (response.status == 429) {
        summary = "Too many requests. Try again later.";
    } else if (response.status >= 500 && response.status < 600) {
        summary = "xAI failed while generating the video. Try again later.";
    }
    std::string message = "HTTP " + std::to_string(response.status) + ": " + summary + "\nURL: " + url;
    const json::ParseResult parsed = json::parse(response.body);
    std::string detail;
    if (parsed.error.ok()) {
        if (const json::Value* error = parsed.value.get("error")) {
            if (error->is_string()) detail = error->string;
            else if (error->is_object()) detail = json_string_field(error->get("message"));
        }
        if (detail.empty()) detail = json_string_field(parsed.value.get("message"));
    }
    if (detail.empty()) detail = compact_detail(redact_secrets(response.body, {}));
    if (!detail.empty()) message += "\nProvider message: " + detail;
    if (response.status == 401 || response.status == 403) {
        message += "\nSuggestion: Set XAI_API_KEY or GROK_API_KEY and pass --provider xai.";
    }
    return {code, message};
}

Error cancelled_error(const std::string& what) {
    return {ErrorCode::Cancelled,
            "video generation cancelled" + (what.empty() ? std::string() : ": " + what)};
}

Error interruptible_sleep(int milliseconds, runtime::CancellationToken cancellation) {
    int remaining = milliseconds;
    while (remaining > 0) {
        if (cancellation.cancelled()) return cancelled_error("wait");
        const int slice = remaining > kPollSliceMs ? kPollSliceMs : remaining;
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        remaining -= slice;
    }
    return ok_error();
}

}  // namespace

Error build_xai_imagine_video_input(const VideoGenerateRequest& request, json::Value& input) {
    input = json_object_value();
    input.object["model"] = json_string_value(wire_model(request));
    if (!request.prompt.empty()) {
        input.object[request.capability.prompt_field.empty() ? "prompt"
                                                             : request.capability.prompt_field] =
            json_string_value(request.prompt);
    }
    for (const auto& setting : request.settings) {
        input.object[setting.first] = coerce_number(setting.second);
    }
    std::vector<const VideoInput*> images;
    for (const VideoInput& media : request.inputs) {
        if (media.mime_type.rfind("image/", 0) == 0) images.push_back(&media);
        else if (media.mime_type.rfind("video/", 0) == 0 || media.mime_type.rfind("audio/", 0) == 0) {
            return {ErrorCode::BadArgs, "xAI Imagine video accepts image references only"};
        }
    }
    if (request.capability.input_mode == VideoInputMode::Text && !images.empty()) {
        return {ErrorCode::BadArgs, "text-to-video models do not accept --attach"};
    }
    if (request.capability.input_mode == VideoInputMode::Image) {
        if (images.empty()) {
            return {ErrorCode::BadArgs, "this image-to-video model requires a start image"};
        }
        std::string uri;
        Error err = media_data_uri(*images.front(), uri);
        if (!err.ok()) return err;
        input.object["image"] = image_url_object(uri);
    } else if (request.capability.input_mode == VideoInputMode::Reference) {
        if (images.empty()) {
            return {ErrorCode::BadArgs, "reference-to-video requires an image reference"};
        }
        json::Value refs = json_array_value();
        for (const VideoInput* image : images) {
            std::string uri;
            Error err = media_data_uri(*image, uri);
            if (!err.ok()) return err;
            refs.array.push_back(image_url_object(uri));
        }
        input.object["reference_images"] = std::move(refs);
    }
    return ok_error();
}

Error parse_xai_imagine_video_status(const std::string& body, std::string& status,
                                     std::string& request_id, std::string& output_url,
                                     std::string& error_text) {
    status.clear();
    request_id.clear();
    output_url.clear();
    error_text.clear();
    const json::ParseResult parsed = json::parse(body);
    if (!parsed.error.ok()) {
        return {ErrorCode::JsonParse, "xAI Imagine video status is not valid JSON: " + parsed.error.message};
    }
    status = json_string_field(parsed.value.get("status"));
    request_id = json_string_field(parsed.value.get("request_id"));
    if (const json::Value* video = parsed.value.get("video")) {
        output_url = json_string_field(video->get("url"));
        if (video->get("respect_moderation") &&
            video->get("respect_moderation")->type == json::Value::Type::Bool &&
            !video->get("respect_moderation")->boolean && output_url.empty()) {
            error_text = "xAI Imagine blocked the video by moderation";
        }
    }
    if (const json::Value* error = parsed.value.get("error")) {
        if (error->is_string()) error_text = error->string;
        else if (error->is_object()) error_text = json_string_field(error->get("message"));
    }
    return ok_error();
}

Error generate_xai_imagine_video(const RequestContext& context,
                                 const VideoGenerateRequest& request,
                                 VideoGenerateResult& result,
                                 runtime::CancellationToken cancellation) {
    if (cancellation.cancelled()) return cancelled_error("before request");
    json::Value body_value;
    Error err = build_xai_imagine_video_input(request, body_value);
    if (!err.ok()) return err;
    const std::string url = join_url(context.base_url, "/videos/generations");
    http::Request req = make_xai_http(context, "POST", url, cancellation);
    req.headers.emplace_back("Content-Type: application/json");
    req.body = json::stringify(body_value);
    req.retry_post_on_send_error = false;
    const auto started = std::chrono::steady_clock::now();
    auto elapsed = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - started)
            .count();
    };
    const http::Result created = http::perform(req, {context.api_key});
    if (!created.error.ok()) {
        result.total_ms = elapsed();
        return created.error;
    }
    if (created.response.status < 200 || created.response.status >= 300) {
        result.total_ms = elapsed();
        return xai_video_http_error(created.response, url);
    }
    std::string status, request_id, output_url, error_text;
    err = parse_xai_imagine_video_status(
        created.response.body, status, request_id, output_url, error_text);
    if (!err.ok()) {
        result.total_ms = elapsed();
        return err;
    }
    if (request_id.empty()) {
        result.total_ms = elapsed();
        return {ErrorCode::ProviderSchema, "xAI Imagine video did not return a request_id"};
    }
    const std::string poll_url = join_url(context.base_url, "/videos/" + request_id);
    while (ascii_lower(status) != "done" && ascii_lower(status) != "failed" &&
           ascii_lower(status) != "expired" && output_url.empty()) {
        if (cancellation.cancelled()) {
            result.total_ms = elapsed();
            return cancelled_error("while waiting for xAI Imagine");
        }
        Error wait_error = interruptible_sleep(kPollIntervalMs, cancellation);
        if (!wait_error.ok()) {
            result.total_ms = elapsed();
            return wait_error;
        }
        http::Request poll = make_xai_http(context, "GET", poll_url, cancellation);
        if (poll.timeout_seconds <= 0) poll.timeout_seconds = 30;
        const http::Result polled = http::perform(poll, {context.api_key});
        if (!polled.error.ok()) {
            result.total_ms = elapsed();
            return polled.error;
        }
        if (polled.response.status < 200 || polled.response.status >= 300) {
            result.total_ms = elapsed();
            return xai_video_http_error(polled.response, poll_url);
        }
        err = parse_xai_imagine_video_status(
            polled.response.body, status, request_id, output_url, error_text);
        if (!err.ok()) {
            result.total_ms = elapsed();
            return err;
        }
    }
    result.total_ms = elapsed();
    const std::string lower = ascii_lower(status);
    if (lower == "expired") {
        return {ErrorCode::ProviderSchema, "xAI Imagine video request expired"};
    }
    if (lower == "failed") {
        std::string message = "xAI Imagine video generation failed";
        if (!error_text.empty()) message += ": " + error_text;
        return {ErrorCode::ProviderSchema, message};
    }
    if (output_url.empty()) {
        std::string message = "xAI Imagine video completed without an output URL";
        if (!error_text.empty()) message += ": " + error_text;
        return {ErrorCode::ProviderSchema, message};
    }
    return download_generated_video(context, output_url, request, result, cancellation, true);
}

}  // namespace ainiux::provider
