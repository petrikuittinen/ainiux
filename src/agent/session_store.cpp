#include "agent/session_store.hpp"

#include <sqlite3.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <utility>

#include "agent/project_paths.hpp"

namespace ainiux::agent {
namespace {
namespace fs = std::filesystem;

long long now_unix_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Message/tool event timestamps use milliseconds (INTEGER). Older rows may still
// hold Unix seconds; readers normalize via normalize_timestamp_ms().
long long now_unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

Error sqlite_error(sqlite3* db, const std::string& action, const std::string& path) {
    return {ErrorCode::FileWrite,
            action + " for agent session DB " + path + ": " +
                (db == nullptr ? std::string("unknown SQLite error") : sqlite3_errmsg(db))};
}

class Statement {
   public:
    Statement() = default;
    ~Statement() {
        if (statement_ != nullptr) sqlite3_finalize(statement_);
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    Error prepare(sqlite3* db, const std::string& path, const char* sql) {
        const int rc = sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr);
        return rc == SQLITE_OK ? ok_error() : sqlite_error(db, "could not prepare query", path);
    }
    Error bind_text(sqlite3* db, const std::string& path, int index, const std::string& value) {
        return sqlite3_bind_text(statement_, index, value.c_str(), static_cast<int>(value.size()),
                                 SQLITE_TRANSIENT) == SQLITE_OK
                   ? ok_error()
                   : sqlite_error(db, "could not bind text", path);
    }
    Error bind_int64(sqlite3* db, const std::string& path, int index, sqlite3_int64 value) {
        return sqlite3_bind_int64(statement_, index, value) == SQLITE_OK
                   ? ok_error()
                   : sqlite_error(db, "could not bind integer", path);
    }
    Error bind_int(sqlite3* db, const std::string& path, int index, int value) {
        return sqlite3_bind_int(statement_, index, value) == SQLITE_OK
                   ? ok_error()
                   : sqlite_error(db, "could not bind integer", path);
    }
    int step() { return sqlite3_step(statement_); }
    sqlite3_int64 column_int64(int column) const { return sqlite3_column_int64(statement_, column); }
    int column_int(int column) const { return sqlite3_column_int(statement_, column); }
    std::string column_text(int column) const {
        const unsigned char* value = sqlite3_column_text(statement_, column);
        return value == nullptr ? std::string{} : reinterpret_cast<const char*>(value);
    }
    void reset() {
        sqlite3_reset(statement_);
        sqlite3_clear_bindings(statement_);
    }

