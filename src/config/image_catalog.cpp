#include "config/image_catalog.hpp"

#include "config/model_catalog.hpp"

#include <algorithm>
#include <regex>
#include <sstream>

namespace ainiux::config {
namespace {

std::string ascii_lower(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

bool scope_matches(const std::string& configured, const std::string& actual) {
    const std::string normalized = ascii_lower(configured);
    const std::string actual_normalized = ascii_lower(actual);
    return normalized == "any" || normalized == actual_normalized;
}

int specificity(const ImageCapability& capability) {
    return ascii_lower(capability.provider) == "any" ? 0 : 1;
}

}  // namespace

bool parse_image_protocol(const std::string& text, ImageProtocol& protocol) {
    const std::string lower = ascii_lower(text);
    if (lower == "openai_images") {
        protocol = ImageProtocol::OpenAiImages;
        return true;
    }
    if (lower == "replicate_predictions") {
        protocol = ImageProtocol::ReplicatePredictions;
        return true;
    }
    if (lower == "fal_queue") {
        protocol = ImageProtocol::FalQueue;
        return true;
    }
    if (lower == "gemini_interactions") {
        protocol = ImageProtocol::GeminiInteractions;
        return true;
    }
    return false;
}

const char* image_protocol_name(ImageProtocol protocol) {
    switch (protocol) {
        case ImageProtocol::OpenAiImages:
            return "openai_images";
        case ImageProtocol::ReplicatePredictions:
            return "replicate_predictions";
        case ImageProtocol::FalQueue:
            return "fal_queue";
        case ImageProtocol::GeminiInteractions:
            return "gemini_interactions";
    }
    return "openai_images";
}

std::string image_protocol_names() {
    return "openai_images, replicate_predictions, fal_queue, or gemini_interactions";
}

bool image_protocol_implemented(ImageProtocol protocol) {
    return protocol == ImageProtocol::OpenAiImages ||
           protocol == ImageProtocol::ReplicatePredictions ||
           protocol == ImageProtocol::FalQueue ||
           protocol == ImageProtocol::GeminiInteractions;
}

bool image_model_regex_matches(const std::string& expression, const std::string& model) {
    if (model_regex_matches(expression, model)) return true;
    try {
        const std::regex pattern(expression, std::regex::ECMAScript | std::regex::icase);
        return std::regex_search(model, pattern);
    } catch (const std::regex_error&) {
        return false;
    }
}

bool parse_image_size_mode(const std::string& text, ImageSizeMode& mode) {
    const std::string lower = ascii_lower(text);
    if (lower == "pixels") {
        mode = ImageSizeMode::Pixels;
        return true;
    }
    if (lower == "enum") {
        mode = ImageSizeMode::EnumMode;
        return true;
    }
    if (lower == "aspect") {
        mode = ImageSizeMode::Aspect;
        return true;
    }
    if (lower == "width_height") {
        mode = ImageSizeMode::WidthHeight;
        return true;
    }
    return false;
}

const char* image_size_mode_name(ImageSizeMode mode) {
    switch (mode) {
        case ImageSizeMode::Pixels:
            return "pixels";
        case ImageSizeMode::EnumMode:
            return "enum";
        case ImageSizeMode::Aspect:
            return "aspect";
        case ImageSizeMode::WidthHeight:
            return "width_height";
    }
    return "pixels";
}

const ImageCapability* resolve_image_capability(const ImageCatalog& catalog,
                                                const std::string& provider,
                                                const std::string& model) {
    const ImageCapability* best = nullptr;
    for (const ImageCapability& candidate : catalog.models) {
        if (!candidate.enabled || !scope_matches(candidate.provider, provider) ||
            !image_model_regex_matches(candidate.model_regex, model)) {
            continue;
        }
        if (best == nullptr || candidate.priority > best->priority ||
            (candidate.priority == best->priority && specificity(candidate) > specificity(*best)) ||
            (candidate.priority == best->priority && specificity(candidate) == specificity(*best) &&
             candidate.load_order > best->load_order)) {
            best = &candidate;
        }
    }
    return best;
}

std::string default_image_model(const ImageCatalog& catalog, const std::string& provider) {
    const ImageCapability* best = nullptr;
    for (const ImageCapability& candidate : catalog.models) {
        if (!candidate.enabled || !candidate.default_for_provider || candidate.api_model.empty() ||
            !scope_matches(candidate.provider, provider)) {
            continue;
        }
        if (best == nullptr || specificity(candidate) > specificity(*best) ||
            (specificity(candidate) == specificity(*best) && candidate.priority > best->priority) ||
            (specificity(candidate) == specificity(*best) && candidate.priority == best->priority &&
             candidate.load_order > best->load_order)) {
            best = &candidate;
        }
    }
    return best == nullptr ? std::string() : best->api_model;
}

std::string known_image_models_description(const ImageCatalog& catalog,
                                           const std::string& provider) {
    std::vector<std::string> names;
    for (const ImageCapability& candidate : catalog.models) {
        if (!candidate.enabled || !scope_matches(candidate.provider, provider)) continue;
        const std::string name =
            candidate.api_model.empty() ? candidate.id : candidate.api_model;
        if (std::find(names.begin(), names.end(), name) == names.end()) {
            names.push_back(name);
        }
    }
    if (names.empty()) return "(none in images.conf for this provider)";
    std::ostringstream out;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i > 0) out << ", ";
        if (i == 16) {
            out << "...";
            break;
        }
        out << names[i];
    }
    return out.str();
}

}  // namespace ainiux::config
