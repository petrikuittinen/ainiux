#include "editor/dired.hpp"

#include "editor/detail/editor_common.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

#if !defined(_WIN32)
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ainiux::editor {
namespace {

namespace fs = std::filesystem;

constexpr std::uint64_t kDefaultHashCap = 2ULL * 1024ULL * 1024ULL;

bool has_glob_chars(const std::string& text) {
    return text.find('*') != std::string::npos || text.find('?') != std::string::npos;
}

// Minimal glob: * and ? only, no ** recursion.
bool glob_match(const std::string& pattern, const std::string& name) {
    size_t pi = 0;
    size_t ni = 0;
    size_t star_pi = std::string::npos;
    size_t star_ni = 0;
    while (ni < name.size()) {
        if (pi < pattern.size() && (pattern[pi] == '?' || pattern[pi] == name[ni])) {
            ++pi;
            ++ni;
            continue;
        }
        if (pi < pattern.size() && pattern[pi] == '*') {
            star_pi = pi++;
            star_ni = ni;
            continue;
        }
        if (star_pi != std::string::npos) {
            pi = star_pi + 1;
            ni = ++star_ni;
            continue;
        }
        return false;
    }
    while (pi < pattern.size() && pattern[pi] == '*') {
        ++pi;
    }
    return pi == pattern.size();
}

std::string format_mtime(std::int64_t sec) {
    if (sec <= 0) {
        return "---- -- -- --:--";
    }
    const std::time_t t = static_cast<std::time_t>(sec);
    std::tm tm_value{};
#if defined(_WIN32)
    if (localtime_s(&tm_value, &t) != 0) {
        return "---- -- -- --:--";
    }
#else
    if (localtime_r(&t, &tm_value) == nullptr) {
        return "---- -- -- --:--";
    }
#endif
    std::ostringstream out;
    out << std::put_time(&tm_value, "%Y-%m-%d %H:%M");
    return out.str();
}

std::string format_size(std::uint64_t size, bool is_directory) {
    if (is_directory) {
        return "     DIR";
    }
    std::ostringstream out;
    out << std::setw(8) << size;
    return out.str();
}

std::int64_t file_mtime_sec(const fs::path& path) {
    std::error_code ec;
    const auto ftime = fs::last_write_time(path, ec);
    if (ec) {
        return 0;
    }
#if defined(__cpp_lib_chrono) && __cpp_lib_chrono >= 201907L
    const auto system_time = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(system_time.time_since_epoch()).count());
#else
    using namespace std::chrono;
    const auto sctp = time_point_cast<system_clock::duration>(
        ftime - fs::file_time_type::clock::now() + system_clock::now());
    return static_cast<std::int64_t>(duration_cast<seconds>(sctp.time_since_epoch()).count());
#endif
}

bool is_directory_entry(const fs::directory_entry& entry) {
    std::error_code ec;
    return entry.is_directory(ec);
}

bool is_symlink_entry(const fs::directory_entry& entry) {
    std::error_code ec;
    return entry.is_symlink(ec);
}

std::uint64_t entry_size(const fs::directory_entry& entry, bool is_directory) {
    if (is_directory) {
        return 0;
    }
    std::error_code ec;
    const auto size = entry.file_size(ec);
    if (ec) {
        return 0;
    }
    return static_cast<std::uint64_t>(size);
}

void fill_unix_identity(DiredEntry& entry, const fs::path& path) {
#if defined(_WIN32)
    (void)entry;
    (void)path;
#else
    struct stat st {};
    if (::lstat(path.c_str(), &st) != 0) {
        return;
    }
    const mode_t mode = st.st_mode;
    char type = '-';
    if (S_ISDIR(mode)) {
        type = 'd';
    } else if (S_ISLNK(mode)) {
        type = 'l';
    } else if (S_ISCHR(mode)) {
        type = 'c';
    } else if (S_ISBLK(mode)) {
        type = 'b';
    } else if (S_ISFIFO(mode)) {
        type = 'p';
    } else if (S_ISSOCK(mode)) {
        type = 's';
    }
    auto bit = [&](mode_t mask, char ch) { return (mode & mask) ? ch : '-'; };
    std::string mode_str;
    mode_str.push_back(type);
    mode_str.push_back(bit(S_IRUSR, 'r'));
    mode_str.push_back(bit(S_IWUSR, 'w'));
    mode_str.push_back(bit(S_IXUSR, 'x'));
    mode_str.push_back(bit(S_IRGRP, 'r'));
    mode_str.push_back(bit(S_IWGRP, 'w'));
    mode_str.push_back(bit(S_IXGRP, 'x'));
    mode_str.push_back(bit(S_IROTH, 'r'));
    mode_str.push_back(bit(S_IWOTH, 'w'));
    mode_str.push_back(bit(S_IXOTH, 'x'));
    // sticky / setuid / setgid presentation like ls
    if (mode & S_ISUID) {
        mode_str[3] = (mode & S_IXUSR) ? 's' : 'S';
    }
    if (mode & S_ISGID) {
        mode_str[6] = (mode & S_IXGRP) ? 's' : 'S';
    }
    if (mode & S_ISVTX) {
        mode_str[9] = (mode & S_IXOTH) ? 't' : 'T';
    }
    entry.mode = std::move(mode_str);

    if (const passwd* pw = ::getpwuid(st.st_uid)) {
        entry.owner = pw->pw_name != nullptr ? pw->pw_name : std::to_string(st.st_uid);
    } else {
        entry.owner = std::to_string(st.st_uid);
    }
    if (const group* gr = ::getgrgid(st.st_gid)) {
        entry.group = gr->gr_name != nullptr ? gr->gr_name : std::to_string(st.st_gid);
    } else {
        entry.group = std::to_string(st.st_gid);
    }
#endif
}

std::string pad_right(std::string text, size_t width) {
    if (text.size() < width) {
        text.append(width - text.size(), ' ');
    }
    return text;
}

void sort_entries(std::vector<DiredEntry>& entries, DiredSortKey key, bool ascending) {
    std::stable_sort(entries.begin(), entries.end(), [&](const DiredEntry& a, const DiredEntry& b) {
        if (a.is_parent != b.is_parent) {
            return a.is_parent;
        }
        if (a.is_directory != b.is_directory) {
            return a.is_directory && !b.is_directory;
        }
        int cmp = 0;
        switch (key) {
            case DiredSortKey::Name:
                cmp = a.name.compare(b.name);
                break;
            case DiredSortKey::Size:
                if (a.size < b.size) {
                    cmp = -1;
                } else if (a.size > b.size) {
                    cmp = 1;
                } else {
                    cmp = a.name.compare(b.name);
                }
                break;
            case DiredSortKey::Date:
                if (a.mtime_sec < b.mtime_sec) {
                    cmp = -1;
                } else if (a.mtime_sec > b.mtime_sec) {
                    cmp = 1;
                } else {
                    cmp = a.name.compare(b.name);
                }
                break;
        }
        return ascending ? (cmp < 0) : (cmp > 0);
    });
}

Error ensure_parent_dirs(const fs::path& path) {
    std::error_code ec;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
        if (ec) {
            return {ErrorCode::FileWrite, "could not create parent directories: " + ec.message()};
        }
    }
    return ok_error();
}

