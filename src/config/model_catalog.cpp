#include "config/model_catalog.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <regex>
#include <sstream>

#include "ui/text_selector.hpp"

namespace ainiux::config {
namespace {

std::string ascii_lower(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::string trim_ascii(std::string text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.erase(text.begin());
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.pop_back();
    }
    return text;
}

bool valid_named_reasoning_value(const std::string& text) {
    if (text.empty() || text.size() > 64) {
        return false;
    }
    for (const unsigned char ch : text) {
        if (ch > 0x7f || !(std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.')) {
            return false;
        }
    }
    return true;
}

std::string final_model_component(const std::string& model) {
    const size_t slash = model.find_last_of('/');
    return slash == std::string::npos ? model : model.substr(slash + 1);
}

int specificity(const ModelCapability& capability) {
    return (ascii_lower(capability.provider) == "any" ? 0 : 1) +
           (ascii_lower(capability.api) == "any" ? 0 : 1);
}

bool scope_matches(const std::string& configured, const std::string& actual) {
    const std::string normalized = ascii_lower(configured);
    const std::string actual_normalized = ascii_lower(actual);
    return normalized == "any" || normalized == actual_normalized;
}

std::string format_tokens(std::int64_t tokens) {
    std::string digits = std::to_string(tokens);
    for (std::ptrdiff_t pos = static_cast<std::ptrdiff_t>(digits.size()) - 3; pos > 0; pos -= 3) {
        digits.insert(static_cast<size_t>(pos), ",");
    }
    return digits;
}

}  // namespace

Error parse_reasoning_selection(const std::string& raw,
                                ReasoningSelection& selection,
                                bool allow_auto) {
    const std::string text = trim_ascii(raw);
    if (text.empty()) {
        return {ErrorCode::BadArgs, "reasoning value must not be empty"};
    }
    if (ascii_lower(text) == "auto") {
        if (!allow_auto) {
            return {ErrorCode::BadArgs, "auto is only valid as a reasoning override"};
        }
        selection = ReasoningSelection::automatic();
        return ok_error();
    }
    bool digits = true;
    for (const unsigned char ch : text) {
        digits = digits && ch >= '0' && ch <= '9';
    }
    if (digits) {
        try {
            const unsigned long long parsed = std::stoull(text);
            if (parsed > static_cast<unsigned long long>(std::numeric_limits<std::int64_t>::max())) {
                return {ErrorCode::BadArgs, "reasoning token budget is too large"};
            }
            selection = ReasoningSelection::token_budget(static_cast<std::int64_t>(parsed));
            return ok_error();
        } catch (const std::exception&) {
            return {ErrorCode::BadArgs, "reasoning token budget is too large"};
        }
    }
    if (!valid_named_reasoning_value(text)) {
        return {ErrorCode::BadArgs,
                "reasoning value must be a non-negative integer or an ASCII token up to 64 characters"};
    }
    selection = ReasoningSelection::named(text);
    return ok_error();
}

std::string reasoning_selection_value(const ReasoningSelection& selection) {
    switch (selection.kind) {
        case ReasoningSelectionKind::Auto:
            return "auto";
        case ReasoningSelectionKind::Named:
            return selection.value;
        case ReasoningSelectionKind::TokenBudget:
            return std::to_string(selection.tokens);
    }
    return "auto";
}

std::string reasoning_selection_display(const ReasoningSelection& selection) {
    if (selection.kind == ReasoningSelectionKind::TokenBudget) {
        return format_tokens(selection.tokens) + " tokens";
    }
    return reasoning_selection_value(selection);
}

bool reasoning_selection_disables(const ReasoningSelection& selection) {
    if (selection.kind == ReasoningSelectionKind::TokenBudget) {
        return selection.tokens == 0;
    }
    if (selection.kind != ReasoningSelectionKind::Named) {
        return false;
    }
    const std::string value = ascii_lower(selection.value);
    return value == "none" || value == "off" || value == "disabled" || value == "disable" ||
           value == "false" || value == "no" || value == "no_think";
}

Error resolve_reasoning_off(const ModelCatalog& catalog,
                            const std::string& provider,
                            const std::string& api,
                            const std::string& model,
                            ReasoningSelection& selection) {
    if (selection.kind != ReasoningSelectionKind::Named ||
        ascii_lower(selection.value) != "off") {
        return ok_error();
    }
    const ModelCapability* capability =
        resolve_model_capability(catalog, provider, api, model);
    if (capability == nullptr) {
        return ok_error();
    }
    for (const ReasoningSelection& option : capability->reasoning_options) {
        if (reasoning_selection_disables(option)) {
            selection = option;
            return ok_error();
        }
    }
    std::ostringstream choices;
    for (size_t i = 0; i < capability->reasoning_options.size(); ++i) {
        if (i != 0) choices << '|';
        choices << reasoning_selection_value(capability->reasoning_options[i]);
    }
    return {ErrorCode::BadArgs,
            "reasoning off is unavailable for model '" + model +
                "'; models.conf choices: " +
                (choices.str().empty() ? std::string("none") : choices.str())};
}

bool parse_reasoning_protocol(const std::string& text, ReasoningProtocol& protocol) {
    const std::string value = ascii_lower(text);
    const std::pair<const char*, ReasoningProtocol> protocols[] = {
        {"none", ReasoningProtocol::None},
        {"generic_thinking", ReasoningProtocol::GenericThinking},
        {"openai_effort", ReasoningProtocol::OpenAiEffort},
        {"openrouter", ReasoningProtocol::OpenRouter},
        {"gemini_effort", ReasoningProtocol::GeminiEffort},
        {"gemini_thinking_level", ReasoningProtocol::GeminiThinkingLevel},
        {"gemma_thinking_level", ReasoningProtocol::GemmaThinkingLevel},
        {"anthropic_budget", ReasoningProtocol::AnthropicBudget},
        {"thinking_toggle", ReasoningProtocol::ThinkingToggle},
        {"qwen_chat", ReasoningProtocol::QwenChat},
        {"qwen_chat_effort", ReasoningProtocol::QwenChatEffort},
        {"qwen_responses", ReasoningProtocol::QwenResponses},
        {"deepseek", ReasoningProtocol::DeepSeek},
        {"kimi_effort", ReasoningProtocol::KimiEffort},
        {"zai", ReasoningProtocol::Zai},
        {"xai_effort", ReasoningProtocol::XaiEffort},
        {"minimax_responses", ReasoningProtocol::MiniMaxResponses},
        {"nemotron_template", ReasoningProtocol::NemotronTemplate},
        {"hy3_template", ReasoningProtocol::Hy3Template},
    };
    for (const auto& item : protocols) {
        if (value == item.first) {
            protocol = item.second;
            return true;
        }
    }
    return false;
}

const char* reasoning_protocol_name(ReasoningProtocol protocol) {
    switch (protocol) {
        case ReasoningProtocol::None: return "none";
        case ReasoningProtocol::GenericThinking: return "generic_thinking";
        case ReasoningProtocol::OpenAiEffort: return "openai_effort";
        case ReasoningProtocol::OpenRouter: return "openrouter";
        case ReasoningProtocol::GeminiEffort: return "gemini_effort";
        case ReasoningProtocol::GeminiThinkingLevel: return "gemini_thinking_level";
        case ReasoningProtocol::GemmaThinkingLevel: return "gemma_thinking_level";
        case ReasoningProtocol::AnthropicBudget: return "anthropic_budget";
        case ReasoningProtocol::ThinkingToggle: return "thinking_toggle";
        case ReasoningProtocol::QwenChat: return "qwen_chat";
        case ReasoningProtocol::QwenChatEffort: return "qwen_chat_effort";
        case ReasoningProtocol::QwenResponses: return "qwen_responses";
        case ReasoningProtocol::DeepSeek: return "deepseek";
        case ReasoningProtocol::KimiEffort: return "kimi_effort";
        case ReasoningProtocol::Zai: return "zai";
        case ReasoningProtocol::XaiEffort: return "xai_effort";
        case ReasoningProtocol::MiniMaxResponses: return "minimax_responses";
        case ReasoningProtocol::NemotronTemplate: return "nemotron_template";
        case ReasoningProtocol::Hy3Template: return "hy3_template";
    }
    return "none";
}

std::string reasoning_protocol_names() {
    return "none, generic_thinking, openai_effort, openrouter, gemini_effort, "
           "gemini_thinking_level, gemma_thinking_level, anthropic_budget, thinking_toggle, "
           "qwen_chat, qwen_chat_effort, qwen_responses, deepseek, kimi_effort, zai, xai_effort, "
           "minimax_responses, nemotron_template, or hy3_template";
}

bool parse_temperature_support(const std::string& text, TemperatureSupport& support) {
    const std::string value = ascii_lower(text);
    if (value == "unknown") support = TemperatureSupport::Unknown;
    else if (value == "supported") support = TemperatureSupport::Supported;
    else if (value == "unsupported") support = TemperatureSupport::Unsupported;
    else if (value == "reasoning_none_only") support = TemperatureSupport::ReasoningNoneOnly;
    else return false;
    return true;
}

const char* temperature_support_name(TemperatureSupport support) {
    switch (support) {
        case TemperatureSupport::Unknown: return "unknown";
        case TemperatureSupport::Supported: return "supported";
        case TemperatureSupport::Unsupported: return "unsupported";
        case TemperatureSupport::ReasoningNoneOnly: return "reasoning_none_only";
    }
    return "unknown";
}

bool model_regex_matches(const std::string& expression, const std::string& model) {
    try {
        const std::regex pattern(expression, std::regex::ECMAScript | std::regex::icase);
        return std::regex_match(final_model_component(model), pattern);
    } catch (const std::regex_error&) {
        return false;
    }
}

const ModelCapability* resolve_model_capability(const ModelCatalog& catalog,
                                                const std::string& provider,
                                                const std::string& api,
                                                const std::string& model) {
    const ModelCapability* best = nullptr;
    for (const ModelCapability& candidate : catalog.models) {
        if (!candidate.enabled || !scope_matches(candidate.provider, provider) ||
            !scope_matches(candidate.api, api) || !model_regex_matches(candidate.model_regex, model)) {
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

std::string reasoning_catalog_warning(const ModelCatalog& catalog,
                                      const std::string& provider,
                                      const std::string& api,
                                      const std::string& model,
                                      const ReasoningSelection& selection) {
    if (selection.is_auto()) {
        return {};
    }
    const ModelCapability* capability =
        resolve_model_capability(catalog, provider, api, model);
    if (capability == nullptr || capability->reasoning_options.empty() ||
        std::find(capability->reasoning_options.begin(),
                  capability->reasoning_options.end(),
                  selection) != capability->reasoning_options.end()) {
        return {};
    }

    std::ostringstream allowed;
    for (size_t index = 0; index < capability->reasoning_options.size(); ++index) {
        if (index != 0) allowed << '|';
        allowed << reasoning_selection_value(capability->reasoning_options[index]);
    }
    return "reasoning value '" + reasoning_selection_value(selection) +
           "' is not listed for model '" + model + "' (models.conf values: " +
           allowed.str() + "); the provider may reject it";
}

const ModelSetting* find_model_preset(const ModelCatalog& catalog,
                                      const ModelCapability& capability,
                                      const std::string& purpose) {
    for (const ModelSetting& preset : catalog.presets) {
        if (preset.enabled && preset.model_id == capability.id && preset.purpose == purpose) {
            return &preset;
        }
    }
    return nullptr;
}

double temperature_max_for(const ModelCapability* capability) {
    if (capability != nullptr && capability->temperature_max.has_value() &&
        std::isfinite(*capability->temperature_max) && *capability->temperature_max > 0.0) {
        return *capability->temperature_max;
    }
    return 1.0;
}

bool temperature_supported_for(const ModelCapability& capability,
                               const ReasoningSelection& selection) {
    if (capability.temperature == TemperatureSupport::Supported ||
        capability.temperature == TemperatureSupport::Unknown) {
        return true;
    }
    if (capability.temperature == TemperatureSupport::Unsupported) {
        return false;
    }
    ReasoningSelection effective = selection;
    if (effective.is_auto() && capability.reasoning_default.has_value()) {
        effective = *capability.reasoning_default;
    }
    return reasoning_selection_disables(effective);
}

std::string temperature_advisory(const ModelCapability* capability,
                                 const ReasoningSelection& selection,
                                 bool explicit_temperature) {
    if (!explicit_temperature || capability == nullptr ||
        temperature_supported_for(*capability, selection)) {
        return {};
    }
    if (capability->temperature == TemperatureSupport::ReasoningNoneOnly) {
        return "temperature may be rejected for this model unless reasoning=none";
    }
    return "temperature may be rejected for this model";
}

ReasoningSelectorData reasoning_selector_data(const ModelCatalog& catalog,
                                              const std::string& provider,
                                              const std::string& api,
                                              const std::string& model) {
    ReasoningSelectorData data;
    const ModelCapability* capability =
        resolve_model_capability(catalog, provider, api, model);
    if (capability == nullptr) {
        data.guidance = "No reasoning catalog entry matches " + model +
                        ". Use /reasoning VALUE directly or add an entry to models.conf.";
        return data;
    }
    data.values.push_back(ReasoningSelection::automatic());
    std::string automatic = "Auto";
    if (capability->reasoning_default.has_value()) {
        automatic += " (provider default: " +
                     reasoning_selection_display(*capability->reasoning_default) + ")";
    }
    data.labels.push_back(std::move(automatic));
    for (const ReasoningSelection& option : capability->reasoning_options) {
        data.values.push_back(option);
        data.labels.push_back(reasoning_selection_value(option));
    }
    return data;
}

bool next_reasoning_selection(const ModelCatalog& catalog,
                              const std::string& provider,
                              const std::string& api,
                              const std::string& model,
                              const ReasoningSelection& current,
                              ReasoningSelection& next) {
    if (model.empty()) return false;
    const ModelCapability* capability =
        resolve_model_capability(catalog, provider, api, model);
    if (capability == nullptr || capability->reasoning_options.empty()) return false;

    if (capability->reasoning_protocol == ReasoningProtocol::QwenChat ||
        capability->reasoning_protocol == ReasoningProtocol::GemmaThinkingLevel) {
        const bool currently_disabled = reasoning_selection_disables(current);
        next = ReasoningSelection::named(currently_disabled ? "enabled" : "none");
        return true;
    }

    ReasoningSelectorData data =
        reasoning_selector_data(catalog, provider, api, model);
    if (data.values.size() < 2) return false;
    for (size_t i = 0; i < data.values.size(); ++i) {
        if (data.values[i] == current) {
            next = data.values[(i + 1) % data.values.size()];
            return true;
        }
    }
    next = data.values.front();
    return true;
}

std::string reasoning_selector_text(const ModelCatalog& catalog,
                                    const std::string& provider,
                                    const std::string& api,
                                    const std::string& model,
                                    size_t selected,
                                    std::vector<ReasoningSelection>* selections) {
    ReasoningSelectorData data = reasoning_selector_data(catalog, provider, api, model);
    if (!data.guidance.empty()) {
        if (selections != nullptr) selections->clear();
        return data.guidance;
    }
    if (selections != nullptr) *selections = data.values;
    ui::TextSelectorConfig selector;
    selector.header = ui::kTextSelectorStandardHint;
    return ui::render_text_selector(selector, selected, data.labels);
}

}  // namespace ainiux::config
