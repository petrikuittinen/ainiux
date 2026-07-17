#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "common.hpp"
#include "provider/provider.hpp"

namespace ainiux::context {

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
long long estimated_text_tokens(const std::vector<provider::Message>& messages);
long long estimated_usage_tokens(const std::vector<provider::Message>& messages,
                                 const provider::ChatResult& result);
std::string format_context_usage(long long used_tokens, long long window_tokens);
PreparedMessages prepare(const std::vector<provider::Message>& messages,
                         const std::string& policy,
                         size_t max_bytes);

}  // namespace ainiux::context