   private:
    sqlite3_stmt* statement_ = nullptr;
};

}  // namespace

std::string AgentSessionStore::database_path(const std::string& workspace) {
    return (fs::path(workspace) / kProjectStateDirName / "agent.sqlite").string();
}

AgentSessionStore::~AgentSessionStore() { close(); }

AgentSessionStore::AgentSessionStore(AgentSessionStore&& other) noexcept {
    db_ = other.db_;
    path_ = std::move(other.path_);
    workspace_ = std::move(other.workspace_);
    other.db_ = nullptr;
}

AgentSessionStore& AgentSessionStore::operator=(AgentSessionStore&& other) noexcept {
    if (this != &other) {
        close();
        db_ = other.db_;
        path_ = std::move(other.path_);
        workspace_ = std::move(other.workspace_);
        other.db_ = nullptr;
    }
    return *this;
}

void AgentSessionStore::close() {
    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    path_.clear();
    workspace_.clear();
}

Error AgentSessionStore::open(const std::string& workspace) {
    close();
    if (workspace.empty()) return {ErrorCode::BadArgs, "workspace is required for agent.sqlite"};
    workspace_ = workspace;
    path_ = database_path(workspace);

    std::error_code ec;
    fs::create_directories(fs::path(path_).parent_path(), ec);
    if (ec)
        return {ErrorCode::FileWrite,
                std::string("could not create ") + kProjectStateDirName +
                    " for agent session DB: " + ec.message()};

    const int rc =
        sqlite3_open_v2(path_.c_str(), &db_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        Error error = sqlite_error(db_, "could not open", path_);
        if (db_ != nullptr) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        return error;
    }
    sqlite3_busy_timeout(db_, 1000);

    char* message = nullptr;
    auto exec = [&](const char* sql) -> Error {
        const int exec_rc = sqlite3_exec(db_, sql, nullptr, nullptr, &message);
        if (exec_rc == SQLITE_OK) return ok_error();
        const std::string detail = message == nullptr ? sqlite3_errmsg(db_) : message;
        sqlite3_free(message);
        message = nullptr;
        return {ErrorCode::FileWrite, "could not configure agent session DB " + path_ + ": " + detail};
    };

    Error error = exec("PRAGMA journal_mode=WAL");
    if (!error.ok()) {
        close();
        return error;
    }
    error = exec("PRAGMA synchronous=NORMAL");
    if (!error.ok()) {
        close();
        return error;
    }
    error = exec("PRAGMA foreign_keys=ON");
    if (!error.ok()) {
        close();
        return error;
    }

    error = ensure_schema();
    if (!error.ok()) {
        close();
        return error;
    }
    return ok_error();
}

Error AgentSessionStore::ensure_schema() {
    char* message = nullptr;
    auto exec = [&](const char* sql) -> Error {
        const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &message);
        if (rc == SQLITE_OK) return ok_error();
        const std::string detail = message == nullptr ? sqlite3_errmsg(db_) : message;
        sqlite3_free(message);
        message = nullptr;
        return {ErrorCode::FileWrite, "agent schema: " + detail};
    };

    Error error = exec(
        "CREATE TABLE IF NOT EXISTS schema_migrations("
        "  version INTEGER PRIMARY KEY,"
        "  applied_at INTEGER NOT NULL"
        ");");
    if (!error.ok()) return error;

    int applied = 0;
    {
        Statement check;
        error = check.prepare(db_, path_, "SELECT MAX(version) FROM schema_migrations");
        if (!error.ok()) return error;
        if (check.step() == SQLITE_ROW) applied = static_cast<int>(check.column_int64(0));
    }

    if (applied > 0 && applied != kAgentSessionSchemaVersion) {
        // Hard reset: schema is not migrated across agent reworks.
        error = exec(
            "DROP TABLE IF EXISTS tool_events;"
            "DROP TABLE IF EXISTS messages;"
            "DROP TABLE IF EXISTS approvals;"
            "DROP TABLE IF EXISTS sessions;"
            "DROP TABLE IF EXISTS project;"
            "DELETE FROM schema_migrations;");
        if (!error.ok()) return error;
        applied = 0;
    }

    if (applied == kAgentSessionSchemaVersion) {
        // Soft extensions preserve existing v2 project transcripts.
        error = ensure_approvals_table();
        if (!error.ok()) return error;
        return ensure_project_settings_columns();
    }

    error = exec(
        "CREATE TABLE IF NOT EXISTS project("
        "  id INTEGER PRIMARY KEY CHECK (id = 1),"
        "  created_at INTEGER NOT NULL,"
        "  updated_at INTEGER NOT NULL,"
        "  status TEXT NOT NULL,"
        "  provider TEXT,"
        "  model TEXT,"
        "  api TEXT,"
        "  protocol TEXT,"
        "  base_url TEXT,"
        "  settings_json TEXT NOT NULL DEFAULT '{}',"
        "  workspace TEXT NOT NULL,"
        "  summary_text TEXT,"
        "  turns INTEGER NOT NULL DEFAULT 0,"
        "  tool_calls INTEGER NOT NULL DEFAULT 0"
        ");"
        "CREATE TABLE IF NOT EXISTS messages("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  seq INTEGER NOT NULL UNIQUE,"
        "  created_at INTEGER NOT NULL,"
        "  role TEXT NOT NULL,"
        "  content TEXT NOT NULL,"
        "  tool_name TEXT,"
        "  tool_ok INTEGER NOT NULL DEFAULT 1,"
        "  args_preview TEXT"
        ");"
        "CREATE TABLE IF NOT EXISTS tool_events("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  seq INTEGER NOT NULL UNIQUE,"
        "  created_at INTEGER NOT NULL,"
        "  turn INTEGER NOT NULL,"
        "  call_id TEXT,"
        "  tool_name TEXT NOT NULL,"
        "  arguments TEXT NOT NULL,"
        "  result TEXT NOT NULL,"
        "  ok INTEGER NOT NULL DEFAULT 1"
        ");"
        "CREATE TABLE IF NOT EXISTS approvals("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  created_at INTEGER NOT NULL,"
        "  tool_name TEXT NOT NULL,"
        "  command_preview TEXT NOT NULL,"
        "  rule_id TEXT,"
        "  decision TEXT NOT NULL,"
        "  source TEXT NOT NULL,"
        "  message TEXT"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_agent_messages_seq ON messages(seq);"
        "CREATE INDEX IF NOT EXISTS idx_agent_tools_seq ON tool_events(seq);"
        "CREATE INDEX IF NOT EXISTS idx_agent_approvals_created ON approvals(created_at);");
    if (!error.ok()) return error;

