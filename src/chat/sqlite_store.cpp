#include "chat/sqlite_store.hpp"

#include <sqlite3.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

namespace pkchat::chat {
namespace {

constexpr int kSchemaVersion = 2;
constexpr const char kDefaultThreadName[] = "New chat";
constexpr size_t kMaxThreadNameLength = 40;

std::string sqlite_code_name(int code) {
    return sqlite3_errstr(code);
}

Error sqlite_error(sqlite3* db, const std::string& path, const std::string& action, int code) {
    std::string message = action + ": " + path + ": ";
    message += db != nullptr ? sqlite3_errmsg(db) : sqlite_code_name(code);
    switch (code & 0xff) {
        case SQLITE_OK:
        case SQLITE_ROW:
        case SQLITE_DONE:
            return ok_error();
        case SQLITE_CANTOPEN:
        case SQLITE_PERM:
        case SQLITE_READONLY:
            return {ErrorCode::FileWrite, message};
        case SQLITE_BUSY:
        case SQLITE_LOCKED:
            return {ErrorCode::FileRead, message + " (database is busy)"};
        case SQLITE_CORRUPT:
        case SQLITE_NOTADB:
        case SQLITE_SCHEMA:
            return {ErrorCode::ProviderSchema,
                    message + ". Suggestion: move the database aside and restart pkchat."};
        case SQLITE_FULL:
        case SQLITE_IOERR:
            return {ErrorCode::FileWrite, message};
        case SQLITE_NOMEM:
            return {ErrorCode::Internal, message};
        default:
            return {ErrorCode::Internal, message};
    }
}

std::string errno_message(const std::string& action, const std::string& path) {
    return action + ": " + path + ": " + std::strerror(errno);
}

std::string dirname_of(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return ".";
    }
    if (slash == 0) {
        return "/";
    }
    return path.substr(0, slash);
}

Error ensure_directory(const std::string& path) {
    if (path.empty() || path == ".") {
        return ok_error();
    }
    if (path == "/") {
        return ok_error();
    }
    struct stat st {};
    if (stat(path.c_str(), &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return ok_error();
        }
        return {ErrorCode::FileWrite, path + " exists but is not a directory"};
    }
    if (errno != ENOENT) {
        return {ErrorCode::FileWrite, errno_message("could not inspect directory", path)};
    }
    Error parent = ensure_directory(dirname_of(path));
    if (!parent.ok()) {
        return parent;
    }
    if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
        return {ErrorCode::FileWrite, errno_message("could not create directory", path)};
    }
    return ok_error();
}

Error precreate_database_file(const std::string& path) {
    const int fd = open(path.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0) {
        return {ErrorCode::FileWrite, errno_message("could not create SQLite database", path)};
    }
    if (close(fd) != 0) {
        return {ErrorCode::FileWrite, errno_message("could not close SQLite database", path)};
    }
    return ok_error();
}

Error exec_sql(sqlite3* db, const std::string& path, const char* sql, const std::string& action) {
    char* raw_error = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &raw_error);
    if (rc == SQLITE_OK) {
        return ok_error();
    }
    std::string message = action + ": " + path + ": ";
    if (raw_error != nullptr) {
        message += raw_error;
        sqlite3_free(raw_error);
    } else {
        message += sqlite3_errmsg(db);
    }
    Error err = sqlite_error(db, path, action, rc);
    err.message = message;
    return err;
}

