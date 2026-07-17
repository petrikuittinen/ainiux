#pragma once

#include <termios.h>

#include "common.hpp"

namespace ainiux::tui {

class TerminalSession {
   public:
    TerminalSession() = default;
    ~TerminalSession();
    TerminalSession(const TerminalSession&) = delete;
    TerminalSession& operator=(const TerminalSession&) = delete;

    Error enter();
    void restore();

   private:
    termios original_{};
    bool active_ = false;
};

}  // namespace ainiux::tui