#include "tui/tui.hpp"

namespace ainiux::tui {

Layout layout_for_terminal(int terminal_rows, int terminal_cols) {
    Layout layout;
    layout.rows = std::max(8, terminal_rows);
    layout.cols = std::max(20, terminal_cols);
    layout.header_rows = 0;

    const int fixed_without_input = layout.header_rows + 1 + 1 + 1;
    const int max_input_height = std::max(1, layout.rows - fixed_without_input);
    int input_height = std::max(3, layout.rows / 5);
    input_height = std::min(input_height, max_input_height);

    layout.history_row = layout.header_rows + 1;
    layout.history_rows = std::max(1, layout.rows - layout.header_rows - 1 - 1 - input_height);
    layout.status_row = layout.history_row + layout.history_rows;
    layout.input_label_row = layout.status_row + 1;
    layout.input_rect = editor::Rect{layout.input_label_row + 1, 1, input_height, layout.cols};
    return layout;
}

Layout layout_for_agent_terminal(int terminal_rows, int terminal_cols, int framed_input_height) {
    Layout layout;
    layout.rows = std::max(6, terminal_rows);
    layout.cols = std::max(4, terminal_cols);
    layout.header_rows = 0;
    const int box_height =
        std::max(3, std::min(framed_input_height, layout.rows - 3));
    layout.history_row = 1;
    layout.history_rows = std::max(1, layout.rows - box_height - 2);
    layout.activity_row = layout.history_row + layout.history_rows;
    layout.status_row = layout.activity_row + 1;
    layout.input_label_row = layout.status_row + 1;  // framed top border
    layout.input_rect = {
        layout.input_label_row + 1, 2, box_height - 2, std::max(1, layout.cols - 2)};
    return layout;
}

void apply_agent_project_history_handoff(
    chat::Session& session,
    std::vector<provider::Message>& previous_history,
    std::vector<provider::Message> replacement_history,
    bool replacement_succeeded) {
    if (replacement_succeeded || !replacement_history.empty()) {
        session.messages = std::move(replacement_history);
    } else {
        session.messages = std::move(previous_history);
    }
    previous_history.clear();
}

}  // namespace ainiux::tui
