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

}  // namespace ainiux::tui
