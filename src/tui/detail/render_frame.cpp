#include "tui/detail/render.hpp"
#include "tui/tui.hpp"

#include <iostream>
#include <utility>
#include <vector>

namespace pkchat::tui::detail {

editor::EditorState empty_input_editor(size_t undo_limit) {
    editor::EditorState input = editor::EditorState::from_text("");
    input.set_undo_limit(undo_limit);
    input.mode = editor::EditorMode::Chat;
    input.vertical_movement = editor::VerticalMovementMode::VisualRow;
    return input;
}

void set_status_from_error(const Error& err, std::string& status) {
    if (!err.ok()) {
        status = error_line(err);
    }
}

void insert_input(editor::EditorState& input, const std::string& text, std::string& status) {
    set_status_from_error(input.insert(text), status);
}

void render(const chat::Session& session,
            editor::EditorState& input,
            std::string& status,
            int& history_scroll,
            bool show_thinking_traces,
            TuiMode mode,
            const std::string& panel_text,
            const RenderStyle& style) {
    const TuiSize terminal = terminal_size();
    const Layout layout = layout_for_terminal(terminal.rows, terminal.cols);
    const int cols = layout.cols;

    input.ensure_cursor_visible(layout.input_rect);
    const editor::RenderedPanel input_panel = input.render(layout.input_rect);
    const bool panel_active = mode != TuiMode::Chat || !panel_text.empty();
    std::vector<StyledLine> history =
        panel_active ? panel_lines_for_text(panel_text, mode, cols)
                     : history_lines_for_session(session, cols, show_thinking_traces);
    const int max_history_scroll = std::max(0, static_cast<int>(history.size()) - layout.history_rows);
    history_scroll = std::min(std::max(0, history_scroll), max_history_scroll);

    std::cout << "\x1b[?25l";

    const int history_start = std::max(0, static_cast<int>(history.size()) - layout.history_rows - history_scroll);
    int printed = 0;
    const StyleRole history_fill_role = panel_active ? StyleRole::PanelBorder : StyleRole::Text;
    for (int i = history_start; i < static_cast<int>(history.size()) && printed < layout.history_rows; ++i, ++printed) {
        draw_line(layout.history_row + printed, cols, history[static_cast<size_t>(i)].segments, history_fill_role, style);
    }
    while (printed < layout.history_rows) {
        draw_line(layout.history_row + printed, cols, "", history_fill_role, style);
        ++printed;
    }

    draw_line(layout.status_row, cols, status, status_role_for_text(status), style);
    draw_line(layout.input_label_row,
              cols,
              "Input  Tab command/path | Enter send | Alt+Enter newline | Alt+R regen | Ctrl+U undo | Ctrl+R redo | Ctrl+Q quit",
              StyleRole::InputLabel,
              style);

    for (int row = 0; row < layout.input_rect.height; ++row) {
        const std::string line = row < static_cast<int>(input_panel.lines.size())
                                     ? input_panel.lines[static_cast<size_t>(row)]
                                     : std::string();
        draw_line(layout.input_rect.row + row, cols, line, StyleRole::Text, style);
    }

    const int cursor_row = input_panel.cursor.visible ? layout.input_rect.row + input_panel.cursor.row : layout.input_rect.row;
    const int cursor_col = input_panel.cursor.visible ? layout.input_rect.col + input_panel.cursor.col : layout.input_rect.col;
    std::cout << "\x1b[" << std::min(layout.rows, std::max(1, cursor_row)) << ";"
              << std::min(cols, std::max(1, cursor_col)) << "H\x1b[?25h";
    std::cout.flush();
}

}  // namespace pkchat::tui::detail