#include "tui/detail/render.hpp"
#include "tui/events.hpp"
#include "tui/tui.hpp"

#include "app/detail.hpp"
#include "provider/provider.hpp"

#include <iostream>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

namespace pkchat::tui::detail {

namespace {

size_t utf8_len_at(const std::string& text, size_t pos) {
    const unsigned char ch = static_cast<unsigned char>(text[pos]);
    if (ch < 0x80U) {
        return 1;
    }
    if ((ch & 0xE0U) == 0xC0U && pos + 1 < text.size()) {
        return 2;
    }
    if ((ch & 0xF0U) == 0xE0U && pos + 2 < text.size()) {
        return 3;
    }
    if ((ch & 0xF8U) == 0xF0U && pos + 3 < text.size()) {
        return 4;
    }
    return 1;
}

std::string take_cells(const std::string& text, size_t& pos, int width) {
    std::string out;
    int cells = 0;
    while (pos < text.size() && cells < width) {
        if (text[pos] == '\n') {
            ++pos;
            break;
        }
        const size_t len = utf8_len_at(text, pos);
        out.append(text, pos, len);
        pos += len;
        ++cells;
    }
    return out;
}

std::string clip_cells(const std::string& text, int width) {
    if (width <= 0) {
        return "";
    }
    size_t pos = 0;
    return take_cells(text, pos, width);
}

int displayed_cells(const std::string& text) {
    int cells = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        if (text[pos] == '\n') {
            break;
        }
        pos += utf8_len_at(text, pos);
        ++cells;
    }
    return cells;
}

std::string style_sequence(ThemeName theme, StyleRole role) {
    const StylePair pair = style_pair_for(theme, role);
    return "\x1b[38;2;" + std::to_string(pair.foreground.r) + ";" +
           std::to_string(pair.foreground.g) + ";" + std::to_string(pair.foreground.b) +
           "m\x1b[48;2;" + std::to_string(pair.background.r) + ";" +
           std::to_string(pair.background.g) + ";" + std::to_string(pair.background.b) + "m";
}

void write_style(const RenderStyle& style, StyleRole role) {
    if (style.colors) {
        std::cout << style_sequence(style.theme, role);
    }
}

void reset_style(const RenderStyle& style) {
    if (style.colors) {
        std::cout << "\x1b[0m";
    }
}

std::string take_cells_before_newline(const std::string& text, size_t& pos, int width) {
    std::string out;
    int cells = 0;
    while (pos < text.size() && cells < width && text[pos] != '\n') {
        const size_t len = utf8_len_at(text, pos);
        out.append(text, pos, len);
        pos += len;
        ++cells;
    }
    return out;
}

void append_styled_piece(std::vector<StyledSegment>& line, std::string text, StyleRole role) {
    if (text.empty()) {
        return;
    }
    if (!line.empty() && line.back().role == role) {
        line.back().text += text;
    } else {
        line.push_back({std::move(text), role});
    }
}

bool starts_with(const std::string& text, const std::string& prefix) {
    return text.rfind(prefix, 0) == 0;
}

char lower_ascii(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<char>(ch - 'A' + 'a');
    }
    return ch;
}

bool tag_at_case_insensitive(const std::string& text, std::size_t pos, const std::string& tag) {
    if (pos + tag.size() > text.size()) {
        return false;
    }
    for (std::size_t i = 0; i < tag.size(); ++i) {
        if (lower_ascii(text[pos + i]) != tag[i]) {
            return false;
        }
    }
    return true;
}

std::size_t find_tag_case_insensitive(const std::string& text, const std::string& tag, std::size_t start) {
    for (std::size_t pos = start; pos + tag.size() <= text.size(); ++pos) {
        if (tag_at_case_insensitive(text, pos, tag)) {
            return pos;
        }
    }
    return std::string::npos;
}

void append_segment(std::vector<StyledSegment>& segments, std::string text, StyleRole role) {
    if (text.empty()) {
        return;
    }
    if (!segments.empty() && segments.back().role == role) {
        segments.back().text += text;
        return;
    }
    segments.push_back({std::move(text), role});
}

std::string message_label(const std::string& role) {
    if (role == "user") {
        return "You";
    }
    if (role == "assistant") {
        return "Assistant";
    }
    if (role == "system") {
        return "System";
    }
    return role;
}

StyleRole label_role_for_message(const std::string& role) {
    if (role == "user") {
        return StyleRole::UserLabel;
    }
    if (role == "assistant") {
        return StyleRole::AssistantLabel;
    }
    return StyleRole::Muted;
}

