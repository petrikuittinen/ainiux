#include "editor/editor.hpp"
#include "editor/detail/editor_common.hpp"
#include "editor/detail/wrap.hpp"

#include <atomic>
#include <limits>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace ainiux::editor {

using detail::WrapSegment;
using detail::WrappedLocation;
using detail::cursor_in_wrapped_line;
using detail::offset_for_wrapped_location;
using detail::selection_end_exclusive_for;
using detail::wrap_line_segments;
using detail::wrapped_location_for_offset;
using detail::wrapped_row_count;

namespace {

std::uint64_t next_buffer_id() {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

struct IndentChange {
    size_t position = 0;
    size_t removed = 0;
    size_t inserted = 0;
};

size_t map_indent_offset(size_t offset, const std::vector<IndentChange>& changes) {
    size_t mapped = offset;
    for (const IndentChange& change : changes) {
        if (offset < change.position) {
            break;
        }
        if (change.removed == 0 && offset == change.position) {
            continue;
        }
        const size_t removed_end = change.position + change.removed;
        if (offset < removed_end) {
            const size_t relative = offset - change.position;
            return mapped - relative + std::min(relative, change.inserted);
        }
        if (change.inserted >= change.removed) {
            mapped += change.inserted - change.removed;
        } else {
            mapped -= change.removed - change.inserted;
        }
    }
    return mapped;
}

struct IndentRemoval {
    size_t keep = 0;
    size_t remove = 0;
};

IndentRemoval leading_whitespace_removal(const std::string& text,
                                         size_t start,
                                         size_t end,
                                         size_t tab_width) {
    tab_width = std::max<size_t>(1, tab_width);
    size_t prefix_end = start;
    size_t column = 0;
    while (prefix_end < end && (text[prefix_end] == ' ' || text[prefix_end] == '\t')) {
        if (text[prefix_end] == '\t') {
            column += tab_width - (column % tab_width);
        } else {
            ++column;
        }
        ++prefix_end;
    }
    if (prefix_end == start) {
        return {};
    }
    const size_t target = column == 0
                              ? 0
                              : column - (column % tab_width == 0 ? tab_width
                                                                  : column % tab_width);
    size_t keep_end = start;
    column = 0;
    while (keep_end < prefix_end) {
        const size_t next_column = text[keep_end] == '\t'
                                       ? column + tab_width - (column % tab_width)
                                       : column + 1;
        if (next_column > target) {
            break;
        }
        column = next_column;
        ++keep_end;
    }
    return {keep_end - start, prefix_end - keep_end};
}

}  // namespace

EditorState::EditorState() : buffer_id_(next_buffer_id()) {}

EditorState EditorState::from_text(std::string content) {
    EditorState state;
    state.text = PieceTable::from_string(std::move(content));
    state.selection.clear(state.cursor);
    return state;
}

void EditorState::set_path(std::string value) {
    path = std::move(value);
    if (language_automatic) {
        redetect_language();
    }
}

void EditorState::set_language(highlight::Language value, bool automatic) {
    language = value;
    language_automatic = automatic;
    highlight_cache_.clear();
}

void EditorState::redetect_language() {
    language = highlight::detect_language(path);
    highlight_cache_.clear();
}

Error EditorState::begin_file_session(const std::string& value, bool target_exists) {
    release_file_session();
    Error canonical_error = canonicalize_editor_target(value, canonical_path);
    if (!canonical_error.ok()) {
        read_only = target_exists;
        return canonical_error;
    }
    Error fingerprint_error = fingerprint_file(canonical_path, disk_fingerprint);
    if (!fingerprint_error.ok()) {
        read_only = target_exists;
        return fingerprint_error;
    }
    has_disk_fingerprint = true;
    EditorLockAttempt attempt = acquire_editor_file_lock(canonical_path);
    if (!attempt.lock) {
        read_only = target_exists;
        return attempt.error;
    }
    file_lock = std::move(attempt.lock);
    read_only = false;
    reload_required = false;
    return ok_error();
}

Error EditorState::retry_file_lock() {
    if (!read_only) return ok_error();
    if (canonical_path.empty()) {
        return {ErrorCode::FileLock, "read-only editor buffer has no canonical file path"};
    }
    EditorLockAttempt attempt = acquire_editor_file_lock(canonical_path);
    if (!attempt.lock) return attempt.error;
    FileFingerprint current;
    Error fingerprint_error = fingerprint_file(canonical_path, current);
    if (!fingerprint_error.ok()) return fingerprint_error;
    file_lock = std::move(attempt.lock);
    if (!has_disk_fingerprint || current != disk_fingerprint) {
        reload_required = true;
        return {ErrorCode::FileLock,
                "file changed while this buffer was read-only: " + canonical_path +
                    ". Reload before editing"};
    }
    read_only = false;
    reload_required = false;
    return ok_error();
}

void EditorState::release_file_session() {
    file_lock.reset();
    canonical_path.clear();
    has_disk_fingerprint = false;
    reload_required = false;
}

Error EditorState::refresh_disk_fingerprint() {
    if (canonical_path.empty()) {
        has_disk_fingerprint = false;
        disk_fingerprint = {};
        return ok_error();
    }
    Error error = fingerprint_file(canonical_path, disk_fingerprint);
    has_disk_fingerprint = error.ok();
    return error;
}

Error EditorState::mutation_allowed() {
    if (!read_only) return ok_error();
    return retry_file_lock();
}

EditorSnapshot EditorState::snapshot() const {
    return {text.str(), cursor, preferred_column, scroll_line, scroll_column};
}

void EditorState::restore_snapshot(const EditorSnapshot& snapshot) {
    text = PieceTable::from_string(snapshot.content);
    word_index_.invalidate();
    cursor = std::min(snapshot.cursor, text.size());
    preferred_column = snapshot.preferred_column;
    scroll_line = snapshot.scroll_line;
    scroll_column = snapshot.scroll_column;
    selection.clear(cursor);
    ++revision_;
}

void EditorState::revert_to_snapshot(const EditorSnapshot& snapshot) {
    if (!mutation_allowed().ok()) {
        return;
    }
    const std::string before = text.str();
    restore_snapshot(snapshot);
    if (text.str() != before) {
        dirty = true;
    }
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

const WordIndex& EditorState::completion_word_index() const {
    if (!word_index_.initialized()) {
        word_index_.ensure(text.str());
    }
    return word_index_;
}

const WordIndex& EditorState::completion_word_index(const std::string& current_text) const {
    word_index_.ensure(current_text);
    return word_index_;
}

void EditorState::invalidate_word_index() {
    word_index_.invalidate();
}

Error EditorState::insert(const std::string& value) {
    Error writable = mutation_allowed();
    if (!writable.ok()) return writable;
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
    const size_t insert_position = cursor;
    Error err = text.insert(cursor, value);
    if (!err.ok()) {
        return err;
    }
    word_index_.apply_edit(before.content, insert_position, 0, value);
    remember_undo(std::move(before));
    cursor += value.size();
    ++revision_;
    dirty = true;
    record_autosave_change(value.size());
    update_preferred_column(*this);
    return ok_error();
}

Error EditorState::insert_without_undo(const std::string& value) {
    Error writable = mutation_allowed();
    if (!writable.ok()) return writable;
    if (value.empty()) {
        return ok_error();
    }
    Error err = text.insert(cursor, value);
    if (!err.ok()) {
        return err;
    }
    cursor += value.size();
    ++revision_;
    // Streaming writes can arrive in many small chunks. Rebuild lazily on the
    // next completion instead of repeatedly materializing the whole piece table.
    word_index_.invalidate();
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
    return replace_completion(pos, count, value, true);
}

Error EditorState::replace_completion(size_t pos,
                                      size_t count,
                                      const std::string& value,
                                      bool record_undo) {
    Error writable = mutation_allowed();
    if (!writable.ok()) return writable;
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
    word_index_.apply_edit(before_text, pos, count, value);
    if (record_undo) {
        remember_undo(std::move(before));
    }
    cursor = pos + value.size();
    ++revision_;
    selection.clear(cursor);
    dirty = true;
    record_autosave_change(count + value.size());
    update_preferred_column(*this);
    return ok_error();
}

Error EditorState::erase_before_cursor() {
    Error writable = mutation_allowed();
    if (!writable.ok()) return writable;
    // Backspace on a selection cuts it into the process clipboard so Ctrl+V
    // can restore it (dedicated Ctrl+X cut is reserved for window commands).
    if (selection.has_range()) {
        return cut_selection(shared_clipboard());
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
    word_index_.apply_edit(before.content, previous, deleted, "");
    remember_undo(std::move(before));
    cursor = previous;
    ++revision_;
    dirty = true;
    record_autosave_change(deleted);
    update_preferred_column(*this);
    return ok_error();
}

Error EditorState::erase_at_cursor() {
    Error writable = mutation_allowed();
    if (!writable.ok()) return writable;
    // Delete on a selection cuts it into the process clipboard (same as Backspace).
    if (selection.has_range()) {
        return cut_selection(shared_clipboard());
    }
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
    word_index_.apply_edit(before.content, cursor, deleted, "");
    remember_undo(std::move(before));
    ++revision_;
    dirty = true;
    record_autosave_change(deleted);
    update_preferred_column(*this);
    return ok_error();
}

bool EditorState::undo() {
    if (!mutation_allowed().ok()) return false;
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
    if (!mutation_allowed().ok()) return false;
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
    Error writable = mutation_allowed();
    if (!writable.ok()) return writable;
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
    word_index_.invalidate();
    remember_undo(std::move(before));
    ++revision_;
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
    Error writable = mutation_allowed();
    if (!writable.ok()) return writable;
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

Error EditorState::indent() {
    Error writable = mutation_allowed();
    if (!writable.ok()) return writable;
    tab_width = std::max<size_t>(1, std::min(tab_width, kMaxTabWidth));
    if (!selection.has_range()) {
        if (tab_style == TabStyle::Tab) {
            return insert("\t");
        }
        const size_t column = text.display_column_for_offset(cursor, tab_width);
        const size_t count = tab_width - (column % tab_width);
        try {
            return insert(std::string(count, ' '));
        } catch (const std::bad_alloc&) {
            return {ErrorCode::Internal, "not enough memory to insert editor indentation"};
        }
    }

    const size_t first_line = text.line_for_offset(selection.start());
    size_t last_line = text.line_for_offset(selection.end());
    if (selection.end() > selection.start() &&
        selection.end() == text.line_start(last_line) && last_line > first_line) {
        --last_line;
    }
    const size_t block_start = text.line_start(first_line);
    const size_t block_end = last_line + 1 < text.line_count()
                                 ? text.line_start(last_line + 1)
                                 : text.size();
    const std::string prefix = tab_style == TabStyle::Tab
                                   ? std::string("\t")
                                   : std::string(tab_width, ' ');
    const size_t line_count = last_line - first_line + 1;
    if (line_count > (std::numeric_limits<size_t>::max() - (block_end - block_start)) /
                         prefix.size()) {
        return {ErrorCode::Internal, "selected indentation is too large for this platform"};
    }

    try {
        const std::string original = text.range_text(block_start, block_end - block_start);
        std::string transformed;
        transformed.reserve(original.size() + line_count * prefix.size());
        std::vector<IndentChange> changes;
        changes.reserve(line_count);
        size_t local = 0;
        for (size_t line = first_line; line <= last_line; ++line) {
            changes.push_back({block_start + local, 0, prefix.size()});
            transformed += prefix;
            const size_t newline = original.find('\n', local);
            const size_t end = newline == std::string::npos ? original.size() : newline + 1;
            transformed.append(original, local, end - local);
            local = end;
        }

        const size_t old_anchor = selection.anchor;
        const size_t old_active = selection.active;
        const size_t old_cursor = cursor;
        Error err = replace(block_start, block_end - block_start, transformed);
        if (!err.ok()) {
            return err;
        }
        selection.anchor = map_indent_offset(old_anchor, changes);
        selection.active = map_indent_offset(old_active, changes);
        cursor = map_indent_offset(old_cursor, changes);
        update_preferred_column(*this);
        return ok_error();
    } catch (const std::bad_alloc&) {
        return {ErrorCode::Internal, "not enough memory to indent the selected block"};
    } catch (const std::length_error&) {
        return {ErrorCode::Internal, "selected indentation is too large for this platform"};
    }
}

Error EditorState::outdent() {
    Error writable = mutation_allowed();
    if (!writable.ok()) return writable;
    tab_width = std::max<size_t>(1, std::min(tab_width, kMaxTabWidth));
    const bool selected = selection.has_range();
    const size_t selection_start = selected ? selection.start() : cursor;
    const size_t selection_end = selected ? selection.end() : cursor;
    const size_t first_line = text.line_for_offset(selection_start);
    size_t last_line = text.line_for_offset(selection_end);
    if (selected && selection_end > selection_start &&
        selection_end == text.line_start(last_line) && last_line > first_line) {
        --last_line;
    }
    const size_t block_start = text.line_start(first_line);
    const size_t block_end = last_line + 1 < text.line_count()
                                 ? text.line_start(last_line + 1)
                                 : text.size();

    try {
        const std::string original = text.range_text(block_start, block_end - block_start);
        std::string transformed;
        transformed.reserve(original.size());
        std::vector<IndentChange> changes;
        changes.reserve(last_line - first_line + 1);
        size_t local = 0;
        for (size_t line = first_line; line <= last_line; ++line) {
            const size_t newline = original.find('\n', local);
            const size_t content_end = newline == std::string::npos ? original.size() : newline;
            const IndentRemoval removal =
                leading_whitespace_removal(original, local, content_end, tab_width);
            if (removal.remove > 0) {
                changes.push_back(
                    {block_start + local + removal.keep, removal.remove, 0});
            }
            transformed.append(original, local, removal.keep);
            transformed.append(original,
                               local + removal.keep + removal.remove,
                               content_end - local - removal.keep - removal.remove);
            if (newline != std::string::npos) {
                transformed.push_back('\n');
                local = newline + 1;
            } else {
                local = original.size();
            }
        }
        if (changes.empty()) {
            return ok_error();
        }

        const size_t old_anchor = selection.anchor;
        const size_t old_active = selection.active;
        const size_t old_cursor = cursor;
        Error err = replace(block_start, block_end - block_start, transformed);
        if (!err.ok()) {
            return err;
        }
        cursor = map_indent_offset(old_cursor, changes);
        if (selected) {
            selection.anchor = map_indent_offset(old_anchor, changes);
            selection.active = map_indent_offset(old_active, changes);
        } else {
            selection.clear(cursor);
        }
        update_preferred_column(*this);
        return ok_error();
    } catch (const std::bad_alloc&) {
        return {ErrorCode::Internal, "not enough memory to outdent the selected block"};
    } catch (const std::length_error&) {
        return {ErrorCode::Internal, "selected outdent is too large for this platform"};
    }
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
    cursor = text.offset_for_line_column(line - 1, preferred_column, tab_width);
}

void EditorState::move_down() {
    const size_t line = text.line_for_offset(cursor);
    if (line + 1 >= text.line_count()) {
        return;
    }
    cursor = text.offset_for_line_column(line + 1, preferred_column, tab_width);
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
    const WrappedLocation location = wrapped_location_for_offset(text, cursor, width, tab_width);
    if (location.segment > 0) {
        cursor = offset_for_wrapped_location(
            text, location.line, location.segment - 1, preferred_column, width, tab_width);
        return;
    }
    if (location.line == 0) {
        return;
    }
    const size_t previous_line = location.line - 1;
    const size_t previous_rows = wrapped_row_count(text.line_text(previous_line), width, tab_width);
    cursor = offset_for_wrapped_location(
        text, previous_line, previous_rows - 1, preferred_column, width, tab_width);
}

void EditorState::move_down_visual(const Rect& rect) {
    const size_t width = static_cast<size_t>(std::max(1, rect.width));
    const WrappedLocation location = wrapped_location_for_offset(text, cursor, width, tab_width);
    const size_t current_rows = wrapped_row_count(text.line_text(location.line), width, tab_width);
    if (location.segment + 1 < current_rows) {
        cursor = offset_for_wrapped_location(
            text, location.line, location.segment + 1, preferred_column, width, tab_width);
        return;
    }
    if (location.line + 1 >= text.line_count()) {
        return;
    }
    cursor = offset_for_wrapped_location(
        text, location.line + 1, 0, preferred_column, width, tab_width);
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
    const WrappedLocation location = wrapped_location_for_offset(text, cursor, width, tab_width);
    const std::string line_text_value = text.line_text(location.line);
    const std::vector<WrapSegment> segments = wrap_line_segments(line_text_value, width, tab_width);
    const WrapSegment& segment = segments[std::min(location.segment, segments.size() - 1)];
    cursor = text.line_start(location.line) + segment.start;
    update_preferred_column(*this);
    selection.clear(cursor);
}

void EditorState::move_line_end(const Rect& rect) {
    const size_t width = static_cast<size_t>(std::max(1, rect.width));
    const WrappedLocation location = wrapped_location_for_offset(text, cursor, width, tab_width);
    const std::string line_text_value = text.line_text(location.line);
    const std::vector<WrapSegment> segments = wrap_line_segments(line_text_value, width, tab_width);
    const WrapSegment& segment = segments[std::min(location.segment, segments.size() - 1)];
    cursor = text.line_start(location.line) + segment.end;
    update_preferred_column(*this);
    selection.clear(cursor);
}

Error EditorState::kill_to_line_end(Clipboard& clipboard) {
    Error writable = mutation_allowed();
    if (!writable.ok()) return writable;
    const size_t line = text.line_for_offset(cursor);
    const size_t start = text.line_start(line);
    const size_t length = text.line_length(line);
    const size_t end = start + length;
    if (cursor < end) {
        const std::string killed = text.range_text(cursor, end - cursor);
        clipboard.set(killed);
        EditorSnapshot before = snapshot();
        Error err = text.erase(cursor, end - cursor);
        if (!err.ok()) {
            return err;
        }
        word_index_.apply_edit(before.content, cursor, end - cursor, "");
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
    clipboard.set("\n");
    EditorSnapshot before = snapshot();
    Error err = text.erase(erase_pos, 1);
    if (!err.ok()) {
        return err;
    }
    word_index_.apply_edit(before.content, erase_pos, 1, "");
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
        cursor_row += wrapped_row_count(text.line_text(i), width, tab_width);
    }
    const size_t line_start_offset = text.line_start(line);
    const std::string line_text_value = text.line_text(line);
    cursor_row +=
        cursor_in_wrapped_line(line_text_value,
                               cursor - line_start_offset,
                               width,
                               tab_width)
            .row;

    if (cursor_row < scroll_line) {
        scroll_line = cursor_row;
    } else if (cursor_row >= scroll_line + height) {
        scroll_line = cursor_row - height + 1;
    }
    scroll_column = 0;
}

bool EditorState::scroll_view_rows(const Rect& rect, int rows) {
    const size_t width = static_cast<size_t>(std::max(1, rect.width));
    const size_t height = static_cast<size_t>(std::max(1, rect.height));
    const size_t content_rows = visual_row_count_bounded(width, static_cast<size_t>(-1));
    const size_t max_scroll = content_rows > height ? content_rows - height : 0;
    const size_t before = scroll_line;
    if (rows < 0) {
        const size_t amount = static_cast<size_t>(-(static_cast<long long>(rows)));
        scroll_line = amount > scroll_line ? 0 : scroll_line - amount;
    } else if (rows > 0) {
        const size_t amount = static_cast<size_t>(rows);
        scroll_line = amount > max_scroll - std::min(scroll_line, max_scroll)
                          ? max_scroll
                          : std::min(scroll_line, max_scroll) + amount;
    }
    scroll_line = std::min(scroll_line, max_scroll);
    scroll_column = 0;
    return scroll_line != before;
}

RenderedPanel EditorState::render(const Rect& rect) const {
    return render(rect, cursor, scroll_line, scroll_column);
}

RenderedPanel EditorState::render(const Rect& rect,
                                  size_t view_cursor,
                                  size_t view_scroll_line,
                                  size_t view_scroll_column) const {
    const std::optional<Selection> active_selection =
        selection.has_range() ? std::optional<Selection>(selection) : std::nullopt;
    return render_panel(text,
                        rect,
                        std::min(view_cursor, text.size()),
                        view_scroll_line,
                        view_scroll_column,
                        active_selection,
                        language,
                        highlight_enabled,
                        &highlight_cache_,
                        tab_width);
}

size_t EditorState::visual_row_count_bounded(size_t width, size_t limit) const {
    if (limit == 0) return 0;
    width = std::max<size_t>(1, width);
    // A conservative byte budget keeps this sizing pass independent of total
    // draft size and avoids building the PieceTable line cache. Eight bytes per
    // cell covers ordinary UTF-8 plus combining marks; an unusually dense
    // grapheme prefix simply reports the cap, which is safe for frame sizing.
    const size_t scaled_budget =
        width > static_cast<size_t>(-1) / limit / 8
            ? static_cast<size_t>(-1)
            : width * limit * 8;
    const size_t byte_budget =
        std::min(text.size(), std::max<size_t>(4096, scaled_budget));
    const std::string prefix = text.range_text(0, byte_budget);
    size_t rows = 0;
    size_t line_start = 0;
    while (line_start <= prefix.size() && rows < limit) {
        const size_t newline = prefix.find('\n', line_start);
        const size_t line_end =
            newline == std::string::npos ? prefix.size() : newline;
        rows += detail::wrapped_row_count_bounded(
            prefix.substr(line_start, line_end - line_start),
            width,
            limit - rows,
            tab_width);
        if (newline == std::string::npos) break;
        line_start = newline + 1;
    }
    if (byte_budget < text.size() && rows < limit) return limit;
    return std::min(rows, limit);
}


}  // namespace ainiux::editor
