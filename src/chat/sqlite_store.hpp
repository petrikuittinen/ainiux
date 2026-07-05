#pragma once

#include <string>
#include <vector>

#include "chat/session.hpp"
#include "common.hpp"

struct sqlite3;

namespace pkchat::chat {

struct DatabasePathResult {
    std::string path;
    Error error;
};

struct ThreadSummary {
    long long id = 0;
    std::string name;
    std::string created_at;
    std::string modified_at;
    std::string last_provider;
    std::string last_base_url;
    std::string last_model;
    long long message_count = 0;
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
    Error load_session(long long thread_id, Session& session);
    Error list_threads(std::vector<ThreadSummary>& threads, int limit = 200);
    Error last_thread_id(long long& thread_id, bool& found);
    Error set_last_thread_id(long long thread_id);
    Error soft_delete_thread(long long thread_id);
    Error soft_delete_empty_threads(long long& deleted_count,
                                    long long watch_thread_id,
                                    bool& watch_thread_deleted);

   private:
    sqlite3* db_ = nullptr;
    std::string path_;
};

DatabasePathResult default_sqlite_database_path();

}  // namespace pkchat::chat
