#pragma once

#include <string>

namespace ainiux::agent {

// Project-local state directory (index, agent.sqlite, history, logs).
// Deliberately distinct from the user profile dir ~/.ainiux/ (chat DB, media).
inline constexpr const char kProjectStateDirName[] = ".ainiux-pr";

// True if name is a path component that agent tools must not mutate/escape into.
// Includes the project state dir and the user-profile name if it appears in a tree.
inline bool is_protected_state_dir_name(const std::string& name) {
    return name == kProjectStateDirName || name == ".ainiux" || name == ".git" ||
           name == ".hg" || name == ".svn";
}

}  // namespace ainiux::agent
