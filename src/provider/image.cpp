#include "provider/image.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <utility>

#include "config/image_catalog.hpp"
#include "input/input.hpp"
#include "json/json.hpp"
#include "provider/image_fal.hpp"
#include "provider/image_gemini.hpp"
#include "provider/image_replicate.hpp"

namespace ainiux::provider {
namespace {

constexpr long long kExperimentalPixels = 2560LL * 1440LL;

int gcd_positive(int a, int b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b != 0) {
        const int t = a % b;
        a = b;
        b = t;
    }
    return a;
}

bool is_ascii_digit(char ch) {
    return ch >= '0' && ch <= '9';
}

std::string lower_copy(std::string text) {
    return ascii_lower(std::move(text));
}

Error invalid_size(const std::string& size, const std::string& reason) {
    return {ErrorCode::BadArgs, "image size " + size + " is invalid: " + reason};
}

bool parse_positive_int(const std::string& text, int& out) {
    if (text.empty()) return false;
    long long value = 0;
    for (char ch : text) {
        if (!is_ascii_digit(ch)) return false;
        value = value * 10 + (ch - '0');
        if (value > 1000000000LL) return false;
    }
    if (value <= 0) return false;
    out = static_cast<int>(value);
    return true;
}

Error parse_wh(const std::string& text, int& width, int& height) {
    const std::string lower = lower_copy(text);
    const size_t sep = lower.find('x');
    if (sep == std::string::npos || sep == 0 || sep + 1 >= lower.size()) {
        return {ErrorCode::BadArgs,
                "--size must be WIDTHxHEIGHT (for example 1536x1024) or 1k, 2k, 4k, or auto"};
    }
    int w = 0;
    int h = 0;
    if (!parse_positive_int(lower.substr(0, sep), w) ||
        !parse_positive_int(lower.substr(sep + 1), h)) {
        return {ErrorCode::BadArgs, "--size WIDTHxHEIGHT values must be positive integers"};
    }
    width = w;
    height = h;
    return ok_error();
}

Error parse_ar(const std::string& text, const ImageCapability& capability, int& ar_w, int& ar_h) {
    const std::string trimmed = ascii_trim(text);
    const size_t sep = trimmed.find(':');
    if (sep == std::string::npos || sep == 0 || sep + 1 >= trimmed.size()) {
        return {ErrorCode::BadArgs, "--ar must be W:H (for example 16:9)"};
    }
    int w = 0;
    int h = 0;
    if (!parse_positive_int(ascii_trim(trimmed.substr(0, sep)), w) ||
        !parse_positive_int(ascii_trim(trimmed.substr(sep + 1)), h)) {
        return {ErrorCode::BadArgs, "--ar W:H values must be positive integers"};
    }
    const int long_side = std::max(w, h);
    const int short_side = std::min(w, h);
    if (capability.max_ratio > 0 && short_side > 0 &&
        long_side > capability.max_ratio * short_side) {
        return {ErrorCode::BadArgs,
                "--ar " + text + " exceeds the " + std::to_string(capability.max_ratio) +
                    ":1 long-to-short limit for this image model"};
    }
    const int g = gcd_positive(w, h);
    ar_w = w / g;
    ar_h = h / g;
    return ok_error();
}

std::string reduced_ar_text(int ar_w, int ar_h) {
    return std::to_string(ar_w) + ":" + std::to_string(ar_h);
}

std::string format_wh(int width, int height) {
    return std::to_string(width) + "x" + std::to_string(height);
}

Error validate_wh(const ImageCapability& capability, int width, int height, const std::string& display) {
    const int multiple = capability.multiple > 0 ? capability.multiple : 1;
    if (width % multiple != 0 || height % multiple != 0) {
        return invalid_size(display,
                            "both edges must be multiples of " + std::to_string(multiple) + "px");
    }
    if (capability.max_edge > 0 && (width > capability.max_edge || height > capability.max_edge)) {
        return invalid_size(display, "maximum edge length is " + std::to_string(capability.max_edge) + "px");
    }
    if (width < multiple || height < multiple) {
        return invalid_size(display, "each edge must be at least " + std::to_string(multiple) + "px");
    }
    const int long_side = std::max(width, height);
    const int short_side = std::min(width, height);
    if (capability.max_ratio > 0 &&
        static_cast<long long>(long_side) > static_cast<long long>(capability.max_ratio) * short_side) {
        return invalid_size(display,
                            "long edge to short edge ratio must not exceed " +
                                std::to_string(capability.max_ratio) + ":1");
    }
    const long long pixels = static_cast<long long>(width) * static_cast<long long>(height);
    if (capability.min_pixels > 0 && pixels < capability.min_pixels) {
        return invalid_size(display,
                            "total pixels must be at least " + std::to_string(capability.min_pixels));
    }
    if (capability.max_pixels > 0 && pixels > capability.max_pixels) {
        return invalid_size(display,
                            "total pixels must be at most " + std::to_string(capability.max_pixels));
    }
    return ok_error();
}

bool class_long_edge(const ImageCapability& capability, const std::string& size, int& edge) {
    for (const auto& item : capability.size_classes) {
        if (item.first == size) {
            edge = item.second;
            return true;
        }
    }
    return false;
}

bool lookup_size_map(const ImageCapability& capability,
                     const std::string& size_class,
                     const std::string& aspect,
                     std::string& out_wh) {
    for (const ImageSizeMapEntry& item : capability.size_map) {
        if (item.size_class == size_class && item.aspect == aspect) {
            out_wh = item.output;
            return true;
        }
    }
    return false;
}

bool list_allows(const std::vector<std::string>& values, const std::string& token) {
    return std::find(values.begin(), values.end(), token) != values.end();
}

bool list_canonical(const std::vector<std::string>& values,
                    const std::string& token,
                    std::string& canonical) {
    const std::string lower = lower_copy(token);
    for (const std::string& value : values) {
        if (lower_copy(value) == lower) {
            canonical = value;
            return true;
        }
    }
    return false;
}

bool lookup_size_enum(const ImageCapability& capability,
                      const std::string& size_class,
                      std::string& token) {
    const std::string lower = lower_copy(size_class);
    for (const auto& item : capability.size_enum) {
        if (item.first == lower) {
            token = item.second;
            return true;
        }
    }
    return false;
}

bool lookup_size_enum_value(const ImageCapability& capability,
                            const std::string& token,
                            std::string& canonical) {
    const std::string lower = lower_copy(token);
    for (const auto& item : capability.size_enum) {
        if (lower_copy(item.second) == lower) {
            canonical = item.second;
            return true;
        }
    }
    return false;
}

Error require_enum_size(const ImageCapability& capability, std::string& value) {
    if (value.empty()) return ok_error();
    std::string canonical;
    if (list_canonical(capability.sizes, value, canonical) ||
        lookup_size_enum_value(capability, value, canonical)) {
        value = canonical;
        return ok_error();
    }
    if (capability.sizes.empty() && capability.size_enum.empty()) return ok_error();
    std::string allowed;
    if (!capability.sizes.empty()) {
        for (size_t i = 0; i < capability.sizes.size(); ++i) {
            if (i) allowed += ", ";
            allowed += capability.sizes[i];
        }
    } else {
        for (size_t i = 0; i < capability.size_enum.size(); ++i) {
            if (i) allowed += ", ";
            allowed += capability.size_enum[i].first + "/" + capability.size_enum[i].second;
        }
    }
    return {ErrorCode::BadArgs, "image size " + value + " is not supported; use " + allowed};
}

Error size_from_class_and_ar(const ImageCapability& capability,
                             int long_edge,
                             int ar_w,
                             int ar_h,
                             std::string& out_wh) {
    const int multiple = capability.multiple > 0 ? capability.multiple : 16;
    const int max_ab = std::max(ar_w, ar_h);
    int t = long_edge / (multiple * max_ab);
    if (t < 1) t = 1;

    auto dims_ok = [&](int w, int h) {
        return validate_wh(capability, w, h, format_wh(w, h)).ok();
    };

    auto make = [&](int step) {
        return std::pair<int, int>(multiple * ar_w * step, multiple * ar_h * step);
    };

    auto [width, height] = make(t);
    if (!dims_ok(width, height)) {
        const long long pixels = static_cast<long long>(width) * static_cast<long long>(height);
        const bool too_small =
            (capability.min_pixels > 0 && pixels < capability.min_pixels) || width < multiple ||
            height < multiple;
        if (too_small) {
            while (true) {
                ++t;
                auto next = make(t);
                if (capability.max_edge > 0 &&
                    (next.first > capability.max_edge || next.second > capability.max_edge)) {
                    break;
                }
                if (capability.max_pixels > 0 &&
                    static_cast<long long>(next.first) * static_cast<long long>(next.second) >
                        capability.max_pixels) {
                    break;
                }
                width = next.first;
                height = next.second;
                if (dims_ok(width, height)) break;
            }
        } else {
            while (t > 1) {
                --t;
                auto next = make(t);
                width = next.first;
                height = next.second;
                if (dims_ok(width, height)) break;
            }
        }
    }

    const std::string display = format_wh(width, height);
    Error err = validate_wh(capability, width, height, display);
    if (!err.ok()) {
        return {ErrorCode::BadArgs,
                err.message + " (from --size class and --ar)"};
    }
    out_wh = display;
    return ok_error();
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

http::Request make_image_http_request(const RequestContext& context,
                                      const std::string& url,
                                      runtime::CancellationToken cancellation) {
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
    return req;
}

std::string compact_detail(std::string text) {
    text = ascii_trim(std::move(text));
    for (char& ch : text) {
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
    }
    if (text.size() > 500) text = text.substr(0, 500) + "...";
    return text;
}

std::string provider_message(const json::Value& root) {
    const json::Value* err = root.get("error");
    if (err == nullptr) return "";
    if (err->is_string()) return err->string;
    if (!err->is_object()) return "";
    const json::Value* msg = err->get("message");
    if (msg != nullptr && msg->is_string()) return msg->string;
    const json::Value* detail = err->get("detail");
    if (detail != nullptr && detail->is_string()) return detail->string;
    return "";
}

Error image_http_status_error(const http::Response& response, const std::string& url) {
    ErrorCode code = ErrorCode::HttpStatus;
    if (response.status == 401 || response.status == 403) code = ErrorCode::Auth;
    else if (response.status == 429) code = ErrorCode::RateLimit;

    std::string summary = "Unexpected HTTP status from the provider.";
    if (response.status == 400) {
        summary = "Bad request. The provider rejected the image prompt, size, format, or attachments.";
    } else if (response.status == 401) {
        summary = "Authentication failed. Check that the API key is set for this provider.";
    } else if (response.status == 403) {
        summary = "Access forbidden. The API key may not have access to this model or image endpoint.";
    } else if (response.status == 404) {
        summary = "Image endpoint or model was not found. Check the base URL and model name.";
    } else if (response.status == 429) {
        summary = "Too many requests. Try again later or use a different model or provider.";
    } else if (response.status >= 500 && response.status < 600) {
        summary = "Provider service failed while generating the image. Try again later.";
    }

    std::string message = "HTTP " + std::to_string(response.status) + ": " + summary + "\nURL: " + url;
    const json::ParseResult parsed = json::parse(response.body);
    std::string detail;
    if (parsed.error.ok()) detail = compact_detail(provider_message(parsed.value));
    if (detail.empty()) detail = compact_detail(response.body);
    if (!detail.empty()) message += "\nProvider message: " + detail;

    const std::string lower = ascii_lower(detail + " " + response.body);
    if (response.status == 403 &&
        (lower.find("verif") != std::string::npos ||
         lower.find("organization") != std::string::npos)) {
        message +=
            "\nSuggestion: GPT Image models may require organization verification in the OpenAI dashboard.";
    } else if (response.status == 401 || response.status == 403) {
        message += "\nSuggestion: Verify the API key, selected provider, and that the model is gpt-image-2.";
    } else if (response.status == 404) {
        message +=
            "\nSuggestion: Image generation uses /v1/images/generations or /v1/images/edits; check --base-url.";
    }
    return {code, message};
}

long long json_int(const json::Value* value) {
    if (value == nullptr) return 0;
    if (value->type == json::Value::Type::Number) {
        return static_cast<long long>(std::llround(value->number));
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

std::string json_string(const json::Value* value) {
    if (value != nullptr && value->is_string()) return value->string;
    return {};
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

bool looks_like_http_url(const std::string& text) {
    const std::string lower = ascii_lower(text);
    return lower.rfind("https://", 0) == 0 || lower.rfind("http://", 0) == 0;
}

bool extract_first_http_url_depth(const json::Value& value, std::string& url, int depth) {
    if (depth > 6) return false;
    if (value.is_string()) {
        if (looks_like_http_url(value.string)) {
            url = value.string;
            return true;
        }
        return false;
    }
    if (value.is_array()) {
        for (const json::Value& item : value.array) {
            if (extract_first_http_url_depth(item, url, depth + 1)) return true;
        }
        return false;
    }
    if (value.is_object()) {
        if (const json::Value* direct = value.get("url")) {
            if (extract_first_http_url_depth(*direct, url, depth + 1)) return true;
        }
        if (const json::Value* href = value.get("href")) {
            if (extract_first_http_url_depth(*href, url, depth + 1)) return true;
        }
        if (const json::Value* images = value.get("images")) {
            if (extract_first_http_url_depth(*images, url, depth + 1)) return true;
        }
        for (const auto& item : value.object) {
            if (extract_first_http_url_depth(item.second, url, depth + 1)) return true;
        }
    }
    return false;
}

bool catalog_format_listed(const ImageCapability& capability, const std::string& token) {
    if (list_allows(capability.format, token)) return true;
    if (token == "jpeg") return list_allows(capability.format, "jpg");
    if (token == "jpg") return list_allows(capability.format, "jpeg");
    return false;
}

std::string catalog_api_format(const ImageCapability& capability, const std::string& format) {
    if (format.empty()) return {};
    if (format == "jpeg" && catalog_format_listed(capability, "jpg") &&
        !list_allows(capability.format, "jpeg")) {
        return "jpg";
    }
    return format;
}

}  // namespace

bool extract_first_http_url(const json::Value& value, std::string& url) {
    url.clear();
    return extract_first_http_url_depth(value, url, 0);
}

Error build_catalog_image_input(const ImageGenerateRequest& request, json::Value& input) {
    const ImageCapability& capability = request.capability;
    input = json_object_value();
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
    input.object[prompt_field] = json_string_value(request.prompt);

    int width = 0;
    int height = 0;
    const bool pixel_size = parse_wh(request.size, width, height).ok();
    if (pixel_size && !capability.width_field.empty() && !capability.height_field.empty()) {
        input.object[capability.width_field] = json_number_value(width);
        input.object[capability.height_field] = json_number_value(height);
        if (!capability.aspect_field.empty() && request.aspect.empty()) {
            input.object[capability.aspect_field] = json_string_value("custom");
        }
    } else if (pixel_size && !capability.size_field.empty()) {
        json::Value dims = json_object_value();
        dims.object["width"] = json_number_value(width);
        dims.object["height"] = json_number_value(height);
        input.object[capability.size_field] = std::move(dims);
    } else if (!request.size.empty() && !capability.size_field.empty()) {
        input.object[capability.size_field] = json_string_value(request.size);
    }

    if (!request.aspect.empty() && !capability.aspect_field.empty()) {
        input.object[capability.aspect_field] = json_string_value(request.aspect);
    }

    const std::string format = catalog_api_format(capability, request.output_format);
    if (!format.empty() && !capability.format_field.empty()) {
        input.object[capability.format_field] = json_string_value(format);
    }
    if (!request.quality.empty() && !capability.quality_field.empty()) {
        input.object[capability.quality_field] = json_string_value(request.quality);
    }

    if (!request.images.empty()) {
        if (capability.images_field.empty()) {
            return {ErrorCode::BadArgs, "image model " + capability.id +
                                            " does not support --attach reference images"};
        }
        json::Value images = json_array_value();
        images.array.reserve(request.images.size());
        for (const ImageInput& image : request.images) {
            images.array.push_back(
                json_string_value("data:" + image.mime_type + ";base64," + image.base64_data));
        }
        input.object[capability.images_field] = std::move(images);
    }
    return ok_error();
}

Error finish_resolved_size(const ImageCapability& capability, std::string& out_wh) {
    if (capability.size_mode == ImageSizeMode::EnumMode && !out_wh.empty()) {
        return require_enum_size(capability, out_wh);
    }
    if (capability.size_mode == ImageSizeMode::Pixels ||
        capability.size_mode == ImageSizeMode::WidthHeight) {
        int width = 0;
        int height = 0;
        Error err = parse_wh(out_wh, width, height);
        if (!err.ok()) return err;
        return validate_wh(capability, width, height, out_wh);
    }
    return ok_error();
}

Error resolve_aspect_token(const ImageCapability& capability,
                           const std::string& ar_text,
                           std::string& out_ar) {
    out_ar.clear();
    const std::string trimmed = ascii_trim(ar_text);
    if (trimmed.empty()) return ok_error();

    std::string canonical;
    if (list_canonical(capability.aspect_ratios, trimmed, canonical)) {
        out_ar = canonical;
        return ok_error();
    }

    int ar_w = 0;
    int ar_h = 0;
    Error ar_error = parse_ar(trimmed, capability, ar_w, ar_h);
    if (!ar_error.ok()) {
        return ar_error;
    }
    const std::string reduced = reduced_ar_text(ar_w, ar_h);
    if (!capability.aspect_ratios.empty() &&
        !list_canonical(capability.aspect_ratios, reduced, canonical) &&
        !list_canonical(capability.aspect_ratios, trimmed, canonical)) {
        return {ErrorCode::BadArgs, "--ar " + trimmed + " is not supported by this image model"};
    }
    out_ar = canonical.empty() ? reduced : canonical;
    return ok_error();
}

bool has_width_height_fields(const ImageCapability& capability) {
    return !capability.width_field.empty() && !capability.height_field.empty();
}

Error resolve_pixel_size(const ImageCapability& capability,
                         const std::string& size_text,
                         const std::string& ar_text,
                         std::string& out_size) {
    int class_edge = 0;
    const bool named = class_long_edge(capability, size_text, class_edge);
    if (!named) {
        if (!ar_text.empty()) {
            return {ErrorCode::BadArgs,
                    "--ar cannot be combined with an explicit WIDTHxHEIGHT --size; "
                    "use --size 1k|2k|4k with --ar, or pass --size WIDTHxHEIGHT without --ar"};
        }
        int width = 0;
        int height = 0;
        Error err = parse_wh(size_text, width, height);
        if (!err.ok()) return err;
        out_size = format_wh(width, height);
        return finish_resolved_size(capability, out_size);
    }

    if (!ar_text.empty()) {
        int ar_w = 0;
        int ar_h = 0;
        Error ar_error = parse_ar(ar_text, capability, ar_w, ar_h);
        if (!ar_error.ok()) return ar_error;
        const std::string reduced = reduced_ar_text(ar_w, ar_h);
        if (lookup_size_map(capability, size_text, reduced, out_size)) {
            return finish_resolved_size(capability, out_size);
        }
        return size_from_class_and_ar(capability, class_edge, ar_w, ar_h, out_size);
    }

    std::string mapped;
    if (lookup_size_map(capability, size_text, "", mapped) ||
        lookup_size_map(capability, size_text, "1:1", mapped)) {
        out_size = mapped;
        return finish_resolved_size(capability, out_size);
    }

    int width = class_edge;
    int height = class_edge;
    const int multiple = capability.multiple > 0 ? capability.multiple : 1;
    width -= width % multiple;
    height -= height % multiple;
    if (capability.max_pixels > 0) {
        while (width >= multiple &&
               static_cast<long long>(width) * static_cast<long long>(height) >
                   capability.max_pixels) {
            width -= multiple;
            height -= multiple;
        }
    }
    out_size = format_wh(width, height);
    return finish_resolved_size(capability, out_size);
}

Error resolve_enum_size(const ImageCapability& capability,
                        const std::string& size_text,
                        std::string& out_size) {
    if (size_text.empty()) return ok_error();
    if (lookup_size_enum(capability, size_text, out_size)) {
        return require_enum_size(capability, out_size);
    }
    if (lookup_size_enum_value(capability, size_text, out_size)) {
        return require_enum_size(capability, out_size);
    }
    std::string canonical;
    if (list_canonical(capability.sizes, size_text, canonical)) {
        out_size = canonical;
        return ok_error();
    }
    if (size_text.find('x') != std::string::npos &&
        (has_width_height_fields(capability) || !capability.size_field.empty())) {
        int width = 0;
        int height = 0;
        Error err = parse_wh(size_text, width, height);
        if (!err.ok()) return err;
        out_size = format_wh(width, height);
        if (capability.multiple > 1 || capability.max_edge > 0 || capability.min_pixels > 0 ||
            capability.max_pixels > 0 || capability.max_ratio > 0) {
            return validate_wh(capability, width, height, out_size);
        }
        return ok_error();
    }
    out_size = size_text;
    return require_enum_size(capability, out_size);
}

Error resolve_image_size(const ImageCapability& capability,
                         const std::string& size,
                         const std::string& ar,
                         std::string& out_size,
                         std::string& out_ar) {
    out_size.clear();
    out_ar.clear();
    const std::string size_text = lower_copy(ascii_trim(size));
    const std::string ar_text = ascii_trim(ar);

    if (size_text.empty() && ar_text.empty()) {
        return ok_error();
    }
    if (size_text == "auto") {
        if (!ar_text.empty() && capability.size_mode != ImageSizeMode::EnumMode &&
            capability.size_mode != ImageSizeMode::Aspect) {
            return {ErrorCode::BadArgs, "--ar requires --size 1k, 2k, or 4k"};
        }
        if (!ar_text.empty()) return resolve_aspect_token(capability, ar_text, out_ar);
        return ok_error();
    }

    if (capability.size_mode == ImageSizeMode::EnumMode) {
        if (!ar_text.empty()) {
            if (capability.aspect_ratios.empty() && capability.aspect_field.empty() &&
                capability.size_map.empty()) {
                return {ErrorCode::BadArgs, "this image model does not use --ar"};
            }
            Error ar_error = resolve_aspect_token(capability, ar_text, out_ar);
            if (!ar_error.ok()) return ar_error;
        }
        std::string mapped;
        const std::string class_key = size_text.empty() ? std::string("1k") : size_text;
        if (!out_ar.empty() && capability.aspect_field.empty() &&
            (lookup_size_map(capability, class_key, out_ar, mapped) ||
             lookup_size_map(capability, "", out_ar, mapped))) {
            out_size = mapped;
            return ok_error();
        }
        if (size_text.empty()) return ok_error();
        return resolve_enum_size(capability, size_text, out_size);
    }

    if (capability.size_mode == ImageSizeMode::Aspect) {
        if (!ar_text.empty()) {
            Error ar_error = resolve_aspect_token(capability, ar_text, out_ar);
            if (!ar_error.ok()) return ar_error;
        }
        if (!size_text.empty() && size_text.find('x') != std::string::npos) {
            if (!has_width_height_fields(capability)) {
                return {ErrorCode::BadArgs,
                        "this image model uses --ar for shape; do not pass --size WIDTHxHEIGHT"};
            }
            int width = 0;
            int height = 0;
            Error err = parse_wh(size_text, width, height);
            if (!err.ok()) return err;
            out_size = format_wh(width, height);
            Error valid = validate_wh(capability, width, height, out_size);
            if (!valid.ok()) return valid;
            if (out_ar.empty()) {
                std::string custom;
                if (list_canonical(capability.aspect_ratios, "custom", custom)) out_ar = custom;
                else if (!capability.aspect_field.empty()) out_ar = "custom";
            }
            return ok_error();
        }
        if (ar_text.empty()) {
            return {ErrorCode::BadArgs, "this image model requires --ar W:H"};
        }
        if (!size_text.empty() &&
            lookup_size_map(capability, size_text, out_ar, out_size)) {
            return ok_error();
        }
        if (lower_copy(out_ar) == "custom") {
            return {ErrorCode::BadArgs, "--ar custom requires --size WIDTHxHEIGHT"};
        }
        return ok_error();
    }

    if (size_text.empty()) {
        return {ErrorCode::BadArgs, "--ar requires --size 1k, 2k, or 4k"};
    }
    return resolve_pixel_size(capability, size_text, ar_text, out_size);
}

Error normalize_image_quality(const ImageCapability& capability,
                              const std::string& value,
                              std::string& out) {
    const std::string lower = lower_copy(ascii_trim(value));
    if (lower.empty() || lower == "auto") {
        if (!capability.quality.empty() && lower == "auto" &&
            !list_allows(capability.quality, "auto") && !list_allows(capability.quality, lower)) {
            return {ErrorCode::BadArgs, "--quality auto is not supported by this image model"};
        }
        out.clear();
        return ok_error();
    }
    if (!capability.quality.empty() && !list_allows(capability.quality, lower)) {
        std::string allowed;
        for (size_t i = 0; i < capability.quality.size(); ++i) {
            if (i) allowed += ", ";
            allowed += capability.quality[i];
        }
        return {ErrorCode::BadArgs, "--quality must be " + allowed};
    }
    if (capability.quality.empty() &&
        lower != "low" && lower != "medium" && lower != "high") {
        return {ErrorCode::BadArgs, "--quality must be low, medium, high, or auto"};
    }
    out = lower;
    return ok_error();
}

bool format_listed(const ImageCapability& capability, const std::string& token) {
    if (list_allows(capability.format, token)) return true;
    if (token == "jpeg") return list_allows(capability.format, "jpg");
    if (token == "jpg") return list_allows(capability.format, "jpeg");
    return false;
}

Error normalize_image_format(const ImageCapability& capability,
                             const std::string& value,
                             std::string& out) {
    std::string lower = lower_copy(ascii_trim(value));
    if (lower == "jpg") lower = "jpeg";
    if (lower.empty()) {
        out = capability.format_default.empty() ? kDefaultImageFormat : capability.format_default;
        if (out == "jpg") out = "jpeg";
        if (out == "auto") out.clear();
        return ok_error();
    }
    if (lower == "auto") {
        if (!capability.format.empty() && !list_allows(capability.format, "auto")) {
            return {ErrorCode::BadArgs, "--format auto is not supported by this image model"};
        }
        out.clear();
        return ok_error();
    }
    if (capability.format.empty() && lower != "png" && lower != "jpeg" && lower != "webp") {
        return {ErrorCode::BadArgs, "--format in image mode must be png, jpeg, webp, or auto"};
    }
    if (!capability.format.empty() && !format_listed(capability, lower) &&
        !list_allows(capability.format, "auto")) {
        std::string allowed;
        for (size_t i = 0; i < capability.format.size(); ++i) {
            if (i) allowed += ", ";
            allowed += capability.format[i];
        }
        return {ErrorCode::BadArgs, "--format in image mode must be " + allowed};
    }
    out = lower;
    return ok_error();
}

std::string image_extension_for_format(const std::string& format) {
    if (format == "jpeg" || format == "jpg") return "jpg";
    if (format == "webp") return "webp";
    return "png";
}

bool image_size_is_experimental(const std::string& size_wh) {
    int width = 0;
    int height = 0;
    if (!parse_wh(size_wh, width, height).ok()) return false;
    return static_cast<long long>(width) * static_cast<long long>(height) > kExperimentalPixels;
}

std::string serialize_image_request(const ImageGenerateRequest& request) {
    std::ostringstream out;
    out << "{\"model\":" << json::quote(request.model)
        << ",\"prompt\":" << json::quote(request.prompt)
        << ",\"n\":1";
    if (!request.size.empty()) {
        out << ",\"size\":" << json::quote(request.size);
    }
    if (!request.quality.empty()) {
        out << ",\"quality\":" << json::quote(request.quality);
    }
    if (!request.output_format.empty()) {
        out << ",\"output_format\":" << json::quote(request.output_format);
    }
    if (!request.images.empty()) {
        out << ",\"images\":[";
        for (size_t i = 0; i < request.images.size(); ++i) {
            if (i > 0) out << ",";
            const std::string data_url = "data:" + request.images[i].mime_type + ";base64," +
                                         request.images[i].base64_data;
            out << "{\"image_url\":" << json::quote(data_url) << "}";
        }
        out << "]";
    }
    out << "}";
    return out.str();
}

Error parse_images_response(const std::string& body, ImageGenerateResult& result) {
    const json::ParseResult parsed = json::parse(body);
    if (!parsed.error.ok()) {
        return {ErrorCode::JsonParse, "image response is not valid JSON: " + parsed.error.message};
    }
    const std::string err_msg = provider_message(parsed.value);
    if (!err_msg.empty()) {
        return {ErrorCode::ProviderSchema, "image provider error: " + compact_detail(err_msg)};
    }
    const json::Value* data = parsed.value.get("data");
    if (data == nullptr || !data->is_array() || data->array.empty()) {
        return {ErrorCode::ProviderSchema, "image response did not contain data[0].b64_json"};
    }
    const json::Value* first = data->at(0);
    if (first == nullptr || !first->is_object()) {
        return {ErrorCode::ProviderSchema, "image response did not contain data[0].b64_json"};
    }
    const json::Value* b64 = first->get("b64_json");
    if (b64 == nullptr || !b64->is_string() || b64->string.empty()) {
        return {ErrorCode::ProviderSchema, "image response did not contain data[0].b64_json"};
    }
    std::string encoded = b64->string;
    encoded.erase(std::remove_if(encoded.begin(), encoded.end(),
                                 [](unsigned char ch) { return std::isspace(ch) != 0; }),
                  encoded.end());
    Error decode_error = input::decode_base64(encoded, result.bytes);
    if (!decode_error.ok()) {
        return {ErrorCode::ProviderSchema, "image response b64_json is malformed"};
    }
    encoded.clear();
    encoded.shrink_to_fit();

    result.output_format = json_string(parsed.value.get("output_format"));
    result.size = json_string(parsed.value.get("size"));
    result.quality = json_string(parsed.value.get("quality"));
    if (const json::Value* usage = parsed.value.get("usage")) {
        result.total_tokens = json_int(usage->get("total_tokens"));
        result.input_tokens = json_int(usage->get("input_tokens"));
        result.output_tokens = json_int(usage->get("output_tokens"));
    }
    return ok_error();
}

std::string image_endpoint_url(const RequestContext& context, bool edits) {
    return join_url(context.base_url, edits ? "/images/edits" : "/images/generations");
}

Error generate_or_edit_image(const RequestContext& context,
                             const ImageGenerateRequest& request,
                             ImageGenerateResult& result,
                             runtime::CancellationToken cancellation) {
    if (context.profile.offline) {
        return {ErrorCode::UnsupportedFeature,
                "provider none cannot generate images; select an online image provider"};
    }
    if (context.base_url.empty()) {
        return {ErrorCode::BadUrl, "no base URL configured; pass BASE_URL or --base-url"};
    }
    if (request.prompt.empty()) {
        return {ErrorCode::BadArgs, "image generation requires a prompt"};
    }
    if (!config::image_protocol_implemented(request.protocol)) {
        return {ErrorCode::UnsupportedFeature,
                std::string("image protocol ") +
                    config::image_protocol_name(request.protocol) +
                    " is not built into this ainiux"};
    }
    if (request.images.size() > static_cast<size_t>(kMaxImageEditInputs)) {
        return {ErrorCode::BadArgs,
                "image generation accepts at most 16 --attach images"};
    }
    if (request.protocol == ImageProtocol::ReplicatePredictions) {
        return generate_replicate_image(context, request, result, cancellation);
    }
    if (request.protocol == ImageProtocol::FalQueue) {
        return generate_fal_image(context, request, result, cancellation);
    }
    if (request.protocol == ImageProtocol::GeminiInteractions) {
        return generate_gemini_image(context, request, result, cancellation);
    }

    const bool edits = !request.images.empty();
    const std::string url = image_endpoint_url(context, edits);
    http::Request req = make_image_http_request(context, url, cancellation);
    req.body = serialize_image_request(request);

    const auto started = std::chrono::steady_clock::now();
    const http::Result http_result = http::perform(req, {context.api_key});
    result.total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - started)
                          .count();
    if (!http_result.error.ok()) {
        return http_result.error;
    }
    if (http_result.response.status < 200 || http_result.response.status >= 300) {
        return image_http_status_error(http_result.response, url);
    }
    return parse_images_response(http_result.response.body, result);
}

Error allocate_unused_image_path(const std::string& directory,
                                 const std::string& extension,
                                 std::string& path) {
    const std::string ext = extension.empty() ? "png" : extension;
    const bool relative_cwd = directory.empty() || directory == ".";
    std::error_code fs_error;
    const std::filesystem::path dir = relative_cwd ? std::filesystem::path{}
                                                   : std::filesystem::u8path(directory);
    if (!relative_cwd) {
        std::error_code exists_error;
        if (!std::filesystem::is_directory(dir, exists_error) || exists_error) {
            return {ErrorCode::FileWrite, "image output directory does not exist: " + directory};
        }
    }
    (void)fs_error;
    for (int n = 1; n <= 9999; ++n) {
        const std::string name = "image" + std::to_string(n) + "." + ext;
        const std::filesystem::path candidate =
            relative_cwd ? std::filesystem::u8path(name) : dir / std::filesystem::u8path(name);
        std::error_code exists_error;
        if (!std::filesystem::exists(candidate, exists_error) && !exists_error) {
            path = candidate.u8string();
            return ok_error();
        }
    }
    return {ErrorCode::FileWrite, "could not allocate an unused imageN." + ext + " name"};
}

}  // namespace ainiux::provider