std::string provider_model_status_label(const std::string& provider_name, const std::string& model_name) {
    const std::string display_provider =
        provider_name.empty() ? "" : provider::display_name_for_profile(provider_name);
    if (display_provider.empty() && model_name.empty()) {
        return "";
    }
    if (display_provider.empty()) {
        return "[" + model_name + "]";
    }
    if (model_name.empty()) {
        return "[" + display_provider + " / model unknown]";
    }
    return "[" + display_provider + " / " + model_name + "]";
}

std::string provider_model_status_label(const chat::Session& session) {
    return provider_model_status_label(session.provider, session.model);
}

std::string provider_model_status_message(const std::string& label,
                                          const std::string& indicator,
                                          const std::string& suffix) {
    if (label.empty()) {
        return indicator + suffix;
    }
    return label + " " + indicator + suffix;
}

}  // namespace

TuiSize terminal_size() {
    TuiSize size;
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        size.rows = ws.ws_row;
        size.cols = ws.ws_col;
    }
    return size;
}

std::string error_line(const Error& error) {
    return std::string(error_code_name(error.code)) + ": " + error.message;
}

void draw_line(int row, int cols, const std::vector<StyledSegment>& segments, StyleRole fill_role, const RenderStyle& style) {
    std::cout << "\x1b[" << row << ";1H";
    int used = 0;
    for (const StyledSegment& segment : segments) {
        if (used >= cols) {
            break;
        }
        const std::string clipped = clip_cells(segment.text, cols - used);
        if (clipped.empty()) {
            continue;
        }
        write_style(style, segment.role);
        std::cout << clipped;
        used += displayed_cells(clipped);
    }
    if (used < cols) {
        write_style(style, fill_role);
        std::cout << std::string(static_cast<size_t>(cols - used), ' ');
    }
    reset_style(style);
    std::cout << "\x1b[K";
}

void draw_line(int row, int cols, const std::string& text, StyleRole role, const RenderStyle& style) {
    draw_line(row, cols, std::vector<StyledSegment>{{text, role}}, role, style);
}

void append_wrapped_segments(std::vector<std::vector<StyledSegment>>& lines,
                             const std::vector<StyledSegment>& segments,
                             int width) {
    if (width <= 0) {
        lines.push_back({});
        return;
    }

    std::vector<StyledSegment> current;
    int cells = 0;
    bool last_was_newline = false;
    auto finish_line = [&]() {
        lines.push_back(std::move(current));
        current.clear();
        cells = 0;
    };

    for (const StyledSegment& segment : segments) {
        size_t pos = 0;
        while (pos < segment.text.size()) {
            if (segment.text[pos] == '\n') {
                finish_line();
                last_was_newline = true;
                ++pos;
                continue;
            }
            if (cells >= width) {
                finish_line();
            }
            const size_t before = pos;
            const std::string piece = take_cells_before_newline(segment.text, pos, width - cells);
            if (piece.empty()) {
                if (pos == before) {
                    pos += utf8_len_at(segment.text, pos);
                }
                continue;
            }
            append_styled_piece(current, piece, segment.role);
            cells += displayed_cells(piece);
            last_was_newline = false;
        }
    }

    if (lines.empty() || !current.empty() || last_was_newline) {
        lines.push_back(std::move(current));
    }
}

std::vector<StyledSegment> visible_thinking_trace_segments(const std::string& content) {
    const std::string open_tag = "<think>";
    const std::string close_tag = "</think>";
    std::vector<StyledSegment> segments;
    std::size_t pos = 0;
    while (pos < content.size()) {
        const std::size_t open = find_tag_case_insensitive(content, open_tag, pos);
        if (open == std::string::npos) {
            append_segment(segments, content.substr(pos), StyleRole::Text);
            break;
        }
        append_segment(segments, content.substr(pos, open - pos), StyleRole::Text);
        const std::size_t after_open = open + open_tag.size();
        const std::size_t close = find_tag_case_insensitive(content, close_tag, after_open);
        if (close == std::string::npos) {
            append_segment(segments, content.substr(open), StyleRole::ThinkingTrace);
            break;
        }
        append_segment(segments, content.substr(open, close + close_tag.size() - open), StyleRole::ThinkingTrace);
        pos = close + close_tag.size();
    }
    if (segments.empty()) {
        segments.push_back({"", StyleRole::Text});
    }
    return segments;
}

std::vector<StyledSegment> plain_text_segments(const std::string& content) {
    return std::vector<StyledSegment>{{content, StyleRole::Text}};
}

StyleRole status_role_for_text(const std::string& status) {
    if (starts_with(status, "PKCHAT_ERR_") || starts_with(status, "Unknown command") ||
        starts_with(status, "Usage:") || starts_with(status, "Cannot ") ||
        starts_with(status, "No previous") || starts_with(status, "A model job") ||
        starts_with(status, "A file job") || starts_with(status, "terminal input error")) {
        return StyleRole::Error;
    }
    return StyleRole::Status;
}

