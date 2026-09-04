#include "server/image_catalog_api.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <vector>

#include "config/image_catalog.hpp"
#include "json/json.hpp"
#include "provider/provider.hpp"
#include "server/limits.hpp"

namespace ainiux::server {
namespace {

std::string lower(std::string value) {
    for (char& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}

void append_unique(std::vector<std::string>& values, const std::string& value) {
    if (value.empty()) return;
    const std::string key = lower(value);
    for (const std::string& existing : values) {
        if (lower(existing) == key) return;
    }
    values.push_back(value);
}

std::string strings_json(const std::vector<std::string>& values) {
    std::string output = "[";
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index) output += ',';
        output += json::quote(values[index]);
    }
    return output + ']';
}

std::vector<std::string> size_options(const ImageCapability& capability) {
    std::vector<std::string> result;
    for (const auto& entry : capability.size_classes) append_unique(result, entry.first);
    for (const auto& entry : capability.size_enum) append_unique(result, entry.first);
    for (const std::string& size : capability.sizes) append_unique(result, size);
    return result;
}

std::vector<std::string> aspect_options(const ImageCapability& capability) {
    std::vector<std::string> result = capability.aspect_ratios;
    for (const ImageSizeMapEntry& entry : capability.size_map) append_unique(result, entry.aspect);
    if (capability.size_mode == ImageSizeMode::Pixels && !capability.size_classes.empty()) {
        append_unique(result, "1:1");
    }
    return result;
}

bool custom_size_enabled(const ImageCapability& capability) {
    if (capability.size_mode == ImageSizeMode::Pixels ||
        capability.size_mode == ImageSizeMode::WidthHeight) return true;
    return std::find(capability.aspect_ratios.begin(), capability.aspect_ratios.end(),
                     "custom") != capability.aspect_ratios.end() &&
           !capability.width_field.empty() && !capability.height_field.empty();
}

}  // namespace

std::string public_image_catalog_json(const ImageCatalog& catalog,
                                      const std::string& configured_provider) {
    std::set<std::string> provider_names;
    for (const ImageCapability& capability : catalog.models) {
        if (capability.enabled && !capability.api_model.empty() &&
            config::image_protocol_implemented(capability.protocol) &&
            lower(capability.provider) != "any") {
            provider_names.insert(provider::canonical_profile_name(capability.provider));
        }
    }
    const std::string default_provider =
        provider::canonical_profile_name(configured_provider.empty() ? "openai" : configured_provider);
    std::string providers = "[";
    bool first = true;
    for (const std::string& name : provider_names) {
        if (!first) providers += ',';
        first = false;
        providers += json::quote(name);
    }
    providers += ']';

    std::string models = "[";
    first = true;
    for (const ImageCapability& capability : catalog.models) {
        if (!capability.enabled || capability.api_model.empty() ||
            !config::image_protocol_implemented(capability.protocol)) continue;
        if (!first) models += ',';
        first = false;
        const bool custom = custom_size_enabled(capability);
        models += "{\"id\":" + json::quote(capability.id) +
                  ",\"provider\":" + json::quote(provider::canonical_profile_name(capability.provider)) +
                  ",\"model\":" + json::quote(capability.api_model) +
                  ",\"default\":" + (capability.default_for_provider ? "true" : "false") +
                  ",\"edits\":" + (capability.edits ? "true" : "false") +
                  ",\"max_input_images\":" +
                      std::to_string(capability.edits
                                         ? std::min<int>(Limits::image_input_count,
                                                         capability.max_input_images > 0
                                                             ? capability.max_input_images
                                                             : Limits::image_input_count)
                                         : 0) +
                  ",\"size_mode\":" + json::quote(config::image_size_mode_name(capability.size_mode)) +
                  ",\"sizes\":" + strings_json(size_options(capability)) +
                  ",\"aspect_ratios\":" + strings_json(aspect_options(capability)) +
                  ",\"qualities\":" + strings_json(capability.quality) +
                  ",\"formats\":" + strings_json(capability.format) +
                  ",\"format_default\":" + json::quote(capability.format_default) +
                  ",\"custom_size\":{\"enabled\":" + (custom ? "true" : "false") +
                  ",\"multiple\":" + std::to_string(std::max(1, capability.multiple)) +
                  ",\"max_edge\":" + std::to_string(capability.max_edge) +
                  ",\"min_pixels\":" + std::to_string(capability.min_pixels) +
                  ",\"max_pixels\":" + std::to_string(capability.max_pixels) +
                  ",\"max_ratio\":" + std::to_string(capability.max_ratio) + "}}";
    }
    models += ']';
    return "{\"default_provider\":" + json::quote(default_provider) +
           ",\"limits\":{\"max_input_images\":" + std::to_string(Limits::image_input_count) +
           ",\"max_image_bytes\":" + std::to_string(Limits::upload_body_bytes) +
           ",\"max_total_image_bytes\":" + std::to_string(Limits::image_job_upload_bytes) +
           "},\"providers\":" + providers + ",\"models\":" + models + "}";
}

}  // namespace ainiux::server