Error copy_path_recursive(const fs::path& from, const fs::path& to, bool overwrite) {
    std::error_code ec;
    const auto options = overwrite ? (fs::copy_options::overwrite_existing | fs::copy_options::recursive)
                                   : (fs::copy_options::skip_existing | fs::copy_options::recursive);
    if (fs::exists(to, ec) && !overwrite) {
        return {ErrorCode::FileWrite, "destination exists (confirm overwrite): " + to.string()};
    }
    fs::copy(from, to, options, ec);
    if (ec) {
        return {ErrorCode::FileWrite, "copy failed: " + ec.message()};
    }
    return ok_error();
}

Error remove_path(const fs::path& path, bool recursive_ok) {
    std::error_code ec;
    if (fs::is_directory(path, ec) && !ec) {
        if (!fs::is_empty(path, ec) && !ec) {
            if (!recursive_ok) {
                return {ErrorCode::FileWrite,
                        "directory is not empty; confirm recursive delete"};
            }
        }
        const auto removed = fs::remove_all(path, ec);
        if (ec || removed == 0) {
            return {ErrorCode::FileWrite, "could not remove directory: " + ec.message()};
        }
        return ok_error();
    }
    if (!fs::remove(path, ec) || ec) {
        return {ErrorCode::FileWrite, "could not remove: " + (ec ? ec.message() : path.string())};
    }
    return ok_error();
}

