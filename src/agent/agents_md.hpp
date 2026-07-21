#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "common.hpp"

namespace ainiux::agent {

// Project AGENTS.md is untrusted workspace data. It must never become system
// prompt text or override safety rules / the user's direct request.
struct AgentsMdDocument {
    std::string path;  // workspace-relative
    std::string content;
    std::string content_hash;
    bool truncated = false;
};

struct AgentsMdBundle {
    std::vector<AgentsMdDocument> documents;
    // Ready-to-send user-message body (empty when nothing was loaded).
    std::string injection_text;
    bool truncated = false;
    std::size_t total_bytes = 0;
};

// Default cap from the v1.0 plan (agents_md.max_bytes_total).
inline constexpr std::size_t kDefaultAgentsMdMaxBytes = 20000;

// Load workspace-root AGENTS.md when present. Missing file is success with empty
// bundle. Non-UTF-8 / binary files are refused with a clear error so the agent
// does not silently swallow corrupt project rules.
Error load_root_agents_md(const std::string& workspace,
                          std::size_t max_bytes,
                          AgentsMdBundle& bundle);

// Future: also load AGENTS.md files between workspace root and a target path.
// For now this is an alias of load_root_agents_md (root only).
Error load_agents_md_for_path(const std::string& workspace,
                              const std::string& relative_path,
                              std::size_t max_bytes,
                              AgentsMdBundle& bundle);

}  // namespace ainiux::agent
