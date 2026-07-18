#include "ui/provider_model_selector.hpp"

#include "provider/provider.hpp"
#include "ui/text_selector.hpp"

namespace ainiux::ui {
namespace {

TextSelectorConfig standard_selector_config() {
    TextSelectorConfig config;
    config.header = kTextSelectorStandardHint;
    return config;
}

}  // namespace

std::vector<std::string> selectable_provider_ids() {
    std::vector<std::string> providers;
    for (const provider::Profile& profile : provider::built_in_profiles()) {
        if (provider::is_selectable_provider(profile)) {
            providers.push_back(profile.name);
        }
    }
    return providers;
}

std::string provider_selector_text(const std::vector<std::string>& provider_ids, size_t selected) {
    return render_text_selector(standard_selector_config(), selected, provider_ids.size(), [&](size_t index) {
        return provider::display_name_for_profile(provider_ids[index]);
    });
}

std::string model_selector_text(const std::vector<std::string>& models, size_t selected) {
    return render_text_selector(standard_selector_config(), selected, models);
}

bool should_auto_select_only_model(const std::vector<std::string>& models) {
    return models.size() == 1;
}

}  // namespace ainiux::ui
