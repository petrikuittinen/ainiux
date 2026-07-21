#include "agent/project_root.hpp"

#include <filesystem>
#include <queue>
#include <system_error>

namespace ainiux::agent {
namespace {
namespace fs = std::filesystem;

std::string generic_string(const fs::path& path) {
    return path.lexically_normal().generic_string();
}

}  // namespace

bool has_project_state_dir(const std::string& path) {
    std::error_code ec;
    const fs::path marker = fs::path(path) / kProjectStateDirName;
    return fs::is_directory(marker, ec) && !ec;
}

Error resolve_agent_project_root(const std::string& workspace, std::string& absolute_root) {
    absolute_root.clear();
    std::error_code ec;
    const fs::path input = workspace.empty() ? fs::path(".") : fs::path(workspace);
    const fs::path cwd = fs::absolute(input, ec);
    if (ec) {
        return {ErrorCode::BadArgs, "could not resolve agent workspace path: " + ec.message()};
    }
    const fs::path root = fs::weakly_canonical(cwd, ec);
    const fs::path candidate = ec ? cwd.lexically_normal() : root;

    // Parent owns a project (.ainiux-pr only; ~/.ainiux is never a project marker).
    fs::path walk = candidate.parent_path();
    while (!walk.empty() && walk != walk.root_path()) {
        if (has_project_state_dir(walk.string())) {
            return {ErrorCode::BadArgs,
                    "agent project is ambiguous: parent project exists at " + generic_string(walk) +
                        " (cwd " + generic_string(candidate) +
                        "). Run ainiux from that project root, or remove the nested/parent " +
                        std::string(kProjectStateDirName) + "."};
        }
        const fs::path parent = walk.parent_path();
        if (parent == walk) break;
        walk = parent;
    }

    // Nested child project under cwd → refuse.
    {
        std::queue<std::pair<fs::path, int>> q;
        q.push({candidate, 0});
        int visited = 0;
        while (!q.empty() && visited < 2000) {
            const auto [dir, depth] = q.front();
            q.pop();
            if (depth >= 6) continue;
            std::error_code iter_ec;
            if (!fs::is_directory(dir, iter_ec) || iter_ec) continue;
            for (fs::directory_iterator it(dir, iter_ec), end; !iter_ec && it != end;
                 it.increment(iter_ec)) {
                if (iter_ec) break;
                const fs::path entry = it->path();
                const std::string name = entry.filename().string();
                if (name == "." || name == ".." || name == ".git") continue;
                std::error_code st_ec;
                if (!fs::is_directory(entry, st_ec) || st_ec) continue;
                if (name == kProjectStateDirName && depth > 0) {
                    return {ErrorCode::BadArgs,
                            "agent project is ambiguous: nested project at " +
                                generic_string(entry.parent_path()) + " under " +
                                generic_string(candidate) +
                                ". Enter that project root, or remove the nested " +
                                std::string(kProjectStateDirName) + "."};
                }
                if (name == kProjectStateDirName) continue;  // cwd's own marker is fine
                q.push({entry, depth + 1});
                ++visited;
            }
        }
    }

    absolute_root = generic_string(candidate);
    return ok_error();
}

}  // namespace ainiux::agent
