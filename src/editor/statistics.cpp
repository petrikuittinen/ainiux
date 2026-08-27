#include "editor/statistics.hpp"

#include "editor/detail/unicode.hpp"

#include <sstream>

namespace ainiux::editor {
namespace {

bool is_ascii_whitespace(unsigned char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

}  // namespace

EditorBufferStats buffer_statistics(const std::string& text) {
    EditorBufferStats stats;
    stats.total_lines = 1;
    bool line_empty = true;
    bool in_word = false;
    size_t pos = 0;
    while (pos < text.size()) {
        const unsigned char ch = static_cast<unsigned char>(text[pos]);
        if (ch == '\n') {
            ++stats.total_lines;
            if (line_empty) {
                ++stats.empty_lines;
            }
            line_empty = true;
            in_word = false;
            ++pos;
            continue;
        }
        const size_t next = detail::next_grapheme_offset(text, pos);
        if (is_ascii_whitespace(ch)) {
            in_word = false;
            if (ch != '\r') {
                ++stats.characters;
            }
            pos = next;
            continue;
        }
        line_empty = false;
        if (!in_word) {
            ++stats.words;
            in_word = true;
        }
        ++stats.characters;
        pos = next;
    }
    if (line_empty) {
        ++stats.empty_lines;
    }
    return stats;
}

std::string format_statistics_message(const EditorBufferStats& stats, bool selection) {
    std::ostringstream out;
    if (selection) {
        out << "Selection — ";
    }
    out << "Character count: " << stats.characters
        << " · Word count: " << stats.words
        << " · Total lines: " << stats.total_lines
        << " · Empty lines: " << stats.empty_lines;
    return out.str();
}

}  // namespace ainiux::editor
