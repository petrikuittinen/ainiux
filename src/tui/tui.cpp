#include "tui/tui.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

#include "runtime/runtime.hpp"

namespace pkchat::tui {

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

RegenerationPlan regeneration_plan_for_session(const chat::Session& session) {
    for (std::size_t i = session.messages.size(); i > 0; --i) {
        const std::size_t index = i - 1;
        if (session.messages[index].role == "user") {
            return {true, index, session.messages[index].content};
        }
    }
    return {};
}

const char* theme_name(ThemeName theme) {
    switch (theme) {
        case ThemeName::Dark:
            return "dark";
        case ThemeName::Light:
            return "light";
    }
    return "dark";
}

bool parse_theme_name(const std::string& text, ThemeName& out) {
    std::string lower;
    lower.reserve(text.size());
    for (char ch : text) {
        if (ch >= 'A' && ch <= 'Z') {
            lower.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else {
            lower.push_back(ch);
        }
    }
    if (lower == "dark") {
        out = ThemeName::Dark;
        return true;
    }
    if (lower == "light") {
        out = ThemeName::Light;
        return true;
    }
    return false;
}

StylePair style_pair_for(ThemeName theme, StyleRole role) {
    const bool dark = theme == ThemeName::Dark;
    const Rgb background = dark ? Rgb{0x0B, 0x0F, 0x14} : Rgb{0xFA, 0xFA, 0xFA};
    const Rgb text = dark ? Rgb{0xE6, 0xED, 0xF3} : Rgb{0x11, 0x18, 0x27};
    const Rgb muted = dark ? Rgb{0x9B, 0xA7, 0xB4} : Rgb{0x4B, 0x55, 0x63};
    const Rgb thinking = dark ? Rgb{0xA7, 0xB8, 0xC9} : Rgb{0x52, 0x63, 0x7A};
    const Rgb user = dark ? Rgb{0x7D, 0xD3, 0xFC} : Rgb{0x07, 0x59, 0x85};
    const Rgb assistant = dark ? Rgb{0x86, 0xEF, 0xAC} : Rgb{0x16, 0x65, 0x34};
    const Rgb error = dark ? Rgb{0xFC, 0xA5, 0xA5} : Rgb{0xB9, 0x1C, 0x1C};
    const Rgb status_bg = dark ? Rgb{0x1F, 0x29, 0x37} : Rgb{0xE5, 0xE7, 0xEB};
    const Rgb status_fg = dark ? Rgb{0xFF, 0xFF, 0xFF} : Rgb{0x11, 0x18, 0x27};

    switch (role) {
        case StyleRole::Text:
            return {text, background};
        case StyleRole::Muted:
            return {muted, background};
        case StyleRole::ThinkingTrace:
            return {thinking, background};
        case StyleRole::UserLabel:
            return {user, background};
        case StyleRole::AssistantLabel:
            return {assistant, background};
        case StyleRole::Error:
            return {error, background};
        case StyleRole::Status:
            return {status_fg, status_bg};
        case StyleRole::InputLabel:
            return {status_fg, status_bg};
    }
    return {text, background};
}

double contrast_ratio(Rgb foreground, Rgb background) {
    auto linear = [](int channel) {
        const double value = static_cast<double>(channel) / 255.0;
        if (value <= 0.03928) {
            return value / 12.92;
        }
        return std::pow((value + 0.055) / 1.055, 2.4);
    };
    auto luminance = [&](Rgb color) {
        return 0.2126 * linear(color.r) + 0.7152 * linear(color.g) + 0.0722 * linear(color.b);
    };
    const double a = luminance(foreground);
    const double b = luminance(background);
    const double lighter = std::max(a, b);
    const double darker = std::min(a, b);
    return (lighter + 0.05) / (darker + 0.05);
}

int history_scroll_for_thread_beginning() {
    return std::numeric_limits<int>::max();
}

int history_scroll_for_thread_end() {
    return 0;
}

ThinkingDisplay thinking_display_text(const std::string& content, bool show_traces) {
    ThinkingDisplay display;
    if (show_traces) {
        display.text = content;
        return display;
    }

    const std::string open_tag = "<think>";
    const std::string close_tag = "</think>";
    auto lower_ascii = [](char ch) {
        if (ch >= 'A' && ch <= 'Z') {
            return static_cast<char>(ch - 'A' + 'a');
        }
        return ch;
    };
    auto tag_at = [&](std::size_t pos, const std::string& tag) {
        if (pos + tag.size() > content.size()) {
            return false;
        }
        for (std::size_t i = 0; i < tag.size(); ++i) {
            if (lower_ascii(content[pos + i]) != tag[i]) {
                return false;
            }
        }
        return true;
    };
    auto find_tag = [&](const std::string& tag, std::size_t start) {
        for (std::size_t pos = start; pos + tag.size() <= content.size(); ++pos) {
            if (tag_at(pos, tag)) {
                return pos;
            }
        }
        return std::string::npos;
    };
    auto trim_outer_newlines = [](std::string& text) {
        while (!text.empty() && (text.front() == '\n' || text.front() == '\r')) {
            text.erase(text.begin());
        }
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
    };

    bool in_thinking = false;
    std::size_t pos = 0;
    while (pos < content.size()) {
        if (!in_thinking) {
            const std::size_t open = find_tag(open_tag, pos);
            if (open == std::string::npos) {
                display.text.append(content, pos, std::string::npos);
                break;
            }
            display.text.append(content, pos, open - pos);
            display.saw_thinking_tag = true;
            pos = open + open_tag.size();
            in_thinking = true;
            continue;
        }

        const std::size_t close = find_tag(close_tag, pos);
        if (close == std::string::npos) {
            display.open_thinking_tag = true;
            break;
        }
        pos = close + close_tag.size();
        in_thinking = false;
    }

    if (display.saw_thinking_tag) {
        trim_outer_newlines(display.text);
    }
    return display;
}

namespace {

int exit_code_for(ErrorCode code) {
    switch (code) {
        case ErrorCode::Ok:
            return 0;
        case ErrorCode::BadArgs:
        case ErrorCode::BadUrl:
            return 2;
        case ErrorCode::Dns:
        case ErrorCode::Connect:
        case ErrorCode::Tls:
        case ErrorCode::Timeout:
            return 3;
        case ErrorCode::HttpStatus:
        case ErrorCode::Auth:
        case ErrorCode::RateLimit:
        case ErrorCode::JsonParse:
        case ErrorCode::SseParse:
        case ErrorCode::ProviderSchema:
            return 4;
        case ErrorCode::FileRead:
        case ErrorCode::FileWrite:
        case ErrorCode::Config:
            return 5;
        case ErrorCode::Cancelled:
            return 130;
        case ErrorCode::UnsupportedFeature:
        case ErrorCode::Internal:
            return 6;
    }
    return 6;
}

std::string trim_ascii(std::string text) {
    auto is_ws = [](unsigned char ch) { return ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t'; };
    while (!text.empty() && is_ws(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
    }
    while (!text.empty() && is_ws(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    return text;
}

void refresh_session_metadata(chat::Session& session, const provider::RequestContext& context) {
    session.provider = context.profile.name;
    session.base_url = context.base_url;
    session.model = context.options.model;
}

bool has_system_message(const chat::Session& session) {
    for (const provider::Message& message : session.messages) {
        if (message.role == "system") {
            return true;
        }
    }
    return false;
}

void apply_system_prompt(chat::Session& session, const std::string& system) {
    if (trim_ascii(system).empty() || has_system_message(session)) {
        return;
    }
    session.messages.insert(session.messages.begin(), {"system", system});
}

void replace_system_prompt(chat::Session& session, const std::string& system) {
    for (auto it = session.messages.begin(); it != session.messages.end();) {
        if (it->role == "system") {
            it = session.messages.erase(it);
        } else {
            ++it;
        }
    }
    if (!trim_ascii(system).empty()) {
        session.messages.insert(session.messages.begin(), {"system", system});
    }
}

struct TuiSize {
    int rows = 24;
    int cols = 80;
};

TuiSize terminal_size() {
    TuiSize size;
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        size.rows = ws.ws_row;
        size.cols = ws.ws_col;
    }
    return size;
}

class TerminalSession {
   public:
    TerminalSession() = default;
    ~TerminalSession() { restore(); }
    TerminalSession(const TerminalSession&) = delete;
    TerminalSession& operator=(const TerminalSession&) = delete;

    Error enter() {
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
            return {ErrorCode::BadArgs, "--tui requires an interactive terminal"};
        }
        if (tcgetattr(STDIN_FILENO, &original_) != 0) {
            return {ErrorCode::Internal, std::string("could not read terminal mode: ") + std::strerror(errno)};
        }
        termios raw = original_;
        raw.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON | IEXTEN | ISIG));
        raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL | BRKINT));
        raw.c_oflag &= static_cast<tcflag_t>(~OPOST);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
            return {ErrorCode::Internal, std::string("could not set terminal mode: ") + std::strerror(errno)};
        }
        active_ = true;
        std::cout << "\x1b[?1049h\x1b[?25h\x1b[2J\x1b[H";
        std::cout.flush();
        return ok_error();
    }

