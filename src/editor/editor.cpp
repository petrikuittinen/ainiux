#include "editor/editor.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <ostream>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

namespace pkchat::editor {
namespace {

constexpr size_t kTabStop = 4;

size_t utf8_len(unsigned char ch, size_t remaining) {
    if (ch < 0x80U) return 1;
    if ((ch & 0xE0U) == 0xC0U && remaining >= 2) return 2;
    if ((ch & 0xF0U) == 0xE0U && remaining >= 3) return 3;
    if ((ch & 0xF8U) == 0xF0U && remaining >= 4) return 4;
    return 1;
}

size_t display_width_at(const std::string& text, size_t pos, size_t column) {
    const unsigned char ch = static_cast<unsigned char>(text[pos]);
    if (ch == '\t') {
        return kTabStop - (column % kTabStop);
    }
    if (ch < 0x20U || ch == 0x7FU) {
        return 1;
    }
    return 1;
}

size_t display_column_for_text(const std::string& text, size_t byte_offset) {
    size_t column = 0;
    size_t pos = 0;
    const size_t limit = std::min(byte_offset, text.size());
    while (pos < limit) {
        const size_t len = utf8_len(static_cast<unsigned char>(text[pos]), text.size() - pos);
        column += display_width_at(text, pos, column);
        pos += std::min(len, limit - pos);
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
        const size_t len = utf8_len(static_cast<unsigned char>(text[pos]), text.size() - pos);
        column += width;
        pos += len;
    }
    return pos;
}

std::string display_slice(const std::string& text, size_t scroll_column, size_t width) {
    std::string out;
    size_t column = 0;
    size_t visible = 0;
    size_t pos = 0;
    while (pos < text.size() && visible < width) {
        const unsigned char ch = static_cast<unsigned char>(text[pos]);
        const size_t len = utf8_len(ch, text.size() - pos);
        const size_t char_width = display_width_at(text, pos, column);
        const size_t next_column = column + char_width;

        if (next_column <= scroll_column) {
            column = next_column;
            pos += len;
            continue;
        }

        if (ch == '\t') {
            for (size_t tab_col = column; tab_col < next_column && visible < width; ++tab_col) {
                if (tab_col >= scroll_column) {
                    out.push_back(' ');
                    ++visible;
                }
            }
        } else if (ch < 0x20U || ch == 0x7FU) {
            out.push_back('?');
            ++visible;
        } else {
            if (column >= scroll_column) {
                out.append(text, pos, len);
                ++visible;
            }
        }

        column = next_column;
        pos += len;
    }
    while (visible < width) {
        out.push_back(' ');
        ++visible;
    }
    return out;
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
        std::cout << "\x1b[?1049h\x1b[?25h\x1b[2J\x1b[H";
        std::cout.flush();
        return ok_error();
    }

    void restore() {
        if (!active_) {
            return;
        }
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_);
        std::cout << "\x1b[0m\x1b[?25h\x1b[2J\x1b[H\x1b[?1049l";
        std::cout.flush();
        active_ = false;
    }

   private:
    termios original_{};
    bool active_ = false;
};

bool read_byte(unsigned char& out, int timeout_ms) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(STDIN_FILENO, &read_fds);
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    const int ready = select(STDIN_FILENO + 1, &read_fds, nullptr, nullptr, &tv);
    if (ready <= 0 || !FD_ISSET(STDIN_FILENO, &read_fds)) {
        return false;
    }
    const ssize_t n = read(STDIN_FILENO, &out, 1);
    return n == 1;
}

void update_preferred_column(EditorState& state) {
    state.preferred_column = state.text.display_column_for_offset(state.cursor);
}

std::string editor_title(const EditorState& state, const std::string& status) {
    std::ostringstream out;
    out << (state.path.empty() ? "[scratch]" : state.path);
    if (state.dirty) {
        out << " *";
    }
    out << "  " << status;
    out << "  Ctrl+S save | Ctrl+Q quit | Enter newline";
    return out.str();
}