DiredEntry make_entry_from_path(const fs::path& path, const std::string& display_name, bool parent) {
    DiredEntry entry;
    entry.name = display_name;
    entry.path = path.lexically_normal().string();
    entry.is_parent = parent;
    std::error_code ec;
    entry.is_symlink = fs::is_symlink(path, ec);
    entry.is_directory = parent || fs::is_directory(path, ec);
    if (!entry.is_directory) {
        const auto size = fs::file_size(path, ec);
        if (!ec) {
            entry.size = static_cast<std::uint64_t>(size);
        }
        entry.content_hash = dired_hash_file(entry.path, kDefaultHashCap);
    }
    entry.mtime_sec = file_mtime_sec(path);
    fill_unix_identity(entry, path);
    return entry;
}

Error fill_directory(DiredState& state) {
    state.entries.clear();
    std::error_code ec;
    fs::path dir = fs::absolute(state.directory, ec);
    if (ec) {
        return {ErrorCode::FileRead, "could not resolve directory: " + ec.message()};
    }
    if (!fs::is_directory(dir, ec) || ec) {
        return {ErrorCode::FileRead, "not a directory: " + dir.string()};
    }
    state.directory = dir.lexically_normal().string();

    const fs::path parent = dir.has_parent_path() && dir != dir.root_path() ? dir.parent_path() : dir;
    state.entries.push_back(make_entry_from_path(parent, "..", true));

    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        return {ErrorCode::FileRead, "could not list directory: " + ec.message()};
    }
    for (const fs::directory_entry& dent : it) {
        const std::string name = dent.path().filename().string();
        if (name.empty() || name == "." || name == "..") {
            continue;
        }
        if (!state.glob_pattern.empty() && !glob_match(state.glob_pattern, name)) {
            continue;
        }
        const bool is_dir = is_directory_entry(dent);
        DiredEntry entry;
        entry.name = name;
        entry.path = dent.path().lexically_normal().string();
        entry.is_directory = is_dir;
        entry.is_symlink = is_symlink_entry(dent);
        entry.size = entry_size(dent, is_dir);
        entry.mtime_sec = file_mtime_sec(dent.path());
        if (!is_dir) {
            entry.content_hash = dired_hash_file(entry.path, kDefaultHashCap);
        }
        fill_unix_identity(entry, dent.path());
        state.entries.push_back(std::move(entry));
    }

    sort_entries(state.entries, state.sort_key, state.sort_ascending);
    if (state.selected >= state.entries.size()) {
        state.selected = state.entries.empty() ? 0 : state.entries.size() - 1;
    }
    dired_update_dirty_flags(state);
    return ok_error();
}

