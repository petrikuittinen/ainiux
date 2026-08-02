#include "ui/provider_model_display.hpp"

#include "editor/detail/unicode.hpp"
#include "provider/provider.hpp"

#include <regex>

namespace ainiux::ui {
namespace {

const std::regex kModelPathPrefix(R"(^.*/)");

}  // namespace

std::string compact_model_name_for_display(const std::string& model_name) {
    const std::string display_name = std::regex_replace(model_name, kModelPathPrefix, "");
    std::size_t position = 0;
    std::size_t ellipsis_position = 0;
    for (std::size_t count = 0;
         count < kModelDisplayCharacterLimit && position < display_name.size();
         ++count) {
        if (count + 1 == kModelDisplayCharacterLimit) {
            ellipsis_position = position;
        }
        const std::size_t next = editor::detail::next_grapheme_offset(display_name, position);
        if (next <= position) {
            break;
        }
        position = next;
    }
    if (position < display_name.size()) {
        return display_name.substr(0, ellipsis_position) + u8"…";
    }
    return display_name;
}

std::string provider_model_display_label(const std::string& provider_name,
                                         const std::string& model_name,
                                         const std::string& qualifier) {
    const std::string display_provider =
        provider_name.empty() ? "" : provider::display_name_for_profile(provider_name);
    const std::string display_model = compact_model_name_for_display(model_name);
    if (display_provider.empty() && display_model.empty()) {
        return "";
    }
    if (display_provider.empty()) {
        return "[" + display_model + (qualifier.empty() ? "" : " " + qualifier) + "]";
    }
    if (display_model.empty()) {
        return "[" + display_provider + "/model unknown" +
               (qualifier.empty() ? "" : " " + qualifier) + "]";
    }
    return "[" + display_provider + "/" + display_model +
           (qualifier.empty() ? "" : " " + qualifier) + "]";
}

std::string model_reasoning_bracket(const std::string& model_name,
                                    const std::string& reasoning) {
    if (model_name.empty()) {
        return {};
    }
    const std::string reason = reasoning.empty() ? "auto" : reasoning;
    return provider_model_display_label("", model_name, reason);
}

std::string model_status_message(const std::string& model_name, const std::string& suffix) {
    const std::string label = provider_model_display_label("", model_name);
    if (label.empty()) {
        return suffix;
    }
    if (suffix.empty()) {
        return label;
    }
    return label + " " + suffix;
}

}  // namespace ainiux::ui
