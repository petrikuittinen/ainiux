#include "provider/image_fal.hpp"

#include <cctype>
#include <chrono>
#include <sstream>
#include <thread>
#include <utility>

#include "config/image_catalog.hpp"
#include "json/json.hpp"

namespace ainiux::provider {
namespace {

constexpr long kFalDownloadMaxBytes = 40L * 1024L * 1024L;
constexpr int kFalPollSliceMs = 100;
constexpr int kFalPollIntervalMs = 1000;

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

std::string join_url(const std::string& base, const std::string& suffix) {
    if (base.empty()) return suffix;
    if (base.back() == '/' && !suffix.empty() && suffix.front() == '/') {
        return base.substr(0, base.size() - 1) + suffix;
    }
    if (base.back() != '/' && (suffix.empty() || suffix.front() != '/')) return base + "/" + suffix;
    return base + suffix;
}

bool valid_fal_model(const std::string& api_model) {
    if (api_model.empty() || api_model.front() == '/' || api_model.back() == '/') return false;
    bool have_slash = false;
    for (char ch : api_model) {
        if (ch == '/') {
            have_slash = true;
            continue;
        }
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (!(std::isalnum(uch) != 0 || ch == '_' || ch == '-' || ch == '.')) return false;
    }
    return have_slash;
}

bool has_authorization_header(const std::vector<std::string>& headers) {
    for (const std::string& header : headers) {
        const size_t colon = header.find(':');
        if (colon == std::string::npos || colon == 0) continue;
        if (ascii_lower(header.substr(0, colon)) == "authorization") return true;
    }
    return false;
}

http::Request make_fal_http(const RequestContext& context,
                            const std::string& method,
                            const std::string& url,
                            runtime::CancellationToken cancellation) {
    http::Request req;
    req.method = method;
    req.url = url;
    req.headers = context.headers;
    if (!context.api_key.empty() && !has_authorization_header(req.headers)) {
        req.headers.emplace_back("Authorization: Key " + context.api_key);
    }
    req.connect_timeout_seconds = context.options.connect_timeout_seconds;
    req.timeout_seconds = context.options.timeout_seconds;
    req.proxy = context.options.proxy;
    req.insecure_tls = context.options.insecure_tls;
    req.trace = context.options.trace_http;
    req.cancellation = cancellation;
    return req;
}

Error cancelled_error(const std::string& what) {
    return {ErrorCode::Cancelled,
            "image generation cancelled" + (what.empty() ? std::string() : ": " + what)};
}

Error interruptible_sleep(int milliseconds, runtime::CancellationToken cancellation) {
    int remaining = milliseconds;
    while (remaining > 0) {
        if (cancellation.cancelled()) return cancelled_error("wait");
        const int slice = remaining > kFalPollSliceMs ? kFalPollSliceMs : remaining;
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        remaining -= slice;
    }
    return ok_error();
}

std::string fal_error_detail(const json::Value& root) {
    std::string message = json_string_field(root.get("error"));
    if (message.empty() && root.get("detail") != nullptr && root.get("detail")->is_string()) {
        message = root.get("detail")->string;
    }
    const std::string type = json_string_field(root.get("error_type"));
    if (!type.empty()) {
        if (!message.empty()) message += " ";
        message += "(" + type + ")";
    }
    if (message.empty() && root.get("msg") != nullptr && root.get("msg")->is_string()) {
        message = root.get("msg")->string;
    }
    return compact_detail(message);
}

Error fal_http_status_error(const http::Response& response, const std::string& url) {
    ErrorCode code = ErrorCode::HttpStatus;
    if (response.status == 401 || response.status == 403) code = ErrorCode::Auth;
    else if (response.status == 429) code = ErrorCode::RateLimit;

    std::string summary = "Unexpected HTTP status from fal.";
    if (response.status == 400) {
        summary = "Bad request. fal rejected the image prompt, size, format, or attachments.";
    } else if (response.status == 401) {
        summary = "Authentication failed. Check FAL_API_KEY or FAL_KEY.";
    } else if (response.status == 403) {
        summary = "Access forbidden. The fal key may not have access to this model.";
    } else if (response.status == 404) {
        summary = "fal model or request was not found. Check -m/--model.";
    } else if (response.status == 429) {
        summary = "Too many requests. Try again later or use a different fal model.";
    } else if (response.status >= 500 && response.status < 600) {
        summary = "fal failed while generating the image. Try again later.";
    }

    std::string message = "HTTP " + std::to_string(response.status) + ": " + summary + "\nURL: " + url;
    const json::ParseResult parsed = json::parse(response.body);
    std::string detail;
    if (parsed.error.ok()) detail = fal_error_detail(parsed.value);
    if (detail.empty()) detail = compact_detail(response.body);
    if (!detail.empty()) message += "\nProvider message: " + detail;
    if (response.status == 401 || response.status == 403) {
        message += "\nSuggestion: Set FAL_API_KEY (or FAL_KEY) and pass --provider fal.";
    }
    return {code, message};
}

void best_effort_cancel(const RequestContext& context,
                        const std::string& cancel_url,
                        runtime::CancellationToken cancellation) {
    if (cancel_url.empty()) return;
    http::Request req = make_fal_http(context, "PUT", cancel_url, cancellation);
    req.timeout_seconds = 10;
    (void)http::perform(req, {context.api_key});
}

Error download_output(const RequestContext& context,
                      const std::string& url,
                      ImageGenerateResult& result,
                      runtime::CancellationToken cancellation) {
    http::Request req = make_fal_http(context, "GET", url, cancellation);
    req.follow_redirects = true;
    req.max_body_bytes = kFalDownloadMaxBytes;
    if (req.timeout_seconds <= 0) req.timeout_seconds = 120;
    const http::Result http_result = http::perform(req, {context.api_key});
    if (!http_result.error.ok()) return http_result.error;
    if (http_result.response.status < 200 || http_result.response.status >= 300) {
        return fal_http_status_error(http_result.response, url);
    }
    if (http_result.response.body.empty()) {
        return {ErrorCode::ProviderSchema, "fal output file was empty: " + url};
    }
    result.bytes = http_result.response.body;
    return ok_error();
}

}  // namespace

std::string fal_queue_url(const RequestContext& context, const std::string& api_model) {
    return join_url(context.base_url, api_model);
}

bool fal_status_completed(const std::string& status) {
    return ascii_lower(status) == "completed";
}

bool fal_status_failed(const std::string& status) {
    const std::string lower = ascii_lower(status);
    return lower == "failed" || lower == "error";
}

Error serialize_fal_request(const ImageGenerateRequest& request, std::string& body) {
    body.clear();
    json::Value input;
    Error err = build_catalog_image_input(request, input);
    if (!err.ok()) return err;
    body = json::stringify(input);
    return ok_error();
}

Error parse_fal_queue_submit(const std::string& body,
                             std::string& request_id,
                             std::string& status_url,
                             std::string& response_url,
                             std::string& cancel_url) {
    request_id.clear();
    status_url.clear();
    response_url.clear();
    cancel_url.clear();
    const json::ParseResult parsed = json::parse(body);
    if (!parsed.error.ok()) {
        return {ErrorCode::JsonParse, "fal queue submit is not valid JSON: " + parsed.error.message};
    }
    request_id = json_string_field(parsed.value.get("request_id"));
    status_url = json_string_field(parsed.value.get("status_url"));
    response_url = json_string_field(parsed.value.get("response_url"));
    cancel_url = json_string_field(parsed.value.get("cancel_url"));
    if (request_id.empty() && status_url.empty()) {
        return {ErrorCode::ProviderSchema, "fal queue submit did not include request_id"};
    }
    return ok_error();
}

Error parse_fal_queue_status(const std::string& body, std::string& status, std::string& error_text) {
    status.clear();
    error_text.clear();
    const json::ParseResult parsed = json::parse(body);
    if (!parsed.error.ok()) {
        return {ErrorCode::JsonParse, "fal queue status is not valid JSON: " + parsed.error.message};
    }
    status = json_string_field(parsed.value.get("status"));
    error_text = fal_error_detail(parsed.value);
    return ok_error();
}

Error parse_fal_queue_result(const std::string& body, std::string& output_url) {
    output_url.clear();
    const json::ParseResult parsed = json::parse(body);
    if (!parsed.error.ok()) {
        return {ErrorCode::JsonParse, "fal image result is not valid JSON: " + parsed.error.message};
    }
    const std::string detail = fal_error_detail(parsed.value);
    if (!extract_first_http_url(parsed.value, output_url)) {
        std::string message = "fal image result did not contain an output URL";
        if (!detail.empty()) message += ": " + detail;
        return {ErrorCode::ProviderSchema, message};
    }
    return ok_error();
}

Error generate_fal_image(const RequestContext& context,
                         const ImageGenerateRequest& request,
                         ImageGenerateResult& result,
                         runtime::CancellationToken cancellation) {
    if (cancellation.cancelled()) return cancelled_error("before request");
    const std::string api_model =
        request.capability.api_model.empty() ? request.model : request.capability.api_model;
    if (!valid_fal_model(api_model)) {
        return {ErrorCode::BadArgs,
                "fal image models require api_model as an endpoint id (for example fal-ai/flux/schnell)"};
    }
    if (request.prompt.empty()) {
        return {ErrorCode::BadArgs, "image generation requires a prompt"};
    }

    std::string body;
    Error serialize_error = serialize_fal_request(request, body);
    if (!serialize_error.ok()) return serialize_error;

    const std::string url = fal_queue_url(context, api_model);
    http::Request req = make_fal_http(context, "POST", url, cancellation);
    req.headers.emplace_back("Content-Type: application/json");
    req.body = std::move(body);

    const auto started = std::chrono::steady_clock::now();
    const http::Result created = http::perform(req, {context.api_key});
    auto elapsed = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - started)
            .count();
    };
    if (!created.error.ok()) {
        result.total_ms = elapsed();
        return created.error;
    }
    if (created.response.status < 200 || created.response.status >= 300) {
        result.total_ms = elapsed();
        return fal_http_status_error(created.response, url);
    }

    std::string request_id;
    std::string status_url;
    std::string response_url;
    std::string cancel_url;
    Error parse_error =
        parse_fal_queue_submit(created.response.body, request_id, status_url, response_url, cancel_url);
    if (!parse_error.ok()) {
        result.total_ms = elapsed();
        return parse_error;
    }
    if (status_url.empty() && !request_id.empty()) {
        status_url = join_url(url, "requests/" + request_id + "/status");
    }
    if (response_url.empty() && !request_id.empty()) {
        response_url = join_url(url, "requests/" + request_id);
    }
    if (cancel_url.empty() && !request_id.empty()) {
        cancel_url = join_url(url, "requests/" + request_id + "/cancel");
    }

    std::string status;
    std::string error_text;
    while (true) {
        if (cancellation.cancelled()) {
            best_effort_cancel(context, cancel_url, cancellation);
            result.total_ms = elapsed();
            return cancelled_error("while waiting for fal");
        }
        if (status_url.empty()) {
            result.total_ms = elapsed();
            return {ErrorCode::ProviderSchema, "fal queue submit did not include status_url"};
        }
        http::Request poll = make_fal_http(context, "GET", status_url, cancellation);
        if (poll.timeout_seconds <= 0) poll.timeout_seconds = 30;
        const http::Result polled = http::perform(poll, {context.api_key});
        if (!polled.error.ok()) {
            result.total_ms = elapsed();
            return polled.error;
        }
        if (polled.response.status < 200 || polled.response.status >= 300) {
            result.total_ms = elapsed();
            return fal_http_status_error(polled.response, status_url);
        }
        parse_error = parse_fal_queue_status(polled.response.body, status, error_text);
        if (!parse_error.ok()) {
            result.total_ms = elapsed();
            return parse_error;
        }
        if (fal_status_completed(status) || fal_status_failed(status)) break;
        Error wait_error = interruptible_sleep(kFalPollIntervalMs, cancellation);
        if (!wait_error.ok()) {
            best_effort_cancel(context, cancel_url, cancellation);
            result.total_ms = elapsed();
            return wait_error;
        }
    }

    if (fal_status_failed(status) || !error_text.empty()) {
        result.total_ms = elapsed();
        std::string message = "fal image generation failed";
        if (!error_text.empty()) message += ": " + error_text;
        return {ErrorCode::ProviderSchema, message};
    }
    if (response_url.empty()) {
        result.total_ms = elapsed();
        return {ErrorCode::ProviderSchema, "fal completed without a response_url"};
    }

    http::Request get_result = make_fal_http(context, "GET", response_url, cancellation);
    if (get_result.timeout_seconds <= 0) get_result.timeout_seconds = 60;
    const http::Result payload = http::perform(get_result, {context.api_key});
    if (!payload.error.ok()) {
        result.total_ms = elapsed();
        return payload.error;
    }
    if (payload.response.status < 200 || payload.response.status >= 300) {
        result.total_ms = elapsed();
        return fal_http_status_error(payload.response, response_url);
    }

    std::string output_url;
    parse_error = parse_fal_queue_result(payload.response.body, output_url);
    result.total_ms = elapsed();
    if (!parse_error.ok()) return parse_error;
    return download_output(context, output_url, result, cancellation);
}

}  // namespace ainiux::provider
