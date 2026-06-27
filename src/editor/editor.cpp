#include "editor/editor.hpp"

#include "editor/ai_continue.hpp"
#include "editor/editor_assist.hpp"
#include "editor/clipboard.hpp"
#include "editor/selection.hpp"
#include "editor/terminal_input.hpp"
#include "runtime/runtime.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

namespace pkchat::editor {
namespace {

constexpr size_t kTabStop = 4;

struct DecodedChar {
    uint32_t codepoint = 0xFFFDU;
    size_t length = 1;
    bool valid = false;
};

bool is_continuation_byte(unsigned char ch) {
    return (ch & 0xC0U) == 0x80U;
}

DecodedChar decode_utf8_at(const std::string& text, size_t pos) {
    if (pos >= text.size()) {
        return {};
    }

    const unsigned char b0 = static_cast<unsigned char>(text[pos]);
    if (b0 < 0x80U) {
        return {b0, 1, true};
    }

    size_t length = 0;
    uint32_t codepoint = 0;
    uint32_t minimum = 0;
    if ((b0 & 0xE0U) == 0xC0U) {
        length = 2;
        codepoint = b0 & 0x1FU;
        minimum = 0x80U;
    } else if ((b0 & 0xF0U) == 0xE0U) {
        length = 3;
        codepoint = b0 & 0x0FU;
        minimum = 0x800U;
    } else if ((b0 & 0xF8U) == 0xF0U) {
        length = 4;
        codepoint = b0 & 0x07U;
        minimum = 0x10000U;
    } else {
        return {0xFFFDU, 1, false};
    }

    if (pos + length > text.size()) {
        return {0xFFFDU, 1, false};
    }
    for (size_t i = 1; i < length; ++i) {
        const unsigned char byte = static_cast<unsigned char>(text[pos + i]);
        if (!is_continuation_byte(byte)) {
            return {0xFFFDU, 1, false};
        }
        codepoint = (codepoint << 6U) | static_cast<uint32_t>(byte & 0x3FU);
    }

    if (codepoint < minimum || codepoint > 0x10FFFFU ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
        return {0xFFFDU, 1, false};
    }
    return {codepoint, length, true};
}

bool is_combining_mark(uint32_t codepoint) {
    return (codepoint >= 0x0300U && codepoint <= 0x036FU) ||
           (codepoint >= 0x0591U && codepoint <= 0x05BDU) ||
           codepoint == 0x05BFU ||
           (codepoint >= 0x05C1U && codepoint <= 0x05C2U) ||
           (codepoint >= 0x05C4U && codepoint <= 0x05C5U) ||
           codepoint == 0x05C7U ||
           (codepoint >= 0x0610U && codepoint <= 0x061AU) ||
           (codepoint >= 0x064BU && codepoint <= 0x065FU) ||
           codepoint == 0x0670U ||
           (codepoint >= 0x06D6U && codepoint <= 0x06DCU) ||
           (codepoint >= 0x06DFU && codepoint <= 0x06E4U) ||
           (codepoint >= 0x06E7U && codepoint <= 0x06E8U) ||
           (codepoint >= 0x06EAU && codepoint <= 0x06EDU) ||
           (codepoint >= 0x1AB0U && codepoint <= 0x1AFFU) ||
           (codepoint >= 0x1DC0U && codepoint <= 0x1DFFU) ||
           (codepoint >= 0x20D0U && codepoint <= 0x20FFU) ||
           (codepoint >= 0xFE20U && codepoint <= 0xFE2FU);
}

bool is_variation_selector(uint32_t codepoint) {
    return (codepoint >= 0xFE00U && codepoint <= 0xFE0FU) ||
           (codepoint >= 0xE0100U && codepoint <= 0xE01EFU);
}

bool is_emoji_modifier(uint32_t codepoint) {
    return codepoint >= 0x1F3FBU && codepoint <= 0x1F3FFU;
}

bool is_wide_codepoint(uint32_t codepoint) {
    return (codepoint >= 0x1100U && codepoint <= 0x115FU) ||
           (codepoint >= 0x2329U && codepoint <= 0x232AU) ||
           (codepoint >= 0x2E80U && codepoint <= 0xA4CFU) ||
           (codepoint >= 0xAC00U && codepoint <= 0xD7A3U) ||
           (codepoint >= 0xF900U && codepoint <= 0xFAFFU) ||
           (codepoint >= 0xFE10U && codepoint <= 0xFE19U) ||
           (codepoint >= 0xFE30U && codepoint <= 0xFE6FU) ||
           (codepoint >= 0xFF00U && codepoint <= 0xFF60U) ||
           (codepoint >= 0xFFE0U && codepoint <= 0xFFE6U) ||
           (codepoint >= 0x1F000U && codepoint <= 0x1FAFFU);
}

bool is_control_codepoint(uint32_t codepoint) {
    return codepoint < 0x20U || (codepoint >= 0x7FU && codepoint < 0xA0U);
}

size_t next_grapheme_offset(const std::string& text, size_t pos) {
    if (pos >= text.size()) {
        return text.size();
    }

    DecodedChar current = decode_utf8_at(text, pos);
    size_t next = pos + current.length;
    if (!current.valid) {
        return next;
    }

    while (next < text.size()) {
        const DecodedChar decoded = decode_utf8_at(text, next);
        if (!decoded.valid) {
            break;
        }
        if (is_combining_mark(decoded.codepoint) || is_variation_selector(decoded.codepoint) ||
            is_emoji_modifier(decoded.codepoint)) {
            next += decoded.length;
            current = decoded;
            continue;
        }
        if (decoded.codepoint == 0x200DU) {
            next += decoded.length;
            if (next < text.size()) {
                const DecodedChar joined = decode_utf8_at(text, next);
                if (joined.valid) {
                    next += joined.length;
                    current = joined;
                    continue;
                }
            }
            break;
        }
        break;
    }
    return next;
}

size_t previous_grapheme_offset(const std::string& text, size_t pos) {
    const size_t target = std::min(pos, text.size());
    size_t previous = 0;
    size_t current = 0;
    while (current < target) {
        previous = current;
        const size_t next = next_grapheme_offset(text, current);
        if (next <= current || next >= target) {
            return previous;
        }
        current = next;
    }
    return previous;
}

size_t display_width_at(const std::string& text, size_t pos, size_t column) {
    if (pos >= text.size()) {
        return 0;
    }
    const DecodedChar first = decode_utf8_at(text, pos);
    if (!first.valid || is_control_codepoint(first.codepoint)) {
        return 1;
    }
    if (first.codepoint == '\t') {
        return kTabStop - (column % kTabStop);
    }

    const size_t end = next_grapheme_offset(text, pos);
    size_t width = 0;
    size_t scan = pos;
    while (scan < end) {
        const DecodedChar decoded = decode_utf8_at(text, scan);
        if (!decoded.valid || is_control_codepoint(decoded.codepoint)) {
            return 1;
        }
        if (decoded.codepoint == '\t') {
            return kTabStop - (column % kTabStop);
        }
        if (is_wide_codepoint(decoded.codepoint)) {
            width = std::max<size_t>(width, 2);
        } else if (!is_combining_mark(decoded.codepoint) &&
                   !is_variation_selector(decoded.codepoint) &&
                   !is_emoji_modifier(decoded.codepoint) &&
                   decoded.codepoint != 0x200DU) {
            width = std::max<size_t>(width, 1);
        }
        scan += decoded.length;
    }
    return width;
}

size_t display_column_for_text(const std::string& text, size_t byte_offset) {
    size_t column = 0;
    size_t pos = 0;
    const size_t limit = std::min(byte_offset, text.size());
    while (pos < limit) {
        const size_t next = next_grapheme_offset(text, pos);
        if (next > limit) {
            break;
        }
        column += display_width_at(text, pos, column);
        pos = next;
    }
    return column;
}

size_t byte_offset_for_display_column(const std::string& text, size_t target_column) {
    size_t column = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t width = display_width_at(text, pos, column);
        if (column + width > target_column) {
            break;
        }
        column += width;
        pos = next_grapheme_offset(text, pos);
    }
    return pos;
}

struct WrapSegment {
    size_t start = 0;
    size_t end = 0;
};

size_t display_width_for_range(const std::string& text, size_t start, size_t end) {
    size_t column = 0;
    size_t pos = start;
    const size_t limit = std::min(end, text.size());
    while (pos < limit) {
        const size_t next = next_grapheme_offset(text, pos);
        if (next > limit) {
            break;
        }
        column += display_width_at(text, pos, column);
        pos = next;
    }
    return column;
}

std::vector<WrapSegment> wrap_line_segments(const std::string& text, size_t width) {
    width = std::max<size_t>(1, width);
    if (text.empty()) {
        return {{0, 0}};
    }

    std::vector<WrapSegment> segments;
    size_t start = 0;
    while (start < text.size()) {
        size_t pos = start;
        size_t column = 0;
        size_t last_break = std::string::npos;
        size_t hard_break = start;

        while (pos < text.size()) {
            const size_t grapheme_start = pos;
            const size_t grapheme_end = next_grapheme_offset(text, pos);
            const size_t char_width = display_width_at(text, pos, column);
            if (column + char_width > width) {
                break;
            }
            column += char_width;
            pos = grapheme_end;
            hard_break = pos;
            const DecodedChar decoded = decode_utf8_at(text, grapheme_start);
            if (decoded.valid && (decoded.codepoint == ' ' || decoded.codepoint == '\t')) {
                last_break = pos;
            }
            if (column >= width) {
                break;
            }
        }

        size_t end = hard_break;
        if (pos < text.size() && last_break != std::string::npos && last_break > start) {
            end = last_break;
        }
        if (end <= start) {
            end = text.size();
            if (start < text.size()) {
                end = next_grapheme_offset(text, start);
            }
        }

        segments.push_back({start, end});
        start = end;
    }
    return segments;
}

size_t wrapped_row_count(const std::string& text, size_t width) {
    return wrap_line_segments(text, width).size();
}

struct WrappedCursor {
    size_t row = 0;
    size_t col = 0;
};

WrappedCursor cursor_in_wrapped_line(const std::string& text, size_t byte_offset, size_t width) {
    width = std::max<size_t>(1, width);
    const std::vector<WrapSegment> segments = wrap_line_segments(text, width);
    const size_t clamped = std::min(byte_offset, text.size());
    for (size_t i = 0; i < segments.size(); ++i) {
        const WrapSegment& segment = segments[i];
        if (clamped == segment.end && i + 1 < segments.size()) {
            continue;
        }
        if (clamped <= segment.end || i + 1 == segments.size()) {
            const size_t col = display_width_for_range(text, segment.start, clamped);
            if (clamped == segment.end && col >= width) {
                return {i + 1, 0};
            }
            return {i, std::min(col, width - 1)};
        }
    }
    return {};
}

std::string display_range(const std::string& text, size_t start, size_t end, size_t width) {
    std::string out;
    size_t column = 0;
    size_t visible = 0;
    size_t pos = start;
    const size_t limit = std::min(end, text.size());
    while (pos < limit && visible < width) {
        const DecodedChar decoded = decode_utf8_at(text, pos);
        const size_t next = std::min(next_grapheme_offset(text, pos), limit);
        const size_t char_width = display_width_at(text, pos, column);
        const size_t next_column = column + char_width;

        if (decoded.valid && decoded.codepoint == '\t') {
            for (size_t tab_col = column; tab_col < next_column && visible < width; ++tab_col) {
                out.push_back(' ');
                ++visible;
            }
        } else if (!decoded.valid || is_control_codepoint(decoded.codepoint)) {
            out.push_back('?');
            ++visible;
        } else {
            out.append(text, pos, next - pos);
            visible += char_width;
        }

        column = next_column;
        pos = next;
    }
    while (visible < width) {
        out.push_back(' ');
        ++visible;
    }
    return out;
}

std::string display_range_highlighted(const std::string& text,
                                      size_t start,
                                      size_t end,
                                      size_t width,
                                      size_t global_line_start,
                                      size_t sel_start,
                                      size_t sel_end,
                                      bool highlight_selection) {
    std::string out;
    size_t column = 0;
    size_t visible = 0;
    size_t pos = start;
    const size_t limit = std::min(end, text.size());
    bool highlight_on = false;
    while (pos < limit && visible < width) {
        const size_t global_offset = global_line_start + pos;
        const bool selected = highlight_selection && sel_start < sel_end &&
                              global_offset >= sel_start && global_offset < sel_end;
        if (selected != highlight_on) {
            out += selected ? "\x1b[7m" : "\x1b[0m";
            highlight_on = selected;
        }

        const DecodedChar decoded = decode_utf8_at(text, pos);
        const size_t next = std::min(next_grapheme_offset(text, pos), limit);
        const size_t char_width = display_width_at(text, pos, column);
        const size_t next_column = column + char_width;

        if (decoded.valid && decoded.codepoint == '\t') {
            for (size_t tab_col = column; tab_col < next_column && visible < width; ++tab_col) {
                out.push_back(' ');
                ++visible;
            }
        } else if (!decoded.valid || is_control_codepoint(decoded.codepoint)) {
            out.push_back('?');
            ++visible;
        } else {
            out.append(text, pos, next - pos);
            visible += char_width;
        }

        column = next_column;
        pos = next;
    }
    if (highlight_on) {
        out += "\x1b[0m";
    }
    while (visible < width) {
        out.push_back(' ');
        ++visible;
    }
    return out;
}

size_t byte_offset_for_range_column(const std::string& text, size_t start, size_t end, size_t target_column) {
    size_t column = 0;
    size_t pos = start;
    const size_t limit = std::min(end, text.size());
    while (pos < limit) {
        const size_t width = display_width_at(text, pos, column);
        if (column + width > target_column) {
            break;
        }
        column += width;
        pos = next_grapheme_offset(text, pos);
    }
    return pos;
}

struct WrappedLocation {
    size_t line = 0;
    size_t segment = 0;
};

WrappedLocation wrapped_location_for_offset(const PieceTable& text, size_t offset, size_t width) {
    width = std::max<size_t>(1, width);
    const size_t line = text.line_for_offset(offset);
    const std::string line_text_value = text.line_text(line);
    const size_t line_start = text.line_start(line);
    const size_t local_offset = std::min(offset - line_start, line_text_value.size());
    const std::vector<WrapSegment> segments = wrap_line_segments(line_text_value, width);

    for (size_t i = 0; i < segments.size(); ++i) {
        const WrapSegment& segment = segments[i];
        if (local_offset < segment.end || i + 1 == segments.size()) {
            return {line, i};
        }
        if (local_offset == segment.end && i + 1 < segments.size()) {
            continue;
        }
    }
    return {line, 0};
}

size_t offset_for_wrapped_location(const PieceTable& text,
                                   size_t line,
                                   size_t segment_index,
                                   size_t column,
                                   size_t width) {
    width = std::max<size_t>(1, width);
    const std::string line_text_value = text.line_text(line);
    const std::vector<WrapSegment> segments = wrap_line_segments(line_text_value, width);
    const WrapSegment& segment = segments[std::min(segment_index, segments.size() - 1)];
    return text.line_start(line) + byte_offset_for_range_column(line_text_value, segment.start, segment.end, column);
}

std::string pad_or_clip_ascii(const std::string& text, int width) {
    if (width <= 0) {
        return "";
    }
    std::string out = text.substr(0, static_cast<size_t>(width));
    while (out.size() < static_cast<size_t>(width)) {
        out.push_back(' ');
    }
    return out;
}

struct TerminalSize {
    int rows = 24;
    int cols = 80;
};

TerminalSize terminal_size() {
    TerminalSize size;
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
            return {ErrorCode::BadArgs, "--editor requires an interactive terminal"};
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
        clear_terminal_input_queue();
        std::cout << "\x1b[?1049h\x1b[?25h\x1b[2J\x1b[H" << bracketed_paste_enable_sequence();
        std::cout.flush();
        return ok_error();
    }