    Statement insert;
    error = insert.prepare(db_, path_,
                           "INSERT INTO schema_migrations(version, applied_at) VALUES(?, ?)");
    if (!error.ok()) return error;
    error = insert.bind_int64(db_, path_, 1, kAgentSessionSchemaVersion);
    if (!error.ok()) return error;
    error = insert.bind_int64(db_, path_, 2, now_unix_seconds());
    if (!error.ok()) return error;
    if (insert.step() != SQLITE_DONE) return sqlite_error(db_, "could not record schema version", path_);
    return ensure_project_settings_columns();
}

Error AgentSessionStore::ensure_approvals_table() {
    // Soft-add for DBs created before the approvals slice (schema version still 2).
    char* message = nullptr;
    const int rc = sqlite3_exec(
        db_,
        "CREATE TABLE IF NOT EXISTS approvals("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  created_at INTEGER NOT NULL,"
        "  tool_name TEXT NOT NULL,"
        "  command_preview TEXT NOT NULL,"
        "  rule_id TEXT,"
        "  decision TEXT NOT NULL,"
        "  source TEXT NOT NULL,"
        "  message TEXT"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_agent_approvals_created ON approvals(created_at);",
        nullptr, nullptr, &message);
    if (rc == SQLITE_OK) return ok_error();
    const std::string detail = message == nullptr ? sqlite3_errmsg(db_) : message;
    sqlite3_free(message);
    return {ErrorCode::FileWrite, "agent approvals schema: " + detail};
}

Error AgentSessionStore::ensure_project_settings_columns() {
    bool have_base_url = false;
    bool have_settings_json = false;
    Error error;
    {
        Statement columns;
        error = columns.prepare(db_, path_, "PRAGMA table_info(project)");
        if (!error.ok()) return error;
        while (columns.step() == SQLITE_ROW) {
            const std::string name = columns.column_text(1);
            if (name == "base_url") have_base_url = true;
            if (name == "settings_json") have_settings_json = true;
        }
    }

    auto add_column = [&](const char* sql) -> Error {
        char* message = nullptr;
        const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &message);
        if (rc == SQLITE_OK) return ok_error();
        const std::string detail = message == nullptr ? sqlite3_errmsg(db_) : message;
        sqlite3_free(message);
        return {ErrorCode::FileWrite, "agent project settings schema: " + detail};
    };
    if (!have_base_url) {
        error = add_column("ALTER TABLE project ADD COLUMN base_url TEXT");
        if (!error.ok()) return error;
    }
    if (!have_settings_json) {
        error = add_column(
            "ALTER TABLE project ADD COLUMN settings_json TEXT NOT NULL DEFAULT '{}'");
        if (!error.ok()) return error;
    }
    return ok_error();
}

