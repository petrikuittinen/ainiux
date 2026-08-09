#include "agent/history_backup.hpp"

#include "agent/index/index.hpp"
#include "agent/project_paths.hpp"
#include "agent/project_root.hpp"

#include <filesystem>
#include <system_error>

namespace ainiux::agent {
namespace {

namespace fs = std::filesystem;

}  // namespace

std::string history_backup_filename(const std::string& relative_path_generic) {
    const std::string generic = fs::u8path(relative_path_generic).generic_u8string();
    const std::string digest = index::content_hash(generic);
    std::string short_hash = digest.size() > 16 ? digest.substr(0, 16) : digest;
    std::string safe_tail = generic;
    for (char& ch : safe_tail) {
        if (ch == '/' || ch == '\\' || ch == ':' || ch == ' ') {
            ch = '_';
        }
    }
    if (safe_tail.size() > 48) {
        safe_tail = safe_tail.substr(safe_tail.size() - 48);
    }
    return short_hash + "-" + safe_tail + ".bak";
}

std::string history_backup_path(const std::string& project_root,
                                const std::string& relative_path_generic) {
    const fs::path history_file = fs::u8path(project_root) / kProjectStateDirName / "history" /
                                  history_backup_filename(relative_path_generic);
    return history_file.u8string();
}

bool find_enclosing_project_root(const std::string& file_or_dir, std::string& project_root_out) {
    project_root_out.clear();
    if (file_or_dir.empty()) {
        return false;
    }
    std::error_code ec;
    fs::path walk = fs::absolute(fs::u8path(file_or_dir), ec);
    if (ec) {
        return false;
    }
    walk = walk.lexically_normal();
    const fs::file_status status = fs::status(walk, ec);
    if (!ec && fs::is_regular_file(status)) {
        walk = walk.parent_path();
    }
    while (!walk.empty()) {
        if (has_project_state_dir(walk.u8string())) {
            project_root_out = walk.lexically_normal().u8string();
            return true;
        }
        if (!walk.has_parent_path() || walk == walk.root_path()) {
            break;
        }
        const fs::path parent = walk.parent_path();
        if (parent == walk) {
            break;
        }
        walk = parent;
    }
    return false;
}

bool project_relative_path(const std::string& project_root,
                           const std::string& absolute_file,
                           std::string& relative_generic_out) {
    relative_generic_out.clear();
    if (project_root.empty() || absolute_file.empty()) {
        return false;
    }
    std::error_code ec;
    fs::path root = fs::absolute(fs::u8path(project_root), ec);
    if (ec) {
        return false;
    }
    root = root.lexically_normal();
    fs::path file = fs::absolute(fs::u8path(absolute_file), ec);
    if (ec) {
        return false;
    }
    file = file.lexically_normal();

    // Prefer weakly_canonical when the paths exist so symlinks resolve consistently.
    {
        std::error_code canon_ec;
        const fs::path canon_root = fs::weakly_canonical(root, canon_ec);
        if (!canon_ec) {
            root = canon_root;
        }
        canon_ec.clear();
        const fs::path canon_file = fs::weakly_canonical(file, canon_ec);
        if (!canon_ec) {
            file = canon_file;
        }
    }

    const fs::path relative = file.lexically_relative(root);
    if (relative.empty()) {
        return false;
    }
    const std::string generic = relative.generic_u8string();
    if (generic == "." || generic.empty()) {
        return false;
    }
    if (generic == ".." || generic.rfind("../", 0) == 0 || generic.find("/../") != std::string::npos) {
        return false;
    }
    relative_generic_out = generic;
    return true;
}

}  // namespace ainiux::agent
