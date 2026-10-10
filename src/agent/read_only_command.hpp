#pragma once

#include <string>
#include <vector>

namespace ainiux::agent {

// A conservative, complete-argv assessment used by restricted command policy and by
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

// Index of the git porcelain subcommand after git(1) global options such as
// `-c name=value`, `-C path`, and pager flags. Returns arguments.size() when
// no subcommand is present. Shared by Smart classification, pager hardening,
// and Guard so `git -c core.pager=cat status` is still `status`.
std::size_t git_subcommand_index(const std::vector<std::string>& arguments);

// True when `git -c NAME=VALUE` can run an alias, hook, editor, credential
// helper, filter, or other external program. Agent git policy hard-denies
// those overrides.
bool git_config_assignment_is_dangerous(const std::string& assignment);

// Smart-mode auto-allow for `node --test` [paths]. Executes project tests, so it
// is not part of RestrictedReadOnly. Broader node file/`-e` forms use
// assess_workspace_interpreter_command.
ReadOnlyCommandAssessment assess_node_test_command(
    const std::vector<std::string>& arguments);

// Smart-mode auto-allow for workspace-scoped interpreters and everyday
// C/C++/Java/C# toolchains: python file / -m / -c, node file / --test / -e,
// pytest, gcc/g++/clang/make/cmake/ninja, javac/java/jar/mvn/gradle, and
// dotnet/csc/msbuild. Not part of RestrictedReadOnly. Unknown python/node
// flags, listeners, inspect/watch/loaders, compiler plugins, install/publish
// targets, and inline payloads with out-of-workspace path literals stay
// unvetted (Smart still asks).
ReadOnlyCommandAssessment assess_workspace_interpreter_command(
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