    void restore() {
        if (!active_) {
            return;
        }
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_);
        std::cout << bracketed_paste_disable_sequence()
                  << "\x1b[0m\x1b[?25h\x1b[2J\x1b[H\x1b[?1049l";
        std::cout.flush();
        clear_terminal_input_queue();
        active_ = false;
    }

   private:
    termios original_{};
    bool active_ = false;
};

void update_preferred_column(EditorState& state) {
    state.preferred_column = state.text.display_column_for_offset(state.cursor);
}

enum class MinibufferAction {
    None,
    SaveFile,
    LoadFile,
    Search,
    ReplaceSearch,
    ReplaceWith,
    ConfirmLoad,
    ConfirmQuit,
    ConfirmSaveOnQuit,
    ConfirmOverwrite,
    AssistCommand,
    AssistScopeChoice,
    AssistPromptMode,
};

struct PendingSaveRequest {
    std::string path;
    bool update_path = true;
    bool quit_after_save = false;
};

struct MinibufferState {
    bool active = false;
    MinibufferAction action = MinibufferAction::None;
    std::string prompt;
    std::string input;
    std::string message = "Ready";
};

struct PendingAssist {
    AssistCommandKind kind = AssistCommandKind::Unknown;
    size_t command_index = 0;
    std::string custom_prompt;
};

struct ReplaceSession {
    bool active = false;
    bool match_valid = false;
    size_t match_start = 0;
    std::string needle;
    std::string replacement;
};

