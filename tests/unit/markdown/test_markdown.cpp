#include "markdown/test_markdown.hpp"
#include "support/test_support.hpp"
#include "markdown/markdown.hpp"
#include "pkchat/version.hpp"
#include <string>

namespace pkchat::test::markdown {

namespace {

using pkchat::test::check;
using pkchat::test::read_fixture;

void test_llm_typical_markdown_to_html_fixture() {
    const std::string input = read_fixture("tests/fixtures/llm_typical.md");
    const std::string html = pkchat::markdown::to_html_fragment(input);

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
    const std::string output = pkchat::markdown::to_html_document(input);

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
    const std::string html = pkchat::markdown::to_html_fragment(md);
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
    const std::string plain = pkchat::markdown::to_plaintext(md);
    check(plain.find("Heading") != std::string::npos && plain.find("##") == std::string::npos,
          "Markdown plaintext strips heading marker");
    check(plain.find("Paragraph with bold and docs (https://example.com).") != std::string::npos,
          "Markdown plaintext strips inline markup and keeps link URL");
    check(plain.find("**not bold**") != std::string::npos, "Markdown plaintext keeps code block content");

    const std::string doc = pkchat::markdown::to_html_document("# Saved");
    check(doc.find("<!doctype html>") == 0, "Markdown HTML document starts with doctype");
    check(doc.find(R"PK(<meta charset="utf-8">)PK") != std::string::npos, "Markdown HTML document includes charset");
    check(doc.find(R"PK(name="viewport")PK") != std::string::npos, "Markdown HTML document includes viewport");
    check(doc.find("<title>" + pkchat::app_version_label() + " output</title>") != std::string::npos,
          "Markdown HTML document title displays the current app version");
    check(doc.find("<h1>Saved</h1>") != std::string::npos, "Markdown HTML document includes rendered body");
}

void test_markdown_empty_unicode_and_format_parsing() {
    check(pkchat::markdown::to_html_fragment("").empty(),
          "Markdown HTML fragment conversion of empty input returns empty output");
    check(pkchat::markdown::to_plaintext("").empty(),
          "Markdown plaintext conversion of empty input returns empty output");

    const std::string unicode_md = u8"## عنوان\n\n你好 👨‍👩‍👧‍👦";
    const std::string html = pkchat::markdown::to_html_fragment(unicode_md);
    check(html.find(u8"你好") != std::string::npos &&
              html.find(u8"👨‍👩‍👧‍👦") != std::string::npos,
          "Markdown HTML conversion preserves Chinese and complex emoji text");
    const std::string plain = pkchat::markdown::to_plaintext(unicode_md);
    check(plain.find(u8"你好") != std::string::npos &&
              plain.find("##") == std::string::npos,
          "Markdown plaintext conversion preserves Unicode while stripping markup");

    const std::string long_md = std::string(100000, 'a');
    check(pkchat::markdown::to_plaintext(long_md).size() >= 100000,
          "Markdown plaintext conversion preserves very long text");

    pkchat::markdown::OutputFormat format = pkchat::markdown::OutputFormat::Html;
    check(pkchat::markdown::parse_output_format("html", format) &&
              format == pkchat::markdown::OutputFormat::Html,
          "Markdown output format parser accepts html");
    check(pkchat::markdown::parse_output_format("plaintext", format) &&
              format == pkchat::markdown::OutputFormat::Plaintext,
          "Markdown output format parser accepts plaintext");
    check(!pkchat::markdown::parse_output_format("pdf", format),
          "Markdown output format parser rejects unsupported formats");
    check(!pkchat::markdown::parse_output_format("", format),
          "Markdown output format parser rejects empty format names");
}

}  // namespace

void run_all() {
    test_llm_typical_markdown_to_html_fixture();
    test_comprehensive_markdown_to_html_fixture();
    test_markdown_empty_unicode_and_format_parsing();
    test_markdown_html_rendering();
    test_markdown_plaintext_and_document_rendering();
}

}  // namespace pkchat::test::markdown
