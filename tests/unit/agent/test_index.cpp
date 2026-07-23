#include "agent/test_index.hpp"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
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
    ainiux::agent::index::RefreshStats first;
    ainiux::Error error = ainiux::agent::index::refresh(options, first);
    check(error.ok(), "initial project index refresh succeeds");
    check(first.discovered == 5 && first.indexed == 2 && first.skipped == 3,
          "discovery honors ignores and records binary, invalid UTF-8, and oversized skips");
    check(fs::exists(root / ".ainiux-pr" / "index.sqlite"), "project-local SQLite index is created");

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
              markdown.str().find("| Language | Files | Lines of code | Indexed |") !=
                  std::string::npos &&
              markdown.str().find("| **All languages** | **5** | **4** | **2** | **3** |") !=
                  std::string::npos &&
              markdown.str().find("Language: Python; lines: 3; status: indexed.") !=
                  std::string::npos &&
              markdown.str().find("private.jsonl") == std::string::npos &&
              markdown.str().find("agent.jsonl") == std::string::npos,
          "Markdown report includes symbols, skips, per-language lines, and combined totals");

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

void test_corrupt_index_errors() {
    const fs::path root = temporary_workspace("corrupt");
    write_file(root / "main.c", "int main(void);\n");
    write_file(root / ".ainiux-pr" / "index.sqlite", "not a sqlite database");
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
              markdown.str().find("| JavaScript | 1 | 2 | 1 | 0 | 2 |") != std::string::npos,
          "upgraded index persists and reports source line counts");

    std::error_code cleanup_error;
    fs::remove_all(root, cleanup_error);
    check(!cleanup_error, "schema upgrade test workspace is removed");
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
    test_corrupt_index_errors();
    test_schema_upgrade_adds_line_counts();
    test_clear_database();
}

}  // namespace ainiux::test::agent_index
