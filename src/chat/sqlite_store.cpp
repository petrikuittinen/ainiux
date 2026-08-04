#include "chat/sqlite_store.hpp"

#include "chat/media_store.hpp"
#include "platform/environment.hpp"
#include "platform/filesystem.hpp"

#include <sqlite3.h>

#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <utility>

namespace ainiux::chat {
namespace {

constexpr int kSchemaVersion = 4;
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
                    message + ". Suggestion: move the database aside and restart ainiux."};
        case SQLITE_FULL:
        case SQLITE_IOERR:
            return {ErrorCode::FileWrite, message};
        case SQLITE_NOMEM:
            return {ErrorCode::Internal, message};
        default:
            return {ErrorCode::Internal, message};
    }
}

std::string dirname_of(const std::string& path) {
    const std::filesystem::path parent =
        std::filesystem::u8path(path).parent_path();
    return parent.empty() ? "." : parent.u8string();
}

Error ensure_directory(const std::string& path) {
    const bool product_profile =
        std::filesystem::u8path(path).filename().u8string() == ".ainiux";
    return platform::ensure_private_directory(path, true, product_profile);
}

Error precreate_database_file(const std::string& path) {
    return platform::create_private_file_if_missing(path);
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

bool table_has_column(sqlite3* db,
                      const std::string& path,
                      const std::string& table,
                      const std::string& column,
                      Error& error) {
    Statement stmt(db, path);
    error = stmt.prepare(("PRAGMA table_info(" + table + ");").c_str());
    if (!error.ok()) {
        return false;
    }
    while (true) {
        const int rc = stmt.step();
        if (rc == SQLITE_DONE) {
            return false;
        }
        if (rc != SQLITE_ROW) {
            error = sqlite_error(db, path, "could not inspect SQLite table columns", rc);
            return false;
        }
        if (stmt.column_text(1) == column) {
            return true;
        }
    }
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
                "SQLite database schema is newer than this ainiux build: " + path};
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
    if (version < 3) {
        Transaction migration(db, path);
        err = migration.begin();
        if (!err.ok()) {
            return err;
        }
        err = exec_sql(db, path, R"SQL(
CREATE TABLE IF NOT EXISTS media_objects (
    sha256 TEXT PRIMARY KEY,
    kind TEXT NOT NULL,
    mime_type TEXT NOT NULL,
    byte_size INTEGER NOT NULL,
    storage_ref TEXT NOT NULL UNIQUE,
    created_at TEXT NOT NULL,
    last_used_at TEXT NOT NULL,
    deleted_at TEXT,
    delete_reason TEXT NOT NULL DEFAULT ''
);
)SQL",
                       "could not migrate SQLite media schema to version 3");
        if (!err.ok()) {
            return err;
        }
        for (const auto& column : {
                 std::pair<const char*, const char*>{"threads", "read_only INTEGER NOT NULL DEFAULT 0"},
                 {"threads", "read_only_reason TEXT NOT NULL DEFAULT ''"},
                 {"attachments", "object_sha256 TEXT REFERENCES media_objects(sha256)"},
                 {"attachments", "source_ref TEXT NOT NULL DEFAULT ''"},
                 {"attachments", "byte_size INTEGER NOT NULL DEFAULT 0"}}) {
            const std::string definition = column.second;
            const std::string column_name = definition.substr(0, definition.find(' '));
            Error inspect_error;
            if (!table_has_column(db, path, column.first, column_name, inspect_error)) {
                if (!inspect_error.ok()) {
                    return inspect_error;
                }
                const std::string sql = "ALTER TABLE " + std::string(column.first) +
                                        " ADD COLUMN " + definition + ";";
                err = exec_sql(db, path, sql.c_str(),
                               "could not add SQLite media schema column");
                if (!err.ok()) {
                    return err;
                }
            }
        }
        err = exec_sql(db, path, R"SQL(
CREATE INDEX IF NOT EXISTS idx_attachments_object ON attachments(object_sha256);
CREATE INDEX IF NOT EXISTS idx_media_last_used ON media_objects(deleted_at, last_used_at);
INSERT OR IGNORE INTO schema_migrations(version, applied_at)
    VALUES(3, strftime('%Y-%m-%dT%H:%M:%SZ','now'));
)SQL",
                       "could not finish SQLite media schema migration 3");
        if (!err.ok()) {
            return err;
        }
        err = migration.commit();
        if (!err.ok()) {
            return err;
        }
        version = 3;
    }
    if (version < 4) {
        Transaction migration(db, path);
        err = migration.begin();
        if (!err.ok()) {
            return err;
        }
        Error inspect_error;
        if (!table_has_column(db, path, "attachments", "inline_content", inspect_error)) {
            if (!inspect_error.ok()) {
                return inspect_error;
            }
            err = exec_sql(db, path,
                           "ALTER TABLE attachments ADD COLUMN inline_content TEXT NOT NULL DEFAULT '';",
                           "could not add inline Markdown attachment content");
            if (!err.ok()) {
                return err;
            }
        }
        err = exec_sql(db, path,
                       "INSERT OR IGNORE INTO schema_migrations(version, applied_at) "
                       "VALUES(4, strftime('%Y-%m-%dT%H:%M:%SZ','now'));",
                       "could not record SQLite schema migration 4");
        if (!err.ok()) {
            return err;
        }
        err = migration.commit();
        if (!err.ok()) {
            return err;
        }
        version = 4;
    }
    return ok_error();
}

