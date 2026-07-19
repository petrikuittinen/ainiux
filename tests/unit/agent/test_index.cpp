#include "agent/test_index.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "agent/index/index.hpp"
#include "support/test_support.hpp"

namespace ainiux::test::agent_index {
namespace {

namespace fs = std::filesystem;
using ainiux::agent::index::Language;
using ainiux::agent::index::ScanResult;
using ainiux::test::check;

bool has_symbol(const ScanResult& scan,
                const std::string& kind,
                const std::string& qualified_name) {
    for (const auto& symbol : scan.symbols) {
        if (symbol.kind == kind && symbol.qualified_name == qualified_name) return true;
    }
    return false;
}

fs::path temporary_workspace(const std::string& label) {
    const long long stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const fs::path path = fs::temp_directory_path() /
                          ("ainiux-index-test-" + label + "-" + std::to_string(stamp));
    std::error_code error;
    fs::create_directories(path, error);
    check(!error, "index test workspace is created");
    return path;
}

void write_file(const fs::path& path, const std::string& body) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(body.data(), static_cast<std::streamsize>(body.size()));
    check(output.good(), "index test fixture is written: " + path.string());
}

void test_python_scanner() {
    const std::string source =
        "# public constant\n"
        "CONSTANT = 3\n\n"
        "fake = \"def hidden(): pass\"\n"
        "# Widget docs\n"
        "class Widget:\n"
        "    \"\"\"A useful widget.\"\"\"\n"
        "    count: int = 0\n\n"
        "    @decorator\n"
        "    def render(\n"
        "        self, value: str\n"
        "    ) -> str:\n"
        "        local = 1\n"
        "        return value\n\n"
        "async def fetch(name):\n"
        "    # def hidden_too(): pass\n"
        "    return name\n";
    const ScanResult scan = ainiux::agent::index::scan_source("sample.py", source, Language::Python);
    check(has_symbol(scan, "constant", "CONSTANT"), "Python scanner finds module constant");
    check(has_symbol(scan, "class", "Widget"), "Python scanner finds class");
    check(has_symbol(scan, "field", "Widget::count"), "Python scanner finds class variable");
    check(has_symbol(scan, "method", "Widget::render"), "Python scanner finds multiline method");
    check(has_symbol(scan, "function", "fetch"), "Python scanner finds async function");
    check(!has_symbol(scan, "global", "Widget::render::local"), "Python scanner excludes local variables");
    bool documented = false;
    for (const auto& symbol : scan.symbols) {
        if (symbol.qualified_name == "Widget") documented = symbol.documentation == "A useful widget.";
    }
    check(documented, "Python scanner captures docstrings");
}

void test_c_scanner() {
    std::string source =
        "// Coordinate documentation\n"
        "struct Point {\n"
        "    int x;\n"
        "    int y;\n"
        "};\n\n"
        "typedef unsigned long Count;\n"
        "extern int global_value;\n"
        "int add(int left, int right);\n"
        "int add(int left, int right) {\n"
        "    const char *fake = \"int hidden(void);\";\n"
        "    return left + right;\n"
        "}\n"
        "int compact(void) { return 1; }\n";
    source += "#define DECLARE(name) ";
    source.push_back('\\');
    source += "\n           int name(void);\n";
    const ScanResult scan = ainiux::agent::index::scan_source("sample.c", source, Language::C);
    check(has_symbol(scan, "struct", "Point"), "C scanner finds structs");
    check(has_symbol(scan, "field", "Point::x") && has_symbol(scan, "field", "Point::y"),
          "C scanner finds struct fields");
    check(has_symbol(scan, "typedef", "Count"), "C scanner finds typedefs");
    check(has_symbol(scan, "global", "global_value"), "C scanner finds globals");
    check(has_symbol(scan, "function", "add"), "C scanner finds functions and prototypes");
    check(has_symbol(scan, "function", "compact"), "C scanner finds compact one-line functions");
    check(!has_symbol(scan, "function", "hidden"), "C scanner ignores declarations in strings");
    check(!has_symbol(scan, "function", "name"), "C scanner ignores continued macro declarations");
}

void test_cpp_header_detection() {
    const std::string source =
        "namespace demo {\n"
        "class Greeter {\n"
        "public:\n"
        "    Greeter();\n"
        "    std::string hello(const std::string& name) const;\n"
        "};\n"
        "using Count = unsigned long;\n"
        "}\n";
    const ScanResult scan = ainiux::agent::index::scan_source("greeter.h", source, Language::C);
    check(scan.language == Language::Cpp, "ambiguous .h with C++ declarations is labeled C++");
    check(has_symbol(scan, "namespace", "demo"), "C++ scanner finds namespace");
    check(has_symbol(scan, "class", "demo::Greeter"), "C++ scanner finds qualified class");
    check(has_symbol(scan, "method", "demo::Greeter::Greeter"), "C++ scanner finds constructor");
    check(has_symbol(scan, "alias", "demo::Count"), "C++ scanner finds aliases");

    const ScanResult c_header = ainiux::agent::index::scan_source(
        "plain.h", "struct Plain { int value; };\n", Language::C);
    check(c_header.language == Language::C, "plain .h remains C");
}

void test_refresh_incremental_report_and_skips() {
    const fs::path root = temporary_workspace("refresh");
    write_file(root / ".gitignore", "*.py\n!keep.py\n");
    write_file(root / "keep.py", "# greeting\ndef hello(name):\n    return 'Hei, ' + name\n");
    write_file(root / "ignored.py", "def ignored():\n    pass\n");
    write_file(root / "src" / "main.c", "int main(void) { return 0; }\n");
    write_file(root / "build" / "generated.cpp", "int generated();\n");
    write_file(root / "bad.c", std::string("int ok;\0binary", 14));
    write_file(root / "invalid.c", std::string("value = ") + static_cast<char>(0xff));
    write_file(root / "oversized.cpp", std::string(1100, ' '));
    const fs::path outside = root.string() + "-outside";
    write_file(outside / "escaped.py", "def escaped():\n    pass\n");
    std::error_code symlink_error;
    fs::create_directory_symlink(outside, root / "linked-outside", symlink_error);

    ainiux::agent::index::Options options;
    options.workspace = root.string();
    options.max_source_code_file_size = 1024;
    ainiux::agent::index::RefreshStats first;
    ainiux::Error error = ainiux::agent::index::refresh(options, first);
    check(error.ok(), "initial project index refresh succeeds");
    check(first.discovered == 5 && first.indexed == 2 && first.skipped == 3,
          "discovery honors ignores and records binary, invalid UTF-8, and oversized skips");
    check(fs::exists(root / ".ainiux" / "index.sqlite"), "project-local SQLite index is created");

    ainiux::agent::index::RefreshStats second;
    error = ainiux::agent::index::refresh(options, second);
    check(error.ok() && second.unchanged == 5 && second.indexed == 0,
          "unchanged refresh performs no source scans");

    ainiux::agent::index::Freshness freshness;
    error = ainiux::agent::index::check_freshness(options, freshness);
    check(error.ok() && freshness.fresh, "freshness check recognizes current snapshot");
    std::ostringstream markdown;
    error = ainiux::agent::index::print_markdown(options, freshness, markdown);
    check(error.ok() && markdown.str().find("# ainiux Code Index") != std::string::npos &&
              markdown.str().find("hello") != std::string::npos &&
              markdown.str().find("bad.c") != std::string::npos,
          "Markdown report includes symbols and skipped files");

    write_file(root / "src" / "new.cpp", "namespace n { int value; }\n");
    freshness = {};
    error = ainiux::agent::index::check_freshness(options, freshness);
    check(error.ok() && !freshness.fresh && freshness.added.size() == 1,
          "freshness check reports newly added source without updating the snapshot");

    ainiux::agent::index::Options cancelled = options;
    cancelled.interrupted = [] { return true; };
    ainiux::agent::index::RefreshStats cancelled_stats;
    error = ainiux::agent::index::refresh(cancelled, cancelled_stats);
    check(!error.ok() && error.code == ainiux::ErrorCode::Cancelled,
          "cancelled refresh reports cancellation");
    freshness = {};
    error = ainiux::agent::index::check_freshness(options, freshness);
    check(error.ok() && !freshness.fresh && freshness.added.size() == 1,
          "cancelled refresh preserves previous completed snapshot");

    std::error_code remove_error;
    fs::remove(root / "src" / "main.c", remove_error);
    check(!remove_error, "indexed source can be removed for incremental test");
    ainiux::agent::index::RefreshStats changed;
    error = ainiux::agent::index::refresh(options, changed);
    check(error.ok() && changed.indexed == 1 && changed.removed == 1,
          "incremental refresh adds new sources and removes deleted sources transactionally");

    write_file(root / "keep.py", "def hello(name):\n    return 'Terve ' + name\n");
    freshness = {};
    error = ainiux::agent::index::check_freshness(options, freshness);
    check(error.ok() && !freshness.fresh && freshness.changed.size() == 1,
          "freshness check reports modified source metadata");
    write_file(root / ".gitignore", "*.py\n!keep.py\n# fingerprint changed\n");
    freshness = {};
    error = ainiux::agent::index::check_freshness(options, freshness);
    check(error.ok() && !freshness.fresh && freshness.reason == "root ignore rules changed",
          "freshness check reports changed root ignore rules");

    std::error_code cleanup_error;
    fs::remove_all(root, cleanup_error);
    check(!cleanup_error, "index test workspace is removed");
    cleanup_error.clear();
    fs::remove_all(outside, cleanup_error);
    check(!cleanup_error, "symlink escape target is removed");
}

void test_corrupt_index_errors() {
    const fs::path root = temporary_workspace("corrupt");
    write_file(root / "main.c", "int main(void);\n");
    write_file(root / ".ainiux" / "index.sqlite", "not a sqlite database");
    ainiux::agent::index::Options options;
    options.workspace = root.string();
    ainiux::agent::index::Freshness freshness;
    const ainiux::Error error = ainiux::agent::index::check_freshness(options, freshness);
    check(!error.ok() && error.code == ainiux::ErrorCode::FileRead,
          "corrupt project code index produces a read error");
    std::error_code cleanup_error;
    fs::remove_all(root, cleanup_error);
    check(!cleanup_error, "corrupt index test workspace is removed");
}

}  // namespace

void run_all() {
    test_python_scanner();
    test_c_scanner();
    test_cpp_header_detection();
    test_refresh_incremental_report_and_skips();
    test_corrupt_index_errors();
}

}  // namespace ainiux::test::agent_index
