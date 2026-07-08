#include "editor/editor.hpp"
#include "editor/detail/editor_common.hpp"
#include "editor/detail/wrap.hpp"

#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace pkchat::editor {

using detail::WrapSegment;
using detail::WrappedLocation;
using detail::cursor_in_wrapped_line;
using detail::offset_for_wrapped_location;
using detail::selection_end_exclusive_for;
using detail::wrap_line_segments;
using detail::wrapped_location_for_offset;
using detail::wrapped_row_count;

EditorState EditorState::from_text(std::string content) {
    EditorState state;
    state.text = PieceTable::from_string(std::move(content));
    state.selection.clear(state.cursor);
    return state;
}

EditorSnapshot EditorState::snapshot() const {
    return {text.str(), cursor, preferred_column, scroll_line, scroll_column};
}

void EditorState::restore_snapshot(const EditorSnapshot& snapshot) {
    text = PieceTable::from_string(snapshot.content);
    cursor = std::min(snapshot.cursor, text.size());
    preferred_column = snapshot.preferred_column;
    scroll_line = snapshot.scroll_line;
    scroll_column = snapshot.scroll_column;
    selection.clear(cursor);
}

void EditorState::remember_undo(EditorSnapshot snapshot) {
    redo_stack_.clear();
    if (undo_limit_ == 0) {
        return;
    }
    while (undo_stack_.size() >= undo_limit_) {
        undo_stack_.erase(undo_stack_.begin());
    }
    undo_stack_.push_back(std::move(snapshot));
}

size_t EditorState::autosave_pending_bytes() const {
    return autosave_pending_bytes_;
}

void EditorState::record_autosave_change(size_t bytes) {
    if (bytes > 0) {
        autosave_pending_bytes_ += bytes;
    }
}

void EditorState::reset_autosave_pending() {
    autosave_pending_bytes_ = 0;
}

Error EditorState::insert(const std::string& value) {
    if (value.empty()) {
        return ok_error();
    }
    if (selection.has_range()) {
        Error replaced =
            replace(selection.start(), selection_end_exclusive() - selection.start(), value);
        if (replaced.ok()) {
            selection.clear(cursor);
        }
        return replaced;
    }
    EditorSnapshot before = snapshot();
    Error err = text.insert(cursor, value);
    if (!err.ok()) {
        return err;
    }
    remember_undo(std::move(before));
    cursor += value.size();
    dirty = true;
    record_autosave_change(value.size());
    update_preferred_column(*this);
    return ok_error();
}

Error EditorState::insert_without_undo(const std::string& value) {
    if (value.empty()) {
        return ok_error();
    }
    Error err = text.insert(cursor, value);
    if (!err.ok()) {
        return err;
    }
    cursor += value.size();
    dirty = true;
    update_preferred_column(*this);
    return ok_error();
}

void EditorState::finalize_stream_edit(const EditorSnapshot& before) {
    if (snapshot().content != before.content || cursor != before.cursor) {
        remember_undo(before);
        const size_t before_size = before.content.size();
        const size_t after_size = text.size();
        record_autosave_change(after_size >= before_size ? after_size - before_size
                                                         : before_size - after_size);
    }
}

EditorSnapshot EditorState::capture_state() const {
    return snapshot();
}

void EditorState::restore_captured_state(const EditorSnapshot& snapshot) {
    restore_snapshot(snapshot);
}

Error EditorState::replace(size_t pos, size_t count, const std::string& value) {
    if (pos > text.size()) {
        return {ErrorCode::BadArgs, "editor replace position is past the end of the buffer"};
    }
    count = std::min(count, text.size() - pos);
    const std::string before_text = text.str();
    if (before_text.substr(pos, count) == value) {
        return ok_error();
    }

    EditorSnapshot before{before_text, cursor, preferred_column, scroll_line, scroll_column};
    PieceTable replacement = text;
    Error err = replacement.erase(pos, count);
    if (!err.ok()) {
        return err;
    }
    err = replacement.insert(pos, value);
    if (!err.ok()) {
        return err;
    }
    text = std::move(replacement);
    remember_undo(std::move(before));
    cursor = pos + value.size();
    selection.clear(cursor);
    dirty = true;
    record_autosave_change(count + value.size());
    update_preferred_column(*this);
    return ok_error();
}

