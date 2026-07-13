#include "highlight/test_highlight.hpp"

#include "highlight/highlight.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace pkchat::test::highlight {
namespace {

using pkchat::highlight::Language;
using pkchat::highlight::Span;
using pkchat::highlight::TokenRole;

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

void test_mode_parsing_and_detection() {
    const std::vector<std::pair<const char*, Language>> aliases = {
        {"text", Language::Text},       {"markdown", Language::Markdown}, {"MD", Language::Markdown},
        {"python", Language::Python},  {"py", Language::Python},         {"c", Language::C},
        {"cpp", Language::Cpp},        {"c++", Language::Cpp},           {"cxx", Language::Cpp},
        {"csharp", Language::CSharp},  {"c#", Language::CSharp},         {"cs", Language::CSharp},
        {"java", Language::Java},      {"javascript", Language::JavaScript},
        {"js", Language::JavaScript},  {"typescript", Language::TypeScript},
        {"ts", Language::TypeScript},  {"html", Language::Html},         {"html5", Language::Html},
        {"css", Language::Css},        {"css3", Language::Css},          {"xml", Language::Xml},
        {"json", Language::Json},      {"jsonl", Language::Json},        {"ndjson", Language::Json},
        {"bash", Language::Bash},      {"sh", Language::Bash},           {"shell", Language::Bash},
    };
    for (const auto& alias : aliases) {
        Language language = Language::Text;
        check(pkchat::highlight::parse_language(alias.first, language) && language == alias.second,
              std::string("highlight parses mode alias: ") + alias.first);
    }
    Language language = Language::Text;
    check(!pkchat::highlight::parse_language("rust", language),
          "highlight rejects unsupported language modes");

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
        {"index.html", Language::Html}, {"index.htm", Language::Html}, {"page.xhtml", Language::Html},
        {"site.css", Language::Css}, {"document.xml", Language::Xml}, {"schema.xsd", Language::Xml},
        {"style.xsl", Language::Xml}, {"transform.xslt", Language::Xml}, {"icon.svg", Language::Xml},
        {"data.json", Language::Json}, {"events.jsonl", Language::Json},
        {"map.geojson", Language::Json}, {"config.json5", Language::Json},
        {"build.sh", Language::Bash}, {"login.bash", Language::Bash},
        {"/home/user/.bashrc", Language::Bash}, {".bash_profile", Language::Bash},
        {".bash_login", Language::Bash}, {".bash_logout", Language::Bash}, {".profile", Language::Bash},
    };
    for (const auto& path : paths) {
        check(pkchat::highlight::detect_language(path.first) == path.second,
              std::string("highlight detects language from path: ") + path.first);
    }
    check(pkchat::highlight::detect_language("notes.txt") == Language::Text,
          "highlight treats txt as text");
    check(pkchat::highlight::detect_language("") == Language::Text,
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
    };
    for (const Fixture& fixture : fixtures) {
        const auto highlighted = pkchat::highlight::highlight_line(fixture.language, fixture.line);
        for (TokenRole role : fixture.roles) {
            check(has_role(highlighted.spans, role),
                  std::string(fixture.name) + " emits requested semantic token role");
        }
    }

    const std::string c_line = "const char *text = \"return 123\";";
    const auto c = pkchat::highlight::highlight_line(Language::C, c_line);
    check(has_role_at(c.spans, c_line.find("return"), TokenRole::String) &&
              !has_role_at(c.spans, c_line.find("return"), TokenRole::Keyword),
          "strings take precedence over lower-priority C keyword and number rules");
}

void test_markup_and_embedded_languages() {
    const std::string html =
        "<script type=\"module\">const answer = 42;</script><p class=\"x\">Hi</p>";
    const auto html_line = pkchat::highlight::highlight_line(Language::Html, html);
    check(has_role(html_line.spans, TokenRole::Tag), "HTML highlights tag names and delimiters");
    check(has_role(html_line.spans, TokenRole::Attribute), "HTML highlights attributes");
    check(has_role(html_line.spans, TokenRole::String), "HTML highlights attribute values");
    check(has_role_at(html_line.spans, html.find("const"), TokenRole::Keyword),
          "HTML delegates script contents to JavaScript highlighting");
    check(has_role_at(html_line.spans, html.find("42"), TokenRole::Number),
          "HTML highlights JavaScript numbers inside script elements");

    const std::string jsx = "return <button onClick={run}>Go</button>;";
    const auto jsx_line = pkchat::highlight::highlight_line(Language::TypeScript, jsx);
    check(has_role(jsx_line.spans, TokenRole::Tag) && has_role(jsx_line.spans, TokenRole::Attribute),
          "TypeScript/JSX highlights tags and attributes");

    const std::string xml = "<xs:element name=\"message\" type=\"xs:string\"/>";
    const auto xml_line = pkchat::highlight::highlight_line(Language::Xml, xml);
    check(has_role(xml_line.spans, TokenRole::Tag) && has_role(xml_line.spans, TokenRole::Attribute),
          "XML highlights namespace-qualified tags and attributes");
}

