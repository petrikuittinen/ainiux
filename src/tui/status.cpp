#include "tui/tui.hpp"

#include "context/context.hpp"
#include "pkchat/version.hpp"
#include "provider/provider.hpp"

#include <iomanip>
#include <sstream>

namespace pkchat::tui {
namespace {

std::string provider_model_status_label(const std::string& provider_name, const std::string& model_name) {
    const std::string display_provider =
        provider_name.empty() ? "" : provider::display_name_for_profile(provider_name);
    if (display_provider.empty() && model_name.empty()) {
        return "";
    }
    if (display_provider.empty()) {
        return "[" + model_name + "]";
    }
    if (model_name.empty()) {
        return "[" + display_provider + " / model unknown]";
    }
    return "[" + display_provider + " / " + model_name + "]";
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
    return std::string("pkchat v") + kVersion +
           ". TAB command/path /help Alt+enter newline Ctrl+B/D scroll chat Alt+Home/End jump";
}

std::string sqlite_unavailable_status(const std::string& reason) {
    if (reason.empty()) {
        return "Saved chat database unavailable; move ~/.pkchat/pkchat.db aside and restart pkchat";
    }
    return "Saved chat database unavailable: " + reason;
}

std::string chat_startup_status(const provider::RequestContext& context) {
    if (context.profile.offline) {
        return "Select a provider with /provider, then choose a model with /model";
    }
    if (context.options.model.empty()) {
        return "Choose a model with /model · Change provider with /provider";
    }
    return "Change provider with /provider · Open saved threads with /list";
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
    if (context_tokens > 0) {
        out << " | ";
        if (result.completion_tokens_estimated) {
            out << "~";
        }
        out << std::fixed << std::setprecision(1) << provider::tokens_per_second(result, stream) << " tok/s";
    } else {
        out << " | Token/s: " << std::fixed << std::setprecision(1)
            << provider::tokens_per_second(result, stream);
        if (result.completion_tokens_estimated) {
            out << " (estimated)";
        }
    }
    if (context_tokens > 0) {
        const long long locally_estimated = context::estimated_text_tokens(messages);
        const long long reported = provider::reported_total_tokens(result);
        const long long used = std::max(locally_estimated, reported);
        const double percentage = static_cast<double>(used) * 100.0 / static_cast<double>(context_tokens);
        out << " | Context used: " << used << "/" << context_tokens << " (" << std::fixed
            << std::setprecision(1) << percentage << "%)";
    }
    return out.str();
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

}  // namespace pkchat::tui
