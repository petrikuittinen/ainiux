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

void test_mode_parsing_and_detection() {
    Language language = Language::Text;
    check(pkchat::highlight::parse_language("markdown", language) && language == Language::Markdown,
          "highlight parses markdown mode");
    check(pkchat::highlight::parse_language("MD", language) && language == Language::Markdown,
          "highlight parses md alias case-insensitively");
    check(pkchat::highlight::parse_language("text", language) && language == Language::Text,
          "highlight parses text mode");
    check(!pkchat::highlight::parse_language("python", language),
          "highlight defers unsupported language modes");

    for (const char* path : {"README.md", "notes.MARKDOWN", "draft.mdown", "doc.MKD"}) {
        check(pkchat::highlight::detect_language(path) == Language::Markdown,
              std::string("highlight detects Markdown extension: ") + path);
    }
    check(pkchat::highlight::detect_language("notes.txt") == Language::Text,
          "highlight treats txt as text");
    check(pkchat::highlight::detect_language("") == Language::Text,
          "highlight treats scratch buffers as text");
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
    check(lines[3].spans.size() == 1 && lines[3].spans[0].role == TokenRole::String,
          "Markdown fenced text suppresses heading rules");
    check(has_role(lines[4].spans, TokenRole::Preprocessor), "Markdown highlights closing fences");
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

    const std::string long_line(pkchat::highlight::kMaximumHighlightedLineBytes + 1, '*');
    const pkchat::highlight::HighlightedLine limited =
        pkchat::highlight::highlight_line(Language::Markdown, long_line);
    check(limited.work_limited && limited.spans.empty(),
          "Markdown long-line highlighting falls back to plain text");
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
    test_markdown_inline_and_structure();
    test_markdown_emphasis_delimiters_are_complete();
    test_markdown_multiline_state_and_precedence();
    test_setext_unicode_invalid_bytes_and_budget();
    test_incremental_cache_invalidation();
}

}  // namespace pkchat::test::highlight