void test_multiline_language_states() {
    const auto c = pkchat::highlight::highlight_document(
        Language::C, "/* open\nreturn 17;\n*/ int value = 3;");
    check(c.size() == 3 && c[1].spans.size() == 1 && c[1].spans[0].role == TokenRole::Comment,
          "C block comments suppress lower-priority tokens across lines");
    check(has_role(c[2].spans, TokenRole::Type) && has_role(c[2].spans, TokenRole::Number),
          "C highlighting resumes after a block comment closes");

    const auto python = pkchat::highlight::highlight_document(
        Language::Python, "value = \"\"\"open\nreturn 17\n\"\"\"\nprint(value)");
    check(python.size() == 4 && python[1].spans.size() == 1 &&
              python[1].spans[0].role == TokenRole::String,
          "Python triple strings preserve multiline lexical state");
    check(has_role(python[3].spans, TokenRole::Function),
          "Python highlighting resumes after a triple string");

    const auto bash = pkchat::highlight::highlight_document(
        Language::Bash, "cat <<EOF\nreturn $HOME\nEOF\necho done");
    check(bash.size() == 4 && bash[1].spans.size() == 1 && bash[1].spans[0].role == TokenRole::String,
          "Bash heredoc bodies preserve explicit multiline state");
    check(has_role(bash[2].spans, TokenRole::Preprocessor), "Bash highlights heredoc terminators");

    const auto xml = pkchat::highlight::highlight_document(
        Language::Xml, "<![CDATA[<not-a-tag>\ncontinued\n]]><real id=\"1\"/>");
    check(xml.size() == 3 && xml[1].spans.size() == 1 && xml[1].spans[0].role == TokenRole::String,
          "XML CDATA preserves multiline state");
    check(has_role(xml[2].spans, TokenRole::Tag), "XML resumes tag highlighting after CDATA");

    const auto html = pkchat::highlight::highlight_document(
        Language::Html, "<style>\n/* open\n*/ color: #2563eb;\n</style>");
    check(html.size() == 4 && has_role(html[1].spans, TokenRole::Comment),
          "HTML style blocks delegate multiline comment state to CSS");
    check(has_role(html[2].spans, TokenRole::Property) && has_role(html[2].spans, TokenRole::Literal),
          "HTML style blocks resume CSS tokens after comments");
}

void test_markdown_inline_and_structure() {
    const std::string text =
        "# Heading\n"
        "Plain *emphasis* and [link](https://example.test) `code` &amp;\\*\n"
        "> quoted\n"
        "- item\n"
        "<span title=\"x\">raw</span>";
    const std::vector<pkchat::highlight::HighlightedLine> lines =
        pkchat::highlight::highlight_document(Language::Markdown, text);
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
    const std::vector<pkchat::highlight::HighlightedLine> link_lines =
        pkchat::highlight::highlight_document(Language::Markdown, inline_link);
    const size_t url_start = inline_link.find("http://");
    check(link_lines.size() == 1 && has_role(link_lines[0].spans, TokenRole::Link),
          "Markdown keeps link text and delimiters in the link role");
    check(link_lines.size() == 1 &&
              has_exact_span(link_lines[0].spans,
                             url_start,
                             url_start + std::string("http://example.com").size(),
                             TokenRole::Attribute),
          "Markdown gives an inline link URL its own semantic color span");
}

void test_markdown_emphasis_delimiters_are_complete() {
    for (const std::string text : {"*emphasis*",
                                   "**bold text**",
                                   "***bold text***",
                                   "**bold *text***",
                                   "_emphasis_",
                                   "__bold text__",
                                   "~~strikethrough~~"}) {
        const std::vector<pkchat::highlight::HighlightedLine> lines =
            pkchat::highlight::highlight_document(Language::Markdown, text);
        check(lines.size() == 1 &&
                  has_exact_span(lines[0].spans, 0, text.size(), TokenRole::Emphasis),
              "Markdown highlights every opening and closing emphasis delimiter byte: " + text);
    }

    pkchat::highlight::DocumentCache cache;
    cache.update({"**bold text*"}, Language::Markdown);
    size_t budget = pkchat::highlight::kDefaultFrameBudgetBytes;
    check(cache.highlight_through(0, budget),
          "Markdown cache highlights an unfinished strong-emphasis edit");
    cache.update({"**bold text**"}, Language::Markdown);
    budget = pkchat::highlight::kDefaultFrameBudgetBytes;
    check(cache.highlight_through(0, budget) && cache.line(0) != nullptr &&
              has_exact_span(cache.line(0)->spans, 0, 13, TokenRole::Emphasis),
          "Markdown cache includes the final asterisk after incremental typing");
}

