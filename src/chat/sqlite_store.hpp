#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "chat/session.hpp"
#include "common.hpp"
#include "runtime/runtime.hpp"

struct sqlite3;

namespace ainiux::chat {

struct DatabasePathResult {
    std::string path;
    Error error;
};

struct ThreadSummary {
    long long id = 0;
    long long revision = 0;
    std::string name;
    std::string created_at;
    std::string modified_at;
    std::string last_provider;
    std::string last_base_url;
    std::string last_model;
    long long message_count = 0;
    bool read_only = false;
    std::string read_only_reason;
};

struct MediaCleanupResult {
    long long objects_expired = 0;
    long long files_removed = 0;
    long long bytes_reclaimed = 0;
    long long threads_locked = 0;
};

struct LoadSessionOptions {
    // Zero retains the existing unbounded local-TUI load behavior.
    std::size_t max_messages = 0;
    std::size_t max_content_bytes = 0;
    std::size_t max_attachments_per_message = 0;
    bool metadata_only_attachments = false;
    bool load_compactions = true;
    bool update_last_thread = true;
};

class SqliteStore {
   public:
    SqliteStore() = default;
    ~SqliteStore();
    SqliteStore(const SqliteStore&) = delete;
    SqliteStore& operator=(const SqliteStore&) = delete;
    SqliteStore(SqliteStore&& other) noexcept;
    SqliteStore& operator=(SqliteStore&& other) noexcept;

    Error open(const std::string& path);
    Error open_default();
    void close();
    bool is_open() const { return db_ != nullptr; }
    const std::string& path() const { return path_; }

    Error save_session(Session& session);
    Error append_messages(long long thread_id,
                          long long expected_revision,
                          const std::vector<provider::Message>& messages,
                          const std::optional<std::string>& provider,
                          const std::optional<std::string>& model,
                          long long& revision,
                          long long& message_count);
    Error load_session(long long thread_id,
                       Session& session,
                       const LoadSessionOptions& options = LoadSessionOptions());
    Error list_threads(std::vector<ThreadSummary>& threads, int limit = 200);
    Error last_thread_id(long long& thread_id, bool& found);
    Error set_last_thread_id(long long thread_id);
    Error app_state(const std::string& key, std::string& value, bool& found);
    Error set_app_state(const std::string& key, const std::string& value);
    Error import_media(const std::string& bytes,
                       const std::string& mime_type,
                       const std::string& display_name,
                       const std::string& source_ref,
                       provider::ImageInput& image);
    Error import_text_attachment(const std::string& markdown,
                                 size_t max_size_to_store_to_db,
                                 const std::string& display_name,
                                 const std::string& source_ref,
                                 provider::TextAttachment& attachment);
    Error cleanup_media(int expiration_days,
                        long long protected_thread_id,
                        const std::string& reason,
                        MediaCleanupResult& result,
                        runtime::CancellationToken cancellation = runtime::CancellationToken());
    Error soft_delete_thread(long long thread_id);
    Error soft_delete_empty_threads(long long& deleted_count,
                                    long long watch_thread_id,
                                    bool& watch_thread_deleted);

   private:
    sqlite3* db_ = nullptr;
    std::string path_;
};

DatabasePathResult default_sqlite_database_path();

}  // namespace ainiux::chat
