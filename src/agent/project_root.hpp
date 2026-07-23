#pragma once

#include <string>

#include "agent/project_paths.hpp"
#include "common.hpp"

namespace ainiux::agent {

// Resolve the agent project root for workspace (usually "." / cwd).
// Agent owns exactly one project tree: this directory's .ainiux-pr/
// (never ~/.ainiux/, which is the user chat/media profile).
// Errors if a parent or nested child .ainiux-pr would make ownership ambiguous.
Error resolve_agent_project_root(const std::string& workspace, std::string& absolute_root);

// True if path/.ainiux-pr exists as a directory (project state marker).
bool has_project_state_dir(const std::string& path);

struct NewProjectTarget {
    std::string root;
    std::string state_dir;
    bool root_exists = false;
    bool state_dir_exists = false;
};

// Resolve /new's optional path from the active project root. Expands only "~"
// and "~/"; relative paths are rooted at active_root. The target may already
// be a directory, or exactly its final component may be missing.
Error resolve_new_project_target(const std::string& active_root,
                                 const std::string& requested_path,
                                 NewProjectTarget& target);

// Backward-compatible alias used by older call sites/tests.
inline bool has_ainiux_dir(const std::string& path) { return has_project_state_dir(path); }

}  // namespace ainiux::agent
