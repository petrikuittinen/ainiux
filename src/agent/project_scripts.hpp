#pragma once

#include <string>
#include <vector>

#include "common.hpp"

namespace ainiux::agent {

// Reusable project helpers live at scripts/ainiux/NAME (flat portable filename).
// These are ordinary workspace files: indexed, greppable, and git-visible.
inline constexpr const char kProjectScriptsDirectory[] = "scripts/ainiux";

bool portable_project_script_name(const std::string& name);
bool project_script_path(const std::string& path, std::string* name = nullptr);
bool project_scripts_directory(const std::string& path);
// True for the retired .ainiux-pr/scripts (and .ainiux-pr/script typo) location.
bool retired_project_script_path(const std::string& path);
const char* retired_project_script_message();
bool project_script_invocation(const std::vector<std::string>& arguments,
                               std::string* interpreter = nullptr,
                               std::string* relative_path = nullptr);
Error list_project_scripts(const std::string& workspace, std::vector<std::string>& names);
std::string project_script_catalog_text(const std::vector<std::string>& names);

}  // namespace ainiux::agent
