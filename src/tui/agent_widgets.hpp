#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "editor/editor.hpp"
#include "tui/events.hpp"
#include "tui/tui.hpp"

namespace ainiux::tui {

struct InlineChoice {
    std::string label;
    char mnemonic = '\0';
};

struct InlineChoiceModel {
    std::vector<InlineChoice> choices;
    size_t escape_choice = 0;
};

struct InlineChoiceResult {
    bool matched = false;
    size_t index = 0;
};

bool valid_inline_choices(const InlineChoiceModel& model, std::string* reason = nullptr);
std::string render_inline_choices(const InlineChoiceModel& model);
InlineChoiceResult parse_inline_choice_key(const InlineChoiceModel& model, unsigned char key);
InlineChoiceModel agent_inline_choices_for_mode(TuiMode mode);

struct AgentInputFrame {
    std::string workspace;
    std::string mode_label = "act";
    std::string permission_label = "smart";
    std::string credit_label;
    // When true, the top-right chrome shows "index" between permission and credits.
    bool index_enabled = false;
};

struct AgentInputGeometry {
    int box_height = 3;
    editor::Rect content_rect{1, 2, 1, 1};
};

AgentInputGeometry agent_input_geometry(int terminal_rows,
                                        int terminal_cols,
                                        size_t measured_visual_rows,
                                        int max_height_percent);
std::string abbreviate_agent_workspace(const std::string& workspace);
std::string agent_input_title(const AgentInputFrame& frame, int available_cells);
std::string agent_input_top_border(const AgentInputFrame& frame, int cols);
std::string agent_input_bottom_border(int cols);

std::string agent_status_line(const std::string& model_name,
                              const std::string& reasoning,
                              long long used_tokens,
                              long long window_tokens,
                              int cols);
std::string agent_activity_line(AgentActivityState state,
                                bool cancellable,
                                long long elapsed_seconds,
                                long long completed_task_ms,
                                int cols,
                                double completed_decode_tokens_per_second = -1.0,
                                bool completed_tokens_estimated = false);

}  // namespace ainiux::tui