Error AgentSessionStore::next_seq(long long& seq) {
    Statement statement;
    Error error = statement.prepare(
        db_, path_,
        "SELECT COALESCE(MAX(seq), 0) FROM ("
        "  SELECT seq FROM messages UNION ALL SELECT seq FROM tool_events"
        ")");
    if (!error.ok()) return error;
    if (statement.step() != SQLITE_ROW) return sqlite_error(db_, "could not allocate seq", path_);
    seq = statement.column_int64(0) + 1;
    return ok_error();
}

Error AgentSessionStore::touch() {
    Statement statement;
    Error error =
        statement.prepare(db_, path_, "UPDATE project SET updated_at=? WHERE id=1");
    if (!error.ok()) return error;
    error = statement.bind_int64(db_, path_, 1, now_unix_seconds());
    if (!error.ok()) return error;
    if (statement.step() != SQLITE_DONE) return sqlite_error(db_, "could not touch project", path_);
    return ok_error();
}

Error AgentSessionStore::open_project(AgentProjectRecord& record) {
    if (!is_open()) return {ErrorCode::Internal, "agent session DB is not open"};
    Statement select;
    Error error = select.prepare(
        db_, path_,
        "SELECT id, created_at, updated_at, status, provider, model, api, protocol, workspace, "
        "summary_text, turns, tool_calls, base_url, settings_json FROM project WHERE id=1");
    if (!error.ok()) return error;
    if (select.step() == SQLITE_ROW) {
        record.id = select.column_int64(0);
        record.created_at = select.column_int64(1);
        record.updated_at = select.column_int64(2);
        record.status = select.column_text(3);
        record.provider = select.column_text(4);
        record.model = select.column_text(5);
        record.api = select.column_text(6);
        record.protocol = select.column_text(7);
        record.workspace = select.column_text(8);
        record.summary_text = select.column_text(9);
        record.turns = select.column_int64(10);
        record.tool_calls = select.column_int64(11);
        record.base_url = select.column_text(12);
        record.settings_json = select.column_text(13);
        return ok_error();
    }

    const long long now = now_unix_seconds();
    Statement insert;
    error = insert.prepare(
        db_, path_,
        "INSERT INTO project(id, created_at, updated_at, status, provider, model, api, protocol, "
        "workspace, summary_text, turns, tool_calls, base_url, settings_json) "
        "VALUES(1,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    if (!error.ok()) return error;
    error = insert.bind_int64(db_, path_, 1, now);
    if (!error.ok()) return error;
    error = insert.bind_int64(db_, path_, 2, now);
    if (!error.ok()) return error;
    error = insert.bind_text(db_, path_, 3, record.status.empty() ? "idle" : record.status);
    if (!error.ok()) return error;
    error = insert.bind_text(db_, path_, 4, record.provider);
    if (!error.ok()) return error;
    error = insert.bind_text(db_, path_, 5, record.model);
    if (!error.ok()) return error;
    error = insert.bind_text(db_, path_, 6, record.api);
    if (!error.ok()) return error;
    error = insert.bind_text(db_, path_, 7, record.protocol);
    if (!error.ok()) return error;
    error = insert.bind_text(db_, path_, 8,
                             record.workspace.empty() ? workspace_ : record.workspace);
    if (!error.ok()) return error;
    error = insert.bind_text(db_, path_, 9, record.summary_text);
    if (!error.ok()) return error;
    error = insert.bind_int64(db_, path_, 10, record.turns);
    if (!error.ok()) return error;
    error = insert.bind_int64(db_, path_, 11, record.tool_calls);
    if (!error.ok()) return error;
    error = insert.bind_text(db_, path_, 12, record.base_url);
    if (!error.ok()) return error;
    error = insert.bind_text(db_, path_, 13,
                             record.settings_json.empty() ? "{}" : record.settings_json);
    if (!error.ok()) return error;
    if (insert.step() != SQLITE_DONE) return sqlite_error(db_, "could not create project row", path_);

    record.id = 1;
    record.created_at = now;
    record.updated_at = now;
    if (record.status.empty()) record.status = "idle";
    if (record.workspace.empty()) record.workspace = workspace_;
    return ok_error();
}

Error AgentSessionStore::update_project_meta(const AgentProjectRecord& record) {
    Statement statement;
    Error error = statement.prepare(
        db_, path_,
        "UPDATE project SET updated_at=?, status=?, provider=?, model=?, api=?, protocol=?, "
        "workspace=?, summary_text=?, turns=?, tool_calls=?, base_url=?, settings_json=? "
        "WHERE id=1");
    if (!error.ok()) return error;
    error = statement.bind_int64(db_, path_, 1, now_unix_seconds());
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, 2, record.status);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, 3, record.provider);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, 4, record.model);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, 5, record.api);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, 6, record.protocol);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, 7, record.workspace);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, 8, record.summary_text);
    if (!error.ok()) return error;
    error = statement.bind_int64(db_, path_, 9, record.turns);
    if (!error.ok()) return error;
    error = statement.bind_int64(db_, path_, 10, record.tool_calls);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, 11, record.base_url);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, 12,
                                record.settings_json.empty() ? "{}" : record.settings_json);
    if (!error.ok()) return error;
    if (statement.step() != SQLITE_DONE)
        return sqlite_error(db_, "could not update project meta", path_);
    return ok_error();
}