class Statement {
   public:
    Statement(sqlite3* db, std::string path) : db_(db), path_(std::move(path)) {}
    ~Statement() {
        if (stmt_ != nullptr) {
            sqlite3_finalize(stmt_);
        }
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    Error prepare(const char* sql) {
        const int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt_, nullptr);
        if (rc != SQLITE_OK) {
            return sqlite_error(db_, path_, "could not prepare SQLite statement", rc);
        }
        return ok_error();
    }
    Error bind_int64(int index, long long value) {
        const int rc = sqlite3_bind_int64(stmt_, index, static_cast<sqlite3_int64>(value));
        if (rc != SQLITE_OK) {
            return sqlite_error(db_, path_, "could not bind SQLite integer", rc);
        }
        return ok_error();
    }
    Error bind_null(int index) {
        const int rc = sqlite3_bind_null(stmt_, index);
        if (rc != SQLITE_OK) {
            return sqlite_error(db_, path_, "could not bind SQLite null", rc);
        }
        return ok_error();
    }
    Error bind_text(int index, const std::string& value) {
        const int rc = sqlite3_bind_text(stmt_, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
        if (rc != SQLITE_OK) {
            return sqlite_error(db_, path_, "could not bind SQLite text", rc);
        }
        return ok_error();
    }
    int step() { return sqlite3_step(stmt_); }
    Error step_done(const std::string& action) {
        const int rc = sqlite3_step(stmt_);
        if (rc != SQLITE_DONE) {
            return sqlite_error(db_, path_, action, rc);
        }
        return ok_error();
    }
    long long column_int64(int index) const {
        return static_cast<long long>(sqlite3_column_int64(stmt_, index));
    }
    std::string column_text(int index) const {
        const unsigned char* text = sqlite3_column_text(stmt_, index);
        const int bytes = sqlite3_column_bytes(stmt_, index);
        if (text == nullptr || bytes <= 0) {
            return "";
        }
        return std::string(reinterpret_cast<const char*>(text), static_cast<size_t>(bytes));
    }

   private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
    std::string path_;
};

class BindChain {
   public:
    explicit BindChain(Statement& stmt, Error initial = ok_error()) : stmt_(stmt), err_(std::move(initial)) {}

    BindChain& int64(int index, long long value) {
        if (err_.ok()) {
            err_ = stmt_.bind_int64(index, value);
        }
        return *this;
    }
    BindChain& text(int index, const std::string& value) {
        if (err_.ok()) {
            err_ = stmt_.bind_text(index, value);
        }
        return *this;
    }
    BindChain& null_value(int index) {
        if (err_.ok()) {
            err_ = stmt_.bind_null(index);
        }
        return *this;
    }
    Error error() const { return err_; }
    Error step_done(const std::string& action) {
        if (!err_.ok()) {
            return err_;
        }
        return stmt_.step_done(action);
    }

   private:
    Statement& stmt_;
    Error err_;
};

class Transaction {
   public:
    Transaction(sqlite3* db, std::string path) : db_(db), path_(std::move(path)) {}
    ~Transaction() {
        if (active_) {
            exec_sql(db_, path_, "ROLLBACK;", "could not roll back SQLite transaction");
        }
    }
    Error begin() {
        Error err = exec_sql(db_, path_, "BEGIN IMMEDIATE;", "could not begin SQLite transaction");
        active_ = err.ok();
        return err;
    }
    Error commit() {
        Error err = exec_sql(db_, path_, "COMMIT;", "could not commit SQLite transaction");
        if (err.ok()) {
            active_ = false;
        }
        return err;
    }

   private:
    sqlite3* db_ = nullptr;
    std::string path_;
    bool active_ = false;
};

std::string first_line(std::string text) {
    const size_t newline = text.find_first_of("\r\n");
    if (newline != std::string::npos) {
        text.resize(newline);
    }
    return ascii_trim(std::move(text));
}

bool is_auto_thread_name(const std::string& name) {
    const std::string trimmed = ascii_trim(name);
    return trimmed.empty() || trimmed == kDefaultThreadName;
}

std::string derive_thread_name(const Session& session) {
    std::string from_user;
    for (const provider::Message& message : session.messages) {
        if (message.role != "user") {
            continue;
        }
        std::string name = first_line(message.content);
        if (!name.empty()) {
            if (name.size() > kMaxThreadNameLength) {
                name.resize(kMaxThreadNameLength);
            }
            from_user = std::move(name);
            break;
        }
    }

    const std::string explicit_name = ascii_trim(session.name);
    if (!explicit_name.empty() && !is_auto_thread_name(explicit_name)) {
        return explicit_name;
    }
    if (!from_user.empty()) {
        return from_user;
    }
    return kDefaultThreadName;
}

Error load_thread_messages(sqlite3* db, const std::string& path, long long thread_id,
                           std::vector<provider::Message>& messages) {
    messages.clear();
    Statement stmt(db, path);
    Error err = stmt.prepare(
        "SELECT role, content FROM messages WHERE thread_id = ?1 ORDER BY ordinal, id;");
    if (!err.ok()) {
        return err;
    }
    err = BindChain(stmt).int64(1, thread_id).error();
    if (!err.ok()) {
        return err;
    }
    while (true) {
        const int rc = stmt.step();
        if (rc == SQLITE_DONE) {
            return ok_error();
        }
        if (rc != SQLITE_ROW) {
            return sqlite_error(db, path, "could not load SQLite messages for thread name migration", rc);
        }
        messages.push_back({stmt.column_text(0), stmt.column_text(1), {}});
    }
}

Error migrate_thread_names_v2(sqlite3* db, const std::string& path) {
    Statement stmt(db, path);
    Error err = stmt.prepare(
        "SELECT id FROM threads WHERE deleted_at IS NULL AND (name = '' OR name = ?1) "
        "AND EXISTS (SELECT 1 FROM messages WHERE messages.thread_id = threads.id "
        "AND messages.role = 'user' AND trim(messages.content) != '');");
    if (!err.ok()) {
        return err;
    }
    err = BindChain(stmt).text(1, kDefaultThreadName).error();
    if (!err.ok()) {
        return err;
    }

    std::vector<long long> thread_ids;
    while (true) {
        const int rc = stmt.step();
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            return sqlite_error(db, path, "could not list SQLite threads for name migration", rc);
        }
        thread_ids.push_back(stmt.column_int64(0));
    }

