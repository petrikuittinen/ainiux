#include "provider/image_replicate.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <sstream>
#include <thread>
#include <utility>

#include "config/image_catalog.hpp"
#include "json/json.hpp"

namespace ainiux::provider {
namespace {

constexpr long kReplicateWaitSeconds = 60;
constexpr long kReplicateDownloadMaxBytes = 40L * 1024L * 1024L;
constexpr int kReplicatePollSliceMs = 100;
constexpr int kReplicatePollIntervalMs = 1000;

json::Value json_string(const std::string& text) {
    json::Value value;
    value.type = json::Value::Type::String;
    value.string = text;
    return value;
}

json::Value json_number(double number) {
    json::Value value;
    value.type = json::Value::Type::Number;
    value.number = number;
    return value;
}

json::Value json_array() {
    json::Value value;
    value.type = json::Value::Type::Array;
    return value;
}

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

bool looks_like_url(const std::string& text) {
    const std::string lower = ascii_lower(text);
    return lower.rfind("https://", 0) == 0 || lower.rfind("http://", 0) == 0;
}

bool extract_first_url(const json::Value& value, std::string& url, int depth) {
    if (depth > 6) return false;
    if (value.is_string()) {
        if (looks_like_url(value.string)) {
            url = value.string;
            return true;
        }
        return false;
    }
    if (value.is_array()) {
        for (const json::Value& item : value.array) {
            if (extract_first_url(item, url, depth + 1)) return true;
        }
        return false;
    }
    if (value.is_object()) {
        if (const json::Value* direct = value.get("url")) {
            if (extract_first_url(*direct, url, depth + 1)) return true;
        }
        if (const json::Value* href = value.get("href")) {
            if (extract_first_url(*href, url, depth + 1)) return true;
        }
        for (const auto& item : value.object) {
            if (extract_first_url(item.second, url, depth + 1)) return true;
        }
    }
    return false;
}

bool parse_wh_size(const std::string& text, int& width, int& height) {
    const std::string lower = ascii_lower(ascii_trim(text));
    const size_t sep = lower.find('x');
    if (sep == std::string::npos || sep == 0 || sep + 1 >= lower.size()) return false;
    try {
        const long w = std::stol(lower.substr(0, sep));
        const long h = std::stol(lower.substr(sep + 1));
        if (w <= 0 || h <= 0) return false;
        width = static_cast<int>(w);
        height = static_cast<int>(h);
        return true;
    } catch (...) {
        return false;
    }
}

bool list_has_ci(const std::vector<std::string>& values, const std::string& token) {
    const std::string lower = ascii_lower(token);
    for (const std::string& value : values) {
        if (ascii_lower(value) == lower) return true;
    }
    return false;
}

std::string api_output_format(const ImageCapability& capability, const std::string& format) {
    if (format.empty()) return {};
    if (format == "jpeg" && list_has_ci(capability.format, "jpg") &&
        !list_has_ci(capability.format, "jpeg")) {
        return "jpg";
    }
    return format;
}

bool valid_replicate_model(const std::string& api_model) {
    const size_t slash = api_model.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= api_model.size()) return false;
    if (api_model.find('/', slash + 1) != std::string::npos) return false;
    for (char ch : api_model) {
        if (ch == '/') continue;
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (!(std::isalnum(uch) != 0 || ch == '_' || ch == '-' || ch == '.')) return false;
    }
    return true;
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

http::Request make_replicate_http(const RequestContext& context,
                                  const std::string& method,
                                  const std::string& url,
                                  runtime::CancellationToken cancellation) {
    http::Request req;
    req.method = method;
    req.url = url;
    req.headers = context.headers;
    if (!context.api_key.empty() && !has_authorization_header(req.headers)) {
        req.headers.emplace_back("Authorization: Bearer " + context.api_key);
    }
    req.connect_timeout_seconds = context.options.connect_timeout_seconds;
    req.timeout_seconds = context.options.timeout_seconds;
    req.proxy = context.options.proxy;
    req.insecure_tls = context.options.insecure_tls;
    req.trace = context.options.trace_http;
    req.cancellation = cancellation;
    return req;
}

std::string prediction_error_detail(const json::Value& root) {
    std::string message;
    const json::Value* err = root.get("error");
    if (err != nullptr) {
        if (err->is_string()) message = err->string;
        else if (err->is_object()) {
            if (const json::Value* detail = err->get("detail")) {
                if (detail->is_string()) message = detail->string;
            }
            if (message.empty() && err->get("message") != nullptr && err->get("message")->is_string()) {
                message = err->get("message")->string;
            }
        }
    }
    const std::string logs = json_string_field(root.get("logs"));
    if (!logs.empty()) {
        if (!message.empty()) message += " ";
        message += logs;
    }
    return compact_detail(message);
}

Error replicate_http_status_error(const http::Response& response, const std::string& url) {
    ErrorCode code = ErrorCode::HttpStatus;
    if (response.status == 401 || response.status == 403) code = ErrorCode::Auth;
    else if (response.status == 429) code = ErrorCode::RateLimit;

    std::string summary = "Unexpected HTTP status from Replicate.";
    if (response.status == 400) {
        summary = "Bad request. Replicate rejected the image prompt, size, format, or attachments.";
    } else if (response.status == 401) {
        summary = "Authentication failed. Check REPLICATE_API_KEY or REPLICATE_API_TOKEN.";
    } else if (response.status == 403) {
        summary = "Access forbidden. The Replicate token may not have access to this model.";
    } else if (response.status == 404) {
        summary = "Replicate model or prediction was not found. Check -m/--model.";
    } else if (response.status == 429) {
        summary = "Too many requests. Try again later or use a different Replicate model.";
    } else if (response.status >= 500 && response.status < 600) {
        summary = "Replicate failed while generating the image. Try again later.";
    }

    std::string message = "HTTP " + std::to_string(response.status) + ": " + summary + "\nURL: " + url;
    const json::ParseResult parsed = json::parse(response.body);
    std::string detail;
    if (parsed.error.ok()) {
        if (parsed.value.get("detail") != nullptr && parsed.value.get("detail")->is_string()) {
            detail = parsed.value.get("detail")->string;
        }
        if (detail.empty()) detail = prediction_error_detail(parsed.value);
        if (detail.empty() && parsed.value.get("title") != nullptr &&
            parsed.value.get("title")->is_string()) {
            detail = parsed.value.get("title")->string;
        }
    }
    if (detail.empty()) detail = compact_detail(response.body);
    if (!detail.empty()) message += "\nProvider message: " + detail;
    if (response.status == 401 || response.status == 403) {
        message +=
            "\nSuggestion: Set REPLICATE_API_KEY (or REPLICATE_API_TOKEN) and pass --provider replicate.";
    }
    return {code, message};
}

Error cancelled_error(const std::string& what) {
    return {ErrorCode::Cancelled, "image generation cancelled" + (what.empty() ? std::string() : ": " + what)};
}

Error interruptible_sleep(int milliseconds, runtime::CancellationToken cancellation) {
    int remaining = milliseconds;
    while (remaining > 0) {
        if (cancellation.cancelled()) return cancelled_error("wait");
        const int slice = remaining > kReplicatePollSliceMs ? kReplicatePollSliceMs : remaining;
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        remaining -= slice;
    }
    return ok_error();
}

void best_effort_cancel(const RequestContext& context,
                        const std::string& cancel_url,
                        runtime::CancellationToken cancellation) {
    if (cancel_url.empty()) return;
    http::Request req = make_replicate_http(context, "POST", cancel_url, cancellation);
    req.timeout_seconds = 10;
    (void)http::perform(req, {context.api_key});
}

Error download_output(const RequestContext& context,
                      const std::string& url,
                      ImageGenerateResult& result,
                      runtime::CancellationToken cancellation) {
    http::Request req = make_replicate_http(context, "GET", url, cancellation);
    req.follow_redirects = true;
    req.max_body_bytes = kReplicateDownloadMaxBytes;
    if (req.timeout_seconds <= 0) req.timeout_seconds = 120;
    const http::Result http_result = http::perform(req, {context.api_key});
    if (!http_result.error.ok()) return http_result.error;
    if (http_result.response.status < 200 || http_result.response.status >= 300) {
        return replicate_http_status_error(http_result.response, url);
    }
    if (http_result.response.body.empty()) {
        return {ErrorCode::ProviderSchema, "Replicate output file was empty: " + url};
    }
    result.bytes = http_result.response.body;
    return ok_error();
}

}  // namespace

std::string replicate_prediction_url(const RequestContext& context, const std::string& api_model) {
    return join_url(context.base_url, "/models/" + api_model + "/predictions");
}

bool replicate_status_succeeded(const std::string& status) {
    const std::string lower = ascii_lower(status);
    return lower == "succeeded" || lower == "successful";
}

bool replicate_status_failed(const std::string& status) {
    return ascii_lower(status) == "failed";
}

bool replicate_status_canceled(const std::string& status) {
    const std::string lower = ascii_lower(status);
    return lower == "canceled" || lower == "cancelled";
}

bool replicate_status_terminal(const std::string& status) {
    return replicate_status_succeeded(status) || replicate_status_failed(status) ||
           replicate_status_canceled(status);
}

Error serialize_replicate_request(const ImageGenerateRequest& request, std::string& body) {
    body.clear();
    const ImageCapability& capability = request.capability;
    json::Value input;
    input.type = json::Value::Type::Object;
    if (!capability.defaults_json.empty()) {
        const json::ParseResult parsed = json::parse(capability.defaults_json);
        if (!parsed.error.ok() || !parsed.value.is_object()) {
            return {ErrorCode::Config, "image model " + capability.id +
                                           " has invalid defaults_json; expected a JSON object"};
        }
        input = parsed.value;
    }

    const std::string prompt_field =
        capability.prompt_field.empty() ? std::string("prompt") : capability.prompt_field;
    input.object[prompt_field] = json_string(request.prompt);

    int width = 0;
    int height = 0;
    const bool pixel_size = parse_wh_size(request.size, width, height);
    if (pixel_size && !capability.width_field.empty() && !capability.height_field.empty()) {
        input.object[capability.width_field] = json_number(width);
        input.object[capability.height_field] = json_number(height);
        if (!capability.aspect_field.empty() && request.aspect.empty()) {
            input.object[capability.aspect_field] = json_string("custom");
        }
    } else if (!request.size.empty() && !capability.size_field.empty() && !pixel_size) {
        input.object[capability.size_field] = json_string(request.size);
    }

    if (!request.aspect.empty() && !capability.aspect_field.empty()) {
        input.object[capability.aspect_field] = json_string(request.aspect);
    }

    const std::string format = api_output_format(capability, request.output_format);
    if (!format.empty() && !capability.format_field.empty()) {
        input.object[capability.format_field] = json_string(format);
    }
    if (!request.quality.empty() && !capability.quality_field.empty()) {
        input.object[capability.quality_field] = json_string(request.quality);
    }

    if (!request.images.empty()) {
        if (capability.images_field.empty()) {
            return {ErrorCode::BadArgs, "image model " + capability.id +
                                            " does not support --attach reference images"};
        }
        json::Value images = json_array();
        images.array.reserve(request.images.size());
        for (const ImageInput& image : request.images) {
            images.array.push_back(
                json_string("data:" + image.mime_type + ";base64," + image.base64_data));
        }
        input.object[capability.images_field] = std::move(images);
    }

    json::Value root;
    root.type = json::Value::Type::Object;
    root.object["input"] = std::move(input);
    body = json::stringify(root);
    return ok_error();
}

Error parse_replicate_prediction(const std::string& body,
                                 std::string& status,
                                 std::string& output_url,
                                 std::string& poll_url,
                                 std::string& cancel_url,
                                 std::string& error_text) {
    status.clear();
    output_url.clear();
    poll_url.clear();
    cancel_url.clear();
    error_text.clear();

    const json::ParseResult parsed = json::parse(body);
    if (!parsed.error.ok()) {
        return {ErrorCode::JsonParse, "Replicate prediction is not valid JSON: " + parsed.error.message};
    }
    status = json_string_field(parsed.value.get("status"));
    error_text = prediction_error_detail(parsed.value);
    if (const json::Value* urls = parsed.value.get("urls")) {
        poll_url = json_string_field(urls->get("get"));
        cancel_url = json_string_field(urls->get("cancel"));
    }
    if (poll_url.empty()) {
        const std::string id = json_string_field(parsed.value.get("id"));
        if (!id.empty()) poll_url = "https://api.replicate.com/v1/predictions/" + id;
    }
    if (const json::Value* output = parsed.value.get("output")) {
        (void)extract_first_url(*output, output_url, 0);
    }
    return ok_error();
}

Error generate_replicate_image(const RequestContext& context,
                               const ImageGenerateRequest& request,
                               ImageGenerateResult& result,
                               runtime::CancellationToken cancellation) {
    if (cancellation.cancelled()) return cancelled_error("before request");
    const std::string api_model =
        request.capability.api_model.empty() ? request.model : request.capability.api_model;
    if (!valid_replicate_model(api_model)) {
        return {ErrorCode::BadArgs,
                "Replicate image models require api_model owner/name (for example prunaai/z-image-turbo)"};
    }
    if (request.prompt.empty()) {
        return {ErrorCode::BadArgs, "image generation requires a prompt"};
    }

    std::string body;
    Error serialize_error = serialize_replicate_request(request, body);
    if (!serialize_error.ok()) return serialize_error;

    const std::string url = replicate_prediction_url(context, api_model);
    http::Request req = make_replicate_http(context, "POST", url, cancellation);
    req.headers.emplace_back("Content-Type: application/json");
    req.headers.emplace_back("Prefer: wait=" + std::to_string(kReplicateWaitSeconds));
    req.body = std::move(body);

    const auto started = std::chrono::steady_clock::now();
    const http::Result created = http::perform(req, {context.api_key});
    if (!created.error.ok()) {
        result.total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - started)
                              .count();
        return created.error;
    }
    if (created.response.status < 200 || created.response.status >= 300) {
        result.total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - started)
                              .count();
        return replicate_http_status_error(created.response, url);
    }

    std::string status;
    std::string output_url;
    std::string poll_url;
    std::string cancel_url;
    std::string error_text;
    Error parse_error = parse_replicate_prediction(
        created.response.body, status, output_url, poll_url, cancel_url, error_text);
    if (!parse_error.ok()) {
        result.total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - started)
                              .count();
        return parse_error;
    }

    while (!replicate_status_terminal(status) && output_url.empty()) {
        if (cancellation.cancelled()) {
            best_effort_cancel(context, cancel_url, cancellation);
            result.total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - started)
                                  .count();
            return cancelled_error("while waiting for Replicate");
        }
        if (poll_url.empty()) {
            result.total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - started)
                                  .count();
            return {ErrorCode::ProviderSchema,
                    "Replicate prediction did not include urls.get for polling"};
        }
        Error wait_error = interruptible_sleep(kReplicatePollIntervalMs, cancellation);
        if (!wait_error.ok()) {
            best_effort_cancel(context, cancel_url, cancellation);
            result.total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - started)
                                  .count();
            return wait_error;
        }
        http::Request poll = make_replicate_http(context, "GET", poll_url, cancellation);
        if (poll.timeout_seconds <= 0) poll.timeout_seconds = 30;
        const http::Result polled = http::perform(poll, {context.api_key});
        if (!polled.error.ok()) {
            result.total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - started)
                                  .count();
            return polled.error;
        }
        if (polled.response.status < 200 || polled.response.status >= 300) {
            result.total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - started)
                                  .count();
            return replicate_http_status_error(polled.response, poll_url);
        }
        parse_error = parse_replicate_prediction(
            polled.response.body, status, output_url, poll_url, cancel_url, error_text);
        if (!parse_error.ok()) {
            result.total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - started)
                                  .count();
            return parse_error;
        }
    }

    result.total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - started)
                          .count();

    if (replicate_status_canceled(status) || cancellation.cancelled()) {
        return cancelled_error("by Replicate or the user");
    }
    if (replicate_status_failed(status)) {
        std::string message = "Replicate prediction failed";
        if (!error_text.empty()) message += ": " + error_text;
        return {ErrorCode::ProviderSchema, message};
    }
    if (output_url.empty()) {
        std::string message = "Replicate prediction completed without an output image URL";
        if (!error_text.empty()) message += ": " + error_text;
        return {ErrorCode::ProviderSchema, message};
    }
    return download_output(context, output_url, result, cancellation);
}

}  // namespace ainiux::provider
