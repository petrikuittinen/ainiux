#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ainiux/video_setting.hpp"
#include "common.hpp"
#include "json/json.hpp"
#include "provider/provider.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::provider {

struct VideoInput {
    std::string mime_type;
    std::string display_name;
    std::shared_ptr<const std::string> bytes;
    std::string remote_url;
};

struct VideoGenerateRequest {
    std::string model;
    std::string prompt;
    VideoCapability capability;
    std::map<std::string, json::Value> settings;
    std::vector<VideoInput> inputs;
    std::string output_path;
    bool overwrite = false;
};

struct VideoGenerateResult {
    std::string path;
    std::string content_type = "video/mp4";
    long long byte_size = 0;
    long long total_ms = -1;
};

Error normalize_video_settings(const VideoCapability& capability,
                               const std::map<std::string, json::Value>& requested,
                               std::map<std::string, json::Value>& normalized);
Error build_fal_video_input(const VideoGenerateRequest& request, json::Value& input);
Error build_replicate_video_input(const VideoGenerateRequest& request, json::Value& input);
Error build_xai_imagine_video_input(const VideoGenerateRequest& request, json::Value& input);
Error build_gemini_omni_video_request(const VideoGenerateRequest& request, json::Value& body);
Error build_gemini_veo_video_input(const VideoGenerateRequest& request, json::Value& body);
Error parse_fal_video_result(const std::string& body, std::string& output_url);
Error parse_xai_imagine_video_status(const std::string& body, std::string& status,
                                     std::string& request_id, std::string& output_url,
                                     std::string& error_text);
Error parse_gemini_omni_video_response(const std::string& body, std::string& status,
                                       std::string& output_uri, std::string& b64_data,
                                       std::string& error_text);
Error parse_gemini_veo_operation(const std::string& body, bool& done, std::string& operation_name,
                                 std::string& output_uri, std::string& error_text);
Error generate_video(const RequestContext& context,
                     const VideoGenerateRequest& request,
                     VideoGenerateResult& result,
                     runtime::CancellationToken cancellation = runtime::CancellationToken());
Error generate_replicate_video(const RequestContext& context,
                               const VideoGenerateRequest& request,
                               VideoGenerateResult& result,
                               runtime::CancellationToken cancellation = runtime::CancellationToken());
Error generate_xai_imagine_video(const RequestContext& context,
                                 const VideoGenerateRequest& request,
                                 VideoGenerateResult& result,
                                 runtime::CancellationToken cancellation = runtime::CancellationToken());
Error generate_gemini_video(const RequestContext& context,
                            const VideoGenerateRequest& request,
                            VideoGenerateResult& result,
                            runtime::CancellationToken cancellation = runtime::CancellationToken());
Error download_generated_video(const RequestContext& context,
                               const std::string& url,
                               const VideoGenerateRequest& request,
                               VideoGenerateResult& result,
                               runtime::CancellationToken cancellation,
                               bool authorize,
                               const std::string& extra_header = {});
Error allocate_unused_video_path(const std::string& directory, std::string& path);

}  // namespace ainiux::provider
