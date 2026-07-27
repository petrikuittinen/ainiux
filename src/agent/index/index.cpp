#include "agent/index/index.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <deque>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <thread>
#include <utility>

#include <sys/stat.h>

#include "agent/project_paths.hpp"
#include "html/html.hpp"

namespace ainiux::agent::index {
namespace {

namespace fs = std::filesystem;
using ::ainiux::agent::kProjectStateDirName;

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
    std::vector<Reference> references;
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
    Error bind_double(Database& db, int index, double value) {
        return sqlite3_bind_double(statement_, index, value) == SQLITE_OK
                   ? ok_error()
                   : sqlite_error(db.get(), "could not bind query number", db.path());
    }
    Error bind_null(Database& db, int index) {
        return sqlite3_bind_null(statement_, index) == SQLITE_OK
                   ? ok_error()
                   : sqlite_error(db.get(), "could not bind query null", db.path());
    }
    int step() { return sqlite3_step(statement_); }
    sqlite3_int64 column_int64(int column) const { return sqlite3_column_int64(statement_, column); }
    double column_double(int column) const { return sqlite3_column_double(statement_, column); }
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
    if (value == "Markdown") return Language::Markdown;
    if (value == "C++") return Language::Cpp;
    if (value == "C") return Language::C;
    if (value == "C#") return Language::CSharp;
    if (value == "Java") return Language::Java;
    if (value == "JavaScript") return Language::JavaScript;
    if (value == "TypeScript") return Language::TypeScript;
    if (value == "HTML") return Language::Html;
    if (value == "HTML-only") return Language::HtmlOnly;
    if (value == "CSS") return Language::Css;
    if (value == "XML") return Language::Xml;
    if (value == "JSON") return Language::Json;
    if (value == "Bash") return Language::Bash;
    if (value == "PHP") return Language::Php;
    if (value == "Perl") return Language::Perl;
    if (value == "Ruby") return Language::Ruby;
    if (value == "Rust") return Language::Rust;
    if (value == "Go") return Language::Go;
    if (value == "PowerShell") return Language::PowerShell;
    if (value == "Assembly") return Language::Assembly;
    if (value == "SQL") return Language::Sql;
    if (value == "TOML") return Language::Toml;
    if (value == "YAML") return Language::Yaml;
    if (value == "INI") return Language::Ini;
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
        "CREATE INDEX IF NOT EXISTS symbols_kind ON symbols(kind);"
        "CREATE TABLE IF NOT EXISTS refs("
        " id INTEGER PRIMARY KEY,"
        " source_file_id INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,"
        " source_symbol_id INTEGER REFERENCES symbols(id) ON DELETE CASCADE,"
        " kind TEXT NOT NULL,target_spelling TEXT NOT NULL,qualifier TEXT NOT NULL,"
        " receiver_type TEXT NOT NULL,evidence TEXT NOT NULL,line INTEGER NOT NULL,"
        " confidence REAL NOT NULL,target_symbol_id INTEGER REFERENCES symbols(id) ON DELETE SET NULL,"
        " resolution TEXT NOT NULL);"
        "CREATE INDEX IF NOT EXISTS refs_source_symbol ON refs(source_symbol_id,kind,line,id);"
        "CREATE INDEX IF NOT EXISTS refs_target_symbol ON refs(target_symbol_id,kind,id);"
        "CREATE INDEX IF NOT EXISTS refs_target_spelling ON refs(target_spelling);"
        "CREATE TABLE IF NOT EXISTS symbol_scores("
        " symbol_id INTEGER PRIMARY KEY REFERENCES symbols(id) ON DELETE CASCADE,"
        " caller_count INTEGER NOT NULL,page_rank REAL NOT NULL);");
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
    // POSIX convention: a leading dot marks a hidden directory. Hidden trees
    // contain editor state, VCS metadata, caches, credentials, and Ainiux's own
    // project artifacts; none are code-index candidates by default.
    if (!name.empty() && name.front() == '.') return true;
    static const std::set<std::string> excluded = {
        "build", "node_modules", "vendor", "target", "dist", "out", "venv", "env",
        "__pycache__"};
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
    const std::size_t available = hardware == 0 ? 4 : hardware;
    const std::size_t worker_count =
        options.update_paths.size() == 1
            ? 1
            : std::min<std::size_t>(
                  32, std::max<std::size_t>(1, (available * 3) / 4));
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
    output.references = std::move(scan.references);
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

    Statement remove_references;
    if (!(error = remove_references.prepare(
              db, "DELETE FROM refs WHERE source_file_id=?")).ok() ||
        !(error = remove_references.bind_int64(db, 1, file_id)).ok()) return error;
    if (remove_references.step() != SQLITE_DONE)
        return sqlite_error(db.get(), "could not replace indexed references", db.path());

