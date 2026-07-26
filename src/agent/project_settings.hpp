#pragma once

#include <string>

#include "cli/args.hpp"
#include "common.hpp"

namespace ainiux::agent {

// Restore an existing project's provider and request settings into startup
// defaults. This does not create .ainiux-pr for a new project.
Error restore_project_settings(const std::string& workspace,
                               cli::Options& options,
                               bool& restored);

}  // namespace ainiux::agent
