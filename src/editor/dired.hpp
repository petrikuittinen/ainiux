#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "common.hpp"
#include "editor/editor.hpp"
#include "editor/selection.hpp"

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
    std::string last_search;
};

// Physical F4 key escape sequences (not the internal Alt+X sentinel 0xF4).
bool is_dired_f4_sequence(const std::string& sequence);

std::string dired_sort_label(DiredSortKey key, bool ascending);
// Short panel title (single line; full key help lives in dired_list_text).
std::string dired_header_line(const DiredState& state);
// Two leading help lines, then the file rows.
std::string dired_list_text(const DiredState& state);
std::string dired_status_line(const DiredState& state);

// Open path or directory/glob (e.g. "src/", "src/*.cpp", ".").
Error dired_open(DiredState& state, const std::string& path_or_glob);
Error dired_refresh(DiredState& state);
void dired_close(DiredState& state);

void dired_set_sort(DiredState& state, DiredSortKey key, bool ascending);
void dired_move_selection(DiredState& state, MovementKey key, int page_rows);
void dired_capture_baseline(DiredState& state);
void dired_update_dirty_flags(DiredState& state);

// Enter directory / parent, or open read-only view for a file.
Error dired_activate_selection(DiredState& state, const EditorSettings& settings);
// Left arrow: parent directory (reselects the directory we left when possible).
Error dired_go_parent(DiredState& state);
// Right arrow: enter selected directory when possible (no-op for plain files).
Error dired_go_deeper(DiredState& state);
void dired_close_view(DiredState& state);

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