Error fill_glob(DiredState& state, const fs::path& base_dir, const std::string& pattern) {
    state.entries.clear();
    std::error_code ec;
    fs::path dir = fs::absolute(base_dir, ec);
    if (ec || !fs::is_directory(dir, ec)) {
        return {ErrorCode::FileRead, "could not list glob directory: " + base_dir.string()};
    }
    state.directory = dir.lexically_normal().string();
    state.glob_pattern = pattern;

    const fs::path parent = dir.has_parent_path() && dir != dir.root_path() ? dir.parent_path() : dir;
    state.entries.push_back(make_entry_from_path(parent, "..", true));

    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        return {ErrorCode::FileRead, "could not list directory: " + ec.message()};
    }
    for (const fs::directory_entry& dent : it) {
        const std::string name = dent.path().filename().string();
        if (name.empty() || name == "." || name == "..") {
            continue;
        }
        if (!glob_match(pattern, name)) {
            continue;
        }
        const bool is_dir = is_directory_entry(dent);
        DiredEntry entry;
        entry.name = name;
        entry.path = dent.path().lexically_normal().string();
        entry.is_directory = is_dir;
        entry.is_symlink = is_symlink_entry(dent);
        entry.size = entry_size(dent, is_dir);
        entry.mtime_sec = file_mtime_sec(dent.path());
        if (!is_dir) {
            entry.content_hash = dired_hash_file(entry.path, kDefaultHashCap);
        }
        fill_unix_identity(entry, dent.path());
        state.entries.push_back(std::move(entry));
    }
    sort_entries(state.entries, state.sort_key, state.sort_ascending);
    state.selected = 0;
    dired_update_dirty_flags(state);
    return ok_error();
}

fs::path resolve_under_directory(const DiredState& state, const std::string& name) {
    fs::path input(name);
    if (input.is_absolute()) {
        return input.lexically_normal();
    }
    return (fs::path(state.directory) / input).lexically_normal();
}

}  // namespace

bool is_dired_f4_sequence(const std::string& sequence) {
    // xterm: ESC OS / ESC [14~ ; linux console often ESC [[D for F4.
    return sequence == "OS" || sequence == "[14~" || sequence == "[[D";
}

std::string dired_sort_label(DiredSortKey key, bool ascending) {
    const char* name = "name";
    switch (key) {
        case DiredSortKey::Name:
            name = "name";
            break;
        case DiredSortKey::Size:
            name = "size";
            break;
        case DiredSortKey::Date:
            name = "date";
            break;
    }
    // ASCII markers for portability: ^ ascending, v descending.
    return std::string(name) + (ascending ? "^" : "v");
}

std::string dired_header_line(const DiredState& state) {
    std::ostringstream out;
    out << "Dired  sort:" << dired_sort_label(state.sort_key, state.sort_ascending);
    if (state.focus == DiredFocus::View) {
        out << "  [VIEW RET=list]";
    } else {
        out << "  [LIST]";
    }
    return out.str();
}

std::string dired_list_text(const DiredState& state) {
    std::ostringstream out;
    // Two fixed help lines (kept short so typical terminals show both fully).
    out << "  RET view  o edit  g refresh  r rename  c copy  d del  n file  m dir\n";
    out << "  t touch  f find  * reviewed  left=parent  right=enter dir  q quit\n";
    for (size_t i = 0; i < state.entries.size(); ++i) {
        const DiredEntry& entry = state.entries[i];
        const bool selected = i == state.selected && state.focus == DiredFocus::List;
        out << (selected ? "> " : "  ");
        out << (entry.dirty && !entry.is_parent ? "*" : " ");
#if !defined(_WIN32)
        if (!entry.mode.empty()) {
            out << entry.mode << ' ' << pad_right(entry.owner, 8) << ' '
                << pad_right(entry.group, 8) << ' ';
        }
#endif
        out << format_size(entry.size, entry.is_directory) << "  "
            << format_mtime(entry.mtime_sec) << "  ";
        std::string name = entry.name;
        if (entry.is_directory && !entry.is_parent) {
            name.push_back('/');
        } else if (entry.is_parent) {
            name = "../";
        }
        if (entry.is_symlink && !entry.is_parent) {
            name.push_back('@');
        }
        out << name << '\n';
    }
    if (state.entries.empty()) {
        out << "  (empty)\n";
    }
    return out.str();
}

std::string dired_status_line(const DiredState& state) {
    std::ostringstream out;
    out << "Dired  " << state.directory;
    if (!state.glob_pattern.empty()) {
        out << "  filter:" << state.glob_pattern;
    }
    out << "  " << dired_sort_label(state.sort_key, state.sort_ascending);
    if (state.focus == DiredFocus::View) {
        out << "  viewing " << (state.view_path.empty() ? state.view.path : state.view_path)
            << " [RO]";
    } else if (const DiredEntry* entry = dired_selected_entry(state)) {
        out << "  " << (entry->is_directory ? "dir " : "file ") << entry->name;
        if (entry->dirty) {
            out << " *changed";
        }
    }
    if (state.sort_pending) {
        out << "  sort: (n)ame (s)ize (d)ate asc / (N)ame (S)ize (D)ate desc";
    }
    return out.str();
}

