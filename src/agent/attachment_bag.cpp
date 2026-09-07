#include "agent/attachment_bag.hpp"

#include "input/input.hpp"

namespace ainiux::agent {

void AttachmentBag::clear() {
    entries_.clear();
    next_id_ = 1;
}

Error AttachmentBag::add_image(const std::string& absolute_path,
                               const std::string& display_name,
                               const std::string& mime_type,
                               std::string base64_data,
                               std::size_t byte_size,
                               AttachmentSource source,
                               bool vision_queued) {
    if (absolute_path.empty() && display_name.empty())
        return {ErrorCode::BadArgs, "attachment requires a path or display name"};
    // Update existing same absolute path.
    for (AttachmentEntry& entry : entries_) {
        if (!absolute_path.empty() && entry.absolute_path == absolute_path) {
            if (!base64_data.empty()) {
                entry.base64_data = std::move(base64_data);
                entry.byte_size = byte_size;
            }
            if (!mime_type.empty()) entry.mime_type = mime_type;
            if (!display_name.empty()) entry.display_name = display_name;
            entry.vision_queued = entry.vision_queued || vision_queued;
            entry.source = source;
            return ok_error();
        }
    }
    AttachmentEntry entry;
    entry.id = next_id_++;
    entry.kind = AttachmentKind::Image;
    entry.source = source;
    entry.display_name = display_name.empty() ? absolute_path : display_name;
    entry.absolute_path = absolute_path;
    entry.mime_type = mime_type;
    entry.base64_data = std::move(base64_data);
    entry.byte_size = byte_size;
    entry.vision_queued = vision_queued;
    entries_.push_back(std::move(entry));
    return ok_error();
}

const AttachmentEntry* AttachmentBag::find_by_path(const std::string& path_or_name) const {
    if (path_or_name.empty()) return nullptr;
    for (const AttachmentEntry& entry : entries_) {
        if (entry.absolute_path == path_or_name || entry.display_name == path_or_name)
            return &entry;
    }
    return nullptr;
}

AttachmentEntry* AttachmentBag::find_by_path_mut(const std::string& path_or_name) {
    if (path_or_name.empty()) return nullptr;
    for (AttachmentEntry& entry : entries_) {
        if (entry.absolute_path == path_or_name || entry.display_name == path_or_name)
            return &entry;
    }
    return nullptr;
}

Error AttachmentBag::ensure_base64(AttachmentEntry& entry,
                                   std::size_t max_bytes,
                                   runtime::CancellationToken cancellation) {
    if (!entry.base64_data.empty()) return ok_error();
    if (entry.absolute_path.empty())
        return {ErrorCode::FileRead, "attachment has no path to load"};
    input::FileType type;
    Error err = input::classify_file_type(entry.absolute_path, type);
    if (!err.ok()) return err;
    if (type.kind != input::Kind::Image)
        return {ErrorCode::BadArgs, "attachment is not a supported image type"};
    input::ImageData loaded;
    err = input::load_image_file(entry.absolute_path, type, max_bytes, loaded, cancellation);
    if (!err.ok()) return err;
    entry.mime_type = loaded.mime_type;
    entry.base64_data = std::move(loaded.base64_data);
    entry.byte_size = loaded.byte_size;
    return ok_error();
}

}  // namespace ainiux::agent
