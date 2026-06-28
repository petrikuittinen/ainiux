#include "editor/editor.hpp"
#include "editor/detail/unicode.hpp"

#include <algorithm>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace pkchat::editor {

using detail::byte_offset_for_display_column;
using detail::display_column_for_text;
using detail::next_grapheme_offset;
using detail::previous_grapheme_offset;

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
    if (pos == 0 || total_size_ == 0) {
        return 0;
    }
    const std::string content = str();
    return previous_grapheme_offset(content, std::min(pos, total_size_));
}

size_t PieceTable::next_char_offset(size_t pos) const {
    if (pos >= total_size_) {
        return total_size_;
    }
    const std::string content = str();
    return next_grapheme_offset(content, std::min(pos, total_size_));
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

std::string PieceTable::range_text(size_t start, size_t length) const {
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


}  // namespace pkchat::editor
