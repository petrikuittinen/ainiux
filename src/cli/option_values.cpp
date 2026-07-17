#include "cli/option_values.hpp"

namespace ainiux::cli::option_values {

const std::vector<const char*>& image_capabilities() {
    static const std::vector<const char*> kValues = {
        kImageCapabilityAuto,
        kImageCapabilityAllow,
        kImageCapabilityDeny,
    };
    return kValues;
}

std::string image_capability_description() {
    return "auto, allow, or deny";
}

std::vector<std::string> image_capability_strings() {
    std::vector<std::string> out;
    out.reserve(image_capabilities().size());
    for (const char* capability : image_capabilities()) {
        out.emplace_back(capability);
    }
    return out;
}

bool is_image_capability(const std::string& value) {
    for (const char* capability : image_capabilities()) {
        if (value == capability) {
            return true;
        }
    }
    return false;
}

}  // namespace ainiux::cli::option_values