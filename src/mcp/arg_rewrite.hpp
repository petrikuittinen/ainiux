#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "agent/attachment_bag.hpp"
#include "common.hpp"
#include "mcp/registry.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::mcp {

struct ArgRewriteCaps {
    std::size_t max_image_bytes = 20U * 1024U * 1024U;
    std::size_t max_arguments_json_bytes = 12U * 1024U * 1024U;
    // When true, prefer base64 even for stdio (tests / remote-like).
    bool force_base64 = false;
};

struct ArgRewriteResult {
    std::string arguments_json;
    std::string history_arguments_json;  // redacted base64 for transcripts
    std::vector<std::string> notes;
    bool changed = false;
};

// Rewrite MCP tool arguments: resolve image paths from the bag (or load on demand
// under workspace) to absolute paths (stdio) or base64 (HTTP / schema hints).
Error rewrite_mcp_arguments(const ServerConfig& server,
                            const std::string& tool_name,
                            const std::string& input_schema_json,
                            const std::string& arguments_json,
                            agent::AttachmentBag& bag,
                            const ArgRewriteCaps& caps,
                            runtime::CancellationToken cancellation,
                            ArgRewriteResult& out);

// For unit tests: redact long base64-looking strings in JSON text.
std::string redact_base64_in_json_text(const std::string& json_text);

}  // namespace ainiux::mcp
