#pragma once

#include <cstddef>
#include <chrono>
#include <string>

#include "common.hpp"
#include "editor/clipboard.hpp"

namespace ainiux::editor {

struct EditorState;

enum class TerminalInputType { Timeout, Byte, BracketedPaste, Osc52ClipboardResponse, Mouse };

enum class MouseButton {
    Unknown,
    Left,
    Middle,
    Right,
    Release,
    WheelUp,
    WheelDown,
    WheelLeft,
    WheelRight,
};

struct MouseInputEvent {
    MouseButton button = MouseButton::Unknown;
    bool pressed = false;
    bool shift = false;
    bool alt = false;
    bool ctrl = false;
    bool motion = false;
    int row = 0;
    int col = 0;
};

struct TerminalInputEvent {
    TerminalInputType type = TerminalInputType::Timeout;
    unsigned char byte = 0;
    bool decoded_escape = false;
    std::string text;
    std::string message;
    MouseInputEvent mouse;
};

// Terminals do not report key-up for a plain Escape key. This guard prevents
// delayed key-repeat bytes from immediately cancelling UI state opened by the
// first Escape, while becoming inactive after a quiet interval or any other key.
class EscapeRepeatGuard {
   public:
    using Clock = std::chrono::steady_clock;

    void arm(Clock::time_point now);
    void reset();
    bool suppress(Clock::time_point now);

   private:
    Clock::time_point deadline_{};
    bool armed_ = false;
};

constexpr const char* bracketed_paste_enable_sequence() { return "\x1b[?2004h"; }
constexpr const char* bracketed_paste_disable_sequence() { return "\x1b[?2004l"; }
constexpr const char* keyboard_modifier_enable_sequence() { return "\x1b[>4;2m\x1b[>1u"; }
constexpr const char* keyboard_modifier_disable_sequence() { return "\x1b[<1u\x1b[>4;0m"; }
constexpr const char* mouse_reporting_enable_sequence() { return "\x1b[?1002h\x1b[?1006h"; }
constexpr const char* mouse_reporting_disable_sequence() { return "\x1b[?1006l\x1b[?1002l"; }
// DECAWM: disable autowrap so full-width row paints (status, agent borders, last
// history cell) do not spill onto the next line on Windows Terminal / ConPTY.
constexpr const char* autowrap_disable_sequence() { return "\x1b[?7l"; }
constexpr const char* autowrap_enable_sequence() { return "\x1b[?7h"; }
constexpr int terminal_escape_inter_byte_timeout_ms() { return 100; }

bool read_terminal_byte(unsigned char& out, int timeout_ms);
bool read_terminal_input(TerminalInputEvent& out, int timeout_ms);
void clear_terminal_input_queue();
void push_terminal_input_bytes(const std::string& bytes);
bool consume_pending_escape_alt_meta();

// Internal sentinel returned by decode_control_key_sequence for Ctrl+Shift+S.
constexpr unsigned char editor_key_save_as() { return static_cast<unsigned char>(0xF1); }
constexpr unsigned char editor_key_backtab() { return static_cast<unsigned char>(0xF2); }
constexpr unsigned char editor_key_toggle_thinking_traces() {
    return static_cast<unsigned char>(0xF3);
}
constexpr unsigned char editor_key_command_minibuffer() {
    return static_cast<unsigned char>(0xF4);
}
constexpr bool is_editor_command_minibuffer_key(unsigned char ch) {
    return ch == 5 || ch == editor_key_command_minibuffer();
}

// Decode kitty/xterm control-key escape sequences such as "[19;5u" (Ctrl+S).
bool decode_control_key_sequence(const std::string& sequence, unsigned char& out);
bool decode_sgr_mouse_sequence(const std::string& sequence, MouseInputEvent& out);
bool decode_x10_mouse_bytes(unsigned char code,
                            unsigned char col,
                            unsigned char row,
                            MouseInputEvent& out);
bool decode_osc52_clipboard_payload(const std::string& encoded,
                                    std::string& text,
                                    std::string& error);

Error paste_with_clipboard_preference(EditorState& state,
                                      Clipboard& clipboard,
                                      const std::string& terminal_paste_text);

bool is_editor_undo_key(unsigned char ch);
bool is_editor_redo_key(unsigned char ch);

void publish_terminal_clipboard(const std::string& text);
void request_terminal_clipboard();
void cancel_terminal_clipboard_request();

}  // namespace ainiux::editor
