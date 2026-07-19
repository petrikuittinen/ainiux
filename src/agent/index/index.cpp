#include "agent/index/index.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <deque>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <thread>
#include <utility>

#include <sys/stat.h>

#include "html/html.hpp"

namespace ainiux::agent::index {
namespace {

namespace fs = std::filesystem;

struct FileRecord {
    sqlite3_int64 id = 0;
    std::string path;
    Language language = Language::Python;
    std::uintmax_t size = 0;
    long long mtime_ns = 0;
    std::string content_hash;
    std::string status;
    std::string error;
};

struct Candidate {
    fs::path absolute_path;
    std::string path;
    Language language = Language::Python;
    std::uintmax_t size = 0;
    long long mtime_ns = 0;
};

struct ScannedFile {
    Candidate candidate;
    Language language = Language::Python;
    std::string content_hash;
    std::size_t line_count = 0;
    std::string status = "indexed";
    std::string error;
    std::vector<Symbol> symbols;
};

std::uint64_t fnv1a_bytes(const std::string& text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string hash_hex(std::uint64_t hash) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

long long current_unix_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

long long file_mtime_ns(const fs::file_time_type& value) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count();
}

bool cancelled(const Options& options) {
    return options.cancellation.cancelled() || (options.interrupted && options.interrupted());
}

Error sqlite_error(sqlite3* db, const std::string& action, const std::string& path) {
    return {ErrorCode::FileWrite,
            action + " for code index " + path + ": " +
                (db == nullptr ? std::string("unknown SQLite error") : sqlite3_errmsg(db))};
}

class Database {
   public:
    ~Database() {
        if (db_ != nullptr) sqlite3_close(db_);
    }
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database() = default;

    Error open(const std::string& path, bool read_only) {
        path_ = path;
        const int flags = read_only ? SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX
                                    : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
        const int rc = sqlite3_open_v2(path.c_str(), &db_, flags, nullptr);
        if (rc != SQLITE_OK) {
            Error error = sqlite_error(db_, "could not open", path);
            if (db_ != nullptr) {
                sqlite3_close(db_);
                db_ = nullptr;
            }
            error.code = read_only ? ErrorCode::FileRead : ErrorCode::FileWrite;
            return error;
        }
        sqlite3_busy_timeout(db_, 250);
        return ok_error();
    }

    Error exec(const char* sql) {
        char* message = nullptr;
        const int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &message);
        if (rc == SQLITE_OK) return ok_error();
        const std::string detail = message == nullptr ? sqlite3_errmsg(db_) : message;
        sqlite3_free(message);
        return {ErrorCode::FileWrite, "could not update code index " + path_ + ": " + detail};
    }

    sqlite3* get() const { return db_; }
    const std::string& path() const { return path_; }

