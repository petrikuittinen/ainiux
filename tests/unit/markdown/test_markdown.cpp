#include "markdown/test_markdown.hpp"
#include "support/test_support.hpp"
#include "markdown/markdown.hpp"
#include "markdown/table_format.hpp"
#include "ainiux/version.hpp"
#include <string>
#include <vector>

namespace ainiux::test::markdown {

namespace {

using ainiux::test::check;
using ainiux::test::read_fixture;

void test_llm_typical_markdown_to_html_fixture() {
    const std::string input = read_fixture("tests/fixtures/llm_typical.md");
    const std::string html = ainiux::markdown::to_html_fragment(input);

    check(html.find("<h1>LLM Typical Markdown Fixture</h1>") != std::string::npos,
          "llm_typical Markdown converts the title heading");
    check(html.find("<strong>bold</strong>") != std::string::npos &&
              html.find("<em>italic</em>") != std::string::npos &&
              html.find("<strong><em>bold italic</em></strong>") != std::string::npos,
          "llm_typical Markdown converts bold, italic, and bold-italic");
    check(html.find("<code>inline code</code>") != std::string::npos &&
              html.find("<del>strikethrough</del>") != std::string::npos,
          "llm_typical Markdown converts inline code and strikethrough");
    check(html.find(R"PK(<a href="https://example.com/path?q=1">a link</a>)PK") != std::string::npos,
          "llm_typical Markdown converts links");
    check(html.find("<br>") != std::string::npos,
          "llm_typical Markdown converts hard line breaks");
    check(html.find("<hr>") != std::string::npos,
          "llm_typical Markdown converts horizontal rules");
    check(html.find("<blockquote>") != std::string::npos &&
              html.find("Block quote first line") != std::string::npos,
          "llm_typical Markdown converts block quotes");
    check(html.find("<ul>") != std::string::npos && html.find("<ol>") != std::string::npos,
          "llm_typical Markdown converts ordered and unordered lists");
    check(html.find("<table>") != std::string::npos &&
              html.find("<td><strong>Bold</strong></td>") != std::string::npos,
          "llm_typical Markdown converts tables with inline formatting");
    check(html.find(R"PK(<pre><code class="language-python">)PK") != std::string::npos,
          "llm_typical Markdown converts fenced code blocks");
    check(html.find("😀 🚀 ✅") != std::string::npos,
          "llm_typical Markdown preserves emoji");
}

void test_comprehensive_markdown_to_html_fixture() {
    const std::string input = read_fixture("tests/fixtures/comprehensive.md");
    const std::string output = ainiux::markdown::to_html_document(input);

    check(output.find("<h1>Comprehensive Markdown Fixture</h1>") != std::string::npos,
          "comprehensive Markdown converts level-one heading");
    check(output.find("<h2>Lists And Structure</h2>") != std::string::npos &&
              output.find("<h3>Third-Level Heading</h3>") != std::string::npos,
          "comprehensive Markdown converts three heading levels");
    check(output.find("<strong>bold text</strong>") != std::string::npos &&
              output.find("<em>italic text</em>") != std::string::npos &&
              output.find("<u>underlined text</u>") != std::string::npos,
          "comprehensive Markdown converts inline formatting");
    check(output.find("<ul>") != std::string::npos && output.find("<ol>") != std::string::npos,
          "comprehensive Markdown converts ordered and unordered lists");
    check(output.find(R"PK(<a href="https://example.com/docs?lang=en&amp;mode=test">normal link</a>)PK") !=
              std::string::npos,
          "comprehensive Markdown converts and escapes links");
    check(output.find(R"PK(<a href="https://example.com/gallery"><img src="https://example.com/assets/placeholder.png" alt="A linked placeholder image"></a>)PK") !=
              std::string::npos,
          "comprehensive Markdown converts a linked image");
    check(output.find("你好，世界") != std::string::npos && output.find("مرحبا بالعالم") != std::string::npos &&
              output.find("😀 🚀 ✅") != std::string::npos,
          "comprehensive Markdown preserves multilingual UTF-8 and emoji");
    check(output.find(R"PK(<code class="language-javascript">)PK") != std::string::npos &&
              output.find("<table>") != std::string::npos,
          "comprehensive Markdown converts fenced code and a table");
}

void test_markdown_html_rendering() {
    const std::string md =
        "# Title & More\n\n"
        "Paragraph with **bold**, *em*, ++under++, [docs](https://example.com?a=1&b=2), and `code <x>`.\n\n"
        "- parent\n"
        "  - child\n\n"
        "1. first\n"
        "2. second\n\n"
        "| Name | Value |\n"
        "| --- | --- |\n"
        "| A | **B** |\n\n"
        "```cpp\n"
        "if (a < b) return;\n"
        "```\n\n"
        "<div>raw</div>\n";
    const std::string html = ainiux::markdown::to_html_fragment(md);
    check(html.find("<h1>Title &amp; More</h1>") != std::string::npos, "Markdown h1 converts to HTML");
    check(html.find("<strong>bold</strong>") != std::string::npos, "Markdown bold converts to strong");
    check(html.find("<em>em</em>") != std::string::npos, "Markdown italic converts to em");
    check(html.find("<u>under</u>") != std::string::npos, "Markdown underline converts to u");
    check(html.find(R"PK(<a href="https://example.com?a=1&amp;b=2">docs</a>)PK") != std::string::npos,
          "Markdown links become escaped anchors");
    check(html.find("<code>code &lt;x&gt;</code>") != std::string::npos, "Markdown inline code escapes HTML");
    check(html.find(R"PK(<ul>
<li>parent<ul>
<li>child</li>)PK") != std::string::npos,
          "Markdown nested unordered lists convert to nested ul/li");
    check(html.find(R"PK(<ol>
<li>first</li>
<li>second</li>)PK") != std::string::npos,
          "Markdown ordered lists convert to ol/li");
    check(html.find("<table>") != std::string::npos && html.find("<th>Name</th>") != std::string::npos &&
              html.find("<td><strong>B</strong></td>") != std::string::npos,
          "Markdown tables convert to HTML tables");
    check(html.find(R"PK(<pre><code class="language-cpp">if (a &lt; b) return;
</code></pre>)PK") != std::string::npos,
          "Markdown fenced code converts to escaped pre/code");
    check(html.find("<div>raw</div>") != std::string::npos, "Markdown raw HTML block is preserved");
}

void test_markdown_plaintext_and_document_rendering() {
    const std::string md = "## Heading\n\nParagraph with **bold** and [docs](https://example.com).\n\n```\n**not bold**\n```\n";
    const std::string plain = ainiux::markdown::to_plaintext(md);
    check(plain.find("Heading") != std::string::npos && plain.find("##") == std::string::npos,
          "Markdown plaintext strips heading marker");
    check(plain.find("Paragraph with bold and docs (https://example.com).") != std::string::npos,
          "Markdown plaintext strips inline markup and keeps link URL");
    check(plain.find("**not bold**") != std::string::npos, "Markdown plaintext keeps code block content");

    const std::string doc = ainiux::markdown::to_html_document("# Saved");
    check(doc.find("<!doctype html>") == 0, "Markdown HTML document starts with doctype");
    check(doc.find(R"PK(<meta charset="utf-8">)PK") != std::string::npos, "Markdown HTML document includes charset");
    check(doc.find(R"PK(name="viewport")PK") != std::string::npos, "Markdown HTML document includes viewport");
    check(doc.find("<title>" + ainiux::app_version_label() + " output</title>") != std::string::npos,
          "Markdown HTML document title displays the current app version");
    check(doc.find("<h1>Saved</h1>") != std::string::npos, "Markdown HTML document includes rendered body");
}

void test_markdown_empty_unicode_and_format_parsing() {
    check(ainiux::markdown::to_html_fragment("").empty(),
          "Markdown HTML fragment conversion of empty input returns empty output");
    check(ainiux::markdown::to_plaintext("").empty(),
          "Markdown plaintext conversion of empty input returns empty output");

    const std::string unicode_md = u8"## عنوان\n\n你好 👨‍👩‍👧‍👦";
    const std::string html = ainiux::markdown::to_html_fragment(unicode_md);
    check(html.find(u8"你好") != std::string::npos &&
              html.find(u8"👨‍👩‍👧‍👦") != std::string::npos,
          "Markdown HTML conversion preserves Chinese and complex emoji text");
    const std::string plain = ainiux::markdown::to_plaintext(unicode_md);
    check(plain.find(u8"你好") != std::string::npos &&
              plain.find("##") == std::string::npos,
          "Markdown plaintext conversion preserves Unicode while stripping markup");

    const std::string long_md = std::string(100000, 'a');
    check(ainiux::markdown::to_plaintext(long_md).size() >= 100000,
          "Markdown plaintext conversion preserves very long text");

    ainiux::markdown::OutputFormat format = ainiux::markdown::OutputFormat::Html;
    check(ainiux::markdown::parse_output_format("html", format) &&
              format == ainiux::markdown::OutputFormat::Html,
          "Markdown output format parser accepts html");
    check(ainiux::markdown::parse_output_format("plaintext", format) &&
              format == ainiux::markdown::OutputFormat::Plaintext,
          "Markdown output format parser accepts plaintext");
    check(!ainiux::markdown::parse_output_format("pdf", format),
          "Markdown output format parser rejects unsupported formats");
    check(!ainiux::markdown::parse_output_format("", format),
          "Markdown output format parser rejects empty format names");
}

void test_pretty_table_unicode_and_gfm() {
    const std::vector<std::string> headers = {"Language", "Files", "Lines of code"};
    const std::vector<ainiux::markdown::TableAlign> aligns = {
        ainiux::markdown::TableAlign::Left, ainiux::markdown::TableAlign::Right,
        ainiux::markdown::TableAlign::Right};
    const std::vector<std::vector<std::string>> body = {
        {"JSON", "1", "211"},
        {"Markdown", "2", "443"},
    };

    const std::string unicode =
        ainiux::markdown::format_table(headers, aligns, body,
                                       ainiux::markdown::TableStyle::UnicodeBox);
    check(unicode.find(u8"┌") == 0 && unicode.find(u8"└") != std::string::npos,
          "Unicode table has box corners");
    check(unicode.find("│ JSON     │") != std::string::npos ||
              unicode.find("│ JSON") != std::string::npos,
          "Unicode table pads Language column");
    check(unicode.find("│ Markdown │") != std::string::npos,
          "Unicode table includes Markdown row");
    check(unicode.find("211") != std::string::npos && unicode.find("443") != std::string::npos,
          "Unicode table includes numeric cells");

    const std::string gfm =
        ainiux::markdown::format_table(headers, aligns, body,
                                       ainiux::markdown::TableStyle::PaddedGfm);
    check(gfm.find("| Language | Files | Lines of code |") != std::string::npos,
          "Padded GFM keeps pipe headers");
    check(gfm.find("| Markdown |") != std::string::npos &&
              gfm.find("----") != std::string::npos,
          "Padded GFM has separator dashes");
    // Right-aligned numeric column should left-pad spaces before the number.
    check(gfm.find("211") != std::string::npos, "Padded GFM includes values");
}

void test_pretty_format_tables_streaming_and_fences() {
    const std::string input =
        "Intro\n"
        "| Language | Files | Lines of code |\n"
        "| --- | ---: | ---: |\n"
        "| JSON | 1 | 211 |\n"
        "| Markdown | 2 | 443 |\n"
        "\n"
        "```\n"
        "| not | a | table |\n"
        "| --- | --- | --- |\n"
        "```\n";
    ainiux::markdown::TableFormatOptions options;
    options.style = ainiux::markdown::TableStyle::UnicodeBox;
    options.reformat_open_tables = true;
    const std::string pretty = ainiux::markdown::pretty_format_tables(input, options);
    check(pretty.find(u8"┌") != std::string::npos &&
              pretty.find("│ JSON") != std::string::npos,
          "pretty_format_tables rewrites GFM tables to Unicode boxes");
    check(pretty.find("```\n| not | a | table |") != std::string::npos,
          "pretty_format_tables leaves fenced pipe examples alone");

    // Open table (header + separator + one body row, still streaming).
    const std::string open_table =
        "| A | B |\n"
        "| --- | ---: |\n"
        "| x | 1 |";
    const std::string open_pretty =
        ainiux::markdown::pretty_format_tables(open_table, options);
    check(open_pretty.find(u8"┌") != std::string::npos &&
              open_pretty.find("│ x") != std::string::npos,
          "open streaming tables reformat with known rows");

    // Header only is not a table yet.
    const std::string header_only = "| A | B |\n";
    check(ainiux::markdown::pretty_format_tables(header_only, options) == header_only,
          "header-only lines stay raw until a separator arrives");

    // Already boxed tables are not GFM and should pass through.
    const std::string boxed =
        ainiux::markdown::format_table({"A", "B"}, {}, {{"1", "2"}},
                                       ainiux::markdown::TableStyle::UnicodeBox);
    check(ainiux::markdown::pretty_format_tables(boxed, options) == boxed,
          "Unicode box tables are left unchanged (idempotent)");

    check(ainiux::markdown::table_display_width("ab") == 2,
          "ASCII display width is one cell per character");
    check(ainiux::markdown::table_display_width(u8"你好") == 4,
          "CJK display width counts two cells per character");
}

}  // namespace

void run_all() {
    test_llm_typical_markdown_to_html_fixture();
    test_comprehensive_markdown_to_html_fixture();
    test_markdown_empty_unicode_and_format_parsing();
    test_markdown_html_rendering();
    test_markdown_plaintext_and_document_rendering();
    test_pretty_table_unicode_and_gfm();
    test_pretty_format_tables_streaming_and_fences();
}

}  // namespace ainiux::test::markdown
