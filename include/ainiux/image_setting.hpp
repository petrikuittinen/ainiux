#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ainiux {

enum class ImageProtocol {
    OpenAiImages,
    ReplicatePredictions,
    FalQueue,
};

enum class ImageSizeMode {
    Pixels,
    EnumMode,
    Aspect,
    WidthHeight,
};

struct ImageSizeMapEntry {
    std::string size_class;
    std::string aspect;
    std::string output;
};

struct ImageCapability {
    std::string id;
    std::string provider = "any";
    std::string model_regex;
    std::string api_model;
    ImageProtocol protocol = ImageProtocol::OpenAiImages;
    bool default_for_provider = false;
    bool edits = false;
    int max_input_images = 0;
    ImageSizeMode size_mode = ImageSizeMode::Pixels;
    std::vector<std::pair<std::string, int>> size_classes;
    std::vector<std::pair<std::string, std::string>> size_enum;
    std::vector<ImageSizeMapEntry> size_map;
    std::vector<std::string> sizes;
    std::vector<std::string> aspect_ratios;
    int max_edge = 0;
    long long min_pixels = 0;
    long long max_pixels = 0;
    int max_ratio = 0;
    int multiple = 16;
    std::vector<std::string> quality;
    std::vector<std::string> format;
    std::string format_default = "png";
    std::string prompt_field = "prompt";
    std::string width_field;
    std::string height_field;
    std::string size_field;
    std::string aspect_field;
    std::string format_field;
    std::string quality_field;
    std::string images_field;
    std::string defaults_json;
    int priority = 0;
    bool enabled = true;
    std::uint64_t load_order = 0;
};

struct ImageCatalog {
    std::vector<ImageCapability> models;
    std::uint64_t next_load_order = 1;
};

}  // namespace ainiux