Error upsert_media_object(sqlite3* db,
                          const std::string& path,
                          const StoredMedia& stored,
                          const std::string& kind,
                          const std::string& timestamp) {
    Statement stmt(db, path);
    Error err = stmt.prepare(
        "INSERT INTO media_objects(sha256, kind, mime_type, byte_size, storage_ref, created_at, "
        "last_used_at, deleted_at, delete_reason) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?6, NULL, '') "
        "ON CONFLICT(sha256) DO UPDATE SET mime_type = excluded.mime_type, "
        "byte_size = excluded.byte_size, storage_ref = excluded.storage_ref, "
        "last_used_at = excluded.last_used_at, deleted_at = NULL, delete_reason = ''; ");
    if (!err.ok()) {
        return err;
    }
    return BindChain(stmt)
        .text(1, stored.sha256)
        .text(2, kind)
        .text(3, stored.mime_type)
        .int64(4, stored.byte_size)
        .text(5, stored.storage_ref)
        .text(6, timestamp)
        .step_done("could not register managed media object");
}

Error insert_attachment(sqlite3* db,
                        const std::string& path,
                        long long thread_id,
                        long long message_id,
                        long long ordinal,
                        const provider::ImageInput& original_image,
                        const std::string& created_at) {
    provider::ImageInput image = original_image;
    StoredMedia stored;
    if (image.storage_ref.empty()) {
        Error store_error = store_media_base64(path, image.base64_data, image.mime_type, stored);
        if (!store_error.ok()) {
            return store_error;
        }
        image.storage_ref = stored.sha256;
        image.byte_size = stored.byte_size;
    } else {
        bool available = false;
        Error media_error = media_file_available(path, image.storage_ref, image.mime_type,
                                                 image.byte_size, available);
        if (!media_error.ok()) {
            return media_error;
        }
        if (!available) {
            return {ErrorCode::FileRead,
                    "cannot save message because managed attachment media is unavailable: " +
                        image.storage_ref};
        }
        stored.sha256 = image.storage_ref;
        stored.storage_ref = "sha256/" + image.storage_ref.substr(0, 2) + "/" + image.storage_ref;
        stored.mime_type = image.mime_type;
        stored.byte_size = image.byte_size;
    }
    Error err = upsert_media_object(db, path, stored, "image", created_at);
    if (!err.ok()) {
        return err;
    }
    Statement stmt(db, path);
    err = stmt.prepare(
        "INSERT INTO attachments(thread_id, message_id, ordinal, kind, mime_type, display_name, "
        "metadata_json, storage_ref, created_at, object_sha256, source_ref, byte_size) "
        "VALUES(?1, ?2, ?3, 'image', ?4, ?5, '{}', '', ?6, ?7, ?8, ?9);");
    if (!err.ok()) {
        return err;
    }
    return BindChain(stmt)
        .int64(1, thread_id)
        .int64(2, message_id)
        .int64(3, ordinal)
        .text(4, image.mime_type)
        .text(5, image.display_name)
        .text(6, created_at)
        .text(7, image.storage_ref)
        .text(8, image.source_ref)
        .int64(9, image.byte_size)
        .step_done("could not insert SQLite attachment");
}

