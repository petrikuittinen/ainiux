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

// Backward-compatible alias used by older call sites/tests.
inline bool has_ainiux_dir(const std::string& path) { return has_project_state_dir(path); }

}  // namespace ainiux::agent
