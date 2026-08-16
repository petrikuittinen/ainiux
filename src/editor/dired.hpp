#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "common.hpp"
#include "editor/editor.hpp"
#include "editor/selection.hpp"
#include "tui/tui.hpp"

namespace ainiux::editor {

enum class DiredSortKey {
    Name,
    Size,
    Date,
};

enum class DiredFocus {
    List,
    View,
};

struct DiredEntry {
    std::string name;
    std::string path;
    bool is_directory = false;
    bool is_parent = false;
    bool is_symlink = false;
    bool is_hidden = false;
    bool is_read_only = false;
    bool is_reparse_point = false;
    bool is_executable = false;
    std::uint64_t size = 0;
    std::int64_t mtime_sec = 0;
    // POSIX ls-style fields; empty on Windows (not shown in the listing).
    std::string mode;   // e.g. "-rw-r--r--" or "drwxr-xr-x"
    std::string owner;
    std::string group;
    std::string content_hash;
    bool dirty = false;
};

struct DiredState {
    bool active = false;
    DiredFocus focus = DiredFocus::List;
    bool sort_pending = false;
    std::string directory;
    std::string glob_pattern;
    DiredSortKey sort_key = DiredSortKey::Name;
    bool sort_ascending = true;
    std::vector<DiredEntry> entries;
    size_t selected = 0;
    int list_scroll = 0;
    // path -> content hash at last "mark reviewed" / open baseline.
    std::map<std::string, std::string> reviewed_hashes;
    EditorState view;
    std::string view_path;
    // Content hash of the currently loaded RO view (for live agent-write sync).
    std::string view_content_hash;
    std::string last_search;
    // Per source-line marks for dired read-only history diff (empty = no overlay).
    std::vector<bool> view_changed_lines;
    bool view_has_history_baseline = false;
};

// Physical F4 key escape sequences (not the internal Alt+X sentinel 0xF4).
bool is_dired_f4_sequence(const std::string& sequence);

// Idle minibuffer hint while a read-only file view is focused.
inline constexpr const char kDiredViewHelp[] =
    "space/PageDown down, PageUp/b back, n next dirty block, p previous dirty block, q quit";

std::string dired_sort_label(DiredSortKey key, bool ascending);
// Short panel title (single line; full key help lives in dired_list_text).
std::string dired_header_line(const DiredState& state);
// Two leading help lines, then the file rows (plain text; tests / debugging).
std::string dired_list_text(const DiredState& state);
// Styled body rows for the list panel (help + entries). Reuses existing theme roles.
std::vector<tui::StyledLine> dired_list_body_lines(const DiredState& state);
std::string dired_status_line(const DiredState& state);

// Listing helpers (also used for coloring).
bool dired_entry_is_hidden(const DiredEntry& entry);
bool dired_entry_is_executable(const DiredEntry& entry);
// Name color role: dirs / hidden dirs / files / executables / dirty files.
tui::StyleRole dired_entry_name_role(const DiredEntry& entry);

// Open path or directory/glob (e.g. "src/", "src/*.cpp", ".").
Error dired_open(DiredState& state, const std::string& path_or_glob);
Error dired_refresh(DiredState& state);
void dired_close(DiredState& state);

void dired_set_sort(DiredState& state, DiredSortKey key, bool ascending);
void dired_move_selection(DiredState& state, MovementKey key, int page_rows);
// Session / bulk: store current hashes for all listed files as reviewed (all clean).
void dired_capture_baseline(DiredState& state);
void dired_update_dirty_flags(DiredState& state);
// Toggle the selected regular file: dirty → reviewed, reviewed → dirty (per-file pass).
Error dired_toggle_pass_selected(DiredState& state);

// Enter directory / parent, or open read-only view for a file.
Error dired_activate_selection(DiredState& state, const EditorSettings& settings);
// Left arrow: parent directory (reselects the directory we left when possible).
Error dired_go_parent(DiredState& state);
// Right arrow: enter selected directory when possible (no-op for plain files).
Error dired_go_deeper(DiredState& state);
void dired_close_view(DiredState& state);
// Soft refresh while staying in dired: re-hash listed files and dirty flags.
// If the RO view is open and the file changed on disk (e.g. agent edit_file),
// reloads content and remakes per-line history marks. Returns true when the
// listing or view content was updated.
bool dired_sync_live(DiredState& state, const EditorSettings& settings);

// RO view only: jump to next/previous changed-line block (wraps). No-op and
// returns false when there is no history-diff baseline or no changed lines.
// A block is a maximal contiguous run of true view_changed_lines entries.
// next: first block start strictly after the cursor line (wrap to first).
// prev: if mid-block, start of the current block; else previous block start
// (wrap to last).
bool dired_goto_next_changed_block(DiredState& state);
bool dired_goto_prev_changed_block(DiredState& state);

// Filesystem operations on the selected entry (or prompts supply names).
Error dired_rename_selected(DiredState& state, const std::string& new_path, bool overwrite);
Error dired_copy_selected(DiredState& state, const std::string& dest_path, bool overwrite);
Error dired_delete_selected(DiredState& state, bool recursive_confirmed);
Error dired_touch_selected(DiredState& state);
Error dired_create_file(DiredState& state, const std::string& name, std::string& created_path);
Error dired_create_directory(DiredState& state, const std::string& name);

// Hash helpers (content-based; independent of mtime).
std::string dired_hash_bytes(const std::string& bytes);
std::string dired_hash_file(const std::string& path, std::uint64_t max_bytes = 2ULL * 1024ULL * 1024ULL);

const DiredEntry* dired_selected_entry(const DiredState& state);
DiredEntry* dired_selected_entry(DiredState& state);

}  // namespace ainiux::editor
