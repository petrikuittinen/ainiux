#pragma once

#include <mutex>
#include <string>

#include "chat/sqlite_store.hpp"
#include "common.hpp"
#include "cli/args.hpp"

namespace ainiux::server {

// Revision-safe protocol boundary over the personal chat store. The service
// owns no provider credentials and never serializes database or managed-media
// paths.
class ChatService {
   public:
    explicit ChatService(std::string database_path = {}, cli::Options defaults = {});

    ChatService(const ChatService&) = delete;
    ChatService& operator=(const ChatService&) = delete;

    Error list(std::string& body);
    Error load(long long thread_id, std::string& body);
    Error create(const std::string& request_body, std::string& body);
    Error settings(long long thread_id, const std::string& request_body,
                   std::string& body, long long& current_revision);
    Error append(long long thread_id,
                 const std::string& request_body,
                 std::string& body,
                 long long& current_revision);
    Error rewind_last_answer(long long thread_id,
                             const std::string& request_body,
                             std::string& body,
                             long long& current_revision);
    Error abandon(long long thread_id,
                  const std::string& request_body,
                  std::string& body,
                  long long& current_revision);
    Error cleanup_empty(const std::string& request_body, std::string& body);
    Error remove(long long thread_id,
                 const std::string& request_body,
                 std::string& body,
                 long long& current_revision);
    Error edit_message(long long thread_id,
                       const std::string& request_body,
                       std::string& body,
                       long long& current_revision);
    Error delete_message(long long thread_id,
                         const std::string& request_body,
                         std::string& body,
                         long long& current_revision);

   private:
    Error ensure_open();

    std::mutex mutex_;
    std::string database_path_;
    chat::SqliteStore store_;
    cli::Options defaults_;
};

}  // namespace ainiux::server