std::vector<StyledLine> history_lines_for_session(const chat::Session& session, int cols, bool show_thinking_traces) {
    std::vector<StyledLine> history;
    const int min_content_width = 8;
    for (const provider::Message& message : session.messages) {
        const std::string prefix = message_label(message.role) + ": ";
        const StyleRole label_role = label_role_for_message(message.role);
        std::string content = message.content;
        if (message.role == "assistant") {
            if (message.content.empty()) {
                content = "(waiting...)";
            } else {
                const ThinkingDisplay display = thinking_display_text(message.content, show_thinking_traces);
                if (!show_thinking_traces && display.saw_thinking_tag &&
                    app::detail::trim_ascii(display.text).empty()) {
                    content = provider_model_status_message(provider_model_status_label(session),
                                                            kThinkingActivityIndicator,
                                                            "thinking...");
                } else {
                    content = display.text;
                }
            }
        }
        std::vector<StyledSegment> content_segments = plain_text_segments(content);
        if (message.role == "assistant" && show_thinking_traces) {
            content_segments = visible_thinking_trace_segments(content);
        }
        std::vector<std::vector<StyledSegment>> wrapped;
        append_wrapped_segments(wrapped, content_segments,
                                std::max(min_content_width, cols - static_cast<int>(prefix.size())));
        for (size_t i = 0; i < wrapped.size(); ++i) {
            StyledLine line;
            if (i == 0) {
                line.segments.push_back({prefix, label_role});
            } else {
                line.segments.push_back({std::string(prefix.size(), ' '), StyleRole::Muted});
            }
            line.segments.insert(line.segments.end(), wrapped[i].begin(), wrapped[i].end());
            history.push_back(std::move(line));
        }
    }
    return history;
}

const char* panel_title_for_mode(TuiMode mode) {
    switch (mode) {
        case TuiMode::ThreadList:
            return "Threads";
        case TuiMode::RemoveConfirm:
            return "Remove Thread";
        case TuiMode::ModelConfirm:
            return "Model";
        case TuiMode::SystemEdit:
            return "System Prompt";
        case TuiMode::HistoryEdit:
            return "Edit Message";
        case TuiMode::Chat:
            return "Help";
    }
    return "Panel";
}

void append_panel_rule_line(std::vector<StyledLine>& lines, const std::string& title, int cols) {
    StyledLine line;
    line.segments.push_back({u8"── ", StyleRole::PanelBorder});
    line.segments.push_back({title, StyleRole::PanelTitle});
    std::string tail = " ";
    int used = displayed_cells(u8"── ") + displayed_cells(title) + 1;
    while (used < cols) {
        tail += u8"─";
        ++used;
    }
    line.segments.push_back({tail, StyleRole::PanelBorder});
    lines.push_back(std::move(line));
}

void append_panel_fill_line(std::vector<StyledLine>& lines, int cols) {
    std::string rule;
    for (int i = 0; i < cols; ++i) {
        rule += u8"─";
    }
    lines.push_back({{{rule, StyleRole::PanelBorder}}});
}

StyleRole panel_body_role_for_line(TuiMode mode, const std::string& line) {
    if (mode == TuiMode::ThreadList &&
        (line.rfind(u8"› ", 0) == 0 || line.rfind("> ", 0) == 0)) {
        return StyleRole::PanelHighlight;
    }
    if (line.find("Press y") != std::string::npos || line.find("Enter saves") != std::string::npos ||
        line.find("Enter opens") != std::string::npos || line.find("Esc cancel") != std::string::npos) {
        return StyleRole::PanelHint;
    }
    return StyleRole::PanelBody;
}

std::vector<StyledLine> panel_lines_for_text(const std::string& text, TuiMode mode, int cols) {
    std::vector<StyledLine> lines;
    if (cols <= 0) {
        return lines;
    }

    append_panel_rule_line(lines, panel_title_for_mode(mode), cols);

    std::vector<std::string> content_lines;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            content_lines.push_back(text.substr(start));
            break;
        }
        content_lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }

    if (!content_lines.empty()) {
        append_panel_fill_line(lines, cols);
        for (const std::string& line : content_lines) {
            const StyleRole role = panel_body_role_for_line(mode, line);
            std::vector<std::vector<StyledSegment>> wrapped;
            append_wrapped_segments(wrapped, plain_text_segments(line), cols);
            for (std::vector<StyledSegment>& wrapped_line : wrapped) {
                for (StyledSegment& segment : wrapped_line) {
                    segment.role = role;
                }
                lines.push_back({std::move(wrapped_line)});
            }
        }
        append_panel_fill_line(lines, cols);
    }

    return lines;
}

}  // namespace pkchat::tui::detail