Error AgentSessionStore::append_message(const std::string& role,
                                        const std::string& content,
                                        const std::string& tool_name,
                                        bool tool_ok,
                                        const std::string& args_preview) {
    long long seq = 0;
    Error error = next_seq(seq);
    if (!error.ok()) return error;
    Statement statement;
    error = statement.prepare(
        db_, path_,
        "INSERT INTO messages(seq, created_at, role, content, tool_name, tool_ok, args_preview) "
        "VALUES(?,?,?,?,?,?,?)");
    if (!error.ok()) return error;
    error = statement.bind_int64(db_, path_, 1, seq);
    if (!error.ok()) return error;
    error = statement.bind_int64(db_, path_, 2, now_unix_ms());
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, 3, role);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, 4, content);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, 5, tool_name);
    if (!error.ok()) return error;
    error = statement.bind_int(db_, path_, 6, tool_ok ? 1 : 0);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, 7, args_preview);
    if (!error.ok()) return error;
    if (statement.step() != SQLITE_DONE) return sqlite_error(db_, "could not append message", path_);
    return touch();
}

Error AgentSessionStore::append_message(long long /*session_id*/,
                                        const std::string& role,
                                        const std::string& content,
                                        const std::string& /*tool_call_id*/) {
    return append_message(role, content, {}, true, {});
}

Error AgentSessionStore::append_tool_event(long long /*session_id*/,
                                           long long turn,
                                           const std::string& call_id,
                                           const std::string& tool_name,
                                           const std::string& arguments,
                                           const std::string& result,
                                           bool ok) {
    long long seq = 0;
    Error error = next_seq(seq);
    if (!error.ok()) return error;
    Statement statement;
    error = statement.prepare(
        db_, path_,
        "INSERT INTO tool_events(seq, created_at, turn, call_id, tool_name, arguments, result, ok) "
        "VALUES(?,?,?,?,?,?,?,?)");
    if (!error.ok()) return error;
    error = statement.bind_int64(db_, path_, 1, seq);
    if (!error.ok()) return error;
    error = statement.bind_int64(db_, path_, 2, now_unix_ms());
    if (!error.ok()) return error;
    error = statement.bind_int64(db_, path_, 3, turn);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, 4, call_id);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, 5, tool_name);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, 6, arguments);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, 7, result);
    if (!error.ok()) return error;
    error = statement.bind_int(db_, path_, 8, ok ? 1 : 0);
    if (!error.ok()) return error;
    if (statement.step() != SQLITE_DONE)
        return sqlite_error(db_, "could not append tool event", path_);
    return touch();
}