Error EditorState::erase_before_cursor() {
    if (selection.has_range()) {
        const size_t start = selection.start();
        Error err = replace(start, selection_end_exclusive() - start, "");
        if (err.ok()) {
            cursor = start;
            selection.clear(cursor);
        }
        return err;
    }
    if (cursor == 0) {
        return ok_error();
    }
    EditorSnapshot before = snapshot();
    const size_t previous = text.previous_char_offset(cursor);
    const size_t deleted = cursor - previous;
    Error err = text.erase(previous, deleted);
    if (!err.ok()) {
        return err;
    }
    remember_undo(std::move(before));
    cursor = previous;
    dirty = true;
    record_autosave_change(deleted);
    update_preferred_column(*this);
    return ok_error();
}

Error EditorState::erase_at_cursor() {
    if (cursor >= text.size()) {
        return ok_error();
    }
    EditorSnapshot before = snapshot();
    const size_t next = text.next_char_offset(cursor);
    const size_t deleted = next - cursor;
    Error err = text.erase(cursor, deleted);
    if (!err.ok()) {
        return err;
    }
    remember_undo(std::move(before));
    dirty = true;
    record_autosave_change(deleted);
    update_preferred_column(*this);
    return ok_error();
}

bool EditorState::undo() {
    if (undo_stack_.empty()) {
        return false;
    }
    while (redo_stack_.size() >= undo_limit_ && undo_limit_ > 0) {
        redo_stack_.erase(redo_stack_.begin());
    }
    redo_stack_.push_back(snapshot());
    const EditorSnapshot target = std::move(undo_stack_.back());
    undo_stack_.pop_back();
    const size_t before_size = text.size();
    restore_snapshot(target);
    const size_t after_size = text.size();
    record_autosave_change(after_size >= before_size ? after_size - before_size : before_size - after_size);
    dirty = true;
    return true;
}

bool EditorState::redo() {
    if (redo_stack_.empty()) {
        return false;
    }
    while (undo_stack_.size() >= undo_limit_ && undo_limit_ > 0) {
        undo_stack_.erase(undo_stack_.begin());
    }
    undo_stack_.push_back(snapshot());
    const EditorSnapshot target = std::move(redo_stack_.back());
    redo_stack_.pop_back();
    const size_t before_size = text.size();
    restore_snapshot(target);
    const size_t after_size = text.size();
    record_autosave_change(after_size >= before_size ? after_size - before_size : before_size - after_size);
    dirty = true;
    return true;
}

bool EditorState::can_undo() const {
    return !undo_stack_.empty();
}

bool EditorState::can_redo() const {
    return !redo_stack_.empty();
}

void EditorState::set_undo_limit(size_t limit) {
    undo_limit_ = limit;
    if (undo_limit_ == 0) {
        undo_stack_.clear();
        redo_stack_.clear();
        return;
    }
    while (undo_stack_.size() > undo_limit_) {
        undo_stack_.erase(undo_stack_.begin());
    }
    while (redo_stack_.size() > undo_limit_) {
        redo_stack_.erase(redo_stack_.begin());
    }
}

size_t EditorState::undo_limit() const {
    return undo_limit_;
}

void EditorState::clear_undo_history() {
    undo_stack_.clear();
    redo_stack_.clear();
}

bool EditorState::search(const std::string& needle) {
    if (needle.empty()) {
        return false;
    }
    const std::string haystack = text.str();
    if (haystack.empty()) {
        return false;
    }

    size_t found = haystack.find(needle, std::min(cursor, haystack.size()));
    if (found == std::string::npos) {
        found = haystack.find(needle);
    }
    if (found == std::string::npos) {
        return false;
    }
    cursor = found;
    selection.clear(cursor);
    update_preferred_column(*this);
    return true;
}

