#include "config/video_catalog.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

#include "config/model_catalog.hpp"

namespace ainiux::config {
namespace {
std::string lower(std::string value) {
    for (char& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}
bool scope_matches(const std::string& configured, const std::string& actual) {
    return lower(configured) == "any" || lower(configured) == lower(actual);
}
int specificity(const VideoCapability& value) { return lower(value.provider) == "any" ? 0 : 1; }
bool video_regex_matches(const std::string& expression, const std::string& model) {
    if (model_regex_matches(expression, model)) return true;
    try {
        return std::regex_search(model, std::regex(expression, std::regex::ECMAScript | std::regex::icase));
    } catch (const std::regex_error&) { return false; }
}
}  // namespace

bool parse_video_protocol(const std::string& text, VideoProtocol& protocol) {
    if (lower(text) != "fal_queue") return false;
    protocol = VideoProtocol::FalQueue;
    return true;
}
const char* video_protocol_name(VideoProtocol) { return "fal_queue"; }

bool parse_video_input_mode(const std::string& text, VideoInputMode& mode) {
    const std::string value = lower(text);
    if (value == "text") mode = VideoInputMode::Text;
    else if (value == "image") mode = VideoInputMode::Image;
    else if (value == "reference") mode = VideoInputMode::Reference;
    else return false;
    return true;
}
const char* video_input_mode_name(VideoInputMode mode) {
    switch (mode) {
        case VideoInputMode::Text: return "text";
        case VideoInputMode::Image: return "image";
        case VideoInputMode::Reference: return "reference";
    }
    return "text";
}

const VideoCapability* resolve_video_capability(const VideoCatalog& catalog,
                                                const std::string& provider,
                                                const std::string& model) {
    const VideoCapability* best = nullptr;
    for (const VideoCapability& candidate : catalog.models) {
        if (!candidate.enabled || !scope_matches(candidate.provider, provider) ||
            !video_regex_matches(candidate.model_regex, model)) continue;
        if (!best || candidate.priority > best->priority ||
            (candidate.priority == best->priority && specificity(candidate) > specificity(*best)) ||
            (candidate.priority == best->priority && specificity(candidate) == specificity(*best) &&
             candidate.load_order > best->load_order)) best = &candidate;
    }
    return best;
}

std::string default_video_model(const VideoCatalog& catalog, const std::string& provider) {
    const VideoCapability* best = nullptr;
    for (const VideoCapability& candidate : catalog.models) {
        if (!candidate.enabled || !candidate.default_for_provider || candidate.api_model.empty() ||
            !scope_matches(candidate.provider, provider)) continue;
        if (!best || specificity(candidate) > specificity(*best) ||
            (specificity(candidate) == specificity(*best) && candidate.priority > best->priority) ||
            (specificity(candidate) == specificity(*best) && candidate.priority == best->priority &&
             candidate.load_order > best->load_order)) best = &candidate;
    }
    return best ? best->api_model : std::string();
}

std::string known_video_models_description(const VideoCatalog& catalog,
                                           const std::string& provider) {
    std::vector<std::string> names;
    for (const VideoCapability& candidate : catalog.models) {
        if (!candidate.enabled || !scope_matches(candidate.provider, provider)) continue;
        const std::string name = candidate.api_model.empty() ? candidate.id : candidate.api_model;
        if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    }
    if (names.empty()) return "(none in videos.conf for this provider)";
    std::ostringstream out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i) out << ", ";
        out << names[i];
    }
    return out.str();
}
}  // namespace ainiux::config
