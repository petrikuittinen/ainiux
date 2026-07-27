#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

#include "common.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::agent::index {

inline constexpr int kSchemaVersion = 3;
inline constexpr int kScannerVersion = 4;

enum class Language {
    Markdown,
    Python,
    C,
    Cpp,
    CSharp,
    Java,
    JavaScript,
    TypeScript,
    Html,
    HtmlOnly,
    Css,
    Xml,
    Json,
    Bash,
    Php,
    Perl,
    Ruby,
    Rust,
    Go,
    PowerShell,
    Assembly,
    Sql,
    Toml,
    Yaml,
    Ini,
};

struct Symbol {
    std::string kind;
    std::string name;
    std::string qualified_name;
    std::string signature;
    std::string parameters;
    std::string return_type;
    int line_start = 1;
    int line_end = 1;
    std::string documentation;
    std::uint64_t signature_hash = 0;
    std::uint64_t body_hash = 0;
};

struct Reference {
    // call | import | include | inherit | instantiate | use
    std::string kind;
    std::string target_spelling;
    std::string qualifier;
    std::string receiver_type;
    std::string evidence;
    int line = 1;
    // Index into ScanResult::symbols. -1 means file/module scope.
    int source_symbol_index = -1;
    // Lexical extraction confidence before project-wide resolution.
    double confidence = 0.0;
};

struct ScanResult {
    Language language = Language::Python;
    std::vector<Symbol> symbols;
    std::vector<Reference> references;
};

struct Options {
    std::string workspace = ".";
    std::size_t max_source_code_file_size = 10U * 1024U * 1024U;
    runtime::CancellationToken cancellation;
    std::function<bool()> interrupted;
    // When true, re-scan every discovered file even if size/mtime match the DB.
    bool force_rescan = false;
    // When non-empty, only re-scan these workspace-relative paths (others stay
    // unchanged; removal detection still runs for the whole tree).
    std::vector<std::string> update_paths;
};

struct RefreshStats {
    std::size_t discovered = 0;
    std::size_t unchanged = 0;
    std::size_t indexed = 0;
    std::size_t skipped = 0;
    std::size_t removed = 0;
    std::size_t symbols = 0;
    std::size_t references = 0;
    std::size_t worker_count = 0;
    long long elapsed_ms = 0;
    std::vector<std::string> diagnostics;
};

struct ClearStats {
    std::size_t removed_files = 0;
};

struct Freshness {
    bool fresh = false;
    std::vector<std::string> added;
    std::vector<std::string> changed;
    std::vector<std::string> removed;
    std::string reason;
};

struct IndexedFile {
    long long id = 0;
    std::string path;
    Language language = Language::Python;
    std::uintmax_t size = 0;
    long long mtime_ns = 0;
    std::string content_hash;
    std::size_t line_count = 0;
    std::string status;
    std::string error;
};

struct IndexedSymbol {
    long long id = 0;
    long long file_id = 0;
    std::string path;
    Symbol symbol;
};

struct IndexedReference {
    long long id = 0;
    long long source_file_id = 0;
    long long source_symbol_id = 0;
    std::string source_path;
    std::string kind;
    std::string target_spelling;
    std::string qualifier;
    std::string receiver_type;
    std::string evidence;
    int line = 1;
    double confidence = 0.0;
    long long target_symbol_id = 0;
    std::string target_path;
    std::string target_qualified_name;
    // resolved | ambiguous | unresolved
    std::string resolution;
};

struct SymbolScore {
    long long symbol_id = 0;
    std::size_t caller_count = 0;
    double page_rank = 0.0;
};

struct LanguageTotal {
    Language language = Language::Python;
    std::size_t files = 0;
    std::size_t lines = 0;
    std::uintmax_t bytes = 0;
};

struct Snapshot {
    std::string workspace;
    long long updated_at = 0;
    std::vector<IndexedFile> files;
    std::vector<IndexedSymbol> symbols;
    std::vector<IndexedReference> references;
    std::vector<SymbolScore> symbol_scores;
    std::vector<LanguageTotal> language_totals;
};

struct RankedSymbol {
    const IndexedSymbol* symbol = nullptr;
    double score = 0.0;
    std::size_t caller_count = 0;
    std::string reason;
};

const char* language_name(Language language);
bool language_for_path(const std::string& path, Language& language);
ScanResult scan_source(const std::string& path, const std::string& source, Language language);

std::string database_path(const std::string& workspace);
Error clear_database(const Options& options, ClearStats& stats);
Error refresh(const Options& options, RefreshStats& stats);
Error check_freshness(const Options& options, Freshness& freshness);
Error print_markdown(const Options& options, const Freshness& freshness, std::ostream& output);
Error load_snapshot(const Options& options, Snapshot& snapshot);
std::string content_hash(const std::string& content);

std::size_t distinct_caller_count(const Snapshot& snapshot, long long symbol_id);
std::vector<RankedSymbol> rank_task_symbols(const Snapshot& snapshot,
                                            const std::string& task,
                                            std::size_t maximum,
                                            std::size_t seed_maximum = 8);
std::string format_task_hints(const Snapshot& snapshot,
                              const std::string& task,
                              std::size_t max_symbols = 16,
                              std::size_t max_bytes = 4U * 1024U,
                              std::size_t seed_maximum = 16);

}  // namespace ainiux::agent::index
