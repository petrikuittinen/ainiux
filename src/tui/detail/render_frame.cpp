#include "tui/activity.hpp"
#include "tui/agent_widgets.hpp"
#include "tui/detail/render.hpp"
#include "tui/tui.hpp"



#include <iostream>
#include <iterator>
#include <utility>
#include <vector>

namespace ainiux::tui::detail {

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
            ActivityKind activity_kind,
            size_t activity_frame,
            bool syntax_highlight,
            const RenderStyle& style,
            const char* panel_title_override,
            bool agent_mode,
            const AgentChrome& agent_chrome) {
    const TuiSize terminal = terminal_size();
    Layout layout = layout_for_terminal(terminal.rows, terminal.cols);
    if (agent_mode) {
        const int percentage_cap =
            std::max(3, (std::max(5, terminal.rows) *
                         std::min(80, std::max(10, agent_chrome.input_max_height_percent))) /
                            100);
        const size_t measured = input.visual_row_count_bounded(
            static_cast<size_t>(std::max(1, terminal.cols - 2)),
            static_cast<size_t>(std::max(1, percentage_cap - 2)));
        const AgentInputGeometry geometry =
            agent_input_geometry(terminal.rows, terminal.cols, measured,
                                 agent_chrome.input_max_height_percent);
        layout = layout_for_agent_terminal(
            terminal.rows, terminal.cols, geometry.box_height);
    }
    const int cols = layout.cols;

    input.ensure_cursor_visible(layout.input_rect);
    const editor::RenderedPanel input_panel = input.render(layout.input_rect);
    const InlineChoiceModel inline_choices =
        agent_mode ? agent_inline_choices_for_mode(mode) : InlineChoiceModel{};
    const bool agent_choice_active = valid_inline_choices(inline_choices);
    const bool panel_active =
        (mode != TuiMode::Chat || !panel_text.empty()) && !agent_choice_active;
    std::vector<StyledLine> history =
        panel_active ? panel_lines_for_text(panel_text, mode, cols, panel_title_override)
                     : history_lines_for_session(session, cols, show_thinking_traces, activity_kind,
                                                 activity_frame, syntax_highlight, agent_mode);
    if (agent_choice_active && !panel_text.empty()) {
        std::vector<StyledLine> details =
            panel_lines_for_text(panel_text, mode, cols, panel_title_override);
        history.insert(history.end(),
                       std::make_move_iterator(details.begin()),
                       std::make_move_iterator(details.end()));
    }
    const bool picker_top_aligned = mode == TuiMode::ThreadList || mode == TuiMode::ProviderList ||
                                    mode == TuiMode::ModelList || mode == TuiMode::ReasoningList;
    const int max_history_scroll = std::max(0, static_cast<int>(history.size()) - layout.history_rows);
    history_scroll = std::min(std::max(0, history_scroll), max_history_scroll);

    if (picker_top_aligned) {
        int highlighted_line = -1;
        for (size_t i = 0; i < history.size(); ++i) {
            if (!history[i].segments.empty() &&
                history[i].segments.front().role == StyleRole::PanelHighlight) {
                highlighted_line = static_cast<int>(i);
                break;
            }
        }
        if (highlighted_line >= 0) {
            if (highlighted_line < history_scroll) {
                history_scroll = highlighted_line;
            } else if (highlighted_line >= history_scroll + layout.history_rows) {
                history_scroll = highlighted_line - layout.history_rows + 1;
            }
        }
        history_scroll = std::min(std::max(0, history_scroll), max_history_scroll);
    }

    std::cout << "\x1b[?25l";

    const int history_start =
        picker_top_aligned
            ? history_scroll
            : std::max(0, static_cast<int>(history.size()) - layout.history_rows - history_scroll);
    int printed = 0;
    const StyleRole history_fill_role = panel_active ? StyleRole::PanelBorder : StyleRole::Text;
    for (int i = history_start; i < static_cast<int>(history.size()) && printed < layout.history_rows; ++i, ++printed) {
        draw_line(layout.history_row + printed, cols, history[static_cast<size_t>(i)].segments, history_fill_role, style);
    }
    while (printed < layout.history_rows) {
        draw_line(layout.history_row + printed, cols, "", history_fill_role, style);
        ++printed;
    }

    if (agent_mode) {
        draw_line(layout.activity_row, cols,
                  agent_activity_line(agent_chrome.activity_state,
                                      agent_chrome.cancellable,
                                      agent_chrome.task_elapsed_seconds,
                                      agent_chrome.completed_task_ms,
                                      cols),
                  StyleRole::Status, style);
        if (agent_choice_active) {
            draw_line(layout.status_row, cols, render_inline_choices(inline_choices),
                      StyleRole::Status, style);
        } else {
            draw_line(layout.status_row, cols,
                      agent_status_line(agent_chrome.model,
                                        agent_chrome.reasoning,
                                        agent_chrome.used_tokens,
                                        agent_chrome.window_tokens,
                                        cols),
                      status_role_for_text(status), style);
        }
    } else if (show_activity_on_status_row(activity_kind, agent_mode) &&
        mode == TuiMode::Chat && !panel_active) {
        const std::string label = session_status_label(session);
        const std::string suffix =
            activity_kind == ActivityKind::Thinking ? "thinking..." : "streaming response ...";
        draw_line(layout.status_row, cols,
                  activity_status_segments(label, activity_kind, activity_frame, suffix),
                  StyleRole::Status, style);
    } else {
        const std::string displayed_status = session.read_only ? "[RO] " + status : status;
        draw_line(layout.status_row, cols, displayed_status,
                  status_role_for_text(displayed_status), style);
    }
    if (agent_mode) {
        draw_line(layout.input_label_row, cols,
                  agent_input_top_border(
                      {agent_chrome.workspace, agent_chrome.mode_label,
                       agent_chrome.permission_label},
                      cols),
                  StyleRole::PanelBorder, style);
    } else {
        draw_line(layout.input_label_row, cols,
                  input_label_segments_for_mode(false, agent_chrome),
                  StyleRole::InputLabel, style);
    }

    for (int row = 0; row < layout.input_rect.height; ++row) {
        const size_t index = static_cast<size_t>(row);
        const std::string line = row < static_cast<int>(input_panel.lines.size())
                                     ? input_panel.lines[index]
                                     : std::string();
        std::vector<StyledSegment> segments;
        size_t pos = 0;
        if (index < input_panel.line_spans.size()) {
            for (const editor::RenderedPanel::Span& span : input_panel.line_spans[index]) {
                const size_t start = std::min(span.start, line.size());
                const size_t end = std::min(span.end, line.size());
                if (start > pos) {
                    segments.push_back({line.substr(pos, start - pos), StyleRole::Text, false});
                }
                if (end > start) {
                    segments.push_back({line.substr(start, end - start),
                                        span.syntax ? style_role_for_token(span.role) : StyleRole::Text,
                                        span.selected,
                                        span.syntax ? text_attributes_for_token(span.role)
                                                    : TextAttributes{}});
                }
                pos = std::max(pos, end);
            }
        }
        if (pos < line.size()) {
            segments.push_back({line.substr(pos), StyleRole::Text, false});
        }
        if (agent_mode) {
            std::vector<StyledSegment> framed;
            framed.push_back({u8"│", StyleRole::PanelBorder});
            framed.insert(framed.end(), segments.begin(), segments.end());
            framed.push_back({u8"│", StyleRole::PanelBorder});
            draw_line(layout.input_rect.row + row, cols, framed, StyleRole::Text, style);
        } else {
            draw_line(layout.input_rect.row + row, cols, segments, StyleRole::Text, style);
        }
    }
    if (agent_mode) {
        draw_line(layout.input_rect.row + layout.input_rect.height, cols,
                  agent_input_bottom_border(cols), StyleRole::PanelBorder, style);
    }

    const int cursor_row = input_panel.cursor.visible ? layout.input_rect.row + input_panel.cursor.row : layout.input_rect.row;
    const int cursor_col = input_panel.cursor.visible ? layout.input_rect.col + input_panel.cursor.col : layout.input_rect.col;
    std::cout << "\x1b[" << std::min(layout.rows, std::max(1, cursor_row)) << ";"
              << std::min(cols, std::max(1, cursor_col)) << "H\x1b[?25h";
    std::cout.flush();
}

}  // namespace ainiux::tui::detail
