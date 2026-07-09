#include "editor/detail/editor_common.hpp"

#include <utility>

#include "common.hpp"

namespace pkchat::editor {

void update_preferred_column(EditorState& state) {
    state.preferred_column = state.text.display_column_for_offset(state.cursor);
}

std::string trim_ascii_copy(std::string text) { return pkchat::ascii_trim(std::move(text)); }

namespace detail {

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

}  // namespace pkchat::editor
