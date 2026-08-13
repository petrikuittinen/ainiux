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

// Conservative argv classifier for in-project mkdir/rmdir/rm/mv. Used by Smart
// so those run invocations do not prompt unless rm -r targets a non-empty tree.
// Unknown flags or shapes are not classified (Smart still asks).
struct WorkspaceFsCommandAssessment {
    bool classified = false;
    bool recursive_rm = false;
    std::vector<std::string> path_operands;
    std::string reason;
};

WorkspaceFsCommandAssessment assess_workspace_fs_command(
    const std::vector<std::string>& arguments);

}  // namespace ainiux::agent
