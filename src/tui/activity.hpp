#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "chat/session.hpp"
#include "tui/tui.hpp"

namespace ainiux::tui {

const char* input_label_status_message();
std::string input_label_text();
std::string input_label_text_for_mode(bool agent_mode);
std::vector<StyledSegment> input_label_segments();
std::vector<StyledSegment> input_label_segments_for_mode(bool agent_mode);
std::vector<StyledSegment> input_label_segments_for_mode(bool agent_mode, const AgentChrome& chrome);

std::string activity_indicator_text(ActivityKind kind, size_t frame);
size_t activity_indicator_width(ActivityKind kind);
StyleRole activity_indicator_role(ActivityKind kind);

std::vector<StyledSegment> activity_status_segments(const std::string& label,
                                                    ActivityKind kind,
                                                    size_t frame,
                                                    const std::string& suffix);

std::vector<StyledSegment> activity_placeholder_segments(const std::string& label,
                                                         ActivityKind kind,
                                                         size_t frame,
                                                         const std::string& suffix);

ActivityKind activity_kind_for_pending_assistant(const chat::Session& session,
                                                 size_t pending_assistant_index,
                                                 bool show_thinking_traces);

std::string session_status_label(const chat::Session& session);

}  // namespace ainiux::tui