bool EditorState::search_next(const std::string& needle) {
    if (needle.empty()) {
        return false;
    }
    const std::string haystack = text.str();
    if (haystack.empty()) {
        return false;
    }

    const size_t start = cursor < haystack.size() ? cursor + 1 : 0;
    size_t found = haystack.find(needle, start);
    if (found == std::string::npos) {
        found = haystack.find(needle);
    }
    if (found == std::string::npos) {
        return false;
    }
    cursor = found;
    selection.clear(cursor);
    update_preferred_column(*this);
    return true;
}

bool EditorState::search_previous(const std::string& needle) {
    if (needle.empty()) {
        return false;
    }
    const std::string haystack = text.str();
    if (haystack.empty()) {
        return false;
    }

    const size_t start = cursor == 0 ? haystack.size() : cursor - 1;
    size_t found = haystack.rfind(needle, start);
    if (found == std::string::npos) {
        found = haystack.rfind(needle);
    }
    if (found == std::string::npos) {
        return false;
    }
    cursor = found;
    selection.clear(cursor);
    update_preferred_column(*this);
    return true;
}

Error EditorState::replace_all_from(size_t start,
                                    const std::string& needle,
                                    const std::string& value,
                                    size_t& replacements) {
    replacements = 0;
    if (needle.empty()) {
        return {ErrorCode::BadArgs, "editor replace search string is empty"};
    }
    const std::string before_text = text.str();
    if (start > before_text.size()) {
        return {ErrorCode::BadArgs, "editor replace start position is past the end of the buffer"};
    }

    std::string replaced;
    replaced.reserve(before_text.size());
    replaced.append(before_text, 0, start);

    size_t scan = start;
    size_t cursor_after_last_replacement = cursor;
    while (scan < before_text.size()) {
        const size_t found = before_text.find(needle, scan);
        if (found == std::string::npos) {
            break;
        }
        replaced.append(before_text, scan, found - scan);
        replaced.append(value);
        cursor_after_last_replacement = replaced.size();
        ++replacements;
        scan = found + needle.size();
    }
    replaced.append(before_text, scan, std::string::npos);

    if (replacements == 0 || replaced == before_text) {
        return ok_error();
    }

    EditorSnapshot before{before_text, cursor, preferred_column, scroll_line, scroll_column};
    text = PieceTable::from_string(std::move(replaced));
    remember_undo(std::move(before));
    cursor = std::min(cursor_after_last_replacement, text.size());
    dirty = true;
    record_autosave_change(replacements * (needle.size() + value.size()));
    update_preferred_column(*this);
    return ok_error();
}

void EditorState::clear_selection() {
    selection.clear(cursor);
}

void EditorState::select_all() {
    selection.anchor = 0;
    selection.active = text.size();
    cursor = text.size();
    update_preferred_column(*this);
}

size_t EditorState::selection_end_exclusive() const {
    return selection_end_exclusive_for(selection, text, cursor);
}

std::string EditorState::selected_text() const {
    if (!selection.has_range()) {
        return "";
    }
    const size_t start = selection.start();
    const size_t end = selection_end_exclusive();
    return text.range_text(start, end - start);
}

Error EditorState::copy_selection(Clipboard& clipboard) {
    if (!selection.has_range()) {
        return {ErrorCode::BadArgs, "no selection to copy"};
    }
    clipboard.set(selected_text());
    return ok_error();
}

Error EditorState::cut_selection(Clipboard& clipboard) {
    if (!selection.has_range()) {
        return {ErrorCode::BadArgs, "no selection to cut"};
    }
    clipboard.set(selected_text());
    const size_t start = selection.start();
    Error err = replace(start, selection_end_exclusive() - start, "");
    if (err.ok()) {
        cursor = start;
        selection.clear(cursor);
    }
    return err;
}

