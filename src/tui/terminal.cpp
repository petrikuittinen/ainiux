#include "tui/terminal.hpp"

#include "editor/terminal_input.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <sys/ioctl.h>
#include <unistd.h>

namespace ainiux::tui {

TerminalSession::~TerminalSession() {
    restore();
}

Error TerminalSession::enter() {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        return {ErrorCode::BadArgs, "--chat requires an interactive terminal"};
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
    editor::clear_terminal_input_queue();
    std::cout << "\x1b[?1049h\x1b[?25h\x1b[2J\x1b[H" << editor::autowrap_disable_sequence()
              << editor::bracketed_paste_enable_sequence()
              << editor::keyboard_modifier_enable_sequence()
              << editor::mouse_reporting_enable_sequence();
    std::cout.flush();
    return ok_error();
}

void TerminalSession::restore() {
    if (!active_) {
        return;
    }
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_);
    std::cout << editor::mouse_reporting_disable_sequence()
              << editor::keyboard_modifier_disable_sequence()
              << editor::bracketed_paste_disable_sequence()
              << editor::autowrap_enable_sequence()
              << "\x1b[0m\x1b[?25h\x1b[2J\x1b[H\x1b[?1049l";
    std::cout.flush();
    editor::clear_terminal_input_queue();
    active_ = false;
}

}  // namespace ainiux::tui
