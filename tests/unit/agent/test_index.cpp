#include "agent/test_index.hpp"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <mutex>
#include <utility>
#include <vector>

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

const ainiux::agent::index::Symbol* find_symbol(const ScanResult& scan,
                                                const std::string& kind,
                                                const std::string& qualified_name) {
    for (const auto& symbol : scan.symbols) {
        if (symbol.kind == kind && symbol.qualified_name == qualified_name) return &symbol;
    }
    return nullptr;
}

std::string symbol_summary(const ScanResult& scan) {
    std::string output;
    for (const auto& symbol : scan.symbols) {
        if (!output.empty()) output += ", ";
        output += symbol.kind + ":" + symbol.qualified_name;
    }
    return output;
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

ainiux::agent::index::IndexedSymbol indexed_symbol(
    long long id,
    const std::string& path,
    const std::string& name,
    int line = 1,
    int importance = 50) {
    ainiux::agent::index::IndexedSymbol symbol;
    symbol.id = id;
    symbol.file_id = id;
    symbol.path = path;
    symbol.symbol.kind = "function";
    symbol.symbol.name = name;
    symbol.symbol.qualified_name = name;
    symbol.symbol.line_start = line;
    symbol.symbol.line_end = line + 2;
    symbol.symbol.importance = importance;
    return symbol;
}

int importance_of(const ScanResult& scan, const std::string& name) {
    for (const auto& symbol : scan.symbols)
        if (symbol.qualified_name == name) return symbol.importance;
    return -1;
}

class TestSqliteDatabase {
   public:
    ~TestSqliteDatabase() {
        if (database_ != nullptr) sqlite3_close(database_);
    }
    TestSqliteDatabase(const TestSqliteDatabase&) = delete;
    TestSqliteDatabase& operator=(const TestSqliteDatabase&) = delete;
    TestSqliteDatabase() = default;

    bool open(const fs::path& path) {
        return sqlite3_open(path.string().c_str(), &database_) == SQLITE_OK;
    }
    bool execute(const char* sql) {
        return sqlite3_exec(database_, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    }
    bool table_exists(const char* name) {
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(
                database_,
                "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?",
                -1, &statement, nullptr) != SQLITE_OK)
            return false;
        sqlite3_bind_text(statement, 1, name, -1, SQLITE_TRANSIENT);
        const bool found = sqlite3_step(statement) == SQLITE_ROW;
        sqlite3_finalize(statement);
        return found;
    }
    bool column_exists(const char* table, const char* name) {
        sqlite3_stmt* statement = nullptr;
        const std::string sql =
            std::string("PRAGMA table_info(") + table + ")";
        if (sqlite3_prepare_v2(database_, sql.c_str(), -1, &statement,
                              nullptr) != SQLITE_OK)
            return false;
        bool found = false;
        while (sqlite3_step(statement) == SQLITE_ROW) {
            const unsigned char* value = sqlite3_column_text(statement, 1);
            if (value != nullptr &&
                reinterpret_cast<const char*>(value) == std::string(name)) {
                found = true;
                break;
            }
        }
        sqlite3_finalize(statement);
        return found;
    }

   private:
    sqlite3* database_ = nullptr;
};

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
        "        return format_value(value)\n\n"
        "async def fetch(name):\n"
        "    # def hidden_too(): pass\n"
        "    widget = Widget()\n"
        "    return widget.render(name)\n";
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
        "int compact(void) { return add(1, 2); }\n";
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
        "class FriendlyGreeter : public Greeter {};\n"
        "std::string greet() { Greeter value; return value.hello(\"A\"); }\n"
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

void test_javascript_scanner() {
    const std::string source =
        "// UI widget documentation\n"
        "export class Widget {\n"
        "    readonly count: number = 0;\n"
        "    constructor(value: string) { this.count = value.length; }\n"
        "    async render(value: string): Promise<string> {\n"
        "        const localValue = value;\n"
        "        return localValue;\n"
        "    }\n"
        "}\n"
        "export async function fetchUser(id) { return id; }\n"
        "export const parse = (text: string): number => { return text.length; };\n"
        "const expression = function(value) { return value; };\n"
        "const pattern = /function hidden() {}/;\n"
        "const template = `class Ghost { method() {} }`;\n"
        "let globalValue = 1;\n";
    const ScanResult scan = ainiux::agent::index::scan_source(
        "widget.js", source, Language::JavaScript);
    check(has_symbol(scan, "class", "Widget"), "JavaScript scanner finds exported classes");
    check(has_symbol(scan, "field", "Widget::count"), "JavaScript scanner finds class fields");
    check(has_symbol(scan, "method", "Widget::constructor") &&
              has_symbol(scan, "method", "Widget::render"),
          "JavaScript scanner finds constructors and async methods");
    check(has_symbol(scan, "function", "fetchUser") &&
              has_symbol(scan, "function", "parse") &&
              has_symbol(scan, "function", "expression"),
          "JavaScript scanner finds declarations, arrows, and function expressions");
    check(has_symbol(scan, "global", "globalValue"), "JavaScript scanner finds module globals");
    check(!has_symbol(scan, "function", "hidden") && !has_symbol(scan, "class", "Ghost"),
          "JavaScript scanner masks regular expressions and template strings");
    check(!has_symbol(scan, "constant", "Widget::render::localValue"),
          "JavaScript scanner excludes local variables");
    const auto* widget = find_symbol(scan, "class", "Widget");
    check(widget != nullptr && widget->line_start == 2 && widget->line_end == 9 &&
              widget->documentation == "UI widget documentation",
          "JavaScript scanner records ranges and preceding documentation");
}

void test_typescript_scanner() {
    const std::string source =
        "export interface User {\n"
        "    readonly id: number;\n"
        "    format(prefix: string): string;\n"
        "}\n"
        "export type Maybe<T> = T | null;\n"
        "export enum State {\n"
        "    Ready,\n"
        "    Busy = 2,\n"
        "}\n"
        "export namespace Api {\n"
        "    export function load(id: number): User { return { id, format: () => '' }; }\n"
        "}\n";
    const ScanResult scan = ainiux::agent::index::scan_source(
        "models.ts", source, Language::TypeScript);
    check(has_symbol(scan, "interface", "User"), "TypeScript scanner finds interfaces");
    check(has_symbol(scan, "field", "User::id") && has_symbol(scan, "method", "User::format"),
          "TypeScript scanner finds interface fields and methods");
    check(has_symbol(scan, "type-alias", "Maybe"), "TypeScript scanner finds type aliases");
    check(has_symbol(scan, "enum", "State") && has_symbol(scan, "enum-member", "State::Ready"),
          "TypeScript scanner finds enums and members");
    check(has_symbol(scan, "namespace", "Api") && has_symbol(scan, "function", "Api::load"),
          "TypeScript scanner qualifies namespace members");
}

void test_css_scanner() {
    const std::string source =
        "/* Theme roots. */\n"
        ":root, body {\n"
        "    --brand-color: #369;\n"
        "}\n"
        ".card { color: var(--brand-color); }\n"
        "@media (width > 40rem) {\n"
        "    .card:hover { color: white; }\n"
        "}\n"
        "@keyframes fade {\n"
        "    from { opacity: 0; }\n"
        "    to { opacity: 1; }\n"
        "}\n"
        ".fake { content: \".hidden { color: red; }\"; }\n";
    const ScanResult scan = ainiux::agent::index::scan_source("theme.css", source, Language::Css);
    check(has_symbol(scan, "selector", ":root, body") && has_symbol(scan, "selector", ".card"),
          "CSS scanner finds selector rules");
    check(has_symbol(scan, "custom-property", "--brand-color"),
          "CSS scanner finds custom properties");
    check(has_symbol(scan, "at-rule", "@media (width > 40rem)"),
          "CSS scanner finds conditional at-rules");
    check(has_symbol(scan, "selector", "@media (width > 40rem)::.card:hover"),
          "CSS scanner qualifies nested rules");
    check(has_symbol(scan, "keyframes", "fade"), "CSS scanner finds keyframes");
    check(!has_symbol(scan, "selector", ".hidden"), "CSS scanner masks selectors in strings");
    const auto* root = find_symbol(scan, "selector", ":root, body");
    check(root != nullptr && root->line_start == 2 && root->line_end == 4 &&
              root->documentation == "Theme roots.",
          "CSS scanner records rule ranges and documentation");
}

void test_html_embedded_scanner() {
    const std::string source =
        "<!doctype html>\n"
        "<main id=\"app\">\n"
        "  <style>\n"
        "    .panel { --gap: 1rem; }\n"
        "  </style>\n"
        "  <script type=\"module\">\n"
        "    export function boot() { return true; }\n"
        "    const render = () => { return 'ok'; };\n"
        "  </script>\n"
        "  <script type=\"application/json\">{\"fake\": \"function hidden() {}\"}</script>\n"
        "  <user-card></user-card>\n"
        "</main>\n";
    const ScanResult scan = ainiux::agent::index::scan_source("index.html", source, Language::Html);
    check(has_symbol(scan, "element", "#app") && has_symbol(scan, "custom-element", "user-card"),
          "HTML scanner finds identified and custom elements");
    check(has_symbol(scan, "selector", ".panel") && has_symbol(scan, "custom-property", "--gap"),
          "HTML scanner delegates style blocks to CSS");
    check(has_symbol(scan, "function", "boot") && has_symbol(scan, "function", "render"),
          "HTML scanner delegates script blocks to JavaScript");
    check(!has_symbol(scan, "function", "hidden"), "HTML scanner skips data-only script blocks");
    const auto* boot = find_symbol(scan, "function", "boot");
    check(boot != nullptr && boot->line_start == 7 && boot->line_end == 7,
          "HTML scanner offsets embedded JavaScript ranges to document lines");
}

void test_web_language_detection() {
    Language language = Language::Python;
    check(ainiux::agent::index::language_for_path("module.JS", language) &&
              language == Language::JavaScript,
          "code index detects .js through editor detection");
    check(ainiux::agent::index::language_for_path("module.JSX", language) &&
              language == Language::JavaScript,
          "code index detects .jsx case-insensitively");
    check(ainiux::agent::index::language_for_path("view.TS", language) &&
              language == Language::TypeScript,
          "code index detects .ts through editor detection");
    check(ainiux::agent::index::language_for_path("view.TSX", language) &&
              language == Language::TypeScript,
          "code index detects .tsx case-insensitively");
    check(ainiux::agent::index::language_for_path("module.MTS", language) &&
              language == Language::TypeScript,
          "code index detects .mts case-insensitively");
    check(ainiux::agent::index::language_for_path("module.CTS", language) &&
              language == Language::TypeScript,
          "code index detects .cts case-insensitively");
    check(ainiux::agent::index::language_for_path("index.HTML", language) &&
              language == Language::Html,
          "code index detects HTML extensions through editor detection");
    check(ainiux::agent::index::language_for_path("theme.CSS", language) && language == Language::Css,
          "code index detects CSS extensions through editor detection");
}

void test_all_editor_language_detection() {
    const std::vector<std::pair<std::string, Language>> cases = {
        {"README.MD", Language::Markdown},       {"module.PY", Language::Python},
        {"module.C", Language::C},               {"module.CPP", Language::Cpp},
        {"module.CS", Language::CSharp},         {"module.JAVA", Language::Java},
        {"module.JSX", Language::JavaScript},    {"module.TSX", Language::TypeScript},
        {"index.HTML", Language::Html},           {"index.XHTML", Language::HtmlOnly},
        {"theme.CSS", Language::Css},             {"schema.XML", Language::Xml},
        {"data.JSONL", Language::Json},           {"script.BASH", Language::Bash},
        {"module.PHP", Language::Php},            {"module.PM", Language::Perl},
        {"Gemfile", Language::Ruby},              {"module.RS", Language::Rust},
        {"module.GO", Language::Go},              {"module.PS1", Language::PowerShell},
        {"module.S", Language::Assembly},         {"schema.SQL", Language::Sql},
        {"config.TOML", Language::Toml},          {"config.YML", Language::Yaml},
        {"config.INI", Language::Ini},
    };
    for (const auto& item : cases) {
        Language detected = Language::Python;
        check(ainiux::agent::index::language_for_path(item.first, detected) && detected == item.second,
              "code index matches editor language detection for " + item.first);
    }
    Language detected = Language::Python;
    check(!ainiux::agent::index::language_for_path("notes.txt", detected),
          "code index continues to exclude editor plain-text files");
}

void test_markdown_markup_and_data_scanners() {
    const std::string markdown =
        "# Guide\n"
        "## Setup\n"
        "[manual]: https://example.test/manual\n"
        "```md\n"
        "# Hidden\n"
        "```\n";
    ScanResult scan = ainiux::agent::index::scan_source("README.md", markdown, Language::Markdown);
    check(has_symbol(scan, "heading", "Guide") && has_symbol(scan, "heading", "Guide::Setup"),
          "Markdown scanner finds and qualifies headings");
    check(has_symbol(scan, "link-reference", "manual") && !has_symbol(scan, "heading", "Hidden"),
          "Markdown scanner finds references and ignores fenced headings");

    const std::string html_only =
        "<main id=\"app\"><script>function hidden() {}</script><user-card/></main>\n";
    scan = ainiux::agent::index::scan_source("index.xhtml", html_only, Language::HtmlOnly);
    check(scan.language == Language::HtmlOnly && has_symbol(scan, "element", "#app") &&
              !has_symbol(scan, "function", "hidden"),
          "HTML-only scanner indexes markup without embedded JavaScript");

    const std::string xml =
        "<!-- <fake id=\"hidden\"/> -->\n"
        "<xs:schema xmlns:xs=\"urn:test\">\n"
        "  <xs:element name=\"User\"/>\n"
        "</xs:schema>\n";
    scan = ainiux::agent::index::scan_source("schema.xml", xml, Language::Xml);
    check(has_symbol(scan, "element", "xs:schema") &&
              has_symbol(scan, "declaration", "xs:element::User") &&
              !has_symbol(scan, "element", "#hidden"),
          "XML scanner finds roots and named declarations while masking comments");

    const std::string json =
        "{\n"
        "  \"config\": {\"port\": 8080},\n"
        "  \"message\": \"{\\\"hidden\\\": 1}\"\n"
        "}\n"
        "{\"jsonl_key\": true}\n";
    scan = ainiux::agent::index::scan_source("data.jsonl", json, Language::Json);
    check(has_symbol(scan, "key", "config") && has_symbol(scan, "key", "config::port") &&
              has_symbol(scan, "key", "jsonl_key") && !has_symbol(scan, "key", "hidden"),
          "JSON scanner finds nested and JSONL keys without matching strings");
}

void test_managed_language_scanners() {
    const std::string csharp =
        "namespace Demo.App {\n"
        "  // Widget documentation\n"
        "  public class Widget {\n"
        "    public const int Limit = 4;\n"
        "    public string Name { get; set; }\n"
        "    public int Add(int left, int right) { return left + right; }\n"
        "    string fake = \"class Ghost { void Hidden() {} }\";\n"
        "  }\n"
        "}\n";
    ScanResult scan = ainiux::agent::index::scan_source("Widget.cs", csharp, Language::CSharp);
    check(has_symbol(scan, "namespace", "Demo.App") &&
              has_symbol(scan, "class", "Demo.App::Widget") &&
              has_symbol(scan, "constant", "Demo.App::Widget::Limit") &&
              has_symbol(scan, "property", "Demo.App::Widget::Name") &&
              has_symbol(scan, "method", "Demo.App::Widget::Add"),
          "C# scanner finds namespaces, types, fields, properties, and methods: " +
              symbol_summary(scan));
    check(!has_symbol(scan, "class", "Demo.App::Widget::Ghost"),
          "C# scanner masks declarations inside strings");
    const auto* widget = find_symbol(scan, "class", "Demo.App::Widget");
    check(widget != nullptr && widget->line_start == 3 && widget->line_end == 8 &&
              widget->documentation == "Widget documentation",
          "C# scanner records ranges and documentation");

    const std::string java =
        "package com.example;\n"
        "public final class Service {\n"
        "  private final int size = 1;\n"
        "  public String greet(String name) { return name; }\n"
        "  // interface Hidden {}\n"
        "}\n";
    scan = ainiux::agent::index::scan_source("Service.java", java, Language::Java);
    check(has_symbol(scan, "package", "com.example") &&
              has_symbol(scan, "class", "com.example::Service") &&
              has_symbol(scan, "field", "com.example::Service::size") &&
              has_symbol(scan, "method", "com.example::Service::greet") &&
              !has_symbol(scan, "interface", "com.example::Service::Hidden"),
          "Java scanner finds packages, classes, fields, and methods without comment false positives: " +
              symbol_summary(scan));
}

void test_scripting_language_scanners() {
    const std::string bash =
        "# Main entry.\n"
        "main() {\n"
        "  local inside=1\n"
        "}\n"
        "readonly LIMIT=4\n"
        "text='fake() { :; }'\n";
    ScanResult scan = ainiux::agent::index::scan_source("tool.sh", bash, Language::Bash);
    check(has_symbol(scan, "function", "main") && has_symbol(scan, "constant", "LIMIT") &&
              has_symbol(scan, "global", "text") && !has_symbol(scan, "function", "fake"),
          "Bash scanner finds functions and module variables without string false positives");

    const std::string php =
        "<?php\n"
        "namespace Demo\\Core;\n"
        "class Worker {\n"
        "  private string $name;\n"
        "  public function run(string $value): string { return $value; }\n"
        "}\n"
        "const VERSION = 1;\n"
        "function helper(): void {}\n";
    scan = ainiux::agent::index::scan_source("Worker.php", php, Language::Php);
    check(has_symbol(scan, "namespace", "Demo\\Core") &&
              has_symbol(scan, "class", "Demo\\Core::Worker") &&
              has_symbol(scan, "field", "Demo\\Core::Worker::name") &&
              has_symbol(scan, "method", "Demo\\Core::Worker::run") &&
              has_symbol(scan, "constant", "Demo\\Core::VERSION") &&
              has_symbol(scan, "function", "Demo\\Core::helper"),
          "PHP scanner finds namespaces, classes, properties, methods, constants, and functions: " +
              symbol_summary(scan));

    const std::string perl =
        "package Demo;\n"
        "use constant LIMIT => 4;\n"
        "our $VALUE = 1;\n"
        "sub run { return $VALUE; }\n"
        "my $text = 'sub hidden {}';\n";
    scan = ainiux::agent::index::scan_source("Demo.pm", perl, Language::Perl);
    check(has_symbol(scan, "package", "Demo") && has_symbol(scan, "constant", "Demo::LIMIT") &&
              has_symbol(scan, "global", "Demo::VALUE") &&
              has_symbol(scan, "function", "Demo::run") &&
              !has_symbol(scan, "function", "Demo::hidden"),
          "Perl scanner finds packages, subs, constants, and globals");

    const std::string ruby =
        "module Demo\n"
        "  VERSION = 1\n"
        "  class Worker\n"
        "    def run(value)\n"
        "      value\n"
        "    end\n"
        "  end\n"
        "end\n"
        "text = 'class Hidden; end'\n";
    scan = ainiux::agent::index::scan_source("worker.rb", ruby, Language::Ruby);
    check(has_symbol(scan, "module", "Demo") && has_symbol(scan, "constant", "Demo::VERSION") &&
              has_symbol(scan, "class", "Demo::Worker") &&
              has_symbol(scan, "method", "Demo::Worker::run") &&
              !has_symbol(scan, "class", "Hidden"),
          "Ruby scanner finds modules, classes, methods, and constants");

    const std::string powershell =
        "class Worker {\n"
        "  [string] $Name\n"
        "  [string] Render([string] $value) { return $value }\n"
        "}\n"
        "function Start-Worker { return [Worker]::new() }\n"
        "$script:Cache = @{}\n"
        "# function Hidden {}\n";
    scan = ainiux::agent::index::scan_source("Worker.ps1", powershell, Language::PowerShell);
    check(has_symbol(scan, "class", "Worker") && has_symbol(scan, "property", "Worker::Name") &&
              has_symbol(scan, "method", "Worker::Render") &&
              has_symbol(scan, "function", "Start-Worker") && has_symbol(scan, "global", "Cache") &&
              !has_symbol(scan, "function", "Hidden"),
          "PowerShell scanner finds classes, properties, methods, functions, and globals");
}

void test_systems_language_scanners() {
    const std::string rust =
        "pub mod demo {\n"
        "  pub struct Worker { value: i32 }\n"
        "  impl Worker {\n"
        "    pub fn run(&self, value: i32) -> i32 { value }\n"
        "  }\n"
        "  pub const LIMIT: i32 = 4;\n"
        "  pub fn start() {}\n"
        "  const TEXT: &str = \"fn hidden() {}\";\n"
        "}\n";
    ScanResult scan = ainiux::agent::index::scan_source("lib.rs", rust, Language::Rust);
    check(has_symbol(scan, "mod", "demo") && has_symbol(scan, "struct", "demo::Worker") &&
              has_symbol(scan, "impl", "demo::Worker") &&
              has_symbol(scan, "method", "demo::Worker::run") &&
              has_symbol(scan, "constant", "demo::LIMIT") &&
              has_symbol(scan, "function", "demo::start") &&
              !has_symbol(scan, "function", "demo::hidden"),
          "Rust scanner finds modules, types, impls, methods, functions, and constants: " +
              symbol_summary(scan));

    const std::string go =
        "package demo\n"
        "type Worker struct { Value int }\n"
        "func (w *Worker) Run(value int) int { return value }\n"
        "func Start() {}\n"
        "const Limit = 4\n"
        "var Global = 1\n"
        "var text = \"func Hidden() {}\"\n";
    scan = ainiux::agent::index::scan_source("worker.go", go, Language::Go);
    check(has_symbol(scan, "package", "demo") && has_symbol(scan, "type", "demo::Worker") &&
              has_symbol(scan, "method", "demo::Worker::Run") &&
              has_symbol(scan, "function", "demo::Start") &&
              has_symbol(scan, "constant", "demo::Limit") &&
              has_symbol(scan, "global", "demo::Global") &&
              !has_symbol(scan, "function", "demo::Hidden"),
          "Go scanner finds packages, types, methods, functions, constants, and globals: " +
              symbol_summary(scan));

    const std::string assembly =
        ".type start, @function\n"
        ".globl start\n"
        "start:\n"
        "  ret\n"
        ".equ LIMIT, 4\n"
        "; fake:\n";
    scan = ainiux::agent::index::scan_source("start.s", assembly, Language::Assembly);
    check(has_symbol(scan, "function", "start") && has_symbol(scan, "constant", "LIMIT") &&
              !has_symbol(scan, "label", "fake"),
          "Assembly scanner recognizes typed function labels and constants");
}

void test_sql_and_configuration_scanners() {
    const std::string sql =
        "-- User records.\n"
        "CREATE TABLE app.users (id INTEGER);\n"
        "CREATE VIEW app.active_users AS SELECT * FROM app.users;\n"
        "CREATE FUNCTION app.load_user(id INTEGER) RETURNS INTEGER AS 'body';\n"
        "SELECT 'CREATE TABLE hidden(id int)';\n";
    ScanResult scan = ainiux::agent::index::scan_source("schema.sql", sql, Language::Sql);
    check(has_symbol(scan, "table", "app.users") &&
              has_symbol(scan, "view", "app.active_users") &&
              has_symbol(scan, "function", "app.load_user") &&
              !has_symbol(scan, "table", "hidden"),
          "SQL scanner finds common CREATE declarations without matching strings");
    const auto* users = find_symbol(scan, "table", "app.users");
    check(users != nullptr && users->documentation == "User records.",
          "SQL scanner records preceding documentation");

    const std::string toml =
        "# Server settings.\n"
        "[server]\n"
        "port = 8080\n"
        "[[server.routes]]\n"
        "path = \"/\"\n"
        "# hidden = 1\n";
    scan = ainiux::agent::index::scan_source("config.toml", toml, Language::Toml);
    check(has_symbol(scan, "table", "server") && has_symbol(scan, "key", "server::port") &&
              has_symbol(scan, "array-table", "server.routes") &&
              has_symbol(scan, "key", "server.routes::path") && !has_symbol(scan, "key", "hidden"),
          "TOML scanner finds tables, array tables, and qualified keys");

    const std::string yaml =
        "app:\n"
        "  port: 8080\n"
        "  database:\n"
        "    host: localhost\n"
        "description: |\n"
        "  fake: value\n"
        "# hidden: true\n";
    scan = ainiux::agent::index::scan_source("config.yaml", yaml, Language::Yaml);
    check(has_symbol(scan, "mapping", "app") && has_symbol(scan, "key", "app::port") &&
              has_symbol(scan, "mapping", "app::database") &&
              has_symbol(scan, "key", "app::database::host") &&
              !has_symbol(scan, "key", "fake") && !has_symbol(scan, "key", "hidden"),
          "YAML scanner qualifies mappings and ignores block-scalar/comment content");

    const std::string ini =
        "; Database settings.\n"
        "[database]\n"
        "host = localhost\n"
        "port: 5432\n"
        "# hidden = yes\n";
    scan = ainiux::agent::index::scan_source("config.ini", ini, Language::Ini);
    check(has_symbol(scan, "section", "database") && has_symbol(scan, "key", "database::host") &&
              has_symbol(scan, "key", "database::port") && !has_symbol(scan, "key", "hidden"),
          "INI scanner finds sections and qualified keys");
}

void test_refresh_incremental_report_and_skips() {
    const fs::path root = temporary_workspace("refresh");
    write_file(root / ".gitignore", "*.py\n!keep.py\n");
    write_file(root / "keep.py", "# greeting\ndef hello(name):\n    return 'Hei, ' + name\n");
    write_file(root / "ignored.py", "def ignored():\n    pass\n");
    write_file(root / "src" / "main.c", "int main(void) { return 0; }\n");
    write_file(root / "build" / "generated.cpp", "int generated();\n");
    write_file(root / ".hidden" / "private.jsonl",
               "{\"secret_symbol\":\"must_not_be_indexed\"}\n");
    write_file(root / ".ainiux-pr" / "logs" / "agent.jsonl",
               "{\"agent_log_symbol\":\"must_not_be_indexed\"}\n");
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
    std::vector<ainiux::agent::index::DiscoveredFile> live_files;
    ainiux::Error error =
        ainiux::agent::index::discover_source_files(options, live_files);
    bool found_keep = false;
    bool found_main = false;
    bool found_ignored = false;
    for (const ainiux::agent::index::DiscoveredFile& file : live_files) {
        found_keep = found_keep || file.path == "keep.py";
        found_main = found_main || file.path == "src/main.c";
        found_ignored = found_ignored || file.path == "ignored.py" ||
                        file.path == "build/generated.cpp" ||
                        file.path == ".hidden/private.jsonl" ||
                        file.path == "linked-outside/escaped.py";
    }
    check(error.ok() && live_files.size() == 5 && found_keep &&
              found_main && !found_ignored &&
              !fs::exists(root / ".ainiux-pr" / "index.sqlite"),
          "read-only live discovery matches index eligibility without creating a database");
    runtime::CancellationSource cancelled_source;
    cancelled_source.cancel();
    ainiux::agent::index::Options cancelled_options = options;
    cancelled_options.cancellation = cancelled_source.token();
    live_files.clear();
    error = ainiux::agent::index::discover_source_files(
        cancelled_options, live_files);
    check(!error.ok() && error.code == ErrorCode::Cancelled &&
              !fs::exists(root / ".ainiux-pr" / "index.sqlite"),
          "read-only live discovery is cancellable and remains database-free");
    ainiux::agent::index::ProbeResult missing_probe;
    error = ainiux::agent::index::probe(options, missing_probe);
    check(error.ok() &&
              missing_probe.state ==
                  ainiux::agent::index::ProbeState::MissingOrIncomplete &&
              !fs::exists(root / ".ainiux-pr" / "index.sqlite"),
          "read-only probe recognizes a missing index without creating it");
    std::mutex progress_mutex;
    std::vector<ainiux::agent::index::Progress> progress;
    options.on_progress =
        [&](const ainiux::agent::index::Progress& update) {
            std::lock_guard<std::mutex> lock(progress_mutex);
            progress.push_back(update);
        };
    ainiux::agent::index::RefreshStats first;
    error = ainiux::agent::index::refresh(options, first);
    check(error.ok(), "initial project index refresh succeeds");
    check(first.discovered == 5 && first.indexed == 2 && first.skipped == 3,
          "discovery honors ignores and records binary, invalid UTF-8, and oversized skips");
    check(fs::exists(root / ".ainiux-pr" / "index.sqlite"), "project-local SQLite index is created");
    bool ordered = !progress.empty();
    int prior_phase = -1;
    std::size_t prior_completed = 0;
    for (const auto& update : progress) {
        const int phase = static_cast<int>(update.phase);
        if (phase < prior_phase) ordered = false;
        if (phase != prior_phase) {
            prior_phase = phase;
            prior_completed = 0;
        }
        if (update.completed < prior_completed) ordered = false;
        prior_completed = update.completed;
    }
    check(ordered && progress.back().phase ==
                         ainiux::agent::index::ProgressPhase::SnapshotCommit &&
              progress.back().completed == progress.back().total,
          "foreground progress phases are ordered, monotonic, and complete");

    ainiux::agent::index::ProbeResult probe;
    error = ainiux::agent::index::probe(options, probe);
    check(error.ok() &&
              probe.state == ainiux::agent::index::ProbeState::Completed,
          "read-only probe recognizes a completed index");

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
              markdown.str().find("bad.c") != std::string::npos &&
              markdown.str().find("│ Language") != std::string::npos &&
              markdown.str().find("Files") != std::string::npos &&
              markdown.str().find("Lines of code") != std::string::npos &&
              markdown.str().find("**All languages**") != std::string::npos &&
              markdown.str().find("**5**") != std::string::npos &&
              markdown.str().find("**4**") != std::string::npos &&
              markdown.str().find("**2**") != std::string::npos &&
              markdown.str().find("**3**") != std::string::npos &&
              markdown.str().find("Language: Python; lines: 3; status: indexed.") !=
                  std::string::npos &&
              markdown.str().find("Importance:") != std::string::npos &&
              markdown.str().find("Reference Graph") == std::string::npos &&
              markdown.str().find("private.jsonl") == std::string::npos &&
              markdown.str().find("agent.jsonl") == std::string::npos,
          "Markdown report includes symbols, skips, per-language lines, and combined totals");
    ainiux::agent::index::Snapshot compact_snapshot;
    error = ainiux::agent::index::load_snapshot(options, compact_snapshot);
    const std::string compact_totals =
        ainiux::agent::index::compact_totals_markdown(compact_snapshot);
    const std::size_t full_table_start = markdown.str().find(u8"┌");
    const std::size_t full_table_rule = markdown.str().find(u8"└", full_table_start);
    const std::size_t full_table_end =
        full_table_rule == std::string::npos
            ? std::string::npos
            : markdown.str().find('\n', full_table_rule);
    const std::string full_totals =
        full_table_start == std::string::npos ||
                full_table_end == std::string::npos
            ? std::string()
            : markdown.str().substr(
                  full_table_start, full_table_end - full_table_start + 1);
    check(error.ok() &&
              compact_totals.find(u8"┌") == 0 &&
              compact_totals.find("│ Language") != std::string::npos &&
              compact_totals.find("Skipped/errors") != std::string::npos &&
              compact_totals.find("**All languages**") != std::string::npos &&
              compact_totals.find("**5**") != std::string::npos &&
              compact_totals.find("# ainiux Code Index") == std::string::npos &&
              compact_totals.find("hello") == std::string::npos &&
              compact_totals == full_totals,
          "compact index report exactly matches the totals table without per-file detail");

    write_file(root / ".hidden" / "later.cpp", "int hidden_later(void);\n");
    freshness = {};
    error = ainiux::agent::index::check_freshness(options, freshness);
    check(error.ok() && freshness.fresh,
          "freshness ignores changes inside dot-prefixed directories");

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

void test_static_importance_and_lexical_ranking() {
    struct Fixture {
        const char* path;
        Language language;
        const char* source;
        const char* type;
        const char* public_method;
        const char* private_method;
        const char* global_function;
    };
    const Fixture fixtures[] = {
        {"sample.cpp", Language::Cpp,
         "class Box {\npublic:\n void open();\nprotected:\n void wait();\nprivate:\n void hide();\n};\n"
         "int launch() { return 0; }\n",
         "Box", "Box::open", "Box::hide", "launch"},
        {"sample.py", Language::Python,
         "class Box:\n    def open(self):\n        return 1\n"
         "    def _hide(self):\n        return 0\n"
         "def launch():\n    return 1\n",
         "Box", "Box::open", "Box::_hide", "launch"},
        {"sample.ts", Language::TypeScript,
         "export class Box {\n public open() {\n }\n private hide() {\n }\n}\n"
         "export function launch() {\n}\n",
         "Box", "Box::open", "Box::hide", "launch"},
        {"Sample.java", Language::Java,
         "public class Box {\n public void open() {\n }\n private void hide() {\n }\n}\n"
         "public void launch() {\n}\n",
         "Box", "Box::open", "Box::hide", "launch"},
        {"Sample.cs", Language::CSharp,
         "public class Box {\n public void Open() {\n }\n private void Hide() {\n }\n}\n"
         "public void Launch() {\n}\n",
         "Box", "Box::Open", "Box::Hide", "Launch"},
        {"sample.rs", Language::Rust,
         "pub struct Box {}\nimpl Box {\n pub fn open(&self) {\n }\n"
         " fn hide(&self) {\n }\n}\npub fn launch() {\n}\n",
         "Box", "Box::open", "Box::hide", "launch"},
        {"sample.go", Language::Go,
         "type Box struct {}\nfunc (b Box) Open() {\n}\nfunc (b Box) hide() {\n}\n"
         "func Launch() {\n}\n",
         "Box", "Box::Open", "Box::hide", "Launch"},
    };
    for (const Fixture& fixture : fixtures) {
        const ScanResult scan = ainiux::agent::index::scan_source(
            fixture.path, fixture.source, fixture.language);
        const int type = importance_of(scan, fixture.type);
        const int public_method =
            importance_of(scan, fixture.public_method);
        const int private_method =
            importance_of(scan, fixture.private_method);
        const int global =
            importance_of(scan, fixture.global_function);
        check(type >= 0 && public_method >= 0 && private_method >= 0 &&
                  type > public_method && public_method > private_method &&
                  global >= 65,
              std::string("importance ordering holds for ") + fixture.path +
                  ": " + symbol_summary(scan));
    }

    ainiux::agent::index::Snapshot snapshot;
    snapshot.symbols = {
        indexed_symbol(1, "z.cpp", "log", 7, 1),
        indexed_symbol(2, "a.cpp", "logger", 3, 100),
        indexed_symbol(3, "b.cpp", "catalog", 2, 100),
        indexed_symbol(4, "c.cpp", "log_writer", 4, 40),
    };
    const auto ranked =
        ainiux::agent::index::rank_task_symbols(snapshot, "log", 10);
    check(ranked.size() == 3 && ranked[0].symbol->id == 1 &&
              ranked[1].symbol->id == 4 &&
              ranked[2].symbol->id == 2,
          "full-name and exact-component tiers outrank importance");
    bool catalog_matched = false;
    for (const auto& hit : ranked)
        catalog_matched = catalog_matched || hit.symbol->id == 3;
    check(!catalog_matched,
          "component matching does not match log inside catalog");
}

void test_lazy_queries_match_snapshot_ranking_and_worker_policy() {
    const fs::path root = temporary_workspace("lazy-query");
    write_file(root / "z.cpp", "void log() {}\n");
    write_file(root / "a.cpp", "void logger() {}\n");
    write_file(root / "c.cpp", "void log_writer() {}\n");
    ainiux::agent::index::Options options;
    options.workspace = root.string();
    ainiux::agent::index::RefreshStats stats;
    ainiux::Error error = ainiux::agent::index::refresh(options, stats);
    check(error.ok(), "lazy-query fixture index refresh succeeds");

    ainiux::agent::index::Snapshot snapshot;
    error = ainiux::agent::index::load_snapshot(options, snapshot);
    check(error.ok(), "authorization snapshot loads for ranking comparison");
    const auto eager =
        ainiux::agent::index::rank_task_symbols(snapshot, "log", 10);
    std::vector<ainiux::agent::index::OwnedRankedSymbol> lazy;
    error = ainiux::agent::index::query_ranked_symbols(
        options, "log", 10, lazy);
    bool same = error.ok() && eager.size() == lazy.size();
    for (std::size_t i = 0; same && i < eager.size(); ++i) {
        same = eager[i].symbol->id == lazy[i].symbol.id &&
               eager[i].score == lazy[i].score &&
               eager[i].reason == lazy[i].reason;
    }
    check(same,
          "bounded lazy SQLite ranking matches eager deterministic ranking");

    std::vector<ainiux::agent::index::IndexedFile> files;
    error = ainiux::agent::index::query_files(options, files);
    ainiux::agent::index::QueryTotals totals;
    const ainiux::Error totals_error =
        ainiux::agent::index::query_totals(options, totals);
    check(error.ok() && totals_error.ok() && files.size() == 3 &&
              totals.files == 3 && totals.indexed == 3 &&
              totals.symbols == snapshot.symbols.size(),
          "lazy file and totals queries return owned completed-index records");

    ainiux::agent::index::IndexedSymbol selected;
    bool found = false;
    error = ainiux::agent::index::query_symbol(
        options, lazy.front().symbol.id, selected, found);
    check(error.ok() && found &&
              selected.id == lazy.front().symbol.id,
          "lazy symbol-id query returns an owned record");

    runtime::CancellationSource cancelled;
    cancelled.cancel();
    ainiux::agent::index::Options cancelled_options = options;
    cancelled_options.cancellation = cancelled.token();
    files.clear();
    error =
        ainiux::agent::index::query_files(cancelled_options, files);
    check(!error.ok() && error.code == ErrorCode::Cancelled,
          "lazy SQLite queries honor cancellation");

    check(ainiux::agent::index::worker_count_for(20, 100) == 16 &&
              ainiux::agent::index::worker_count_for(20, 3) == 3 &&
              ainiux::agent::index::worker_count_for(20, 0) == 0 &&
              ainiux::agent::index::worker_count_for(1, 1) == 1,
          "index worker selection uses 80 percent, bounds by work, and handles zero");

    std::error_code cleanup_error;
    fs::remove_all(root, cleanup_error);
}

void test_corrupt_index_errors() {
    const fs::path root = temporary_workspace("corrupt");
    write_file(root / "main.c", "int main(void);\n");
    write_file(root / ".ainiux-pr" / "index.sqlite", "not a sqlite database");
    ainiux::agent::index::Options options;
    options.workspace = root.string();
    ainiux::agent::index::Freshness freshness;
    ainiux::agent::index::ProbeResult probe;
    ainiux::Error probe_error =
        ainiux::agent::index::probe(options, probe);
    check(probe_error.ok() &&
              probe.state == ainiux::agent::index::ProbeState::Corrupt &&
              !probe.error.ok(),
          "read-only probe distinguishes a corrupt index");
    const ainiux::Error error = ainiux::agent::index::check_freshness(options, freshness);
    check(!error.ok() && error.code == ainiux::ErrorCode::FileRead,
          "corrupt project code index produces a read error");
    std::error_code cleanup_error;
    fs::remove_all(root, cleanup_error);
    check(!cleanup_error, "corrupt index test workspace is removed");
}

void test_schema_upgrade_adds_line_counts() {
    const fs::path root = temporary_workspace("schema-upgrade");
    write_file(root / "legacy.js", "function first() {}\nfunction second() {}\n");
    std::error_code directory_error;
    fs::create_directories(root / ".ainiux-pr", directory_error);
    check(!directory_error, "legacy index directory is created");
    {
        TestSqliteDatabase database;
        check(database.open(root / ".ainiux-pr" / "index.sqlite"), "legacy SQLite index is opened");
        check(database.execute(
                  "CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
                  "CREATE TABLE files(id INTEGER PRIMARY KEY,path TEXT NOT NULL UNIQUE,language TEXT NOT NULL,"
                  "size INTEGER NOT NULL,mtime_ns INTEGER NOT NULL,content_hash TEXT NOT NULL,"
                  "scan_status TEXT NOT NULL,scan_error TEXT NOT NULL,indexed_at INTEGER NOT NULL);"
                  "CREATE TABLE symbols(id INTEGER PRIMARY KEY,file_id INTEGER NOT NULL REFERENCES files(id) "
                  "ON DELETE CASCADE,kind TEXT NOT NULL,name TEXT NOT NULL,qualified_name TEXT NOT NULL,"
                  "signature TEXT NOT NULL,parameters TEXT NOT NULL,return_type TEXT NOT NULL,"
                  "line_start INTEGER NOT NULL,line_end INTEGER NOT NULL,documentation TEXT NOT NULL,"
                  "signature_hash TEXT NOT NULL,body_hash TEXT NOT NULL);"
                  "INSERT INTO metadata(key,value) VALUES('schema_version','1');"
                  "INSERT INTO metadata(key,value) VALUES('scanner_version','1');"
                  "INSERT INTO metadata(key,value) VALUES('complete','1');"),
              "legacy schema fixture is created");
    }

    ainiux::agent::index::Options options;
    options.workspace = root.string();
    ainiux::agent::index::RefreshStats stats;
    ainiux::Error error = ainiux::agent::index::refresh(options, stats);
    check(error.ok() && stats.indexed == 1,
          "refresh transaction upgrades a version-one index and rescans its source");
    ainiux::agent::index::Freshness freshness;
    error = ainiux::agent::index::check_freshness(options, freshness);
    std::ostringstream markdown;
    if (error.ok()) error = ainiux::agent::index::print_markdown(options, freshness, markdown);
    check(error.ok() && freshness.fresh &&
              markdown.str().find("JavaScript") != std::string::npos &&
              markdown.str().find(u8"┌") != std::string::npos &&
              markdown.str().find("**All languages**") != std::string::npos,
          "upgraded index persists and reports source line counts");
    {
        TestSqliteDatabase upgraded;
        check(upgraded.open(root / ".ainiux-pr" / "index.sqlite") &&
                  upgraded.column_exists("symbols", "importance") &&
                  !upgraded.table_exists("refs") &&
                  !upgraded.table_exists("symbol_scores"),
              "upgraded index stores importance and contains no graph tables");
    }

    std::error_code cleanup_error;
    fs::remove_all(root, cleanup_error);
    check(!cleanup_error, "schema upgrade test workspace is removed");
}

void create_graph_schema_fixture(const fs::path& root) {
    write_file(root / "sample.cpp", "int launch() { return 0; }\n");
    std::error_code error;
    fs::create_directories(root / ".ainiux-pr", error);
    TestSqliteDatabase database;
    check(database.open(root / ".ainiux-pr" / "index.sqlite") &&
              database.execute(
                  "CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
                  "CREATE TABLE files(id INTEGER PRIMARY KEY,path TEXT NOT NULL UNIQUE,language TEXT NOT NULL,"
                  "size INTEGER NOT NULL,mtime_ns INTEGER NOT NULL,content_hash TEXT NOT NULL,"
                  "line_count INTEGER NOT NULL,scan_status TEXT NOT NULL,scan_error TEXT NOT NULL,"
                  "indexed_at INTEGER NOT NULL);"
                  "CREATE TABLE symbols(id INTEGER PRIMARY KEY,file_id INTEGER NOT NULL REFERENCES files(id),"
                  "kind TEXT NOT NULL,name TEXT NOT NULL,qualified_name TEXT NOT NULL,signature TEXT NOT NULL,"
                  "parameters TEXT NOT NULL,return_type TEXT NOT NULL,line_start INTEGER NOT NULL,"
                  "line_end INTEGER NOT NULL,documentation TEXT NOT NULL,signature_hash TEXT NOT NULL,"
                  "body_hash TEXT NOT NULL);"
                  "CREATE TABLE refs(id INTEGER PRIMARY KEY,source_file_id INTEGER NOT NULL,"
                  "source_symbol_id INTEGER,kind TEXT,target_spelling TEXT,qualifier TEXT,"
                  "receiver_type TEXT,evidence TEXT,line INTEGER,confidence REAL,"
                  "target_symbol_id INTEGER,resolution TEXT);"
                  "CREATE TABLE symbol_scores(symbol_id INTEGER PRIMARY KEY,"
                  "caller_count INTEGER,page_rank REAL);"
                  "INSERT INTO metadata VALUES('schema_version','3');"
                  "INSERT INTO metadata VALUES('scanner_version','4');"
                  "INSERT INTO metadata VALUES('complete','1');"),
          "graph-schema migration fixture is created");
}

void test_graph_schema_migration_and_cancellation_rollback() {
    const fs::path migrated_root = temporary_workspace("graph-migrate");
    create_graph_schema_fixture(migrated_root);
    ainiux::agent::index::Options options;
    options.workspace = migrated_root.string();
    ainiux::agent::index::RefreshStats stats;
    ainiux::Error error = ainiux::agent::index::refresh(options, stats);
    TestSqliteDatabase migrated;
    check(error.ok() &&
              migrated.open(migrated_root / ".ainiux-pr" / "index.sqlite") &&
              migrated.column_exists("symbols", "importance") &&
              !migrated.table_exists("refs") &&
              !migrated.table_exists("symbol_scores"),
          "schema-three graph index migrates to compact definitions-only storage");

    const fs::path cancelled_root = temporary_workspace("graph-cancel");
    create_graph_schema_fixture(cancelled_root);
    runtime::CancellationSource cancellation;
    cancellation.cancel();
    options = {};
    options.workspace = cancelled_root.string();
    options.cancellation = cancellation.token();
    stats = {};
    error = ainiux::agent::index::refresh(options, stats);
    TestSqliteDatabase preserved;
    check(error.code == ErrorCode::Cancelled &&
              preserved.open(cancelled_root / ".ainiux-pr" / "index.sqlite") &&
              preserved.table_exists("refs") &&
              preserved.table_exists("symbol_scores") &&
              !preserved.column_exists("symbols", "importance"),
          "cancelled migration preserves the previous completed graph snapshot");

    std::error_code cleanup_error;
    fs::remove_all(migrated_root, cleanup_error);
    cleanup_error.clear();
    fs::remove_all(cancelled_root, cleanup_error);
}

void test_clear_database() {
    const fs::path root = temporary_workspace("clear");
    write_file(root / "component.jsx", "export function App() { return <main>Hello</main>; }\n");
    ainiux::agent::index::Options options;
    options.workspace = root.string();
    ainiux::agent::index::RefreshStats refresh_stats;
    ainiux::Error error = ainiux::agent::index::refresh(options, refresh_stats);
    check(error.ok() && refresh_stats.indexed == 1,
          "clear test creates an index containing JSX source");

    const fs::path database = root / ".ainiux-pr" / "index.sqlite";
    write_file(fs::path(database.string() + "-wal"), "stale wal");
    write_file(fs::path(database.string() + "-shm"), "stale shm");
    ainiux::agent::index::ClearStats clear_stats;
    error = ainiux::agent::index::clear_database(options, clear_stats);
    check(error.ok() && clear_stats.removed_files == 3 && !fs::exists(database) &&
              !fs::exists(fs::path(database.string() + "-wal")) &&
              !fs::exists(fs::path(database.string() + "-shm")),
          "clear-index removes the database and SQLite sidecars");

    clear_stats = {};
    error = ainiux::agent::index::clear_database(options, clear_stats);
    check(error.ok() && clear_stats.removed_files == 0,
          "clearing an absent code index is idempotent");

    std::error_code cleanup_error;
    fs::remove_all(root / ".ainiux-pr", cleanup_error);
    check(!cleanup_error, "empty index state directory is removed for symlink safety test");
    const fs::path outside = temporary_workspace("clear-outside");
    write_file(outside / "index.sqlite", "outside database");
    fs::create_directory_symlink(outside, root / ".ainiux-pr", cleanup_error);
    check(!cleanup_error, "symlinked index state directory fixture is created");
    clear_stats = {};
    error = ainiux::agent::index::clear_database(options, clear_stats);
    check(!error.ok() && fs::exists(outside / "index.sqlite"),
          "clear-index refuses a symlinked .ainiux-pr directory and preserves its target");

    cleanup_error.clear();
    fs::remove_all(root, cleanup_error);
    check(!cleanup_error, "clear index test workspace is removed");
    cleanup_error.clear();
    fs::remove_all(outside, cleanup_error);
    check(!cleanup_error, "clear index symlink target workspace is removed");
}

}  // namespace

void run_all() {
    test_python_scanner();
    test_c_scanner();
    test_cpp_header_detection();
    test_javascript_scanner();
    test_typescript_scanner();
    test_css_scanner();
    test_html_embedded_scanner();
    test_web_language_detection();
    test_all_editor_language_detection();
    test_markdown_markup_and_data_scanners();
    test_managed_language_scanners();
    test_scripting_language_scanners();
    test_systems_language_scanners();
    test_sql_and_configuration_scanners();
    test_refresh_incremental_report_and_skips();
    test_static_importance_and_lexical_ranking();
    test_lazy_queries_match_snapshot_ranking_and_worker_policy();
    test_corrupt_index_errors();
    test_schema_upgrade_adds_line_counts();
    test_graph_schema_migration_and_cancellation_rollback();
    test_clear_database();
}

}  // namespace ainiux::test::agent_index