    Statement remove;
    if (!(error = remove.prepare(db, "DELETE FROM symbols WHERE file_id=?")).ok() ||
        !(error = remove.bind_int64(db, 1, file_id)).ok()) return error;
    if (remove.step() != SQLITE_DONE) return sqlite_error(db.get(), "could not replace indexed symbols", db.path());

    Statement insert;
    error = insert.prepare(
        db, "INSERT INTO symbols(file_id,kind,name,qualified_name,signature,parameters,return_type,"
            "line_start,line_end,documentation,signature_hash,body_hash) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
    if (!error.ok()) return error;
    std::vector<sqlite3_int64> symbol_ids;
    symbol_ids.reserve(file.symbols.size());
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
        symbol_ids.push_back(sqlite3_last_insert_rowid(db.get()));
        insert.reset();
    }

    Statement insert_reference;
    error = insert_reference.prepare(
        db, "INSERT INTO refs(source_file_id,source_symbol_id,kind,target_spelling,qualifier,"
            "receiver_type,evidence,line,confidence,target_symbol_id,resolution)"
            " VALUES(?,?,?,?,?,?,?,?,?,NULL,'unresolved')");
    if (!error.ok()) return error;
    for (const Reference& reference : file.references) {
        parameter = 1;
        if (!(error = insert_reference.bind_int64(db, parameter++, file_id)).ok()) return error;
        if (reference.source_symbol_index >= 0 &&
            static_cast<std::size_t>(reference.source_symbol_index) < symbol_ids.size()) {
            error = insert_reference.bind_int64(
                db, parameter++, symbol_ids[static_cast<std::size_t>(
                                      reference.source_symbol_index)]);
        } else {
            error = insert_reference.bind_null(db, parameter++);
        }
        if (!error.ok() ||
            !(error = insert_reference.bind_text(db, parameter++, reference.kind)).ok() ||
            !(error = insert_reference.bind_text(
                  db, parameter++, reference.target_spelling)).ok() ||
            !(error = insert_reference.bind_text(db, parameter++, reference.qualifier)).ok() ||
            !(error = insert_reference.bind_text(
                  db, parameter++, reference.receiver_type)).ok() ||
            !(error = insert_reference.bind_text(db, parameter++, reference.evidence)).ok() ||
            !(error = insert_reference.bind_int64(db, parameter++, reference.line)).ok() ||
            !(error = insert_reference.bind_double(
                  db, parameter, reference.confidence)).ok())
            return error;
        if (insert_reference.step() != SQLITE_DONE)
            return sqlite_error(db.get(), "could not store indexed reference", db.path());
        insert_reference.reset();
    }
    return ok_error();
}

std::string normalized_symbol_name(std::string name) {
    std::string output;
    output.reserve(name.size());
    int template_depth = 0;
    for (std::size_t index = 0; index < name.size(); ++index) {
        const char ch = name[index];
        if (ch == '<') {
            ++template_depth;
            continue;
        }
        if (ch == '>' && template_depth > 0) {
            --template_depth;
            continue;
        }
        if (template_depth > 0 || std::isspace(static_cast<unsigned char>(ch))) continue;
        if (ch == '-' && index + 1 < name.size() && name[index + 1] == '>') {
            output += "::";
            ++index;
        } else if (ch == '.') {
            output += "::";
        } else {
            output.push_back(ch);
        }
    }
    while (output.rfind("::", 0) == 0) output.erase(0, 2);
    return output;
}

std::string normalized_simple_name(const std::string& name) {
    const std::string normalized = normalized_symbol_name(name);
    const std::size_t separator = normalized.rfind("::");
    return separator == std::string::npos ? normalized : normalized.substr(separator + 2);
}

bool compatible_reference_target(const std::string& reference_kind,
                                 const std::string& symbol_kind) {
    if (reference_kind == "call")
        return symbol_kind == "function" || symbol_kind == "method";
    if (reference_kind == "instantiate" || reference_kind == "inherit")
        return symbol_kind == "class" || symbol_kind == "struct" ||
               symbol_kind == "union" || symbol_kind == "typedef" ||
               symbol_kind == "alias";
    return reference_kind == "use";
}

struct ResolverSymbol {
    sqlite3_int64 id = 0;
    sqlite3_int64 file_id = 0;
    std::string path;
    std::string kind;
    std::string name;
    std::string qualified_name;
    std::string normalized_qualified;
    int line_start = 1;
    int line_end = 1;
};

struct ResolverReference {
    sqlite3_int64 id = 0;
    sqlite3_int64 source_file_id = 0;
    sqlite3_int64 source_symbol_id = 0;
    std::string source_path;
    std::string kind;
    std::string target_spelling;
    std::string qualifier;
    std::string receiver_type;
    double lexical_confidence = 0.0;
};