Error AgentSessionStore::load_messages(std::vector<AgentMessageRecord>& messages, int limit) const {
    messages.clear();
    std::string sql =
        "SELECT id, seq, created_at, role, content, tool_name, tool_ok, args_preview "
        "FROM messages ORDER BY seq ASC";
    if (limit > 0) sql += " LIMIT " + std::to_string(limit);
    Statement statement;
    Error error = statement.prepare(db_, path_, sql.c_str());
    if (!error.ok()) return error;
    while (statement.step() == SQLITE_ROW) {
        AgentMessageRecord row;
        row.id = statement.column_int64(0);
        row.seq = statement.column_int64(1);
        row.created_at = statement.column_int64(2);
        row.role = statement.column_text(3);
        row.content = statement.column_text(4);
        row.tool_name = statement.column_text(5);
        row.tool_ok = statement.column_int(6) != 0;
        row.args_preview = statement.column_text(7);
        messages.push_back(std::move(row));
    }
    return ok_error();
}

Error AgentSessionStore::compact_with_summary(const std::string& summary_text, int keep_recent) {
    (void)keep_recent;
    if (summary_text.empty())
        return {ErrorCode::BadArgs, "agent compaction summary must not be empty"};

    char* message = nullptr;
    if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, &message) != SQLITE_OK) {
        const std::string detail = message == nullptr ? sqlite3_errmsg(db_) : message;
        sqlite3_free(message);
        return {ErrorCode::FileWrite, "could not begin compact: " + detail};
    }

    auto rollback = [&]() {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    };

    Error error = append_message("summary", summary_text);
    if (!error.ok()) {
        rollback();
        return error;
    }

    AgentProjectRecord project;
    error = open_project(project);
    if (!error.ok()) {
        rollback();
        return error;
    }
    project.summary_text = summary_text;
    error = update_project_meta(project);
    if (!error.ok()) {
        rollback();
        return error;
    }

    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, &message) != SQLITE_OK) {
        const std::string detail = message == nullptr ? sqlite3_errmsg(db_) : message;
        sqlite3_free(message);
        rollback();
        return {ErrorCode::FileWrite, "could not commit compact: " + detail};
    }
    return ok_error();
}

Error AgentSessionStore::set_status(const std::string& status) {
    AgentProjectRecord project;
    Error error = open_project(project);
    if (!error.ok()) return error;
    project.status = status;
    return update_project_meta(project);
}

Error AgentSessionStore::bump_counters(long long turns_delta, long long tool_calls_delta) {
    AgentProjectRecord project;
    Error error = open_project(project);
    if (!error.ok()) return error;
    project.turns += turns_delta;
    project.tool_calls += tool_calls_delta;
    return update_project_meta(project);
}

Error AgentSessionStore::finish_session(long long /*session_id*/,
                                        const std::string& status,
                                        const std::string& final_text,
                                        const std::string& /*error_code*/,
                                        const std::string& error_message,
                                        long long turns,
                                        long long tool_calls) {
    AgentProjectRecord project;
    Error error = open_project(project);
    if (!error.ok()) return error;
    project.status = status == "success" ? "idle" : status;
    project.turns = turns;
    project.tool_calls = tool_calls;
    if (!final_text.empty()) {
        // Keep last final text available as a lightweight summary hint.
        if (project.summary_text.empty()) project.summary_text = final_text;
    }
    if (!error_message.empty() && status != "success") {
        (void)append_message("notice", error_message);
    }
    return update_project_meta(project);
}

Error AgentSessionStore::list_sessions(std::vector<AgentSessionRecord>& sessions, int /*limit*/) const {
    sessions.clear();
    AgentProjectRecord project;
    // load without mutating
    Statement select;
    Error error = select.prepare(
        db_, path_,
        "SELECT id, created_at, updated_at, status, provider, model, api, protocol, workspace, "
        "summary_text, turns, tool_calls, base_url, settings_json FROM project WHERE id=1");
    if (!error.ok()) return error;
    if (select.step() != SQLITE_ROW) return ok_error();
    project.id = select.column_int64(0);
    project.created_at = select.column_int64(1);
    project.updated_at = select.column_int64(2);
    project.status = select.column_text(3);
    project.provider = select.column_text(4);
    project.model = select.column_text(5);
    project.api = select.column_text(6);
    project.protocol = select.column_text(7);
    project.workspace = select.column_text(8);
    project.summary_text = select.column_text(9);
    project.turns = select.column_int64(10);
    project.tool_calls = select.column_int64(11);
    project.base_url = select.column_text(12);
    project.settings_json = select.column_text(13);
    sessions.push_back(std::move(project));
    return ok_error();
}

