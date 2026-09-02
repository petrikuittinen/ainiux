#pragma once

#include "cli/args.hpp"
#include "common.hpp"

namespace ainiux::server {

Error validate_server_options(const cli::Options& options);
Error managed_server_secret_path(std::string& path);
Error load_or_create_managed_server_secret(const std::string& path,
                                           std::string& secret,
                                           bool& created);
int run_server(const cli::Options& options);

}  // namespace ainiux::server
