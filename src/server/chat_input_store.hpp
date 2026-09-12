#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common.hpp"

namespace ainiux::server {

enum class ChatInputKind {
    Image,
    Text,
};

struct StoredChatInput {
    std::string id;
    ChatInputKind kind = ChatInputKind::Text;
    std::string mime_type;
    std::string display_name;
    bool converted = false;
    std::vector<std::string> warnings;
    std::shared_ptr<const std::string> bytes;
    std::chrono::system_clock::time_point expires_at;
};

class ChatInputStore {
   public:
    explicit ChatInputStore(std::size_t byte_capacity,
                            std::chrono::seconds lifetime = std::chrono::hours(1));
    ChatInputStore(const ChatInputStore&) = delete;
    ChatInputStore& operator=(const ChatInputStore&) = delete;

    Error add(std::string mime_type,
              std::string filename,
              std::string bytes,
              StoredChatInput& output);
    Error resolve(const std::vector<std::string>& ids, std::vector<StoredChatInput>& output);
    bool erase(const std::string& id);

   private:
    struct Budget;
    struct Entry {
        ChatInputKind kind = ChatInputKind::Text;
        std::string mime_type;
        std::string display_name;
        bool converted = false;
        std::vector<std::string> warnings;
        std::shared_ptr<const std::string> bytes;
        std::chrono::system_clock::time_point expires_at;
    };

    void erase_expired_locked(std::chrono::system_clock::time_point now);

    std::shared_ptr<Budget> budget_;
    std::chrono::seconds lifetime_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
};

}  // namespace ainiux::server