Error dired_open(DiredState& state, const std::string& path_or_glob) {
    const bool was_active = state.active;
    const auto kept_hashes = state.reviewed_hashes;
    state = DiredState{};
    state.reviewed_hashes = kept_hashes;
    state.active = true;
    state.focus = DiredFocus::List;

    std::string input = trim_ascii_copy(path_or_glob);
    if (input.empty()) {
        input = ".";
    }

    std::error_code ec;
    fs::path path(input);
    if (!path.is_absolute()) {
        path = fs::absolute(path, ec);
        if (ec) {
            state.active = false;
            return {ErrorCode::FileRead, "could not resolve path: " + ec.message()};
        }
    }

    if (has_glob_chars(path.filename().string()) && !fs::exists(path, ec)) {
        const fs::path parent = path.has_parent_path() ? path.parent_path() : fs::current_path();
        Error err = fill_glob(state, parent, path.filename().string());
        if (!err.ok()) {
            state.active = false;
            return err;
        }
    } else if (fs::is_directory(path, ec)) {
        state.directory = path.lexically_normal().string();
        state.glob_pattern.clear();
        Error err = fill_directory(state);
        if (!err.ok()) {
            state.active = false;
            return err;
        }
    } else if (fs::is_regular_file(path, ec)) {
        state.directory = path.parent_path().lexically_normal().string();
        state.glob_pattern.clear();
        Error err = fill_directory(state);
        if (!err.ok()) {
            state.active = false;
            return err;
        }
        for (size_t i = 0; i < state.entries.size(); ++i) {
            if (state.entries[i].path == path.lexically_normal().string()) {
                state.selected = i;
                break;
            }
        }
    } else {
        // Treat as directory path that may not exist yet → error, or parent+glob.
        if (has_glob_chars(input)) {
            fs::path as_path(input);
            fs::path parent = as_path.has_parent_path() ? as_path.parent_path() : fs::path(".");
            Error err = fill_glob(state, parent, as_path.filename().string());
            if (!err.ok()) {
                state.active = false;
                return err;
            }
        } else {
            state.active = false;
            return {ErrorCode::FileRead, "path not found: " + input};
        }
    }

    // First activation in a session establishes the review baseline if empty.
    if (!was_active && state.reviewed_hashes.empty()) {
        dired_capture_baseline(state);
    } else {
        dired_update_dirty_flags(state);
    }
    return ok_error();
}

Error dired_refresh(DiredState& state) {
    if (!state.active) {
        return {ErrorCode::Internal, "dired is not active"};
    }
    dired_close_view(state);
    state.sort_pending = false;
    if (!state.glob_pattern.empty()) {
        return fill_glob(state, state.directory, state.glob_pattern);
    }
    return fill_directory(state);
}

void dired_close(DiredState& state) {
    const auto hashes = state.reviewed_hashes;
    state = DiredState{};
    state.reviewed_hashes = hashes;
}

void dired_set_sort(DiredState& state, DiredSortKey key, bool ascending) {
    state.sort_key = key;
    state.sort_ascending = ascending;
    state.sort_pending = false;
    if (state.entries.empty()) {
        return;
    }
    const std::string selected_path =
        state.selected < state.entries.size() ? state.entries[state.selected].path : std::string();
    sort_entries(state.entries, state.sort_key, state.sort_ascending);
    if (!selected_path.empty()) {
        for (size_t i = 0; i < state.entries.size(); ++i) {
            if (state.entries[i].path == selected_path) {
                state.selected = i;
                break;
            }
        }
    }
}

