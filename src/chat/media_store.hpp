#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "common.hpp"
#include "provider/provider.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::chat {

struct StoredMedia {
    std::string sha256;
    std::string storage_ref;
    std::string mime_type;
    long long byte_size = 0;
};

std::string media_root_for_database(const std::string& database_path);

Error store_media_bytes(const std::string& database_path,
                        const std::string& bytes,
                        const std::string& mime_type,
                        StoredMedia& stored);

Error store_media_base64(const std::string& database_path,
                         const std::string& base64_data,
                         const std::string& mime_type,
                         StoredMedia& stored);

Error hydrate_message_images(const std::string& database_path,
                             std::vector<provider::Message>& messages,
                             size_t max_image_bytes,
                             runtime::CancellationToken cancellation = runtime::CancellationToken());

Error media_file_available(const std::string& database_path,
                           const std::string& sha256,
                           long long expected_size,
                           bool& available);

Error remove_media_file(const std::string& database_path,
                        const std::string& sha256,
                        bool& removed);

}  // namespace ainiux::chat
