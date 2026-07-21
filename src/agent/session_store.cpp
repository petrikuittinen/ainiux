#include "agent/session_store.hpp"

#include <sqlite3.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <utility>

namespace ainiux::agent {
namespace {
namespace fs = std::filesystem;

long long now_unix_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
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
    int step() { return sqlite3_step(statement_); }
    sqlite3_int64 column_int64(int column) const { return sqlite3_column_int64(statement_, column); }
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

AgentSessionRecord row_to_session(Statement& statement) {
    AgentSessionRecord record;
    record.id = statement.column_int64(0);
    record.created_at = statement.column_int64(1);
    record.updated_at = statement.column_int64(2);
    record.finished_at = statement.column_int64(3);
    record.status = statement.column_text(4);
    record.goal = statement.column_text(5);
    record.provider = statement.column_text(6);
    record.model = statement.column_text(7);
    record.api = statement.column_text(8);
    record.protocol = statement.column_text(9);
    record.workspace = statement.column_text(10);
    record.final_text = statement.column_text(11);
    record.error_code = statement.column_text(12);
    record.error_message = statement.column_text(13);
    record.turns = statement.column_int64(14);
    record.tool_calls = statement.column_int64(15);
    record.run_id = statement.column_text(16);
    return record;
}

const char* kSessionSelectColumns =
    "id, created_at, updated_at, finished_at, status, goal, provider, model, api, protocol, "
    "workspace, final_text, error_code, error_message, turns, tool_calls, run_id";

}  // namespace

std::string AgentSessionStore::database_path(const std::string& workspace) {
    return (fs::path(workspace) / ".ainiux" / "agent.sqlite").string();
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
                "could not create .ainiux for agent session DB: " + ec.message()};

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
    const char* sql =
        "CREATE TABLE IF NOT EXISTS schema_migrations("
        "  version INTEGER PRIMARY KEY,"
        "  applied_at INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS sessions("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  created_at INTEGER NOT NULL,"
        "  updated_at INTEGER NOT NULL,"
        "  finished_at INTEGER,"
        "  status TEXT NOT NULL,"
        "  goal TEXT NOT NULL,"
        "  provider TEXT,"
        "  model TEXT,"
        "  api TEXT,"
        "  protocol TEXT,"
        "  workspace TEXT NOT NULL,"
        "  final_text TEXT,"
        "  error_code TEXT,"
        "  error_message TEXT,"
        "  turns INTEGER NOT NULL DEFAULT 0,"
        "  tool_calls INTEGER NOT NULL DEFAULT 0,"
        "  run_id TEXT"
        ");"
        "CREATE TABLE IF NOT EXISTS messages("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
        "  seq INTEGER NOT NULL,"
        "  created_at INTEGER NOT NULL,"
        "  role TEXT NOT NULL,"
        "  content TEXT NOT NULL,"
        "  tool_call_id TEXT,"
        "  UNIQUE(session_id, seq)"
        ");"
        "CREATE TABLE IF NOT EXISTS tool_events("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
        "  seq INTEGER NOT NULL,"
        "  created_at INTEGER NOT NULL,"
        "  turn INTEGER NOT NULL,"
        "  call_id TEXT,"
        "  tool_name TEXT NOT NULL,"
        "  arguments TEXT NOT NULL,"
        "  result TEXT NOT NULL,"
        "  ok INTEGER NOT NULL DEFAULT 1,"
        "  UNIQUE(session_id, seq)"
        ");"
        "CREATE TABLE IF NOT EXISTS approvals("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,"
        "  created_at INTEGER NOT NULL,"
        "  action TEXT NOT NULL,"
        "  path_or_command TEXT,"
        "  decision TEXT NOT NULL,"
        "  rule_id TEXT,"
        "  detail TEXT"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_messages_session ON messages(session_id, seq);"
        "CREATE INDEX IF NOT EXISTS idx_tool_events_session ON tool_events(session_id, seq);"
        "CREATE INDEX IF NOT EXISTS idx_sessions_updated ON sessions(updated_at DESC);";

