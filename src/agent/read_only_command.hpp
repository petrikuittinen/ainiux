#pragma once

#include <string>
#include <vector>

namespace ainiux::agent {

// A conservative, complete-argv assessment used by Plan command policy and by
// Smart-mode approval. `path_operands` contains every filename/directory input
// recognized by the accepted invocation, including auxiliary input files.
// Unknown commands, options, or invocation shapes are not vetted.
struct ReadOnlyCommandAssessment {
    bool vetted = false;
    std::vector<std::string> path_operands;
    std::string reason;
};

ReadOnlyCommandAssessment assess_read_only_command(
    const std::vector<std::string>& arguments);

}  // namespace ainiux::agent
