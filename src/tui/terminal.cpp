#include "tui/terminal.hpp"

#include "editor/terminal_input.hpp"

#include <cerrno>
#include <cstring>
#include <iostream>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/select.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace ainiux::tui {

TerminalSession::~TerminalSession() {
    restore();
}

Error TerminalSession::enter() {
#if defined(_WIN32)
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD input_mode = 0;
    DWORD output_mode = 0;
    if (input == nullptr || input == INVALID_HANDLE_VALUE || output == nullptr ||
        output == INVALID_HANDLE_VALUE || !GetConsoleMode(input, &input_mode) ||
        !GetConsoleMode(output, &output_mode)) {
        return {ErrorCode::BadArgs,
                "full-screen modes require Windows Terminal or modern conhost; "
                "mintty/MSYS pseudo-terminals are not supported"};
    }
    // Consume only VT byte sequences through ReadFile. Console mouse/window
    // INPUT_RECORDs can signal the handle without supplying a byte and make a
    // byte-oriented reader block after WaitForSingleObject. Dimensions are
    // polled through GetConsoleScreenBufferInfo, while SGR mouse input arrives
    // through ENABLE_VIRTUAL_TERMINAL_INPUT. Quick Edit must be off so wheel and
    // drag reports reach the application (Shift remains the terminal override).
    const DWORD raw_input =
        (input_mode | ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_EXTENDED_FLAGS) &
        ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT |
          ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT | ENABLE_QUICK_EDIT_MODE);
    const DWORD vt_output = output_mode | ENABLE_PROCESSED_OUTPUT |
                            ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                            DISABLE_NEWLINE_AUTO_RETURN;
    if (!SetConsoleMode(output, vt_output) || !SetConsoleMode(input, raw_input)) {
        (void)SetConsoleMode(output, output_mode);
        (void)SetConsoleMode(input, input_mode);
        return {ErrorCode::UnsupportedFeature,
                "this Windows console does not support virtual-terminal input/output; "
                "use Windows 10 1903+ Windows Terminal or modern conhost"};
    }
    input_handle_ = input;
    output_handle_ = output;
    original_input_mode_ = input_mode;
    original_output_mode_ = output_mode;
    original_input_code_page_ = GetConsoleCP();
    original_output_code_page_ = GetConsoleOutputCP();
    (void)SetConsoleCP(CP_UTF8);
    (void)SetConsoleOutputCP(CP_UTF8);
#else
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        return {ErrorCode::BadArgs, "full-screen mode requires an interactive terminal"};
    }
    if (tcgetattr(STDIN_FILENO, &original_) != 0) {
        return {ErrorCode::Internal, std::string("could not read terminal mode: ") + std::strerror(errno)};
    }
    termios raw = original_;
    raw.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON | IEXTEN | ISIG));
    raw.c_iflag &= static_cast<tcflag_t>(~(IXON | IXOFF | ICRNL | BRKINT));
    raw.c_oflag &= static_cast<tcflag_t>(~OPOST);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
        return {ErrorCode::Internal, std::string("could not set terminal mode: ") + std::strerror(errno)};
    }
#endif
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
    std::cout << editor::mouse_reporting_disable_sequence()
              << editor::keyboard_modifier_disable_sequence()
              << editor::bracketed_paste_disable_sequence()
              << editor::autowrap_enable_sequence()
              << "\x1b[0m\x1b[?25h\x1b[2J\x1b[H\x1b[?1049l";
    std::cout.flush();
    editor::clear_terminal_input_queue();
#if defined(_WIN32)
    if (input_handle_ != nullptr)
        (void)SetConsoleMode(static_cast<HANDLE>(input_handle_), original_input_mode_);
    if (output_handle_ != nullptr)
        (void)SetConsoleMode(static_cast<HANDLE>(output_handle_), original_output_mode_);
    if (original_input_code_page_ != 0) (void)SetConsoleCP(original_input_code_page_);
    if (original_output_code_page_ != 0) (void)SetConsoleOutputCP(original_output_code_page_);
    input_handle_ = nullptr;
    output_handle_ = nullptr;
#else
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_);
#endif
    active_ = false;
}

bool terminal_input_is_interactive() {
#if defined(_WIN32)
    DWORD mode = 0;
    const HANDLE handle = GetStdHandle(STD_INPUT_HANDLE);
    return handle != nullptr && handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode);
#else
    return ::isatty(STDIN_FILENO) != 0;
#endif
}

bool terminal_output_is_interactive() {
#if defined(_WIN32)
    DWORD mode = 0;
    const HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    return handle != nullptr && handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode);
#else
    return ::isatty(STDOUT_FILENO) != 0;
#endif
}

TerminalDimensions terminal_dimensions() {
    TerminalDimensions dimensions;
#if defined(_WIN32)
    CONSOLE_SCREEN_BUFFER_INFO info{};
    const HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE &&
        GetConsoleScreenBufferInfo(handle, &info)) {
        const int rows = info.srWindow.Bottom - info.srWindow.Top + 1;
        const int cols = info.srWindow.Right - info.srWindow.Left + 1;
        if (rows > 0) dimensions.rows = rows;
        if (cols > 0) dimensions.cols = cols;
    }
#else
    winsize size{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_row > 0 &&
        size.ws_col > 0) {
        dimensions.rows = size.ws_row;
        dimensions.cols = size.ws_col;
    }
#endif
    return dimensions;
}

bool terminal_read_byte(unsigned char& byte, int timeout_ms) {
#if defined(_WIN32)
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (input == nullptr || input == INVALID_HANDLE_VALUE) return false;
    const DWORD timeout = timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms);
    if (WaitForSingleObject(input, timeout) != WAIT_OBJECT_0) return false;
    DWORD count = 0;
    return ReadFile(input, &byte, 1, &count, nullptr) && count == 1;
#else
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(STDIN_FILENO, &read_fds);
    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    const int ready = select(STDIN_FILENO + 1, &read_fds, nullptr, nullptr, &timeout);
    if (ready <= 0 || !FD_ISSET(STDIN_FILENO, &read_fds)) return false;
    return ::read(STDIN_FILENO, &byte, 1) == 1;
#endif
}

}  // namespace ainiux::tui
