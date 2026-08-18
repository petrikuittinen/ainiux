#pragma once

#include <string>
#include <vector>

#include "chat/session.hpp"
#include "common.hpp"
#include "editor/editor.hpp"

#include "tui/events.hpp"
#include "tui/detail/frame_buffer.hpp"
#include "tui/theme_registry.hpp"
#include "tui/tui.hpp"

namespace ainiux::tui::detail {

struct RenderStyle {
    const ThemeRegistry* themes = nullptr;
    std::string theme_name = "dark";
    bool colors = true;
    ColorMode color_mode = ColorMode::Truecolor;
};

struct TuiSize {
    int rows = 24;
    int cols = 80;
};

TuiSize terminal_size();

void draw_line(int row, int cols, const std::vector<StyledSegment>& segments, StyleRole fill_role, const RenderStyle& style);
void draw_line(int row, int cols, const std::string& text, StyleRole role, const RenderStyle& style);
std::string format_line(int row,
                        int cols,
                        const std::vector<StyledSegment>& segments,
                        StyleRole fill_role,
                        const RenderStyle& style);
std::string format_line(int row,
                        int cols,
                        const std::string& text,
                        StyleRole role,
                        const RenderStyle& style);

std::string error_line(const Error& error);
StyleRole status_role_for_text(const std::string& status);

std::vector<StyledLine> history_lines_for_session(const chat::Session& session,
                                                  int cols,
                                                  bool show_thinking_traces,
                                                  ActivityKind activity_kind = ActivityKind::None,
                                                  size_t activity_frame = 0,
                                                  bool markdown_highlight = true,
                                                  bool agent_mode = false);
std::vector<StyledLine> panel_lines_for_text(const std::string& text,
                                               TuiMode mode,
                                               int cols,
                                               const char* title_override = nullptr);

editor::EditorState empty_input_editor(size_t undo_limit);
void set_status_from_error(const Error& err, std::string& status);
void insert_input(editor::EditorState& input, const std::string& text, std::string& status);

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
            bool show_scrollbar,
            const RenderStyle& style,
            TerminalFrameRenderer& frame_renderer,
            const char* panel_title_override = nullptr,
            bool agent_mode = false,
            const AgentChrome& agent_chrome = AgentChrome{},
            const std::vector<StyledLine>* overlay_lines = nullptr);

}  // namespace ainiux::tui::detail