void test_markdown_multiline_state_and_precedence() {
    const std::string text =
        "before <!-- *not emphasis*\n"
        "still [not a link](x) --> after *yes*\n"
        "```unknown\n"
        "# not a heading\n"
        "```\n"
        "after";
    const std::vector<pkchat::highlight::HighlightedLine> lines =
        pkchat::highlight::highlight_document(Language::Markdown, text);
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

    const auto tagged = pkchat::highlight::highlight_document(
        Language::Markdown, "```py\ndef greet(name: str):\n    return True\n```\n~~~not-a-mode\nconst x = 1\n~~~");
    check(tagged.size() == 7 && has_role(tagged[1].spans, TokenRole::Keyword) &&
              has_role(tagged[1].spans, TokenRole::Function) &&
              has_role(tagged[1].spans, TokenRole::Type),
          "Markdown delegates known fenced language aliases to their highlighter");
    check(has_role(tagged[2].spans, TokenRole::Literal),
          "Markdown fenced highlighter retains embedded language state");
    check(tagged[5].spans.empty(), "Markdown leaves unknown tagged fences as plain text");

    const auto partial = pkchat::highlight::highlight_document(
        Language::Markdown, "```js\nconst value = 17");
    check(partial.size() == 2 && has_role(partial[1].spans, TokenRole::Keyword) &&
              partial[1].next_state.block == pkchat::highlight::LineState::Block::Fence,
          "Markdown keeps streaming partial fences open while highlighting received code");
}

void test_setext_unicode_invalid_bytes_and_budget() {
    std::string text = u8"你好 ÄÖÅ é 👨‍👩‍👧‍👦\n---\n";
    text.push_back(static_cast<char>(0xFF));
    text += " *ok*";
    const std::vector<pkchat::highlight::HighlightedLine> lines =
        pkchat::highlight::highlight_document(Language::Markdown, text);
    check(lines.size() == 3, "Markdown highlighter preserves Unicode and invalid-byte lines");
    check(lines[0].spans.size() == 1 && lines[0].spans[0].role == TokenRole::Heading,
          "Markdown highlights setext heading text retroactively");
    check(has_role(lines[1].spans, TokenRole::Operator), "Markdown highlights setext underline");
    check(has_role(lines[2].spans, TokenRole::Emphasis),
          "Markdown safely scans markup after invalid UTF-8 bytes");

    std::string invalid_code = u8"const Привет = 17; ";
    invalid_code.push_back(static_cast<char>(0xFF));
    invalid_code += " return true;";
    const auto code = pkchat::highlight::highlight_line(Language::JavaScript, invalid_code);
    check(has_role(code.spans, TokenRole::Keyword) && has_role(code.spans, TokenRole::Literal),
          "programming-language highlighting preserves Unicode and scans past invalid UTF-8");

    const std::string long_line(pkchat::highlight::kMaximumHighlightedLineBytes + 1, '*');
    const pkchat::highlight::HighlightedLine limited =
        pkchat::highlight::highlight_line(Language::Markdown, long_line);
    check(limited.work_limited && limited.spans.empty(),
          "Markdown long-line highlighting falls back to plain text");
    const pkchat::highlight::HighlightedLine limited_code =
        pkchat::highlight::highlight_line(Language::Python, long_line);
    check(limited_code.work_limited && limited_code.spans.empty(),
          "programming-language long lines fall back to plain text");
}

void test_incremental_cache_invalidation() {
    pkchat::highlight::DocumentCache cache;
    std::vector<std::string> lines = {"plain", "*em*", "tail"};
    cache.update(lines, Language::Markdown);
    size_t budget = pkchat::highlight::kDefaultFrameBudgetBytes;
    check(cache.highlight_through(2, budget) && cache.valid_line_count() == 3,
          "Markdown document cache highlights requested lines");
    check(cache.line(1) != nullptr && has_role(cache.line(1)->spans, TokenRole::Emphasis),
          "Markdown document cache stores spans");

    lines[1] = "<!-- open";
    cache.update(lines, Language::Markdown);
    check(cache.valid_line_count() <= 1,
          "Markdown document cache invalidates from before an edited line");
    budget = pkchat::highlight::kDefaultFrameBudgetBytes;
    check(cache.highlight_through(2, budget), "Markdown document cache re-highlights after edit");
    check(cache.line(2) != nullptr && cache.line(2)->next_state.block ==
                                          pkchat::highlight::LineState::Block::HtmlComment,
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
    test_markdown_multiline_state_and_precedence();
    test_setext_unicode_invalid_bytes_and_budget();
    test_incremental_cache_invalidation();
}

}  // namespace pkchat::test::highlight