Error load_images_for_message(sqlite3* db,
                              const std::string& path,
                              long long message_id,
                              std::vector<provider::ImageInput>& images) {
    Statement stmt(db, path);
    Error err = stmt.prepare(
        "SELECT a.mime_type, a.storage_ref, COALESCE(a.object_sha256, ''), a.display_name, "
        "a.source_ref, a.byte_size FROM attachments a "
        "WHERE a.message_id = ?1 AND a.kind = 'image' ORDER BY a.ordinal, a.id;");
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
        provider::ImageInput image;
        image.mime_type = stmt.column_text(0);
        image.storage_ref = stmt.column_text(2);
        image.display_name = stmt.column_text(3);
        image.source_ref = stmt.column_text(4);
        image.byte_size = stmt.column_int64(5);
        if (image.storage_ref.empty()) {
            // Version 1/2 compatibility: storage_ref contained inline base64.
            image.base64_data = stmt.column_text(1);
        }
        images.push_back(std::move(image));
    }
}

Error insert_text_attachment(sqlite3* db,
                             const std::string& path,
                             long long thread_id,
                             long long message_id,
                             long long ordinal,
                             const provider::TextAttachment& attachment,
                             const std::string& created_at) {
    if (!attachment.storage_ref.empty()) {
        bool available = false;
        Error err = media_file_available(path, attachment.storage_ref, "text/markdown",
                                         attachment.byte_size, available);
        if (!err.ok()) {
            return err;
        }
        if (!available) {
            return {ErrorCode::FileRead,
                    "cannot save message because managed Markdown is unavailable: " +
                        attachment.storage_ref};
        }
        StoredMedia stored;
        stored.sha256 = attachment.storage_ref;
        stored.storage_ref = "sha256/" + attachment.storage_ref.substr(0, 2) + "/" +
                             attachment.storage_ref + ".md";
        stored.mime_type = "text/markdown";
        stored.byte_size = attachment.byte_size;
        err = upsert_media_object(db, path, stored, "markdown", created_at);
        if (!err.ok()) {
            return err;
        }
        Statement stmt(db, path);
        err = stmt.prepare(
            "INSERT INTO attachments(thread_id, message_id, ordinal, kind, mime_type, display_name, "
            "metadata_json, storage_ref, created_at, object_sha256, source_ref, byte_size, inline_content) "
            "VALUES(?1, ?2, ?3, 'markdown', 'text/markdown', ?4, '{}', '', ?5, ?6, ?7, ?8, '');");
        if (!err.ok()) {
            return err;
        }
        return BindChain(stmt)
            .int64(1, thread_id)
            .int64(2, message_id)
            .int64(3, ordinal)
            .text(4, attachment.display_name)
            .text(5, created_at)
            .text(6, attachment.storage_ref)
            .text(7, attachment.source_ref)
            .int64(8, attachment.byte_size)
            .step_done("could not insert managed Markdown attachment");
    }

    Statement stmt(db, path);
    Error err = stmt.prepare(
        "INSERT INTO attachments(thread_id, message_id, ordinal, kind, mime_type, display_name, "
        "metadata_json, storage_ref, created_at, object_sha256, source_ref, byte_size, inline_content) "
        "VALUES(?1, ?2, ?3, 'markdown', 'text/markdown', ?4, '{}', '', ?5, NULL, ?6, ?7, ?8);");
    if (!err.ok()) {
        return err;
    }
    return BindChain(stmt)
        .int64(1, thread_id)
        .int64(2, message_id)
        .int64(3, ordinal)
        .text(4, attachment.display_name)
        .text(5, created_at)
        .text(6, attachment.source_ref)
        .int64(7, static_cast<long long>(attachment.markdown_content.size()))
        .text(8, attachment.markdown_content)
        .step_done("could not insert inline Markdown attachment");
}