std::string trim_ascii_copy(std::string text) {
    auto is_ws = [](unsigned char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    };
    while (!text.empty() && is_ws(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
    }
    while (!text.empty() && is_ws(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    return text;
}

std::string editor_status_line(const EditorState& state) {
    std::ostringstream out;
    out << (state.path.empty() ? "[scratch]" : state.path);
    if (state.dirty) {
        out << " *";
    }
    const size_t line = state.text.line_for_offset(state.cursor) + 1;
    const size_t column = state.text.display_column_for_offset(state.cursor) + 1;
    out << "  Mode: Editor"
        << "  Ln " << line << ", Col " << column
        << "  Esc /command | Ctrl+Space continue | Ctrl+C copy | Ctrl+X cut | Ctrl+V paste | Ctrl+U undo | Ctrl+S save | Ctrl+Q quit";
    return out.str();
}

std::string minibuffer_text(const MinibufferState& minibuffer) {
    if (minibuffer.active) {
        return minibuffer.prompt + minibuffer.input;
    }
    return minibuffer.message;
}

void minibuffer_message(MinibufferState& minibuffer, std::string message) {
    minibuffer.active = false;
    minibuffer.action = MinibufferAction::None;
    minibuffer.prompt.clear();
    minibuffer.input.clear();
    minibuffer.message = std::move(message);
}

void start_minibuffer(MinibufferState& minibuffer,
                      MinibufferAction action,
                      std::string prompt,
                      std::string initial = "") {
    minibuffer.active = true;
    minibuffer.action = action;
    minibuffer.prompt = std::move(prompt);
    minibuffer.input = std::move(initial);
}

bool is_assist_minibuffer_action(MinibufferAction action) {
    return action == MinibufferAction::AssistCommand || action == MinibufferAction::AssistScopeChoice ||
           action == MinibufferAction::AssistPromptMode;
}

void exit_assist_command_mode(MinibufferState& minibuffer, AssistCompleterState& completer) {
    completer = AssistCompleterState{};
    minibuffer_message(minibuffer, "Ready");
}

void start_assist_command_mode(MinibufferState& minibuffer, AssistCompleterState& completer) {
    completer = AssistCompleterState{};
    start_minibuffer(minibuffer, MinibufferAction::AssistCommand, "Command: ", "");
}

void reset_editor_buffer(EditorState& state, PieceTable text, std::string path) {
    state.text = std::move(text);
    state.cursor = 0;
    state.preferred_column = 0;
    state.scroll_line = 0;
    state.scroll_column = 0;
    state.path = std::move(path);
    state.dirty = false;
    state.clear_undo_history();
    state.clear_selection();
}

bool editor_target_exists(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::error_code filesystem_error;
    return std::filesystem::exists(path, filesystem_error) &&
           std::filesystem::is_regular_file(path, filesystem_error);
}

bool needs_overwrite_confirm(const std::string& target, const std::string& current_path) {
    if (!editor_target_exists(target)) {
        return false;
    }
    return current_path.empty() || target != current_path;
}

std::string overwrite_prompt_message(const std::string& path) {
    return path + " exists. Do you want to overwrite it (yes/no) ? ";
}

void save_editor_to_path(EditorState& state,
                         const std::string& path,
                         MinibufferState& minibuffer,
                         bool update_path) {
    Error save_error = save_file(path, state.text);
    if (save_error.ok()) {
        state.dirty = false;
        if (update_path) {
            state.path = path;
        }
        minibuffer_message(minibuffer, "Saved " + path);
    } else {
        minibuffer_message(minibuffer, save_error.message);
    }
}

void request_save_editor_to_path(EditorState& state,
                                 const std::string& path,
                                 MinibufferState& minibuffer,
                                 bool update_path,
                                 bool quit_after_save,
                                 bool& quit,
                                 PendingSaveRequest& pending_save) {
    if (needs_overwrite_confirm(path, state.path)) {
        pending_save.path = path;
        pending_save.update_path = update_path;
        pending_save.quit_after_save = quit_after_save;
        start_minibuffer(minibuffer,
                         MinibufferAction::ConfirmOverwrite,
                         overwrite_prompt_message(path));
        return;
    }
    save_editor_to_path(state, path, minibuffer, update_path);
    if (quit_after_save && !state.dirty) {
        quit = true;
    }
}

bool yes_answer(const std::string& value) {
    return value == "y" || value == "Y" || value == "yes" || value == "YES";
}

bool no_answer(const std::string& value) {
    return value == "n" || value == "N" || value == "no" || value == "NO";
}

std::string file_size_warning_message(const std::string& path, std::uintmax_t size) {
    return "Warning: " + path + " is " + std::to_string(size) + " bytes";
}

void load_editor_from_path(EditorState& state,
                           const std::string& path,
                           const EditorSettings& settings,
                           MinibufferState& minibuffer) {
    PieceTable loaded;
    Error load_error = load_file(path, settings, loaded);
    if (load_error.ok()) {
        reset_editor_buffer(state, std::move(loaded), path);
        minibuffer_message(minibuffer, "Loaded " + path);
    } else {
        minibuffer_message(minibuffer, load_error.message);
    }
}

void request_load_editor_from_path(EditorState& state,
                                   const std::string& path,
                                   const EditorSettings& settings,
                                   MinibufferState& minibuffer,
                                   std::string& pending_load_path) {
    FileLoadCheck check;
    Error err = check_load_file_size(path, settings, check);
    if (!err.ok()) {
        minibuffer_message(minibuffer, err.message);
        return;
    }
    if (check.should_warn) {
        pending_load_path = path;
        start_minibuffer(minibuffer,
                         MinibufferAction::ConfirmLoad,
                         file_size_warning_message(path, check.size) + "; load anyway? (y/n) ");
        return;
    }
    load_editor_from_path(state, path, settings, minibuffer);
}

bool confirm_huge_load_before_terminal(const std::string& path, const FileLoadCheck& check) {
    std::cerr << file_size_warning_message(path, check.size)
              << ". Load anyway? [y/N] ";
    std::cerr.flush();
    std::string response;
    if (!std::getline(std::cin, response)) {
        return false;
    }
    return yes_answer(trim_ascii_copy(response));
}

std::string search_found_message(const std::string& needle) {
    return "Found: " + needle;
}

std::string search_not_found_message(const std::string& needle) {
    return "Search not found: " + needle;
}

std::string replacement_count_message(size_t replacements) {
    std::ostringstream out;
    out << "Replaced " << replacements << " occurrence";
    if (replacements != 1) {
        out << "s";
    }
    return out.str();
}

std::string replace_prompt_message(const std::string& needle) {
    return "Replace '" + needle + "': Space replace | s skip | a all | Esc done";
}

void finish_replace_session(ReplaceSession& replace, MinibufferState& minibuffer, std::string message) {
    replace = ReplaceSession{};
    minibuffer_message(minibuffer, std::move(message));
}

bool find_replace_match(EditorState& state, ReplaceSession& replace, size_t start) {
    if (replace.needle.empty()) {
        return false;
    }
    const std::string haystack = state.text.str();
    if (haystack.empty()) {
        return false;
    }
    const size_t found = haystack.find(replace.needle, std::min(start, haystack.size()));
    if (found == std::string::npos) {
        replace.match_valid = false;
        return false;
    }
    replace.active = true;
    replace.match_valid = true;
    replace.match_start = found;
    state.cursor = found;
    update_preferred_column(state);
    return true;
}

void begin_replace_choices(EditorState& state, MinibufferState& minibuffer, ReplaceSession& replace) {
    minibuffer.active = false;
    minibuffer.action = MinibufferAction::None;
    minibuffer.prompt.clear();
    minibuffer.input.clear();
    if (find_replace_match(state, replace, state.cursor)) {
        minibuffer.message = replace_prompt_message(replace.needle);
    } else {
        finish_replace_session(replace, minibuffer, search_not_found_message(replace.needle));
    }
}

void submit_minibuffer(EditorState& state,
                       MinibufferState& minibuffer,
                       bool& quit,
                       std::string& last_search,
                       ReplaceSession& replace,
                       const EditorSettings& settings,
                       std::string& pending_load_path,
                       bool& pending_quit_after_save,
                       PendingSaveRequest& pending_save) {
    const MinibufferAction action = minibuffer.action;
    const std::string value = trim_ascii_copy(minibuffer.input);
    const std::string raw_value = minibuffer.input;
    if (action == MinibufferAction::SaveFile) {
        if (value.empty()) {
            minibuffer.prompt = pending_quit_after_save ? "Save file before quit (path required): "
                                                        : "Save file (path required): ";
            return;
        }
        request_save_editor_to_path(state,
                                    value,
                                    minibuffer,
                                    true,
                                    pending_quit_after_save,
                                    quit,
                                    pending_save);
        if (!pending_save.path.empty()) {
            return;
        }
        pending_quit_after_save = false;
        return;
    }
    if (action == MinibufferAction::LoadFile) {
        if (value.empty()) {
            minibuffer.prompt = "Load file (path required): ";
            return;
        }
        request_load_editor_from_path(state, value, settings, minibuffer, pending_load_path);
        return;
    }
    if (action == MinibufferAction::Search) {
        if (value.empty()) {
            minibuffer.prompt = "Search (substring required): ";
            return;
        }
        last_search = value;
        minibuffer_message(minibuffer,
                           state.search(value) ? search_found_message(value)
                                               : search_not_found_message(value));
        return;
    }
    if (action == MinibufferAction::ReplaceSearch) {
        if (raw_value.empty()) {
            minibuffer.prompt = "Replace search (substring required): ";
            return;
        }
        replace = ReplaceSession{};
        replace.active = true;
        replace.needle = raw_value;
        last_search = raw_value;
        start_minibuffer(minibuffer, MinibufferAction::ReplaceWith, "Replace with: ");
        return;
    }
    if (action == MinibufferAction::ReplaceWith) {
        replace.replacement = raw_value;
        begin_replace_choices(state, minibuffer, replace);
        return;
    }
    if (action == MinibufferAction::ConfirmLoad) {
        if (yes_answer(value)) {
            const std::string path = pending_load_path;
            pending_load_path.clear();
            load_editor_from_path(state, path, settings, minibuffer);
        } else if (no_answer(value) || value.empty()) {
            pending_load_path.clear();
            minibuffer_message(minibuffer, "Load cancelled");
        } else {
            minibuffer.prompt = "Type y or n: ";
            minibuffer.input.clear();
        }
        return;
    }
    if (action == MinibufferAction::ConfirmQuit) {
        if (yes_answer(value)) {
            quit = true;
        } else if (no_answer(value) || value.empty()) {
            minibuffer_message(minibuffer, "Quit cancelled");
        } else {
            minibuffer.prompt = "Type y or n: ";
            minibuffer.input.clear();
        }
        return;
    }
    if (action == MinibufferAction::ConfirmSaveOnQuit) {
        if (yes_answer(value)) {
            pending_quit_after_save = true;
            start_minibuffer(minibuffer, MinibufferAction::SaveFile, "Save file: ");
        } else if (no_answer(value) || value.empty()) {
            quit = true;
        } else {
            minibuffer.prompt = "Type y or n: ";
            minibuffer.input.clear();
        }
        return;
    }
    if (action == MinibufferAction::ConfirmOverwrite) {
        if (yes_answer(value)) {
            const PendingSaveRequest request = pending_save;
            pending_save = PendingSaveRequest{};
            save_editor_to_path(state, request.path, minibuffer, request.update_path);
            if (request.quit_after_save && !state.dirty) {
                quit = true;
            }
            pending_quit_after_save = false;
        } else if (no_answer(value) || value.empty()) {
            pending_save = PendingSaveRequest{};
            pending_quit_after_save = false;
            minibuffer_message(minibuffer, "Save cancelled");
        } else {
            minibuffer.prompt = "Type y or n: ";
            minibuffer.input.clear();
        }
        return;
    }
    minibuffer_message(minibuffer, "");
}

bool handle_minibuffer_key(EditorState& state,
                           MinibufferState& minibuffer,
                           unsigned char ch,
                           bool& quit,
                           std::string& last_search,
                           ReplaceSession& replace,
                           const EditorSettings& settings,
                           std::string& pending_load_path,
                           bool& pending_quit_after_save,
                           PendingSaveRequest& pending_save) {
    if (!minibuffer.active) {
        return false;
    }
    if (is_assist_minibuffer_action(minibuffer.action)) {
        return false;
    }
    if (ch == 27 || ch == 7) {
        replace = ReplaceSession{};
        pending_load_path.clear();
        pending_quit_after_save = false;
        pending_save = PendingSaveRequest{};
        minibuffer_message(minibuffer, "Minibuffer cancelled");
        return true;
    }
    if (ch == '\r' || ch == '\n') {
        submit_minibuffer(state,
                          minibuffer,
                          quit,
                          last_search,
                          replace,
                          settings,
                          pending_load_path,
                          pending_quit_after_save,
                          pending_save);
        return true;
    }
    if (ch == 127 || ch == 8) {
        if (!minibuffer.input.empty()) {
            minibuffer.input.pop_back();
        }
        return true;
    }
    if (minibuffer.action == MinibufferAction::ConfirmQuit ||
        minibuffer.action == MinibufferAction::ConfirmLoad ||
        minibuffer.action == MinibufferAction::ConfirmSaveOnQuit ||
        minibuffer.action == MinibufferAction::ConfirmOverwrite) {
        if (ch == 'y' || ch == 'Y') {
            if (minibuffer.action == MinibufferAction::ConfirmQuit) {
                quit = true;
            } else if (minibuffer.action == MinibufferAction::ConfirmLoad) {
                const std::string path = pending_load_path;
                pending_load_path.clear();
                load_editor_from_path(state, path, settings, minibuffer);
            } else if (minibuffer.action == MinibufferAction::ConfirmSaveOnQuit) {
                pending_quit_after_save = true;
                start_minibuffer(minibuffer, MinibufferAction::SaveFile, "Save file: ");
            } else {
                const PendingSaveRequest request = pending_save;
                pending_save = PendingSaveRequest{};
                save_editor_to_path(state, request.path, minibuffer, request.update_path);
                if (request.quit_after_save && !state.dirty) {
                    quit = true;
                }
                pending_quit_after_save = false;
            }
        } else if (ch == 'n' || ch == 'N') {
            if (minibuffer.action == MinibufferAction::ConfirmLoad) {
                pending_load_path.clear();
                minibuffer_message(minibuffer, "Load cancelled");
            } else if (minibuffer.action == MinibufferAction::ConfirmSaveOnQuit) {
                quit = true;
            } else if (minibuffer.action == MinibufferAction::ConfirmOverwrite) {
                pending_save = PendingSaveRequest{};
                pending_quit_after_save = false;
                minibuffer_message(minibuffer, "Save cancelled");
            } else {
                minibuffer_message(minibuffer, "Quit cancelled");
            }
        } else {
            minibuffer.prompt = "Type y or n: ";
        }
        return true;
    }
    if (ch == '\t') {
        minibuffer.prompt = "Tab completion disabled; enter path: ";
        return true;
    }
    if (ch >= 0x20U) {
        minibuffer.input.push_back(static_cast<char>(ch));
        return true;
    }
    return true;
}

void discard_escape_sequence_tail() {
    unsigned char ch = 0;
    std::string sequence;
    while (sequence.size() < 16 && read_terminal_byte(ch, 1)) {
        sequence.push_back(static_cast<char>(ch));
        if (sequence.size() == 1 && ch == 'O') {
            continue;
        }
        if ((ch >= 'A' && ch <= 'Z') || ch == '~') {
            break;
        }
    }
}

void replace_current_match(EditorState& state, MinibufferState& minibuffer, ReplaceSession& replace) {
    if (!replace.match_valid && !find_replace_match(state, replace, state.cursor)) {
        finish_replace_session(replace, minibuffer, "Replace complete");
        return;
    }

    const size_t replaced_at = replace.match_start;
    Error err = state.replace(replaced_at, replace.needle.size(), replace.replacement);
    if (!err.ok()) {
        finish_replace_session(replace, minibuffer, err.message);
        return;
    }

    const size_t next_start = replaced_at + replace.replacement.size();
    if (find_replace_match(state, replace, next_start)) {
        minibuffer.message = replacement_count_message(1) + ". " + replace_prompt_message(replace.needle);
    } else {
        finish_replace_session(replace, minibuffer, replacement_count_message(1) + "; no more matches");
    }
}

void skip_current_match(EditorState& state, MinibufferState& minibuffer, ReplaceSession& replace) {
    if (!replace.match_valid && !find_replace_match(state, replace, state.cursor)) {
        finish_replace_session(replace, minibuffer, "Replace complete");
        return;
    }

    const size_t next_start = replace.match_start + replace.needle.size();
    if (find_replace_match(state, replace, next_start)) {
        minibuffer.message = replace_prompt_message(replace.needle);
    } else {
        finish_replace_session(replace, minibuffer, "Replace complete");
    }
}

void replace_all_remaining(EditorState& state, MinibufferState& minibuffer, ReplaceSession& replace) {
    const size_t start = replace.match_valid ? replace.match_start : state.cursor;
    size_t replacements = 0;
    Error err = state.replace_all_from(start, replace.needle, replace.replacement, replacements);
    if (!err.ok()) {
        finish_replace_session(replace, minibuffer, err.message);
        return;
    }
    finish_replace_session(replace, minibuffer, replacement_count_message(replacements));
}

bool handle_replace_key(EditorState& state,
                        MinibufferState& minibuffer,
                        ReplaceSession& replace,
                        unsigned char ch) {
    if (!replace.active || minibuffer.active) {
        return false;
    }
    if (ch == 27 || ch == 7) {
        discard_escape_sequence_tail();
        finish_replace_session(replace, minibuffer, "Replace ended");
        return true;
    }
    if (ch == ' ') {
        replace_current_match(state, minibuffer, replace);
        return true;
    }
    if (ch == 's' || ch == 'S') {
        skip_current_match(state, minibuffer, replace);
        return true;
    }
    if (ch == 'a' || ch == 'A') {
        replace_all_remaining(state, minibuffer, replace);
        return true;
    }
    minibuffer.message = replace_prompt_message(replace.needle);
    return true;
}

void render_terminal(EditorState& state, const MinibufferState& minibuffer) {
    const TerminalSize size = terminal_size();
    const int rows = std::max(3, size.rows);
    const int cols = std::max(20, size.cols);
    const int width = std::max(1, cols - 1);
    Rect panel_rect{1, 1, std::max(1, rows - 2), width};
    state.ensure_cursor_visible(panel_rect);
    const RenderedPanel panel = state.render(panel_rect);

    std::cout << "\x1b[?25l";
    for (int row = 0; row < panel_rect.height; ++row) {
        std::cout << "\x1b[" << (panel_rect.row + row) << ";" << panel_rect.col << "H\x1b[K";
        if (row < static_cast<int>(panel.lines.size())) {
            std::cout << panel.lines[static_cast<size_t>(row)];
        }
    }

    const int status_row = rows - 1;
    const int minibuffer_row = rows;
    std::cout << "\x1b[" << status_row << ";1H\x1b[7m"
              << pad_or_clip_ascii(editor_status_line(state), width) << "\x1b[0m\x1b[K";
    std::cout << "\x1b[" << minibuffer_row << ";1H"
              << pad_or_clip_ascii(minibuffer_text(minibuffer), width) << "\x1b[K";

    int cursor_row = panel.cursor.visible ? panel_rect.row + panel.cursor.row : panel_rect.row;
    int cursor_col = panel.cursor.visible ? panel_rect.col + panel.cursor.col : panel_rect.col;
    if (minibuffer.active) {
        cursor_row = minibuffer_row;
        const size_t prompt_width = minibuffer.prompt.size();
        const size_t input_width = minibuffer.input.size();
        cursor_col = 1 + static_cast<int>(std::min<size_t>(
            static_cast<size_t>(std::max(0, width - 1)), prompt_width + input_width));
    }
    std::cout << "\x1b[" << cursor_row << ";" << cursor_col << "H\x1b[?25h";
    std::cout.flush();
}

std::string read_escape_suffix() {
    std::string sequence;
    unsigned char ch = 0;
    while (sequence.size() < 16 && read_terminal_byte(ch, 25)) {
        sequence.push_back(static_cast<char>(ch));
        if (sequence.size() == 1 && ch == 'O') {
            continue;
        }
        if ((ch >= 'A' && ch <= 'Z') || ch == '~') {
            break;
        }
    }
    return sequence;
}

void dispatch_escape_sequence(EditorState& state,
                            const std::string& sequence,
                            std::string& status,
                            const std::string& last_search) {
    if (sequence.empty()) {
        return;
    }

    MovementKeyEvent movement;
    if (parse_movement_sequence(sequence, movement)) {
        const TerminalSize size = terminal_size();
        const Rect panel_rect{1, 1, std::max(1, size.rows - 2), std::max(1, size.cols - 1)};
        state.apply_movement(movement.key, panel_rect, movement.shift);
        return;
    }

    if (sequence == "[3~") {
        Error err = state.erase_at_cursor();
        if (!err.ok()) {
            status = err.message;
        }
    } else if (sequence == "OR" || sequence == "[13~" || sequence == "[[C") {
        if (last_search.empty()) {
            status = "No search string; use Ctrl+F";
        } else {
            status = state.search_next(last_search) ? search_found_message(last_search)
                                                    : search_not_found_message(last_search);
        }
    } else if (sequence == "[1;2R" || sequence == "O1;2R" ||
               sequence == "[13;2~" || sequence == "[25~") {
        if (last_search.empty()) {
            status = "No search string; use Ctrl+F";
        } else {
            status = state.search_previous(last_search) ? search_found_message(last_search)
                                                        : search_not_found_message(last_search);
        }
    }
}

void handle_escape(EditorState& state, std::string& status, const std::string& last_search) {
    dispatch_escape_sequence(state, read_escape_suffix(), status, last_search);
}

}  // namespace

PieceTable PieceTable::from_string(std::string original) {
    PieceTable table;
    table.total_size_ = original.size();
    table.original_ = std::move(original);
    if (table.total_size_ != 0) {
        table.pieces_.push_back({Source::Original, 0, table.total_size_});
    }
    table.invalidate_line_cache();
    return table;
}

std::string PieceTable::str() const {
    std::string out;
    out.reserve(total_size_);
    for (const Piece& piece : pieces_) {
        const std::string& src = source_for(piece);
        out.append(src, piece.start, piece.length);
    }
    return out;
}

Error PieceTable::write_to(std::ostream& out) const {
    for (const Piece& piece : pieces_) {
        const std::string& src = source_for(piece);
        out.write(src.data() + static_cast<std::streamoff>(piece.start),
                  static_cast<std::streamsize>(piece.length));
        if (!out) {
            return {ErrorCode::FileWrite, "failed while writing editor buffer"};
        }
    }
    return ok_error();
}

char PieceTable::char_at(size_t pos) const {
    if (pos >= total_size_) {
        return '\0';
    }
    size_t offset = 0;
    for (const Piece& piece : pieces_) {
        if (pos < offset + piece.length) {
            return source_for(piece)[piece.start + (pos - offset)];
        }
        offset += piece.length;
    }
    return '\0';
}

Error PieceTable::insert(size_t pos, const std::string& text) {
    if (pos > total_size_) {
        return {ErrorCode::BadArgs, "editor insert position is past the end of the buffer"};
    }
    if (text.empty()) {
        return ok_error();
    }

    const Piece inserted{Source::Add, add_.size(), text.size()};
    add_ += text;

    if (pieces_.empty()) {
        pieces_.push_back(inserted);
    } else if (pos == total_size_) {
        pieces_.push_back(inserted);
    } else {
        size_t offset = 0;
        for (size_t i = 0; i < pieces_.size(); ++i) {
            const Piece piece = pieces_[i];
            const size_t end = offset + piece.length;
            if (pos < end) {
                const size_t inside = pos - offset;
                if (inside == 0) {
                    pieces_.insert(pieces_.begin() + static_cast<std::ptrdiff_t>(i), inserted);
                } else {
                    Piece before = piece;
                    before.length = inside;
                    Piece after = piece;
                    after.start += inside;
                    after.length -= inside;
                    pieces_[i] = before;
                    pieces_.insert(pieces_.begin() + static_cast<std::ptrdiff_t>(i + 1), inserted);
                    pieces_.insert(pieces_.begin() + static_cast<std::ptrdiff_t>(i + 2), after);
                }
                break;
            }
            if (pos == end) {
                pieces_.insert(pieces_.begin() + static_cast<std::ptrdiff_t>(i + 1), inserted);
                break;
            }
            offset = end;
        }
    }

    total_size_ += text.size();
    invalidate_line_cache();
    return ok_error();
}

Error PieceTable::erase(size_t pos, size_t count) {
    if (pos > total_size_) {
        return {ErrorCode::BadArgs, "editor erase position is past the end of the buffer"};
    }
    if (count == 0 || pos == total_size_) {
        return ok_error();
    }
    count = std::min(count, total_size_ - pos);
    const size_t erase_end = pos + count;
    std::vector<Piece> kept;
    kept.reserve(pieces_.size());

    size_t offset = 0;
    for (const Piece& piece : pieces_) {
        const size_t piece_begin = offset;
        const size_t piece_end = offset + piece.length;
        if (piece_end <= pos || piece_begin >= erase_end) {
            kept.push_back(piece);
        } else {
            if (pos > piece_begin) {
                Piece left = piece;
                left.length = pos - piece_begin;
                kept.push_back(left);
            }
            if (erase_end < piece_end) {
                Piece right = piece;
                right.start += erase_end - piece_begin;
                right.length = piece_end - erase_end;
                kept.push_back(right);
            }
        }
        offset = piece_end;
    }

    pieces_ = std::move(kept);
    total_size_ -= count;
    invalidate_line_cache();
    return ok_error();
}

size_t PieceTable::previous_char_offset(size_t pos) const {
    if (pos == 0 || total_size_ == 0) {
        return 0;
    }
    const std::string content = str();
    return previous_grapheme_offset(content, std::min(pos, total_size_));
}

size_t PieceTable::next_char_offset(size_t pos) const {
    if (pos >= total_size_) {
        return total_size_;
    }
    const std::string content = str();
    return next_grapheme_offset(content, std::min(pos, total_size_));
}

size_t PieceTable::line_count() const {
    rebuild_line_cache();
    return line_starts_.size();
}

size_t PieceTable::line_start(size_t line) const {
    rebuild_line_cache();
    if (line >= line_starts_.size()) {
        return total_size_;
    }
    return line_starts_[line];
}

size_t PieceTable::line_length(size_t line) const {
    rebuild_line_cache();
    if (line >= line_starts_.size()) {
        return 0;
    }
    const size_t start = line_starts_[line];
    if (line + 1 >= line_starts_.size()) {
        return total_size_ - start;
    }
    return line_starts_[line + 1] - start - 1;
}

size_t PieceTable::line_for_offset(size_t offset) const {
    rebuild_line_cache();
    const size_t clamped = std::min(offset, total_size_);
    const auto it = std::upper_bound(line_starts_.begin(), line_starts_.end(), clamped);
    if (it == line_starts_.begin()) {
        return 0;
    }
    return static_cast<size_t>(std::distance(line_starts_.begin(), it) - 1);
}

size_t PieceTable::display_column_for_offset(size_t offset) const {
    const size_t line = line_for_offset(offset);
    const size_t start = line_start(line);
    const size_t clamped = std::min(offset, total_size_);
    std::string text;
    append_range(text, start, clamped - start);
    return display_column_for_text(text, text.size());
}

size_t PieceTable::offset_for_line_column(size_t line, size_t display_column) const {
    const size_t start = line_start(line);
    const std::string text = line_text(line);
    return start + byte_offset_for_display_column(text, display_column);
}

std::string PieceTable::line_text(size_t line) const {
    const size_t start = line_start(line);
    const size_t length = line_length(line);
    std::string out;
    out.reserve(length);
    append_range(out, start, length);
    return out;
}

std::string PieceTable::range_text(size_t start, size_t length) const {
    std::string out;
    out.reserve(length);
    append_range(out, start, length);
    return out;
}

const std::string& PieceTable::source_for(const Piece& piece) const {
    return piece.source == Source::Original ? original_ : add_;
}

void PieceTable::append_range(std::string& out, size_t start, size_t length) const {
    if (length == 0 || start >= total_size_) {
        return;
    }
    const size_t end = std::min(total_size_, start + length);
    size_t offset = 0;
    for (const Piece& piece : pieces_) {
        const size_t piece_begin = offset;
        const size_t piece_end = offset + piece.length;
        if (piece_end <= start) {
            offset = piece_end;
            continue;
        }
        if (piece_begin >= end) {
            break;
        }
        const size_t local_begin = std::max(start, piece_begin) - piece_begin;
        const size_t local_end = std::min(end, piece_end) - piece_begin;
        out.append(source_for(piece), piece.start + local_begin, local_end - local_begin);
        offset = piece_end;
    }
}

void PieceTable::invalidate_line_cache() {
    line_cache_valid_ = false;
}

void PieceTable::rebuild_line_cache() const {
    if (line_cache_valid_) {
        return;
    }
    line_starts_.clear();
    line_starts_.push_back(0);
    size_t absolute = 0;
    for (const Piece& piece : pieces_) {
        const std::string& src = source_for(piece);
        for (size_t i = 0; i < piece.length; ++i) {
            if (src[piece.start + i] == '\n') {
                line_starts_.push_back(absolute + 1);
            }
            ++absolute;
        }
    }
    line_cache_valid_ = true;
}

EditorState EditorState::from_text(std::string content) {
    EditorState state;
    state.text = PieceTable::from_string(std::move(content));
    state.selection.clear(state.cursor);
    return state;
}

EditorSnapshot EditorState::snapshot() const {
    return {text.str(), cursor, preferred_column, scroll_line, scroll_column};
}

void EditorState::restore_snapshot(const EditorSnapshot& snapshot) {
    text = PieceTable::from_string(snapshot.content);
    cursor = std::min(snapshot.cursor, text.size());
    preferred_column = snapshot.preferred_column;
    scroll_line = snapshot.scroll_line;
    scroll_column = snapshot.scroll_column;
    selection.clear(cursor);
}

void EditorState::remember_undo(EditorSnapshot snapshot) {
    redo_stack_.clear();
    if (undo_limit_ == 0) {
        return;
    }
    while (undo_stack_.size() >= undo_limit_) {
        undo_stack_.erase(undo_stack_.begin());
    }
    undo_stack_.push_back(std::move(snapshot));
}

Error EditorState::insert(const std::string& value) {
    if (value.empty()) {
        return ok_error();
    }
    if (selection.has_range()) {
        Error replaced = replace(selection.start(), selection.end() - selection.start(), value);
        if (replaced.ok()) {
            selection.clear(cursor);
        }
        return replaced;
    }
    EditorSnapshot before = snapshot();
    Error err = text.insert(cursor, value);
    if (!err.ok()) {
        return err;
    }
    remember_undo(std::move(before));
    cursor += value.size();
    dirty = true;
    update_preferred_column(*this);
    return ok_error();
}

Error EditorState::insert_without_undo(const std::string& value) {
    if (value.empty()) {
        return ok_error();
    }
    Error err = text.insert(cursor, value);
    if (!err.ok()) {
        return err;
    }
    cursor += value.size();
    dirty = true;
    update_preferred_column(*this);
    return ok_error();
}

void EditorState::finalize_stream_edit(const EditorSnapshot& before) {
    if (snapshot().content != before.content || cursor != before.cursor) {
        remember_undo(before);
    }
}

EditorSnapshot EditorState::capture_state() const {
    return snapshot();
}

Error EditorState::replace(size_t pos, size_t count, const std::string& value) {
    if (pos > text.size()) {
        return {ErrorCode::BadArgs, "editor replace position is past the end of the buffer"};
    }
    count = std::min(count, text.size() - pos);
    const std::string before_text = text.str();
    if (before_text.substr(pos, count) == value) {
        return ok_error();
    }

    EditorSnapshot before{before_text, cursor, preferred_column, scroll_line, scroll_column};
    PieceTable replacement = text;
    Error err = replacement.erase(pos, count);
    if (!err.ok()) {
        return err;
    }
    err = replacement.insert(pos, value);
    if (!err.ok()) {
        return err;
    }
    text = std::move(replacement);
    remember_undo(std::move(before));
    cursor = pos + value.size();
    selection.clear(cursor);
    dirty = true;
    update_preferred_column(*this);
    return ok_error();
}

Error EditorState::erase_before_cursor() {
    if (selection.has_range()) {
        const size_t start = selection.start();
        Error err = replace(start, selection.end() - start, "");
        if (err.ok()) {
            cursor = start;
            selection.clear(cursor);
        }
        return err;
    }
    if (cursor == 0) {
        return ok_error();
    }
    EditorSnapshot before = snapshot();
    const size_t previous = text.previous_char_offset(cursor);
    Error err = text.erase(previous, cursor - previous);
    if (!err.ok()) {
        return err;
    }
    remember_undo(std::move(before));
    cursor = previous;
    dirty = true;
    update_preferred_column(*this);
    return ok_error();
}

Error EditorState::erase_at_cursor() {
    if (cursor >= text.size()) {
        return ok_error();
    }
    EditorSnapshot before = snapshot();
    const size_t next = text.next_char_offset(cursor);
    Error err = text.erase(cursor, next - cursor);
    if (!err.ok()) {
        return err;
    }
    remember_undo(std::move(before));
    dirty = true;
    update_preferred_column(*this);
    return ok_error();
}

bool EditorState::undo() {
    if (undo_stack_.empty()) {
        return false;
    }
    while (redo_stack_.size() >= undo_limit_ && undo_limit_ > 0) {
        redo_stack_.erase(redo_stack_.begin());
    }
    redo_stack_.push_back(snapshot());
    const EditorSnapshot target = std::move(undo_stack_.back());
    undo_stack_.pop_back();
    restore_snapshot(target);
    dirty = true;
    return true;
}

bool EditorState::redo() {
    if (redo_stack_.empty()) {
        return false;
    }
    while (undo_stack_.size() >= undo_limit_ && undo_limit_ > 0) {
        undo_stack_.erase(undo_stack_.begin());
    }
    undo_stack_.push_back(snapshot());
    const EditorSnapshot target = std::move(redo_stack_.back());
    redo_stack_.pop_back();
    restore_snapshot(target);
    dirty = true;
    return true;
}

bool EditorState::can_undo() const {
    return !undo_stack_.empty();
}

bool EditorState::can_redo() const {
    return !redo_stack_.empty();
}

void EditorState::set_undo_limit(size_t limit) {
    undo_limit_ = limit;
    if (undo_limit_ == 0) {
        undo_stack_.clear();
        redo_stack_.clear();
        return;
    }
    while (undo_stack_.size() > undo_limit_) {
        undo_stack_.erase(undo_stack_.begin());
    }
    while (redo_stack_.size() > undo_limit_) {
        redo_stack_.erase(redo_stack_.begin());
    }
}

size_t EditorState::undo_limit() const {
    return undo_limit_;
}

void EditorState::clear_undo_history() {
    undo_stack_.clear();
    redo_stack_.clear();
}

bool EditorState::search(const std::string& needle) {
    if (needle.empty()) {
        return false;
    }
    const std::string haystack = text.str();
    if (haystack.empty()) {
        return false;
    }

    size_t found = haystack.find(needle, std::min(cursor, haystack.size()));
    if (found == std::string::npos) {
        found = haystack.find(needle);
    }
    if (found == std::string::npos) {
        return false;
    }
    cursor = found;
    selection.clear(cursor);
    update_preferred_column(*this);
    return true;
}

bool EditorState::search_next(const std::string& needle) {
    if (needle.empty()) {
        return false;
    }
    const std::string haystack = text.str();
    if (haystack.empty()) {
        return false;
    }

    const size_t start = cursor < haystack.size() ? cursor + 1 : 0;
    size_t found = haystack.find(needle, start);
    if (found == std::string::npos) {
        found = haystack.find(needle);
    }
    if (found == std::string::npos) {
        return false;
    }
    cursor = found;
    selection.clear(cursor);
    update_preferred_column(*this);
    return true;
}

bool EditorState::search_previous(const std::string& needle) {
    if (needle.empty()) {
        return false;
    }
    const std::string haystack = text.str();
    if (haystack.empty()) {
        return false;
    }

    const size_t start = cursor == 0 ? haystack.size() : cursor - 1;
    size_t found = haystack.rfind(needle, start);
    if (found == std::string::npos) {
        found = haystack.rfind(needle);
    }
    if (found == std::string::npos) {
        return false;
    }
    cursor = found;
    selection.clear(cursor);
    update_preferred_column(*this);
    return true;
}

Error EditorState::replace_all_from(size_t start,
                                    const std::string& needle,
                                    const std::string& value,
                                    size_t& replacements) {
    replacements = 0;
    if (needle.empty()) {
        return {ErrorCode::BadArgs, "editor replace search string is empty"};
    }
    const std::string before_text = text.str();
    if (start > before_text.size()) {
        return {ErrorCode::BadArgs, "editor replace start position is past the end of the buffer"};
    }

    std::string replaced;
    replaced.reserve(before_text.size());
    replaced.append(before_text, 0, start);

    size_t scan = start;
    size_t cursor_after_last_replacement = cursor;
    while (scan < before_text.size()) {
        const size_t found = before_text.find(needle, scan);
        if (found == std::string::npos) {
            break;
        }
        replaced.append(before_text, scan, found - scan);
        replaced.append(value);
        cursor_after_last_replacement = replaced.size();
        ++replacements;
        scan = found + needle.size();
    }
    replaced.append(before_text, scan, std::string::npos);

    if (replacements == 0 || replaced == before_text) {
        return ok_error();
    }

    EditorSnapshot before{before_text, cursor, preferred_column, scroll_line, scroll_column};
    text = PieceTable::from_string(std::move(replaced));
    remember_undo(std::move(before));
    cursor = std::min(cursor_after_last_replacement, text.size());
    dirty = true;
    update_preferred_column(*this);
    return ok_error();
}

void EditorState::clear_selection() {
    selection.clear(cursor);
}

std::string EditorState::selected_text() const {
    if (!selection.has_range()) {
        return "";
    }
    const size_t start = selection.start();
    const size_t count = selection.end() - start;
    return text.str().substr(start, count);
}

Error EditorState::copy_selection(Clipboard& clipboard) {
    if (!selection.has_range()) {
        return {ErrorCode::BadArgs, "no selection to copy"};
    }
    clipboard.set(selected_text());
    return ok_error();
}

Error EditorState::cut_selection(Clipboard& clipboard) {
    if (!selection.has_range()) {
        return {ErrorCode::BadArgs, "no selection to cut"};
    }
    clipboard.set(selected_text());
    const size_t start = selection.start();
    Error err = replace(start, selection.end() - start, "");
    if (err.ok()) {
        cursor = start;
        selection.clear(cursor);
    }
    return err;
}

Error EditorState::paste(Clipboard& clipboard) {
    if (clipboard.empty()) {
        return {ErrorCode::BadArgs, "clipboard is empty"};
    }
    if (selection.has_range()) {
        Error err = replace(selection.start(), selection.end() - selection.start(), clipboard.text());
        if (err.ok()) {
            selection.clear(cursor);
        }
        return err;
    }
    return insert(clipboard.text());
}

void EditorState::begin_movement(bool extend_selection) {
    if (!extend_selection) {
        selection.clear(cursor);
    } else if (!selection.has_range()) {
        selection.anchor = cursor;
        selection.active = cursor;
    }
}

void EditorState::finish_movement(bool extend_selection) {
    if (extend_selection) {
        selection.active = cursor;
    } else {
        selection.clear(cursor);
    }
}

void EditorState::apply_movement(MovementKey key, const Rect& rect, bool extend_selection) {
    begin_movement(extend_selection);
    switch (key) {
        case MovementKey::Left:
            move_left();
            break;
        case MovementKey::Right:
            move_right();
            break;
        case MovementKey::Up:
            move_up(rect);
            break;
        case MovementKey::Down:
            move_down(rect);
            break;
        case MovementKey::PageUp:
            page_up(rect);
            break;
        case MovementKey::PageDown:
            page_down(rect);
            break;
        case MovementKey::Home:
            move_home();
            break;
        case MovementKey::End:
            move_end();
            break;
    }
    finish_movement(extend_selection);
    if (key == MovementKey::PageUp || key == MovementKey::PageDown || key == MovementKey::Home ||
        key == MovementKey::End) {
        ensure_cursor_visible(rect);
    }
}

void EditorState::move_left() {
    cursor = text.previous_char_offset(cursor);
    update_preferred_column(*this);
}

void EditorState::move_right() {
    cursor = text.next_char_offset(cursor);
    update_preferred_column(*this);
}

void EditorState::move_up() {
    const size_t line = text.line_for_offset(cursor);
    if (line == 0) {
        return;
    }
    cursor = text.offset_for_line_column(line - 1, preferred_column);
}

void EditorState::move_down() {
    const size_t line = text.line_for_offset(cursor);
    if (line + 1 >= text.line_count()) {
        return;
    }
    cursor = text.offset_for_line_column(line + 1, preferred_column);
}

void EditorState::move_up(const Rect& rect) {
    if (vertical_movement == VerticalMovementMode::VisualRow) {
        move_up_visual(rect);
        return;
    }
    move_up();
}

void EditorState::move_down(const Rect& rect) {
    if (vertical_movement == VerticalMovementMode::VisualRow) {
        move_down_visual(rect);
        return;
    }
    move_down();
}

void EditorState::page_up(const Rect& rect) {
    const int count = std::max(1, rect.height);
    for (int i = 0; i < count; ++i) {
        const size_t previous = cursor;
        move_up(rect);
        if (cursor == previous) {
            break;
        }
    }
    ensure_cursor_visible(rect);
}

void EditorState::page_down(const Rect& rect) {
    const int count = std::max(1, rect.height);
    for (int i = 0; i < count; ++i) {
        const size_t previous = cursor;
        move_down(rect);
        if (cursor == previous) {
            break;
        }
    }
    ensure_cursor_visible(rect);
}

void EditorState::move_up_visual(const Rect& rect) {
    const size_t width = static_cast<size_t>(std::max(1, rect.width));
    const WrappedLocation location = wrapped_location_for_offset(text, cursor, width);
    if (location.segment > 0) {
        cursor = offset_for_wrapped_location(text, location.line, location.segment - 1, preferred_column, width);
        return;
    }
    if (location.line == 0) {
        return;
    }
    const size_t previous_line = location.line - 1;
    const size_t previous_rows = wrapped_row_count(text.line_text(previous_line), width);
    cursor = offset_for_wrapped_location(text, previous_line, previous_rows - 1, preferred_column, width);
}

void EditorState::move_down_visual(const Rect& rect) {
    const size_t width = static_cast<size_t>(std::max(1, rect.width));
    const WrappedLocation location = wrapped_location_for_offset(text, cursor, width);
    const size_t current_rows = wrapped_row_count(text.line_text(location.line), width);
    if (location.segment + 1 < current_rows) {
        cursor = offset_for_wrapped_location(text, location.line, location.segment + 1, preferred_column, width);
        return;
    }
    if (location.line + 1 >= text.line_count()) {
        return;
    }
    cursor = offset_for_wrapped_location(text, location.line + 1, 0, preferred_column, width);
}

void EditorState::move_home() {
    cursor = 0;
    preferred_column = 0;
    scroll_line = 0;
    scroll_column = 0;
    selection.clear(cursor);
}

void EditorState::move_end() {
    cursor = text.size();
    update_preferred_column(*this);
    selection.clear(cursor);
}

Error EditorState::kill_to_line_end() {
    const size_t line = text.line_for_offset(cursor);
    const size_t start = text.line_start(line);
    const size_t length = text.line_length(line);
    const size_t end = start + length;
    if (cursor < end) {
        EditorSnapshot before = snapshot();
        Error err = text.erase(cursor, end - cursor);
        if (!err.ok()) {
            return err;
        }
        remember_undo(std::move(before));
        dirty = true;
        update_preferred_column(*this);
        return ok_error();
    }
    if (length != 0 || text.line_count() <= 1) {
        return ok_error();
    }

    size_t erase_pos = start;
    if (line + 1 >= text.line_count()) {
        erase_pos = start == 0 ? 0 : start - 1;
    }
    EditorSnapshot before = snapshot();
    Error err = text.erase(erase_pos, 1);
    if (!err.ok()) {
        return err;
    }
    remember_undo(std::move(before));
    cursor = std::min(erase_pos, text.size());
    dirty = true;
    update_preferred_column(*this);
    return ok_error();
}

void EditorState::ensure_cursor_visible(const Rect& rect) {
    const size_t line = text.line_for_offset(cursor);
    const size_t height = static_cast<size_t>(std::max(1, rect.height));
    const size_t width = static_cast<size_t>(std::max(1, rect.width));

    size_t cursor_row = 0;
    for (size_t i = 0; i < line; ++i) {
        cursor_row += wrapped_row_count(text.line_text(i), width);
    }
    const size_t line_start_offset = text.line_start(line);
    const std::string line_text_value = text.line_text(line);
    cursor_row += cursor_in_wrapped_line(line_text_value, cursor - line_start_offset, width).row;

    if (cursor_row < scroll_line) {
        scroll_line = cursor_row;
    } else if (cursor_row >= scroll_line + height) {
        scroll_line = cursor_row - height + 1;
    }
    scroll_column = 0;
}

RenderedPanel EditorState::render(const Rect& rect) const {
    const std::optional<Selection> active_selection =
        selection.has_range() ? std::optional<Selection>(selection) : std::nullopt;
    return render_panel(text, rect, cursor, scroll_line, scroll_column, active_selection);
}

RenderedPanel render_panel(const PieceTable& text,
                           const Rect& rect,
                           size_t cursor,
                           size_t scroll_line,
                           size_t scroll_column,
                           const std::optional<Selection>& selection) {
    (void)scroll_column;
    RenderedPanel rendered;
    const size_t height = static_cast<size_t>(std::max(0, rect.height));
    const size_t width = static_cast<size_t>(std::max(0, rect.width));
    rendered.lines.reserve(height);
    if (width == 0) {
        rendered.lines.resize(height);
        return rendered;
    }

    const bool highlight_selection = selection.has_value() && selection->has_range();
    const size_t sel_start = highlight_selection ? selection->start() : 0;
    const size_t sel_end = highlight_selection ? selection->end() : 0;

    const size_t line_count = text.line_count();
    size_t line = 0;
    size_t segment_index = 0;
    size_t rows_to_skip = scroll_line;
    while (line < line_count) {
        const std::string line_text_value = text.line_text(line);
        const size_t rows = wrapped_row_count(line_text_value, width);
        if (rows_to_skip < rows) {
            segment_index = rows_to_skip;
            break;
        }
        rows_to_skip -= rows;
        ++line;
    }

    for (size_t row = 0; row < height; ++row) {
        if (line >= line_count) {
            rendered.lines.push_back(std::string(width, ' '));
            continue;
        }

        const std::string line_text_value = text.line_text(line);
        const std::vector<WrapSegment> segments = wrap_line_segments(line_text_value, width);
        const WrapSegment segment = segments[std::min(segment_index, segments.size() - 1)];
        const size_t global_line_start = text.line_start(line);
        if (highlight_selection) {
            rendered.lines.push_back(display_range_highlighted(line_text_value,
                                                              segment.start,
                                                              segment.end,
                                                              width,
                                                              global_line_start,
                                                              sel_start,
                                                              sel_end,
                                                              true));
        } else {
            rendered.lines.push_back(display_range(line_text_value, segment.start, segment.end, width));
        }

        ++segment_index;
        if (segment_index >= segments.size()) {
            segment_index = 0;
            ++line;
        }
    }

    const size_t cursor_line = text.line_for_offset(cursor);
    size_t cursor_row = 0;
    for (size_t i = 0; i < cursor_line; ++i) {
        cursor_row += wrapped_row_count(text.line_text(i), width);
    }
    const size_t cursor_line_start = text.line_start(cursor_line);
    const std::string cursor_line_text = text.line_text(cursor_line);
    const WrappedCursor wrapped_cursor = cursor_in_wrapped_line(cursor_line_text, cursor - cursor_line_start, width);
    cursor_row += wrapped_cursor.row;
    if (cursor_row >= scroll_line && cursor_row < scroll_line + height) {
        rendered.cursor.row = static_cast<int>(cursor_row - scroll_line);
        rendered.cursor.col = static_cast<int>(wrapped_cursor.col);
        rendered.cursor.visible = true;
    }
    return rendered;
}

Error load_file(const std::string& path, PieceTable& out) {
    return load_file(path, EditorSettings{}, out);
}

Error check_load_file_size(const std::string& path, const EditorSettings& settings, FileLoadCheck& check) {
    check = {};
    std::error_code filesystem_error;
    const std::filesystem::file_status status = std::filesystem::status(path, filesystem_error);
    if (filesystem_error || !std::filesystem::exists(status)) {
        return {ErrorCode::FileRead, "could not inspect editor file before loading: " + path};
    }
    if (!std::filesystem::is_regular_file(status)) {
        return {ErrorCode::FileRead, "editor path is not a regular file: " + path};
    }
    const std::uintmax_t file_size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error) {
        return {ErrorCode::FileRead, "could not determine editor file size before loading: " + path};
    }
    if (settings.file_size_limit >= 0 &&
        file_size > static_cast<std::uintmax_t>(settings.file_size_limit)) {
        return {ErrorCode::FileRead,
                "editor file exceeds FILE_SIZE_LIMIT of " +
                    std::to_string(settings.file_size_limit) + " bytes: " + path +
                    " (" + std::to_string(file_size) + " bytes)"};
    }
    check.size = file_size;
    check.should_warn = settings.huge_file_size_warning > 0 &&
                        file_size >= static_cast<std::uintmax_t>(settings.huge_file_size_warning);
    return ok_error();
}

Error load_file(const std::string& path, const EditorSettings& settings, PieceTable& out) {
    FileLoadCheck check;
    Error err = check_load_file_size(path, settings, check);
    if (!err.ok()) {
        return err;
    }
    if (check.size > static_cast<std::uintmax_t>(std::numeric_limits<size_t>::max())) {
        return {ErrorCode::FileRead,
                "editor file is too large for this platform address space: " + path +
                    " (" + std::to_string(check.size) + " bytes)"};
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {ErrorCode::FileRead, "could not open editor file for reading: " + path};
    }

    std::string content;
    try {
        content.reserve(static_cast<size_t>(check.size));
        std::array<char, 65536> buffer{};
        while (in) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = in.gcount();
            if (count <= 0) {
                break;
            }
            content.append(buffer.data(), static_cast<size_t>(count));
        }
    } catch (const std::bad_alloc&) {
        return {ErrorCode::Internal, "not enough memory to load editor file: " + path};
    } catch (const std::length_error&) {
        return {ErrorCode::FileRead, "editor file is too large to load into memory: " + path};
    }
    if (!in.good() && !in.eof()) {
        return {ErrorCode::FileRead, "failed while reading editor file: " + path};
    }
    out = PieceTable::from_string(std::move(content));
    return ok_error();
}

