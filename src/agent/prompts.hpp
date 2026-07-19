#pragma once

#include <string>

#include "common.hpp"

namespace ainiux::agent {

struct TrustedPrompts {
    std::string master;
    std::string security;

    std::string security_system_prompt() const { return master + "\n" + security; }
};

Error load_trusted_prompts(const std::string& override_directory, TrustedPrompts& prompts);

}  // namespace ainiux::agent