Error load_text_attachments_for_message(
    sqlite3* db,
    const std::string& path,
    long long message_id,
    std::vector<provider::TextAttachment>& attachments) {
    Statement stmt(db, path);
    Error err = stmt.prepare(
        "SELECT display_name, source_ref, byte_size, inline_content, "
        "COALESCE(object_sha256, '') FROM attachments "
        "WHERE message_id = ?1 AND kind = 'markdown' ORDER BY ordinal, id;");
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
            return sqlite_error(db, path, "could not load SQLite text attachments", rc);
        }
        provider::TextAttachment attachment;
        attachment.display_name = stmt.column_text(0);
        attachment.source_ref = stmt.column_text(1);
        attachment.byte_size = stmt.column_int64(2);
        attachment.markdown_content = stmt.column_text(3);
        attachment.storage_ref = stmt.column_text(4);
        attachments.push_back(std::move(attachment));
    }
}

}  // namespace

DatabasePathResult default_sqlite_database_path() {
    const std::string home = platform::home_directory();
    if (home.empty()) {
        return {"", {ErrorCode::Config,
                     "HOME is not set (and USERPROFILE is unavailable on Windows); "
                     "cannot locate ~/.ainiux/ainiux.db"}};
    }
    return {(std::filesystem::u8path(home) / ".ainiux" / "ainiux.db").u8string(),
            ok_error()};
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
    return set_app_state("last_thread_id", std::to_string(thread_id));
}

Error SqliteStore::set_app_state(const std::string& key, const std::string& value) {
    if (db_ == nullptr) {
        return {ErrorCode::Internal, "SQLite database is not open"};
    }
    if (key.empty() || key.size() > 128) {
        return {ErrorCode::BadArgs, "SQLite app-state key must contain 1 through 128 characters"};
    }
    Statement stmt(db_, path_);
    Error err = stmt.prepare(
        "INSERT INTO app_state(key, value, updated_at) VALUES(?1, ?2, ?3) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at;");
    if (!err.ok()) {
        return err;
    }
    return BindChain(stmt)
        .text(1, key)
        .text(2, value)
        .text(3, current_timestamp_utc())
        .step_done("could not update SQLite app state");
}

Error SqliteStore::last_thread_id(long long& thread_id, bool& found) {
    found = false;
    thread_id = 0;
    std::string value;
    Error err = app_state("last_thread_id", value, found);
    if (!err.ok() || !found) return err;
    try {
        thread_id = std::stoll(value);
        found = thread_id > 0;
    } catch (...) {
        found = false;
        thread_id = 0;
    }
    return ok_error();
}