    const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &message);
    if (rc != SQLITE_OK) {
        const std::string detail = message == nullptr ? sqlite3_errmsg(db_) : message;
        sqlite3_free(message);
        return {ErrorCode::FileWrite, "could not create agent session schema: " + detail};
    }

    // Record schema version if missing.
    Statement check;
    Error error = check.prepare(db_, path_, "SELECT version FROM schema_migrations WHERE version=?");
    if (!error.ok()) return error;
    error = check.bind_int64(db_, path_, 1, kAgentSessionSchemaVersion);
    if (!error.ok()) return error;
    if (check.step() == SQLITE_ROW) return ok_error();

    Statement insert;
    error = insert.prepare(db_, path_,
                           "INSERT INTO schema_migrations(version, applied_at) VALUES(?, ?)");
    if (!error.ok()) return error;
    error = insert.bind_int64(db_, path_, 1, kAgentSessionSchemaVersion);
    if (!error.ok()) return error;
    error = insert.bind_int64(db_, path_, 2, now_unix_seconds());
    if (!error.ok()) return error;
    if (insert.step() != SQLITE_DONE) return sqlite_error(db_, "could not record schema version", path_);
    return ok_error();
}

Error AgentSessionStore::next_seq(long long session_id, long long& seq) {
    // Max of message and tool_event seq so ordering is global within a session.
    Statement statement;
    Error error = statement.prepare(
        db_, path_,
        "SELECT COALESCE(MAX(s), 0) FROM ("
        "  SELECT MAX(seq) AS s FROM messages WHERE session_id=?"
        "  UNION ALL"
        "  SELECT MAX(seq) AS s FROM tool_events WHERE session_id=?"
        ")");
    if (!error.ok()) return error;
    error = statement.bind_int64(db_, path_, 1, session_id);
    if (!error.ok()) return error;
    error = statement.bind_int64(db_, path_, 2, session_id);
    if (!error.ok()) return error;
    if (statement.step() != SQLITE_ROW) return sqlite_error(db_, "could not allocate seq", path_);
    seq = statement.column_int64(0) + 1;
    return ok_error();
}

Error AgentSessionStore::touch_session(long long session_id) {
    Statement statement;
    Error error =
        statement.prepare(db_, path_, "UPDATE sessions SET updated_at=? WHERE id=?");
    if (!error.ok()) return error;
    error = statement.bind_int64(db_, path_, 1, now_unix_seconds());
    if (!error.ok()) return error;
    error = statement.bind_int64(db_, path_, 2, session_id);
    if (!error.ok()) return error;
    if (statement.step() != SQLITE_DONE) return sqlite_error(db_, "could not touch session", path_);
    return ok_error();
}

Error AgentSessionStore::create_session(AgentSessionRecord& record) {
    if (db_ == nullptr) return {ErrorCode::Internal, "agent session DB is not open"};
    if (record.goal.empty()) return {ErrorCode::BadArgs, "session goal must not be empty"};
    if (record.workspace.empty()) record.workspace = workspace_;
    if (record.status.empty()) record.status = "running";

    const long long now = now_unix_seconds();
    record.created_at = now;
    record.updated_at = now;
    record.finished_at = 0;

    Statement statement;
    Error error = statement.prepare(
        db_, path_,
        "INSERT INTO sessions(created_at, updated_at, finished_at, status, goal, provider, model, "
        "api, protocol, workspace, final_text, error_code, error_message, turns, tool_calls, run_id) "
        "VALUES(?, ?, NULL, ?, ?, ?, ?, ?, ?, ?, NULL, NULL, NULL, 0, 0, ?)");
    if (!error.ok()) return error;
    int bind = 1;
    error = statement.bind_int64(db_, path_, bind++, now);
    if (!error.ok()) return error;
    error = statement.bind_int64(db_, path_, bind++, now);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, bind++, record.status);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, bind++, record.goal);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, bind++, record.provider);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, bind++, record.model);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, bind++, record.api);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, bind++, record.protocol);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, bind++, record.workspace);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, bind++, record.run_id);
    if (!error.ok()) return error;
    if (statement.step() != SQLITE_DONE) return sqlite_error(db_, "could not create session", path_);
    record.id = static_cast<long long>(sqlite3_last_insert_rowid(db_));
    return ok_error();
}