   private:
    sqlite3* db_ = nullptr;
    std::string path_;
};

class Statement {
   public:
    Statement() = default;
    ~Statement() { if (statement_ != nullptr) sqlite3_finalize(statement_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    Error prepare(Database& db, const char* sql) {
        const int rc = sqlite3_prepare_v2(db.get(), sql, -1, &statement_, nullptr);
        return rc == SQLITE_OK ? ok_error() : sqlite_error(db.get(), "could not prepare query", db.path());
    }
    Error bind_text(Database& db, int index, const std::string& value) {
        return sqlite3_bind_text(statement_, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT) ==
                       SQLITE_OK
                   ? ok_error()
                   : sqlite_error(db.get(), "could not bind query value", db.path());
    }
    Error bind_int64(Database& db, int index, sqlite3_int64 value) {
        return sqlite3_bind_int64(statement_, index, value) == SQLITE_OK
                   ? ok_error()
                   : sqlite_error(db.get(), "could not bind query integer", db.path());
    }
    int step() { return sqlite3_step(statement_); }
    sqlite3_int64 column_int64(int column) const { return sqlite3_column_int64(statement_, column); }
    std::string column_text(int column) const {
        const unsigned char* value = sqlite3_column_text(statement_, column);
        return value == nullptr ? std::string{} : reinterpret_cast<const char*>(value);
    }
    void reset() { sqlite3_reset(statement_); sqlite3_clear_bindings(statement_); }

   private:
    sqlite3_stmt* statement_ = nullptr;
};

class Transaction {
   public:
    explicit Transaction(Database& db) : db_(db) {}
    ~Transaction() { if (active_) db_.exec("ROLLBACK"); }
    Error begin() {
        Error error = db_.exec("BEGIN IMMEDIATE");
        active_ = error.ok();
        return error;
    }
    Error commit() {
        Error error = db_.exec("COMMIT");
        if (error.ok()) active_ = false;
        return error;
    }
   private:
    Database& db_;
    bool active_ = false;
};

Language parse_language(const std::string& value) {
    if (value == "C++") return Language::Cpp;
    if (value == "C") return Language::C;
    if (value == "JavaScript") return Language::JavaScript;
    if (value == "TypeScript") return Language::TypeScript;
    if (value == "HTML") return Language::Html;
    if (value == "CSS") return Language::Css;
    return Language::Python;
}

Error ensure_schema(Database& db) {
    Error error = db.exec("PRAGMA journal_mode=WAL");
    if (!error.ok()) return error;
    error = db.exec("PRAGMA synchronous=NORMAL");
    if (!error.ok()) return error;
    error = db.exec("PRAGMA foreign_keys=ON");
    if (!error.ok()) return error;
    return db.exec(
        "CREATE TABLE IF NOT EXISTS metadata(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS files("
        " id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE, language TEXT NOT NULL,"
        " size INTEGER NOT NULL, mtime_ns INTEGER NOT NULL, content_hash TEXT NOT NULL,"
        " line_count INTEGER NOT NULL,"
        " scan_status TEXT NOT NULL, scan_error TEXT NOT NULL, indexed_at INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS symbols("
        " id INTEGER PRIMARY KEY, file_id INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,"
        " kind TEXT NOT NULL, name TEXT NOT NULL, qualified_name TEXT NOT NULL, signature TEXT NOT NULL,"
        " parameters TEXT NOT NULL, return_type TEXT NOT NULL, line_start INTEGER NOT NULL,"
        " line_end INTEGER NOT NULL, documentation TEXT NOT NULL, signature_hash TEXT NOT NULL,"
        " body_hash TEXT NOT NULL);"
        "CREATE INDEX IF NOT EXISTS symbols_file_source ON symbols(file_id,line_start,id);"
        "CREATE INDEX IF NOT EXISTS symbols_name ON symbols(name);"
        "CREATE INDEX IF NOT EXISTS symbols_qualified_name ON symbols(qualified_name);"
        "CREATE INDEX IF NOT EXISTS symbols_kind ON symbols(kind);");
}

Error ensure_line_count_column(Database& db) {
    bool found = false;
    {
        Statement columns;
        Error error = columns.prepare(db, "PRAGMA table_info(files)");
        if (!error.ok()) return error;
        for (int rc = columns.step(); rc != SQLITE_DONE; rc = columns.step()) {
            if (rc != SQLITE_ROW)
                return sqlite_error(db.get(), "could not inspect indexed files schema", db.path());
            if (columns.column_text(1) == "line_count") found = true;
        }
    }
    return found ? ok_error()
                 : db.exec("ALTER TABLE files ADD COLUMN line_count INTEGER NOT NULL DEFAULT 0");
}

Error validate_read_schema(Database& db) {
    Statement statement;
    Error error = statement.prepare(db, "SELECT value FROM metadata WHERE key='schema_version'");
    if (!error.ok()) {
        return {ErrorCode::FileRead,
                "could not read code index metadata at " + db.path() + ": " +
                    sqlite3_errmsg(db.get()) + ". The index may be corrupt; rebuild it with --index-code."};
    }
    const int rc = statement.step();
    if (rc != SQLITE_ROW) {
        return {ErrorCode::FileRead,
                "no completed code index exists at " + db.path() + "; run --index-code first"};
    }
    if (statement.column_text(0) != std::to_string(kSchemaVersion)) {
        return {ErrorCode::UnsupportedFeature,
                "code index schema at " + db.path() + " is not supported; run --index-code to rebuild it"};
    }
    Statement complete;
    error = complete.prepare(db, "SELECT value FROM metadata WHERE key='complete'");
    if (!error.ok() || complete.step() != SQLITE_ROW || complete.column_text(0) != "1") {
        return {ErrorCode::FileRead,
                "no completed code index exists at " + db.path() + "; run --index-code first"};
    }
    return ok_error();
}

Error set_metadata(Database& db, const std::string& key, const std::string& value) {
    Statement statement;
    Error error = statement.prepare(
        db, "INSERT INTO metadata(key,value) VALUES(?,?) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    if (!error.ok()) return error;
    if (!(error = statement.bind_text(db, 1, key)).ok() ||
        !(error = statement.bind_text(db, 2, value)).ok()) return error;
    return statement.step() == SQLITE_DONE ? ok_error()
                                            : sqlite_error(db.get(), "could not store metadata", db.path());
}

Error metadata(Database& db, const std::string& key, std::string& value, bool& found) {
    Statement statement;
    Error error = statement.prepare(db, "SELECT value FROM metadata WHERE key=?");
    if (!error.ok()) return error;
    if (!(error = statement.bind_text(db, 1, key)).ok()) return error;
    const int rc = statement.step();
    found = rc == SQLITE_ROW;
    if (found) value = statement.column_text(0);
    return rc == SQLITE_ROW || rc == SQLITE_DONE
               ? ok_error()
               : sqlite_error(db.get(), "could not read metadata", db.path());
}

Error load_records(Database& db, std::map<std::string, FileRecord>& records) {
    Statement statement;
    Error error = statement.prepare(
        db, "SELECT id,path,language,size,mtime_ns,content_hash,scan_status,scan_error FROM files ORDER BY path");
    if (!error.ok()) return error;
    for (int rc = statement.step(); rc != SQLITE_DONE; rc = statement.step()) {
        if (rc != SQLITE_ROW) return sqlite_error(db.get(), "could not read indexed files", db.path());
        FileRecord record;
        record.id = statement.column_int64(0);
        record.path = statement.column_text(1);
        record.language = parse_language(statement.column_text(2));
        record.size = static_cast<std::uintmax_t>(statement.column_int64(3));
        record.mtime_ns = statement.column_int64(4);
        record.content_hash = statement.column_text(5);
        record.status = statement.column_text(6);
        record.error = statement.column_text(7);
        records.emplace(record.path, std::move(record));
    }
    return ok_error();
}

std::string regex_escape(char ch) {
    static const std::string special = R"(.^$|()[]{}+\)";
    return special.find(ch) == std::string::npos ? std::string(1, ch)
                                                 : std::string("\\") + ch;
}

struct IgnorePattern {
    bool negated = false;
    std::regex expression;
};

struct IgnoreRules {
    std::vector<IgnorePattern> patterns;
    std::string fingerprint;

    bool ignored(const std::string& path) const {
        bool value = false;
        for (const IgnorePattern& pattern : patterns) {
            if (std::regex_match(path, pattern.expression)) value = !pattern.negated;
        }
        return value;
    }
};

std::string glob_regex(std::string pattern) {
    if (!pattern.empty() && pattern.front() == '/') pattern.erase(pattern.begin());
    if (!pattern.empty() && pattern.back() == '/') pattern.pop_back();
    const bool contains_slash = pattern.find('/') != std::string::npos;
    std::string output = contains_slash ? "^" : "^(?:.*/)?";
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char ch = pattern[i];
        if (ch == '*') {
            if (i + 1 < pattern.size() && pattern[i + 1] == '*') {
                ++i;
                if (i + 1 < pattern.size() && pattern[i + 1] == '/') {
                    ++i;
                    output += "(?:.*/)?";
                } else {
                    output += ".*";
                }
            } else {
                output += "[^/]*";
            }
        } else if (ch == '?') {
            output += "[^/]";
        } else {
            output += regex_escape(ch);
        }
    }
    output += "(?:/.*)?$";
    return output;
}

Error load_ignore_rules(const fs::path& root, IgnoreRules& rules) {
    std::string fingerprint_input;
    for (const char* filename : {".gitignore", ".ignore"}) {
        const fs::path path = root / filename;
        std::error_code error;
        if (!fs::exists(path, error)) {
            if (error) return {ErrorCode::FileRead, "could not inspect " + path.string() + ": " + error.message()};
            fingerprint_input += std::string(filename) + "=<missing>\n";
            continue;
        }
        std::ifstream input(path, std::ios::binary);
        if (!input) return {ErrorCode::FileRead, "could not read root ignore file: " + path.string()};
        std::string body((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        if (input.bad()) return {ErrorCode::FileRead, "could not read root ignore file: " + path.string()};
        fingerprint_input += std::string(filename) + "=" + body + "\n";
        std::istringstream lines(body);
        std::string line;
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            line = ascii_trim(std::move(line));
            if (line.empty() || line.front() == '#') continue;
            bool negated = false;
            if (line.front() == '!') {
                negated = true;
                line.erase(line.begin());
                if (line.empty()) continue;
            }
            try {
                rules.patterns.push_back({negated, std::regex(glob_regex(line), std::regex::optimize)});
            } catch (const std::regex_error& exception) {
                return {ErrorCode::Config,
                        "could not compile ignore pattern " + line + " from " + path.string() +
                            ": " + exception.what()};
            }
        }
    }
    rules.fingerprint = hash_hex(fnv1a_bytes(fingerprint_input));
    return ok_error();
}

bool excluded_directory(const std::string& name) {
    static const std::set<std::string> excluded = {
        ".ainiux", ".git", ".hg", ".svn", "build", "node_modules", "vendor", "target",
        "dist", "out", ".cache", ".venv", "venv", "env", "__pycache__"};
    return excluded.find(name) != excluded.end();
}

Error workspace_root(const std::string& requested, fs::path& root) {
    std::error_code error;
    root = fs::canonical(fs::absolute(requested, error), error);
    if (error) return {ErrorCode::FileRead, "could not resolve workspace " + requested + ": " + error.message()};
    if (!fs::is_directory(root, error) || error) {
        return {ErrorCode::FileRead, "workspace is not a readable directory: " + root.string()};
    }
    return ok_error();
}

Error discover(const fs::path& root,
               const IgnoreRules& ignores,
               const Options& options,
               std::vector<Candidate>& candidates) {
    std::deque<fs::path> directories{root};
    std::mutex mutex;
    std::condition_variable ready;
    std::size_t pending_directories = 1;
    bool failed = false;
    Error traversal_error;
    const unsigned hardware = std::thread::hardware_concurrency();
    const std::size_t worker_count = std::min<std::size_t>(32, hardware == 0 ? 4 : hardware);
    auto fail = [&](Error error) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!failed) {
            failed = true;
            traversal_error = std::move(error);
        }
        ready.notify_all();
    };
    auto worker = [&] {
        while (true) {
            fs::path directory;
            {
                std::unique_lock<std::mutex> lock(mutex);
                ready.wait(lock, [&] {
                    return failed || !directories.empty() || pending_directories == 0;
                });
                if (failed || pending_directories == 0) return;
                directory = std::move(directories.front());
                directories.pop_front();
            }
            if (cancelled(options)) {
                fail({ErrorCode::Cancelled, "code indexing cancelled"});
                return;
            }
            std::error_code filesystem_error;
            fs::directory_iterator iterator(directory, fs::directory_options::none, filesystem_error);
            const fs::directory_iterator end;
            if (filesystem_error) {
                fail({ErrorCode::FileRead,
                      "could not traverse workspace directory " + directory.string() + ": " +
                          filesystem_error.message()});
                return;
            }
            while (iterator != end) {
                if (cancelled(options)) {
                    fail({ErrorCode::Cancelled, "code indexing cancelled"});
                    return;
                }
                const fs::directory_entry entry = *iterator;
                const fs::file_status status = entry.symlink_status(filesystem_error);
                if (filesystem_error) {
                    fail({ErrorCode::FileRead,
                          "could not inspect workspace path " + entry.path().string() + ": " +
                              filesystem_error.message()});
                    return;
                }
                const std::string relative = entry.path().lexically_relative(root).generic_string();
                if (!fs::is_symlink(status) && fs::is_directory(status)) {
                    if (!excluded_directory(entry.path().filename().string())) {
                        std::lock_guard<std::mutex> lock(mutex);
                        directories.push_back(entry.path());
                        ++pending_directories;
                        ready.notify_one();
                    }
                } else if (!fs::is_symlink(status) && fs::is_regular_file(status) &&
                           !ignores.ignored(relative)) {
                    Language language;
                    if (language_for_path(relative, language)) {
                        Candidate candidate;
                        candidate.absolute_path = entry.path();
                        candidate.path = relative;
                        candidate.language = language;
                        candidate.size = entry.file_size(filesystem_error);
                        if (filesystem_error) {
                            fail({ErrorCode::FileRead,
                                  "could not inspect source file size " + relative + ": " +
                                      filesystem_error.message()});
                            return;
                        }
                        const fs::file_time_type mtime = entry.last_write_time(filesystem_error);
                        if (filesystem_error) {
                            fail({ErrorCode::FileRead,
                                  "could not inspect source file time " + relative + ": " +
                                      filesystem_error.message()});
                            return;
                        }
                        candidate.mtime_ns = file_mtime_ns(mtime);
                        std::lock_guard<std::mutex> lock(mutex);
                        candidates.push_back(std::move(candidate));
                    }
                }
                iterator.increment(filesystem_error);
                if (filesystem_error) {
                    fail({ErrorCode::FileRead,
                          "could not continue workspace traversal at " + relative + ": " +
                              filesystem_error.message()});
                    return;
                }
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                --pending_directories;
                ready.notify_all();
            }
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    try {
        for (std::size_t index = 0; index < worker_count; ++index) workers.emplace_back(worker);
    } catch (const std::exception& exception) {
        fail({ErrorCode::Internal,
              "could not start parallel workspace traversal: " + std::string(exception.what())});
    }
    for (std::thread& thread : workers) thread.join();
    if (failed) return traversal_error;
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
        return left.path < right.path;
    });
    return ok_error();
}

ScannedFile scan_candidate(const Candidate& candidate, const Options& options) {
    ScannedFile output;
    output.candidate = candidate;
    output.language = candidate.language;
    if (candidate.size > options.max_source_code_file_size) {
        output.status = "skipped";
        output.error = "file exceeds max_source_code_file_size of " +
                       std::to_string(options.max_source_code_file_size) + " bytes";
        return output;
    }
    std::ifstream input(candidate.absolute_path, std::ios::binary);
    if (!input) {
        output.status = "skipped";
        output.error = "could not open source file for reading";
        return output;
    }
    std::string source;
    source.reserve(static_cast<std::size_t>(candidate.size));
    char buffer[32768];
    while (input) {
        if (cancelled(options)) {
            output.status = "cancelled";
            return output;
        }
        input.read(buffer, sizeof(buffer));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            if (source.size() + static_cast<std::size_t>(count) > options.max_source_code_file_size) {
                output.status = "skipped";
                output.error = "file grew beyond max_source_code_file_size while being read";
                return output;
            }
            source.append(buffer, static_cast<std::size_t>(count));
        }
    }
    if (input.bad()) {
        output.status = "skipped";
        output.error = "read failed before end of source file";
        return output;
    }
    output.candidate.size = source.size();
    output.content_hash = hash_hex(fnv1a_bytes(source));
    const std::size_t nul = source.find('\0');
    if (nul != std::string::npos) {
        output.status = "skipped";
        output.error = "binary source contains NUL byte at offset " + std::to_string(nul);
        return output;
    }
    std::size_t invalid = 0;
    if (!html::is_valid_utf8(source, &invalid)) {
        output.status = "skipped";
        output.error = "source is not valid UTF-8 (invalid byte at offset " + std::to_string(invalid) + ")";
        return output;
    }
    if (!source.empty()) {
        output.line_count = static_cast<std::size_t>(std::count(source.begin(), source.end(), '\n'));
        if (source.back() != '\n') ++output.line_count;
    }
    ScanResult scan = scan_source(candidate.path, source, candidate.language);
    output.language = scan.language;
    output.symbols = std::move(scan.symbols);
    return output;
}

Error replace_file(Database& db, const ScannedFile& file, long long indexed_at) {
    Statement upsert;
    Error error = upsert.prepare(
        db, "INSERT INTO files(path,language,size,mtime_ns,content_hash,line_count,scan_status,scan_error,indexed_at)"
            " VALUES(?,?,?,?,?,?,?,?,?) ON CONFLICT(path) DO UPDATE SET language=excluded.language,"
            " size=excluded.size,mtime_ns=excluded.mtime_ns,content_hash=excluded.content_hash,"
            " line_count=excluded.line_count,"
            " scan_status=excluded.scan_status,scan_error=excluded.scan_error,indexed_at=excluded.indexed_at");
    if (!error.ok()) return error;
    int parameter = 1;
    if (!(error = upsert.bind_text(db, parameter++, file.candidate.path)).ok() ||
        !(error = upsert.bind_text(db, parameter++, language_name(file.language))).ok() ||
        !(error = upsert.bind_int64(db, parameter++, static_cast<sqlite3_int64>(file.candidate.size))).ok() ||
        !(error = upsert.bind_int64(db, parameter++, file.candidate.mtime_ns)).ok() ||
        !(error = upsert.bind_text(db, parameter++, file.content_hash)).ok() ||
        !(error = upsert.bind_int64(db, parameter++, static_cast<sqlite3_int64>(file.line_count))).ok() ||
        !(error = upsert.bind_text(db, parameter++, file.status)).ok() ||
        !(error = upsert.bind_text(db, parameter++, file.error)).ok() ||
        !(error = upsert.bind_int64(db, parameter, indexed_at)).ok()) return error;
    if (upsert.step() != SQLITE_DONE) return sqlite_error(db.get(), "could not store indexed file", db.path());

    Statement find;
    if (!(error = find.prepare(db, "SELECT id FROM files WHERE path=?")).ok() ||
        !(error = find.bind_text(db, 1, file.candidate.path)).ok()) return error;
    if (find.step() != SQLITE_ROW) return sqlite_error(db.get(), "could not locate indexed file", db.path());
    const sqlite3_int64 file_id = find.column_int64(0);

    Statement remove;
    if (!(error = remove.prepare(db, "DELETE FROM symbols WHERE file_id=?")).ok() ||
        !(error = remove.bind_int64(db, 1, file_id)).ok()) return error;
    if (remove.step() != SQLITE_DONE) return sqlite_error(db.get(), "could not replace indexed symbols", db.path());

    Statement insert;
    error = insert.prepare(
        db, "INSERT INTO symbols(file_id,kind,name,qualified_name,signature,parameters,return_type,"
            "line_start,line_end,documentation,signature_hash,body_hash) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
    if (!error.ok()) return error;
    for (const Symbol& symbol : file.symbols) {
        parameter = 1;
        if (!(error = insert.bind_int64(db, parameter++, file_id)).ok() ||
            !(error = insert.bind_text(db, parameter++, symbol.kind)).ok() ||
            !(error = insert.bind_text(db, parameter++, symbol.name)).ok() ||
            !(error = insert.bind_text(db, parameter++, symbol.qualified_name)).ok() ||
            !(error = insert.bind_text(db, parameter++, symbol.signature)).ok() ||
            !(error = insert.bind_text(db, parameter++, symbol.parameters)).ok() ||
            !(error = insert.bind_text(db, parameter++, symbol.return_type)).ok() ||
            !(error = insert.bind_int64(db, parameter++, symbol.line_start)).ok() ||
            !(error = insert.bind_int64(db, parameter++, symbol.line_end)).ok() ||
            !(error = insert.bind_text(db, parameter++, symbol.documentation)).ok() ||
            !(error = insert.bind_text(db, parameter++, hash_hex(symbol.signature_hash))).ok() ||
            !(error = insert.bind_text(db, parameter, hash_hex(symbol.body_hash))).ok()) return error;
        if (insert.step() != SQLITE_DONE) return sqlite_error(db.get(), "could not store indexed symbol", db.path());
        insert.reset();
    }
    return ok_error();
}

Error remove_file(Database& db, const std::string& path) {
    Statement statement;
    Error error = statement.prepare(db, "DELETE FROM files WHERE path=?");
    if (!error.ok() || !(error = statement.bind_text(db, 1, path)).ok()) return error;
    return statement.step() == SQLITE_DONE ? ok_error()
                                            : sqlite_error(db.get(), "could not remove stale indexed file", db.path());
}

std::string markdown_text(std::string text) {
    for (char& ch : text) {
        if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
    }
    std::string output;
    output.reserve(text.size());
    for (char ch : text) {
        if (ch == '\\' || ch == '`' || ch == '|' || ch == '[' || ch == ']' || ch == '<' || ch == '>')
            output.push_back('\\');
        output.push_back(ch);
    }
    return output;
}

std::string markdown_code(std::string text) {
    for (char& ch : text) {
        if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
    }
    std::size_t maximum_run = 0;
    std::size_t run = 0;
    for (char ch : text) {
        if (ch == '`') maximum_run = std::max(maximum_run, ++run);
        else run = 0;
    }
    const std::string fence(maximum_run + 1, '`');
    const bool pad = !text.empty() && (text.front() == '`' || text.back() == '`');
    return fence + (pad ? " " : "") + text + (pad ? " " : "") + fence;
}

std::string utc_time(std::string seconds) {
    long long value = 0;
    try { value = std::stoll(seconds); } catch (...) { return seconds; }
    std::time_t time = static_cast<std::time_t>(value);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif
    char buffer[32];
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S UTC", &tm) == 0) return seconds;
    return buffer;
}

}  // namespace

std::string database_path(const std::string& workspace) {
    return (fs::path(workspace) / ".ainiux" / "index.sqlite").string();
}

Error clear_database(const Options& options, ClearStats& stats) {
    fs::path root;
    Error error = workspace_root(options.workspace, root);
    if (!error.ok()) return error;

    const fs::path state_directory = root / ".ainiux";
    std::error_code filesystem_error;
    const fs::file_status directory_status = fs::symlink_status(state_directory, filesystem_error);
    if (filesystem_error == std::errc::no_such_file_or_directory) return ok_error();
    if (filesystem_error) {
        return {ErrorCode::FileWrite,
                "could not inspect project index directory " + state_directory.string() + ": " +
                    filesystem_error.message()};
    }
    if (!fs::exists(directory_status)) return ok_error();
    if (fs::is_symlink(directory_status) || !fs::is_directory(directory_status)) {
        return {ErrorCode::FileWrite,
                "refusing to clear code index because " + state_directory.string() +
                    " is not a project-local directory"};
    }

    const fs::path db_path = state_directory / "index.sqlite";
    const fs::path targets[] = {db_path, fs::path(db_path.string() + "-wal"),
                                fs::path(db_path.string() + "-shm")};
    for (const fs::path& target : targets) {
        filesystem_error.clear();
        const fs::file_status status = fs::symlink_status(target, filesystem_error);
        if (filesystem_error == std::errc::no_such_file_or_directory) continue;
        if (filesystem_error) {
            return {ErrorCode::FileWrite,
                    "could not inspect code index file " + target.string() + ": " +
                        filesystem_error.message()};
        }
        if (!fs::exists(status)) continue;
        if (!fs::is_regular_file(status) && !fs::is_symlink(status)) {
            return {ErrorCode::FileWrite,
                    "refusing to remove non-file code index path " + target.string()};
        }
        if (!fs::remove(target, filesystem_error) || filesystem_error) {
            return {ErrorCode::FileWrite,
                    "could not remove code index file " + target.string() + ": " +
                        (filesystem_error ? filesystem_error.message() : "path was not removed")};
        }
        ++stats.removed_files;
    }
    return ok_error();
}

Error refresh(const Options& options, RefreshStats& stats) {
    const auto started = std::chrono::steady_clock::now();
    fs::path root;
    Error error = workspace_root(options.workspace, root);
    if (!error.ok()) return error;
    IgnoreRules ignores;
    if (!(error = load_ignore_rules(root, ignores)).ok()) return error;
    std::vector<Candidate> candidates;
    if (!(error = discover(root, ignores, options, candidates)).ok()) return error;
    stats.discovered = candidates.size();
    if (cancelled(options)) return {ErrorCode::Cancelled, "code indexing cancelled"};

    const fs::path state_directory = root / ".ainiux";
    std::error_code filesystem_error;
    const bool created_state_directory = fs::create_directories(state_directory, filesystem_error);
    if (filesystem_error) {
        return {ErrorCode::FileWrite,
                "could not create project index directory " + state_directory.string() + ": " +
                    filesystem_error.message()};
    }
    if (created_state_directory && chmod(state_directory.c_str(), S_IRWXU) != 0) {
        return {ErrorCode::FileWrite,
                "could not set permissions on project index directory " + state_directory.string() +
                    ": " + std::strerror(errno)};
    }
    const fs::path db_path = state_directory / "index.sqlite";
    const bool existed = fs::exists(db_path, filesystem_error);
    if (filesystem_error) return {ErrorCode::FileWrite, "could not inspect code index path: " + filesystem_error.message()};
    Database db;
    if (!(error = db.open(db_path.string(), false)).ok()) return error;
    if (!existed && chmod(db_path.c_str(), S_IRUSR | S_IWUSR) != 0) {
        return {ErrorCode::FileWrite,
                "could not set permissions on project code index " + db_path.string() +
                    ": " + std::strerror(errno)};
    }
    if (!(error = ensure_schema(db)).ok()) return error;
    std::map<std::string, FileRecord> existing;
    if (!(error = load_records(db, existing)).ok()) return error;

    bool found = false;
    std::string stored_scanner;
    if (!(error = metadata(db, "scanner_version", stored_scanner, found)).ok()) return error;
    const bool scanner_changed = !found || stored_scanner != std::to_string(kScannerVersion);
    std::string stored_limit;
    if (!(error = metadata(db, "max_source_code_file_size", stored_limit, found)).ok()) return error;
    const bool limit_changed = !found || stored_limit != std::to_string(options.max_source_code_file_size);

    std::set<std::string> current_paths;
    std::vector<Candidate> changed;
    for (const Candidate& candidate : candidates) {
        current_paths.insert(candidate.path);
        const auto old = existing.find(candidate.path);
        if (!scanner_changed && !limit_changed && old != existing.end() &&
            old->second.size == candidate.size && old->second.mtime_ns == candidate.mtime_ns) {
            ++stats.unchanged;
        } else {
            changed.push_back(candidate);
        }
    }
    std::vector<std::string> removed;
    for (const auto& item : existing) {
        if (current_paths.find(item.first) == current_paths.end()) removed.push_back(item.first);
    }

    std::vector<ScannedFile> scanned(changed.size());
    const unsigned hardware = std::thread::hardware_concurrency();
    const std::size_t automatic = hardware == 0 ? 4 : hardware;
    stats.worker_count = changed.empty() ? 0 : std::min<std::size_t>({32, automatic, changed.size()});
    std::atomic<std::size_t> cursor{0};
    std::atomic<bool> worker_failed{false};
    std::mutex worker_error_mutex;
    std::string worker_error;
    std::vector<std::thread> workers;
    workers.reserve(stats.worker_count);
    try {
        for (std::size_t worker = 0; worker < stats.worker_count; ++worker) {
            workers.emplace_back([&] {
                try {
                    while (!cancelled(options) && !worker_failed.load()) {
                        const std::size_t index = cursor.fetch_add(1);
                        if (index >= changed.size()) break;
                        scanned[index] = scan_candidate(changed[index], options);
                    }
                } catch (const std::exception& exception) {
                    worker_failed.store(true);
                    std::lock_guard<std::mutex> lock(worker_error_mutex);
                    worker_error = exception.what();
                } catch (...) {
                    worker_failed.store(true);
                    std::lock_guard<std::mutex> lock(worker_error_mutex);
                    worker_error = "unknown scanner exception";
                }
            });
        }
    } catch (const std::exception& exception) {
        worker_failed.store(true);
        std::lock_guard<std::mutex> lock(worker_error_mutex);
        worker_error = exception.what();
    }
    for (std::thread& worker : workers) worker.join();
    if (worker_failed.load()) {
        return {ErrorCode::Internal, "parallel source scan failed: " + worker_error};
    }
    if (cancelled(options)) return {ErrorCode::Cancelled, "code indexing cancelled; previous snapshot preserved"};

    Transaction transaction(db);
    if (!(error = transaction.begin()).ok()) return error;
    if (!(error = ensure_line_count_column(db)).ok()) return error;
    for (const std::string& path : removed) {
        if (cancelled(options)) return {ErrorCode::Cancelled, "code indexing cancelled; previous snapshot preserved"};
        if (!(error = remove_file(db, path)).ok()) return error;
        ++stats.removed;
    }
    const long long indexed_at = current_unix_seconds();
    for (const ScannedFile& file : scanned) {
        if (cancelled(options)) return {ErrorCode::Cancelled, "code indexing cancelled; previous snapshot preserved"};
        if (!(error = replace_file(db, file, indexed_at)).ok()) return error;
        if (file.status == "indexed") {
            ++stats.indexed;
            stats.symbols += file.symbols.size();
        } else {
            ++stats.skipped;
            stats.diagnostics.push_back(file.candidate.path + ": " + file.error);
        }
    }
    if (cancelled(options)) return {ErrorCode::Cancelled, "code indexing cancelled; previous snapshot preserved"};
    if (!(error = set_metadata(db, "schema_version", std::to_string(kSchemaVersion))).ok() ||
        !(error = set_metadata(db, "scanner_version", std::to_string(kScannerVersion))).ok() ||
        !(error = set_metadata(db, "ignore_fingerprint", ignores.fingerprint)).ok() ||
        !(error = set_metadata(db, "max_source_code_file_size", std::to_string(options.max_source_code_file_size))).ok() ||
        !(error = set_metadata(db, "workspace", root.string())).ok() ||
        !(error = set_metadata(db, "updated_at", std::to_string(indexed_at))).ok() ||
        !(error = set_metadata(db, "complete", "1")).ok()) return error;
    if (cancelled(options)) return {ErrorCode::Cancelled, "code indexing cancelled; previous snapshot preserved"};
    if (!(error = transaction.commit()).ok()) return error;
    stats.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count();
    return ok_error();
}

Error check_freshness(const Options& options, Freshness& freshness) {
    fs::path root;
    Error error = workspace_root(options.workspace, root);
    if (!error.ok()) return error;
    const fs::path path = root / ".ainiux" / "index.sqlite";
    Database db;
    if (!(error = db.open(path.string(), true)).ok()) {
        return {ErrorCode::FileRead,
                "no completed code index exists at " + path.string() + "; run --index-code first"};
    }
    if (!(error = validate_read_schema(db)).ok()) return error;
    std::map<std::string, FileRecord> existing;
    if (!(error = load_records(db, existing)).ok()) return error;
    IgnoreRules ignores;
    if (!(error = load_ignore_rules(root, ignores)).ok()) return error;
    std::vector<Candidate> candidates;
    if (!(error = discover(root, ignores, options, candidates)).ok()) return error;
    std::set<std::string> current;
    for (const Candidate& candidate : candidates) {
        current.insert(candidate.path);
        const auto old = existing.find(candidate.path);
        if (old == existing.end()) freshness.added.push_back(candidate.path);
        else if (old->second.size != candidate.size || old->second.mtime_ns != candidate.mtime_ns)
            freshness.changed.push_back(candidate.path);
    }
    for (const auto& item : existing) {
        if (current.find(item.first) == current.end()) freshness.removed.push_back(item.first);
    }
    bool found = false;
    std::string value;
    if (!(error = metadata(db, "scanner_version", value, found)).ok()) return error;
    if (!found || value != std::to_string(kScannerVersion)) freshness.reason = "scanner version changed";
    if (!(error = metadata(db, "ignore_fingerprint", value, found)).ok()) return error;
    if ((!found || value != ignores.fingerprint) && freshness.reason.empty()) freshness.reason = "root ignore rules changed";
    if (!(error = metadata(db, "max_source_code_file_size", value, found)).ok()) return error;
    if ((!found || value != std::to_string(options.max_source_code_file_size)) && freshness.reason.empty())
        freshness.reason = "maximum source-code file size changed";
    freshness.fresh = freshness.added.empty() && freshness.changed.empty() && freshness.removed.empty() &&
                      freshness.reason.empty();
    return ok_error();
}

Error print_markdown(const Options& options, const Freshness& freshness, std::ostream& output) {
    fs::path root;
    Error error = workspace_root(options.workspace, root);
    if (!error.ok()) return error;
    const fs::path path = root / ".ainiux" / "index.sqlite";
    Database db;
    if (!(error = db.open(path.string(), true)).ok() || !(error = validate_read_schema(db)).ok()) return error;
    std::string updated;
    bool found = false;
    if (!(error = metadata(db, "updated_at", updated, found)).ok()) return error;

    output << "# ainiux Code Index\n\n"
           << "- Workspace: " << markdown_code(root.string()) << "\n"
           << "- Database: " << markdown_code(path.string()) << "\n"
           << "- Schema version: " << kSchemaVersion << "\n"
           << "- Scanner version: " << kScannerVersion << "\n"
           << "- Updated: " << (found ? utc_time(updated) : std::string("unknown")) << "\n"
           << "- Freshness: " << (freshness.fresh ? "fresh" : "stale") << "\n\n";

    Statement totals;
    if (!(error = totals.prepare(
              db, "SELECT language,COUNT(*),"
                  "COALESCE(SUM(CASE WHEN scan_status='indexed' THEN line_count ELSE 0 END),0),"
                  "SUM(CASE WHEN scan_status='indexed' THEN 1 ELSE 0 END),"
                  "SUM(CASE WHEN scan_status!='indexed' THEN 1 ELSE 0 END),"
                  "(SELECT COUNT(*) FROM symbols s JOIN files sf ON sf.id=s.file_id WHERE sf.language=f.language) "
                  "FROM files f GROUP BY language ORDER BY language")).ok()) return error;
    output << "## Totals\n\n| Language | Files | Lines of code | Indexed | Skipped/errors | Symbols |\n"
              "| --- | ---: | ---: | ---: | ---: | ---: |\n";
    sqlite3_int64 total_files = 0;
    sqlite3_int64 total_lines = 0;
    sqlite3_int64 total_indexed = 0;
    sqlite3_int64 total_skipped = 0;
    sqlite3_int64 total_symbols = 0;
    for (int rc = totals.step(); rc != SQLITE_DONE; rc = totals.step()) {
        if (cancelled(options)) return {ErrorCode::Cancelled, "printing code index cancelled"};
        if (rc != SQLITE_ROW) return sqlite_error(db.get(), "could not read index totals", db.path());
        total_files += totals.column_int64(1);
        total_lines += totals.column_int64(2);
        total_indexed += totals.column_int64(3);
        total_skipped += totals.column_int64(4);
        total_symbols += totals.column_int64(5);
        output << "| " << markdown_text(totals.column_text(0)) << " | " << totals.column_int64(1)
               << " | " << totals.column_int64(2) << " | " << totals.column_int64(3)
               << " | " << totals.column_int64(4) << " | " << totals.column_int64(5) << " |\n";
    }
    output << "| **All languages** | **" << total_files << "** | **" << total_lines
           << "** | **" << total_indexed << "** | **" << total_skipped << "** | **"
           << total_symbols << "** |\n";
    output << "\n";
    if (!freshness.fresh) {
        output << "## Stale Snapshot\n\n";
        if (!freshness.reason.empty()) output << "- " << markdown_text(freshness.reason) << "\n";
        for (const std::string& item : freshness.added) output << "- New: " << markdown_code(item) << "\n";
        for (const std::string& item : freshness.changed) output << "- Changed: " << markdown_code(item) << "\n";
        for (const std::string& item : freshness.removed) output << "- Deleted: " << markdown_code(item) << "\n";
        output << "\n";
    }

    Statement skipped;
    if (!(error = skipped.prepare(db, "SELECT path,scan_error FROM files WHERE scan_status!='indexed' ORDER BY path")).ok())
        return error;
    bool wrote_skipped = false;
    for (int rc = skipped.step(); rc != SQLITE_DONE; rc = skipped.step()) {
        if (cancelled(options)) return {ErrorCode::Cancelled, "printing code index cancelled"};
        if (rc != SQLITE_ROW) return sqlite_error(db.get(), "could not read index errors", db.path());
        if (!wrote_skipped) output << "## Skipped Files and Errors\n\n";
        wrote_skipped = true;
        output << "- " << markdown_code(skipped.column_text(0)) << ": "
               << markdown_text(skipped.column_text(1)) << "\n";
    }
    if (wrote_skipped) output << "\n";

    Statement files;
    if (!(error = files.prepare(db, "SELECT id,path,language,line_count,scan_status,scan_error FROM files ORDER BY path")).ok())
        return error;
    output << "## Files\n\n";
    while (true) {
        if (cancelled(options)) return {ErrorCode::Cancelled, "printing code index cancelled"};
        const int rc = files.step();
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) return sqlite_error(db.get(), "could not read indexed files", db.path());
        const sqlite3_int64 file_id = files.column_int64(0);
        output << "### " << markdown_code(files.column_text(1)) << "\n\n"
               << "Language: " << markdown_text(files.column_text(2))
               << "; lines: " << files.column_int64(3)
               << "; status: " << markdown_text(files.column_text(4)) << ".\n\n";
        Statement symbols;
        if (!(error = symbols.prepare(
                  db, "SELECT kind,qualified_name,signature,line_start,line_end,documentation FROM symbols "
                      "WHERE file_id=? ORDER BY line_start,id")).ok() ||
            !(error = symbols.bind_int64(db, 1, file_id)).ok()) return error;
        bool any = false;
        for (int symbol_rc = symbols.step(); symbol_rc != SQLITE_DONE; symbol_rc = symbols.step()) {
            if (cancelled(options)) return {ErrorCode::Cancelled, "printing code index cancelled"};
            if (symbol_rc != SQLITE_ROW) return sqlite_error(db.get(), "could not read indexed symbols", db.path());
            any = true;
            output << "- **" << markdown_text(symbols.column_text(0)) << "** "
                   << markdown_code(symbols.column_text(1)) << " (lines " << symbols.column_int64(3)
                   << "-" << symbols.column_int64(4) << ")\n"
                   << "  - Signature: " << markdown_code(symbols.column_text(2)) << "\n";
            const std::string documentation = symbols.column_text(5);
            if (!documentation.empty()) output << "  - Documentation: " << markdown_text(documentation) << "\n";
        }
        if (!any) output << "No symbols found.\n";
        output << "\n";
    }
    if (!output) return {ErrorCode::FileWrite, "could not write Markdown code index report"};
    return ok_error();
}

}  // namespace ainiux::agent::index
