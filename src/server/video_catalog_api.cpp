#include "server/video_catalog_api.hpp"

#include <set>

#include "config/video_catalog.hpp"
#include "json/json.hpp"
#include "provider/provider.hpp"
#include "server/limits.hpp"

namespace ainiux::server {
std::string public_video_catalog_json(const VideoCatalog& catalog,
                                      const std::string& configured_provider) {
    std::set<std::string> names;
    for (const auto& model : catalog.models) if (model.enabled) names.insert(provider::canonical_profile_name(model.provider));
    std::string providers = "["; bool first = true;
    for (const std::string& name : names) { if (!first) providers += ','; first = false; providers += json::quote(name); }
    providers += ']';
    std::string models = "["; first = true;
    for (const VideoCapability& model : catalog.models) {
        if (!model.enabled) continue;
        if (!first) models += ',';
        first = false;
        models += "{\"id\":" + json::quote(model.id) +
            ",\"provider\":" + json::quote(provider::canonical_profile_name(model.provider)) +
            ",\"model\":" + json::quote(model.api_model) +
            ",\"default\":" + (model.default_for_provider ? "true" : "false") +
            ",\"input_mode\":" + json::quote(config::video_input_mode_name(model.input_mode)) +
            ",\"max_input_images\":" + std::to_string(model.max_input_images) +
            ",\"max_input_videos\":" + std::to_string(model.max_input_videos) +
            ",\"max_input_audios\":" + std::to_string(model.max_input_audios) +
            ",\"max_input_total\":" + std::to_string(model.max_input_total) +
            ",\"max_input_image_bytes\":" + std::to_string(model.max_input_image_bytes) +
            ",\"max_input_video_bytes\":" + std::to_string(model.max_input_video_bytes) +
            ",\"max_input_audio_bytes\":" + std::to_string(model.max_input_audio_bytes) +
            ",\"settings\":" + model.settings_json + "}";
    }
    models += ']';
    return "{\"default_provider\":" + json::quote(provider::canonical_profile_name(configured_provider.empty() ? "fal" : configured_provider)) +
        ",\"limits\":{\"max_input_bytes\":" + std::to_string(Limits::video_upload_body_bytes) +
        ",\"max_image_bytes\":" + std::to_string(30U * 1024U * 1024U) +
        ",\"max_audio_bytes\":" + std::to_string(15U * 1024U * 1024U) +
        ",\"max_total_bytes\":" + std::to_string(Limits::video_job_upload_bytes) +
        ",\"max_inputs\":" + std::to_string(Limits::video_input_count) + "},\"providers\":" + providers + ",\"models\":" + models + "}";
}
}  // namespace ainiux::server
