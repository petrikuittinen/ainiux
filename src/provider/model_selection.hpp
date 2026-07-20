#pragma once

#include <string>

#include "ainiux/model_setting.hpp"
#include "cli/args.hpp"
#include "common.hpp"

namespace ainiux::provider {

struct ModelSelection {
    std::string provider;
    std::string model;
    std::string api = "chat";
    ReasoningSelection reasoning;
};

ModelSelection model_selection_from_options(const cli::Options& options);
void apply_model_selection(cli::Options& options, const ModelSelection& selection);
bool can_restore_model_selection(const cli::Options& configured_options,
                                 const ModelSelection& selection);
// After CLI is reparsed over remembered/configured options, drop a stale model
// (and reset reasoning) when the user switched provider or endpoint without an
// explicit --model / --reasoning. Model ids are provider-scoped and must not
// ride along into discovery for local profiles such as vllm.
void apply_cli_target_change(cli::Options& options,
                             const cli::Options& previous_options,
                             bool positional_target_changed);
std::string serialize_model_selection(const ModelSelection& selection);
Error parse_model_selection(const std::string& text, ModelSelection& selection);

}  // namespace ainiux::provider
