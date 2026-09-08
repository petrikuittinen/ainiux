#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ainiux {

enum class VideoProtocol { FalQueue, ReplicatePredictions };
enum class VideoInputMode { Text, Image, Reference, Mixed };

struct VideoCapability {
    std::string id;
    std::string provider = "fal";
    std::string model_regex;
    std::string api_model;
    VideoProtocol protocol = VideoProtocol::FalQueue;
    VideoInputMode input_mode = VideoInputMode::Text;
    bool default_for_provider = false;
    int max_input_images = 0;
    int max_input_videos = 0;
    int max_input_audios = 0;
    int max_input_total = 0;
    int max_input_image_bytes = 0;
    int max_input_video_bytes = 0;
    int max_input_audio_bytes = 0;
    std::string prompt_field = "prompt";
    std::string start_image_field = "image_url";
    std::string end_image_field = "end_image_url";
    std::string reference_images_field;
    std::string reference_videos_field;
    std::string reference_audios_field;
    // JSON object merged into the provider request before explicit settings.
    std::string defaults_json;
    // Public JSON array of scalar setting descriptors. Each descriptor has a
    // name, provider field, type and optional default/options/min/max/step.
    std::string settings_json = "[]";
    int priority = 0;
    bool enabled = true;
    std::uint64_t load_order = 0;
};

struct VideoCatalog {
    std::vector<VideoCapability> models;
    std::uint64_t next_load_order = 1;
};

}  // namespace ainiux
