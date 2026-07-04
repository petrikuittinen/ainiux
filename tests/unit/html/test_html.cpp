#include "html/test_html.hpp"
#include "support/test_support.hpp"
#include "html/html.hpp"
#include <string>

namespace pkchat::test::html {

namespace {

using pkchat::test::check;
using pkchat::test::read_fixture;

void test_llm_typical_html_to_markdown_fixture() {
    const std::string input = read_fixture("tests/fixtures/llm_typical.html");
    const std::string output = pkchat::html::convert(input, pkchat::html::OutputFormat::Markdown);

    check(output.find("# LLM Typical HTML Fixture") != std::string::npos,
          "llm_typical HTML converts the title heading");
    check(output.find("**bold**") != std::string::npos && output.find("*italic*") != std::string::npos &&
              output.find("***bold italic***") != std::string::npos,
          "llm_typical HTML converts bold, italic, and bold-italic");
    check(output.find("`inline code`") != std::string::npos && output.find("~~strikethrough~~") != std::string::npos,
          "llm_typical HTML converts inline code and strikethrough");
    check(output.find("[a link](https://example.com/path?q=1)") != std::string::npos,
          "llm_typical HTML converts links");
    check(output.find("---") != std::string::npos,
          "llm_typical HTML converts horizontal rules");
    check(output.find("> Block quote first line") != std::string::npos &&
              output.find("> Block quote second line") != std::string::npos,
          "llm_typical HTML converts block quotes");
    check(output.find("- unordered alpha") != std::string::npos &&
              output.find("1. ordered one") != std::string::npos,
          "llm_typical HTML converts ordered and unordered lists");
    check(output.find("| Feature | Sample |") != std::string::npos &&
              output.find("| **Bold** | *Italic* |") != std::string::npos &&
              output.find("| `code` | ~~strike~~ |") != std::string::npos,
          "llm_typical HTML converts tables with inline formatting");
    check(output.find("```python") != std::string::npos &&
              output.find("def greet(name)") != std::string::npos,
          "llm_typical HTML converts fenced code blocks");
    check(output.find("😀 🚀 ✅") != std::string::npos,
          "llm_typical HTML preserves emoji");
}

void test_comprehensive_html_to_markdown_fixture() {
    const std::string input = read_fixture("tests/fixtures/comprehensive.html");
    const std::string output = pkchat::html::convert(input, pkchat::html::OutputFormat::Markdown);

    check(output.find("# Comprehensive HTML Fixture") != std::string::npos,
          "comprehensive HTML converts level-one heading");
    check(output.find("## Languages And Emoji") != std::string::npos &&
              output.find("### Multilingual Content") != std::string::npos,
          "comprehensive HTML converts three heading levels");
    check(output.find("**bold text**") != std::string::npos && output.find("*italic text*") != std::string::npos &&
              output.find("++underlined text++") != std::string::npos,
          "comprehensive HTML converts inline formatting");
    check(output.find("- First item") != std::string::npos &&
              output.find("1. Prepare the fixture") != std::string::npos &&
              output.find("3. Verify the result ✅") != std::string::npos,
          "comprehensive HTML converts ordered and unordered lists");
    check(output.find("[a normal link](https://example.com/docs?lang=en&mode=test)") != std::string::npos,
          "comprehensive HTML converts links and decodes entities");
    check(output.find("[![A linked placeholder image](https://example.com/assets/placeholder.png)](https://example.com/gallery)") !=
              std::string::npos,
          "comprehensive HTML converts a linked image");
    check(output.find("你好，世界") != std::string::npos && output.find("مرحبا بالعالم") != std::string::npos &&
              output.find("😀 🚀 ✅") != std::string::npos,
          "comprehensive HTML preserves multilingual UTF-8 and emoji");
    check(output.find("fixtureGreeting") == std::string::npos && output.find("color-scheme") == std::string::npos,
          "comprehensive HTML excludes script and style contents");
}

void test_html_large_ignored_blocks() {
    std::string html = "<h1>Before</h1><script>";
    html += std::string(200000, '<');
    html += "</script><style>";
    html += std::string(200000, '>');
    html += "</style><p>After <a href=\"https://example.com\">link</a></p>";
    const std::string out = pkchat::html::convert(html, pkchat::html::OutputFormat::Markdown);
    check(out.find("# Before") != std::string::npos, "HTML large ignored block keeps preceding text");
    check(out.find("After [link](https://example.com)") != std::string::npos,
          "HTML large ignored block keeps following text");
}

void test_html_malformed_documents() {
    const std::string no_doctype = "<html><body><h1>No doctype</h1><p>Body text</p></body></html>";
    std::string out = pkchat::html::convert(no_doctype, pkchat::html::OutputFormat::Markdown);
    check(out.find("# No doctype") != std::string::npos, "HTML conversion does not require a DOCTYPE");
    check(out.find("Body text") != std::string::npos, "HTML without DOCTYPE keeps body text");

    const std::string unclosed = "<html><body><h1>I forgot to close this...<p>Next paragraph";
    out = pkchat::html::convert(unclosed, pkchat::html::OutputFormat::Markdown);
    check(out.find("# I forgot to close this...") != std::string::npos,
          "HTML unclosed heading keeps heading text");
    check(out.find("Next paragraph") != std::string::npos, "HTML unclosed tags keep following text");

    const std::string misquoted = "<p>Before <img width=\"100 height=\"100\"> after</p>";
    out = pkchat::html::convert(misquoted, pkchat::html::OutputFormat::Text);
    check(out.find("Before after") != std::string::npos,
          "HTML misquoted image attributes do not swallow surrounding text");
    check(out.find("width") == std::string::npos, "HTML misquoted image tag is stripped as a tag");

    const std::string misspelled = "<p><strnog>not bold</strnog> and <emphasis>not italic</emphasis></p>";
    out = pkchat::html::convert(misspelled, pkchat::html::OutputFormat::Markdown);
    check(out.find("not bold and not italic") != std::string::npos,
          "HTML misspelled tags are ignored while keeping text");
    check(out.find("**") == std::string::npos && out.find("*not italic*") == std::string::npos,
          "HTML misspelled formatting tags do not create Markdown emphasis");
}

void test_html_markdown_conversion() {
    const std::string html =
        "<html><head><style>.x{}</style><script>bad()</script></head>"
        "<body><h1>Title &amp; More</h1><p>Hello <strong>bold</strong> and <em>em</em> "
        "<a href=\"https://example.com?q=1&amp;x=2\">link</a>.</p>"
        "<h2>Next</h2><p><b>heavy</b> <italic>tilt</italic></p></body></html>";
    const std::string out = pkchat::html::convert(html, pkchat::html::OutputFormat::Markdown);
    check(out.find("# Title & More") != std::string::npos, "HTML h1 converts to Markdown heading");
    check(out.find("Hello **bold** and *em* [link](https://example.com?q=1&x=2).") != std::string::npos,
          "HTML inline tags convert to Markdown");
    check(out.find("## Next") != std::string::npos, "HTML h2 converts to Markdown heading");
    check(out.find("**heavy** *tilt*") != std::string::npos, "HTML b and italic convert to Markdown emphasis");
    check(out.find("bad()") == std::string::npos, "HTML script content is ignored");
}

void test_html_text_conversion() {
    const std::string html =
        "<h1>Title &amp; More</h1><p>Hello <strong>bold</strong> and <em>em</em> "
        "<a href='https://example.com/docs'>docs</a>.</p>";
    const std::string out = pkchat::html::convert(html, pkchat::html::OutputFormat::Text);
    check(out.find("Title & More") != std::string::npos, "HTML text output keeps heading text");
    check(out.find("Hello bold and em docs (https://example.com/docs).") != std::string::npos,
          "HTML text output keeps link URL next to link text");
    check(out.find("**") == std::string::npos && out.find("[") == std::string::npos,
          "HTML text output does not include Markdown syntax");
}

void test_html_utf8_validation() {
    const std::string utf8 = u8"<h1>Привет 中文</h1>";
    size_t offset = 0;
    check(pkchat::html::is_valid_utf8(utf8, &offset), "HTML validator accepts valid UTF-8 Russian and Chinese text");
    check(offset == utf8.size(), "HTML validator reports end offset for valid UTF-8");

    const std::string windows1251_russian = std::string("<h1>") + "\xCF\xF0\xE8\xE2\xE5\xF2" + "</h1>";
    offset = 0;
    check(!pkchat::html::is_valid_utf8(windows1251_russian, &offset),
          "HTML validator rejects Windows-1251 Russian bytes");
    check(offset == 4, "HTML validator reports the first invalid Windows-1251 byte offset");

    const std::string gbk_chinese = std::string("<h1>") + "\xD6\xD0\xCE\xC4" + "</h1>";
    offset = 0;
    check(!pkchat::html::is_valid_utf8(gbk_chinese, &offset), "HTML validator rejects GBK Chinese bytes");
    check(offset == 4, "HTML validator reports the first invalid GBK byte offset");
}

void test_html_empty_unicode_and_format_parsing() {
    check(pkchat::html::convert("", pkchat::html::OutputFormat::Markdown).empty(),
          "HTML conversion of empty input returns empty output");
    const std::string unicode_html = u8"<p>مرحبا 你好 👨‍👩‍👧‍👦</p>";
    const std::string markdown = pkchat::html::convert(unicode_html, pkchat::html::OutputFormat::Markdown);
    check(markdown.find(u8"مرحبا") != std::string::npos &&
              markdown.find(u8"你好") != std::string::npos &&
              markdown.find(u8"👨‍👩‍👧‍👦") != std::string::npos,
          "HTML conversion preserves Arabic, Chinese, and complex emoji text");

    const std::string long_html = "<p>" + std::string(100000, 'x') + "</p>";
    const std::string long_out = pkchat::html::convert(long_html, pkchat::html::OutputFormat::Text);
    check(long_out.size() >= 100000,
          "HTML conversion preserves very long text bodies");

    pkchat::html::OutputFormat format = pkchat::html::OutputFormat::Markdown;
    check(pkchat::html::parse_output_format("markdown", format) &&
              format == pkchat::html::OutputFormat::Markdown,
          "HTML output format parser accepts markdown");
    check(pkchat::html::parse_output_format("text", format) &&
              format == pkchat::html::OutputFormat::Text,
          "HTML output format parser accepts text");
    check(!pkchat::html::parse_output_format("pdf", format),
          "HTML output format parser rejects unsupported formats");
    check(!pkchat::html::parse_output_format("", format),
          "HTML output format parser rejects empty format names");
}

}  // namespace

void run_all() {
    test_llm_typical_html_to_markdown_fixture();
    test_comprehensive_html_to_markdown_fixture();
    test_html_empty_unicode_and_format_parsing();
    test_html_large_ignored_blocks();
    test_html_malformed_documents();
    test_html_markdown_conversion();
    test_html_text_conversion();
    test_html_utf8_validation();
}

}  // namespace pkchat::test::html
