#include "provider/video.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "input/input.hpp"
#include "json/json.hpp"
#include "platform/filesystem.hpp"
#include "security/redact.hpp"

namespace ainiux::provider {
namespace {

constexpr int kPollSliceMs = 100;
constexpr int kPollIntervalMs = 2000;

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

bool ends_with_ignore_case(const std::string& text, const std::string& suffix) {
    if (text.size() < suffix.size()) return false;
    return ascii_lower(text.substr(text.size() - suffix.size())) == ascii_lower(suffix);
}

std::string gemini_native_base(const RequestContext& context) {
    std::string base = context.base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (ends_with_ignore_case(base, "/openai")) {
        base.resize(base.size() - 7);
        while (!base.empty() && base.back() == '/') base.pop_back();
    }
    return base;
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

std::string goog_api_key_header(const RequestContext& context) {
    if (context.api_key.empty()) return {};
    return "x-goog-api-key: " + context.api_key;
}

http::Request make_gemini_http(const RequestContext& context, const std::string& method,
                               const std::string& url, runtime::CancellationToken cancellation) {
    http::Request req;
    req.method = method;
    req.url = url;
    req.headers = context.headers;
    if (!context.api_key.empty() && !has_header_name(req.headers, "x-goog-api-key")) {
        req.headers.emplace_back(goog_api_key_header(context));
    }
    req.connect_timeout_seconds = context.options.connect_timeout_seconds;
    req.timeout_seconds = context.options.timeout_seconds > 0 ? context.options.timeout_seconds : 600;
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

json::Value json_number_value(double number) {
    json::Value value;
    value.type = json::Value::Type::Number;
    value.number = number;
    return value;
}

std::string json_string_field(const json::Value* value) {
    if (value != nullptr && value->is_string()) return value->string;
    return {};
}

std::string setting_text(const json::Value& value) {
    if (value.is_string()) return value.string;
    if (value.type == json::Value::Type::Number &&
        value.number == static_cast<double>(static_cast<long long>(value.number))) {
        return std::to_string(static_cast<long long>(value.number));
    }
    if (value.type == json::Value::Type::Bool) return value.boolean ? "true" : "false";
    return json::stringify(value);
}

json::Value veo_parameter_value(const std::string& field, const json::Value& value) {
    if (field == "seed" || field == "durationSeconds") {
        if (value.type == json::Value::Type::Number) return value;
        if (value.is_string() && !value.string.empty()) {
            try {
                return json_number_value(std::stod(value.string));
            } catch (...) {
                return value;
            }
        }
    }
    if (value.is_string()) return value;
    return json_string_value(setting_text(value));
}

std::string wire_model(const VideoGenerateRequest& request) {
    std::string model =
        request.capability.api_model.empty() ? request.model : request.capability.api_model;
    const std::size_t slash = model.find('/');
    if (slash != std::string::npos) model.erase(slash);
    return model;
}

Error media_base64(const VideoInput& media, std::string& encoded) {
    if (!media.bytes || media.bytes->empty()) {
        return {ErrorCode::Internal, "Gemini video input data is unavailable"};
    }
    encoded = input::encode_base64(*media.bytes);
    return ok_error();
}

json::Value veo_inline_image(const VideoInput& media, const std::string& encoded) {
    json::Value inline_data = json_object_value();
    inline_data.object["mimeType"] =
        json_string_value(media.mime_type.empty() ? std::string("image/png") : media.mime_type);
    inline_data.object["data"] = json_string_value(encoded);
    json::Value image = json_object_value();
    image.object["inlineData"] = std::move(inline_data);
    return image;
}

std::string gemini_error_detail(const json::Value& root) {
    if (const json::Value* error = root.get("error")) {
        if (error->is_string()) return error->string;
        if (error->is_object()) {
            std::string message = json_string_field(error->get("message"));
            if (message.empty()) message = json_string_field(error->get("status"));
            return message;
        }
    }
    if (const json::Value* errors = root.get("errors")) {
        if (errors->is_array()) {
            std::string message;
            for (const json::Value& item : errors->array) {
                std::string part = json_string_field(item.get("message"));
                if (part.empty() && item.is_string()) part = item.string;
                if (part.empty()) continue;
                if (!message.empty()) message += "; ";
                message += part;
            }
            return message;
        }
    }
    return json_string_field(root.get("message"));
}

Error gemini_video_http_error(const http::Response& response, const std::string& url) {
    ErrorCode code = ErrorCode::HttpStatus;
    if (response.status == 401 || response.status == 403) code = ErrorCode::Auth;
    else if (response.status == 429) code = ErrorCode::RateLimit;
    std::string summary = "Unexpected HTTP status from Gemini video.";
    if (response.status == 400 || response.status == 422) {
        summary = "Bad request. Gemini rejected the video prompt, settings, or attachments.";
    } else if (response.status == 401) {
        summary = "Authentication failed. Check GEMINI_API_KEY.";
    } else if (response.status == 403) {
        summary = "Access forbidden. The Gemini key may not have access to this video model.";
    } else if (response.status == 404) {
        summary = "Gemini video model or request was not found. Check -m/--model.";
    } else if (response.status == 429) {
        summary = "Too many requests. Try again later.";
    } else if (response.status >= 500 && response.status < 600) {
        summary = "Gemini failed while generating the video. Try again later.";
    }
    std::string message = "HTTP " + std::to_string(response.status) + ": " + summary + "\nURL: " + url;
    const json::ParseResult parsed = json::parse(response.body);
    std::string detail;
    if (parsed.error.ok()) detail = gemini_error_detail(parsed.value);
    if (detail.empty()) detail = compact_detail(redact_secrets(response.body, {}));
    if (!detail.empty()) message += "\nProvider message: " + detail;
    if (response.status == 401 || response.status == 403) {
        message += "\nSuggestion: Set GEMINI_API_KEY and pass --provider gemini.";
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

std::string gemini_file_id(const std::string& uri) {
    const std::string marker = "files/";
    const std::size_t start = uri.find(marker);
    if (start == std::string::npos) return {};
    std::size_t index = start + marker.size();
    std::size_t end = index;
    while (end < uri.size()) {
        const unsigned char ch = static_cast<unsigned char>(uri[end]);
        if (!(std::isalnum(ch) != 0 || uri[end] == '_' || uri[end] == '-')) break;
        ++end;
    }
    return uri.substr(index, end - index);
}

void consider_video_block(const json::Value& block, std::string& uri, std::string& data) {
    if (!block.is_object()) return;
    if (ascii_lower(json_string_field(block.get("type"))) != "video") return;
    if (uri.empty()) uri = json_string_field(block.get("uri"));
    if (data.empty()) data = json_string_field(block.get("data"));
}

void consider_content(const json::Value* content, std::string& uri, std::string& data) {
    if (content == nullptr) return;
    if (content->is_array()) {
        for (const json::Value& item : content->array) consider_video_block(item, uri, data);
        return;
    }
    consider_video_block(*content, uri, data);
}

Error persist_video_bytes(const std::string& bytes, const VideoGenerateRequest& request,
                          VideoGenerateResult& result) {
    if (bytes.size() < 12) return {ErrorCode::ProviderSchema, "video output was empty or truncated"};
    if (bytes.size() < 8 || bytes.compare(4, 4, "ftyp") != 0) {
        return {ErrorCode::ProviderSchema, "video output was not an MP4 file"};
    }
    std::string random;
    Error err = platform::secure_random_hex(8, random);
    if (!err.ok()) return err;
    const std::string temporary = request.output_path + ".ainiux-download-" + random;
    struct Cleanup {
        std::string path;
        ~Cleanup() {
            if (!path.empty()) {
                std::error_code e;
                std::filesystem::remove(std::filesystem::u8path(path), e);
            }
        }
    } cleanup{temporary};
    std::ofstream file(std::filesystem::u8path(temporary), std::ios::binary | std::ios::trunc);
    if (!file) return {ErrorCode::FileWrite, "could not create temporary video output: " + temporary};
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.flush();
    if (!file) return {ErrorCode::FileWrite, "could not write temporary video output: " + temporary};
    file.close();
    err = platform::atomic_move(temporary, request.output_path, request.overwrite);
    if (!err.ok()) return err;
    cleanup.path.clear();
    result.path = request.output_path;
    result.byte_size = static_cast<long long>(bytes.size());
    result.content_type = "video/mp4";
    return ok_error();
}

Error collect_images(const VideoGenerateRequest& request, std::vector<const VideoInput*>& images) {
    images.clear();
    for (const VideoInput& media : request.inputs) {
        if (media.mime_type.rfind("image/", 0) == 0) images.push_back(&media);
        else if (media.mime_type.rfind("video/", 0) == 0 || media.mime_type.rfind("audio/", 0) == 0) {
            return {ErrorCode::BadArgs, "this Gemini video model accepts image references only"};
        }
    }
    return ok_error();
}

}  // namespace

Error build_gemini_omni_video_request(const VideoGenerateRequest& request, json::Value& body) {
    body = json_object_value();
    const std::string model = wire_model(request);
    if (model.empty()) return {ErrorCode::BadArgs, "Gemini Omni video requires api_model"};
    body.object["model"] = json_string_value(model);
    json::Value input = json_array_value();
    json::Value text = json_object_value();
    text.object["type"] = json_string_value("text");
    text.object["text"] = json_string_value(request.prompt);
    input.array.push_back(std::move(text));
    std::vector<const VideoInput*> images;
    Error err = collect_images(request, images);
    if (!err.ok()) return err;
    for (const VideoInput* image : images) {
        std::string encoded;
        err = media_base64(*image, encoded);
        if (!err.ok()) return err;
        json::Value part = json_object_value();
        part.object["type"] = json_string_value("image");
        part.object["mime_type"] =
            json_string_value(image->mime_type.empty() ? std::string("image/png") : image->mime_type);
        part.object["data"] = json_string_value(encoded);
        input.array.push_back(std::move(part));
    }
    body.object["input"] = std::move(input);
    json::Value response_format = json_object_value();
    response_format.object["type"] = json_string_value("video");
    response_format.object["delivery"] = json_string_value("uri");
    for (const auto& setting : request.settings) {
        if (setting.first == "aspect_ratio" || setting.first == "resolution") {
            response_format.object[setting.first] = json_string_value(setting_text(setting.second));
        }
    }
    body.object["response_format"] = std::move(response_format);
    body.object["store"] = json_bool_value(true);
    body.object["background"] = json_bool_value(false);
    body.object["stream"] = json_bool_value(false);
    return ok_error();
}

Error build_gemini_veo_video_input(const VideoGenerateRequest& request, json::Value& body) {
    body = json_object_value();
    json::Value instance = json_object_value();
    instance.object["prompt"] = json_string_value(request.prompt);
    std::vector<const VideoInput*> images;
    Error err = collect_images(request, images);
    if (!err.ok()) return err;
    if (request.capability.input_mode == VideoInputMode::Text && !images.empty()) {
        return {ErrorCode::BadArgs, "text-to-video models do not accept --attach"};
    }
    if (request.capability.input_mode == VideoInputMode::Image) {
        if (images.empty()) {
            return {ErrorCode::BadArgs, "this image-to-video model requires a start image"};
        }
        std::string encoded;
        err = media_base64(*images.front(), encoded);
        if (!err.ok()) return err;
        instance.object["image"] = veo_inline_image(*images.front(), encoded);
        if (images.size() > 1) {
            err = media_base64(*images[1], encoded);
            if (!err.ok()) return err;
            instance.object["lastFrame"] = veo_inline_image(*images[1], encoded);
        }
    } else if (request.capability.input_mode == VideoInputMode::Reference) {
        if (images.empty()) {
            return {ErrorCode::BadArgs, "reference-to-video requires an image reference"};
        }
        json::Value refs = json_array_value();
        for (const VideoInput* image : images) {
            std::string encoded;
            err = media_base64(*image, encoded);
            if (!err.ok()) return err;
            json::Value ref = json_object_value();
            ref.object["image"] = veo_inline_image(*image, encoded);
            ref.object["referenceType"] = json_string_value("asset");
            refs.array.push_back(std::move(ref));
        }
        instance.object["referenceImages"] = std::move(refs);
    }
    json::Value instances = json_array_value();
    instances.array.push_back(std::move(instance));
    body.object["instances"] = std::move(instances);
    json::Value parameters = json_object_value();
    for (const auto& setting : request.settings) {
        parameters.object[setting.first] = veo_parameter_value(setting.first, setting.second);
    }
    if (!parameters.object.empty()) body.object["parameters"] = std::move(parameters);
    return ok_error();
}

Error parse_gemini_omni_video_response(const std::string& body, std::string& status,
                                       std::string& output_uri, std::string& b64_data,
                                       std::string& error_text) {
    status.clear();
    output_uri.clear();
    b64_data.clear();
    error_text.clear();
    const json::ParseResult parsed = json::parse(body);
    if (!parsed.error.ok()) {
        return {ErrorCode::JsonParse,
                "Gemini Omni video response is not valid JSON: " + parsed.error.message};
    }
    status = json_string_field(parsed.value.get("status"));
    error_text = gemini_error_detail(parsed.value);
    if (const json::Value* steps = parsed.value.get("steps")) {
        if (steps->is_array()) {
            for (const json::Value& step : steps->array) {
                if (!step.is_object()) continue;
                const std::string type = ascii_lower(json_string_field(step.get("type")));
                if (type == "thought") continue;
                consider_content(step.get("content"), output_uri, b64_data);
            }
        }
    }
    if (const json::Value* output = parsed.value.get("output_video")) {
        if (output_uri.empty()) output_uri = json_string_field(output->get("uri"));
        if (b64_data.empty()) b64_data = json_string_field(output->get("data"));
    }
    return ok_error();
}

Error parse_gemini_veo_operation(const std::string& body, bool& done, std::string& operation_name,
                                 std::string& output_uri, std::string& error_text) {
    done = false;
    operation_name.clear();
    output_uri.clear();
    error_text.clear();
    const json::ParseResult parsed = json::parse(body);
    if (!parsed.error.ok()) {
        return {ErrorCode::JsonParse, "Gemini Veo operation is not valid JSON: " + parsed.error.message};
    }
    operation_name = json_string_field(parsed.value.get("name"));
    if (parsed.value.get("done") && parsed.value.get("done")->type == json::Value::Type::Bool) {
        done = parsed.value.get("done")->boolean;
    }
    error_text = gemini_error_detail(parsed.value);
    const json::Value* response = parsed.value.get("response");
    const json::Value* generate =
        response && response->is_object() ? response->get("generateVideoResponse") : nullptr;
    const json::Value* samples =
        generate && generate->is_object() ? generate->get("generatedSamples") : nullptr;
    if (samples && samples->is_array() && !samples->array.empty()) {
        const json::Value* first = samples->at(0);
        const json::Value* video = first && first->is_object() ? first->get("video") : nullptr;
        if (video && video->is_object()) output_uri = json_string_field(video->get("uri"));
    }
    if (error_text.empty() && generate && generate->is_object()) {
        const json::Value* reasons = generate->get("raiMediaFilteredReasons");
        if (reasons && reasons->is_array() && !reasons->array.empty()) {
            error_text = "Gemini blocked the video by safety filters";
            const json::Value* first = reasons->at(0);
            if (first && first->is_string() && !first->string.empty()) {
                error_text += ": " + first->string;
            }
        }
    }
    return ok_error();
}

namespace {

Error wait_for_gemini_file(const RequestContext& context, const std::string& native_base,
                           const std::string& file_id, runtime::CancellationToken cancellation) {
    const std::string url = join_url(native_base, "/files/" + file_id);
    while (true) {
        if (cancellation.cancelled()) return cancelled_error("while waiting for Gemini file");
        http::Request req = make_gemini_http(context, "GET", url, cancellation);
        if (req.timeout_seconds <= 0) req.timeout_seconds = 30;
        const http::Result polled = http::perform(req, {context.api_key});
        if (!polled.error.ok()) return polled.error;
        if (polled.response.status < 200 || polled.response.status >= 300) {
            return gemini_video_http_error(polled.response, url);
        }
        const json::ParseResult parsed = json::parse(polled.response.body);
        if (!parsed.error.ok()) {
            return {ErrorCode::JsonParse, "Gemini file status is not valid JSON: " + parsed.error.message};
        }
        const std::string state = ascii_lower(json_string_field(parsed.value.get("state")));
        if (state == "active" || state.empty()) return ok_error();
        if (state == "failed") {
            std::string detail = gemini_error_detail(parsed.value);
            return {ErrorCode::ProviderSchema,
                    "Gemini video file processing failed" +
                        (detail.empty() ? std::string() : ": " + detail)};
        }
        Error wait_error = interruptible_sleep(kPollIntervalMs, cancellation);
        if (!wait_error.ok()) return wait_error;
    }
}

Error download_gemini_video(const RequestContext& context, const std::string& url,
                            const VideoGenerateRequest& request, VideoGenerateResult& result,
                            runtime::CancellationToken cancellation) {
    return download_generated_video(context, url, request, result, cancellation, false,
                                    goog_api_key_header(context));
}

Error generate_omni_video(const RequestContext& context, const VideoGenerateRequest& request,
                          VideoGenerateResult& result, runtime::CancellationToken cancellation) {
    json::Value body_value;
    Error err = build_gemini_omni_video_request(request, body_value);
    if (!err.ok()) return err;
    const std::string native = gemini_native_base(context);
    const std::string url = join_url(native, "/interactions");
    if (native.empty()) return {ErrorCode::BadUrl, "no base URL configured for Gemini video"};
    http::Request req = make_gemini_http(context, "POST", url, cancellation);
    req.headers.emplace_back("Content-Type: application/json");
    req.body = json::stringify(body_value);
    req.retry_post_on_send_error = false;
    const http::Result created = http::perform(req, {context.api_key});
    if (!created.error.ok()) return created.error;
    if (created.response.status < 200 || created.response.status >= 300) {
        return gemini_video_http_error(created.response, url);
    }
    std::string status, output_uri, b64_data, error_text;
    err = parse_gemini_omni_video_response(
        created.response.body, status, output_uri, b64_data, error_text);
    if (!err.ok()) return err;
    const std::string lower = ascii_lower(status);
    if (lower == "failed" || lower == "cancelled" || lower == "canceled" ||
        lower == "incomplete") {
        std::string message = "Gemini Omni video generation failed";
        if (!error_text.empty()) message += ": " + error_text;
        return {ErrorCode::ProviderSchema, message};
    }
    if (!b64_data.empty()) {
        std::string encoded = b64_data;
        encoded.erase(std::remove_if(encoded.begin(), encoded.end(),
                                     [](unsigned char ch) { return std::isspace(ch) != 0; }),
                      encoded.end());
        std::string bytes;
        Error decode_error = input::decode_base64(encoded, bytes);
        if (!decode_error.ok()) return {ErrorCode::ProviderSchema, "Gemini Omni video data is malformed"};
        return persist_video_bytes(bytes, request, result);
    }
    if (output_uri.empty()) {
        std::string message = "Gemini Omni video completed without an output URI";
        if (!error_text.empty()) message += ": " + error_text;
        return {ErrorCode::ProviderSchema, message};
    }
    const std::string file_id = gemini_file_id(output_uri);
    if (!file_id.empty()) {
        err = wait_for_gemini_file(context, native, file_id, cancellation);
        if (!err.ok()) return err;
    }
    std::string download_url = output_uri;
    if (download_url.rfind("https://", 0) != 0) {
        download_url = join_url(native, "/files/" + file_id + ":download?alt=media");
    } else if (download_url.find(":download") == std::string::npos && !file_id.empty()) {
        download_url = join_url(native, "/files/" + file_id + ":download?alt=media");
    }
    return download_gemini_video(context, download_url, request, result, cancellation);
}

Error generate_veo_video(const RequestContext& context, const VideoGenerateRequest& request,
                         VideoGenerateResult& result, runtime::CancellationToken cancellation) {
    json::Value body_value;
    Error err = build_gemini_veo_video_input(request, body_value);
    if (!err.ok()) return err;
    const std::string native = gemini_native_base(context);
    if (native.empty()) return {ErrorCode::BadUrl, "no base URL configured for Gemini video"};
    const std::string model = wire_model(request);
    const std::string url = join_url(native, "/models/" + model + ":predictLongRunning");
    http::Request req = make_gemini_http(context, "POST", url, cancellation);
    req.headers.emplace_back("Content-Type: application/json");
    req.body = json::stringify(body_value);
    req.retry_post_on_send_error = false;
    const http::Result created = http::perform(req, {context.api_key});
    if (!created.error.ok()) return created.error;
    if (created.response.status < 200 || created.response.status >= 300) {
        return gemini_video_http_error(created.response, url);
    }
    bool done = false;
    std::string operation_name, output_uri, error_text;
    err = parse_gemini_veo_operation(
        created.response.body, done, operation_name, output_uri, error_text);
    if (!err.ok()) return err;
    if (operation_name.empty()) {
        return {ErrorCode::ProviderSchema, "Gemini Veo did not return an operation name"};
    }
    std::string poll_url = operation_name;
    if (poll_url.rfind("https://", 0) != 0) poll_url = join_url(native, "/" + operation_name);
    while (!done && output_uri.empty()) {
        if (cancellation.cancelled()) return cancelled_error("while waiting for Gemini Veo");
        Error wait_error = interruptible_sleep(kPollIntervalMs, cancellation);
        if (!wait_error.ok()) return wait_error;
        http::Request poll = make_gemini_http(context, "GET", poll_url, cancellation);
        if (poll.timeout_seconds <= 0) poll.timeout_seconds = 30;
        const http::Result polled = http::perform(poll, {context.api_key});
        if (!polled.error.ok()) return polled.error;
        if (polled.response.status < 200 || polled.response.status >= 300) {
            return gemini_video_http_error(polled.response, poll_url);
        }
        err = parse_gemini_veo_operation(
            polled.response.body, done, operation_name, output_uri, error_text);
        if (!err.ok()) return err;
    }
    if (!error_text.empty() && output_uri.empty()) {
        return {ErrorCode::ProviderSchema, "Gemini Veo video generation failed: " + error_text};
    }
    if (output_uri.empty()) {
        return {ErrorCode::ProviderSchema, "Gemini Veo completed without an output URI"};
    }
    return download_gemini_video(context, output_uri, request, result, cancellation);
}

}  // namespace

Error generate_gemini_video(const RequestContext& context, const VideoGenerateRequest& request,
                            VideoGenerateResult& result, runtime::CancellationToken cancellation) {
    if (cancellation.cancelled()) return cancelled_error("before request");
    const auto started = std::chrono::steady_clock::now();
    Error err;
    if (request.capability.protocol == VideoProtocol::GeminiVeo) {
        err = generate_veo_video(context, request, result, cancellation);
    } else {
        err = generate_omni_video(context, request, result, cancellation);
    }
    result.total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - started)
                          .count();
    return err;
}

}  // namespace ainiux::provider
