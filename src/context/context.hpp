#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "common.hpp"
#include "provider/provider.hpp"

namespace pkchat::context {

struct CompactionEvent {
    std::string timestamp;
    std::string policy;
    size_t messages_compacted = 0;
    size_t original_bytes = 0;
    size_t request_bytes = 0;
    std::string notice;
};

struct PreparedMessages {
    std::vector<provider::Message> messages;
    CompactionEvent event;
    bool compacted = false;
    Error error;
};

size_t estimated_text_bytes(const std::vector<provider::Message>& messages);
PreparedMessages prepare(const std::vector<provider::Message>& messages,
                         const std::string& policy,
                         size_t max_bytes);

}  // namespace pkchat::context