Error SqliteStore::app_state(const std::string& key, std::string& value, bool& found) {
    found = false;
    value.clear();
    if (db_ == nullptr) return {ErrorCode::Internal, "SQLite database is not open"};
    Statement stmt(db_, path_);
    Error err = stmt.prepare("SELECT value FROM app_state WHERE key = ?1;");
    if (!err.ok()) return err;
    err = BindChain(stmt).text(1, key).error();
    if (!err.ok()) return err;
    const int rc = stmt.step();
    if (rc == SQLITE_DONE) return ok_error();
    if (rc != SQLITE_ROW) return sqlite_error(db_, path_, "could not read SQLite app state", rc);
    value = stmt.column_text(0);
    found = true;
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
    if (session.thread_id > 0) {
        Statement locked(db_, path_);
        Error lock_error = locked.prepare(
            "SELECT read_only, read_only_reason FROM threads WHERE id = ?1;");
        if (!lock_error.ok()) {
            return lock_error;
        }
        lock_error = BindChain(locked).int64(1, session.thread_id).error();
        if (!lock_error.ok()) {
            return lock_error;
        }
        const int lock_rc = locked.step();
        if (lock_rc == SQLITE_ROW && locked.column_int64(0) != 0) {
            const std::string reason = locked.column_text(1);
            return {ErrorCode::FileWrite,
                    "chat thread is read-only" +
                        (reason.empty() ? std::string() : ": " + reason)};
        }
        if (lock_rc != SQLITE_ROW && lock_rc != SQLITE_DONE) {
            return sqlite_error(db_, path_, "could not inspect SQLite thread lock", lock_rc);
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
        for (size_t text_index = 0; text_index < message.text_attachments.size(); ++text_index) {
            err = insert_text_attachment(db_, path_, session.thread_id, message_id,
                                         static_cast<long long>(text_index),
                                         message.text_attachments[text_index], session.updated_at);
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
        "settings_json, usage_json, message_count, read_only, read_only_reason "
        "FROM threads WHERE id = ?1 AND deleted_at IS NULL;");
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
    loaded.read_only = thread.column_int64(10) != 0;
    loaded.read_only_reason = thread.column_text(11);

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
        std::vector<provider::TextAttachment> text_attachments;
        err = load_text_attachments_for_message(db_, path_, messages.column_int64(0),
                                                text_attachments);
        if (!err.ok()) return err;
        loaded.messages.push_back({messages.column_text(1), messages.column_text(2),
                                   std::move(images), std::move(text_attachments)});
    }

    if (!loaded.read_only) {
        for (const provider::Message& message : loaded.messages) {
            for (const provider::ImageInput& image : message.images) {
                if (image.storage_ref.empty()) {
                    continue;
                }
                Statement object(db_, path_);
                err = object.prepare(
                    "SELECT byte_size, deleted_at, delete_reason FROM media_objects WHERE sha256 = ?1;");
                if (!err.ok()) return err;
                err = BindChain(object).text(1, image.storage_ref).error();
                if (!err.ok()) return err;
                const int object_rc = object.step();
                bool available = false;
                std::string media_reason;
                if (object_rc == SQLITE_DONE) {
                    media_reason = "managed attachment metadata is missing";
                } else if (object_rc != SQLITE_ROW) {
                    return sqlite_error(db_, path_, "could not inspect managed media", object_rc);
                } else if (!object.column_text(1).empty()) {
                    media_reason = object.column_text(2).empty()
                                       ? "a managed attachment has expired"
                                       : object.column_text(2);
                } else {
                    Error media_error = media_file_available(path_, image.storage_ref,
                                                             image.mime_type,
                                                             object.column_int64(0), available);
                    if (!media_error.ok()) {
                        media_reason = media_error.message;
                    } else if (!available) {
                        media_reason = "a managed attachment file is missing";
                    }
                }
                if (media_reason.empty()) {
                    continue;
                }
                loaded.read_only = true;
                loaded.read_only_reason = media_reason;
                Statement mark(db_, path_);
                err = mark.prepare(
                    "UPDATE threads SET read_only = 1, read_only_reason = ?1 WHERE id = ?2;");
                if (!err.ok()) return err;
                err = BindChain(mark).text(1, media_reason).int64(2, thread_id)
                          .step_done("could not mark thread read-only");
                if (!err.ok()) return err;
                break;
            }
            for (const provider::TextAttachment& attachment : message.text_attachments) {
                if (loaded.read_only || attachment.storage_ref.empty()) {
                    continue;
                }
                Statement object(db_, path_);
                err = object.prepare(
                    "SELECT byte_size, deleted_at, delete_reason FROM media_objects WHERE sha256 = ?1;");
                if (!err.ok()) return err;
                err = BindChain(object).text(1, attachment.storage_ref).error();
                if (!err.ok()) return err;
                const int object_rc = object.step();
                bool available = false;
                std::string media_reason;
                if (object_rc == SQLITE_DONE) {
                    media_reason = "managed Markdown metadata is missing";
                } else if (object_rc != SQLITE_ROW) {
                    return sqlite_error(db_, path_, "could not inspect managed Markdown", object_rc);
                } else if (!object.column_text(1).empty()) {
                    media_reason = object.column_text(2).empty()
                                       ? "a managed Markdown attachment has expired"
                                       : object.column_text(2);
                } else {
                    Error media_error = media_file_available(path_, attachment.storage_ref,
                                                             "text/markdown",
                                                             object.column_int64(0), available);
                    if (!media_error.ok()) {
                        media_reason = media_error.message;
                    } else if (!available) {
                        media_reason = "a managed Markdown attachment file is missing";
                    }
                }
                if (media_reason.empty()) {
                    continue;
                }
                loaded.read_only = true;
                loaded.read_only_reason = media_reason;
                Statement mark(db_, path_);
                err = mark.prepare(
                    "UPDATE threads SET read_only = 1, read_only_reason = ?1 WHERE id = ?2;");
                if (!err.ok()) return err;
                err = BindChain(mark).text(1, media_reason).int64(2, thread_id)
                          .step_done("could not mark thread read-only");
                if (!err.ok()) return err;
                break;
            }
            if (loaded.read_only) {
                break;
            }
        }
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
        "SELECT id, name, created_at, modified_at, last_provider, last_base_url, last_model, message_count, "
        "read_only, read_only_reason "
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
        summary.read_only = stmt.column_int64(8) != 0;
        summary.read_only_reason = stmt.column_text(9);
        threads.push_back(std::move(summary));
    }
}

Error SqliteStore::import_media(const std::string& bytes,
                                const std::string& mime_type,
                                const std::string& display_name,
                                const std::string& source_ref,
                                provider::ImageInput& image) {
    if (db_ == nullptr) {
        return {ErrorCode::Internal, "SQLite database is not open"};
    }
    StoredMedia stored;
    Error err = store_media_bytes(path_, bytes, mime_type, stored);
    if (!err.ok()) {
        return err;
    }
    err = upsert_media_object(db_, path_, stored, "image", current_timestamp_utc());
    if (!err.ok()) {
        return err;
    }
    image.mime_type = mime_type;
    image.base64_data.clear();
    image.storage_ref = stored.sha256;
    image.display_name = display_name;
    image.source_ref = source_ref;
    image.byte_size = stored.byte_size;
    return ok_error();
}

Error SqliteStore::import_text_attachment(const std::string& markdown,
                                          size_t max_size_to_store_to_db,
                                          const std::string& display_name,
                                          const std::string& source_ref,
                                          provider::TextAttachment& attachment) {
    if (db_ == nullptr) {
        return {ErrorCode::Internal, "SQLite database is not open"};
    }
    provider::TextAttachment imported;
    imported.display_name = display_name;
    imported.source_ref = source_ref;
    imported.byte_size = static_cast<long long>(markdown.size());
    if (markdown.size() <= max_size_to_store_to_db) {
        imported.markdown_content = markdown;
        attachment = std::move(imported);
        return ok_error();
    }
    StoredMedia stored;
    Error err = store_media_bytes(path_, markdown, "text/markdown", stored);
    if (!err.ok()) {
        return err;
    }
    err = upsert_media_object(db_, path_, stored, "markdown", current_timestamp_utc());
    if (!err.ok()) {
        return err;
    }
    imported.storage_ref = stored.sha256;
    attachment = std::move(imported);
    return ok_error();
}

Error SqliteStore::cleanup_media(int expiration_days,
                                 long long protected_thread_id,
                                 const std::string& reason,
                                 MediaCleanupResult& result,
                                 runtime::CancellationToken cancellation) {
    result = {};
    if (db_ == nullptr) {
        return {ErrorCode::Internal, "SQLite database is not open"};
    }
    if (expiration_days <= 0) {
        return ok_error();
    }

    struct Candidate {
        std::string sha256;
    };
    std::vector<Candidate> candidates;
    Statement list(db_, path_);
    Error err = list.prepare(
        "SELECT mo.sha256 FROM media_objects mo "
        "WHERE mo.deleted_at IS NULL "
        "AND datetime(mo.last_used_at) < datetime('now', ?1) "
        "AND NOT EXISTS (SELECT 1 FROM attachments protected "
        "                WHERE protected.object_sha256 = mo.sha256 "
        "                AND protected.thread_id = ?2) "
        "ORDER BY mo.sha256;");
    if (!err.ok()) return err;
    const std::string modifier = "-" + std::to_string(expiration_days) + " days";
    err = BindChain(list).text(1, modifier).int64(2, protected_thread_id).error();
    if (!err.ok()) return err;
    while (true) {
        if (cancellation.cancelled()) {
            return {ErrorCode::Cancelled, "media cleanup cancelled while scanning objects"};
        }
        const int rc = list.step();
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            return sqlite_error(db_, path_, "could not list expired managed media", rc);
        }
        candidates.push_back({list.column_text(0)});
    }

    if (!candidates.empty()) {
        Transaction tx(db_, path_);
        err = tx.begin();
        if (!err.ok()) return err;
        const std::string timestamp = current_timestamp_utc();
        for (const Candidate& candidate : candidates) {
            if (cancellation.cancelled()) {
                return {ErrorCode::Cancelled, "media cleanup cancelled before expiration commit"};
            }
            Statement lock(db_, path_);
            err = lock.prepare(
                "UPDATE threads SET read_only = 1, read_only_reason = ?1 "
                "WHERE read_only = 0 AND deleted_at IS NULL AND id IN "
                "(SELECT thread_id FROM attachments WHERE object_sha256 = ?2);");
            if (!err.ok()) return err;
            err = BindChain(lock).text(1, reason).text(2, candidate.sha256)
                      .step_done("could not lock threads with expired media");
            if (!err.ok()) return err;
            result.threads_locked += sqlite3_changes(db_);

            Statement expire(db_, path_);
            err = expire.prepare(
                "UPDATE media_objects SET deleted_at = ?1, delete_reason = ?2 "
                "WHERE sha256 = ?3 AND deleted_at IS NULL;");
            if (!err.ok()) return err;
            err = BindChain(expire).text(1, timestamp).text(2, reason).text(3, candidate.sha256)
                      .step_done("could not expire managed media");
            if (!err.ok()) return err;
            result.objects_expired += sqlite3_changes(db_);
        }
        err = tx.commit();
        if (!err.ok()) return err;
    }

    Statement tombstones(db_, path_);
    err = tombstones.prepare(
        "SELECT sha256, byte_size, mime_type FROM media_objects WHERE deleted_at IS NOT NULL;");
    if (!err.ok()) return err;
    while (true) {
        if (cancellation.cancelled()) {
            return {ErrorCode::Cancelled,
                    "media cleanup cancelled; expired files will be retried later"};
        }
        const int rc = tombstones.step();
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            return sqlite_error(db_, path_, "could not list expired media files", rc);
        }
        bool removed = false;
        err = remove_media_file(path_, tombstones.column_text(0),
                                tombstones.column_text(2), removed);
        if (!err.ok()) {
            return err;
        }
        if (removed) {
            ++result.files_removed;
            result.bytes_reclaimed += tombstones.column_int64(1);
        }
    }
    return ok_error();
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

}  // namespace ainiux::chat
