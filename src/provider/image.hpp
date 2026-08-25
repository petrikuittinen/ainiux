#pragma once

#include <string>
#include <vector>

#include "ainiux/image_setting.hpp"
#include "common.hpp"
#include "json/json.hpp"
#include "provider/provider.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::provider {

inline constexpr int kMaxImageEditInputs = 16;
inline constexpr const char kDefaultImageFormat[] = "png";

struct ImageGenerateRequest {
    std::string model;
    std::string prompt;
    std::string size;
    std::string aspect;
    std::string quality;
    std::string output_format;
    ImageProtocol protocol = ImageProtocol::OpenAiImages;
    ImageCapability capability;
    std::vector<ImageInput> images;
};

struct ImageGenerateResult {
    std::string bytes;
    std::string output_format;
    std::string size;
    std::string quality;
    long long input_tokens = 0;
    long long output_tokens = 0;
    long long total_tokens = 0;
    long long total_ms = -1;
};

// Resolve --size/--ar using the matched images.conf record. Empty size and ar
// leave outputs empty (provider auto). Pixel models put WIDTHxHEIGHT in
// out_size; enum/aspect models may put a class token in out_size and the
// aspect string in out_ar.
Error resolve_image_size(const ImageCapability& capability,
                         const std::string& size,
                         const std::string& ar,
                         std::string& out_size,
                         std::string& out_ar);
inline Error resolve_image_size(const ImageCapability& capability,
                                const std::string& size,
                                const std::string& ar,
                                std::string& out_wh) {
    std::string unused_ar;
    return resolve_image_size(capability, size, ar, out_wh, unused_ar);
}

Error normalize_image_quality(const ImageCapability& capability,
                              const std::string& value,
                              std::string& out);
Error normalize_image_format(const ImageCapability& capability,
                             const std::string& value,
                             std::string& out);

std::string image_extension_for_format(const std::string& format);
bool image_size_is_experimental(const std::string& size_wh);

std::string serialize_image_request(const ImageGenerateRequest& request);
Error parse_images_response(const std::string& body, ImageGenerateResult& result);

std::string image_endpoint_url(const RequestContext& context, bool edits);

Error build_catalog_image_input(const ImageGenerateRequest& request, json::Value& input);
bool extract_first_http_url(const json::Value& value, std::string& url);

Error serialize_replicate_request(const ImageGenerateRequest& request, std::string& body);
Error serialize_fal_request(const ImageGenerateRequest& request, std::string& body);
Error serialize_gemini_request(const ImageGenerateRequest& request, std::string& body);
Error parse_gemini_interaction(const std::string& body, ImageGenerateResult& result);
std::string gemini_interactions_url(const RequestContext& context);
Error parse_fal_queue_submit(const std::string& body,
                             std::string& request_id,
                             std::string& status_url,
                             std::string& response_url,
                             std::string& cancel_url);
Error parse_fal_queue_status(const std::string& body,
                             std::string& status,
                             std::string& error_text);
Error parse_fal_queue_result(const std::string& body, std::string& output_url);
std::string fal_queue_url(const RequestContext& context, const std::string& api_model);
bool fal_status_completed(const std::string& status);
bool fal_status_failed(const std::string& status);
Error parse_replicate_prediction(const std::string& body,
                                 std::string& status,
                                 std::string& output_url,
                                 std::string& poll_url,
                                 std::string& cancel_url,
                                 std::string& error_text);
std::string replicate_prediction_url(const RequestContext& context, const std::string& api_model);
bool replicate_status_succeeded(const std::string& status);
bool replicate_status_failed(const std::string& status);
bool replicate_status_canceled(const std::string& status);
bool replicate_status_terminal(const std::string& status);

Error generate_or_edit_image(const RequestContext& context,
                             const ImageGenerateRequest& request,
                             ImageGenerateResult& result,
                             runtime::CancellationToken cancellation = runtime::CancellationToken());

Error allocate_unused_image_path(const std::string& directory,
                                 const std::string& extension,
                                 std::string& path);

}  // namespace ainiux::provider
