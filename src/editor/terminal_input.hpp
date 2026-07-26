#pragma once

#include <cstddef>
#include <string>

#include "common.hpp"
#include "editor/clipboard.hpp"

namespace ainiux::editor {

struct EditorState;

enum class TerminalInputType { Timeout, Byte, BracketedPaste, Osc52ClipboardResponse };

struct TerminalInputEvent {
    TerminalInputType type = TerminalInputType::Timeout;
    unsigned char byte = 0;
    std::string text;
    std::string message;
};

constexpr const char* bracketed_paste_enable_sequence() { return "\x1b[?2004h"; }
constexpr const char* bracketed_paste_disable_sequence() { return "\x1b[?2004l"; }
constexpr const char* keyboard_modifier_enable_sequence() { return "\x1b[>4;2m\x1b[>1u"; }
constexpr const char* keyboard_modifier_disable_sequence() { return "\x1b[<1u\x1b[>4;0m"; }
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

// Decode kitty/xterm control-key escape sequences such as "[19;5u" (Ctrl+S).
bool decode_control_key_sequence(const std::string& sequence, unsigned char& out);
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