Error AgentSessionStore::append_message(long long session_id,
                                        const std::string& role,
                                        const std::string& content,
                                        const std::string& tool_call_id) {
    if (db_ == nullptr) return {ErrorCode::Internal, "agent session DB is not open"};
    if (session_id <= 0) return {ErrorCode::BadArgs, "session_id must be positive"};
    if (role.empty()) return {ErrorCode::BadArgs, "message role must not be empty"};

    long long seq = 0;
    Error error = next_seq(session_id, seq);
    if (!error.ok()) return error;

    Statement statement;
    error = statement.prepare(
        db_, path_,
        "INSERT INTO messages(session_id, seq, created_at, role, content, tool_call_id) "
        "VALUES(?, ?, ?, ?, ?, ?)");
    if (!error.ok()) return error;
    error = statement.bind_int64(db_, path_, 1, session_id);
    if (!error.ok()) return error;
    error = statement.bind_int64(db_, path_, 2, seq);
    if (!error.ok()) return error;
    error = statement.bind_int64(db_, path_, 3, now_unix_seconds());
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, 4, role);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, 5, content);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, 6, tool_call_id);
    if (!error.ok()) return error;
    if (statement.step() != SQLITE_DONE) return sqlite_error(db_, "could not append message", path_);
    return touch_session(session_id);
}

Error AgentSessionStore::append_tool_event(long long session_id,
                                           long long turn,
                                           const std::string& call_id,
                                           const std::string& tool_name,
                                           const std::string& arguments,
                                           const std::string& result,
                                           bool ok) {
    if (db_ == nullptr) return {ErrorCode::Internal, "agent session DB is not open"};
    if (session_id <= 0) return {ErrorCode::BadArgs, "session_id must be positive"};
    if (tool_name.empty()) return {ErrorCode::BadArgs, "tool_name must not be empty"};

    long long seq = 0;
    Error error = next_seq(session_id, seq);
    if (!error.ok()) return error;

    Statement statement;
    error = statement.prepare(
        db_, path_,
        "INSERT INTO tool_events(session_id, seq, created_at, turn, call_id, tool_name, "
        "arguments, result, ok) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)");
    if (!error.ok()) return error;
    error = statement.bind_int64(db_, path_, 1, session_id);
    if (!error.ok()) return error;
    error = statement.bind_int64(db_, path_, 2, seq);
    if (!error.ok()) return error;
    error = statement.bind_int64(db_, path_, 3, now_unix_seconds());
    if (!error.ok()) return error;
    error = statement.bind_int64(db_, path_, 4, turn);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, 5, call_id);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, 6, tool_name);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, 7, arguments);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, 8, result);
    if (!error.ok()) return error;
    error = statement.bind_int64(db_, path_, 9, ok ? 1 : 0);
    if (!error.ok()) return error;
    if (statement.step() != SQLITE_DONE)
        return sqlite_error(db_, "could not append tool event", path_);
    return touch_session(session_id);
}

Error AgentSessionStore::finish_session(long long session_id,
                                        const std::string& status,
                                        const std::string& final_text,
                                        const std::string& error_code,
                                        const std::string& error_message,
                                        long long turns,
                                        long long tool_calls) {
    if (db_ == nullptr) return {ErrorCode::Internal, "agent session DB is not open"};
    if (session_id <= 0) return {ErrorCode::BadArgs, "session_id must be positive"};
    if (status.empty()) return {ErrorCode::BadArgs, "session status must not be empty"};

    const long long now = now_unix_seconds();
    Statement statement;
    Error error = statement.prepare(
        db_, path_,
        "UPDATE sessions SET updated_at=?, finished_at=?, status=?, final_text=?, "
        "error_code=?, error_message=?, turns=?, tool_calls=? WHERE id=?");
    if (!error.ok()) return error;
    error = statement.bind_int64(db_, path_, 1, now);
    if (!error.ok()) return error;
    error = statement.bind_int64(db_, path_, 2, now);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, 3, status);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, 4, final_text);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, 5, error_code);
    if (!error.ok()) return error;
    error = statement.bind_text(db_, path_, 6, error_message);
    if (!error.ok()) return error;
    error = statement.bind_int64(db_, path_, 7, turns);
    if (!error.ok()) return error;
    error = statement.bind_int64(db_, path_, 8, tool_calls);
    if (!error.ok()) return error;
    error = statement.bind_int64(db_, path_, 9, session_id);
    if (!error.ok()) return error;
    if (statement.step() != SQLITE_DONE) return sqlite_error(db_, "could not finish session", path_);
    if (sqlite3_changes(db_) == 0)
        return {ErrorCode::FileRead, "agent session not found: " + std::to_string(session_id)};
    return ok_error();
}

