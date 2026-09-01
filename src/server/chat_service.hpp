#pragma once

#include <mutex>
#include <string>

#include "chat/sqlite_store.hpp"
#include "common.hpp"

namespace ainiux::server {

// Revision-safe protocol boundary over the personal chat store. The service
// owns no provider credentials and never serializes database or managed-media
// paths.
class ChatService {
   public:
    explicit ChatService(std::string database_path = {});

    ChatService(const ChatService&) = delete;
    ChatService& operator=(const ChatService&) = delete;

    Error list(std::string& body);
    Error load(long long thread_id, std::string& body);
    Error create(const std::string& request_body, std::string& body);
    Error append(long long thread_id,
                 const std::string& request_body,
                 std::string& body,
                 long long& current_revision);

   private:
    Error ensure_open();

    std::mutex mutex_;
    std::string database_path_;
    chat::SqliteStore store_;
};

}  // namespace ainiux::server