Error EditorState::paste(Clipboard& clipboard) {
    if (clipboard.empty()) {
        return {ErrorCode::BadArgs, "clipboard is empty"};
    }
    if (selection.has_range()) {
        Error err = replace(selection.start(),
                            selection_end_exclusive() - selection.start(),
                            clipboard.text());
        if (err.ok()) {
            selection.clear(cursor);
        }
        return err;
    }
    return insert(clipboard.text());
}

void EditorState::begin_movement(bool extend_selection) {
    if (!extend_selection) {
        selection.clear(cursor);
    } else if (!selection.has_range()) {
        selection.anchor = cursor;
        selection.active = cursor;
    }
}

void EditorState::finish_movement(bool extend_selection) {
    if (extend_selection) {
        selection.active = cursor;
    } else {
        selection.clear(cursor);
    }
}

void EditorState::apply_movement(MovementKey key,
                                 const Rect& rect,
                                 bool extend_selection,
                                 bool /*alt*/,
                                 bool ctrl) {
    begin_movement(extend_selection);
    switch (key) {
        case MovementKey::Left:
            move_left();
            break;
        case MovementKey::Right:
            move_right();
            break;
        case MovementKey::Up:
            move_up(rect);
            break;
        case MovementKey::Down:
            move_down(rect);
            break;
        case MovementKey::PageUp:
            page_up(rect);
            break;
        case MovementKey::PageDown:
            page_down(rect);
            break;
        case MovementKey::Home:
            if (ctrl) {
                move_home();
            } else {
                move_line_home(rect);
            }
            break;
        case MovementKey::End:
            if (ctrl) {
                move_end();
            } else {
                move_line_end(rect);
            }
            break;
    }
    finish_movement(extend_selection);
    if (key == MovementKey::PageUp || key == MovementKey::PageDown || key == MovementKey::Home ||
        key == MovementKey::End) {
        ensure_cursor_visible(rect);
    }
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

void EditorState::move_up(const Rect& rect) {
    if (vertical_movement == VerticalMovementMode::VisualRow) {
        move_up_visual(rect);
        return;
    }
    move_up();
}

void EditorState::move_down(const Rect& rect) {
    if (vertical_movement == VerticalMovementMode::VisualRow) {
        move_down_visual(rect);
        return;
    }
    move_down();
}

void EditorState::page_up(const Rect& rect) {
    const int count = std::max(1, rect.height);
    for (int i = 0; i < count; ++i) {
        const size_t previous = cursor;
        move_up(rect);
        if (cursor == previous) {
            break;
        }
    }
    ensure_cursor_visible(rect);
}

void EditorState::page_down(const Rect& rect) {
    const int count = std::max(1, rect.height);
    for (int i = 0; i < count; ++i) {
        const size_t previous = cursor;
        move_down(rect);
        if (cursor == previous) {
            break;
        }
    }
    ensure_cursor_visible(rect);
}

void EditorState::move_up_visual(const Rect& rect) {
    const size_t width = static_cast<size_t>(std::max(1, rect.width));
    const WrappedLocation location = wrapped_location_for_offset(text, cursor, width);
    if (location.segment > 0) {
        cursor = offset_for_wrapped_location(text, location.line, location.segment - 1, preferred_column, width);
        return;
    }
    if (location.line == 0) {
        return;
    }
    const size_t previous_line = location.line - 1;
    const size_t previous_rows = wrapped_row_count(text.line_text(previous_line), width);
    cursor = offset_for_wrapped_location(text, previous_line, previous_rows - 1, preferred_column, width);
}

void EditorState::move_down_visual(const Rect& rect) {
    const size_t width = static_cast<size_t>(std::max(1, rect.width));
    const WrappedLocation location = wrapped_location_for_offset(text, cursor, width);
    const size_t current_rows = wrapped_row_count(text.line_text(location.line), width);
    if (location.segment + 1 < current_rows) {
        cursor = offset_for_wrapped_location(text, location.line, location.segment + 1, preferred_column, width);
        return;
    }
    if (location.line + 1 >= text.line_count()) {
        return;
    }
    cursor = offset_for_wrapped_location(text, location.line + 1, 0, preferred_column, width);
}

void EditorState::move_home() {
    cursor = 0;
    preferred_column = 0;
    scroll_line = 0;
    scroll_column = 0;
    selection.clear(cursor);
}

void EditorState::move_end() {
    cursor = text.size();
    update_preferred_column(*this);
    selection.clear(cursor);
}

void EditorState::move_line_home(const Rect& rect) {
    const size_t width = static_cast<size_t>(std::max(1, rect.width));
    const WrappedLocation location = wrapped_location_for_offset(text, cursor, width);
    const std::string line_text_value = text.line_text(location.line);
    const std::vector<WrapSegment> segments = wrap_line_segments(line_text_value, width);
    const WrapSegment& segment = segments[std::min(location.segment, segments.size() - 1)];
    cursor = text.line_start(location.line) + segment.start;
    update_preferred_column(*this);
    selection.clear(cursor);
}

void EditorState::move_line_end(const Rect& rect) {
    const size_t width = static_cast<size_t>(std::max(1, rect.width));
    const WrappedLocation location = wrapped_location_for_offset(text, cursor, width);
    const std::string line_text_value = text.line_text(location.line);
    const std::vector<WrapSegment> segments = wrap_line_segments(line_text_value, width);
    const WrapSegment& segment = segments[std::min(location.segment, segments.size() - 1)];
    cursor = text.line_start(location.line) + segment.end;
    update_preferred_column(*this);
    selection.clear(cursor);
}

Error EditorState::kill_to_line_end() {
    const size_t line = text.line_for_offset(cursor);
    const size_t start = text.line_start(line);
    const size_t length = text.line_length(line);
    const size_t end = start + length;
    if (cursor < end) {
        EditorSnapshot before = snapshot();
        Error err = text.erase(cursor, end - cursor);
        if (!err.ok()) {
            return err;
        }
        remember_undo(std::move(before));
        dirty = true;
        record_autosave_change(end - cursor);
        update_preferred_column(*this);
        return ok_error();
    }
    if (length != 0 || text.line_count() <= 1) {
        return ok_error();
    }

    size_t erase_pos = start;
    if (line + 1 >= text.line_count()) {
        erase_pos = start == 0 ? 0 : start - 1;
    }
    EditorSnapshot before = snapshot();
    Error err = text.erase(erase_pos, 1);
    if (!err.ok()) {
        return err;
    }
    remember_undo(std::move(before));
    cursor = std::min(erase_pos, text.size());
    dirty = true;
    record_autosave_change(1);
    update_preferred_column(*this);
    return ok_error();
}

void EditorState::ensure_cursor_visible(const Rect& rect) {
    const size_t line = text.line_for_offset(cursor);
    const size_t height = static_cast<size_t>(std::max(1, rect.height));
    const size_t width = static_cast<size_t>(std::max(1, rect.width));

    size_t cursor_row = 0;
    for (size_t i = 0; i < line; ++i) {
        cursor_row += wrapped_row_count(text.line_text(i), width);
    }
    const size_t line_start_offset = text.line_start(line);
    const std::string line_text_value = text.line_text(line);
    cursor_row += cursor_in_wrapped_line(line_text_value, cursor - line_start_offset, width).row;

    if (cursor_row < scroll_line) {
        scroll_line = cursor_row;
    } else if (cursor_row >= scroll_line + height) {
        scroll_line = cursor_row - height + 1;
    }
    scroll_column = 0;
}

RenderedPanel EditorState::render(const Rect& rect) const {
    const std::optional<Selection> active_selection =
        selection.has_range() ? std::optional<Selection>(selection) : std::nullopt;
    return render_panel(text, rect, cursor, scroll_line, scroll_column, active_selection);
}


}  // namespace pkchat::editor
