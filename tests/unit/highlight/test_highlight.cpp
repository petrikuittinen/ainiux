#include "highlight/test_highlight.hpp"

#include "highlight/highlight.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace ainiux::test::highlight {
namespace {

using ainiux::highlight::Language;
using ainiux::highlight::Span;
using ainiux::highlight::TokenRole;

bool has_role(const std::vector<Span>& spans, TokenRole role) {
    return std::any_of(spans.begin(), spans.end(), [&](const Span& span) { return span.role == role; });
}

bool has_exact_span(const std::vector<Span>& spans,
                    size_t start,
                    size_t end,
                    TokenRole role) {
    return std::any_of(spans.begin(), spans.end(), [&](const Span& span) {
        return span.start == start && span.end == end && span.role == role;
    });
}

bool has_role_at(const std::vector<Span>& spans, size_t byte, TokenRole role) {
    return std::any_of(spans.begin(), spans.end(), [&](const Span& span) {
        return span.start <= byte && byte < span.end && span.role == role;
    });
}

bool has_emphasis_role(const std::vector<Span>& spans) {
    return has_role(spans, TokenRole::Emphasis) ||
           has_role(spans, TokenRole::Strong) ||
           has_role(spans, TokenRole::StrongEmphasis) ||
           has_role(spans, TokenRole::Strikethrough);
}

void test_mode_parsing_and_detection() {
    const std::vector<std::pair<const char*, Language>> aliases = {
        {"text", Language::Text},       {"markdown", Language::Markdown}, {"MD", Language::Markdown},
        {"python", Language::Python},  {"py", Language::Python},         {"c", Language::C},
        {"cpp", Language::Cpp},        {"c++", Language::Cpp},           {"cxx", Language::Cpp},
        {"csharp", Language::CSharp},  {"c#", Language::CSharp},         {"cs", Language::CSharp},
        {"java", Language::Java},      {"javascript", Language::JavaScript},
        {"js", Language::JavaScript},  {"typescript", Language::TypeScript},
        {"ts", Language::TypeScript},  {"html", Language::Html},         {"html5", Language::Html},
        {"html-multi", Language::Html}, {"HTMLMULTI", Language::Html},
        {"htmlonly", Language::HtmlOnly}, {"HTML-ONLY", Language::HtmlOnly},
        {"css", Language::Css},        {"css3", Language::Css},          {"xml", Language::Xml},
        {"json", Language::Json},      {"jsonl", Language::Json},        {"ndjson", Language::Json},
        {"bash", Language::Bash},      {"sh", Language::Bash},           {"shell", Language::Bash},
        {"php", Language::Php},        {"perl", Language::Perl},         {"pl", Language::Perl},
        {"ruby", Language::Ruby},      {"rb", Language::Ruby},           {"rust", Language::Rust},
        {"rs", Language::Rust},        {"go", Language::Go},             {"golang", Language::Go},
        {"powershell", Language::PowerShell}, {"pwsh", Language::PowerShell},
        {"ps1", Language::PowerShell}, {"assembly", Language::Assembly}, {"asm", Language::Assembly},
        {"sql", Language::Sql},        {"toml", Language::Toml},         {"yaml", Language::Yaml},
        {"yml", Language::Yaml},       {"ini", Language::Ini},           {"dosini", Language::Ini},
    };
    for (const auto& alias : aliases) {
        Language language = Language::Text;
        check(ainiux::highlight::parse_language(alias.first, language) && language == alias.second,
              std::string("highlight parses mode alias: ") + alias.first);
    }
    Language language = Language::Text;
    check(!ainiux::highlight::parse_language("kotlin", language),
          "highlight rejects unsupported language modes");
    check(std::string(ainiux::highlight::language_name(Language::Html)) == "html" &&
              std::string(ainiux::highlight::language_name(Language::HtmlOnly)) == "htmlonly",
          "HTML modes expose their canonical names");
    const std::vector<std::pair<Language, const char*>> canonical_names = {
        {Language::Php, "php"},           {Language::Perl, "perl"},
        {Language::Ruby, "ruby"},         {Language::Rust, "rust"},
        {Language::Go, "go"},             {Language::PowerShell, "powershell"},
        {Language::Assembly, "assembly"}, {Language::Sql, "sql"},
        {Language::Toml, "toml"},         {Language::Yaml, "yaml"},
        {Language::Ini, "ini"},
    };
    for (const auto& canonical : canonical_names) {
        check(std::string(ainiux::highlight::language_name(canonical.first)) == canonical.second,
              std::string("highlight exposes canonical mode name: ") + canonical.second);
    }

    const std::vector<std::pair<const char*, Language>> paths = {
        {"README.md", Language::Markdown}, {"notes.MARKDOWN", Language::Markdown},
        {"draft.mdown", Language::Markdown}, {"doc.MKD", Language::Markdown},
        {"script.py", Language::Python}, {"window.pyw", Language::Python}, {"types.PYI", Language::Python},
        {"source.c", Language::C}, {"header.h", Language::C},
        {"source.cc", Language::Cpp}, {"source.cpp", Language::Cpp}, {"source.cxx", Language::Cpp},
        {"source.c++", Language::Cpp}, {"header.hh", Language::Cpp}, {"header.hpp", Language::Cpp},
        {"header.hxx", Language::Cpp}, {"header.H++", Language::Cpp}, {"body.ipp", Language::Cpp},
        {"body.tpp", Language::Cpp}, {"body.inl", Language::Cpp},
        {"program.cs", Language::CSharp}, {"Main.java", Language::Java},
        {"script.js", Language::JavaScript}, {"module.mjs", Language::JavaScript},
        {"common.cjs", Language::JavaScript}, {"view.JSX", Language::JavaScript},
        {"source.ts", Language::TypeScript}, {"module.mts", Language::TypeScript},
        {"common.cts", Language::TypeScript}, {"view.TSX", Language::TypeScript},
        {"index.html", Language::Html}, {"index.htm", Language::Html},
        {"page.xhtml", Language::HtmlOnly},
        {"site.css", Language::Css}, {"document.xml", Language::Xml}, {"schema.xsd", Language::Xml},
        {"style.xsl", Language::Xml}, {"transform.xslt", Language::Xml}, {"icon.svg", Language::Xml},
        {"data.json", Language::Json}, {"events.jsonl", Language::Json},
        {"map.geojson", Language::Json}, {"config.json5", Language::Json},
        {"build.sh", Language::Bash}, {"login.bash", Language::Bash},
        {"/home/user/.bashrc", Language::Bash}, {".bash_profile", Language::Bash},
        {".bash_login", Language::Bash}, {".bash_logout", Language::Bash}, {".profile", Language::Bash},
        {"index.php", Language::Php}, {"template.PHTML", Language::Php},
        {"script.pl", Language::Perl}, {"Module.pm", Language::Perl}, {"feature.t", Language::Perl},
        {"app.rb", Language::Ruby}, {"Rakefile", Language::Ruby}, {"package.gemspec", Language::Ruby},
        {"main.rs", Language::Rust}, {"main.go", Language::Go},
        {"profile.ps1", Language::PowerShell}, {"module.psm1", Language::PowerShell},
        {"data.psd1", Language::PowerShell}, {"boot.asm", Language::Assembly},
        {"start.S", Language::Assembly}, {"schema.sql", Language::Sql}, {"Cargo.toml", Language::Toml},
        {"config.conf", Language::Toml}, {"themes.conf", Language::Toml}, {"Ainiux.CONF", Language::Toml},
        {"compose.yaml", Language::Yaml}, {"workflow.yml", Language::Yaml},
        {"settings.ini", Language::Ini}, {"project.cfg", Language::Ini}, {".editorconfig", Language::Ini},
    };
    for (const auto& path : paths) {
        check(ainiux::highlight::detect_language(path.first) == path.second,
              std::string("highlight detects language from path: ") + path.first);
    }
    check(ainiux::highlight::detect_language("notes.txt") == Language::Text,
          "highlight treats txt as text");
    check(ainiux::highlight::detect_language("") == Language::Text,
          "highlight treats scratch buffers as text");
}

void test_programming_language_roles() {
    struct Fixture {
        Language language;
        std::string line;
        std::vector<TokenRole> roles;
        const char* name;
    };
    const std::vector<Fixture> fixtures = {
        {Language::Python, "def greet(name: str = None): # note", {TokenRole::Keyword, TokenRole::Function,
             TokenRole::Type, TokenRole::Literal, TokenRole::Comment}, "Python"},
        {Language::C, "#define N 17", {TokenRole::Preprocessor}, "C preprocessor"},
        {Language::C, "const char *s = \"return\"; /* note */", {TokenRole::Keyword, TokenRole::Type,
             TokenRole::String, TokenRole::Comment}, "C"},
        {Language::Cpp, "template <typename T> T make() { return T{}; }", {TokenRole::Keyword,
             TokenRole::Function}, "C++"},
        {Language::CSharp, "public record Person(string Name, int Age);", {TokenRole::Keyword,
             TokenRole::Type, TokenRole::Function}, "C#"},
        {Language::Java, "public static void main(String[] args) { return; }", {TokenRole::Keyword,
             TokenRole::Type, TokenRole::Function}, "Java"},
        {Language::JavaScript, "const ok = /ab+c/gi; function run() { return true; }",
             {TokenRole::Keyword, TokenRole::String, TokenRole::Function, TokenRole::Literal}, "JavaScript"},
        {Language::TypeScript, "interface User { readonly id: number; }",
             {TokenRole::Keyword, TokenRole::Type}, "TypeScript"},
        {Language::Css, "--accent: #2563eb; margin: 1.5rem; color: var(--accent);",
             {TokenRole::Property, TokenRole::Literal, TokenRole::Number, TokenRole::Function}, "CSS"},
        {Language::Json, "\"enabled\": true, \"count\": 17, \"name\": \"demo\"",
             {TokenRole::Property, TokenRole::Literal, TokenRole::Number, TokenRole::String}, "JSON"},
        {Language::Json, "{ name: 'fixture', enabled: true } // JSON5",
             {TokenRole::Property, TokenRole::String, TokenRole::Literal, TokenRole::Comment}, "JSON5"},
        {Language::Bash, "for item in ${items[@]}; do printf '%s' \"$item\"; done # note",
             {TokenRole::Keyword, TokenRole::Variable, TokenRole::Function, TokenRole::String,
              TokenRole::Comment}, "Bash"},
        {Language::Php,
             "<?php #[Override] readonly class Box { public function value(): int { return $this->value ?? 17; } } // note",
             {TokenRole::Preprocessor, TokenRole::Keyword, TokenRole::Type, TokenRole::Function,
              TokenRole::Variable, TokenRole::Number, TokenRole::Comment}, "PHP"},
        {Language::Perl, "my $value = 17; sub greet { say \"hello\"; } greet(); # note",
             {TokenRole::Keyword, TokenRole::Variable, TokenRole::Number, TokenRole::Function,
              TokenRole::String, TokenRole::Comment}, "Perl"},
        {Language::Ruby, "class Greeter; def call(name); puts(\"hello\"); :ready; end; end # note",
             {TokenRole::Keyword, TokenRole::Function, TokenRole::String, TokenRole::Literal,
              TokenRole::Comment}, "Ruby"},
        {Language::Rust, "pub async fn run(value: i32) -> Option<i32> { Some(value) } // note",
             {TokenRole::Keyword, TokenRole::Type, TokenRole::Function, TokenRole::Comment}, "Rust"},
        {Language::Go, "func main() { var count int = 17; fmt.Println(count) } // note",
             {TokenRole::Keyword, TokenRole::Type, TokenRole::Function, TokenRole::Number,
              TokenRole::Comment}, "Go"},
        {Language::PowerShell,
             "function Get-Thing { param([string]$Name) if ($true) { Write-Output \"hello\" } } # note",
             {TokenRole::Keyword, TokenRole::Type, TokenRole::Function, TokenRole::Variable,
              TokenRole::Literal, TokenRole::String, TokenRole::Comment}, "PowerShell"},
        {Language::Assembly, "start: mov %rax, $0x3c # note",
             {TokenRole::Function, TokenRole::Keyword, TokenRole::Type, TokenRole::Number,
              TokenRole::Comment}, "Assembly"},
        {Language::Sql, "SELECT count(*) FROM users WHERE active = TRUE AND id > 17; -- note",
             {TokenRole::Keyword, TokenRole::Function, TokenRole::Literal, TokenRole::Number,
              TokenRole::Comment}, "SQL"},
        {Language::Toml, "enabled = true # note",
             {TokenRole::Property, TokenRole::Literal, TokenRole::Comment}, "TOML"},
        {Language::Yaml, "name: \"demo\" # note",
             {TokenRole::Property, TokenRole::String, TokenRole::Comment}, "YAML"},
        {Language::Ini, "port = 8080 ; note",
             {TokenRole::Property, TokenRole::Number, TokenRole::Comment}, "INI"},
    };
    for (const Fixture& fixture : fixtures) {
        const auto highlighted = ainiux::highlight::highlight_line(fixture.language, fixture.line);
        for (TokenRole role : fixture.roles) {
            check(has_role(highlighted.spans, role),
                  std::string(fixture.name) + " emits requested semantic token role");
        }
    }

    const std::string c_line = "const char *text = \"return 123\";";
    const auto c = ainiux::highlight::highlight_line(Language::C, c_line);
    check(has_role_at(c.spans, c_line.find("return"), TokenRole::String) &&
              !has_role_at(c.spans, c_line.find("return"), TokenRole::Keyword),
          "strings take precedence over lower-priority C keyword and number rules");

    const std::string php_template = "<h1 class=\"title\"><?= $title ?></h1>";
    const auto php = ainiux::highlight::highlight_line(Language::Php, php_template);
    check(has_role(php.spans, TokenRole::Tag) && has_role(php.spans, TokenRole::Attribute) &&
              has_role(php.spans, TokenRole::Preprocessor) &&
              has_role(php.spans, TokenRole::Variable),
          "PHP highlights surrounding template markup and PHP delimiters");

    const std::string arm = "mov x0, #1  # exit status";
    const auto assembly = ainiux::highlight::highlight_line(Language::Assembly, arm);
    check(has_role_at(assembly.spans, arm.find("#1"), TokenRole::Number) &&
              has_role_at(assembly.spans, arm.rfind('#'), TokenRole::Comment),
          "Assembly distinguishes ARM immediates from hash comments");
}

void test_markup_and_embedded_languages() {
    const std::string html =
        "<script type=\"module\">const answer = 42;</script><p class=\"x\">Hi</p>";
    const auto html_line = ainiux::highlight::highlight_line(Language::Html, html);
    check(has_role(html_line.spans, TokenRole::Tag), "HTML highlights tag names and delimiters");
    check(has_role(html_line.spans, TokenRole::Attribute), "HTML highlights attributes");
    check(has_role(html_line.spans, TokenRole::String), "HTML highlights attribute values");
    check(has_role_at(html_line.spans, html.find("const"), TokenRole::Keyword),
          "HTML delegates script contents to JavaScript highlighting");
    check(has_role_at(html_line.spans, html.find("42"), TokenRole::Number),
          "HTML highlights JavaScript numbers inside script elements");

    const auto plain_html = ainiux::highlight::highlight_line(Language::HtmlOnly, html);
    check(has_role_at(plain_html.spans, html.find("const"), TokenRole::String) &&
              !has_role_at(plain_html.spans, html.find("const"), TokenRole::Keyword),
          "htmlonly mode keeps script bodies as markup strings");

    const std::string inline_code =
        "<button style=\"color: #fff; margin: 2px\" "
        "onclick=\"const value = 17; run(value);\">Go</button>";
    const auto inline_line =
        ainiux::highlight::highlight_line(Language::Html, inline_code);
    check(has_role_at(inline_line.spans, inline_code.find("color"), TokenRole::Property) &&
              has_role_at(inline_line.spans, inline_code.find("#fff"), TokenRole::Literal) &&
              has_role_at(inline_line.spans, inline_code.find("2px"), TokenRole::Number),
          "HTML delegates style attributes to CSS");
    check(has_role_at(inline_line.spans, inline_code.find("const"), TokenRole::Keyword) &&
              has_role_at(inline_line.spans, inline_code.find("17"), TokenRole::Number) &&
              has_role_at(inline_line.spans, inline_code.find("run"), TokenRole::Function),
          "HTML delegates event-handler attributes to JavaScript");
    const size_t style_quote = inline_code.find('"');
    check(has_role_at(inline_line.spans, style_quote, TokenRole::String),
          "HTML preserves the string role on inline-code quotes");

    const auto plain_inline = ainiux::highlight::highlight_line(Language::HtmlOnly, inline_code);
    check(has_role_at(plain_inline.spans, inline_code.find("color"), TokenRole::String) &&
              !has_role_at(plain_inline.spans, inline_code.find("color"), TokenRole::Property) &&
              has_role_at(plain_inline.spans, inline_code.find("const"), TokenRole::String) &&
              !has_role_at(plain_inline.spans, inline_code.find("const"), TokenRole::Keyword),
          "htmlonly mode does not delegate inline CSS or JavaScript");

    const std::string jsx = "return <button onClick={run}>Go</button>;";
    const auto jsx_line = ainiux::highlight::highlight_line(Language::TypeScript, jsx);
    check(has_role(jsx_line.spans, TokenRole::Tag) && has_role(jsx_line.spans, TokenRole::Attribute),
          "TypeScript/JSX highlights tags and attributes");

    const std::string xml = "<xs:element name=\"message\" type=\"xs:string\"/>";
    const auto xml_line = ainiux::highlight::highlight_line(Language::Xml, xml);
    check(has_role(xml_line.spans, TokenRole::Tag) && has_role(xml_line.spans, TokenRole::Attribute),
          "XML highlights namespace-qualified tags and attributes");
}

void test_multiline_language_states() {
    const auto c = ainiux::highlight::highlight_document(
        Language::C, "/* open\nreturn 17;\n*/ int value = 3;");
    check(c.size() == 3 && c[1].spans.size() == 1 && c[1].spans[0].role == TokenRole::Comment,
          "C block comments suppress lower-priority tokens across lines");
    check(has_role(c[2].spans, TokenRole::Type) && has_role(c[2].spans, TokenRole::Number),
          "C highlighting resumes after a block comment closes");

    const auto python = ainiux::highlight::highlight_document(
        Language::Python, "value = \"\"\"open\nreturn 17\n\"\"\"\nprint(value)");
    check(python.size() == 4 && python[1].spans.size() == 1 &&
              python[1].spans[0].role == TokenRole::String,
          "Python triple strings preserve multiline lexical state");
    check(has_role(python[3].spans, TokenRole::Function),
          "Python highlighting resumes after a triple string");

    const auto bash = ainiux::highlight::highlight_document(
        Language::Bash, "cat <<EOF\nreturn $HOME\nEOF\necho done");
    check(bash.size() == 4 && bash[1].spans.size() == 1 && bash[1].spans[0].role == TokenRole::String,
          "Bash heredoc bodies preserve explicit multiline state");
    check(has_role(bash[2].spans, TokenRole::Preprocessor), "Bash highlights heredoc terminators");

    const auto xml = ainiux::highlight::highlight_document(
        Language::Xml, "<![CDATA[<not-a-tag>\ncontinued\n]]><real id=\"1\"/>");
    check(xml.size() == 3 && xml[1].spans.size() == 1 && xml[1].spans[0].role == TokenRole::String,
          "XML CDATA preserves multiline state");
    check(has_role(xml[2].spans, TokenRole::Tag), "XML resumes tag highlighting after CDATA");

    const auto html = ainiux::highlight::highlight_document(
        Language::Html, "<style>\n/* open\n*/ color: #2563eb;\n</style>");
    check(html.size() == 4 && has_role(html[1].spans, TokenRole::Comment),
          "HTML style blocks delegate multiline comment state to CSS");
    check(has_role(html[2].spans, TokenRole::Property) && has_role(html[2].spans, TokenRole::Literal),
          "HTML style blocks resume CSS tokens after comments");

    const auto multiline_tag = ainiux::highlight::highlight_document(
        Language::Html,
        "<button\n style=\"color: #fff; margin: 2px\"\n"
        " onclick=\"const value = 17; run(value);\">Go</button>");
    check(multiline_tag.size() == 3 &&
              multiline_tag[0].next_state.block == ainiux::highlight::LineState::Block::Tag,
          "HTML retains an unfinished opening-tag state");
    check(has_role(multiline_tag[1].spans, TokenRole::Property) &&
              multiline_tag[1].next_state.block == ainiux::highlight::LineState::Block::Tag,
          "HTML highlights CSS attributes on a continued tag");
    check(has_role(multiline_tag[2].spans, TokenRole::Keyword) &&
              multiline_tag[2].next_state.block == ainiux::highlight::LineState::Block::None,
          "HTML highlights JavaScript attributes and closes a continued tag");

    const auto multiline_script_tag = ainiux::highlight::highlight_document(
        Language::Html,
        "<script\n type=\"module\">const value = 17;</script>");
    check(multiline_script_tag.size() == 2 &&
              multiline_script_tag[0].next_state.block == ainiux::highlight::LineState::Block::Tag &&
              has_role(multiline_script_tag[1].spans, TokenRole::Keyword) &&
              has_role(multiline_script_tag[1].spans, TokenRole::Number) &&
              multiline_script_tag[1].next_state.block == ainiux::highlight::LineState::Block::None,
          "HTML starts embedded JavaScript after a continued script tag closes");

    const auto fenced_html = ainiux::highlight::highlight_document(
        Language::Markdown,
        "```html\n<script>\n/* open\nstill comment\n*/ const value = 17;\n"
        "</script>\n```");
    check(fenced_html.size() == 7 && has_role(fenced_html[2].spans, TokenRole::Comment) &&
              has_role(fenced_html[3].spans, TokenRole::Comment),
          "HTML fences preserve nested JavaScript multiline-comment state");
    check(has_role(fenced_html[4].spans, TokenRole::Comment) &&
              has_role(fenced_html[4].spans, TokenRole::Keyword) &&
              has_role(fenced_html[4].spans, TokenRole::Number),
          "HTML fences resume JavaScript tokens after a nested comment closes");

    const auto php = ainiux::highlight::highlight_document(
        Language::Php, "$text = <<<TXT\nreturn 17\nTXT;\necho $text;");
    check(php.size() == 4 && has_role(php[0].spans, TokenRole::Preprocessor) &&
              php[1].spans.size() == 1 && php[1].spans[0].role == TokenRole::String &&
              has_role(php[2].spans, TokenRole::Preprocessor),
          "PHP heredocs preserve multiline string state");

    const auto perl = ainiux::highlight::highlight_document(
        Language::Perl, "=pod\nreturn 17\n=cut\nmy $value = 1;");
    check(perl.size() == 4 && has_role(perl[1].spans, TokenRole::Comment) &&
              has_role(perl[3].spans, TokenRole::Keyword),
          "Perl POD comments preserve multiline state and resume code");
    const auto perl_heredoc = ainiux::highlight::highlight_document(
        Language::Perl, "print <<TEXT;\nreturn 17\nTEXT\nsay 'done';");
    check(perl_heredoc.size() == 4 && has_role(perl_heredoc[1].spans, TokenRole::String) &&
              has_role(perl_heredoc[2].spans, TokenRole::Preprocessor),
          "Perl heredocs preserve multiline string state");

    const auto ruby = ainiux::highlight::highlight_document(
        Language::Ruby, "=begin\nreturn 17\n=end\ndef run = 1");
    check(ruby.size() == 4 && has_role(ruby[1].spans, TokenRole::Comment) &&
              has_role(ruby[3].spans, TokenRole::Keyword),
          "Ruby block comments preserve multiline state and resume code");
    const auto ruby_heredoc = ainiux::highlight::highlight_document(
        Language::Ruby, "text = <<~TEXT\n  return 17\n  TEXT\nputs(text)");
    check(ruby_heredoc.size() == 4 && has_role(ruby_heredoc[1].spans, TokenRole::String) &&
              has_role(ruby_heredoc[2].spans, TokenRole::Preprocessor),
          "Ruby squiggly heredocs accept indented terminators");

    const auto rust = ainiux::highlight::highlight_document(
        Language::Rust, "/* outer\n/* nested */\nstill */ fn run() {}\nlet raw = r##\"open\ntext\nclose\"##;");
    check(rust.size() == 6 && has_role(rust[1].spans, TokenRole::Comment) &&
              has_role(rust[2].spans, TokenRole::Comment) &&
              has_role(rust[2].spans, TokenRole::Keyword),
          "Rust nested block comments retain depth and resume code");
    check(has_role(rust[4].spans, TokenRole::String) && has_role(rust[5].spans, TokenRole::String),
          "Rust raw strings preserve hash-delimited multiline state");

    const auto go = ainiux::highlight::highlight_document(
        Language::Go, "value := `open\nreturn 17\n`\nfmt.Println(value)");
    check(go.size() == 4 && has_role(go[1].spans, TokenRole::String) &&
              has_role(go[2].spans, TokenRole::String) && has_role(go[3].spans, TokenRole::Function),
          "Go raw strings preserve multiline state and resume code");

    const auto powershell = ainiux::highlight::highlight_document(
        Language::PowerShell, "$text = @\"\nreturn 17\n\"@\nWrite-Output $text");
    check(powershell.size() == 4 && has_role(powershell[1].spans, TokenRole::String) &&
              has_role(powershell[2].spans, TokenRole::Preprocessor) &&
              has_role(powershell[3].spans, TokenRole::Function),
          "PowerShell here-strings preserve multiline state and resume commands");
    const auto powershell_comment = ainiux::highlight::highlight_document(
        Language::PowerShell, "<# open\nreturn 17\n#> function Get-Value {}");
    check(powershell_comment.size() == 3 && has_role(powershell_comment[1].spans, TokenRole::Comment) &&
              has_role(powershell_comment[2].spans, TokenRole::Comment) &&
              has_role(powershell_comment[2].spans, TokenRole::Keyword),
          "PowerShell block comments preserve state and resume code");

    const auto sql = ainiux::highlight::highlight_document(
        Language::Sql, "DO $body$\nBEGIN\nRETURN 17;\nEND\n$body$;\nSELECT 1;");
    check(sql.size() == 6 && has_role(sql[1].spans, TokenRole::String) &&
              has_role(sql[4].spans, TokenRole::String) && has_role(sql[5].spans, TokenRole::Keyword),
          "SQL dollar-quoted strings preserve multiline state and resume statements");
    const auto sql_comment = ainiux::highlight::highlight_document(
        Language::Sql, "/* open\nSELECT 17\n*/ SELECT 1;");
    check(sql_comment.size() == 3 && has_role(sql_comment[1].spans, TokenRole::Comment) &&
              has_role(sql_comment[2].spans, TokenRole::Comment) &&
              has_role(sql_comment[2].spans, TokenRole::Keyword),
          "SQL block comments preserve state and resume statements");

    const auto toml = ainiux::highlight::highlight_document(
        Language::Toml, "text = \"\"\"open\nreturn 17\n\"\"\"\nenabled = true");
    check(toml.size() == 4 && has_role(toml[1].spans, TokenRole::String) &&
              has_role(toml[2].spans, TokenRole::String) &&
              has_role(toml[3].spans, TokenRole::Literal),
          "TOML multiline strings preserve state and resume values");

    const auto yaml = ainiux::highlight::highlight_document(
        Language::Yaml, "message: |\n  return 17\nnext: true");
    check(yaml.size() == 3 && has_role(yaml[1].spans, TokenRole::String) &&
              has_role(yaml[2].spans, TokenRole::Property) &&
              has_role(yaml[2].spans, TokenRole::Literal),
          "YAML block scalars preserve indentation state and resume mappings");
}

void test_markdown_inline_and_structure() {
    const std::string text =
        "# Heading\n"
        "Plain *emphasis* and [link](https://example.test) `code` &amp;\\*\n"
        "> quoted\n"
        "- item\n"
        "<span title=\"x\">raw</span>";
    const std::vector<ainiux::highlight::HighlightedLine> lines =
        ainiux::highlight::highlight_document(Language::Markdown, text);
    check(lines.size() == 5, "Markdown highlighter preserves document line count");
    check(has_role(lines[0].spans, TokenRole::Heading), "Markdown highlights ATX headings");
    check(has_role(lines[1].spans, TokenRole::Emphasis), "Markdown highlights emphasis");
    check(has_role(lines[1].spans, TokenRole::Link), "Markdown highlights inline links");
    check(has_role(lines[1].spans, TokenRole::String), "Markdown highlights inline code");
    check(has_role(lines[1].spans, TokenRole::Literal), "Markdown highlights entities");
    check(has_role(lines[1].spans, TokenRole::Operator), "Markdown highlights escapes and heading markers");
    check(has_role(lines[2].spans, TokenRole::Operator), "Markdown highlights block quotes");
    check(has_role(lines[3].spans, TokenRole::Operator), "Markdown highlights list markers");
    check(has_role(lines[4].spans, TokenRole::Tag), "Markdown highlights raw HTML tags");

    const std::string inline_link = "See [link text](http://example.com \"title\") here";
    const std::vector<ainiux::highlight::HighlightedLine> link_lines =
        ainiux::highlight::highlight_document(Language::Markdown, inline_link);
    const size_t url_start = inline_link.find("http://");
    check(link_lines.size() == 1 && has_role(link_lines[0].spans, TokenRole::Link),
          "Markdown keeps link text and delimiters in the link role");
    check(link_lines.size() == 1 &&
              has_exact_span(link_lines[0].spans,
                             url_start,
                             url_start + std::string("http://example.com").size(),
                             TokenRole::LinkDestination),
          "Markdown gives an inline link URL its own semantic color span");

    const std::string bare_url = "Open https://example.test/docs now";
    const auto bare_url_lines =
        ainiux::highlight::highlight_document(Language::Markdown, bare_url);
    const size_t bare_url_start = bare_url.find("https://");
    check(bare_url_lines.size() == 1 &&
              has_exact_span(bare_url_lines[0].spans,
                             bare_url_start,
                             bare_url_start + std::string("https://example.test/docs").size(),
                             TokenRole::LinkDestination),
          "Markdown gives bare URLs a link-destination role");
}

void test_markdown_emphasis_delimiters_are_complete() {
    const std::vector<std::pair<std::string, TokenRole>> cases = {
        {"*emphasis*", TokenRole::Emphasis},
        {"**bold text**", TokenRole::Strong},
        {"***bold text***", TokenRole::StrongEmphasis},
        {"**bold *text***", TokenRole::Strong},
        {"_emphasis_", TokenRole::Emphasis},
        {"__bold text__", TokenRole::Strong},
        {"~~strikethrough~~", TokenRole::Strikethrough},
    };
    for (const auto& item : cases) {
        const std::string& text = item.first;
        const std::vector<ainiux::highlight::HighlightedLine> lines =
            ainiux::highlight::highlight_document(Language::Markdown, text);
        check(lines.size() == 1 &&
                  has_exact_span(lines[0].spans, 0, text.size(), item.second),
              "Markdown highlights every opening and closing emphasis delimiter byte: " + text);
    }

    ainiux::highlight::DocumentCache cache;
    cache.update({"**bold text*"}, Language::Markdown);
    size_t budget = ainiux::highlight::kDefaultFrameBudgetBytes;
    check(cache.highlight_through(0, budget),
          "Markdown cache highlights an unfinished strong-emphasis edit");
    cache.update({"**bold text**"}, Language::Markdown);
    budget = ainiux::highlight::kDefaultFrameBudgetBytes;
    check(cache.highlight_through(0, budget) && cache.line(0) != nullptr &&
              has_exact_span(cache.line(0)->spans, 0, 13, TokenRole::Strong),
          "Markdown cache includes the final asterisk after incremental typing");
}

void test_markdown_emphasis_boundaries_and_punctuation() {
    const std::vector<std::pair<std::string, TokenRole>> valid = {
        {"**Bold here:**", TokenRole::Strong},
        {"**Bold here**:", TokenRole::Strong},
        {"(**Bold here:**)", TokenRole::Strong},
        {"_emphasis_:", TokenRole::Emphasis},
        {"(_emphasis_)", TokenRole::Emphasis},
        {"~~removed:~~", TokenRole::Strikethrough},
    };
    for (const auto& item : valid) {
        const auto lines =
            ainiux::highlight::highlight_document(Language::Markdown, item.first);
        const size_t expected_start = item.first.find(item.second == TokenRole::Emphasis
                                                          ? "_"
                                                          : item.second == TokenRole::Strikethrough
                                                                ? "~~"
                                                                : "**");
        const size_t expected_end =
            item.first.rfind(item.second == TokenRole::Emphasis
                                 ? "_"
                                 : item.second == TokenRole::Strikethrough ? "~~" : "**") +
            (item.second == TokenRole::Emphasis ? 1U : 2U);
        check(lines.size() == 1 &&
                  has_exact_span(lines[0].spans, expected_start, expected_end, item.second),
              "Markdown emphasis accepts adjacent punctuation: " + item.first);
    }

    const std::vector<std::string> literals = {
        "window_width_max",
        "snake_case and CONSTANT_VALUE",
        "foo__bar__baz",
        "trailing_underscore_",
        "_ leading whitespace_",
        "_trailing whitespace _",
        R"(escaped \_marker_)",
        "`window_width_max`",
        "[window_width_max](https://example.test/a_b)",
        "https://example.test/a_b_c",
    };
    for (const std::string& text : literals) {
        const auto lines =
            ainiux::highlight::highlight_document(Language::Markdown, text);
        check(lines.size() == 1 && !has_emphasis_role(lines[0].spans),
              "Markdown leaves non-delimiter underscores literal: " + text);
    }

    const auto quote =
        ainiux::highlight::highlight_document(
            Language::Markdown, "> user prompt\nAnswer...\na > b");
    check(quote.size() == 3 &&
              has_role_at(quote[0].spans, 0, TokenRole::Operator) &&
              !has_role(quote[1].spans, TokenRole::Operator) &&
              !has_role(quote[2].spans, TokenRole::Operator),
          "Markdown recognizes only a leading greater-than sign as a blockquote marker");
}

void test_markdown_multiline_state_and_precedence() {
    const std::string text =
        "before <!-- *not emphasis*\n"
        "still [not a link](x) --> after *yes*\n"
        "```unknown\n"
        "# not a heading\n"
        "```\n"
        "after";
    const std::vector<ainiux::highlight::HighlightedLine> lines =
        ainiux::highlight::highlight_document(Language::Markdown, text);
    check(has_role(lines[0].spans, TokenRole::Comment), "Markdown opens multiline HTML comments");
    const size_t hidden_emphasis = lines[0].spans.empty() ? 0 : text.find("*not");
    (void)hidden_emphasis;
    check(!has_role(lines[0].spans, TokenRole::Emphasis),
          "Markdown does not highlight emphasis inside comments");
    check(has_role(lines[1].spans, TokenRole::Comment), "Markdown closes multiline HTML comments");
    check(has_role(lines[1].spans, TokenRole::Emphasis),
          "Markdown resumes inline highlighting after a comment closes");
    check(has_role(lines[2].spans, TokenRole::Preprocessor), "Markdown highlights opening fences");
    check(lines[3].spans.empty(),
          "Markdown unknown fenced languages stay plain and suppress heading rules");
    check(has_role(lines[4].spans, TokenRole::Preprocessor), "Markdown highlights closing fences");

    const auto tagged = ainiux::highlight::highlight_document(
        Language::Markdown, "```py\ndef greet(name: str):\n    return True\n```\n~~~not-a-mode\nconst x = 1\n~~~");
    check(tagged.size() == 7 && has_role(tagged[1].spans, TokenRole::Keyword) &&
              has_role(tagged[1].spans, TokenRole::Function) &&
              has_role(tagged[1].spans, TokenRole::Type),
          "Markdown delegates known fenced language aliases to their highlighter");
    check(has_role(tagged[2].spans, TokenRole::Literal),
          "Markdown fenced highlighter retains embedded language state");
    check(tagged[5].spans.empty(), "Markdown leaves unknown tagged fences as plain text");

    const auto partial = ainiux::highlight::highlight_document(
        Language::Markdown, "```js\nconst value = 17");
    check(partial.size() == 2 && has_role(partial[1].spans, TokenRole::Keyword) &&
              partial[1].next_state.block == ainiux::highlight::LineState::Block::Fence,
          "Markdown keeps streaming partial fences open while highlighting received code");

    const auto nested_rust = ainiux::highlight::highlight_document(
        Language::Markdown,
        "```rust\n/* outer\n/* nested\nstill nested\n*/\nstill outer\n*/ fn run() {}\n```");
    check(nested_rust.size() == 8 && has_role(nested_rust[5].spans, TokenRole::Comment) &&
              has_role(nested_rust[6].spans, TokenRole::Comment) &&
              has_role(nested_rust[6].spans, TokenRole::Keyword),
          "Markdown fences preserve nested Rust comment depth");

    // Agent/chat models often close a fence on the last code line. Leaving the
    // fence open mis-highlights following prose as shell (can't → string).
    const auto trailing_close = ainiux::highlight::highlight_document(
        Language::Markdown,
        "```sh\n"
        "make test\n"
        "# closes idle connection after ~2s instead of hanging forever```\n"
        "A stall test I can't run headlessly: open `nc 127.0.0.1 8080` and wait.\n"
        "That's exactly what `test_timeout.c` verifies.\n");
    check(trailing_close.size() >= 5, "trailing fence close keeps following prose lines");
    check(has_role(trailing_close[2].spans, TokenRole::Preprocessor),
          "Markdown accepts a closing fence appended to the last code line");
    check(trailing_close[2].next_state.block != ainiux::highlight::LineState::Block::Fence,
          "trailing fence close leaves the fence block");
    check(trailing_close[3].next_state.block != ainiux::highlight::LineState::Block::Fence,
          "prose after a trailing fence close is Markdown, not an open code fence");
    check(has_role(trailing_close[3].spans, TokenRole::String),
          "inline code after trailing fence close still highlights");
    // "can't" must not open a multi-word shell String span covering "run headlessly".
    // Markdown inline code uses String too, but only spans the short `...` runs.
    {
        bool long_string = false;
        for (const auto& span : trailing_close[3].spans) {
            if (span.role == TokenRole::String && span.end > span.start + 20) {
                long_string = true;
                break;
            }
        }
        check(!long_string,
              "apostrophe in can't must not start a long shell string after fence recovery");
    }
    check(!has_role(trailing_close[3].spans, TokenRole::Keyword),
          "prose after fence recovery must not pick up shell keywords like and/do/type");
}

void test_setext_unicode_invalid_bytes_and_budget() {
    std::string text = u8"你好 ÄÖÅ é 👨‍👩‍👧‍👦\n---\n";
    text.push_back(static_cast<char>(0xFF));
    text += " *ok*";
    const std::vector<ainiux::highlight::HighlightedLine> lines =
        ainiux::highlight::highlight_document(Language::Markdown, text);
    check(lines.size() == 3, "Markdown highlighter preserves Unicode and invalid-byte lines");
    check(lines[0].spans.size() == 1 && lines[0].spans[0].role == TokenRole::Heading,
          "Markdown highlights setext heading text retroactively");
    check(has_role(lines[1].spans, TokenRole::Operator), "Markdown highlights setext underline");
    check(has_role(lines[2].spans, TokenRole::Emphasis),
          "Markdown safely scans markup after invalid UTF-8 bytes");

    std::string invalid_code = u8"const Привет = 17; ";
    invalid_code.push_back(static_cast<char>(0xFF));
    invalid_code += " return true;";
    const auto code = ainiux::highlight::highlight_line(Language::JavaScript, invalid_code);
    check(has_role(code.spans, TokenRole::Keyword) && has_role(code.spans, TokenRole::Literal),
          "programming-language highlighting preserves Unicode and scans past invalid UTF-8");

    const std::string long_line(ainiux::highlight::kMaximumHighlightedLineBytes + 1, '*');
    const ainiux::highlight::HighlightedLine limited =
        ainiux::highlight::highlight_line(Language::Markdown, long_line);
    check(limited.work_limited && limited.spans.empty(),
          "Markdown long-line highlighting falls back to plain text");
    const ainiux::highlight::HighlightedLine limited_code =
        ainiux::highlight::highlight_line(Language::Python, long_line);
    check(limited_code.work_limited && limited_code.spans.empty(),
          "programming-language long lines fall back to plain text");
}

void test_incremental_cache_invalidation() {
    ainiux::highlight::DocumentCache cache;
    std::vector<std::string> lines = {"plain", "*em*", "tail"};
    cache.update(lines, Language::Markdown);
    size_t budget = ainiux::highlight::kDefaultFrameBudgetBytes;
    check(cache.highlight_through(2, budget) && cache.valid_line_count() == 3,
          "Markdown document cache highlights requested lines");
    check(cache.line(1) != nullptr && has_role(cache.line(1)->spans, TokenRole::Emphasis),
          "Markdown document cache stores spans");

    lines[1] = "<!-- open";
    cache.update(lines, Language::Markdown);
    check(cache.valid_line_count() <= 1,
          "Markdown document cache invalidates from before an edited line");
    budget = ainiux::highlight::kDefaultFrameBudgetBytes;
    check(cache.highlight_through(2, budget), "Markdown document cache re-highlights after edit");
    check(cache.line(2) != nullptr && cache.line(2)->next_state.block ==
                                          ainiux::highlight::LineState::Block::HtmlComment,
          "Markdown cache propagates changed multiline state");

    cache.clear();
    lines = {std::string(100, '*')};
    cache.update(lines, Language::Markdown);
    budget = 10;
    check(!cache.highlight_through(0, budget) && cache.valid_line_count() == 0,
          "Markdown cache obeys the frame work budget");
}

}  // namespace

void run_all() {
    test_mode_parsing_and_detection();
    test_programming_language_roles();
    test_markup_and_embedded_languages();
    test_multiline_language_states();
    test_markdown_inline_and_structure();
    test_markdown_emphasis_delimiters_are_complete();
    test_markdown_emphasis_boundaries_and_punctuation();
    test_markdown_multiline_state_and_precedence();
    test_setext_unicode_invalid_bytes_and_budget();
    test_incremental_cache_invalidation();
}

}  // namespace ainiux::test::highlight
