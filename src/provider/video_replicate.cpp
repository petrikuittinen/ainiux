#include "provider/video.hpp"

#include <cctype>
#include <chrono>
#include <thread>

#include "platform/filesystem.hpp"
#include "provider/image.hpp"

namespace ainiux::provider {
namespace {

constexpr long kReplicateWaitSeconds = 60;
constexpr int kReplicatePollSliceMs = 100;
constexpr int kReplicatePollIntervalMs = 1000;

std::string compact_detail(std::string text) {
    text = ascii_trim(std::move(text));
    for (char& ch : text) {
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
    }
    if (text.size() > 500) text = text.substr(0, 500) + "...";
    return text;
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

bool has_authorization_header(const std::vector<std::string>& headers) {
    for (const std::string& header : headers) {
        const size_t colon = header.find(':');
        if (colon == std::string::npos || colon == 0) continue;
        if (ascii_lower(header.substr(0, colon)) == "authorization") return true;
    }
    return false;
}

std::string join_url(const std::string& base, const std::string& suffix) {
    if (base.empty()) return suffix;
    if (base.back() == '/') return base.substr(0, base.size() - 1) + suffix;
    return base + suffix;
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
    req.timeout_seconds = context.options.timeout_seconds > 0 ? context.options.timeout_seconds : 300;
    req.proxy = context.options.proxy;
    req.insecure_tls = context.options.insecure_tls;
    req.trace = context.options.trace_http;
    req.cancellation = cancellation;
    return req;
}

std::string json_string_field(const json::Value* value) {
    if (value != nullptr && value->is_string()) return value->string;
    return {};
}

Error replicate_video_http_error(const http::Response& response, const std::string& url) {
    ErrorCode code = ErrorCode::HttpStatus;
    if (response.status == 401 || response.status == 403) code = ErrorCode::Auth;
    else if (response.status == 429) code = ErrorCode::RateLimit;

    std::string summary = "Unexpected HTTP status from Replicate.";
    if (response.status == 400 || response.status == 422) {
        summary = "Bad request. Replicate rejected the video prompt, settings, or attachments.";
    } else if (response.status == 401) {
        summary = "Authentication failed. Check REPLICATE_API_KEY or REPLICATE_API_TOKEN.";
    } else if (response.status == 403) {
        summary = "Access forbidden. The Replicate token may not have access to this model.";
    } else if (response.status == 404) {
        summary = "Replicate model or prediction was not found. Check -m/--model.";
    } else if (response.status == 429) {
        summary = "Too many requests. Try again later or use a different Replicate model.";
    } else if (response.status >= 500 && response.status < 600) {
        summary = "Replicate failed while generating the video. Try again later.";
    }

    std::string message = "HTTP " + std::to_string(response.status) + ": " + summary + "\nURL: " + url;
    const json::ParseResult parsed = json::parse(response.body);
    std::string detail;
    if (parsed.error.ok()) {
        if (parsed.value.get("detail") != nullptr && parsed.value.get("detail")->is_string()) {
            detail = parsed.value.get("detail")->string;
        }
        if (detail.empty() && parsed.value.get("error") != nullptr) {
            const json::Value* err = parsed.value.get("error");
            if (err->is_string()) detail = err->string;
            else if (err->is_object() && err->get("message") != nullptr && err->get("message")->is_string()) {
                detail = err->get("message")->string;
            }
        }
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
    return {ErrorCode::Cancelled,
            "video generation cancelled" + (what.empty() ? std::string() : ": " + what)};
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

void best_effort_cancel(const RequestContext& context, const std::string& cancel_url,
                        runtime::CancellationToken cancellation) {
    if (cancel_url.empty()) return;
    http::Request req = make_replicate_http(context, "POST", cancel_url, cancellation);
    req.timeout_seconds = 10;
    (void)http::perform(req, {context.api_key});
}

std::string file_extension(const VideoInput& media) {
    std::string extension = "bin";
    const std::size_t slash = media.mime_type.find('/');
    if (slash != std::string::npos) extension = media.mime_type.substr(slash + 1);
    if (extension == "quicktime") return "mov";
    if (extension == "mpeg") return "mp3";
    if (extension == "x-wav") return "wav";
    return extension;
}

Error upload_replicate_file(const RequestContext& context, VideoInput& media,
                            runtime::CancellationToken cancellation) {
    if (!media.remote_url.empty()) return ok_error();
    if (!media.bytes) return {ErrorCode::Internal, "video input data is unavailable"};
    std::string random;
    Error err = platform::secure_random_hex(8, random);
    if (!err.ok()) return err;
    const std::string boundary = "ainiux" + random;
    const std::string filename = "ainiux-" + random + "." + file_extension(media);
    std::string body;
    body.reserve(media.bytes->size() + 256);
    body += "--" + boundary + "\r\n";
    body += "Content-Disposition: form-data; name=\"content\"; filename=\"" + filename + "\"\r\n";
    body += "Content-Type: " + media.mime_type + "\r\n\r\n";
    body += *media.bytes;
    body += "\r\n--" + boundary + "--\r\n";

    http::Request req = make_replicate_http(context, "POST", join_url(context.base_url, "/files"),
                                            cancellation);
    req.headers.push_back("Content-Type: multipart/form-data; boundary=" + boundary);
    req.body = std::move(body);
    req.retry_post_on_send_error = false;
    if (req.timeout_seconds <= 0) req.timeout_seconds = 300;
    const http::Result uploaded = http::perform(req, {context.api_key});
    if (!uploaded.error.ok()) return uploaded.error;
    if (uploaded.response.status < 200 || uploaded.response.status >= 300) {
        return replicate_video_http_error(uploaded.response, req.url);
    }
    const json::ParseResult parsed = json::parse(uploaded.response.body);
    if (!parsed.error.ok() || !parsed.value.is_object()) {
        return {ErrorCode::JsonParse, "Replicate file upload is not valid JSON"};
    }
    std::string url;
    if (const json::Value* urls = parsed.value.get("urls")) {
        url = json_string_field(urls->get("get"));
    }
    if (url.empty()) url = json_string_field(parsed.value.get("url"));
    if (url.rfind("https://", 0) != 0) {
        return {ErrorCode::ProviderSchema, "Replicate file upload omitted a secure URL"};
    }
    media.remote_url = url;
    media.bytes.reset();
    return ok_error();
}

}  // namespace

Error generate_replicate_video(const RequestContext& context,
                               const VideoGenerateRequest& request,
                               VideoGenerateResult& result,
                               runtime::CancellationToken cancellation) {
    if (cancellation.cancelled()) return cancelled_error("before request");
    const std::string api_model =
        request.capability.api_model.empty() ? request.model : request.capability.api_model;
    if (!valid_replicate_model(api_model)) {
        return {ErrorCode::BadArgs,
                "Replicate video models require api_model owner/name (for example prunaai/p-video)"};
    }

    VideoGenerateRequest wire_request = request;
    for (VideoInput& media : wire_request.inputs) {
        Error upload_error = upload_replicate_file(context, media, cancellation);
        if (!upload_error.ok()) return upload_error;
    }

    json::Value body_value;
    Error err = build_replicate_video_input(wire_request, body_value);
    if (!err.ok()) return err;

    const std::string url = replicate_prediction_url(context, api_model);
    http::Request req = make_replicate_http(context, "POST", url, cancellation);
    req.headers.emplace_back("Content-Type: application/json");
    req.headers.emplace_back("Prefer: wait=" + std::to_string(kReplicateWaitSeconds));
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
        return replicate_video_http_error(created.response, url);
    }

    std::string status, output_url, poll_url, cancel_url, error_text;
    err = parse_replicate_prediction(
        created.response.body, status, output_url, poll_url, cancel_url, error_text);
    if (!err.ok()) {
        result.total_ms = elapsed();
        return err;
    }

    while (!replicate_status_terminal(status) && output_url.empty()) {
        if (cancellation.cancelled()) {
            best_effort_cancel(context, cancel_url, cancellation);
            result.total_ms = elapsed();
            return cancelled_error("while waiting for Replicate");
        }
        if (poll_url.empty()) {
            result.total_ms = elapsed();
            return {ErrorCode::ProviderSchema,
                    "Replicate prediction did not include urls.get for polling"};
        }
        Error wait_error = interruptible_sleep(kReplicatePollIntervalMs, cancellation);
        if (!wait_error.ok()) {
            best_effort_cancel(context, cancel_url, cancellation);
            result.total_ms = elapsed();
            return wait_error;
        }
        http::Request poll = make_replicate_http(context, "GET", poll_url, cancellation);
        if (poll.timeout_seconds <= 0) poll.timeout_seconds = 30;
        const http::Result polled = http::perform(poll, {context.api_key});
        if (!polled.error.ok()) {
            result.total_ms = elapsed();
            return polled.error;
        }
        if (polled.response.status < 200 || polled.response.status >= 300) {
            result.total_ms = elapsed();
            return replicate_video_http_error(polled.response, poll_url);
        }
        err = parse_replicate_prediction(
            polled.response.body, status, output_url, poll_url, cancel_url, error_text);
        if (!err.ok()) {
            result.total_ms = elapsed();
            return err;
        }
    }

    result.total_ms = elapsed();
    if (replicate_status_canceled(status) || cancellation.cancelled()) {
        return cancelled_error("by Replicate or the user");
    }
    if (replicate_status_failed(status)) {
        std::string message = "Replicate prediction failed";
        if (!error_text.empty()) message += ": " + error_text;
        return {ErrorCode::ProviderSchema, message};
    }
    if (output_url.empty()) {
        std::string message = "Replicate prediction completed without an output video URL";
        if (!error_text.empty()) message += ": " + error_text;
        return {ErrorCode::ProviderSchema, message};
    }
    return download_generated_video(context, output_url, request, result, cancellation, true);
}

}  // namespace ainiux::provider