Error AgentSessionStore::list_sessions(std::vector<AgentSessionRecord>& sessions,
                                       int limit) const {
    sessions.clear();
    if (db_ == nullptr) return {ErrorCode::Internal, "agent session DB is not open"};
    if (limit <= 0) limit = 50;
    if (limit > 500) limit = 500;

    Statement statement;
    Error error = statement.prepare(
        db_, path_,
        (std::string("SELECT ") + kSessionSelectColumns +
         " FROM sessions ORDER BY updated_at DESC, id DESC LIMIT ?")
            .c_str());
    if (!error.ok()) return error;
    error = statement.bind_int64(db_, path_, 1, limit);
    if (!error.ok()) return error;
    while (statement.step() == SQLITE_ROW) sessions.push_back(row_to_session(statement));
    return ok_error();
}

Error AgentSessionStore::load_session(long long session_id,
                                      AgentSessionRecord& session,
                                      std::vector<AgentMessageRecord>& messages,
                                      std::vector<AgentToolEventRecord>& tool_events) const {
    session = AgentSessionRecord{};
    messages.clear();
    tool_events.clear();
    if (db_ == nullptr) return {ErrorCode::Internal, "agent session DB is not open"};
    if (session_id <= 0) return {ErrorCode::BadArgs, "session_id must be positive"};

    Statement session_stmt;
    Error error = session_stmt.prepare(
        db_, path_,
        (std::string("SELECT ") + kSessionSelectColumns + " FROM sessions WHERE id=?")
            .c_str());
    if (!error.ok()) return error;
    error = session_stmt.bind_int64(db_, path_, 1, session_id);
    if (!error.ok()) return error;
    if (session_stmt.step() != SQLITE_ROW)
        return {ErrorCode::FileRead, "agent session not found: " + std::to_string(session_id)};
    session = row_to_session(session_stmt);

    Statement msg_stmt;
    error = msg_stmt.prepare(
        db_, path_,
        "SELECT id, session_id, seq, created_at, role, content, tool_call_id "
        "FROM messages WHERE session_id=? ORDER BY seq ASC");
    if (!error.ok()) return error;
    error = msg_stmt.bind_int64(db_, path_, 1, session_id);
    if (!error.ok()) return error;
    while (msg_stmt.step() == SQLITE_ROW) {
        AgentMessageRecord message;
        message.id = msg_stmt.column_int64(0);
        message.session_id = msg_stmt.column_int64(1);
        message.seq = msg_stmt.column_int64(2);
        message.created_at = msg_stmt.column_int64(3);
        message.role = msg_stmt.column_text(4);
        message.content = msg_stmt.column_text(5);
        message.tool_call_id = msg_stmt.column_text(6);
        messages.push_back(std::move(message));
    }

    Statement tool_stmt;
    error = tool_stmt.prepare(
        db_, path_,
        "SELECT id, session_id, seq, created_at, turn, call_id, tool_name, arguments, result, ok "
        "FROM tool_events WHERE session_id=? ORDER BY seq ASC");
    if (!error.ok()) return error;
    error = tool_stmt.bind_int64(db_, path_, 1, session_id);
    if (!error.ok()) return error;
    while (tool_stmt.step() == SQLITE_ROW) {
        AgentToolEventRecord event;
        event.id = tool_stmt.column_int64(0);
        event.session_id = tool_stmt.column_int64(1);
        event.seq = tool_stmt.column_int64(2);
        event.created_at = tool_stmt.column_int64(3);
        event.turn = tool_stmt.column_int64(4);
        event.call_id = tool_stmt.column_text(5);
        event.tool_name = tool_stmt.column_text(6);
        event.arguments = tool_stmt.column_text(7);
        event.result = tool_stmt.column_text(8);
        event.ok = tool_stmt.column_int64(9) != 0;
        tool_events.push_back(std::move(event));
    }
    return ok_error();
}

}  // namespace ainiux::agent