    void restore() {
        if (!active_) {
            return;
        }
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_);
        std::cout << "\x1b[0m\x1b[?25h\x1b[2J\x1b[H\x1b[?1049l";
        std::cout.flush();
        active_ = false;
    }

   private:
    termios original_{};
    bool active_ = false;
};

bool read_byte(unsigned char& out, int timeout_ms) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(STDIN_FILENO, &read_fds);
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    const int ready = select(STDIN_FILENO + 1, &read_fds, nullptr, nullptr, &tv);
    if (ready <= 0 || !FD_ISSET(STDIN_FILENO, &read_fds)) {
        return false;
    }
    const ssize_t n = read(STDIN_FILENO, &out, 1);
    return n == 1;
}

size_t utf8_len_at(const std::string& text, size_t pos) {
    const unsigned char ch = static_cast<unsigned char>(text[pos]);
    if (ch < 0x80U) return 1;
    if ((ch & 0xE0U) == 0xC0U && pos + 1 < text.size()) return 2;
    if ((ch & 0xF0U) == 0xE0U && pos + 2 < text.size()) return 3;
    if ((ch & 0xF8U) == 0xF0U && pos + 3 < text.size()) return 4;
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

struct RenderStyle {
    ThemeName theme = ThemeName::Dark;
    bool colors = true;
};

struct StyledSegment {
    std::string text;
    StyleRole role = StyleRole::Text;
};

struct StyledLine {
    std::vector<StyledSegment> segments;
};

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

std::string message_label(const std::string& role) {
    if (role == "user") return "You";
    if (role == "assistant") return "Assistant";
    if (role == "system") return "System";
    return role;
}

StyleRole label_role_for_message(const std::string& role) {
    if (role == "user") return StyleRole::UserLabel;
    if (role == "assistant") return StyleRole::AssistantLabel;
    return StyleRole::Muted;
}

std::string error_line(const Error& error) {
    return std::string(error_code_name(error.code)) + ": " + error.message;
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
                if (!show_thinking_traces && display.saw_thinking_tag && trim_ascii(display.text).empty()) {
                    content = "thinking...";
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
        append_wrapped_segments(wrapped, content_segments, std::max(min_content_width, cols - static_cast<int>(prefix.size())));
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

editor::EditorState empty_input_editor() {
    editor::EditorState input = editor::EditorState::from_text("");
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
            const RenderStyle& style) {
    const TuiSize terminal = terminal_size();
    const Layout layout = layout_for_terminal(terminal.rows, terminal.cols);
    const int cols = layout.cols;

    input.ensure_cursor_visible(layout.input_rect);
    const editor::RenderedPanel input_panel = input.render(layout.input_rect);
    std::vector<StyledLine> history = history_lines_for_session(session, cols, show_thinking_traces);
    const int max_history_scroll = std::max(0, static_cast<int>(history.size()) - layout.history_rows);
    history_scroll = std::min(std::max(0, history_scroll), max_history_scroll);

    std::cout << "\x1b[?25l";

    const int history_start = std::max(0, static_cast<int>(history.size()) - layout.history_rows - history_scroll);
    int printed = 0;
    for (int i = history_start; i < static_cast<int>(history.size()) && printed < layout.history_rows; ++i, ++printed) {
        draw_line(layout.history_row + printed, cols, history[static_cast<size_t>(i)].segments, StyleRole::Text, style);
    }
    while (printed < layout.history_rows) {
        draw_line(layout.history_row + printed, cols, "", StyleRole::Text, style);
        ++printed;
    }

    draw_line(layout.status_row, cols, status, status_role_for_text(status), style);
    draw_line(layout.input_label_row,
              cols,
              "Input  Enter send | Alt+Enter newline | Esc cancel | Ctrl+R regen | PgUp/PgDn scroll | Ctrl+C quit",
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

enum class TuiEventType { Delta, Done, Error, SaveDone, LoadDone, ModelsDone };

enum class ActiveJob { None, Chat, Models };

struct TuiEvent {
    TuiEventType type = TuiEventType::Delta;
    std::string text;
    Error error;
    provider::ChatResult chat;
    chat::Session session;
    std::vector<std::string> models;
};

std::string join_models_preview(const std::vector<std::string>& models) {
    if (models.empty()) {
        return "No models returned";
    }
    std::string out = "Models:";
    const size_t limit = std::min<size_t>(models.size(), 5);
    for (size_t i = 0; i < limit; ++i) {
        out += (i == 0 ? " " : ", ");
        out += models[i];
    }
    if (models.size() > limit) {
        out += ", ...";
    }
    return out;
}

bool is_escape_final(unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') || ch == '~';
}

bool handle_escape(editor::EditorState& input, const Layout& layout, int& history_scroll, std::string& status) {
    unsigned char ch = 0;
    if (!read_byte(ch, 25)) {
        return false;
    }
    if (ch == '\r' || ch == '\n') {
        insert_input(input, "\n", status);
        status = "Inserted newline. Enter sends; Ctrl+S also sends.";
        return true;
    }

    std::string sequence;
    if (ch == '[' || ch == 'O') {
        sequence.push_back(static_cast<char>(ch));
        while (sequence.size() < 16 && read_byte(ch, 25)) {
            sequence.push_back(static_cast<char>(ch));
            if (is_escape_final(ch)) {
                break;
            }
        }
    } else {
        if (ch >= 32 || ch == '\t') {
            insert_input(input, std::string(1, static_cast<char>(ch)), status);
        }
        return true;
    }

    if (sequence == "[A" || sequence == "OA") {
        input.move_up(layout.input_rect);
    } else if (sequence == "[B" || sequence == "OB") {
        input.move_down(layout.input_rect);
    } else if (sequence == "[C" || sequence == "OC") {
        input.move_right();
    } else if (sequence == "[D" || sequence == "OD") {
        input.move_left();
    } else if (sequence == "[H" || sequence == "[1~" || sequence == "OH") {
        history_scroll = history_scroll_for_thread_beginning();
    } else if (sequence == "[F" || sequence == "[4~" || sequence == "OF") {
        history_scroll = history_scroll_for_thread_end();
    } else if (sequence == "[3~") {
        set_status_from_error(input.erase_at_cursor(), status);
    } else if (sequence == "[5~") {
        history_scroll += std::max(1, layout.history_rows / 2);
    } else if (sequence == "[6~") {
        history_scroll -= std::max(1, layout.history_rows / 2);
    }
    return true;
}

}  // namespace

int run(provider::RequestContext context, chat::Session session) {
    TerminalSession terminal;
    Error err = terminal.enter();
    if (!err.ok()) {
        std::cerr << error_code_name(err.code) << ": " << err.message << "\n";
        return exit_code_for(err.code);
    }

    runtime::EventQueue<TuiEvent> events;
    runtime::JobHandle model_job;
    runtime::JobHandle file_job;
    ActiveJob active_job = ActiveJob::None;
    editor::EditorState input = empty_input_editor();
    std::string status = "Ready";
    ThemeName theme = ThemeName::Dark;
    const bool use_colors = !context.options.no_colors;
    bool quit = false;
    bool show_thinking_traces = false;
    size_t pending_user = static_cast<size_t>(-1);
    size_t pending_assistant = static_cast<size_t>(-1);
    int history_scroll = 0;
    bool regenerate_after_cancel = false;
    std::string queued_regeneration_prompt;

    auto pending_assistant_is_hidden_thinking = [&]() {
        if (show_thinking_traces || pending_assistant == static_cast<size_t>(-1) ||
            pending_assistant >= session.messages.size()) {
            return false;
        }
        const ThinkingDisplay display = thinking_display_text(session.messages[pending_assistant].content, false);
        return display.saw_thinking_tag && trim_ascii(display.text).empty();
    };

    auto set_thinking_trace_mode = [&](bool show_traces) {
        show_thinking_traces = show_traces;
        if (!show_thinking_traces && pending_assistant_is_hidden_thinking()) {
            status = "thinking...";
        } else {
            status = show_thinking_traces ? "Thinking traces shown" : "Thinking traces hidden";
        }
    };

    auto rollback_pending_turn = [&]() {
        if (pending_assistant != static_cast<size_t>(-1) && pending_assistant < session.messages.size()) {
            session.messages.erase(session.messages.begin() + static_cast<long>(pending_assistant));
        }
        if (pending_user != static_cast<size_t>(-1) && pending_user < session.messages.size()) {
            session.messages.erase(session.messages.begin() + static_cast<long>(pending_user));
        }
        pending_user = static_cast<size_t>(-1);
        pending_assistant = static_cast<size_t>(-1);
    };

    auto keep_cancelled_turn = [&]() {
        if (pending_assistant != static_cast<size_t>(-1) && pending_assistant < session.messages.size() &&
            session.messages[pending_assistant].content.empty()) {
            session.messages.erase(session.messages.begin() + static_cast<long>(pending_assistant));
        }
        pending_user = static_cast<size_t>(-1);
        pending_assistant = static_cast<size_t>(-1);
    };

    auto start_save = [&](const std::string& path, chat::Session snapshot) {
        if (path.empty()) {
            return;
        }
        if (file_job.running()) {
            status = "A file job is already running";
            return;
        }
        file_job.start([path, snapshot = std::move(snapshot), &events](runtime::CancellationToken token) mutable {
            TuiEvent event;
            event.type = TuiEventType::SaveDone;
            event.text = path;
            if (token.cancelled()) {
                event.error = {ErrorCode::Cancelled, "save cancelled: " + path};
            } else {
                event.error = chat::save_session_atomic(path, std::move(snapshot));
            }
            events.push(std::move(event));
        });
    };

    auto start_load = [&](const std::string& path) {
        if (file_job.running()) {
            status = "A file job is already running";
            return;
        }
        file_job.start([path, &events](runtime::CancellationToken token) mutable {
            TuiEvent event;
            event.type = TuiEventType::LoadDone;
            event.text = path;
            if (token.cancelled()) {
                event.error = {ErrorCode::Cancelled, "load cancelled: " + path};
            } else {
                event.error = chat::load_session(path, event.session);
            }
            events.push(std::move(event));
        });
        status = "Loading " + path;
    };

    auto start_models = [&]() {
        if (active_job != ActiveJob::None) {
            status = "A model job is already running";
            return;
        }
        active_job = ActiveJob::Models;
        provider::RequestContext job_context = context;
        model_job.start([job_context, &events](runtime::CancellationToken token) mutable {
            TuiEvent event;
            event.type = TuiEventType::ModelsDone;
            provider::ModelsResult models;
            event.error = provider::list_models(job_context, models, token);
            event.models = std::move(models.model_ids);
            events.push(std::move(event));
        });
        status = "Listing models...";
    };

    auto start_turn = [&](const std::string& prompt) {
        if (active_job != ActiveJob::None) {
            status = "A model job is already running";
            return;
        }
        active_job = ActiveJob::Chat;
        history_scroll = 0;
        pending_user = session.messages.size();
        session.messages.push_back({"user", prompt});
        pending_assistant = session.messages.size();
        session.messages.push_back({"assistant", ""});

        std::vector<provider::Message> request_messages = session.messages;
        request_messages.pop_back();
        provider::RequestContext job_context = context;
        model_job.start([job_context, request_messages = std::move(request_messages), &events](runtime::CancellationToken token) mutable {
            provider::ChatResult chat;
            Error send_error = provider::send_chat_messages(
                job_context,
                request_messages,
                [&](const std::string& delta) -> Error {
                    TuiEvent event;
                    event.type = TuiEventType::Delta;
                    event.text = delta;
                    events.push(std::move(event));
                    if (token.cancelled()) {
                        return {ErrorCode::Cancelled, "chat request cancelled while streaming"};
                    }
                    return ok_error();
                },
                chat,
                token);
            TuiEvent event;
            if (send_error.ok()) {
                event.type = TuiEventType::Done;
                event.chat = std::move(chat);
            } else {
                event.type = TuiEventType::Error;
                event.error = send_error;
            }
            events.push(std::move(event));
        });
        status = "Waiting for response...";
    };

    auto clear_queued_regeneration = [&]() {
        regenerate_after_cancel = false;
        queued_regeneration_prompt.clear();
    };

    auto start_queued_regeneration = [&](size_t erase_from) {
        const std::string prompt = queued_regeneration_prompt;
        clear_queued_regeneration();
        if (trim_ascii(prompt).empty()) {
            status = "No previous user prompt to regenerate";
            return;
        }
        if (erase_from != static_cast<size_t>(-1) && erase_from < session.messages.size()) {
            session.messages.erase(session.messages.begin() + static_cast<long>(erase_from), session.messages.end());
        }
        start_turn(prompt);
        status = "Regenerating...";
    };

    auto cancel_active_request = [&]() {
        if (active_job == ActiveJob::None) {
            return;
        }
        clear_queued_regeneration();
        model_job.cancel();
        status = "Cancelling...";
    };

    auto regenerate_last_turn = [&]() {
        if (active_job == ActiveJob::Models) {
            status = "Cannot regenerate while listing models";
            return;
        }
        if (active_job == ActiveJob::Chat) {
            std::string prompt;
            if (pending_user != static_cast<size_t>(-1) && pending_user < session.messages.size() &&
                session.messages[pending_user].role == "user") {
                prompt = session.messages[pending_user].content;
            } else {
                const RegenerationPlan plan = regeneration_plan_for_session(session);
                if (plan.available) {
                    prompt = plan.prompt;
                }
            }
            if (trim_ascii(prompt).empty()) {
                status = "No previous user prompt to regenerate";
                return;
            }
            regenerate_after_cancel = true;
            queued_regeneration_prompt = prompt;
            model_job.cancel();
            status = "Cancelling before regenerate...";
            return;
        }

        const RegenerationPlan plan = regeneration_plan_for_session(session);
        if (!plan.available || trim_ascii(plan.prompt).empty()) {
            status = "No previous user prompt to regenerate";
            return;
        }
        session.messages.erase(session.messages.begin() + static_cast<long>(plan.erase_from), session.messages.end());
        start_turn(plan.prompt);
        status = "Regenerating...";
    };

    auto handle_command = [&](const std::string& text) {
        if (text == "/quit" || text == "/exit") {
            quit = true;
            return;
        }
        if (text == "/help") {
            status = "Enter sends | Alt+Enter newline | Esc cancels | Ctrl+R regen | Ctrl+T thinking";
            return;
        }
        if (text.rfind("/thinking", 0) == 0) {
            const std::string requested = trim_ascii(text.substr(9));
            if (requested.empty()) {
                status = std::string("Thinking traces: ") + (show_thinking_traces ? "trace" : "notrace") +
                         ". Use /thinking trace or /thinking notrace";
                return;
            }
            if (requested == "trace") {
                set_thinking_trace_mode(true);
                return;
            }
            if (requested == "notrace") {
                set_thinking_trace_mode(false);
                return;
            }
            status = "Usage: /thinking trace|notrace";
            return;
        }
        if (text.rfind("/theme", 0) == 0) {
            const std::string requested = trim_ascii(text.substr(6));
            if (requested.empty()) {
                status = std::string("Theme: ") + theme_name(theme) + ". Available: dark, light";
                if (!use_colors) {
                    status += " (colors disabled by --nocolors)";
                }
                return;
            }
            ThemeName next = theme;
            if (!parse_theme_name(requested, next)) {
                status = "Unknown theme: " + requested + ". Available: dark, light";
                return;
            }
            theme = next;
            status = std::string("Theme set to ") + theme_name(theme);
            if (!use_colors) {
                status += " (colors disabled by --nocolors)";
            }
            return;
        }
        if (text == "/clear") {
            session.messages.clear();
            apply_system_prompt(session, context.options.system);
            history_scroll = 0;
            status = "Chat history cleared";
            return;
        }
        if (text.rfind("/model", 0) == 0) {
            const std::string model = trim_ascii(text.substr(6));
            if (model.empty()) {
                status = "Usage: /model MODEL";
                return;
            }
            context.options.model = model;
            session.model = model;
            status = "Model set to " + model;
            return;
        }
        if (text.rfind("/system", 0) == 0) {
            replace_system_prompt(session, trim_ascii(text.substr(7)));
            status = "System prompt updated";
            return;
        }
        if (text == "/models") {
            start_models();
            return;
        }
        if (text.rfind("/save", 0) == 0) {
            std::string path = trim_ascii(text.substr(5));
            if (path.empty()) {
                path = context.options.save_chat_path;
            }
            if (path.empty()) {
                status = "Usage: /save PATH";
                return;
            }
            start_save(path, session);
            status = "Saving " + path;
            return;
        }
        if (text.rfind("/load", 0) == 0) {
            const std::string path = trim_ascii(text.substr(5));
            if (path.empty()) {
                status = "Usage: /load PATH";
                return;
            }
            start_load(path);
            return;
        }
        status = "Unknown command: " + text;
    };

    auto submit_input = [&]() {
        const std::string raw = input.text.str();
        const std::string text = trim_ascii(raw);
        if (text.empty()) {
            input = empty_input_editor();
            return;
        }
        if (raw.find('\n') == std::string::npos && text[0] == '/') {
            input = empty_input_editor();
            handle_command(text);
            return;
        }
        if (active_job != ActiveJob::None) {
            status = "A model job is already running";
            return;
        }
        input = empty_input_editor();
        start_turn(raw);
    };

    if (!trim_ascii(context.options.prompt).empty()) {
        start_turn(context.options.prompt);
    }

    render(session, input, status, history_scroll, show_thinking_traces, RenderStyle{theme, use_colors});
    while (!quit) {
        TuiEvent event;
        while (events.try_pop(event)) {
            switch (event.type) {
                case TuiEventType::Delta:
                    if (pending_assistant != static_cast<size_t>(-1) && pending_assistant < session.messages.size()) {
                        session.messages[pending_assistant].content += event.text;
                    }
                    status = pending_assistant_is_hidden_thinking() ? "thinking..." : "Streaming...";
                    break;
                case TuiEventType::Done: {
                    model_job.join();
                    const bool should_regenerate = regenerate_after_cancel;
                    const size_t regenerate_erase_from = pending_user;
                    if (pending_assistant != static_cast<size_t>(-1) && pending_assistant < session.messages.size()) {
                        session.messages[pending_assistant].content = event.chat.content;
                    }
                    if (!event.chat.model.empty()) {
                        context.options.model = event.chat.model;
                        session.model = event.chat.model;
                    }
                    if (!event.chat.usage_json.empty() && event.chat.usage_json != "null") {
                        session.usage_json = event.chat.usage_json;
                    }
                    pending_user = static_cast<size_t>(-1);
                    pending_assistant = static_cast<size_t>(-1);
                    active_job = ActiveJob::None;
                    if (should_regenerate) {
                        start_queued_regeneration(regenerate_erase_from);
                    } else {
                        status = "Ready";
                        start_save(context.options.save_chat_path, session);
                    }
                    break;
                }
                case TuiEventType::Error: {
                    model_job.join();
                    const bool should_regenerate = regenerate_after_cancel && event.error.code == ErrorCode::Cancelled;
                    active_job = ActiveJob::None;
                    if (should_regenerate) {
                        rollback_pending_turn();
                        start_queued_regeneration(static_cast<size_t>(-1));
                    } else {
                        clear_queued_regeneration();
                        if (event.error.code == ErrorCode::Cancelled) {
                            keep_cancelled_turn();
                            status = "Cancelled";
                        } else {
                            rollback_pending_turn();
                            status = error_line(event.error);
                        }
                    }
                    break;
                }
                case TuiEventType::SaveDone:
                    file_job.join();
                    status = event.error.ok() ? "Saved " + event.text : error_line(event.error);
                    break;
                case TuiEventType::LoadDone:
                    file_job.join();
                    if (event.error.ok()) {
                        session = std::move(event.session);
                        refresh_session_metadata(session, context);
                        history_scroll = 0;
                        status = "Loaded " + event.text;
                    } else {
                        status = error_line(event.error);
                    }
                    break;
                case TuiEventType::ModelsDone:
                    model_job.join();
                    active_job = ActiveJob::None;
                    status = event.error.ok() ? join_models_preview(event.models) : error_line(event.error);
                    break;
            }
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        timeval timeout{};
        timeout.tv_usec = 50000;
        const int ready = select(STDIN_FILENO + 1, &readfds, nullptr, nullptr, &timeout);
        if (ready < 0 && errno != EINTR) {
            status = std::string("terminal input error: ") + std::strerror(errno);
        }
        if (ready > 0 && FD_ISSET(STDIN_FILENO, &readfds)) {
            unsigned char ch = 0;
            while (read(STDIN_FILENO, &ch, 1) == 1) {
                if (ch == 27) {
                    const TuiSize terminal = terminal_size();
                    const bool handled = handle_escape(input, layout_for_terminal(terminal.rows, terminal.cols), history_scroll, status);
                    if (!handled) {
                        cancel_active_request();
                    }
                    continue;
                }
                if (ch == 3) {
                    if (active_job != ActiveJob::None) {
                        cancel_active_request();
                    } else if (file_job.running()) {
                        file_job.cancel();
                        status = "Cancelling file job...";
                    } else {
                        quit = true;
                    }
                    continue;
                }
                if (ch == 18) {
                    regenerate_last_turn();
                    continue;
                }
                if (ch == 20) {
                    set_thinking_trace_mode(!show_thinking_traces);
                    continue;
                }
                if (ch == 19) {
                    submit_input();
                    continue;
                }
                if (ch == 4 && input.text.empty()) {
                    quit = true;
                    continue;
                }
                if (ch == 127 || ch == 8) {
                    set_status_from_error(input.erase_before_cursor(), status);
                    continue;
                }
                if (ch == '\r' || ch == '\n') {
                    submit_input();
                    continue;
                }
                if (ch >= 32 || ch == '\t') {
                    insert_input(input, std::string(1, static_cast<char>(ch)), status);
                }
            }
        }
        render(session, input, status, history_scroll, show_thinking_traces, RenderStyle{theme, use_colors});
    }

    model_job.cancel();
    file_job.cancel();
    model_job.join();
    file_job.join();
    return 0;
}

}  // namespace pkchat::tui
