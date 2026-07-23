#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "chat/session.hpp"
#include "editor/editor.hpp"
#include "provider/provider.hpp"
#include "tui/theme_types.hpp"

namespace ainiux::app {
struct TuiRunResult;
struct InteractiveSession;
}

namespace ainiux::tui {

enum class ActivityKind { None, Thinking, Streaming };

struct ChatAttachment {
    std::string source;  // original path or URL string for display in ---marker---
    provider::TextAttachment attachment;
};

struct Layout {
    int rows = 24;
    int cols = 80;
    int header_rows = 0;
    int history_row = 1;
    int history_rows = 18;
    int status_row = 19;
    int input_label_row = 20;
    editor::Rect input_rect{21, 1, 4, 80};
};

Layout layout_for_terminal(int rows, int cols);

struct StyledSegment {
    std::string text;
    StyleRole role = StyleRole::Text;
    bool reverse = false;
    TextAttributes attributes;

    StyledSegment() = default;
    StyledSegment(std::string value,
                  StyleRole value_role,
                  bool value_reverse = false,
                  TextAttributes value_attributes = {})
        : text(std::move(value)),
          role(value_role),
          reverse(value_reverse),
          attributes(value_attributes) {}
};

struct StyledLine {
    std::vector<StyledSegment> segments;
};

double contrast_ratio(Rgb foreground, Rgb background);
int history_scroll_for_thread_beginning();
int history_scroll_for_thread_end();

struct ThinkingDisplay {
    std::string text;
    bool saw_thinking_tag = false;
    bool open_thinking_tag = false;
};

ThinkingDisplay thinking_display_text(const std::string& content, bool show_traces);
std::string ready_status();
std::string sqlite_unavailable_status(const std::string& reason);
bool chat_provider_model_ready(const provider::RequestContext& context);
std::string chat_provider_model_required_status(const provider::RequestContext& context,
                                                bool require_provider_selection = false);
std::string chat_startup_status(const provider::RequestContext& context);
std::string provider_model_status_message(const provider::RequestContext& context, const std::string& suffix);
std::string provider_model_status_message(const provider::RequestContext& context,
                                          const std::string& indicator,
                                          const std::string& suffix);
std::string generation_ready_status(const std::string& provider_name,
                                    const std::string& model_name,
                                    const provider::ChatResult& result,
                                    bool stream,
                                    const std::vector<provider::Message>& messages,
                                    long long context_tokens);

// Agent-mode permanent chrome (input label). Display-only window default is 256k
// (262144); does not write into Options.context_tokens.
inline constexpr long long kDefaultAgentContextWindowTokens = 262144LL;

long long effective_agent_context_window(long long context_tokens);
std::string format_agent_context_usage(long long used_tokens, long long window_tokens);
std::string agent_provider_model_reasoning_label(const std::string& provider_name,
                                                 const std::string& model_name,
                                                 const std::string& reasoning);
std::string agent_input_label_text(const std::string& provider_name,
                                   const std::string& model_name,
                                   const std::string& reasoning,
                                   long long used_tokens,
                                   long long window_tokens);
std::string agent_ready_status();

struct AgentChrome {
    bool enabled = false;
    std::string provider;
    std::string model;
    std::string reasoning;
    long long used_tokens = 0;
    long long window_tokens = 0;
};

struct RegenerationPlan {
    bool available = false;
    std::size_t erase_from = 0;
    std::string prompt;
};

RegenerationPlan regeneration_plan_for_session(const chat::Session& session);
bool last_unanswered_user_message(const chat::Session& session, std::size_t& index);
bool last_editable_chat_message(const chat::Session& session, std::size_t& index);
bool pop_last_chat_message(chat::Session& session, std::string& removed_role);
ainiux::app::TuiRunResult run(provider::RequestContext context,
                              chat::Session session,
                              ainiux::app::InteractiveSession* interactive = nullptr);

}  // namespace ainiux::tui
