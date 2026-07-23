#include "agent/project_root.hpp"

#include <filesystem>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <queue>
#include <system_error>
#include <unistd.h>

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

Error resolve_new_project_target(const std::string& active_root,
                                 const std::string& requested_path,
                                 NewProjectTarget& target) {
    target = NewProjectTarget{};
    if (active_root.empty())
        return {ErrorCode::BadArgs, "active agent project root is empty"};

    std::string expanded = requested_path;
    if (expanded.empty()) {
        expanded = active_root;
    } else if (expanded == "~" || expanded.rfind("~/", 0) == 0) {
        const char* home = std::getenv("HOME");
        if (home == nullptr || *home == '\0')
            return {ErrorCode::BadArgs,
                    "could not expand ~ for /new because HOME is not set"};
        expanded = expanded == "~"
                       ? std::string(home)
                       : (fs::path(home) / expanded.substr(2)).string();
    } else if (expanded[0] == '~') {
        return {ErrorCode::BadArgs,
                "/new does not expand ~user paths; use an absolute path instead"};
    }

    std::error_code ec;
    fs::path candidate(expanded);
    if (candidate.is_relative()) candidate = fs::path(active_root) / candidate;
    candidate = fs::absolute(candidate, ec).lexically_normal();
    if (ec)
        return {ErrorCode::BadArgs,
                "could not resolve /new target " + expanded + ": " + ec.message()};

    const fs::file_status status = fs::symlink_status(candidate, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        return {ErrorCode::FileRead,
                "could not inspect /new target " + candidate.string() + ": " + ec.message()};
    }
    ec.clear();
    if (fs::exists(status)) {
        if (fs::is_symlink(status))
            return {ErrorCode::BadArgs,
                    "/new target must be a real directory, not a symlink: " +
                        candidate.string()};
        if (!fs::is_directory(status))
            return {ErrorCode::BadArgs,
                    "/new target is not a directory: " + candidate.string()};
        candidate = fs::canonical(candidate, ec);
        if (ec)
            return {ErrorCode::FileRead,
                    "could not access /new target " + candidate.string() + ": " + ec.message()};
        target.root_exists = true;
        if (::access(candidate.c_str(), R_OK | W_OK | X_OK) != 0)
            return {ErrorCode::FileWrite,
                    "cannot read and modify /new target " + candidate.string() + ": " +
                        std::strerror(errno)};
    } else {
        const fs::path parent = candidate.parent_path();
        const fs::file_status parent_status = fs::symlink_status(parent, ec);
        if (ec || !fs::exists(parent_status) || !fs::is_directory(parent_status)) {
            return {ErrorCode::BadArgs,
                    "/new can create only the final path component; parent directory does not "
                    "exist: " +
                        parent.string()};
        }
        fs::path canonical_parent = fs::canonical(parent, ec);
        if (ec)
            return {ErrorCode::FileRead,
                    "could not access /new parent " + parent.string() + ": " + ec.message()};
        if (::access(canonical_parent.c_str(), W_OK | X_OK) != 0)
            return {ErrorCode::FileWrite,
                    "cannot create /new target in parent " + canonical_parent.string() + ": " +
                        std::strerror(errno)};
        candidate = canonical_parent / candidate.filename();
    }

    const fs::path state_dir = candidate / kProjectStateDirName;
    const fs::file_status state_status = fs::symlink_status(state_dir, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        return {ErrorCode::FileRead,
                "could not inspect agent state " + state_dir.string() + ": " + ec.message()};
    }
    if (!ec && fs::exists(state_status)) {
        if (fs::is_symlink(state_status))
            return {ErrorCode::BadArgs,
                    "refusing /new because agent state is a symlink: " +
                        state_dir.string()};
        if (!fs::is_directory(state_status))
            return {ErrorCode::BadArgs,
                    "agent state path exists but is not a directory: " +
                        state_dir.string()};
        target.state_dir_exists = true;
    }

    target.root = generic_string(candidate);
    target.state_dir = generic_string(state_dir);
    return ok_error();
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
