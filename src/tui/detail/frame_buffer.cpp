#include "tui/detail/frame_buffer.hpp"

#include <algorithm>
#include <ostream>
#include <utility>

namespace ainiux::tui::detail {
namespace {

std::string terminal_position(int row, int col) {
    return "\x1b[" + std::to_string(std::max(1, row)) + ";" +
           std::to_string(std::max(1, col)) + "H";
}

std::string blank_row_command(int row) {
    return terminal_position(row, 1) + "\x1b[0m\x1b[K";
}

}  // namespace

TerminalFrame::TerminalFrame(int terminal_rows, int terminal_cols)
    : rows(std::max(1, terminal_rows)),
      cols(std::max(1, terminal_cols)),
      row_commands(static_cast<std::size_t>(rows)) {
    for (int row = 1; row <= rows; ++row) {
        row_commands[static_cast<std::size_t>(row - 1)] = blank_row_command(row);
    }
}

void TerminalFrame::set_row(int row, std::string command) {
    if (row < 1 || row > rows) {
        return;
    }
    row_commands[static_cast<std::size_t>(row - 1)] = std::move(command);
}

void TerminalFrame::append_to_row(int row, std::string command) {
    if (row < 1 || row > rows) {
        return;
    }
    row_commands[static_cast<std::size_t>(row - 1)] += std::move(command);
}

std::size_t TerminalFrameRenderer::present(const TerminalFrame& frame,
                                           std::ostream& output) {
    const bool full_redraw =
        !valid_ || rows_ != frame.rows || cols_ != frame.cols ||
        row_commands_.size() != frame.row_commands.size();
    std::string bytes;
    std::size_t changed_rows = 0;
    for (std::size_t index = 0; index < frame.row_commands.size(); ++index) {
        if (!full_redraw && row_commands_[index] == frame.row_commands[index]) {
            continue;
        }
        if (changed_rows == 0 && full_redraw) {
            bytes += "\x1b[?25l";
        }
        bytes += frame.row_commands[index];
        ++changed_rows;
    }

    const int cursor_row = std::min(frame.rows, std::max(1, frame.cursor_row));
    const int cursor_col = std::min(frame.cols, std::max(1, frame.cursor_col));
    const bool cursor_moved =
        !valid_ || cursor_row_ != cursor_row || cursor_col_ != cursor_col;
    if (changed_rows > 0) {
        bytes += terminal_position(cursor_row, cursor_col);
        if (full_redraw) {
            bytes += "\x1b[?25h";
        }
    } else if (cursor_moved) {
        // Moving an already-visible cursor does not toggle its visibility or blink phase.
        bytes += terminal_position(cursor_row, cursor_col);
    }

    if (!bytes.empty()) {
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.flush();
    }

    rows_ = frame.rows;
    cols_ = frame.cols;
    cursor_row_ = cursor_row;
    cursor_col_ = cursor_col;
    valid_ = true;
    row_commands_ = frame.row_commands;
    return changed_rows;
}

void TerminalFrameRenderer::invalidate() {
    rows_ = 0;
    cols_ = 0;
    cursor_row_ = 0;
    cursor_col_ = 0;
    valid_ = false;
    row_commands_.clear();
}

}  // namespace ainiux::tui::detail
