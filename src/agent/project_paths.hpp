#pragma once

#include <algorithm>
#include <string>

namespace ainiux::agent {

// Project-local state directory (index, agent.sqlite, history, logs).
// Deliberately distinct from the user profile dir ~/.ainiux/ (chat DB, media).
inline constexpr const char kProjectStateDirName[] = ".ainiux-pr";

inline std::string normalized_protected_name(std::string name) {
#if defined(_WIN32)
    std::transform(name.begin(), name.end(), name.begin(), [](char value) {
        return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
    });
#endif
    return name;
}

inline bool is_project_state_dir_name(const std::string& name) {
    return normalized_protected_name(name) == kProjectStateDirName;
}

// True if name is a path component that agent tools must not mutate/escape into.
// Includes the project state dir and the user-profile name if it appears in a tree.
inline bool is_protected_state_dir_name(const std::string& name) {
    const std::string compared = normalized_protected_name(name);
    return compared == kProjectStateDirName || compared == ".ainiux" || compared == ".git" ||
           compared == ".hg" || compared == ".svn";
}

}  // namespace ainiux::agent