Error AgentSessionStore::load_session(long long /*session_id*/,
                                      AgentSessionRecord& session,
                                      std::vector<AgentMessageRecord>& messages,
                                      std::vector<AgentToolEventRecord>& tool_events) const {
    tool_events.clear();
    std::vector<AgentSessionRecord> listed;
    Error error = list_sessions(listed, 1);
    if (!error.ok()) return error;
    if (listed.empty()) return {ErrorCode::FileRead, "no agent project session"};
    session = listed.front();
    error = load_messages(messages, 0);
    if (!error.ok()) return error;

    Statement tools;
    error = tools.prepare(
        db_, path_,
        "SELECT id, seq, created_at, turn, call_id, tool_name, arguments, result, ok "
        "FROM tool_events ORDER BY seq ASC");
    if (!error.ok()) return error;
    while (tools.step() == SQLITE_ROW) {
        AgentToolEventRecord row;
        row.id = tools.column_int64(0);
        row.seq = tools.column_int64(1);
        row.created_at = tools.column_int64(2);
        row.turn = tools.column_int64(3);
        row.call_id = tools.column_text(4);
        row.tool_name = tools.column_text(5);
        row.arguments = tools.column_text(6);
        row.result = tools.column_text(7);
        row.ok = tools.column_int(8) != 0;
        tool_events.push_back(std::move(row));
    }
    return ok_error();
}

Error AgentSessionStore::record_approval(const AgentApprovalRecord& record) {
    if (!is_open()) return {ErrorCode::Internal, "agent session DB is not open"};
    Error error = ensure_approvals_table();
    if (!error.ok()) return error;
    Statement statement;
    error = statement.prepare(
        db_, path_,
        "INSERT INTO approvals(created_at, tool_name, command_preview, rule_id, decision, "
        "source, message) VALUES(?, ?, ?, ?, ?, ?, ?)");
    if (!error.ok()) return error;
    const long long created =
        record.created_at > 0 ? record.created_at : now_unix_ms();
    error = statement.bind_int64(db_, path_, 1, created);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, 2, record.tool_name);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, 3, record.command_preview);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, 4, record.rule_id);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, 5, record.decision);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, 6, record.source);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, 7, record.message);
    if (!error.ok()) return error;
    if (statement.step() != SQLITE_DONE)
        return sqlite_error(db_, "could not record approval", path_);
    return touch();
}

Error AgentSessionStore::load_approvals(std::vector<AgentApprovalRecord>& approvals,
                                        int limit) const {
    approvals.clear();
    if (!is_open()) return {ErrorCode::Internal, "agent session DB is not open"};
    // Best-effort if table missing on very old handles.
    std::string sql =
        "SELECT id, created_at, tool_name, command_preview, rule_id, decision, source, message "
        "FROM approvals ORDER BY id ASC";
    if (limit > 0) sql += " LIMIT " + std::to_string(limit);
    Statement statement;
    Error error = statement.prepare(db_, path_, sql.c_str());
    if (!error.ok()) {
        // Table may not exist yet on a concurrent race; treat as empty.
        if (error.message.find("no such table") != std::string::npos) return ok_error();
        return error;
    }
    while (statement.step() == SQLITE_ROW) {
        AgentApprovalRecord row;
        row.id = statement.column_int64(0);
        row.created_at = statement.column_int64(1);
        row.tool_name = statement.column_text(2);
        row.command_preview = statement.column_text(3);
        row.rule_id = statement.column_text(4);
        row.decision = statement.column_text(5);
        row.source = statement.column_text(6);
        row.message = statement.column_text(7);
        approvals.push_back(std::move(row));
    }
    return ok_error();
}

}  // namespace ainiux::agent
