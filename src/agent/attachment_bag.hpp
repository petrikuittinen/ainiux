#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "common.hpp"
#include "provider/provider.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::agent {

enum class AttachmentKind { Image, Text };

enum class AttachmentSource {
    CliAttach,
    SlashAttach,
    AttachImageTool,
    OnDemandPath,
};

struct AttachmentEntry {
    int id = 0;
    AttachmentKind kind = AttachmentKind::Image;
    AttachmentSource source = AttachmentSource::CliAttach;
    std::string display_name;
    std::string absolute_path;
    std::string mime_type;
    std::string base64_data;  // empty until loaded
    std::size_t byte_size = 0;
    bool vision_queued = false;
};

// Turn-scoped bag: images (and later binary) available to MCP rewrite and optional vision.
class AttachmentBag {
   public:
    void clear();
    bool empty() const { return entries_.empty(); }
    std::size_t size() const { return entries_.size(); }
    const std::vector<AttachmentEntry>& entries() const { return entries_; }

    // Add or refresh an image entry (loads base64 if not already set).
    Error add_image(const std::string& absolute_path,
                    const std::string& display_name,
                    const std::string& mime_type,
                    std::string base64_data,
                    std::size_t byte_size,
                    AttachmentSource source,
                    bool vision_queued = false);

    // Find by absolute path or display name (exact).
    const AttachmentEntry* find_by_path(const std::string& path_or_name) const;
    const AttachmentEntry* find_by_id(int id) const;

    // Ensure base64 is loaded for an existing entry (no-op if already set).
    Error ensure_base64(AttachmentEntry& entry,
                        std::size_t max_bytes,
                        runtime::CancellationToken cancellation = {});

    // Mutable access for ensure_base64 after find by path.
    AttachmentEntry* find_by_path_mut(const std::string& path_or_name);

    std::string summary_line() const;
    int image_count() const;

   private:
    std::vector<AttachmentEntry> entries_;
    int next_id_ = 1;
};

}  // namespace ainiux::agent
