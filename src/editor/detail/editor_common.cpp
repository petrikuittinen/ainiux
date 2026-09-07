#include "editor/detail/editor_common.hpp"

#include <algorithm>
#include <utility>

#include "common.hpp"

namespace ainiux::editor {

void update_preferred_column(EditorState& state) {
    state.preferred_column = state.text.display_column_for_offset(state.cursor, state.tab_width);
}

std::string trim_ascii_copy(std::string text) { return ainiux::ascii_trim(std::move(text)); }

namespace detail {

std::string longest_common_prefix(const std::vector<std::string>& values,
                                  bool case_sensitive) {
    if (values.empty()) return {};
    std::string prefix = values.front();
    for (std::size_t index = 1; index < values.size(); ++index) {
        std::size_t length = 0;
        const std::size_t limit = std::min(prefix.size(), values[index].size());
        while (length < limit) {
            char left = prefix[length];
            char right = values[index][length];
            if (!case_sensitive) {
                if (left >= 'A' && left <= 'Z') left = static_cast<char>(left - 'A' + 'a');
                if (right >= 'A' && right <= 'Z') right = static_cast<char>(right - 'A' + 'a');
            }
            if (left != right) break;
            ++length;
        }
        prefix.resize(length);
    }
    return prefix;
}

size_t selection_end_exclusive_for(const Selection& selection, const PieceTable& text, size_t cursor) {
    if (!selection.has_range()) {
        return selection.start();
    }

    const size_t start = selection.start();
    const size_t raw = selection.end();
    if (raw <= start || raw > text.size()) {
        return raw;
    }

    auto maybe_extend = [&](size_t pos) -> size_t {
        if (pos >= text.size()) {
            return pos;
        }
        const size_t extended = text.next_char_offset(pos);
        if (extended <= pos) {
            return pos;
        }
        if (extended >= text.size() && pos == text.previous_char_offset(text.size())) {
            return pos;
        }
        return extended;
    };

    if (selection.anchor > selection.active && raw == selection.anchor && raw < text.size()) {
        const unsigned char ch = static_cast<unsigned char>(text.str()[raw]);
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
            return maybe_extend(raw);
        }
        return raw;
    }
    if (selection.anchor < selection.active && raw == selection.active && raw == cursor &&
        raw - selection.anchor > 2 && raw < text.size()) {
        const unsigned char ch = static_cast<unsigned char>(text.str()[raw]);
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
            return maybe_extend(raw);
        }
    }
    return raw;
}

}  // namespace detail

}  // namespace ainiux::editor
