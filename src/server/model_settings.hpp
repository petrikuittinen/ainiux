#pragma once

#include "cli/args.hpp"
#include "json/json.hpp"

namespace ainiux::server {

// Public values use the same textual representation and identifiers as the
// native settings widget. Empty optional values clear an override.
std::string public_model_settings(const cli::Options& options);
std::string public_model_fields(const cli::Options& options);
std::string model_settings_revision(const cli::Options& options);
std::string public_model_configuration(const cli::Options& options);
Error apply_public_model_settings(const json::Value& values, cli::Options& options);
Error apply_public_model_target(const json::Value& root, cli::Options& options);

}  // namespace ainiux::server
