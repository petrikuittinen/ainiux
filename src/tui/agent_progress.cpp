#include "tui/agent_progress.hpp"

#include <algorithm>

namespace ainiux::tui {

void adjust_agent_live_rows_after_erase(std::vector<AgentLiveRow>& rows,
                                        std::size_t erased_message_index) {
    for (AgentLiveRow& row : rows)
        if (row.message_index > erased_message_index) --row.message_index;
}

void apply_agent_progress_update(chat::Session& session,
                                 std::vector<AgentLiveRow>& rows,
                                 const agent::AgentProgressUpdate& update) {
    auto row = std::find_if(rows.begin(), rows.end(), [&](const AgentLiveRow& candidate) {
        return candidate.kind == update.kind && candidate.round_id == update.round_id &&
               candidate.tool_id == update.tool_id;
    });
    if (update.action == agent::AgentProgressAction::Discard) {
        if (row == rows.end() || row->message_index >= session.messages.size()) return;
        const std::size_t erased = row->message_index;
        session.messages.erase(session.messages.begin() + static_cast<std::ptrdiff_t>(erased));
        rows.erase(row);
        adjust_agent_live_rows_after_erase(rows, erased);
        return;
    }

    const std::string role =
        update.kind == agent::AgentProgressKind::Thinking
            ? "thinking"
            : update.kind == agent::AgentProgressKind::Tool
                  ? "tool"
                  : update.kind == agent::AgentProgressKind::Response ? "assistant" : "notice";
    if (row == rows.end()) {
        provider::Message message{role, update.text};
        message.created_at_ms = update.created_at_ms;
        session.messages.push_back(std::move(message));
        rows.push_back({update.kind, update.round_id, update.tool_id,
                        session.messages.size() - 1});
        return;
    }
    if (row->message_index >= session.messages.size()) return;
    provider::Message& message = session.messages[row->message_index];
    if (update.action == agent::AgentProgressAction::Append)
        message.content += update.text;
    else
        message.content = update.text;
    if (update.created_at_ms > 0) message.created_at_ms = update.created_at_ms;
}

}  // namespace ainiux::tui