Error save_file(const std::string& path, const PieceTable& text) {
    if (path.empty()) {
        return {ErrorCode::BadArgs, "no editor save path was provided"};
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return {ErrorCode::FileWrite, "could not open editor file for writing: " + path};
    }
    Error err = text.write_to(out);
    if (!err.ok()) {
        return {err.code, err.message + ": " + path};
    }
    out.close();
    if (!out) {
        return {ErrorCode::FileWrite, "failed while closing editor file after writing: " + path};
    }
    return ok_error();
}

Error ensure_empty_file(const std::string& path) {
    if (path.empty()) {
        return ok_error();
    }
    if (access(path.c_str(), F_OK) == 0) {
        return ok_error();
    }
    return save_file(path, PieceTable::from_string(""));
}

struct AssistSession {
    runtime::JobHandle job;
    runtime::EventQueue<ContinueEvent> events;
    bool active = false;
    bool streaming = false;
    bool saw_visible = false;
    AssistEditKind edit_kind = AssistEditKind::StreamInsert;
    EditorSnapshot undo_before;
    std::string model_name;
    size_t replace_start = 0;
    size_t replace_count = 0;
};

void clear_assist_session(AssistSession& session) {
    session.job.join();
    ContinueEvent event;
    while (session.events.try_pop(event)) {
    }
    session.active = false;
    session.streaming = false;
    session.saw_visible = false;
    session.edit_kind = AssistEditKind::StreamInsert;
    session.undo_before = EditorSnapshot{};
    session.model_name.clear();
    session.replace_start = 0;
    session.replace_count = 0;
}