    for (long long thread_id : thread_ids) {
        Session session;
        session.name = kDefaultThreadName;
        err = load_thread_messages(db, path, thread_id, session.messages);
        if (!err.ok()) {
            return err;
        }
        const std::string derived_name = derive_thread_name(session);
        if (is_auto_thread_name(derived_name)) {
            continue;
        }
        Statement update(db, path);
        err = update.prepare("UPDATE threads SET name = ?1 WHERE id = ?2;");
        if (!err.ok()) {
            return err;
        }
        err = BindChain(update)
                  .text(1, derived_name)
                  .int64(2, thread_id)
                  .step_done("could not migrate SQLite thread name");
        if (!err.ok()) {
            return err;
        }
    }
    return ok_error();
}

bool valid_role(const std::string& role) {
    return role == "system" || role == "user" || role == "assistant";
}

Error ensure_schema(sqlite3* db, const std::string& path) {
    const char* schema = R"SQL(
CREATE TABLE IF NOT EXISTS schema_migrations (
    version INTEGER PRIMARY KEY,
    applied_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS app_state (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL,
    updated_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS threads (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    created_at TEXT NOT NULL,
    modified_at TEXT NOT NULL,
    last_provider TEXT NOT NULL,
    last_base_url TEXT NOT NULL DEFAULT '',
    last_model TEXT NOT NULL,
    settings_json TEXT NOT NULL DEFAULT '{}',
    usage_json TEXT NOT NULL DEFAULT '{}',
    message_count INTEGER NOT NULL DEFAULT 0,
    deleted_at TEXT
);
CREATE TABLE IF NOT EXISTS messages (
    id INTEGER PRIMARY KEY,
    thread_id INTEGER NOT NULL REFERENCES threads(id) ON DELETE CASCADE,
    ordinal INTEGER NOT NULL,
    created_at TEXT NOT NULL,
    role TEXT NOT NULL CHECK (role IN ('system', 'user', 'assistant')),
    content TEXT NOT NULL,
    metadata_json TEXT NOT NULL DEFAULT '{}',
    UNIQUE(thread_id, ordinal)
);
CREATE TABLE IF NOT EXISTS attachments (
    id INTEGER PRIMARY KEY,
    thread_id INTEGER NOT NULL REFERENCES threads(id) ON DELETE CASCADE,
    message_id INTEGER REFERENCES messages(id) ON DELETE CASCADE,
    ordinal INTEGER NOT NULL DEFAULT 0,
    kind TEXT NOT NULL,
    mime_type TEXT NOT NULL DEFAULT '',
    display_name TEXT NOT NULL DEFAULT '',
    metadata_json TEXT NOT NULL DEFAULT '{}',
    storage_ref TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS usage_records (
    id INTEGER PRIMARY KEY,
    thread_id INTEGER NOT NULL REFERENCES threads(id) ON DELETE CASCADE,
    message_id INTEGER REFERENCES messages(id) ON DELETE SET NULL,
    provider TEXT NOT NULL DEFAULT '',
    model TEXT NOT NULL DEFAULT '',
    usage_json TEXT NOT NULL,
    created_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS compaction_events (
    id INTEGER PRIMARY KEY,
    thread_id INTEGER NOT NULL REFERENCES threads(id) ON DELETE CASCADE,
    summary_message_id INTEGER REFERENCES messages(id) ON DELETE SET NULL,
    policy TEXT NOT NULL,
    messages_compacted INTEGER NOT NULL,
    original_bytes INTEGER NOT NULL,
    request_bytes INTEGER NOT NULL,
    notice TEXT NOT NULL DEFAULT '',
    metadata_json TEXT NOT NULL DEFAULT '{}',
    created_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_threads_list
    ON threads(deleted_at, modified_at DESC, id DESC);
CREATE INDEX IF NOT EXISTS idx_threads_provider_model
    ON threads(last_provider, last_model, modified_at DESC);
CREATE INDEX IF NOT EXISTS idx_messages_thread_ordinal
    ON messages(thread_id, ordinal);
CREATE INDEX IF NOT EXISTS idx_messages_thread_created
    ON messages(thread_id, created_at);
CREATE INDEX IF NOT EXISTS idx_attachments_thread_message
    ON attachments(thread_id, message_id, ordinal);
CREATE INDEX IF NOT EXISTS idx_usage_thread_created
    ON usage_records(thread_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_usage_message
    ON usage_records(message_id);
CREATE INDEX IF NOT EXISTS idx_compaction_thread_created
    ON compaction_events(thread_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_compaction_summary_message
    ON compaction_events(summary_message_id);
INSERT OR IGNORE INTO schema_migrations(version, applied_at)
    VALUES(1, strftime('%Y-%m-%dT%H:%M:%SZ','now'));
)SQL";
    Error err = exec_sql(db, path, schema, "could not initialize SQLite schema");
    if (!err.ok()) {
        return err;
    }

    Statement stmt(db, path);
    err = stmt.prepare("SELECT COALESCE(MAX(version), 0) FROM schema_migrations;");
    if (!err.ok()) {
        return err;
    }
    const int rc = stmt.step();
    if (rc != SQLITE_ROW) {
        return sqlite_error(db, path, "could not read SQLite schema version", rc);
    }
    long long version = stmt.column_int64(0);
    if (version > kSchemaVersion) {
        return {ErrorCode::ProviderSchema,
                "SQLite database schema is newer than this pkchat build: " + path};
    }
    if (version < 2) {
        err = migrate_thread_names_v2(db, path);
        if (!err.ok()) {
            return err;
        }
        err = exec_sql(db, path,
                       "INSERT OR IGNORE INTO schema_migrations(version, applied_at) "
                       "VALUES(2, strftime('%Y-%m-%dT%H:%M:%SZ','now'));",
                       "could not record SQLite schema migration 2");
        if (!err.ok()) {
            return err;
        }
        version = 2;
    }
    return ok_error();
}

Error insert_attachment(sqlite3* db,
                        const std::string& path,
                        long long thread_id,
                        long long message_id,
                        long long ordinal,
                        const provider::ImageInput& image,
                        const std::string& created_at) {
    Statement stmt(db, path);
    Error err = stmt.prepare(
        "INSERT INTO attachments(thread_id, message_id, ordinal, kind, mime_type, metadata_json, storage_ref, created_at) "
        "VALUES(?1, ?2, ?3, 'image', ?4, '{}', ?5, ?6);");
    if (!err.ok()) {
        return err;
    }
    return BindChain(stmt)
        .int64(1, thread_id)
        .int64(2, message_id)
        .int64(3, ordinal)
        .text(4, image.mime_type)
        .text(5, image.base64_data)
        .text(6, created_at)
        .step_done("could not insert SQLite attachment");
}

Error load_images_for_message(sqlite3* db,
                              const std::string& path,
                              long long message_id,
                              std::vector<provider::ImageInput>& images) {
    Statement stmt(db, path);
    Error err = stmt.prepare(
        "SELECT mime_type, storage_ref FROM attachments "
        "WHERE message_id = ?1 AND kind = 'image' ORDER BY ordinal, id;");
    if (!err.ok()) {
        return err;
    }
    err = BindChain(stmt).int64(1, message_id).error();
    if (!err.ok()) {
        return err;
    }
    while (true) {
        const int rc = stmt.step();
        if (rc == SQLITE_DONE) {
            return ok_error();
        }
        if (rc != SQLITE_ROW) {
            return sqlite_error(db, path, "could not load SQLite attachments", rc);
        }
        images.push_back({stmt.column_text(0), stmt.column_text(1)});
    }
}

}  // namespace

DatabasePathResult default_sqlite_database_path() {
    const char* home = std::getenv("HOME");
    if (home == nullptr || std::string(home).empty()) {
        return {"", {ErrorCode::Config, "HOME is not set; cannot locate ~/.pkchat/pkchat.db"}};
    }
    return {std::string(home) + "/.pkchat/pkchat.db", ok_error()};
}

SqliteStore::~SqliteStore() {
    close();
}

SqliteStore::SqliteStore(SqliteStore&& other) noexcept
    : db_(other.db_), path_(std::move(other.path_)) {
    other.db_ = nullptr;
}

SqliteStore& SqliteStore::operator=(SqliteStore&& other) noexcept {
    if (this != &other) {
        close();
        db_ = other.db_;
        path_ = std::move(other.path_);
        other.db_ = nullptr;
    }
    return *this;
}

void SqliteStore::close() {
    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    path_.clear();
}

Error SqliteStore::open_default() {
    DatabasePathResult resolved = default_sqlite_database_path();
    if (!resolved.error.ok()) {
        return resolved.error;
    }
    return open(resolved.path);
}

Error SqliteStore::open(const std::string& path) {
    close();
    if (path.empty()) {
        return {ErrorCode::BadArgs, "SQLite database path is empty"};
    }
    Error err = ensure_directory(dirname_of(path));
    if (!err.ok()) {
        return err;
    }
    err = precreate_database_file(path);
    if (!err.ok()) {
        return err;
    }
    sqlite3* opened = nullptr;
    const int rc = sqlite3_open_v2(path.c_str(),
                                   &opened,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                                   nullptr);
    if (rc != SQLITE_OK) {
        Error open_error = sqlite_error(opened, path, "could not open SQLite database", rc);
        if (opened != nullptr) {
            sqlite3_close(opened);
        }
        return open_error;
    }
    db_ = opened;
    path_ = path;

    const char* pragmas =
        "PRAGMA foreign_keys = ON;"
        "PRAGMA journal_mode = WAL;"
        "PRAGMA synchronous = NORMAL;"
        "PRAGMA busy_timeout = 50;";
    err = exec_sql(db_, path_, pragmas, "could not configure SQLite database");
    if (!err.ok()) {
        close();
        return err;
    }
    err = ensure_schema(db_, path_);
    if (!err.ok()) {
        close();
        return err;
    }
    return ok_error();
}

Error SqliteStore::set_last_thread_id(long long thread_id) {
    if (db_ == nullptr) {
        return {ErrorCode::Internal, "SQLite database is not open"};
    }
    Statement stmt(db_, path_);
    Error err = stmt.prepare(
        "INSERT INTO app_state(key, value, updated_at) VALUES('last_thread_id', ?1, ?2) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at;");
    if (!err.ok()) {
        return err;
    }
    return BindChain(stmt)
        .text(1, std::to_string(thread_id))
        .text(2, current_timestamp_utc())
        .step_done("could not update SQLite last thread");
}

Error SqliteStore::last_thread_id(long long& thread_id, bool& found) {
    found = false;
    thread_id = 0;
    if (db_ == nullptr) {
        return {ErrorCode::Internal, "SQLite database is not open"};
    }
    Statement stmt(db_, path_);
    Error err = stmt.prepare("SELECT value FROM app_state WHERE key = 'last_thread_id';");
    if (!err.ok()) return err;
    const int rc = stmt.step();
    if (rc == SQLITE_DONE) {
        return ok_error();
    }
    if (rc != SQLITE_ROW) {
        return sqlite_error(db_, path_, "could not read SQLite last thread", rc);
    }
    try {
        thread_id = std::stoll(stmt.column_text(0));
        found = thread_id > 0;
    } catch (...) {
        found = false;
        thread_id = 0;
    }
    return ok_error();
}

Error SqliteStore::save_session(Session& session) {
    if (db_ == nullptr) {
        return {ErrorCode::Internal, "SQLite database is not open"};
    }
    for (const provider::Message& message : session.messages) {
        if (!valid_role(message.role)) {
            return {ErrorCode::ProviderSchema, "cannot save message with unsupported role: " + message.role};
        }
    }

    Transaction tx(db_, path_);
    Error err = tx.begin();
    if (!err.ok()) {
        return err;
    }

    const std::string now = current_timestamp_utc();
    if (session.created_at.empty()) {
        session.created_at = now;
    }
    session.updated_at = now;
    session.name = derive_thread_name(session);

    if (session.thread_id <= 0) {
        Statement insert_thread(db_, path_);
        err = insert_thread.prepare(
            "INSERT INTO threads(name, created_at, modified_at, last_provider, last_base_url, last_model, "
            "settings_json, usage_json, message_count) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9);");
        if (!err.ok()) {
            return err;
        }
        err = BindChain(insert_thread)
                  .text(1, session.name)
                  .text(2, session.created_at)
                  .text(3, session.updated_at)
                  .text(4, session.provider)
                  .text(5, session.base_url)
                  .text(6, session.model)
                  .text(7, session.settings_json.empty() ? "{}" : session.settings_json)
                  .text(8, session.usage_json.empty() ? "{}" : session.usage_json)
                  .int64(9, static_cast<long long>(session.messages.size()))
                  .step_done("could not insert SQLite thread");
        if (!err.ok()) {
            return err;
        }
        session.thread_id = static_cast<long long>(sqlite3_last_insert_rowid(db_));
    } else {
        Statement update_thread(db_, path_);
        err = update_thread.prepare(
            "UPDATE threads SET name = ?1, modified_at = ?2, last_provider = ?3, last_base_url = ?4, "
            "last_model = ?5, settings_json = ?6, usage_json = ?7, message_count = ?8, deleted_at = NULL "
            "WHERE id = ?9;");
        if (!err.ok()) {
            return err;
        }
        err = BindChain(update_thread)
                  .text(1, session.name)
                  .text(2, session.updated_at)
                  .text(3, session.provider)
                  .text(4, session.base_url)
                  .text(5, session.model)
                  .text(6, session.settings_json.empty() ? "{}" : session.settings_json)
                  .text(7, session.usage_json.empty() ? "{}" : session.usage_json)
                  .int64(8, static_cast<long long>(session.messages.size()))
                  .int64(9, session.thread_id)
                  .step_done("could not update SQLite thread");
        if (!err.ok()) {
            return err;
        }
        if (sqlite3_changes(db_) == 0) {
            session.thread_id = 0;
            return {ErrorCode::FileWrite, "could not update SQLite thread: thread does not exist"};
        }
    }

    for (const char* sql : {
             "DELETE FROM usage_records WHERE thread_id = ?1;",
             "DELETE FROM compaction_events WHERE thread_id = ?1;",
             "DELETE FROM messages WHERE thread_id = ?1;"}) {
        Statement cleanup(db_, path_);
        err = cleanup.prepare(sql);
        if (!err.ok()) {
            return err;
        }
        err = BindChain(cleanup).int64(1, session.thread_id).step_done("could not replace SQLite thread contents");
        if (!err.ok()) {
            return err;
        }
    }

    long long last_assistant_message_id = 0;
    for (size_t i = 0; i < session.messages.size(); ++i) {
        const provider::Message& message = session.messages[i];
        Statement insert_message(db_, path_);
        err = insert_message.prepare(
            "INSERT INTO messages(thread_id, ordinal, created_at, role, content, metadata_json) "
            "VALUES(?1, ?2, ?3, ?4, ?5, '{}');");
        if (!err.ok()) {
            return err;
        }
        err = BindChain(insert_message)
                  .int64(1, session.thread_id)
                  .int64(2, static_cast<long long>(i))
                  .text(3, session.updated_at)
                  .text(4, message.role)
                  .text(5, message.content)
                  .step_done("could not insert SQLite message");
        if (!err.ok()) {
            return err;
        }
        const long long message_id = static_cast<long long>(sqlite3_last_insert_rowid(db_));
        if (message.role == "assistant") {
            last_assistant_message_id = message_id;
        }
        for (size_t image_index = 0; image_index < message.images.size(); ++image_index) {
            err = insert_attachment(db_, path_, session.thread_id, message_id,
                                    static_cast<long long>(image_index),
                                    message.images[image_index], session.updated_at);
            if (!err.ok()) return err;
        }
    }

    if (!session.usage_json.empty() && session.usage_json != "null" && session.usage_json != "{}") {
        Statement usage(db_, path_);
        err = usage.prepare(
            "INSERT INTO usage_records(thread_id, message_id, provider, model, usage_json, created_at) "
            "VALUES(?1, ?2, ?3, ?4, ?5, ?6);");
        if (!err.ok()) {
            return err;
        }
        BindChain usage_bind(usage);
        usage_bind.int64(1, session.thread_id);
        if (last_assistant_message_id > 0) {
            usage_bind.int64(2, last_assistant_message_id);
        } else {
            usage_bind.null_value(2);
        }
        err = usage_bind.text(3, session.provider)
                    .text(4, session.model)
                    .text(5, session.usage_json)
                    .text(6, session.updated_at)
                    .step_done("could not insert SQLite usage record");
        if (!err.ok()) {
            return err;
        }
    }

    for (const context::CompactionEvent& event : session.compaction_events) {
        Statement compact(db_, path_);
        err = compact.prepare(
            "INSERT INTO compaction_events(thread_id, summary_message_id, policy, messages_compacted, "
            "original_bytes, request_bytes, notice, metadata_json, created_at) "
            "VALUES(?1, NULL, ?2, ?3, ?4, ?5, ?6, '{}', ?7);");
        if (!err.ok()) {
            return err;
        }
        err = BindChain(compact)
                  .int64(1, session.thread_id)
                  .text(2, event.policy)
                  .int64(3, static_cast<long long>(event.messages_compacted))
                  .int64(4, static_cast<long long>(event.original_bytes))
                  .int64(5, static_cast<long long>(event.request_bytes))
                  .text(6, event.notice)
                  .text(7, event.timestamp.empty() ? session.updated_at : event.timestamp)
                  .step_done("could not insert SQLite compaction event");
        if (!err.ok()) {
            return err;
        }
    }

    if (!(err = tx.commit()).ok()) {
        return err;
    }
    return set_last_thread_id(session.thread_id);
}

Error SqliteStore::load_session(long long thread_id, Session& session) {
    if (db_ == nullptr) {
        return {ErrorCode::Internal, "SQLite database is not open"};
    }
    if (thread_id <= 0) {
        return {ErrorCode::BadArgs, "SQLite thread id must be positive"};
    }

    Statement thread(db_, path_);
    Error err = thread.prepare(
        "SELECT id, name, created_at, modified_at, last_provider, last_base_url, last_model, "
        "settings_json, usage_json, message_count FROM threads WHERE id = ?1 AND deleted_at IS NULL;");
    if (!err.ok()) {
        return err;
    }
    err = BindChain(thread).int64(1, thread_id).error();
    if (!err.ok()) {
        return err;
    }
    int rc = thread.step();
    if (rc == SQLITE_DONE) {
        return {ErrorCode::FileRead, "SQLite chat thread not found: " + std::to_string(thread_id)};
    }
    if (rc != SQLITE_ROW) {
        return sqlite_error(db_, path_, "could not load SQLite thread", rc);
    }

    Session loaded;
    loaded.thread_id = thread.column_int64(0);
    loaded.name = thread.column_text(1);
    loaded.created_at = thread.column_text(2);
    loaded.updated_at = thread.column_text(3);
    loaded.provider = thread.column_text(4);
    loaded.base_url = thread.column_text(5);
    loaded.model = thread.column_text(6);
    loaded.settings_json = thread.column_text(7);
    loaded.usage_json = thread.column_text(8);

    Statement messages(db_, path_);
    err = messages.prepare(
        "SELECT id, role, content FROM messages WHERE thread_id = ?1 ORDER BY ordinal, id;");
    if (!err.ok()) {
        return err;
    }
    err = BindChain(messages).int64(1, thread_id).error();
    if (!err.ok()) {
        return err;
    }
    while (true) {
        rc = messages.step();
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            return sqlite_error(db_, path_, "could not load SQLite messages", rc);
        }
        std::vector<provider::ImageInput> images;
        err = load_images_for_message(db_, path_, messages.column_int64(0), images);
        if (!err.ok()) return err;
        loaded.messages.push_back({messages.column_text(1), messages.column_text(2), std::move(images)});
    }

    Statement compactions(db_, path_);
    err = compactions.prepare(
        "SELECT created_at, policy, messages_compacted, original_bytes, request_bytes, notice "
        "FROM compaction_events WHERE thread_id = ?1 ORDER BY created_at, id;");
    if (!err.ok()) {
        return err;
    }
    err = BindChain(compactions).int64(1, thread_id).error();
    if (!err.ok()) {
        return err;
    }
    while (true) {
        rc = compactions.step();
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            return sqlite_error(db_, path_, "could not load SQLite compaction events", rc);
        }
        context::CompactionEvent event;
        event.timestamp = compactions.column_text(0);
        event.policy = compactions.column_text(1);
        event.messages_compacted = static_cast<size_t>(compactions.column_int64(2));
        event.original_bytes = static_cast<size_t>(compactions.column_int64(3));
        event.request_bytes = static_cast<size_t>(compactions.column_int64(4));
        event.notice = compactions.column_text(5);
        loaded.compaction_events.push_back(std::move(event));
    }

    session = std::move(loaded);
    return set_last_thread_id(thread_id);
}

Error SqliteStore::list_threads(std::vector<ThreadSummary>& threads, int limit) {
    threads.clear();
    if (db_ == nullptr) {
        return {ErrorCode::Internal, "SQLite database is not open"};
    }
    if (limit <= 0) {
        limit = 200;
    }
    if (limit > 1000) {
        limit = 1000;
    }
    Statement stmt(db_, path_);
    Error err = stmt.prepare(
        "SELECT id, name, created_at, modified_at, last_provider, last_base_url, last_model, message_count "
        "FROM threads WHERE deleted_at IS NULL ORDER BY modified_at DESC, id DESC LIMIT ?1;");
    if (!err.ok()) {
        return err;
    }
    err = BindChain(stmt).int64(1, limit).error();
    if (!err.ok()) {
        return err;
    }
    while (true) {
        const int rc = stmt.step();
        if (rc == SQLITE_DONE) {
            return ok_error();
        }
        if (rc != SQLITE_ROW) {
            return sqlite_error(db_, path_, "could not list SQLite threads", rc);
        }
        ThreadSummary summary;
        summary.id = stmt.column_int64(0);
        summary.name = stmt.column_text(1);
        summary.created_at = stmt.column_text(2);
        summary.modified_at = stmt.column_text(3);
        summary.last_provider = stmt.column_text(4);
        summary.last_base_url = stmt.column_text(5);
        summary.last_model = stmt.column_text(6);
        summary.message_count = stmt.column_int64(7);
        threads.push_back(std::move(summary));
    }
}

Error SqliteStore::soft_delete_thread(long long thread_id) {
    if (db_ == nullptr) {
        return {ErrorCode::Internal, "SQLite database is not open"};
    }
    Statement stmt(db_, path_);
    Error err = stmt.prepare("UPDATE threads SET deleted_at = ?1, modified_at = ?1 WHERE id = ?2 AND deleted_at IS NULL;");
    if (!err.ok()) {
        return err;
    }
    err = BindChain(stmt)
              .text(1, current_timestamp_utc())
              .int64(2, thread_id)
              .step_done("could not delete SQLite thread");
    if (!err.ok()) {
        return err;
    }
    return ok_error();
}

Error SqliteStore::soft_delete_empty_threads(long long& deleted_count,
                                             long long watch_thread_id,
                                             bool& watch_thread_deleted) {
    deleted_count = 0;
    watch_thread_deleted = false;
    if (db_ == nullptr) {
        return {ErrorCode::Internal, "SQLite database is not open"};
    }

    std::vector<long long> thread_ids;
    Statement select(db_, path_);
    Error err = select.prepare(
        "SELECT t.id FROM threads t "
        "WHERE t.deleted_at IS NULL AND NOT EXISTS ("
        "SELECT 1 FROM messages m WHERE m.thread_id = t.id AND m.role IN ('user', 'assistant')"
        ");");
    if (!err.ok()) {
        return err;
    }
    while (true) {
        const int rc = select.step();
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            return sqlite_error(db_, path_, "could not list empty SQLite threads", rc);
        }
        thread_ids.push_back(select.column_int64(0));
    }

    if (thread_ids.empty()) {
        return ok_error();
    }

    const std::string now = current_timestamp_utc();
    for (long long thread_id : thread_ids) {
        Statement stmt(db_, path_);
        err = stmt.prepare(
            "UPDATE threads SET deleted_at = ?1, modified_at = ?1 WHERE id = ?2 AND deleted_at IS NULL;");
        if (!err.ok()) {
            return err;
        }
        err = BindChain(stmt).text(1, now).int64(2, thread_id).step_done("could not delete empty SQLite thread");
        if (!err.ok()) {
            return err;
        }
        if (sqlite3_changes(db_) > 0) {
            ++deleted_count;
            if (thread_id == watch_thread_id) {
                watch_thread_deleted = true;
            }
        }
    }
    return ok_error();
}

}  // namespace pkchat::chat