Error resolve_graph(Database& db, const Options& options) {
    std::vector<ResolverSymbol> symbols;
    std::map<std::string, std::vector<std::size_t>> by_simple;
    {
        Statement statement;
        Error error = statement.prepare(
            db, "SELECT s.id,s.file_id,f.path,s.kind,s.name,s.qualified_name,"
                "s.line_start,s.line_end FROM symbols s JOIN files f ON f.id=s.file_id "
                "ORDER BY f.path,s.line_start,s.id");
        if (!error.ok()) return error;
        for (int rc = statement.step(); rc != SQLITE_DONE; rc = statement.step()) {
            if (cancelled(options))
                return {ErrorCode::Cancelled,
                        "code graph resolution cancelled; previous snapshot preserved"};
            if (rc != SQLITE_ROW)
                return sqlite_error(db.get(), "could not load symbols for graph resolution",
                                    db.path());
            ResolverSymbol symbol;
            symbol.id = statement.column_int64(0);
            symbol.file_id = statement.column_int64(1);
            symbol.path = statement.column_text(2);
            symbol.kind = statement.column_text(3);
            symbol.name = statement.column_text(4);
            symbol.qualified_name = statement.column_text(5);
            symbol.normalized_qualified =
                normalized_symbol_name(symbol.qualified_name);
            symbol.line_start = static_cast<int>(statement.column_int64(6));
            symbol.line_end = static_cast<int>(statement.column_int64(7));
            const std::size_t index = symbols.size();
            symbols.push_back(std::move(symbol));
            by_simple[normalized_simple_name(symbols.back().name)].push_back(index);
        }
    }

    std::vector<ResolverReference> references;
    {
        Statement statement;
        Error error = statement.prepare(
            db, "SELECT r.id,r.source_file_id,COALESCE(r.source_symbol_id,0),f.path,"
                "r.kind,r.target_spelling,r.qualifier,r.receiver_type,r.confidence "
                "FROM refs r JOIN files f ON f.id=r.source_file_id ORDER BY r.id");
        if (!error.ok()) return error;
        for (int rc = statement.step(); rc != SQLITE_DONE; rc = statement.step()) {
            if (rc != SQLITE_ROW)
                return sqlite_error(db.get(),
                                    "could not load references for graph resolution",
                                    db.path());
            ResolverReference reference;
            reference.id = statement.column_int64(0);
            reference.source_file_id = statement.column_int64(1);
            reference.source_symbol_id = statement.column_int64(2);
            reference.source_path = statement.column_text(3);
            reference.kind = statement.column_text(4);
            reference.target_spelling = statement.column_text(5);
            reference.qualifier = statement.column_text(6);
            reference.receiver_type = statement.column_text(7);
            reference.lexical_confidence = statement.column_double(8);
            references.push_back(std::move(reference));
        }
    }

    Statement update;
    Error error = update.prepare(
        db, "UPDATE refs SET target_symbol_id=?,confidence=?,resolution=? WHERE id=?");
    if (!error.ok()) return error;
    struct ResolvedEdge {
        sqlite3_int64 source_file_id = 0;
        sqlite3_int64 source_symbol_id = 0;
        sqlite3_int64 target_symbol_id = 0;
    };
    std::vector<ResolvedEdge> resolved_edges;
    for (const ResolverReference& reference : references) {
        if (cancelled(options))
            return {ErrorCode::Cancelled,
                    "code graph resolution cancelled; previous snapshot preserved"};
        sqlite3_int64 target_id = 0;
        double confidence = reference.lexical_confidence;
        std::string resolution = "unresolved";
        if (compatible_reference_target(reference.kind, "function") ||
            reference.kind == "instantiate" || reference.kind == "inherit" ||
            reference.kind == "use") {
            const std::string normalized_target =
                normalized_symbol_name(reference.target_spelling);
            const std::string simple = normalized_simple_name(normalized_target);
            const std::string normalized_receiver =
                normalized_symbol_name(reference.receiver_type);
            struct CandidateScore {
                int score = 0;
                std::size_t index = 0;
            };
            std::vector<CandidateScore> candidates;
            const auto bucket = by_simple.find(simple);
            if (bucket != by_simple.end()) {
                for (const std::size_t symbol_index : bucket->second) {
                    const ResolverSymbol& symbol = symbols[symbol_index];
                    if (!compatible_reference_target(reference.kind, symbol.kind))
                        continue;
                    int score = 50;
                    if (symbol.normalized_qualified == normalized_target)
                        score += 60;
                    else if (symbol.normalized_qualified.size() >
                                 normalized_target.size() + 2 &&
                             symbol.normalized_qualified.compare(
                                 symbol.normalized_qualified.size() -
                                     normalized_target.size(),
                                 normalized_target.size(), normalized_target) == 0)
                        score += 35;
                    if (symbol.path == reference.source_path) score += 24;
                    if (!normalized_receiver.empty()) {
                        const std::string owner_suffix =
                            normalized_receiver + "::" + simple;
                        if (symbol.normalized_qualified == owner_suffix ||
                            (symbol.normalized_qualified.size() >
                                 owner_suffix.size() + 2 &&
                             symbol.normalized_qualified.compare(
                                 symbol.normalized_qualified.size() -
                                     owner_suffix.size(),
                                 owner_suffix.size(), owner_suffix) == 0))
                            score += 48;
                    }
                    // Prefer a body over a one-line declaration when both share a
                    // qualified name. Overloads with equally plausible bodies stay
                    // ambiguous.
                    if (symbol.line_end > symbol.line_start) score += 30;
                    candidates.push_back({score, symbol_index});
                }
            }
            std::sort(candidates.begin(), candidates.end(),
                      [&](const CandidateScore& left,
                          const CandidateScore& right) {
                          if (left.score != right.score)
                              return left.score > right.score;
                          return symbols[left.index].path <
                                     symbols[right.index].path;
                      });
            if (!candidates.empty()) {
                const bool unique_best =
                    candidates.size() == 1 ||
                    candidates[0].score > candidates[1].score;
                if (unique_best) {
                    target_id = symbols[candidates[0].index].id;
                    resolution = "resolved";
                    if (candidates[0].score >= 105)
                        confidence = std::max(confidence, 0.95);
                    else if (!normalized_receiver.empty())
                        confidence = std::max(confidence, 0.82);
                    else if (symbols[candidates[0].index].path ==
                             reference.source_path)
                        confidence = std::max(confidence, 0.78);
                    else
                        confidence = std::max(confidence, 0.65);
                    resolved_edges.push_back(
                        {reference.source_file_id, reference.source_symbol_id,
                         target_id});
                } else {
                    resolution = "ambiguous";
                    confidence = std::min(confidence, 0.49);
                }
            }
        }

        int parameter = 1;
        if (target_id == 0)
            error = update.bind_null(db, parameter++);
        else
            error = update.bind_int64(db, parameter++, target_id);
        if (!error.ok() ||
            !(error = update.bind_double(db, parameter++, confidence)).ok() ||
            !(error = update.bind_text(db, parameter++, resolution)).ok() ||
            !(error = update.bind_int64(db, parameter, reference.id)).ok())
            return error;
        if (update.step() != SQLITE_DONE)
            return sqlite_error(db.get(), "could not publish resolved reference",
                                db.path());
        update.reset();
    }

    std::map<sqlite3_int64, std::set<sqlite3_int64>> callers;
    for (const ResolvedEdge& edge : resolved_edges) {
        const sqlite3_int64 caller =
            edge.source_symbol_id != 0 ? edge.source_symbol_id
                                       : -edge.source_file_id;
        callers[edge.target_symbol_id].insert(caller);
    }

    std::vector<double> rank(symbols.size(), 0.0);
    std::map<sqlite3_int64, std::size_t> symbol_position;
    for (std::size_t index = 0; index < symbols.size(); ++index)
        symbol_position[symbols[index].id] = index;
    if (!symbols.empty()) {
        const double initial = 1.0 / static_cast<double>(symbols.size());
        std::fill(rank.begin(), rank.end(), initial);
        std::vector<std::set<std::size_t>> outgoing(symbols.size());
        for (const ResolvedEdge& edge : resolved_edges) {
            if (edge.source_symbol_id == 0) continue;
            const auto source = symbol_position.find(edge.source_symbol_id);
            const auto target = symbol_position.find(edge.target_symbol_id);
            if (source != symbol_position.end() &&
                target != symbol_position.end() &&
                source->second != target->second)
                outgoing[source->second].insert(target->second);
        }
        constexpr double damping = 0.85;
        for (int iteration = 0; iteration < 20; ++iteration) {
            std::vector<double> next(
                symbols.size(),
                (1.0 - damping) / static_cast<double>(symbols.size()));
            double dangling = 0.0;
            for (std::size_t source = 0; source < symbols.size(); ++source) {
                if (outgoing[source].empty()) {
                    dangling += rank[source];
                    continue;
                }
                const double share =
                    damping * rank[source] /
                    static_cast<double>(outgoing[source].size());
                for (const std::size_t target : outgoing[source])
                    next[target] += share;
            }
            const double dangling_share =
                damping * dangling / static_cast<double>(symbols.size());
            for (double& value : next) value += dangling_share;
            rank.swap(next);
        }
    }

    if (!(error = db.exec("DELETE FROM symbol_scores")).ok()) return error;
    Statement insert_score;
    if (!(error = insert_score.prepare(
              db, "INSERT INTO symbol_scores(symbol_id,caller_count,page_rank)"
                  " VALUES(?,?,?)")).ok())
        return error;
    for (std::size_t index = 0; index < symbols.size(); ++index) {
        const std::size_t caller_count = callers[symbols[index].id].size();
        if (!(error = insert_score.bind_int64(
                  db, 1, symbols[index].id)).ok() ||
            !(error = insert_score.bind_int64(
                  db, 2, static_cast<sqlite3_int64>(caller_count))).ok() ||
            !(error = insert_score.bind_double(
                  db, 3, rank.empty() ? 0.0 : rank[index])).ok())
            return error;
        if (insert_score.step() != SQLITE_DONE)
            return sqlite_error(db.get(), "could not store symbol graph score",
                                db.path());
        insert_score.reset();
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
    return (fs::path(workspace) / kProjectStateDirName / "index.sqlite").string();
}

Error clear_database(const Options& options, ClearStats& stats) {
    fs::path root;
    Error error = workspace_root(options.workspace, root);
    if (!error.ok()) return error;

    const fs::path state_directory = root / kProjectStateDirName;
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

    const fs::path state_directory = root / kProjectStateDirName;
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
    std::string stored_schema;
    if (!(error = metadata(db, "schema_version", stored_schema, found)).ok())
        return error;
    const bool schema_changed =
        !found || stored_schema != std::to_string(kSchemaVersion);
    std::string stored_scanner;
    if (!(error = metadata(db, "scanner_version", stored_scanner, found)).ok()) return error;
    const bool scanner_changed = !found || stored_scanner != std::to_string(kScannerVersion);
    std::string stored_limit;
    if (!(error = metadata(db, "max_source_code_file_size", stored_limit, found)).ok()) return error;
    const bool limit_changed = !found || stored_limit != std::to_string(options.max_source_code_file_size);

    std::set<std::string> force_only;
    for (const std::string& path : options.update_paths) {
        if (!path.empty() && path != ".") force_only.insert(fs::path(path).generic_string());
    }
    const bool path_filter = !force_only.empty();

    std::set<std::string> current_paths;
    std::vector<Candidate> changed;
    for (const Candidate& candidate : candidates) {
        current_paths.insert(candidate.path);
        if (path_filter && force_only.find(candidate.path) == force_only.end()) {
            // Outside the requested path set: leave the existing record as-is.
            ++stats.unchanged;
            continue;
        }
        const auto old = existing.find(candidate.path);
        if (!options.force_rescan && !scanner_changed && !limit_changed && old != existing.end() &&
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
    const std::size_t available = hardware == 0 ? 4 : hardware;
    const std::size_t automatic =
        std::max<std::size_t>(1, (available * 3) / 4);
    stats.worker_count =
        changed.empty()
            ? 0
            : std::min<std::size_t>({32, automatic, changed.size()});
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
            stats.references += file.references.size();
        } else {
            ++stats.skipped;
            stats.diagnostics.push_back(file.candidate.path + ": " + file.error);
        }
    }
    if (cancelled(options)) return {ErrorCode::Cancelled, "code indexing cancelled; previous snapshot preserved"};
    if (schema_changed || scanner_changed || !scanned.empty() ||
        !removed.empty()) {
        if (!(error = resolve_graph(db, options)).ok()) return error;
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
    const fs::path path = root / kProjectStateDirName / "index.sqlite";
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
    const fs::path path = root / kProjectStateDirName / "index.sqlite";
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

Error load_snapshot(const Options& options, Snapshot& snapshot) {
    fs::path root;
    Error error = workspace_root(options.workspace, root);
    if (!error.ok()) return error;
    Database db;
    const fs::path path = root / kProjectStateDirName / "index.sqlite";
    if (!(error = db.open(path.string(), true)).ok() ||
        !(error = validate_read_schema(db)).ok()) return error;

    Snapshot loaded;
    loaded.workspace = root.string();
    std::string updated;
    bool found = false;
    if (!(error = metadata(db, "updated_at", updated, found)).ok()) return error;
    if (found) {
        try {
            loaded.updated_at = std::stoll(updated);
        } catch (...) {
            return {ErrorCode::FileRead,
                    "invalid updated_at metadata in code index " + path.string()};
        }
    }

    Statement files;
    if (!(error = files.prepare(
              db, "SELECT id,path,language,size,mtime_ns,content_hash,line_count,scan_status,scan_error "
                  "FROM files ORDER BY path")).ok()) return error;
    for (int rc = files.step(); rc != SQLITE_DONE; rc = files.step()) {
        if (cancelled(options)) return {ErrorCode::Cancelled, "loading code index snapshot cancelled"};
        if (rc != SQLITE_ROW) return sqlite_error(db.get(), "could not read indexed files", db.path());
        IndexedFile file;
        file.id = files.column_int64(0);
        file.path = files.column_text(1);
        file.language = parse_language(files.column_text(2));
        file.size = static_cast<std::uintmax_t>(files.column_int64(3));
        file.mtime_ns = files.column_int64(4);
        file.content_hash = files.column_text(5);
        file.line_count = static_cast<std::size_t>(files.column_int64(6));
        file.status = files.column_text(7);
        file.error = files.column_text(8);
        loaded.files.push_back(std::move(file));
    }

    Statement symbols;
    if (!(error = symbols.prepare(
              db, "SELECT s.id,s.file_id,f.path,s.kind,s.name,s.qualified_name,s.signature,s.parameters,"
                  "s.return_type,s.line_start,s.line_end,s.documentation,s.signature_hash,s.body_hash "
                  "FROM symbols s JOIN files f ON f.id=s.file_id ORDER BY f.path,s.line_start,s.id")).ok())
        return error;
    for (int rc = symbols.step(); rc != SQLITE_DONE; rc = symbols.step()) {
        if (cancelled(options)) return {ErrorCode::Cancelled, "loading code index symbols cancelled"};
        if (rc != SQLITE_ROW) return sqlite_error(db.get(), "could not read indexed symbols", db.path());
        IndexedSymbol item;
        item.id = symbols.column_int64(0);
        item.file_id = symbols.column_int64(1);
        item.path = symbols.column_text(2);
        item.symbol.kind = symbols.column_text(3);
        item.symbol.name = symbols.column_text(4);
        item.symbol.qualified_name = symbols.column_text(5);
        item.symbol.signature = symbols.column_text(6);
        item.symbol.parameters = symbols.column_text(7);
        item.symbol.return_type = symbols.column_text(8);
        item.symbol.line_start = static_cast<int>(symbols.column_int64(9));
        item.symbol.line_end = static_cast<int>(symbols.column_int64(10));
        item.symbol.documentation = symbols.column_text(11);
        const auto parse_hash = [](const std::string& value) -> std::uint64_t {
            try { return std::stoull(value, nullptr, 16); } catch (...) { return 0; }
        };
        item.symbol.signature_hash = parse_hash(symbols.column_text(12));
        item.symbol.body_hash = parse_hash(symbols.column_text(13));
        loaded.symbols.push_back(std::move(item));
    }

    Statement references;
    if (!(error = references.prepare(
              db, "SELECT r.id,r.source_file_id,COALESCE(r.source_symbol_id,0),f.path,"
                  "r.kind,r.target_spelling,r.qualifier,r.receiver_type,r.evidence,r.line,"
                  "r.confidence,COALESCE(r.target_symbol_id,0),COALESCE(tf.path,''),"
                  "COALESCE(ts.qualified_name,''),r.resolution "
                  "FROM refs r JOIN files f ON f.id=r.source_file_id "
                  "LEFT JOIN symbols ts ON ts.id=r.target_symbol_id "
                  "LEFT JOIN files tf ON tf.id=ts.file_id ORDER BY f.path,r.line,r.id")).ok())
        return error;
    for (int rc = references.step(); rc != SQLITE_DONE;
         rc = references.step()) {
        if (cancelled(options))
            return {ErrorCode::Cancelled,
                    "loading code index references cancelled"};
        if (rc != SQLITE_ROW)
            return sqlite_error(db.get(), "could not read indexed references",
                                db.path());
        IndexedReference reference;
        reference.id = references.column_int64(0);
        reference.source_file_id = references.column_int64(1);
        reference.source_symbol_id = references.column_int64(2);
        reference.source_path = references.column_text(3);
        reference.kind = references.column_text(4);
        reference.target_spelling = references.column_text(5);
        reference.qualifier = references.column_text(6);
        reference.receiver_type = references.column_text(7);
        reference.evidence = references.column_text(8);
        reference.line = static_cast<int>(references.column_int64(9));
        reference.confidence = references.column_double(10);
        reference.target_symbol_id = references.column_int64(11);
        reference.target_path = references.column_text(12);
        reference.target_qualified_name = references.column_text(13);
        reference.resolution = references.column_text(14);
        loaded.references.push_back(std::move(reference));
    }

    Statement scores;
    if (!(error = scores.prepare(
              db, "SELECT symbol_id,caller_count,page_rank FROM symbol_scores "
                  "ORDER BY symbol_id")).ok())
        return error;
    for (int rc = scores.step(); rc != SQLITE_DONE; rc = scores.step()) {
        if (rc != SQLITE_ROW)
            return sqlite_error(db.get(), "could not read symbol graph scores",
                                db.path());
        SymbolScore score;
        score.symbol_id = scores.column_int64(0);
        score.caller_count =
            static_cast<std::size_t>(scores.column_int64(1));
        score.page_rank = scores.column_double(2);
        loaded.symbol_scores.push_back(std::move(score));
    }

    std::map<Language, LanguageTotal> totals;
    for (const IndexedFile& file : loaded.files) {
        LanguageTotal& total = totals[file.language];
        total.language = file.language;
        ++total.files;
        total.bytes += file.size;
        if (file.status == "indexed") total.lines += file.line_count;
    }
    for (const auto& item : totals) loaded.language_totals.push_back(item.second);
    snapshot = std::move(loaded);
    return ok_error();
}

std::size_t distinct_caller_count(const Snapshot& snapshot,
                                  long long symbol_id) {
    for (const SymbolScore& score : snapshot.symbol_scores) {
        if (score.symbol_id == symbol_id) return score.caller_count;
    }
    return 0;
}

std::vector<std::string> task_tokens(const std::string& task) {
    static const std::set<std::string> ignored = {
        "a",       "an",      "and",     "are",      "as",       "at",
        "be",      "but",     "by",      "do",       "for",      "from",
        "has",     "have",    "how",     "i",        "if",       "in",
        "into",    "is",      "it",      "me",       "of",       "on",
        "or",      "our",     "please",  "that",     "the",      "this",
        "to",      "try",     "we",      "with",     "you"};
    std::vector<std::string> tokens;
    std::string token;
    auto append = [&]() {
        if (token.size() >= 2 && ignored.find(token) == ignored.end())
            tokens.push_back(token);
        token.clear();
    };
    for (unsigned char byte : task) {
        if (std::isalnum(byte) || byte == '_' || byte == '-' || byte == '.') {
            token.push_back(
                static_cast<char>(std::tolower(static_cast<unsigned char>(byte))));
        } else {
            append();
        }
    }
    append();
    std::sort(tokens.begin(), tokens.end());
    tokens.erase(std::unique(tokens.begin(), tokens.end()), tokens.end());
    return tokens;
}

bool likely_test_path(const std::string& path) {
    const std::string lower = ascii_lower(path);
    return lower.rfind("test", 0) == 0 ||
           lower.find("/test") != std::string::npos ||
           lower.find("_test.") != std::string::npos ||
           lower.find(".test.") != std::string::npos ||
           lower.find("_spec.") != std::string::npos;
}

std::string hint_field(std::string text) {
    for (char& ch : text) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte < 0x20 || byte == 0x7f) ch = ' ';
    }
    return text;
}

std::vector<RankedSymbol> rank_task_symbols(const Snapshot& snapshot,
                                            const std::string& task,
                                            std::size_t maximum,
                                            std::size_t seed_maximum) {
    if (maximum == 0 || snapshot.symbols.empty()) return {};
    const std::vector<std::string> tokens = task_tokens(task);
    std::map<long long, double> lexical;
    std::map<long long, std::string> reasons;
    std::map<long long, SymbolScore> scores_by_id;
    double maximum_page_rank = 0.0;
    for (const SymbolScore& score : snapshot.symbol_scores) {
        maximum_page_rank = std::max(maximum_page_rank, score.page_rank);
        scores_by_id[score.symbol_id] = score;
    }

    for (const IndexedSymbol& symbol : snapshot.symbols) {
        const std::string simple = ascii_lower(symbol.symbol.name);
        const std::string qualified =
            ascii_lower(symbol.symbol.qualified_name);
        const std::string path = ascii_lower(symbol.path);
        double score = 0.0;
        for (const std::string& token : tokens) {
            if (simple == token)
                score += 8.0;
            else if (qualified == token ||
                     (qualified.size() > token.size() + 2 &&
                      qualified.compare(qualified.size() - token.size(),
                                        token.size(), token) == 0))
                score += 6.0;
            else if (simple.rfind(token, 0) == 0)
                score += 4.0;
            else if (simple.find(token) != std::string::npos ||
                     qualified.find(token) != std::string::npos)
                score += 2.5;
            if (path.find(token) != std::string::npos) score += 1.5;
        }
        lexical[symbol.id] = score;
        if (score > 0.0) reasons[symbol.id] = "task match";
    }

    std::set<long long> seeds;
    std::vector<std::pair<double, long long>> ordered_seeds;
    for (const auto& item : lexical) {
        if (item.second > 0.0) ordered_seeds.push_back({item.second, item.first});
    }
    std::sort(ordered_seeds.begin(), ordered_seeds.end(),
              [](const auto& left, const auto& right) {
                  if (left.first != right.first) return left.first > right.first;
                  return left.second < right.second;
              });
    for (std::size_t index = 0;
         index < ordered_seeds.size() && index < seed_maximum; ++index)
        seeds.insert(ordered_seeds[index].second);

    std::map<long long, double> proximity;
    for (const IndexedReference& reference : snapshot.references) {
        if (reference.resolution != "resolved" ||
            reference.target_symbol_id == 0)
            continue;
        if (reference.source_symbol_id != 0 &&
            seeds.find(reference.source_symbol_id) != seeds.end()) {
            proximity[reference.target_symbol_id] +=
                1.5 * reference.confidence;
        }
        if (seeds.find(reference.target_symbol_id) != seeds.end() &&
            reference.source_symbol_id != 0) {
            proximity[reference.source_symbol_id] +=
                2.0 * reference.confidence;
        }
    }

    std::vector<RankedSymbol> ranked;
    ranked.reserve(snapshot.symbols.size());
    const bool weak_task_match =
        ordered_seeds.empty() || ordered_seeds.front().first < 2.5;
    for (const IndexedSymbol& symbol : snapshot.symbols) {
        double page_rank = 0.0;
        std::size_t callers = 0;
        const auto stored_score = scores_by_id.find(symbol.id);
        if (stored_score != scores_by_id.end()) {
            callers = stored_score->second.caller_count;
            page_rank = stored_score->second.page_rank;
        }
        double score = lexical[symbol.id] + proximity[symbol.id];
        if (callers > 0)
            score += std::min(2.5, std::log1p(static_cast<double>(callers)));
        if (maximum_page_rank > 0.0)
            score += 0.75 * page_rank / maximum_page_rank;
        const std::string lower_name = ascii_lower(symbol.symbol.name);
        const bool architectural_kind =
            symbol.symbol.kind == "function" ||
            symbol.symbol.kind == "method" ||
            symbol.symbol.kind == "class" ||
            symbol.symbol.kind == "struct" ||
            symbol.symbol.kind == "namespace" ||
            symbol.symbol.kind == "module" ||
            symbol.symbol.kind == "package";
        if (lower_name == "main" || lower_name.rfind("run_", 0) == 0)
            score += weak_task_match ? 1.25 : 0.25;
        if (likely_test_path(symbol.path) && proximity[symbol.id] > 0.0)
            score += 1.0;
        if (!weak_task_match && lexical[symbol.id] <= 0.0 &&
            proximity[symbol.id] <= 0.0)
            continue;
        if (weak_task_match && lexical[symbol.id] <= 0.0 &&
            proximity[symbol.id] <= 0.0 && callers == 0 &&
            !architectural_kind)
            continue;
        if (score <= 0.0) continue;

        std::string reason = reasons[symbol.id];
        if (proximity[symbol.id] > 0.0)
            reason += reason.empty() ? "graph neighbor" : ", graph neighbor";
        if (likely_test_path(symbol.path))
            reason += reason.empty() ? "likely test" : ", likely test";
        if (reason.empty())
            reason = callers > 0 ? "high-impact anchor" : "architectural anchor";
        ranked.push_back({&symbol, score, callers, std::move(reason)});
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const RankedSymbol& left, const RankedSymbol& right) {
                  if (left.score != right.score) return left.score > right.score;
                  if (left.symbol->path != right.symbol->path)
                      return left.symbol->path < right.symbol->path;
                  if (left.symbol->symbol.line_start !=
                      right.symbol->symbol.line_start)
                      return left.symbol->symbol.line_start <
                             right.symbol->symbol.line_start;
                  return left.symbol->id < right.symbol->id;
              });
    if (ranked.size() > maximum) ranked.resize(maximum);
    return ranked;
}

