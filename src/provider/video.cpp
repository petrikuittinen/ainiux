#include "provider/video.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <thread>

#include "input/input.hpp"
#include "platform/filesystem.hpp"
#include "provider/image.hpp"
#include "security/redact.hpp"

namespace ainiux::provider {
namespace {
constexpr long kMaxVideoBytes = 1024L * 1024L * 1024L;

json::Value object_value() { json::Value v; v.type = json::Value::Type::Object; return v; }
json::Value array_value() { json::Value v; v.type = json::Value::Type::Array; return v; }
json::Value string_value(const std::string& s) { json::Value v; v.type = json::Value::Type::String; v.string = s; return v; }

bool descriptor_matches_type(const json::Value& value, const std::string& type) {
    if (type == "string") return value.type == json::Value::Type::String;
    if (type == "enum") {
        if (value.type == json::Value::Type::String) return true;
        return value.type == json::Value::Type::Number &&
            value.number == static_cast<double>(static_cast<long long>(value.number));
    }
    if (type == "boolean") return value.type == json::Value::Type::Bool;
    if (type == "integer") return value.type == json::Value::Type::Number &&
        value.number == static_cast<double>(static_cast<long long>(value.number));
    return type == "number" && value.type == json::Value::Type::Number;
}

Error validate_setting(const json::Value& descriptor, const json::Value& value) {
    const json::Value* name = descriptor.get("name");
    const json::Value* type = descriptor.get("type");
    if (!name || !name->is_string() || !type || !type->is_string())
        return {ErrorCode::Config, "videos.conf contains an invalid setting descriptor"};
    if (!descriptor_matches_type(value, type->string))
        return {ErrorCode::BadArgs, "video setting " + name->string + " must be " + type->string};
    if (const json::Value* options = descriptor.get("options")) {
        auto option_text = [](const json::Value& option) -> std::string {
            if (option.is_string()) return option.string;
            if (option.type == json::Value::Type::Number &&
                option.number == static_cast<double>(static_cast<long long>(option.number))) {
                return std::to_string(static_cast<long long>(option.number));
            }
            return json::stringify(option);
        };
        const std::string wanted = option_text(value);
        bool found = false;
        for (const json::Value& option : options->array) {
            if (option_text(option) == wanted) { found = true; break; }
        }
        if (!found) return {ErrorCode::BadArgs, "unsupported value for video setting " + name->string};
    }
    if (value.type == json::Value::Type::Number) {
        const json::Value* minimum = descriptor.get("min");
        const json::Value* maximum = descriptor.get("max");
        if (minimum && value.number < minimum->number) return {ErrorCode::BadArgs, "video setting " + name->string + " is below its minimum"};
        if (maximum && value.number > maximum->number) return {ErrorCode::BadArgs, "video setting " + name->string + " exceeds its maximum"};
    }
    return ok_error();
}

bool starts_with_https(const std::string& url) { return url.rfind("https://", 0) == 0; }

std::string compact_provider_detail(std::string detail) {
    detail = ascii_trim(std::move(detail));
    for (char& ch : detail) {
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
    }
    if (detail.size() > 500) detail = detail.substr(0, 500) + "...";
    return detail;
}

Error fal_video_http_error(const RequestContext& context,
                           const http::Response& response,
                           const std::string& operation) {
    ErrorCode code = ErrorCode::HttpStatus;
    if (response.status == 401 || response.status == 403) code = ErrorCode::Auth;
    else if (response.status == 429) code = ErrorCode::RateLimit;

    std::string summary = "fal " + operation + " failed";
    if (response.status == 400 || response.status == 422) {
        summary = "fal rejected the video request; check the prompt, settings, and attachments";
    } else if (response.status == 401) {
        summary = "fal authentication failed; check FAL_API_KEY or FAL_KEY";
    } else if (response.status == 403) {
        summary = "fal access was forbidden; the key may not have access to this model";
    } else if (response.status == 404) {
        summary = "fal model or request was not found; check -m/--model";
    } else if (response.status == 429) {
        summary = "fal rate limit reached; try again later";
    } else if (response.status >= 500 && response.status < 600) {
        summary = "fal failed while generating the video; try again later";
    }

    std::string detail;
    const json::ParseResult parsed = json::parse(response.body);
    if (parsed.error.ok() && parsed.value.is_object()) {
        for (const char* field : {"error", "message", "msg"}) {
            const json::Value* value = parsed.value.get(field);
            if (value && value->is_string() && !value->string.empty()) {
                detail = value->string;
                break;
            }
        }
        if (detail.empty()) {
            const json::Value* value = parsed.value.get("detail");
            if (value) detail = value->is_string() ? value->string : json::stringify(*value);
        }
    }
    if (detail.empty()) detail = response.body;
    detail = compact_provider_detail(redact_secrets(std::move(detail), request_secrets(context)));
    std::string message = "HTTP " + std::to_string(response.status) + ": " + summary;
    if (!detail.empty()) message += "\nProvider message: " + detail;
    return {code, message};
}

std::string url_origin(const std::string& url) {
    const std::size_t scheme = url.find("://");
    if (scheme == std::string::npos) return {};
    const std::size_t slash = url.find('/', scheme + 3U);
    return url.substr(0, slash);
}

}  // namespace

Error download_generated_video(const RequestContext& context, const std::string& url,
                               const VideoGenerateRequest& request, VideoGenerateResult& result,
                               runtime::CancellationToken cancellation, bool authorize) {
    if (!starts_with_https(url)) return {ErrorCode::BadUrl, "video output URL must use HTTPS"};
    std::string random;
    Error err = platform::secure_random_hex(8, random);
    if (!err.ok()) return err;
    const std::string temporary = request.output_path + ".ainiux-download-" + random;
    struct Cleanup { std::string path; ~Cleanup() { if (!path.empty()) { std::error_code e; std::filesystem::remove(std::filesystem::u8path(path), e); } } } cleanup{temporary};
    std::ofstream file(std::filesystem::u8path(temporary), std::ios::binary | std::ios::trunc);
    if (!file) return {ErrorCode::FileWrite, "could not create temporary video output: " + temporary};
    long long bytes = 0;
    http::Request get;
    get.method = "GET"; get.url = url; get.follow_redirects = true; get.block_private_addresses = true;
    get.connect_timeout_seconds = context.options.connect_timeout_seconds;
    get.timeout_seconds = context.options.timeout_seconds > 0 ? context.options.timeout_seconds : 600;
    get.proxy = context.options.proxy; get.insecure_tls = context.options.insecure_tls;
    get.trace = context.options.trace_http; get.cancellation = cancellation;
    get.max_body_bytes = kMaxVideoBytes; get.retain_body = false;
    if (authorize) {
        get.headers = context.headers;
        bool have_authorization = false;
        for (const std::string& header : get.headers) {
            const std::size_t colon = header.find(':');
            if (colon != std::string::npos && ascii_lower(header.substr(0, colon)) == "authorization") {
                have_authorization = true;
                break;
            }
        }
        if (!have_authorization && !context.api_key.empty()) {
            get.headers.push_back("Authorization: Bearer " + context.api_key);
        }
    }
    get.on_body = [&](const std::string& chunk) {
        file.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        if (!file) return Error{ErrorCode::FileWrite, "could not write temporary video output: " + temporary};
        bytes += static_cast<long long>(chunk.size());
        return ok_error();
    };
    const http::Result downloaded = http::perform(get, {context.api_key});
    file.flush(); file.close();
    if (!downloaded.error.ok()) return downloaded.error;
    if (downloaded.response.status < 200 || downloaded.response.status >= 300)
        return {ErrorCode::HttpStatus, "HTTP " + std::to_string(downloaded.response.status) + ": video download failed\nURL: " + url};
    if (bytes < 12) return {ErrorCode::ProviderSchema, "video output was empty or truncated"};
    std::ifstream check(std::filesystem::u8path(temporary), std::ios::binary);
    char header[32] = {}; check.read(header, sizeof(header));
    const std::streamsize count = check.gcount();
    std::string head(header, count > 0 ? static_cast<std::size_t>(count) : 0U);
    if (head.size() < 8 || head.substr(4, 4) != "ftyp") return {ErrorCode::ProviderSchema, "video output was not an MP4 file"};
    err = platform::atomic_move(temporary, request.output_path, request.overwrite);
    if (!err.ok()) return err;
    cleanup.path.clear();
    result.path = request.output_path; result.byte_size = bytes;
    if (!downloaded.response.content_type.empty()) result.content_type = downloaded.response.content_type;
    return ok_error();
}

namespace {

http::Request fal_request(const RequestContext& context, const std::string& method,
                          const std::string& url, runtime::CancellationToken cancellation);

http::Request signed_request(const RequestContext& context, const std::string& method,
                             const std::string& url, runtime::CancellationToken cancellation) {
    http::Request req; req.method = method; req.url = url; req.block_private_addresses = true;
    req.connect_timeout_seconds = context.options.connect_timeout_seconds;
    req.timeout_seconds = context.options.timeout_seconds > 0 ? context.options.timeout_seconds : 300;
    req.proxy = context.options.proxy; req.insecure_tls = context.options.insecure_tls;
    req.trace = context.options.trace_http; req.cancellation = cancellation; return req;
}

Error upload_fal_media(const RequestContext& context, VideoInput& media,
                       runtime::CancellationToken cancellation) {
    if (!media.bytes) return {ErrorCode::Internal, "video input data is unavailable"};
    const bool multipart = media.bytes->size() > 90U * 1024U * 1024U;
    std::string random; Error err = platform::secure_random_hex(8, random); if (!err.ok()) return err;
    std::string extension = "bin";
    const std::size_t slash = media.mime_type.find('/'); if (slash != std::string::npos) extension = media.mime_type.substr(slash + 1);
    if (extension == "quicktime") extension = "mov"; else if (extension == "mpeg") extension = "mp3";
    json::Value metadata = object_value(); metadata.object["file_name"] = string_value("ainiux-" + random + "." + extension);
    metadata.object["content_type"] = string_value(media.mime_type);
    const std::string init_url = std::string("https://rest.alpha.fal.ai/storage/upload/") +
        (multipart ? "initiate-multipart" : "initiate") + "?storage_type=fal-cdn-v3";
    http::Request init = fal_request(context, "POST", init_url, cancellation);
    init.headers.push_back("Content-Type: application/json");
    init.headers.push_back("X-Fal-Object-Lifecycle: {\"expiration_duration_seconds\":3600}");
    init.body = json::stringify(metadata); init.retry_post_on_send_error = false;
    const http::Result initialized = http::perform(init, {context.api_key});
    if (!initialized.error.ok()) return initialized.error;
    if (initialized.response.status < 200 || initialized.response.status >= 300)
        return fal_video_http_error(context, initialized.response, "CDN upload initiation");
    const json::ParseResult parsed = json::parse(initialized.response.body);
    const json::Value* file_url = parsed.error.ok() ? parsed.value.get("file_url") : nullptr;
    const json::Value* upload_url = parsed.error.ok() ? parsed.value.get("upload_url") : nullptr;
    if (!file_url || !file_url->is_string() || !upload_url || !upload_url->is_string() ||
        !starts_with_https(file_url->string) || !starts_with_https(upload_url->string)) return {ErrorCode::ProviderSchema, "fal CDN upload initiation omitted secure URLs"};
    if (!multipart) {
        http::Request upload = signed_request(context, "PUT", upload_url->string, cancellation);
        upload.headers.push_back("Content-Type: " + media.mime_type); upload.body = *media.bytes;
        const http::Result sent = http::perform(upload, {});
        if (!sent.error.ok()) return sent.error;
        if (sent.response.status < 200 || sent.response.status >= 300) return {ErrorCode::HttpStatus, "HTTP " + std::to_string(sent.response.status) + ": fal CDN upload failed"};
    } else {
        constexpr std::size_t part_size = 10U * 1024U * 1024U;
        json::Value parts = array_value();
        for (std::size_t offset = 0, number = 1; offset < media.bytes->size(); offset += part_size, ++number) {
            const std::size_t count = std::min(part_size, media.bytes->size() - offset);
            const std::size_t query = upload_url->string.find('?');
            const std::string part_url = upload_url->string.substr(0, query) + "/" + std::to_string(number) +
                (query == std::string::npos ? std::string() : upload_url->string.substr(query));
            http::Request upload = signed_request(context, "PUT", part_url, cancellation);
            upload.body = media.bytes->substr(offset, count);
            http::Result sent;
            for (int attempt = 0; attempt < 3; ++attempt) { sent = http::perform(upload, {}); if (sent.error.ok() && sent.response.status >= 200 && sent.response.status < 300) break; }
            if (!sent.error.ok()) return sent.error;
            if (sent.response.status < 200 || sent.response.status >= 300) return {ErrorCode::HttpStatus, "HTTP " + std::to_string(sent.response.status) + ": fal CDN multipart upload failed"};
            const json::ParseResult part = json::parse(sent.response.body);
            const json::Value* etag = part.error.ok() ? part.value.get("etag") : nullptr;
            if (!etag || !etag->is_string()) return {ErrorCode::ProviderSchema, "fal CDN multipart response omitted etag"};
            json::Value entry = object_value(); json::Value part_number; part_number.type = json::Value::Type::Number; part_number.number = static_cast<double>(number);
            entry.object["partNumber"] = part_number; entry.object["etag"] = *etag; parts.array.push_back(std::move(entry));
        }
        json::Value complete = object_value(); complete.object["parts"] = std::move(parts);
        const std::size_t query = upload_url->string.find('?');
        const std::string complete_url = upload_url->string.substr(0, query) + "/complete" +
            (query == std::string::npos ? std::string() : upload_url->string.substr(query));
        http::Request finish = signed_request(context, "POST", complete_url, cancellation);
        finish.headers.push_back("Content-Type: application/json"); finish.body = json::stringify(complete); finish.retry_post_on_send_error = false;
        const http::Result completed = http::perform(finish, {});
        if (!completed.error.ok()) return completed.error;
        if (completed.response.status < 200 || completed.response.status >= 300) return {ErrorCode::HttpStatus, "HTTP " + std::to_string(completed.response.status) + ": fal CDN multipart completion failed"};
    }
    media.remote_url = file_url->string; media.bytes.reset(); return ok_error();
}

http::Request fal_request(const RequestContext& context, const std::string& method,
                          const std::string& url, runtime::CancellationToken cancellation) {
    http::Request req; req.method = method; req.url = url; req.headers = context.headers;
    bool authorization = false;
    for (const std::string& h : req.headers) if (ascii_lower(h.substr(0, h.find(':'))) == "authorization") authorization = true;
    if (!authorization) req.headers.push_back("Authorization: Key " + context.api_key);
    req.connect_timeout_seconds = context.options.connect_timeout_seconds;
    req.timeout_seconds = context.options.timeout_seconds; req.proxy = context.options.proxy;
    req.insecure_tls = context.options.insecure_tls; req.trace = context.options.trace_http;
    req.cancellation = cancellation; return req;
}

Error wait_slice(runtime::CancellationToken cancellation) {
    for (int i = 0; i < 10; ++i) { if (cancellation.cancelled()) return {ErrorCode::Cancelled, "video generation cancelled"}; std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    return ok_error();
}
}  // namespace

Error normalize_video_settings(const VideoCapability& capability,
                               const std::map<std::string, json::Value>& requested,
                               std::map<std::string, json::Value>& normalized) {
    normalized.clear();
    const json::ParseResult descriptors = json::parse(capability.settings_json);
    if (!descriptors.error.ok() || !descriptors.value.is_array()) return {ErrorCode::Config, "invalid settings_json for video model " + capability.id};
    for (const json::Value& descriptor : descriptors.value.array) {
        if (!descriptor.is_object()) return {ErrorCode::Config, "invalid setting descriptor for video model " + capability.id};
        const json::Value* name = descriptor.get("name");
        const json::Value* field = descriptor.get("field");
        if (!name || !name->is_string() || !field || !field->is_string()) return {ErrorCode::Config, "video setting descriptor omits name or field"};
        auto found = requested.find(name->string);
        const json::Value* value = found == requested.end() ? descriptor.get("default") : &found->second;
        if (value) {
            Error err = validate_setting(descriptor, *value); if (!err.ok()) return err;
            normalized[field->string] = *value;
        }
    }
    for (const auto& setting : requested) {
        bool found = false;
        for (const json::Value& descriptor : descriptors.value.array) {
            const json::Value* name = descriptor.get("name"); if (name && name->string == setting.first) { found = true; break; }
        }
        if (!found) return {ErrorCode::BadArgs, "unknown video setting for " + capability.api_model + ": " + setting.first};
    }
    return ok_error();
}

namespace {
json::Value coerce_replicate_setting(const json::Value& value) {
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

void assign_media_field(json::Value& input, const std::string& field,
                        const std::vector<json::Value>& uris, int max_count) {
    if (field.empty() || uris.empty()) return;
    if (max_count == 1) {
        input.object[field] = uris[0];
        return;
    }
    json::Value values = array_value();
    values.array = uris;
    input.object[field] = std::move(values);
}

Error apply_video_media_fields(const VideoGenerateRequest& request, json::Value& input,
                               const std::string& missing_upload_message) {
    std::vector<json::Value> images, videos, audios;
    for (const VideoInput& media : request.inputs) {
        if (media.remote_url.empty()) return {ErrorCode::Internal, missing_upload_message};
        if (media.mime_type.rfind("image/", 0) == 0) images.push_back(string_value(media.remote_url));
        else if (media.mime_type.rfind("video/", 0) == 0) videos.push_back(string_value(media.remote_url));
        else if (media.mime_type.rfind("audio/", 0) == 0) audios.push_back(string_value(media.remote_url));
    }
    const VideoCapability& capability = request.capability;
    if (capability.input_mode == VideoInputMode::Text && !request.inputs.empty()) {
        return {ErrorCode::BadArgs, "text-to-video models do not accept --attach"};
    }
    if (capability.input_mode == VideoInputMode::Image && images.empty()) {
        return {ErrorCode::BadArgs, "this image-to-video model requires a start image"};
    }
    if (capability.input_mode == VideoInputMode::Reference && images.empty() && videos.empty()) {
        return {ErrorCode::BadArgs, "reference-to-video requires an image or video reference"};
    }
    if (capability.input_mode == VideoInputMode::Reference) {
        assign_media_field(input, capability.reference_images_field, images, capability.max_input_images);
        assign_media_field(input, capability.reference_videos_field, videos, capability.max_input_videos);
        assign_media_field(input, capability.reference_audios_field, audios, capability.max_input_audios);
        return ok_error();
    }
    const bool force_reference_images = capability.input_mode == VideoInputMode::Mixed &&
        (!videos.empty() || !audios.empty()) && !capability.reference_images_field.empty() &&
        capability.start_image_field == "image";
    if (force_reference_images) {
        assign_media_field(input, capability.reference_images_field, images, capability.max_input_images);
    } else {
        std::size_t used = 0;
        if (!images.empty() && !capability.start_image_field.empty()) {
            input.object[capability.start_image_field] = images[0];
            used = 1;
        }
        if (images.size() > 1 && !capability.end_image_field.empty()) {
            input.object[capability.end_image_field] = images[1];
            used = 2;
        }
        if (capability.input_mode == VideoInputMode::Mixed && images.size() > used &&
            !capability.reference_images_field.empty()) {
            assign_media_field(input, capability.reference_images_field,
                               std::vector<json::Value>(images.begin() + static_cast<std::ptrdiff_t>(used),
                                                       images.end()),
                               capability.max_input_images);
        }
    }
    if (capability.input_mode == VideoInputMode::Mixed) {
        assign_media_field(input, capability.reference_videos_field, videos, capability.max_input_videos);
        assign_media_field(input, capability.reference_audios_field, audios, capability.max_input_audios);
    }
    return ok_error();
}

Error fill_video_request_object(const VideoGenerateRequest& request, json::Value& input,
                                bool coerce_numeric_settings, const std::string& missing_upload_message) {
    input = object_value();
    if (!request.capability.defaults_json.empty()) {
        const json::ParseResult defaults = json::parse(request.capability.defaults_json);
        if (!defaults.error.ok() || !defaults.value.is_object()) return {ErrorCode::Config, "invalid video defaults_json"};
        input.object = defaults.value.object;
    }
    input.object[request.capability.prompt_field] = string_value(request.prompt);
    for (const auto& setting : request.settings) {
        input.object[setting.first] = coerce_numeric_settings ? coerce_replicate_setting(setting.second)
                                                              : setting.second;
    }
    return apply_video_media_fields(request, input, missing_upload_message);
}
}  // namespace

Error build_fal_video_input(const VideoGenerateRequest& request, json::Value& input) {
    return fill_video_request_object(request, input, false, "video input was not uploaded to fal storage");
}

Error build_replicate_video_input(const VideoGenerateRequest& request, json::Value& input) {
    json::Value payload;
    Error err = fill_video_request_object(
        request, payload, true, "video input was not uploaded to Replicate");
    if (!err.ok()) return err;
    input = object_value();
    input.object["input"] = std::move(payload);
    return ok_error();
}

Error parse_fal_video_result(const std::string& body, std::string& output_url) {
    output_url.clear(); const json::ParseResult parsed = json::parse(body);
    if (!parsed.error.ok()) return {ErrorCode::JsonParse, "fal video result is not valid JSON: " + parsed.error.message};
    const json::Value* video = parsed.value.get("video");
    const json::Value* url = video && video->is_object() ? video->get("url") : nullptr;
    if (!url || !url->is_string() || url->string.empty()) return {ErrorCode::ProviderSchema, "fal video result did not contain video.url"};
    output_url = url->string; return ok_error();
}

Error generate_video(const RequestContext& context, const VideoGenerateRequest& request,
                     VideoGenerateResult& result, runtime::CancellationToken cancellation) {
    if (context.profile.offline) return {ErrorCode::UnsupportedFeature, "provider none cannot generate videos"};
    if (context.base_url.empty()) return {ErrorCode::BadUrl, "no base URL configured for video generation"};
    if (request.prompt.empty()) return {ErrorCode::BadArgs, "video generation requires a prompt"};
    if (request.output_path.empty()) return {ErrorCode::BadArgs, "video generation requires an output path"};
    if (request.capability.protocol == VideoProtocol::ReplicatePredictions) {
        return generate_replicate_video(context, request, result, cancellation);
    }
    VideoGenerateRequest wire_request = request;
    for (VideoInput& media : wire_request.inputs) { Error upload_error = upload_fal_media(context, media, cancellation); if (!upload_error.ok()) return upload_error; }
    json::Value input_value; Error err = build_fal_video_input(wire_request, input_value); if (!err.ok()) return err;
    const std::string endpoint = fal_queue_url(context, request.capability.api_model);
    http::Request submit = fal_request(context, "POST", endpoint, cancellation);
    submit.headers.push_back("Content-Type: application/json"); submit.headers.push_back("X-Fal-Store-IO: 0");
    submit.headers.push_back("X-Fal-Object-Lifecycle-Preference: {\"expiration_duration_seconds\":3600}");
    submit.body = json::stringify(input_value); submit.retry_post_on_send_error = false;
    const auto started = std::chrono::steady_clock::now();
    auto elapsed = [&] { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count(); };
    const http::Result created = http::perform(submit, {context.api_key});
    if (!created.error.ok()) return created.error;
    if (created.response.status < 200 || created.response.status >= 300)
        return fal_video_http_error(context, created.response, "video submission");
    std::string id, status_url, response_url, cancel_url;
    err = parse_fal_queue_submit(created.response.body, id, status_url, response_url, cancel_url); if (!err.ok()) return err;
    const std::string result_url = fal_queue_result_url(context, request.capability.api_model, id);
    if (status_url.empty()) status_url = result_url + "/status";
    // Use fal's canonical result endpoint even if submit advertises a legacy
    // convenience response_url ending in /response.
    if (!id.empty()) response_url = result_url;
    if (cancel_url.empty() && !id.empty()) cancel_url = result_url + "/cancel";
    if (!starts_with_https(endpoint) || url_origin(status_url) != url_origin(endpoint) ||
        url_origin(response_url) != url_origin(endpoint) ||
        (!cancel_url.empty() && url_origin(cancel_url) != url_origin(endpoint))) {
        return {ErrorCode::BadUrl, "fal queue returned an untrusted video control URL"};
    }
    for (;;) {
        if (cancellation.cancelled()) { if (!cancel_url.empty()) { http::Request cancel = fal_request(context, "PUT", cancel_url, {}); cancel.timeout_seconds = 10; (void)http::perform(cancel, {context.api_key}); } return {ErrorCode::Cancelled, "video generation cancelled"}; }
        http::Request poll = fal_request(context, "GET", status_url, cancellation); if (poll.timeout_seconds <= 0) poll.timeout_seconds = 30;
        const http::Result p = http::perform(poll, {context.api_key}); if (!p.error.ok()) return p.error;
        if (p.response.status < 200 || p.response.status >= 300)
            return fal_video_http_error(context, p.response, "video status request");
        std::string status, detail; err = parse_fal_queue_status(p.response.body, status, detail); if (!err.ok()) return err;
        if (fal_status_failed(status)) return {ErrorCode::ProviderSchema, "fal video generation failed" + (detail.empty() ? std::string() : ": " + detail)};
        if (fal_status_completed(status)) break;
        err = wait_slice(cancellation); if (!err.ok()) return err;
    }
    http::Request result_request = fal_request(context, "GET", response_url, cancellation);
    const http::Result payload = http::perform(result_request, {context.api_key}); if (!payload.error.ok()) return payload.error;
    if (payload.response.status < 200 || payload.response.status >= 300)
        return fal_video_http_error(context, payload.response, "video result request");
    std::string output_url; err = parse_fal_video_result(payload.response.body, output_url); if (!err.ok()) return err;
    err = download_generated_video(context, output_url, request, result, cancellation, false); result.total_ms = elapsed(); return err;
}

Error allocate_unused_video_path(const std::string& directory, std::string& path) {
    for (int i = 1; i < 100000; ++i) {
        const std::filesystem::path candidate = std::filesystem::u8path(directory) / ("video" + std::to_string(i) + ".mp4");
        std::error_code ec; if (!std::filesystem::exists(candidate, ec) && !ec) { path = candidate.u8string(); return ok_error(); }
        if (ec) return {ErrorCode::FileRead, "could not inspect video output path: " + candidate.u8string()};
    }
    return {ErrorCode::FileWrite, "could not allocate an unused video output path"};
}
}  // namespace ainiux::provider
