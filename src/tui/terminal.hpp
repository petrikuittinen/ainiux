#pragma once

#include "common.hpp"

#if !defined(_WIN32)
#include <termios.h>
#endif

namespace ainiux::tui {

struct TerminalDimensions {
    int rows = 24;
    int cols = 80;
};

class TerminalSession {
   public:
    TerminalSession() = default;
    ~TerminalSession();
    TerminalSession(const TerminalSession&) = delete;
    TerminalSession& operator=(const TerminalSession&) = delete;

    Error enter();
    void restore();

   private:
#if defined(_WIN32)
    void* input_handle_ = nullptr;
    void* output_handle_ = nullptr;
    unsigned long original_input_mode_ = 0;
    unsigned long original_output_mode_ = 0;
    unsigned int original_input_code_page_ = 0;
    unsigned int original_output_code_page_ = 0;
#else
    termios original_{};
#endif
    bool active_ = false;
};

TerminalDimensions terminal_dimensions();
bool terminal_input_is_interactive();
bool terminal_output_is_interactive();
bool terminal_read_byte(unsigned char& byte, int timeout_ms);

}  // namespace ainiux::tui