void render_terminal(EditorState& state, const std::string& status) {
    const TerminalSize size = terminal_size();
    const int rows = std::max(4, size.rows);
    const int cols = std::max(20, size.cols);
    const int width = std::max(1, cols - 1);
    Rect panel_rect{1, 1, std::max(1, rows - 1), width};
    state.ensure_cursor_visible(panel_rect);
    const RenderedPanel panel = state.render(panel_rect);

    std::cout << "\x1b[?25l";
    for (int row = 0; row < panel_rect.height; ++row) {
        std::cout << "\x1b[" << (panel_rect.row + row) << ";" << panel_rect.col << "H\x1b[K";
        if (row < static_cast<int>(panel.lines.size())) {
            std::cout << panel.lines[static_cast<size_t>(row)];
        }
    }

    std::cout << "\x1b[" << rows << ";1H\x1b[7m"
              << pad_or_clip_ascii(editor_title(state, status), width) << "\x1b[0m\x1b[K";
    const int cursor_row = panel.cursor.visible ? panel_rect.row + panel.cursor.row : panel_rect.row;
    const int cursor_col = panel.cursor.visible ? panel_rect.col + panel.cursor.col : panel_rect.col;
    std::cout << "\x1b[" << cursor_row << ";" << cursor_col << "H\x1b[?25h";
    std::cout.flush();
}

void handle_escape(EditorState& state, std::string& status) {
    std::string sequence;
    unsigned char ch = 0;
    while (sequence.size() < 16 && read_byte(ch, 25)) {
        sequence.push_back(static_cast<char>(ch));
        if ((ch >= 'A' && ch <= 'Z') || ch == '~') {
            break;
        }
    }

    if (sequence == "[A") {
        state.move_up();
    } else if (sequence == "[B") {
        state.move_down();
    } else if (sequence == "[C") {
        state.move_right();
    } else if (sequence == "[D") {
        state.move_left();
    } else if (sequence == "[H" || sequence == "[1~") {
        state.move_home();
    } else if (sequence == "[F" || sequence == "[4~") {
        state.move_end();
    } else if (sequence == "[3~") {
        Error err = state.erase_at_cursor();
        if (!err.ok()) {
            status = err.message;
        }
    }
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
    if (pos == 0) {
        return 0;
    }
    size_t out = std::min(pos, total_size_) - 1;
    while (out > 0 && (static_cast<unsigned char>(char_at(out)) & 0xC0U) == 0x80U) {
        --out;
    }
    return out;
}

size_t PieceTable::next_char_offset(size_t pos) const {
    if (pos >= total_size_) {
        return total_size_;
    }
    const size_t remaining = total_size_ - pos;
    const size_t len = utf8_len(static_cast<unsigned char>(char_at(pos)), remaining);
    return std::min(total_size_, pos + len);
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
    return state;
}

Error EditorState::insert(const std::string& value) {
    Error err = text.insert(cursor, value);
    if (!err.ok()) {
        return err;
    }
    cursor += value.size();
    dirty = true;
    update_preferred_column(*this);
    return ok_error();
}

Error EditorState::erase_before_cursor() {
    if (cursor == 0) {
        return ok_error();
    }
    const size_t previous = text.previous_char_offset(cursor);
    Error err = text.erase(previous, cursor - previous);
    if (!err.ok()) {
        return err;
    }
    cursor = previous;
    dirty = true;
    update_preferred_column(*this);
    return ok_error();
}