void dired_move_selection(DiredState& state, MovementKey key, int page_rows) {
    if (state.entries.empty()) {
        state.selected = 0;
        return;
    }
    const size_t last = state.entries.size() - 1;
    const size_t page = static_cast<size_t>(std::max(1, page_rows));
    switch (key) {
        case MovementKey::Up:
            if (state.selected > 0) {
                --state.selected;
            }
            break;
        case MovementKey::Down:
            if (state.selected < last) {
                ++state.selected;
            }
            break;
        case MovementKey::PageUp:
            state.selected = state.selected > page ? state.selected - page : 0;
            break;
        case MovementKey::PageDown:
            state.selected = std::min(last, state.selected + page);
            break;
        case MovementKey::Home:
            state.selected = 0;
            break;
        case MovementKey::End:
            state.selected = last;
            break;
        case MovementKey::Left:
        case MovementKey::Right:
            break;
    }
}

void dired_capture_baseline(DiredState& state) {
    state.reviewed_hashes.clear();
    for (const DiredEntry& entry : state.entries) {
        if (entry.is_parent || entry.is_directory) {
            continue;
        }
        if (!entry.content_hash.empty()) {
            state.reviewed_hashes[entry.path] = entry.content_hash;
        }
    }
    dired_update_dirty_flags(state);
}

void dired_update_dirty_flags(DiredState& state) {
    for (DiredEntry& entry : state.entries) {
        entry.dirty = false;
        if (entry.is_parent || entry.is_directory || entry.content_hash.empty()) {
            continue;
        }
        const auto it = state.reviewed_hashes.find(entry.path);
        if (it == state.reviewed_hashes.end()) {
            // New file since baseline counts as changed once a baseline exists.
            entry.dirty = !state.reviewed_hashes.empty();
        } else {
            entry.dirty = it->second != entry.content_hash;
        }
    }
}

Error dired_activate_selection(DiredState& state, const EditorSettings& settings) {
    DiredEntry* entry = dired_selected_entry(state);
    if (entry == nullptr) {
        return {ErrorCode::FileRead, "no selection"};
    }
    if (entry->is_directory || entry->is_parent) {
        const std::string came_from =
            entry->is_parent ? fs::path(state.directory).filename().string() : std::string();
        state.directory = entry->path;
        state.glob_pattern.clear();
        state.focus = DiredFocus::List;
        Error err = fill_directory(state);
        if (!err.ok()) {
            return err;
        }
        // After leaving a directory via "..", reselect that child in the parent listing.
        if (!came_from.empty() && came_from != "." && came_from != "/") {
            for (size_t i = 0; i < state.entries.size(); ++i) {
                if (state.entries[i].name == came_from) {
                    state.selected = i;
                    break;
                }
            }
        }
        return ok_error();
    }

    LoadedFile loaded;
    Error err = load_file(entry->path, settings, loaded);
    if (!err.ok()) {
        return err;
    }
    state.view = EditorState{};
    state.view.text = std::move(loaded.text);
    state.view.linebreak = loaded.linebreak;
    state.view.set_path(entry->path);
    state.view.read_only = true;
    state.view.dirty = false;
    state.view.highlight_enabled = true;
    state.view.redetect_language();
    state.view.cursor = 0;
    state.view.scroll_line = 0;
    state.view.scroll_column = 0;
    state.view.preferred_column = 0;
    state.view.clear_selection();
    state.view.clear_undo_history();
    state.view_path = entry->path;
    state.focus = DiredFocus::View;
    return ok_error();
}

Error dired_go_parent(DiredState& state) {
    if (state.focus != DiredFocus::List) {
        return {ErrorCode::BadArgs, "leave the file view first (Enter)"};
    }
    std::error_code ec;
    fs::path dir = fs::absolute(state.directory, ec);
    if (ec) {
        return {ErrorCode::FileRead, "could not resolve directory: " + ec.message()};
    }
    dir = dir.lexically_normal();
    if (!dir.has_parent_path() || dir == dir.root_path()) {
        return {ErrorCode::FileRead, "already at filesystem root"};
    }
    const std::string came_from = dir.filename().string();
    state.directory = dir.parent_path().lexically_normal().string();
    state.glob_pattern.clear();
    Error err = fill_directory(state);
    if (!err.ok()) {
        return err;
    }
    if (!came_from.empty() && came_from != "." && came_from != "/") {
        for (size_t i = 0; i < state.entries.size(); ++i) {
            if (state.entries[i].name == came_from) {
                state.selected = i;
                break;
            }
        }
    }
    return ok_error();
}

