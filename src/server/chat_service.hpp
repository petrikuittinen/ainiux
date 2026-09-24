#pragma once

#include <mutex>
#include <string>

#include "chat/sqlite_store.hpp"
#include "chat/transcript.hpp"
#include "common.hpp"
#include "cli/args.hpp"
#include "provider/provider.hpp"
#include "server/chat_input_store.hpp"

namespace ainiux::server {

// Revision-safe protocol boundary over the personal chat store. The service
// owns no provider credentials and never serializes database or managed-media
// paths.
class ChatService {
   public:
    explicit ChatService(std::string database_path = {}, cli::Options defaults = {});
    void set_chat_inputs(ChatInputStore* store) { chat_inputs_ = store; }

    ChatService(const ChatService&) = delete;
    ChatService& operator=(const ChatService&) = delete;

    // `query` empty lists the newest threads. A non-empty query searches thread
    // names and user/assistant message text. See SqliteStore::search_threads.
    Error list(std::string& body, const std::string& query = {});
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
    Error export_pdf(long long thread_id,
                     const std::string& request_body,
                     std::string& pdf,
                     std::string& filename,
                     long long& current_revision);
    Error export_docx(long long thread_id,
                      const std::string& request_body,
                      std::string& docx,
                      std::string& filename,
                      long long& current_revision);
    Error export_markdown(long long thread_id,
                          const std::string& request_body,
                          std::string& markdown,
                          std::string& filename,
                          long long& current_revision);
    Error export_json(long long thread_id,
                      const std::string& request_body,
                      std::string& json,
                      std::string& filename,
                      long long& current_revision);
    Error import_json(const std::string& request_body, std::string& body);
    Error export_table_xlsx(const std::string& request_body,
                            std::string& xlsx,
                            std::string& filename);
    Error load_job_messages(long long thread_id, std::vector<provider::Message>& messages);

   private:
    Error ensure_open();
    Error export_rendered(long long thread_id,
                          const std::string& request_body,
                          chat::TranscriptFormat format,
                          std::string& bytes,
                          std::string& filename,
                          long long& current_revision);

    std::mutex mutex_;
    std::string database_path_;
    chat::SqliteStore store_;
    cli::Options defaults_;
    ChatInputStore* chat_inputs_ = nullptr;
};

}  // namespace ainiux::server
