#include "pdf/test_pdf.hpp"

#include "pdf/document.hpp"
#include "pdf/pdf.hpp"
#include "pdf/stream.hpp"
#include "pdf/token.hpp"
#include "pdf/value.hpp"
#include "runtime/runtime.hpp"
#include "support/test_support.hpp"

#include <cstdio>
#include <string>
#include <vector>
#include <zlib.h>

namespace ainiux::test::pdf {
namespace {

using ainiux::test::check;
using ainiux::test::read_fixture;

std::string xref_line(std::size_t offset, unsigned generation, char type) {
    char line[32];
    const int n = std::snprintf(line, sizeof(line), "%010zu %05u %c \n", offset, generation, type);
    check(n == 20, "xref line is 20 bytes");
    return std::string(line, 20);
}

std::string make_pdf_with_content(const std::string& content) {
    const std::string obj4 = "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "endstream";
    std::vector<std::string> bodies = {
        "",
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R "
        "/Resources << /Font << /F1 5 0 R >> >> >>",
        obj4,
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    };
    std::string out = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    std::vector<std::size_t> offsets(bodies.size(), 0);
    for (std::size_t i = 1; i < bodies.size(); ++i) {
        offsets[i] = out.size();
        out += std::to_string(i) + " 0 obj\n" + bodies[i] + "\nendobj\n";
    }
    const std::size_t xref = out.size();
    out += "xref\n0 " + std::to_string(bodies.size()) + "\n";
    out += xref_line(0, 65535, 'f');
    for (std::size_t i = 1; i < bodies.size(); ++i) {
        out += xref_line(offsets[i], 0, 'n');
    }
    out += "trailer\n<< /Size " + std::to_string(bodies.size()) + " /Root 1 0 R >>\n";
    out += "startxref\n" + std::to_string(xref) + "\n%%EOF\n";
    return out;
}

std::string make_simple_pdf() {
    return make_pdf_with_content("BT /F1 12 Tf 72 720 Td (Hello PDF) Tj ET\n");
}

std::string flate_bytes(const std::string& raw) {
    uLongf bound = compressBound(static_cast<uLong>(raw.size()));
    std::string out(bound, '\0');
    const int rc = compress(reinterpret_cast<Bytef*>(&out[0]), &bound,
                            reinterpret_cast<const Bytef*>(raw.data()), static_cast<uLong>(raw.size()));
    check(rc == Z_OK, "zlib compress succeeds");
    out.resize(bound);
    return out;
}

void test_tokenizer_and_values() {
    const std::string src = "<< /Type /Catalog /Count 2 /Kids [1 0 R 2 0 R] /Title (Hello) /Flag true >>";
    ainiux::pdf::Tokenizer tokens(ainiux::pdf::Cursor{
        reinterpret_cast<const std::uint8_t*>(src.data()), src.size(), 0});
    ainiux::pdf::Value value;
    const ainiux::Error err = ainiux::pdf::read_value(tokens, value, 0);
    check(err.ok(), "dictionary value parses");
    check(value.type == ainiux::pdf::ValueType::Dict, "parsed value is a dict");
    std::string type;
    check(ainiux::pdf::dict_name(value, "Type", type) && type == "Catalog", "dict name Type=Catalog");
    std::int64_t count = 0;
    check(ainiux::pdf::dict_int(value, "Count", count) && count == 2, "dict int Count=2");
    ainiux::pdf::Ref kid;
    const ainiux::pdf::Value* kids = ainiux::pdf::dict_array(value, "Kids");
    check(kids != nullptr && kids->array.size() == 2 && kids->array[0].type == ainiux::pdf::ValueType::Ref,
          "Kids array holds refs");
    kid = kids->array[0].ref;
    check(kid.number == 1 && kid.generation == 0, "first kid is 1 0 R");
    const ainiux::pdf::Value* title = ainiux::pdf::dict_get(value, "Title");
    check(title != nullptr && title->type == ainiux::pdf::ValueType::String && title->text == "Hello",
          "Title string Hello");
}

void test_name_escape_and_hex() {
    const std::string src = "/A#20B <4869>";
    ainiux::pdf::Tokenizer tokens(ainiux::pdf::Cursor{
        reinterpret_cast<const std::uint8_t*>(src.data()), src.size(), 0});
    ainiux::pdf::Token name;
    ainiux::pdf::Token hex;
    check(tokens.next(name).ok() && name.kind == ainiux::pdf::TokenKind::Name && name.text == "A B",
          "name #20 decodes to space");
    check(tokens.next(hex).ok() && hex.kind == ainiux::pdf::TokenKind::Hex && hex.text == "Hi",
          "hex string <4869> decodes to Hi");
}

void test_nested_string_and_comment() {
    const std::string src = "(nested (yes) ok) % comment\n/Name";
    ainiux::pdf::Tokenizer tokens(ainiux::pdf::Cursor{
        reinterpret_cast<const std::uint8_t*>(src.data()), src.size(), 0});
    ainiux::pdf::Token str;
    ainiux::pdf::Token name;
    check(tokens.next(str).ok() && str.text == "nested (yes) ok", "nested parentheses in strings");
    check(tokens.next(name).ok() && name.kind == ainiux::pdf::TokenKind::Name && name.text == "Name",
          "comment is skipped before next token");
}

void test_flate_roundtrip() {
    const std::string raw = "BT (Hello) Tj ET";
    const std::string compressed = flate_bytes(raw);
    std::string out;
    const ainiux::Error err = ainiux::pdf::inflate_flate(
        reinterpret_cast<const std::uint8_t*>(compressed.data()), compressed.size(), out, 1024);
    check(err.ok() && out == raw, "FlateDecode round-trip");
}

void test_open_simple_pdf() {
    const std::string pdf = make_simple_pdf();
    ainiux::pdf::Document document;
    ainiux::pdf::Options options;
    const ainiux::Error err = ainiux::pdf::Document::open_bytes(pdf, options, document);
    check(err.ok(), "simple PDF opens: " + err.message);
    check(document.version() == "1.4", "simple PDF version is 1.4");
    check(document.page_count() == 1, "simple PDF has one page");
    std::string content;
    check(document.page_content(0, content).ok(), "simple PDF page content decodes");
    check(content.find("Hello PDF") != std::string::npos, "uncompressed content contains Hello PDF");
}

void test_open_missing_startxref_repairs() {
    std::string pdf = make_simple_pdf();
    const std::size_t pos = pdf.rfind("startxref");
    check(pos != std::string::npos, "synthetic PDF has startxref");
    pdf.replace(pos, 9, "XXXXXXXXX");
    ainiux::pdf::Document document;
    const ainiux::Error err = ainiux::pdf::Document::open_bytes(pdf, ainiux::pdf::Options{}, document);
    check(err.ok(), "repair opens PDF without startxref: " + err.message);
    check(document.page_count() == 1, "repaired PDF still has one page");
}

void test_bad_header() {
    ainiux::pdf::Document document;
    const ainiux::Error err = ainiux::pdf::Document::open_bytes("not a pdf", ainiux::pdf::Options{}, document);
    check(!err.ok() && err.message.find("header") != std::string::npos, "non-PDF bytes fail on header");
}

void test_cancelled_open() {
    ainiux::runtime::CancellationSource source;
    source.cancel();
    ainiux::pdf::Options options;
    options.cancellation = source.token();
    ainiux::pdf::Document document;
    const ainiux::Error err = ainiux::pdf::Document::open_bytes(make_simple_pdf(), options, document);
    check(err.code == ainiux::ErrorCode::Cancelled, "cancelled open returns Cancelled");
}

void test_real_short_russian_pdf() {
    const std::string bytes = read_fixture("tests/pdf_files/ru-llm-eval-short-note.pdf");
    ainiux::pdf::Document document;
    const ainiux::Error err = ainiux::pdf::Document::open_bytes(bytes, ainiux::pdf::Options{}, document);
    check(err.ok(), "Russian fixture opens: " + err.message);
    check(document.page_count() == 1, "Russian fixture is one page");
    std::string content;
    const ainiux::Error content_err = document.page_content(0, content);
    check(content_err.ok(), "Russian fixture content decodes: " + content_err.message);
    check(!content.empty(), "Russian fixture content stream is not empty");
}

void test_open_corpus_page_counts() {
    struct Case {
        const char* path;
        std::size_t pages;
    };
    const Case cases[] = {
        {"tests/pdf_files/ru-llm-eval-short-note.pdf", 1},
        {"tests/pdf_files/ru-transformers-short-note.pdf", 2},
        {"tests/pdf_files/arabic-prose-sample.pdf", 8},
        {"tests/pdf_files/chinese-proverbs-collection.pdf", 46},
        {"tests/pdf_files/DeepSeek2501.12948v1.pdf", 22},
        {"tests/pdf_files/Nvidia-Quarterly-Presentation-final-1.pdf", 17},
        {"tests/pdf_files/dgx-spark.pdf", 81},
        {"tests/pdf_files/fortum-tammi-kesakuun-2026-puolivuosikatsaus.pdf", 61},
        {"tests/pdf_files/State-of-AI.pdf", 36},
    };
    for (const Case& item : cases) {
        const std::string bytes = read_fixture(item.path);
        ainiux::pdf::Document document;
        const ainiux::Error err = ainiux::pdf::Document::open_bytes(bytes, ainiux::pdf::Options{}, document);
        check(err.ok(), std::string(item.path) + " opens: " + err.message);
        check(document.page_count() == item.pages,
              std::string(item.path) + " page count " + std::to_string(document.page_count()) + " == " +
                  std::to_string(item.pages));
    }
}

void test_to_markdown_extracts_simple_pdf() {
    ainiux::pdf::Options options;
    std::string markdown;
    const ainiux::Error err = ainiux::pdf::to_markdown_bytes(make_simple_pdf(), options, markdown);
    check(err.ok(), "to_markdown extracts simple PDF: " + err.message);
    check(markdown.find("Hello PDF") != std::string::npos, "simple PDF markdown contains Hello PDF");
}

void test_to_markdown_russian_needle() {
    const std::string bytes = read_fixture("tests/pdf_files/ru-llm-eval-short-note.pdf");
    std::string markdown;
    const ainiux::Error err = ainiux::pdf::to_markdown_bytes(bytes, ainiux::pdf::Options{}, markdown);
    check(err.ok(), "Russian PDF to markdown: " + err.message);
    check(markdown.find("языковая модель") != std::string::npos || markdown.find("языков") != std::string::npos,
          "Russian PDF markdown keeps Cyrillic");
}

std::string markdown_of(const std::string& content) {
    std::string markdown;
    const ainiux::Error err = ainiux::pdf::to_markdown_bytes(make_pdf_with_content(content), ainiux::pdf::Options{},
                                                            markdown);
    check(err.ok(), "synthetic PDF converts: " + err.message);
    return markdown;
}

void test_tj_kerning_does_not_split_words() {
    const std::string markdown = markdown_of("BT /F1 12 Tf 72 720 Td [(gener)-80(ation)] TJ ET\n");
    check(markdown.find("generation") != std::string::npos, "small TJ kerning stays inside generation");
    check(markdown.find("gener ation") == std::string::npos, "small TJ kerning does not insert a space");
}

void test_tj_word_gap_inserts_one_space() {
    const std::string markdown = markdown_of("BT /F1 12 Tf 72 720 Td [(Hello)-400(World)] TJ ET\n");
    check(markdown.find("Hello World") != std::string::npos, "large TJ displacement is one word space");
    check(markdown.find("Hello  World") == std::string::npos, "word gap does not emit two spaces");
}

void test_tj_does_not_space_before_comma() {
    const std::string markdown = markdown_of("BT /F1 12 Tf 72 720 Td [(Hello)-80(,)] TJ ET\n");
    check(markdown.find("Hello,") != std::string::npos, "kerning before comma does not insert a space");
}

void test_td_word_gap_inserts_one_space() {
    const std::string markdown = markdown_of("BT /F1 12 Tf 72 720 Td (Hello) Tj 30 0 Td (World) Tj ET\n");
    check(markdown.find("Hello World") != std::string::npos, "Td word gap inserts one space");
    check(markdown.find("Hello  World") == std::string::npos, "Td word gap does not double-space");
}

void test_to_markdown_deepseek_does_not_split_words() {
    const std::string bytes = read_fixture("tests/pdf_files/DeepSeek2501.12948v1.pdf");
    std::string markdown;
    const ainiux::Error err = ainiux::pdf::to_markdown_bytes(bytes, ainiux::pdf::Options{}, markdown);
    check(err.ok(), "DeepSeek PDF to markdown: " + err.message);
    check(markdown.find("DeepSeek-R1-Zero") != std::string::npos, "DeepSeek keeps DeepSeek-R1-Zero intact");
    check(markdown.find("first-generation") != std::string::npos, "DeepSeek keeps first-generation intact");
    check(markdown.find("reinforcement") != std::string::npos || markdown.find("Reinforcement") != std::string::npos,
          "DeepSeek keeps reinforcement intact");
    check(markdown.find("capabilities") != std::string::npos, "DeepSeek keeps capabilities intact");
    check(markdown.find("Zer o") == std::string::npos, "DeepSeek does not split Zero");
    check(markdown.find("Reinfor cement") == std::string::npos, "DeepSeek does not split reinforcement");
}

}  // namespace

void run_all() {
    test_tokenizer_and_values();
    test_name_escape_and_hex();
    test_nested_string_and_comment();
    test_flate_roundtrip();
    test_open_simple_pdf();
    test_open_missing_startxref_repairs();
    test_bad_header();
    test_cancelled_open();
    test_real_short_russian_pdf();
    test_open_corpus_page_counts();
    test_to_markdown_extracts_simple_pdf();
    test_to_markdown_russian_needle();
    test_tj_kerning_does_not_split_words();
    test_tj_word_gap_inserts_one_space();
    test_tj_does_not_space_before_comma();
    test_td_word_gap_inserts_one_space();
    test_to_markdown_deepseek_does_not_split_words();
}

}  // namespace ainiux::test::pdf
