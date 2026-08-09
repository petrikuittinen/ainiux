#pragma once

#include <string>

namespace ainiux::agent {

// Stable history backup filename for one workspace-relative path (generic '/').
// Must match ReadToolRegistry::save_history_copy naming.
std::string history_backup_filename(const std::string& relative_path_generic);

// Absolute path: <project_root>/.ainiux-pr/history/<filename>.
std::string history_backup_path(const std::string& project_root,
                                const std::string& relative_path_generic);

// Walk ancestors of file_or_dir looking for a directory that contains .ainiux-pr.
// Sets project_root_out to that directory (not the state dir). Returns false if none.
bool find_enclosing_project_root(const std::string& file_or_dir, std::string& project_root_out);

// If absolute_file is under project_root, set relative_generic_out with generic '/' separators.
bool project_relative_path(const std::string& project_root,
                           const std::string& absolute_file,
                           std::string& relative_generic_out);

}  // namespace ainiux::agent
