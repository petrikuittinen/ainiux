#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>
#include <vector>

namespace ainiux::tui::detail {

struct TerminalFrame {
    explicit TerminalFrame(int terminal_rows, int terminal_cols);

    void set_row(int row, std::string command);
    void append_to_row(int row, std::string command);

    int rows = 1;
    int cols = 1;
    int cursor_row = 1;
    int cursor_col = 1;
    std::vector<std::string> row_commands;
};

class TerminalFrameRenderer {
   public:
    // Returns the number of terminal rows emitted. A cursor-only move returns zero.
    std::size_t present(const TerminalFrame& frame, std::ostream& output);
    void invalidate();

   private:
    int rows_ = 0;
    int cols_ = 0;
    int cursor_row_ = 0;
    int cursor_col_ = 0;
    bool valid_ = false;
    std::vector<std::string> row_commands_;
};

}  // namespace ainiux::tui::detail
