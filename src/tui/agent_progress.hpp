#pragma once

#include <cstddef>
#include <vector>

#include "agent/activity.hpp"
#include "chat/session.hpp"

namespace ainiux::tui {

struct AgentLiveRow {
    agent::AgentProgressKind kind = agent::AgentProgressKind::Notice;
    std::size_t round_id = 0;
    std::size_t tool_id = 0;
    std::size_t message_index = 0;
};

void apply_agent_progress_update(chat::Session& session,
                                 std::vector<AgentLiveRow>& rows,
                                 const agent::AgentProgressUpdate& update);

void adjust_agent_live_rows_after_erase(std::vector<AgentLiveRow>& rows,
                                        std::size_t erased_message_index);

}  // namespace ainiux::tui