Error dired_go_deeper(DiredState& state) {
    if (state.focus != DiredFocus::List) {
        return {ErrorCode::BadArgs, "leave the file view first (Enter)"};
    }
    DiredEntry* entry = dired_selected_entry(state);
    if (entry == nullptr) {
        return {ErrorCode::FileRead, "no selection"};
    }
    if (!(entry->is_directory || entry->is_parent)) {
        return {ErrorCode::BadArgs, "not a directory (Enter to view file)"};
    }
    // Enter without opening a file view; settings unused for directories.
    EditorSettings settings;
    return dired_activate_selection(state, settings);
}

void dired_close_view(DiredState& state) {
    state.focus = DiredFocus::List;
    state.view = EditorState{};
    state.view_path.clear();
}

Error dired_rename_selected(DiredState& state, const std::string& new_path, bool overwrite) {
    DiredEntry* entry = dired_selected_entry(state);
    if (entry == nullptr || entry->is_parent) {
        return {ErrorCode::BadArgs, "nothing to rename"};
    }
    const fs::path from(entry->path);
    const fs::path to = resolve_under_directory(state, trim_ascii_copy(new_path));
    if (to.empty()) {
        return {ErrorCode::BadArgs, "empty destination path"};
    }
    std::error_code ec;
    if (fs::exists(to, ec)) {
        if (!overwrite) {
            return {ErrorCode::FileWrite, "destination exists (confirm overwrite): " + to.string()};
        }
        Error remove_err = remove_path(to, true);
        if (!remove_err.ok()) {
            return remove_err;
        }
    }
    Error parent_err = ensure_parent_dirs(to);
    if (!parent_err.ok()) {
        return parent_err;
    }
    fs::rename(from, to, ec);
    if (ec) {
        return {ErrorCode::FileWrite, "rename failed: " + ec.message()};
    }
    auto hash_it = state.reviewed_hashes.find(from.string());
    if (hash_it != state.reviewed_hashes.end()) {
        const std::string hash = hash_it->second;
        state.reviewed_hashes.erase(hash_it);
        state.reviewed_hashes[to.lexically_normal().string()] = hash;
    }
    return dired_refresh(state);
}

Error dired_copy_selected(DiredState& state, const std::string& dest_path, bool overwrite) {
    DiredEntry* entry = dired_selected_entry(state);
    if (entry == nullptr || entry->is_parent) {
        return {ErrorCode::BadArgs, "nothing to copy"};
    }
    const fs::path from(entry->path);
    const fs::path to = resolve_under_directory(state, trim_ascii_copy(dest_path));
    Error parent_err = ensure_parent_dirs(to);
    if (!parent_err.ok()) {
        return parent_err;
    }
    Error err = copy_path_recursive(from, to, overwrite);
    if (!err.ok()) {
        return err;
    }
    return dired_refresh(state);
}

Error dired_delete_selected(DiredState& state, bool recursive_confirmed) {
    DiredEntry* entry = dired_selected_entry(state);
    if (entry == nullptr || entry->is_parent) {
        return {ErrorCode::BadArgs, "nothing to delete"};
    }
    const fs::path path(entry->path);
    std::error_code ec;
    if (entry->is_directory && !fs::is_empty(path, ec) && !ec && !recursive_confirmed) {
        return {ErrorCode::FileWrite, "directory is not empty; confirm recursive delete"};
    }
    Error err = remove_path(path, recursive_confirmed || !entry->is_directory);
    if (!err.ok()) {
        return err;
    }
    state.reviewed_hashes.erase(entry->path);
    return dired_refresh(state);
}

