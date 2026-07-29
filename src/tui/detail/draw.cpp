#include "tui/activity.hpp"
#include "tui/detail/render.hpp"
#include "tui/events.hpp"
#include "tui/theme_registry.hpp"
#include "tui/tui.hpp"

#include "agent/tool_display.hpp"
#include "app/detail.hpp"
#include "provider/provider.hpp"

#include <iostream>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

namespace ainiux::tui::detail {

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

void append_style(std::string& output, const RenderStyle& style, StyleRole role) {
    if (style.colors && style.themes != nullptr) {
        output += style_sequence_for(*style.themes, style.theme_name, role);
    }
}

void append_style_reset(std::string& output, const RenderStyle& style) {
    if (style.colors) {
        output += "\x1b[0m";
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

bool same_attributes(const TextAttributes& left, const TextAttributes& right) {
    return left.bold == right.bold && left.italic == right.italic &&
           left.underline == right.underline;
}

void append_styled_piece(std::vector<StyledSegment>& line,
                         std::string text,
                         StyleRole role,
                         const TextAttributes& attributes = {}) {
    if (text.empty()) {
        return;
    }
    if (!line.empty() && line.back().role == role && !line.back().reverse &&
        same_attributes(line.back().attributes, attributes)) {
        line.back().text += text;
    } else {
        line.push_back({std::move(text), role, false, attributes});
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

void append_segment(std::vector<StyledSegment>& segments,
                    std::string text,
                    StyleRole role,
                    const TextAttributes& attributes = {}) {
    if (text.empty()) {
        return;
    }
    if (!segments.empty() && segments.back().role == role && !segments.back().reverse &&
        same_attributes(segments.back().attributes, attributes)) {
        segments.back().text += text;
        return;
    }
    segments.push_back({std::move(text), role, false, attributes});
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
    if (role == "tool") {
        return "Tool";
    }
    if (role == "notice") {
        return "Notice";
    }
    if (role == "summary") {
        return "Summary";
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

std::string format_line(int row,
                        int cols,
                        const std::vector<StyledSegment>& segments,
                        StyleRole fill_role,
                        const RenderStyle& style) {
    std::string output =
        "\x1b[" + std::to_string(row) + ";1H";
    int used = 0;
    for (const StyledSegment& segment : segments) {
        if (used >= cols) {
            break;
        }
        const std::string clipped = clip_cells(segment.text, cols - used);
        if (clipped.empty()) {
            continue;
        }
        append_style(output, style, segment.role);
        if (style.colors) {
            output += ansi_text_attributes_sequence(segment.attributes);
        }
        if (segment.reverse) {
            output += "\x1b[7m";
        }
        output += clipped;
        if (segment.reverse || (style.colors &&
                                (segment.attributes.bold || segment.attributes.italic ||
                                 segment.attributes.underline))) {
            output += "\x1b[0m";
        }
        used += displayed_cells(clipped);
    }
    if (used < cols) {
        append_style(output, style, fill_role);
        output += std::string(static_cast<size_t>(cols - used), ' ');
    }
    append_style_reset(output, style);
    output += "\x1b[K";
    return output;
}

std::string format_line(int row,
                        int cols,
                        const std::string& text,
                        StyleRole role,
                        const RenderStyle& style) {
    return format_line(row, cols, std::vector<StyledSegment>{{text, role}}, role, style);
}

void draw_line(int row, int cols, const std::vector<StyledSegment>& segments, StyleRole fill_role, const RenderStyle& style) {
    std::cout << format_line(row, cols, segments, fill_role, style);
}

void draw_line(int row, int cols, const std::string& text, StyleRole role, const RenderStyle& style) {
    std::cout << format_line(row, cols, text, role, style);
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
            append_styled_piece(current, piece, segment.role, segment.attributes);
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

std::vector<StyledSegment> markdown_segments(const std::string& content) {
    const std::vector<std::string> lines = highlight::split_lines(content);
    const std::vector<highlight::HighlightedLine> highlighted =
        highlight::highlight_document(highlight::Language::Markdown, content);
    std::vector<StyledSegment> segments;
    for (size_t line_index = 0; line_index < lines.size(); ++line_index) {
        const std::string& line = lines[line_index];
        size_t pos = 0;
        if (line_index < highlighted.size() && !highlighted[line_index].work_limited) {
            for (const highlight::Span& span : highlighted[line_index].spans) {
                const size_t start = std::min(span.start, line.size());
                const size_t end = std::min(span.end, line.size());
                if (start > pos) {
                    append_segment(segments, line.substr(pos, start - pos), StyleRole::Text);
                }
                if (end > start) {
                    append_segment(segments,
                                   line.substr(start, end - start),
                                   style_role_for_token(span.role),
                                   text_attributes_for_token(span.role));
                }
                pos = std::max(pos, end);
            }
        }
        if (pos < line.size()) {
            append_segment(segments, line.substr(pos), StyleRole::Text);
        }
        if (line_index + 1 < lines.size()) {
            append_segment(segments, "\n", StyleRole::Text);
        }
    }
    if (segments.empty()) {
        segments.push_back({"", StyleRole::Text});
    }
    return segments;
}

std::vector<StyledSegment> markdown_outside_thinking_segments(
    const std::vector<StyledSegment>& input) {
    std::vector<StyledSegment> output;
    for (const StyledSegment& segment : input) {
        if (segment.role != StyleRole::Text) {
            append_segment(output, segment.text, segment.role, segment.attributes);
            continue;
        }
        const std::vector<StyledSegment> highlighted = markdown_segments(segment.text);
        for (const StyledSegment& item : highlighted) {
            append_segment(output, item.text, item.role, item.attributes);
        }
    }
    return output;
}

StyleRole status_role_for_text(const std::string& status) {
    if (starts_with(status, "AINIUX_ERR_") || starts_with(status, "Unknown command") ||
        starts_with(status, "Usage:") || starts_with(status, "Cannot ") ||
        starts_with(status, "No previous") || starts_with(status, "A model job") ||
        starts_with(status, "A file job") || starts_with(status, "terminal input error")) {
        return StyleRole::Error;
    }
    return StyleRole::Status;
}

std::vector<StyledLine> history_lines_for_session(const chat::Session& session,
                                                  int cols,
                                                  bool show_thinking_traces,
                                                  ActivityKind activity_kind,
                                                  size_t activity_frame,
                                                  bool markdown_highlight,
                                                  bool agent_mode) {
    std::vector<StyledLine> history;
    const int min_content_width = 8;
    // Agent total task time is relative to the most recent user message timestamp.
    long long agent_turn_start_ms = 0;
    for (size_t message_index = 0; message_index < session.messages.size(); ++message_index) {
        const provider::Message& message = session.messages[message_index];
        if (agent_mode && message.role == "user" && message.created_at_ms > 0) {
            agent_turn_start_ms = message.created_at_ms;
        }
        // Agent mode chrome is minimal: user prompts as "> text", tool/assistant
        // output flush-left without "Assistant:" / "Tool:" labels or hanging indent.
        std::string prefix;
        StyleRole label_role = StyleRole::Muted;
        if (agent_mode) {
            if (message.role == "user") {
                prefix = "> ";
                label_role = StyleRole::UserLabel;
            }
            // assistant / tool / notice / summary / system: no label, no indent
        } else {
            prefix = message_label(message.role) + ": ";
            label_role = label_role_for_message(message.role);
        }
        const bool is_last_message = message_index + 1 == session.messages.size();
        std::string content = message.content;
        if (agent_mode && (message.role == "tool" || message.role == "thinking"))
            content = agent::clip_to_cells(content, cols > 0 ? static_cast<std::size_t>(cols) : 0);
        if (message.role == "assistant") {
            if (message.content.empty()) {
                if (!is_last_message || activity_kind == ActivityKind::None) {
                    content = agent_mode ? "..." : "(waiting...)";
                } else {
                    content.clear();
                }
            } else {
                const ThinkingDisplay display = thinking_display_text(message.content, show_thinking_traces);
                if (!show_thinking_traces && display.saw_thinking_tag &&
                    app::detail::trim_ascii(display.text).empty()) {
                    content.clear();
                } else {
                    content = display.text;
                }
            }
        }
        if (!agent_mode && message.role == "assistant" && content.empty() &&
            activity_kind != ActivityKind::None && is_last_message) {
            // Chat mode owns activity chrome on the fixed status row. Do not add
            // a second thinking/streaming placeholder to transcript history.
            continue;
        }
        const bool show_thinking_placeholder =
            agent_mode && message.role == "assistant" && content.empty() &&
            activity_kind == ActivityKind::Thinking && is_last_message;
        const bool show_streaming_placeholder =
            agent_mode && message.role == "assistant" && content.empty() &&
            activity_kind == ActivityKind::Streaming && is_last_message;
        const bool show_streaming_indicator =
            agent_mode && message.role == "assistant" && !content.empty() &&
            activity_kind == ActivityKind::Streaming && is_last_message;
        std::vector<StyledSegment> content_segments;
        if (show_thinking_placeholder) {
            content_segments =
                activity_placeholder_segments("", ActivityKind::Thinking,
                                              activity_frame, "thinking...");
        } else if (show_streaming_placeholder) {
            content_segments =
                activity_placeholder_segments("", ActivityKind::Streaming,
                                              activity_frame,
                                              "working...");
        } else {
            content_segments = markdown_highlight ? markdown_segments(content) : plain_text_segments(content);
            if (message.role == "assistant" && show_thinking_traces) {
                content_segments = visible_thinking_trace_segments(content);
                if (markdown_highlight) {
                    content_segments = markdown_outside_thinking_segments(content_segments);
                }
            }
            if (show_streaming_indicator) {
                std::vector<StyledSegment> prefixed;
                prefixed.push_back(
                    {activity_indicator_text(ActivityKind::Streaming, activity_frame),
                     StyleRole::StreamingActivity});
                prefixed.push_back({" ", StyleRole::Text});
                prefixed.insert(prefixed.end(), content_segments.begin(), content_segments.end());
                content_segments = std::move(prefixed);
            }
        }
        // Compact tool rows already contain their own execution-only duration.
        // Only the final assistant uses the whole-turn wall clock.
        std::string elapsed_suffix;
        if (agent_mode && message.created_at_ms > 0 && agent_turn_start_ms > 0 &&
            message.role == "assistant" && !show_thinking_placeholder &&
            !show_streaming_placeholder) {
            const bool already_has_timing =
                content.find("Task complete in ") != std::string::npos;
            if (!already_has_timing) {
                const long long elapsed_ms = message.created_at_ms - agent_turn_start_ms;
                // Own line so the completion banner is not glued to the answer text.
                elapsed_suffix = "\n" + agent::format_task_complete(elapsed_ms);
            }
        }
        if (!elapsed_suffix.empty()) {
            content_segments.push_back({elapsed_suffix, StyleRole::Muted});
        }
        std::vector<std::vector<StyledSegment>> wrapped;
        const int prefix_cells = static_cast<int>(prefix.size());
        append_wrapped_segments(wrapped, content_segments,
                                std::max(min_content_width, cols - prefix_cells));
        if (wrapped.empty() && !prefix.empty()) {
            // Keep a lone "> " line if content is empty (unlikely for user).
            StyledLine line;
            line.segments.push_back({prefix, label_role});
            history.push_back(std::move(line));
            continue;
        }
        for (size_t i = 0; i < wrapped.size(); ++i) {
            StyledLine line;
            if (!prefix.empty()) {
                if (i == 0) {
                    line.segments.push_back({prefix, label_role});
                } else {
                    // Hanging indent only for chat labels (and agent "> " multi-line user).
                    line.segments.push_back({std::string(prefix.size(), ' '), StyleRole::Muted});
                }
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
        case TuiMode::ProviderList:
            return "Provider";
        case TuiMode::ModelList:
            return "Model";
        case TuiMode::ReasoningList:
            return "Reasoning";
        case TuiMode::ReasoningConfirm:
            return "Reasoning warning";
        case TuiMode::AttachmentList:
            return "Attachments";
        case TuiMode::AttachmentDeleteConfirm:
            return "Delete Attachment";
        case TuiMode::ThreadDeleteConfirm:
            return "Delete Thread";
        case TuiMode::RemoveConfirm:
            return "Remove Thread";
        case TuiMode::GuardApprovalConfirm:
            return "Guard approval";
        case TuiMode::AgentPermissionSelect:
            return "Agent permissions";
        case TuiMode::AgentContinueConfirm:
            return "Continue agent task";
        case TuiMode::AgentNewConfirm:
            return "Reset agent project";
        case TuiMode::AgentIndexConfirm:
            return "Code index";
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
    if ((mode == TuiMode::ThreadList || mode == TuiMode::ProviderList || mode == TuiMode::ModelList ||
         mode == TuiMode::ReasoningList ||
         mode == TuiMode::AttachmentList) &&
        (line.rfind(u8"› ", 0) == 0 || line.rfind("> ", 0) == 0)) {
        return StyleRole::PanelHighlight;
    }
    if (line.find("Press y") != std::string::npos || line.find("Enter saves") != std::string::npos ||
        line.find("Enter opens") != std::string::npos || line.find("Esc cancel") != std::string::npos ||
        line.find("Enter select") != std::string::npos || line.find("DEL delete") != std::string::npos ||
        line.find("Esc close") != std::string::npos) {
        return StyleRole::PanelHint;
    }
    return StyleRole::PanelBody;
}

std::vector<StyledLine> panel_lines_for_text(const std::string& text,
                                             TuiMode mode,
                                             int cols,
                                             const char* title_override) {
    std::vector<StyledLine> lines;
    if (cols <= 0) {
        return lines;
    }

    const char* title = title_override != nullptr ? title_override : panel_title_for_mode(mode);
    append_panel_rule_line(lines, title, cols);

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

}  // namespace ainiux::tui::detail