std::string format_task_hints(const Snapshot& snapshot,
                              const std::string& task,
                              std::size_t max_symbols,
                              std::size_t max_bytes,
                              std::size_t seed_maximum) {
    if (max_symbols == 0 || max_bytes < 96) return {};
    const std::vector<RankedSymbol> ranked =
        rank_task_symbols(snapshot, task, max_symbols * 3, seed_maximum);
    if (ranked.empty()) return {};
    std::ostringstream output;
    output << "[Approximate code-index hints; verify before editing]\n";
    std::set<std::string> seen;
    std::size_t count = 0;
    std::size_t bytes = output.str().size();
    auto write_group = [&](bool related, const char* heading) {
        bool wrote_heading = false;
        for (const RankedSymbol& ranked_symbol : ranked) {
            if (ranked_symbol.symbol == nullptr || count >= max_symbols) break;
            const bool item_related =
                ranked_symbol.reason.find("graph neighbor") !=
                    std::string::npos ||
                ranked_symbol.reason.find("likely test") != std::string::npos ||
                ranked_symbol.reason.find("anchor") != std::string::npos;
            if (item_related != related) continue;
            const IndexedSymbol& symbol = *ranked_symbol.symbol;
            const std::string key =
                symbol.path + "#" + symbol.symbol.qualified_name;
            if (seen.find(key) != seen.end()) continue;
            std::ostringstream line;
            line << hint_field(symbol.path) << ": "
                 << hint_field(symbol.symbol.qualified_name)
                 << " (lines " << symbol.symbol.line_start << "-"
                 << symbol.symbol.line_end;
            if (ranked_symbol.caller_count > 0)
                line << "; " << ranked_symbol.caller_count
                     << (ranked_symbol.caller_count == 1 ? " caller"
                                                        : " callers");
            line << "; " << ranked_symbol.reason << ")\n";
            const std::string encoded = line.str();
            const std::size_t heading_bytes =
                wrote_heading ? 0 : std::strlen(heading);
            if (bytes + heading_bytes + encoded.size() > max_bytes) break;
            if (!wrote_heading) {
                output << heading;
                bytes += heading_bytes;
                wrote_heading = true;
            }
            output << encoded;
            bytes += encoded.size();
            seen.insert(key);
            ++count;
        }
    };
    write_group(false, "Task matches:\n");
    write_group(true, "Related/high-impact:\n");
    const std::string formatted = output.str();
    return count == 0 ? std::string{} : formatted;
}

std::string content_hash(const std::string& content) {
    return hash_hex(fnv1a_bytes(content));
}

}  // namespace ainiux::agent::index
