#pragma once

#include "cli/args.hpp"
#include "common.hpp"

namespace ainiux::server {

Error validate_server_options(const cli::Options& options);
int run_server(const cli::Options& options);

}  // namespace ainiux::server
