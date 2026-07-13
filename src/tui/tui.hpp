#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "chat/session.hpp"
#include "editor/editor.hpp"
#include "provider/provider.hpp"
#include "tui/theme_types.hpp"

namespace pkchat::app {
struct TuiRunResult;
struct InteractiveSession;
}

namespace pkchat::tui {

enum class ActivityKind { None, Thinking, Streaming };

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
bool should_open_startup_provider_picker(const provider::RequestContext& context);
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

struct RegenerationPlan {
    bool available = false;
    std::size_t erase_from = 0;
    std::string prompt;
};

RegenerationPlan regeneration_plan_for_session(const chat::Session& session);
bool last_unanswered_user_message(const chat::Session& session, std::size_t& index);
bool last_editable_chat_message(const chat::Session& session, std::size_t& index);
bool pop_last_chat_message(chat::Session& session, std::string& removed_role);
pkchat::app::TuiRunResult run(provider::RequestContext context,
                              chat::Session session,
                              pkchat::app::InteractiveSession* interactive = nullptr);

}  // namespace pkchat::tui
