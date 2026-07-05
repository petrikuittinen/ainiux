#pragma once

#include <cstddef>
#include <string>

#include "common.hpp"
#include "editor/clipboard.hpp"

namespace pkchat::editor {

struct EditorState;

enum class TerminalInputType { Timeout, Byte, BracketedPaste };

struct TerminalInputEvent {
    TerminalInputType type = TerminalInputType::Timeout;
    unsigned char byte = 0;
    std::string text;
};

constexpr const char* bracketed_paste_enable_sequence() { return "\x1b[?2004h"; }
constexpr const char* bracketed_paste_disable_sequence() { return "\x1b[?2004l"; }

bool read_terminal_byte(unsigned char& out, int timeout_ms);
bool read_terminal_input(TerminalInputEvent& out, int timeout_ms);
void clear_terminal_input_queue();

// Internal sentinel returned by decode_control_key_sequence for Ctrl+Shift+S.
constexpr unsigned char editor_key_save_as() { return static_cast<unsigned char>(0xF1); }

// Decode kitty/xterm control-key escape sequences such as "[19;5u" (Ctrl+S).
bool decode_control_key_sequence(const std::string& sequence, unsigned char& out);

Error paste_with_clipboard_preference(EditorState& state,
                                      Clipboard& clipboard,
                                      const std::string& terminal_paste_text);

void publish_terminal_clipboard(const std::string& text);

}  // namespace pkchat::editor