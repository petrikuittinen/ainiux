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

inline constexpr int kSchemaVersion = 1;
inline constexpr int kScannerVersion = 1;

enum class Language { Python, C, Cpp };

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

struct ScanResult {
    Language language = Language::Python;
    std::vector<Symbol> symbols;
};

struct Options {
    std::string workspace = ".";
    std::size_t max_source_code_file_size = 10U * 1024U * 1024U;
    runtime::CancellationToken cancellation;
    std::function<bool()> interrupted;
};

struct RefreshStats {
    std::size_t discovered = 0;
    std::size_t unchanged = 0;
    std::size_t indexed = 0;
    std::size_t skipped = 0;
    std::size_t removed = 0;
    std::size_t symbols = 0;
    std::size_t worker_count = 0;
    long long elapsed_ms = 0;
    std::vector<std::string> diagnostics;
};

struct Freshness {
    bool fresh = false;
    std::vector<std::string> added;
    std::vector<std::string> changed;
    std::vector<std::string> removed;
    std::string reason;
};

const char* language_name(Language language);
bool language_for_path(const std::string& path, Language& language);
ScanResult scan_source(const std::string& path, const std::string& source, Language language);

std::string database_path(const std::string& workspace);
Error refresh(const Options& options, RefreshStats& stats);
Error check_freshness(const Options& options, Freshness& freshness);
Error print_markdown(const Options& options, const Freshness& freshness, std::ostream& output);

}  // namespace ainiux::agent::index