int run_editor(const std::string& path,
               const std::string& save_as,
               const EditorSettings& settings,
               const AiContinueContext* ai_continue) {
    EditorState state;
    state.set_undo_limit(settings.undo_limit);
    state.path = path.empty() ? save_as : path;
    std::string status = "Ready";
    if (!path.empty() && access(path.c_str(), F_OK) == 0) {
        FileLoadCheck check;
        Error err = check_load_file_size(path, settings, check);
        if (!err.ok()) {
            std::cerr << error_code_name(err.code) << ": " << err.message << "\n";
            return 5;
        }
        if (check.should_warn && !confirm_huge_load_before_terminal(path, check)) {
            std::cerr << "Editor load cancelled: " << path << "\n";
            return 5;
        }
        err = load_file(path, settings, state.text);
        if (!err.ok()) {
            std::cerr << error_code_name(err.code) << ": " << err.message << "\n";
            return 5;
        }
        status = "Loaded";
    } else if (!path.empty()) {
        Error create_err = ensure_empty_file(path);
        if (!create_err.ok()) {
            std::cerr << error_code_name(create_err.code) << ": " << create_err.message << "\n";
            return 5;
        }
        status = "New file";
    }

    TerminalSession terminal;
    Error err = terminal.enter();
    if (!err.ok()) {
        std::cerr << error_code_name(err.code) << ": " << err.message << "\n";
        return err.code == ErrorCode::BadArgs ? 2 : 6;
    }

    bool quit = false;
    bool pending_quit_after_save = false;
    PendingSaveRequest pending_save;
    MinibufferState minibuffer;
    minibuffer.message = status;
    std::string last_search;
    ReplaceSession replace;
    std::string pending_load_path;
    AssistSession assist_session;
    AssistCompleterState assist_completer;
    PendingAssist pending_assist;
    TerminalSize last_size = terminal_size();
    render_terminal(state, minibuffer);

    auto assist_panel_rect = [&]() {
        const TerminalSize size = terminal_size();
        return Rect{1, 1, std::max(1, size.rows - 2), std::max(1, size.cols - 1)};
    };

    auto set_assist_minibuffer = [&](const std::string& suffix) {
        minibuffer_message(minibuffer, continue_status_message(assist_session.model_name, suffix));
    };

    auto finish_assist_session = [&](const std::string& suffix,
                                     bool commit_stream_undo,
                                     const std::optional<std::string>& inplace_content) {
        assist_session.job.join();
        if (inplace_content.has_value()) {
            Error replace_error =
                state.replace(assist_session.replace_start, assist_session.replace_count, *inplace_content);
            if (!replace_error.ok()) {
                minibuffer_message(minibuffer, replace_error.message);
            } else {
                state.clear_selection();
                set_assist_minibuffer(suffix);
            }
        } else {
            if (commit_stream_undo && assist_session.saw_visible) {
                state.finalize_stream_edit(assist_session.undo_before);
            }
            set_assist_minibuffer(suffix);
        }
        clear_assist_session(assist_session);
    };

    auto process_assist_events = [&]() -> bool {
        bool updated = false;
        ContinueEvent event;
        while (assist_session.events.try_pop(event)) {
            updated = true;
            switch (event.type) {
                case ContinueEventType::Thinking:
                    set_assist_minibuffer("thinking... ESC to abort");
                    break;
                case ContinueEventType::Writing:
                    set_assist_minibuffer("writing. Press ESC to stop.");
                    break;
                case ContinueEventType::Delta:
                    assist_session.saw_visible = true;
                    if (Error insert_error = state.insert_without_undo(event.text); !insert_error.ok()) {
                        assist_session.job.cancel();
                        assist_session.job.join();
                        minibuffer_message(minibuffer, insert_error.message);
                        clear_assist_session(assist_session);
                        return true;
                    }
                    state.ensure_cursor_visible(assist_panel_rect());
                    break;
                case ContinueEventType::Done:
                    if (!event.chat.model.empty()) {
                        assist_session.model_name = event.chat.model;
                    }
                    if (assist_session.edit_kind == AssistEditKind::ReplaceInPlace) {
                        finish_assist_session("ready",
                                              false,
                                              trim_assist_inplace_response(event.chat.content));
                    } else {
                        finish_assist_session("stopped and ready", true, std::nullopt);
                    }
                    return true;
                case ContinueEventType::Error:
                    if (event.error.code == ErrorCode::Cancelled) {
                        if (assist_session.edit_kind == AssistEditKind::ReplaceInPlace) {
                            assist_session.job.join();
                            set_assist_minibuffer("stopped and ready");
                            clear_assist_session(assist_session);
                        } else {
                            finish_assist_session("stopped and ready", true, std::nullopt);
                        }
                    } else {
                        assist_session.job.join();
                        minibuffer_message(minibuffer, event.error.message);
                        clear_assist_session(assist_session);
                    }
                    return true;
            }
        }
        return updated;
    };

    auto start_assist = [&](AssistCommandKind kind,
                            size_t command_index,
                            std::optional<AssistScope> scope,
                            const std::string& custom_prompt,
                            std::optional<AssistPromptMode> prompt_mode) {
        if (assist_session.active) {
            return;
        }
        if (minibuffer.active && !is_assist_minibuffer_action(minibuffer.action)) {
            return;
        }
        if (ai_continue == nullptr) {
            minibuffer_message(minibuffer,
                               "AI assist requires a provider; use --provider lmstudio -m MODEL");
            return;
        }
        Error validation = validate_continue_request(*ai_continue);
        if (!validation.ok()) {
            minibuffer_message(minibuffer, validation.message);
            return;
        }

        AssistExecution execution = build_assist_execution(
            state, *ai_continue, kind, command_index, scope, custom_prompt, prompt_mode);
        if (!execution.ok) {
            minibuffer_message(minibuffer, execution.error_message);
            return;
        }

        pending_assist = PendingAssist{};
        exit_assist_command_mode(minibuffer, assist_completer);
        clear_assist_session(assist_session);
        assist_session.active = true;
        assist_session.streaming = execution.stream;
        assist_session.edit_kind = execution.edit_kind;
        assist_session.model_name = ai_continue->request.options.model;
        assist_session.replace_start = execution.replace_start;
        assist_session.replace_count = execution.replace_count;
        if (execution.edit_kind == AssistEditKind::StreamInsert) {
            assist_session.undo_before = state.capture_state();
        }
        state.clear_selection();
        start_assist_job(*ai_continue, execution.messages, execution.stream, assist_session.events, assist_session.job);
        set_assist_minibuffer("thinking... ESC to abort");
    };

    auto submit_assist_command = [&]() {
        const ParsedAssistCommand parsed =
            parse_assist_command(minibuffer.input, ai_continue == nullptr ? default_editor_assist_config()
                                                                          : ai_continue->assist_config);
        if (!parsed.ok) {
            minibuffer_message(minibuffer, parsed.error_message);
            return;
        }
        if (parsed.kind == AssistCommandKind::Configured) {
            const EditorAssistConfig& assist_config =
                ai_continue == nullptr ? default_editor_assist_config() : ai_continue->assist_config;
            if (parsed.command_index >= assist_config.commands.size()) {
                minibuffer_message(minibuffer, "Configured assist command index is out of range");
                return;
            }
            const EditorAssistCommand& command = assist_config.commands[parsed.command_index];
            if (assist_command_requires_scope(command) && !parsed.scope.has_value()) {
                pending_assist.kind = AssistCommandKind::Configured;
                pending_assist.command_index = parsed.command_index;
                start_minibuffer(minibuffer,
                                 MinibufferAction::AssistScopeChoice,
                                 assist_scope_prompt(command));
                return;
            }
            start_assist(AssistCommandKind::Configured, parsed.command_index, parsed.scope, "", std::nullopt);
            return;
        }
        if (parsed.kind == AssistCommandKind::Prompt) {
            pending_assist.kind = parsed.kind;
            pending_assist.custom_prompt = parsed.custom_prompt;
            start_minibuffer(minibuffer, MinibufferAction::AssistPromptMode, assist_prompt_mode_message());
            return;
        }
        if (parsed.kind == AssistCommandKind::Quit) {
            pending_assist = PendingAssist{};
            exit_assist_command_mode(minibuffer, assist_completer);
            quit = true;
            return;
        }
        start_assist(parsed.kind, parsed.command_index, parsed.scope, parsed.custom_prompt, std::nullopt);
    };

    auto start_continue = [&]() {
        if (ai_continue == nullptr) {
            return;
        }
        const std::optional<size_t> command_index =
            assist_command_index(ai_continue->assist_config, "/continue");
        if (!command_index.has_value()) {
            minibuffer_message(minibuffer, "Configured editor assist commands are missing /continue");
            return;
        }
        start_assist(AssistCommandKind::Configured,
                     *command_index,
                     AssistScope::Continue,
                     "",
                     std::nullopt);
    };

    auto handle_key = [&](unsigned char ch) {
        if (assist_session.active) {
            if (ch == 27) {
                assist_session.job.cancel();
                return;
            }
            return;
        }
        if (minibuffer.active && is_assist_minibuffer_action(minibuffer.action)) {
            if (ch == 27 || ch == 7) {
                pending_assist = PendingAssist{};
                exit_assist_command_mode(minibuffer, assist_completer);
                return;
            }
            if (minibuffer.action == MinibufferAction::AssistCommand) {
                if (ch == '\t') {
                    const AssistCompletionResult result = complete_assist_command(
                        minibuffer.input,
                        assist_completer,
                        ai_continue == nullptr ? default_editor_assist_config() : ai_continue->assist_config);
                    minibuffer.message = assist_completion_status(result);
                    return;
                }
                if (ch == '\r' || ch == '\n') {
                    submit_assist_command();
                    return;
                }
                if (ch == 127 || ch == 8) {
                    if (!minibuffer.input.empty()) {
                        minibuffer.input.pop_back();
                        assist_completer = AssistCompleterState{};
                    }
                    return;
                }
                if (ch >= 0x20U) {
                    minibuffer.input.push_back(static_cast<char>(ch));
                    assist_completer = AssistCompleterState{};
                    return;
                }
                return;
            }
            if (minibuffer.action == MinibufferAction::AssistScopeChoice) {
                if (ch == 's' || ch == 'S') {
                    start_assist(pending_assist.kind,
                                 pending_assist.command_index,
                                 AssistScope::Selection,
                                 "",
                                 std::nullopt);
                } else if (ch == 'a' || ch == 'A') {
                    start_assist(pending_assist.kind,
                                 pending_assist.command_index,
                                 AssistScope::All,
                                 "",
                                 std::nullopt);
                } else if (ch == 'c' || ch == 'C') {
                    start_assist(pending_assist.kind,
                                 pending_assist.command_index,
                                 AssistScope::Continue,
                                 "",
                                 std::nullopt);
                } else if (ch == 'i' || ch == 'I' || ch == 'l' || ch == 'L') {
                    start_assist(pending_assist.kind,
                                 pending_assist.command_index,
                                 AssistScope::Insert,
                                 "",
                                 std::nullopt);
                }
                return;
            }
            if (minibuffer.action == MinibufferAction::AssistPromptMode) {
                if (ch == 'c' || ch == 'C') {
                    start_assist(pending_assist.kind,
                                 pending_assist.command_index,
                                 std::nullopt,
                                 pending_assist.custom_prompt,
                                 AssistPromptMode::Continue);
                } else if (ch == 's' || ch == 'S') {
                    start_assist(pending_assist.kind,
                                 pending_assist.command_index,
                                 std::nullopt,
                                 pending_assist.custom_prompt,
                                 AssistPromptMode::Selection);
                } else if (ch == 'a' || ch == 'A') {
                    start_assist(pending_assist.kind,
                                 pending_assist.command_index,
                                 std::nullopt,
                                 pending_assist.custom_prompt,
                                 AssistPromptMode::All);
                } else if (ch == 'i' || ch == 'I' || ch == 'l' || ch == 'L') {
                    start_assist(pending_assist.kind,
                                 pending_assist.command_index,
                                 std::nullopt,
                                 pending_assist.custom_prompt,
                                 AssistPromptMode::Insert);
                }
                return;
            }
            return;
        }
        if (handle_minibuffer_key(state,
                                  minibuffer,
                                  ch,
                                  quit,
                                  last_search,
                                  replace,
                                  settings,
                                  pending_load_path,
                                  pending_quit_after_save,
                                  pending_save)) {
            return;
        }
        if (handle_replace_key(state, minibuffer, replace, ch)) {
            return;
        }

        if (ch == 17) {
            if (state.path.empty()) {
                if (state.dirty || !state.text.empty()) {
                    start_minibuffer(minibuffer,
                                     MinibufferAction::ConfirmSaveOnQuit,
                                     "Modified buffer exists; save before quit? (y/n) ");
                } else {
                    quit = true;
                }
            } else if (state.dirty) {
                start_minibuffer(minibuffer, MinibufferAction::ConfirmQuit,
                                 "Buffer modified; quit anyway? (y/n) ");
            } else {
                quit = true;
            }
        } else if (ch == 3) {
            Error copy_error = state.copy_selection(shared_clipboard());
            minibuffer_message(minibuffer, copy_error.ok() ? "Copied selection" : copy_error.message);
        } else if (ch == 24) {
            Error cut_error = state.cut_selection(shared_clipboard());
            minibuffer_message(minibuffer, cut_error.ok() ? "Cut selection" : cut_error.message);
        } else if (ch == 22) {
            Error paste_error = paste_with_clipboard_preference(state, shared_clipboard(), "");
            minibuffer_message(minibuffer, paste_error.ok() ? "Pasted" : paste_error.message);
        } else if (ch == 6) {
            start_minibuffer(minibuffer, MinibufferAction::Search, "Search: ", last_search);
        } else if (ch == 8) {
            start_minibuffer(minibuffer, MinibufferAction::ReplaceSearch, "Replace search: ", last_search);
        } else if (ch == 21) {
            minibuffer_message(minibuffer, state.undo() ? "Undone" : "Nothing to undo");
        } else if (ch == 18) {
            minibuffer_message(minibuffer, state.redo() ? "Redone" : "Nothing to redo");
        } else if (ch == 1) {
            state.move_home();
        } else if (ch == 5) {
            state.move_end();
        } else if (ch == 11) {
            Error kill_error = state.kill_to_line_end();
            if (!kill_error.ok()) {
                minibuffer_message(minibuffer, kill_error.message);
            }
        } else if (ch == 15) {
            start_minibuffer(minibuffer, MinibufferAction::LoadFile, "Load file: ");
        } else if (ch == 0) {
            start_continue();
        } else if (ch == 19) {
            const std::string target = save_as.empty() ? state.path : save_as;
            if (target.empty()) {
                start_minibuffer(minibuffer, MinibufferAction::SaveFile, "Save file: ");
            } else {
                const bool update_path = save_as.empty() || state.path.empty();
                request_save_editor_to_path(state,
                                            target,
                                            minibuffer,
                                            update_path,
                                            false,
                                            quit,
                                            pending_save);
            }
        } else if (ch == '\t') {
            minibuffer_message(minibuffer, "Tab completion is disabled in editor mode");
        } else if (ch == 27) {
            if (!minibuffer.active && !replace.active && !assist_session.active) {
                const std::string sequence = read_escape_suffix();
                if (!sequence.empty()) {
                    std::string escape_status;
                    dispatch_escape_sequence(state, sequence, escape_status, last_search);
                    if (!escape_status.empty()) {
                        minibuffer_message(minibuffer, escape_status);
                    }
                    return;
                }
                start_assist_command_mode(minibuffer, assist_completer);
                return;
            }
            std::string escape_status;
            handle_escape(state, escape_status, last_search);
            if (!escape_status.empty()) {
                minibuffer_message(minibuffer, escape_status);
            }
        } else if (ch == 127) {
            Error erase_error = state.erase_before_cursor();
            if (!erase_error.ok()) {
                minibuffer_message(minibuffer, erase_error.message);
            }
        } else if (ch == '\r' || ch == '\n') {
            Error insert_error = state.insert("\n");
            if (!insert_error.ok()) {
                minibuffer_message(minibuffer, insert_error.message);
            }
        } else if (ch >= 0x20U) {
            const std::string text(1, static_cast<char>(ch));
            Error insert_error = state.insert(text);
            if (!insert_error.ok()) {
                minibuffer_message(minibuffer, insert_error.message);
            }
        }
    };

    auto handle_paste = [&](const std::string& terminal_text) {
        Error paste_error = paste_with_clipboard_preference(state, shared_clipboard(), terminal_text);
        minibuffer_message(minibuffer, paste_error.ok() ? "Pasted" : paste_error.message);
    };

    while (!quit) {
        const bool assist_updated = process_assist_events();

        TerminalInputEvent event;
        if (!read_terminal_input(event, 100)) {
            const TerminalSize current_size = terminal_size();
            if (current_size.rows != last_size.rows || current_size.cols != last_size.cols ||
                assist_session.job.running() || assist_updated) {
                last_size = current_size;
                render_terminal(state, minibuffer);
            }
            continue;
        }

        if (event.type == TerminalInputType::BracketedPaste) {
            handle_paste(event.text);
        } else if (event.type == TerminalInputType::Byte) {
            handle_key(event.byte);
            while (!quit) {
                if (!read_terminal_input(event, 0)) {
                    break;
                }
                if (event.type == TerminalInputType::BracketedPaste) {
                    handle_paste(event.text);
                } else if (event.type == TerminalInputType::Byte) {
                    handle_key(event.byte);
                } else {
                    break;
                }
            }
        }

        last_size = terminal_size();
        render_terminal(state, minibuffer);
    }
    return 0;
}

}  // namespace pkchat::editor