Error EditorState::erase_at_cursor() {
    if (cursor >= text.size()) {
        return ok_error();
    }
    const size_t next = text.next_char_offset(cursor);
    Error err = text.erase(cursor, next - cursor);
    if (!err.ok()) {
        return err;
    }
    dirty = true;
    update_preferred_column(*this);
    return ok_error();
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

void EditorState::move_home() {
    cursor = text.line_start(text.line_for_offset(cursor));
    update_preferred_column(*this);
}

void EditorState::move_end() {
    const size_t line = text.line_for_offset(cursor);
    cursor = text.line_start(line) + text.line_length(line);
    update_preferred_column(*this);
}

void EditorState::ensure_cursor_visible(const Rect& rect) {
    const size_t line = text.line_for_offset(cursor);
    const size_t column = text.display_column_for_offset(cursor);
    const size_t height = static_cast<size_t>(std::max(1, rect.height));
    const size_t width = static_cast<size_t>(std::max(1, rect.width));

    if (line < scroll_line) {
        scroll_line = line;
    } else if (line >= scroll_line + height) {
        scroll_line = line - height + 1;
    }

    if (column < scroll_column) {
        scroll_column = column;
    } else if (column >= scroll_column + width) {
        scroll_column = column - width + 1;
    }
}

RenderedPanel EditorState::render(const Rect& rect) const {
    return render_panel(text, rect, cursor, scroll_line, scroll_column);
}

RenderedPanel render_panel(const PieceTable& text,
                           const Rect& rect,
                           size_t cursor,
                           size_t scroll_line,
                           size_t scroll_column) {
    RenderedPanel rendered;
    const size_t height = static_cast<size_t>(std::max(0, rect.height));
    const size_t width = static_cast<size_t>(std::max(0, rect.width));
    rendered.lines.reserve(height);
    for (size_t row = 0; row < height; ++row) {
        const size_t line = scroll_line + row;
        if (line >= text.line_count()) {
            rendered.lines.push_back(std::string(width, ' '));
        } else {
            rendered.lines.push_back(display_slice(text.line_text(line), scroll_column, width));
        }
    }

    const size_t cursor_line = text.line_for_offset(cursor);
    const size_t cursor_column = text.display_column_for_offset(cursor);
    if (cursor_line >= scroll_line && cursor_line < scroll_line + height &&
        cursor_column >= scroll_column && cursor_column < scroll_column + width) {
        rendered.cursor.row = static_cast<int>(cursor_line - scroll_line);
        rendered.cursor.col = static_cast<int>(cursor_column - scroll_column);
        rendered.cursor.visible = true;
    }
    return rendered;
}

Error load_file(const std::string& path, PieceTable& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {ErrorCode::FileRead, "could not open editor file for reading: " + path};
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (!in.good() && !in.eof()) {
        return {ErrorCode::FileRead, "failed while reading editor file: " + path};
    }
    out = PieceTable::from_string(buffer.str());
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

int run_editor(const std::string& path, const std::string& save_as) {
    EditorState state;
    state.path = path.empty() ? save_as : path;
    std::string status = "Ready";
    if (!path.empty() && access(path.c_str(), F_OK) == 0) {
        Error err = load_file(path, state.text);
        if (!err.ok()) {
            std::cerr << error_code_name(err.code) << ": " << err.message << "\n";
            return 5;
        }
        status = "Loaded";
    } else if (!path.empty()) {
        status = "New file";
    }

    TerminalSession terminal;
    Error err = terminal.enter();
    if (!err.ok()) {
        std::cerr << error_code_name(err.code) << ": " << err.message << "\n";
        return err.code == ErrorCode::BadArgs ? 2 : 6;
    }

    bool quit = false;
    TerminalSize last_size = terminal_size();
    render_terminal(state, status);
    while (!quit) {
        unsigned char ch = 0;
        if (!read_byte(ch, 100)) {
            const TerminalSize current_size = terminal_size();
            if (current_size.rows != last_size.rows || current_size.cols != last_size.cols) {
                last_size = current_size;
                render_terminal(state, status);
            }
            continue;
        }

        if (ch == 17) {
            quit = true;
        } else if (ch == 3) {
            quit = true;
        } else if (ch == 19) {
            const std::string target = save_as.empty() ? state.path : save_as;
            Error save_error = save_file(target, state.text);
            if (save_error.ok()) {
                state.dirty = false;
                if (state.path.empty()) {
                    state.path = target;
                }
                status = "Saved";
            } else {
                status = save_error.message;
            }
        } else if (ch == 27) {
            handle_escape(state, status);
        } else if (ch == 127 || ch == 8) {
            Error erase_error = state.erase_before_cursor();
            if (!erase_error.ok()) {
                status = erase_error.message;
            }
        } else if (ch == '\r' || ch == '\n') {
            Error insert_error = state.insert("\n");
            if (!insert_error.ok()) {
                status = insert_error.message;
            }
        } else if (ch == '\t') {
            Error insert_error = state.insert("\t");
            if (!insert_error.ok()) {
                status = insert_error.message;
            }
        } else if (ch >= 0x20U) {
            const std::string text(1, static_cast<char>(ch));
            Error insert_error = state.insert(text);
            if (!insert_error.ok()) {
                status = insert_error.message;
            }
        }

        last_size = terminal_size();
        render_terminal(state, status);
    }
    return 0;
}

}  // namespace pkchat::editor
