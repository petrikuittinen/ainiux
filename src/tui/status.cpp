#include "tui/tui.hpp"

#include "context/context.hpp"
#include "pkchat/version.hpp"
#include "provider/provider.hpp"

#include <iomanip>
#include <sstream>

namespace pkchat::tui {
namespace {

std::string provider_model_status_label(const std::string& provider_name, const std::string& model_name) {
    if (provider_name.empty() && model_name.empty()) {
        return "";
    }
    if (provider_name.empty()) {
        return "[" + model_name + "]";
    }
    if (model_name.empty()) {
        return "[" + provider_name + " / model unknown]";
    }
    return "[" + provider_name + " / " + model_name + "]";
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

}  // namespace

std::string provider_model_status_message(const provider::RequestContext& context, const std::string& suffix) {
    return provider_model_status_message(provider_model_status_label(context), suffix);
}

std::string ready_status() {
    return std::string("Pkchat v") + kVersion + " ready";
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
        out << (context_tokens > 0 ? std::string("Pkchat v") + kVersion + " ready" : ready_status());
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

}  // namespace pkchat::tui