#include "tui/tui.hpp"

#include "ainiux/version.hpp"
#include "context/context.hpp"
#include "provider/provider.hpp"
#include "ui/provider_model_display.hpp"

#include <iomanip>
#include <sstream>

namespace ainiux::tui {
namespace {

std::string provider_model_status_label(const std::string& provider_name, const std::string& model_name) {
    return ui::provider_model_display_label(provider_name, model_name);
}

std::string provider_model_status_label(const provider::RequestContext& context) {
    return provider_model_status_label(context.profile.name, context.options.model);
}

std::string provider_model_status_message(const std::string& label, const std::string& suffix) {
    if (label.empty()) {
        return suffix;
    }
    return label + " " + suffix;
}

std::string provider_model_status_message(const std::string& label,
                                          const std::string& indicator,
                                          const std::string& suffix) {
    if (label.empty()) {
        return indicator + suffix;
    }
    return label + " " + indicator + suffix;
}

}  // namespace

std::string provider_model_status_message(const provider::RequestContext& context, const std::string& suffix) {
    return provider_model_status_message(provider_model_status_label(context), suffix);
}

std::string provider_model_status_message(const provider::RequestContext& context,
                                          const std::string& indicator,
                                          const std::string& suffix) {
    return provider_model_status_message(provider_model_status_label(context), indicator, suffix);
}

std::string ready_status() {
    return "Tab complete | Ctrl+Space continue | Alt+Enter newline";
}

std::string sqlite_unavailable_status(const std::string& reason) {
    if (reason.empty()) {
        return "Saved chat database unavailable; move ~/.ainiux/ainiux.db aside and restart ainiux";
    }
    return "Saved chat database unavailable: " + reason;
}

bool chat_provider_model_ready(const provider::RequestContext& context) {
    return !context.profile.offline && !context.profile.name.empty() &&
           !context.options.model.empty();
}

std::string chat_provider_model_required_status(const provider::RequestContext& context,
                                                bool require_provider_selection) {
    if (require_provider_selection || context.profile.offline || context.profile.name.empty()) {
        return "Setup required: /provider, then /model · sending disabled";
    }
    if (context.options.model.empty()) {
        return "Setup required: /model · sending disabled";
    }
    return "";
}

std::string chat_startup_status(const provider::RequestContext& context) {
    if (context.profile.offline) {
        return "Offline · /list browse · /provider then /model to enable sending";
    }
    if (context.options.model.empty()) {
        return "Choose a model with /model · Change provider with /provider";
    }
    // Keep this short enough that "/list" remains visible on an 80-column status line
    // after the "[provider/model] " prefix (integration tests assert on that hint).
    return provider_model_status_message(context, "ready · /provider · /list");
}

std::string generation_ready_status(const std::string& provider_name,
                                    const std::string& model_name,
                                    const provider::ChatResult& result,
                                    bool stream,
                                    const std::vector<provider::Message>& messages,
                                    long long context_tokens) {
    std::ostringstream out;
    const std::string label = provider_model_status_label(provider_name, model_name);
    if (label.empty()) {
        out << ready_status();
    } else {
        out << label;
    }
    if (stream) {
        if (context_tokens > 0) {
            out << " | TTFT " << result.ttft_ms << "ms";
        } else {
            out << " | TTFT: " << result.ttft_ms << " ms";
        }
    } else {
        if (context_tokens > 0) {
            out << " | Response " << result.total_ms << "ms";
        } else {
            out << " | Response: " << result.total_ms << " ms";
        }
    }
    out << " | ";
    if (result.completion_tokens_estimated) {
        out << "~";
    }
    out << std::fixed << std::setprecision(1) << provider::tokens_per_second(result, stream)
        << " token/s";
    const std::string context_usage =
        context::format_context_usage(context::estimated_usage_tokens(messages, result), context_tokens);
    if (!context_usage.empty()) {
        out << " | context: " << context_usage;
    }
    return out.str();
}

long long effective_agent_context_window(long long context_tokens) {
    // Display-only fallback when CLI/config and /v1/models have not supplied a window.
    // Does not mutate Options.context_tokens (auto-compact keeps using the real value).
    if (context_tokens > 0) {
        return context_tokens;
    }
    return kDefaultAgentContextWindowTokens;
}

std::string format_agent_context_usage(long long used_tokens, long long window_tokens) {
    const long long window = effective_agent_context_window(window_tokens);
    const long long used = used_tokens < 0 ? 0 : used_tokens;
    std::ostringstream out;
    out << used << " tok (";
    // One decimal percent; round half away from zero for positive values.
    const double percent =
        window > 0 ? (static_cast<double>(used) * 100.0) / static_cast<double>(window) : 0.0;
    out << std::fixed << std::setprecision(1) << percent << "%)";
    return out.str();
}

std::string agent_provider_model_reasoning_label(const std::string& provider_name,
                                                 const std::string& model_name,
                                                 const std::string& reasoning) {
    const std::string reason = reasoning.empty() ? "auto" : reasoning;
    return ui::provider_model_display_label(provider_name, model_name, reason);
}

std::string agent_input_label_text(const std::string& provider_name,
                                   const std::string& model_name,
                                   const std::string& reasoning,
                                   long long used_tokens,
                                   long long window_tokens) {
    std::ostringstream out;
    out << app_version_label() << " "
        << agent_provider_model_reasoning_label(provider_name, model_name, reasoning) << " "
        << format_agent_context_usage(used_tokens, window_tokens);
    return out.str();
}

std::string agent_ready_status() {
    return "agent ready";
}

RegenerationPlan regeneration_plan_for_session(const chat::Session& session) {
    for (std::size_t i = session.messages.size(); i > 0; --i) {
        const std::size_t index = i - 1;
        if (session.messages[index].role == "user") {
            return {true, index, session.messages[index].content};
        }
    }
    return {};
}

bool last_unanswered_user_message(const chat::Session& session, std::size_t& index) {
    for (std::size_t i = session.messages.size(); i > 0; --i) {
        const std::size_t candidate = i - 1;
        const std::string& role = session.messages[candidate].role;
        if (role == "assistant") {
            index = 0;
            return false;
        }
        if (role == "user") {
            index = candidate;
            return true;
        }
    }
    index = 0;
    return false;
}

bool last_editable_chat_message(const chat::Session& session, std::size_t& index) {
    for (std::size_t i = session.messages.size(); i > 0; --i) {
        const std::size_t candidate = i - 1;
        const std::string& role = session.messages[candidate].role;
        if (role == "user" || role == "assistant") {
            index = candidate;
            return true;
        }
    }
    index = 0;
    return false;
}

bool pop_last_chat_message(chat::Session& session, std::string& removed_role) {
    for (std::size_t i = session.messages.size(); i > 0; --i) {
        const std::size_t index = i - 1;
        const std::string& role = session.messages[index].role;
        if (role != "user" && role != "assistant") {
            continue;
        }
        removed_role = role;
        session.messages.erase(session.messages.begin() + static_cast<long>(index));
        if (removed_role == "assistant") {
            session.usage_json = "{}";
        }
        return true;
    }
    removed_role.clear();
    return false;
}

}  // namespace ainiux::tui
