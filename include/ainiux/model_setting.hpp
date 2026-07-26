#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ainiux {

enum class ReasoningSelectionKind {
    Auto,
    Named,
    TokenBudget,
};

struct ReasoningSelection {
    ReasoningSelectionKind kind = ReasoningSelectionKind::Auto;
    std::string value;
    std::int64_t tokens = 0;

    static ReasoningSelection automatic() { return {}; }
    static ReasoningSelection named(std::string name) {
        ReasoningSelection selection;
        selection.kind = ReasoningSelectionKind::Named;
        selection.value = std::move(name);
        return selection;
    }
    static ReasoningSelection token_budget(std::int64_t count) {
        ReasoningSelection selection;
        selection.kind = ReasoningSelectionKind::TokenBudget;
        selection.tokens = count;
        return selection;
    }

    bool is_auto() const { return kind == ReasoningSelectionKind::Auto; }
};

inline bool operator==(const ReasoningSelection& lhs, const ReasoningSelection& rhs) {
    return lhs.kind == rhs.kind && lhs.value == rhs.value && lhs.tokens == rhs.tokens;
}

inline bool operator!=(const ReasoningSelection& lhs, const ReasoningSelection& rhs) {
    return !(lhs == rhs);
}

enum class ReasoningProtocol {
    None,
    GenericThinking,
    OpenAiEffort,
    OpenRouter,
    GeminiEffort,
    GeminiThinkingLevel,
    GemmaThinkingLevel,
    AnthropicBudget,
    ThinkingToggle,
    QwenChat,
    QwenResponses,
    DeepSeek,
    KimiEffort,
    Zai,
    XaiEffort,
    MiniMaxResponses,
    NemotronTemplate,
    Hy3Template,
};

enum class TemperatureSupport {
    Unknown,
    Supported,
    Unsupported,
    ReasoningNoneOnly,
};

struct ModelCapability {
    std::string id;
    std::string provider = "any";
    std::string api = "any";
    std::string model_regex;
    std::optional<std::int64_t> context_window_tokens;
    int priority = 0;
    ReasoningProtocol reasoning_protocol = ReasoningProtocol::None;
    std::optional<ReasoningSelection> reasoning_default;
    std::vector<ReasoningSelection> reasoning_options;
    TemperatureSupport temperature = TemperatureSupport::Unknown;
    bool enabled = true;
    std::uint64_t load_order = 0;
};

// Purpose presets refer to a catalog model id. All generation values are
// optional so a preset never needs to invent a parameter unsupported by a
// model.
struct ModelSetting {
    std::string model_id;
    std::string purpose;
    std::optional<std::string> default_system_prompt;
    std::optional<double> temperature;
    std::optional<int> top_k;
    std::optional<double> top_p;
    std::optional<double> min_p;
    std::optional<double> repeat_penalty;
    std::optional<double> presence_penalty;
    std::optional<ReasoningSelection> reasoning;
    bool enabled = true;
};

struct ModelCatalog {
    std::vector<ModelCapability> models;
    std::vector<ModelSetting> presets;
    std::uint64_t next_load_order = 1;
};

}  // namespace ainiux