Error dired_touch_selected(DiredState& state) {
    DiredEntry* entry = dired_selected_entry(state);
    if (entry == nullptr || entry->is_parent) {
        return {ErrorCode::BadArgs, "nothing to touch"};
    }
    std::error_code ec;
    fs::last_write_time(entry->path, fs::file_time_type::clock::now(), ec);
    if (ec) {
        return {ErrorCode::FileWrite, "touch failed: " + ec.message()};
    }
    // Touch does not change content hash; refresh listing mtime only.
    return dired_refresh(state);
}

Error dired_create_file(DiredState& state, const std::string& name, std::string& created_path) {
    created_path.clear();
    const std::string trimmed = trim_ascii_copy(name);
    if (trimmed.empty()) {
        return {ErrorCode::BadArgs, "file name required"};
    }
    const fs::path path = resolve_under_directory(state, trimmed);
    std::error_code ec;
    if (fs::exists(path, ec)) {
        return {ErrorCode::FileWrite, "already exists: " + path.string()};
    }
    Error parent_err = ensure_parent_dirs(path);
    if (!parent_err.ok()) {
        return parent_err;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return {ErrorCode::FileWrite, "could not create file: " + path.string()};
    }
    created_path = path.lexically_normal().string();
    Error refresh = dired_refresh(state);
    if (!refresh.ok()) {
        return refresh;
    }
    for (size_t i = 0; i < state.entries.size(); ++i) {
        if (state.entries[i].path == created_path) {
            state.selected = i;
            break;
        }
    }
    return ok_error();
}

Error dired_create_directory(DiredState& state, const std::string& name) {
    const std::string trimmed = trim_ascii_copy(name);
    if (trimmed.empty()) {
        return {ErrorCode::BadArgs, "directory name required"};
    }
    const fs::path path = resolve_under_directory(state, trimmed);
    std::error_code ec;
    if (!fs::create_directories(path, ec) && ec) {
        return {ErrorCode::FileWrite, "mkdir failed: " + ec.message()};
    }
    // create_directories returns false if already exists without error.
    if (fs::exists(path, ec) && !fs::is_directory(path, ec)) {
        return {ErrorCode::FileWrite, "path exists and is not a directory: " + path.string()};
    }
    Error refresh = dired_refresh(state);
    if (!refresh.ok()) {
        return refresh;
    }
    const std::string want = path.lexically_normal().string();
    for (size_t i = 0; i < state.entries.size(); ++i) {
        if (state.entries[i].path == want) {
            state.selected = i;
            break;
        }
    }
    return ok_error();
}

std::string dired_hash_bytes(const std::string& bytes) {
    // FNV-1a 64-bit, hex — same family as the code index content hash.
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char ch : bytes) {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << hash;
    return out.str();
}

std::string dired_hash_file(const std::string& path, std::uint64_t max_bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    const std::uint64_t cap = max_bytes == 0 ? kDefaultHashCap : max_bytes;
    std::string data(static_cast<size_t>(std::min<std::uint64_t>(cap, 8ULL * 1024ULL * 1024ULL)),
                     '\0');
    in.read(data.data(), static_cast<std::streamsize>(data.size()));
    const auto got = static_cast<size_t>(in.gcount());
    data.resize(got);
    // If truncated at the cap, fold full size into the hash so large-file
    // edits past the first window still differ when size changes.
    if (static_cast<std::uint64_t>(got) >= cap) {
        std::error_code ec;
        const auto size = fs::file_size(path, ec);
        if (!ec) {
            const auto size_u = static_cast<std::uint64_t>(size);
            data.append(reinterpret_cast<const char*>(&size_u), sizeof(size_u));
        }
    }
    return dired_hash_bytes(data);
}

const DiredEntry* dired_selected_entry(const DiredState& state) {
    if (state.selected >= state.entries.size()) {
        return nullptr;
    }
    return &state.entries[state.selected];
}

DiredEntry* dired_selected_entry(DiredState& state) {
    if (state.selected >= state.entries.size()) {
        return nullptr;
    }
    return &state.entries[state.selected];
}

}  // namespace ainiux::editor
