#include "pdf/test_pdf.hpp"

#include "html/html.hpp"
#include "pdf/document.hpp"
#include "pdf/pdf.hpp"
#include "pdf/rtl.hpp"
#include "pdf/stream.hpp"
#include "pdf/ttf.hpp"
#include "pdf/token.hpp"
#include "pdf/value.hpp"
#include "runtime/runtime.hpp"
#include "support/test_support.hpp"

#include <cstdio>
#include <fstream>
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

std::string make_pdf_with_tounicode(const std::string& cmap, const std::string& content) {
    const std::string content_obj =
        "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "endstream";
    const std::string cmap_obj =
        "<< /Length " + std::to_string(cmap.size()) + " >>\nstream\n" + cmap + "endstream";
    std::vector<std::string> bodies = {
        "",
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R "
        "/Resources << /Font << /F1 5 0 R >> >> >>",
        content_obj,
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /ToUnicode 6 0 R >>",
        cmap_obj,
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
        {"tests/pdf_files/hebrew-prose-sample.pdf", 1},
        {"tests/pdf_files/chinese-proverbs-collection.pdf", 46},
        {"tests/pdf_files/chinese-tang-poems-traditional.pdf", 1},
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

void test_to_markdown_consumes_bfrange_destination_array() {
    const std::string cmap =
        "1 begincodespacerange\n"
        "<0000> <FFFF>\n"
        "endcodespacerange\n"
        "1 beginbfrange\n"
        "<0003> <0004> [<0020> <0041>]\n"
        "endbfrange\n"
        "1 beginbfchar\n"
        "<0011> <0042>\n"
        "endbfchar\n";
    std::string markdown;
    const ainiux::Error err = ainiux::pdf::to_markdown_bytes(
        make_pdf_with_tounicode(cmap, "BT /F1 12 Tf 72 720 Td <000300040011> Tj ET\n"),
        ainiux::pdf::Options{}, markdown);
    check(err.ok(), "array-form ToUnicode bfrange extracts: " + err.message);
    check(markdown.find("AB") != std::string::npos,
          "array-form ToUnicode bfrange consumes its closing bracket");
}

void test_to_markdown_rejects_unterminated_bfchar() {
    const std::string cmap = "1 beginbfchar\n<0001> <0041>\n";
    std::string markdown;
    const ainiux::Error err = ainiux::pdf::to_markdown_bytes(
        make_pdf_with_tounicode(cmap, "BT /F1 12 Tf 72 720 Td <0001> Tj ET\n"),
        ainiux::pdf::Options{}, markdown);
    check(err.ok(), "unterminated ToUnicode section produces a bounded page result");
    check(markdown.find("unterminated ToUnicode bfchar section") != std::string::npos,
          "unterminated ToUnicode bfchar reports an extraction error instead of looping");
}

void test_to_markdown_russian_needle() {
    const std::string bytes = read_fixture("tests/pdf_files/ru-llm-eval-short-note.pdf");
    std::string markdown;
    const ainiux::Error err = ainiux::pdf::to_markdown_bytes(bytes, ainiux::pdf::Options{}, markdown);
    check(err.ok(), "Russian PDF to markdown: " + err.message);
    check(markdown.find("языковая модель") != std::string::npos || markdown.find("языков") != std::string::npos,
          "Russian PDF markdown keeps Cyrillic");
}

bool has_rtl_or_logical(const std::string& markdown, const std::string& logical) {
    if (markdown.find(logical) != std::string::npos) {
        return true;
    }
    std::vector<std::string> chars;
    for (std::size_t i = 0; i < logical.size();) {
        const unsigned char lead = static_cast<unsigned char>(logical[i]);
        std::size_t n = 1;
        if ((lead & 0x80) == 0) {
            n = 1;
        } else if ((lead & 0xE0) == 0xC0) {
            n = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            n = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            n = 4;
        }
        if (n == 0 || i + n > logical.size()) {
            n = 1;
        }
        chars.push_back(logical.substr(i, n));
        i += n;
    }
    std::string visual;
    for (auto it = chars.rbegin(); it != chars.rend(); ++it) {
        visual += *it;
    }
    return markdown.find(visual) != std::string::npos;
}

void test_to_markdown_arabic_needles() {
    const std::string bytes = read_fixture("tests/pdf_files/arabic-prose-sample.pdf");
    ainiux::pdf::Document document;
    const ainiux::Error open_err = ainiux::pdf::Document::open_bytes(bytes, ainiux::pdf::Options{}, document);
    check(open_err.ok(), "Arabic fixture opens: " + open_err.message);
    check(document.page_count() == 8, "Arabic fixture is eight pages");
    std::string markdown;
    const ainiux::Error err = ainiux::pdf::to_markdown_bytes(bytes, ainiux::pdf::Options{}, markdown);
    check(err.ok(), "Arabic PDF to markdown: " + err.message);
    check(markdown.find("Ibn al-Muqaffa") != std::string::npos, "Arabic PDF keeps Latin name");
    check(markdown.find("https://example.org/kalila") != std::string::npos, "Arabic PDF keeps URL");
    check(has_rtl_or_logical(markdown, "كليلة"), "Arabic PDF keeps كليلة");
    check(has_rtl_or_logical(markdown, "دمنة"), "Arabic PDF keeps دمنة");
    check(has_rtl_or_logical(markdown, "شتربة"), "Arabic PDF keeps شتربة");
}

void test_arabic_shaping() {
    std::vector<unsigned> lam_alef;
    ainiux::pdf::decode_utf8("لا", lam_alef);
    ainiux::pdf::shape_arabic(lam_alef);
    check(lam_alef.size() == 1 && lam_alef[0] == 0xFEFB, "لا becomes Lam-Alef isolated ligature");

    std::vector<unsigned> bab;
    ainiux::pdf::decode_utf8("باب", bab);
    ainiux::pdf::shape_arabic(bab);
    check(bab.size() == 3, "باب stays three glyphs");
    check(bab[0] == 0xFE91 && bab[1] == 0xFE8E && bab[2] == 0xFE8F,
          "باب shapes to beh-init, alef-final, beh-isol");

    std::vector<unsigned> already = {0xFEFB};
    ainiux::pdf::shape_arabic(already);
    check(already.size() == 1 && already[0] == 0xFEFB, "already-shaped ligature is left alone");
}

void test_hebrew_visual_order() {
    std::vector<unsigned> mixed;
    ainiux::pdf::decode_utf8("Hello שלום", mixed);
    const std::vector<unsigned> visual = ainiux::pdf::visual_order_rtl(mixed);
    const std::string out = ainiux::pdf::encode_utf8(visual);
    check(out.find("Hello") != std::string::npos, "visual RTL keeps Hello");
    check(out.size() >= 5 && out.compare(out.size() - 5, 5, "Hello") == 0, "Hello stays a trailing LTR run");
    check(has_rtl_or_logical(out, "שלום"), "visual RTL keeps שלום");

    std::vector<unsigned> dotted = {0x05D1, 0x05BC, 0x05D0};
    ainiux::pdf::reverse_grapheme_clusters(dotted);
    check(dotted.size() == 3 && dotted[0] == 0x05D0 && dotted[1] == 0x05D1 && dotted[2] == 0x05BC,
          "grapheme reverse keeps Hebrew dagesh on its base");
}

void test_to_markdown_hebrew_needles() {
    const std::string bytes = read_fixture("tests/pdf_files/hebrew-prose-sample.pdf");
    ainiux::pdf::Document document;
    const ainiux::Error open_err = ainiux::pdf::Document::open_bytes(bytes, ainiux::pdf::Options{}, document);
    check(open_err.ok(), "Hebrew fixture opens: " + open_err.message);
    check(document.page_count() == 1, "Hebrew fixture is one page");
    std::string markdown;
    const ainiux::Error err = ainiux::pdf::to_markdown_bytes(bytes, ainiux::pdf::Options{}, markdown);
    check(err.ok(), "Hebrew PDF to markdown: " + err.message);
    check(markdown.find("Hebrew RTL sample") != std::string::npos, "Hebrew PDF keeps English caption");
    check(has_rtl_or_logical(markdown, "תהלים"), "Hebrew PDF keeps תהלים");
    check(has_rtl_or_logical(markdown, "מזמור לדוד"), "Hebrew PDF keeps מזמור לדוד");
    check(has_rtl_or_logical(markdown, "לא אחסר"), "Hebrew PDF keeps לא אחסר");
    check(has_rtl_or_logical(markdown, "בגיא צלמות"), "Hebrew PDF keeps בגיא צלמות");
    check(has_rtl_or_logical(markdown, "לארך ימים"), "Hebrew PDF keeps לארך ימים");
}

void test_to_markdown_traditional_chinese_needles() {
    const std::string bytes = read_fixture("tests/pdf_files/chinese-tang-poems-traditional.pdf");
    ainiux::pdf::Document document;
    const ainiux::Error open_err = ainiux::pdf::Document::open_bytes(bytes, ainiux::pdf::Options{}, document);
    check(open_err.ok(), "Traditional Chinese fixture opens: " + open_err.message);
    check(document.page_count() == 1, "Traditional Chinese fixture is one page");
    std::string markdown;
    const ainiux::Error err = ainiux::pdf::to_markdown_bytes(bytes, ainiux::pdf::Options{}, markdown);
    check(err.ok(), "Traditional Chinese PDF to markdown: " + err.message);
    check(markdown.find("靜夜思") != std::string::npos, "Traditional PDF keeps 靜夜思");
    check(markdown.find("舉頭") != std::string::npos, "Traditional PDF keeps 舉頭");
    check(markdown.find("故鄉") != std::string::npos, "Traditional PDF keeps 故鄉");
    check(markdown.find("不覺曉") != std::string::npos, "Traditional PDF keeps 不覺曉");
    check(markdown.find("處處") != std::string::npos, "Traditional PDF keeps 處處");
    check(markdown.find("夜來風雨聲") != std::string::npos, "Traditional PDF keeps 夜來風雨聲");
    check(markdown.find("登鸛雀樓") != std::string::npos, "Traditional PDF keeps 登鸛雀樓");
    check(markdown.find("黃河") != std::string::npos, "Traditional PDF keeps 黃河");
    check(markdown.find("欲窮") != std::string::npos, "Traditional PDF keeps 欲窮");
    check(markdown.find("一層樓") != std::string::npos, "Traditional PDF keeps 一層樓");
    check(markdown.find("静夜思") == std::string::npos, "Traditional PDF is not Simplified 静夜思");
    check(markdown.find("故乡") == std::string::npos, "Traditional PDF is not Simplified 故乡");
    check(markdown.find("一层楼") == std::string::npos, "Traditional PDF is not Simplified 一层楼");
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

void test_tj_tex_word_glue_is_not_glued() {
    const std::string markdown =
        markdown_of("BT /F1 12 Tf 72 720 Td [(economic)-237(growth)-237(and)-348(labor)] TJ ET\n");
    check(markdown.find("economic growth and labor") != std::string::npos,
          "TeX-style 0.24 em TJ glue remains word spaces");
    check(markdown.find("economicgrowth") == std::string::npos, "0.24 em TJ glue does not concatenate words");
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

void test_from_markdown_print_margins_and_bold_italic() {
    ainiux::pdf::WriteOptions options;
    std::string pdf;
    const ainiux::Error err =
        ainiux::pdf::from_markdown("# Title\n\n***bold italic*** word\n\n> quoted line\n", options, pdf);
    check(err.ok(), "margin/style PDF: " + err.message);
    const size_t media = pdf.find("/MediaBox [0 0 ");
    const size_t crop = pdf.find("/CropBox [0 0 ");
    check(media != std::string::npos && crop != std::string::npos,
          "page writes both MediaBox and CropBox");
    const size_t media_end = pdf.find(']', media);
    const size_t crop_end = pdf.find(']', crop);
    check(media_end != std::string::npos && crop_end != std::string::npos &&
              pdf.substr(media + 10, media_end - (media + 10)) ==
                  pdf.substr(crop + 9, crop_end - (crop + 9)),
          "CropBox equals MediaBox so Edge uses the full paper");
    check(pdf.find("/Helvetica-BoldOblique") != std::string::npos,
          "bold-italic uses Helvetica-BoldOblique");
    ainiux::pdf::Document document;
    const ainiux::Error open_err = ainiux::pdf::Document::open_bytes(pdf, ainiux::pdf::Options{}, document);
    check(open_err.ok(), "margin/style PDF opens: " + open_err.message);
    std::string content;
    const ainiux::Error content_err = document.page_content(0, content);
    check(content_err.ok(), "page content decodes: " + content_err.message);
    check(content.find("/FBI") != std::string::npos, "content selects the bold-italic font");
    check(content.find("0 0 ") != std::string::npos && content.find("792 re") != std::string::npos,
          "page paints a full MediaBox fill so ink-bbox equals the paper");
    bool found_bar = false;
    for (size_t pos = 0; (pos = content.find(" re\n", pos)) != std::string::npos; pos += 4) {
        const size_t line_start = content.rfind('\n', pos);
        const std::string rect =
            content.substr(line_start == std::string::npos ? 0 : line_start + 1,
                           pos - (line_start == std::string::npos ? 0 : line_start + 1));
        if (rect.find("78 ") != std::string::npos) {
            found_bar = true;
            break;
        }
    }
    check(found_bar, "blockquote bar sits in the 1 inch left margin gutter, not on the page edge");
}

void test_from_markdown_hello_roundtrip() {
    ainiux::pdf::WriteOptions options;
    std::string pdf;
    const ainiux::Error err = ainiux::pdf::from_markdown("# Title\n\nHello PDF.\n", options, pdf);
    check(err.ok(), "markdown to PDF: " + err.message);
    check(pdf.rfind("%PDF-1.4", 0) == 0, "written PDF starts with %PDF-1.4");
    ainiux::pdf::Document document;
    const ainiux::Error open_err = ainiux::pdf::Document::open_bytes(pdf, ainiux::pdf::Options{}, document);
    check(open_err.ok(), "written PDF opens: " + open_err.message);
    check(document.page_count() >= 1, "written PDF has a page");
    std::string markdown;
    const ainiux::Error extract_err = ainiux::pdf::to_markdown_bytes(pdf, ainiux::pdf::Options{}, markdown);
    check(extract_err.ok(), "written PDF extracts: " + extract_err.message);
    check(markdown.find("Title") != std::string::npos, "PDF extract contains heading");
    check(markdown.find("Hello PDF") != std::string::npos, "PDF extract contains paragraph");
}

void test_from_markdown_thematic_break_is_rule_not_page_break() {
    ainiux::pdf::WriteOptions options;
    std::string pdf;
    const ainiux::Error err = ainiux::pdf::from_markdown("Alpha\n\n---\n\nBeta\n", options, pdf);
    check(err.ok(), "thematic break PDF: " + err.message);
    ainiux::pdf::Document document;
    const ainiux::Error open_err = ainiux::pdf::Document::open_bytes(pdf, ainiux::pdf::Options{}, document);
    check(open_err.ok(), "thematic break PDF opens: " + open_err.message);
    check(document.page_count() == 1, "thematic break stays on one page");
    std::string content;
    const ainiux::Error content_err = document.page_content(0, content);
    check(content_err.ok(), "thematic break content decodes: " + content_err.message);
    check(content.find(" l S") != std::string::npos, "thematic break draws a hairline");
    std::string markdown;
    const ainiux::Error extract_err = ainiux::pdf::to_markdown_bytes(pdf, ainiux::pdf::Options{}, markdown);
    check(extract_err.ok(), "thematic break extract: " + extract_err.message);
    check(markdown.find("Alpha") != std::string::npos && markdown.find("Beta") != std::string::npos,
          "both sides of the rule survive extract");
}

void test_from_markdown_coalesces_line_into_one_show() {
    ainiux::pdf::WriteOptions options;
    std::string pdf;
    const ainiux::Error err = ainiux::pdf::from_markdown("Hello world from Ainiux.\n", options, pdf);
    check(err.ok(), "coalesce PDF: " + err.message);
    ainiux::pdf::Document document;
    const ainiux::Error open_err = ainiux::pdf::Document::open_bytes(pdf, ainiux::pdf::Options{}, document);
    check(open_err.ok(), "coalesce PDF opens: " + open_err.message);
    std::string content;
    const ainiux::Error content_err = document.page_content(0, content);
    check(content_err.ok(), "coalesce content decodes: " + content_err.message);
    check(content.find("(Hello world from Ainiux.)") != std::string::npos,
          "words on a line share one Tj: " + content);
    size_t tm = 0;
    for (size_t pos = 0; (pos = content.find(" Tm\n", pos)) != std::string::npos; pos += 4) {
        ++tm;
    }
    check(tm <= 4, "line coalescing keeps Tm count near lines, not words: " + std::to_string(tm));
}

void test_to_markdown_does_not_insert_thematic_rules() {
    const std::string bytes = read_fixture("tests/pdf_files/ru-transformers-short-note.pdf");
    std::string markdown;
    const ainiux::Error err = ainiux::pdf::to_markdown_bytes(bytes, ainiux::pdf::Options{}, markdown);
    check(err.ok(), "two-page extract: " + err.message);
    check(markdown.find("\n---\n") == std::string::npos,
          "PDF extract does not insert --- page separators");
}

void test_from_markdown_llm_typical_needles() {
    const std::string input = read_fixture("tests/fixtures/llm_typical.md");
    ainiux::pdf::WriteOptions options;
    std::string pdf;
    const ainiux::Error err = ainiux::pdf::from_markdown(input, options, pdf);
    check(err.ok(), "llm_typical markdown to PDF: " + err.message);
    std::string markdown;
    const ainiux::Error extract_err = ainiux::pdf::to_markdown_bytes(pdf, ainiux::pdf::Options{}, markdown);
    check(extract_err.ok(), "llm_typical PDF extract: " + extract_err.message);
    check(markdown.find("LLM Typical Markdown Fixture") != std::string::npos, "PDF keeps H1");
    check(markdown.find("unordered alpha") != std::string::npos, "PDF keeps list item");
    check(markdown.find("def greet") != std::string::npos, "PDF keeps fenced code");
    check(markdown.find("https://example.com/path?q=1") != std::string::npos, "PDF keeps link URL");
    check(markdown.find("Bold") != std::string::npos || markdown.find("bold") != std::string::npos,
          "PDF keeps table or inline bold");
}

void test_html_to_pdf_via_markdown() {
    const std::string html = "<h1>Fetched</h1><p>Hello from HTML.</p><ul><li>one</li></ul>";
    const std::string md = ainiux::html::convert(html, ainiux::html::OutputFormat::Markdown);
    ainiux::pdf::WriteOptions options;
    std::string pdf;
    const ainiux::Error err = ainiux::pdf::from_markdown(md, options, pdf);
    check(err.ok(), "HTML via markdown to PDF: " + err.message);
    std::string markdown;
    const ainiux::Error extract_err = ainiux::pdf::to_markdown_bytes(pdf, ainiux::pdf::Options{}, markdown);
    check(extract_err.ok(), "HTML PDF extract: " + extract_err.message);
    check(markdown.find("Fetched") != std::string::npos, "HTML PDF keeps heading");
    check(markdown.find("Hello from HTML") != std::string::npos, "HTML PDF keeps paragraph");
    check(markdown.find("one") != std::string::npos, "HTML PDF keeps list item");
}

void test_pdf_md_pdf_latin_needles() {
    const std::string bytes = read_fixture("tests/pdf_files/DeepSeek2501.12948v1.pdf");
    std::string markdown;
    const ainiux::Error extract_err = ainiux::pdf::to_markdown_bytes(bytes, ainiux::pdf::Options{}, markdown);
    check(extract_err.ok(), "DeepSeek extract for reflow: " + extract_err.message);
    ainiux::pdf::WriteOptions options;
    std::string pdf;
    const ainiux::Error write_err = ainiux::pdf::from_markdown(markdown, options, pdf);
    check(write_err.ok(), "DeepSeek reflow write: " + write_err.message);
    std::string again;
    const ainiux::Error again_err = ainiux::pdf::to_markdown_bytes(pdf, ainiux::pdf::Options{}, again);
    check(again_err.ok(), "DeepSeek reflow extract: " + again_err.message);
    check(again.find("DeepSeek") != std::string::npos, "reflow keeps DeepSeek");
    check(again.find("reinforcement") != std::string::npos || again.find("Reinforcement") != std::string::npos,
          "reflow keeps reinforcement");
}

void test_emoji_still_substituted() {
    ainiux::pdf::WriteOptions options;
    std::string pdf;
    const ainiux::Error err = ainiux::pdf::from_markdown("Hello 😀 world\n", options, pdf);
    check(err.ok(), "emoji substitution still writes PDF: " + err.message);
    check(options.substituted_glyphs > 0, "emoji glyphs are counted as substitutions");
    std::string markdown;
    const ainiux::Error extract_err = ainiux::pdf::to_markdown_bytes(pdf, ainiux::pdf::Options{}, markdown);
    check(extract_err.ok(), "substitution PDF extracts: " + extract_err.message);
    check(markdown.find("Hello") != std::string::npos && markdown.find("world") != std::string::npos,
          "Latin text survives substitution");
    check(markdown.find("?") != std::string::npos, "unencodable glyphs become question marks");
}

std::string write_placeholder_font() {
    std::string ttf;
    const ainiux::Error err = ainiux::pdf::make_placeholder_cjk_ttf(ttf);
    check(err.ok(), "placeholder CJK TTF: " + err.message);
    const char* path = "build/test_placeholder_cjk.ttf";
    std::ofstream out(path, std::ios::binary);
    check(static_cast<bool>(out), "can write placeholder font");
    out.write(ttf.data(), static_cast<std::streamsize>(ttf.size()));
    check(static_cast<bool>(out), "placeholder font write finishes");
    return path;
}

void test_cjk_roundtrip_with_embedded_ttf() {
    ainiux::pdf::WriteOptions options;
    options.font_path = write_placeholder_font();
    std::string pdf;
    const ainiux::Error err = ainiux::pdf::from_markdown("Hello 中文\n", options, pdf);
    check(err.ok(), "CJK markdown to PDF: " + err.message);
    check(pdf.find("/Identity-H") != std::string::npos, "CJK PDF uses Identity-H");
    check(pdf.find("/CIDFontType2") != std::string::npos, "CJK PDF embeds CIDFontType2");
    check(options.substituted_glyphs == 0, "embedded TTF avoids CJK substitution");
    std::string markdown;
    const ainiux::Error extract_err = ainiux::pdf::to_markdown_bytes(pdf, ainiux::pdf::Options{}, markdown);
    check(extract_err.ok(), "CJK PDF extracts: " + extract_err.message);
    check(markdown.find("Hello") != std::string::npos, "CJK PDF keeps Hello");
    check(markdown.find("中") != std::string::npos && markdown.find("文") != std::string::npos,
          "CJK PDF round-trip keeps 中文: " + markdown);
}

void test_cjk_missing_font_path_errors() {
    ainiux::pdf::WriteOptions options;
    options.font_path = "build/no-such-cjk-font.ttf";
    std::string pdf;
    const ainiux::Error err = ainiux::pdf::from_markdown("中\n", options, pdf);
    check(!err.ok(), "missing --font path fails when CJK is present");
}

std::string write_placeholder_rtl_font() {
    std::string ttf;
    const ainiux::Error err = ainiux::pdf::make_placeholder_rtl_ttf(ttf);
    check(err.ok(), "placeholder RTL TTF: " + err.message);
    const char* path = "build/test_placeholder_rtl.ttf";
    std::ofstream out(path, std::ios::binary);
    check(static_cast<bool>(out), "can write RTL placeholder font");
    out.write(ttf.data(), static_cast<std::streamsize>(ttf.size()));
    check(static_cast<bool>(out), "RTL placeholder font write finishes");
    return path;
}

void test_rtl_roundtrip_with_embedded_ttf() {
    ainiux::pdf::WriteOptions options;
    options.font_path = write_placeholder_rtl_font();
    std::string pdf;
    const ainiux::Error err = ainiux::pdf::from_markdown("Hello שלום\n\nباب و لا\n", options, pdf);
    check(err.ok(), "RTL markdown to PDF: " + err.message);
    check(pdf.find("/Identity-H") != std::string::npos, "RTL PDF uses Identity-H");
    check(pdf.find("/CIDFontType2") != std::string::npos, "RTL PDF embeds CIDFontType2");
    check(pdf.find("/FA ") != std::string::npos, "RTL PDF uses /FA font resource");
    check(options.substituted_glyphs == 0, "embedded RTL TTF avoids substitution");
    check(!options.rtl_font_missing, "placeholder RTL font is not reported missing");
    std::string markdown;
    const ainiux::Error extract_err = ainiux::pdf::to_markdown_bytes(pdf, ainiux::pdf::Options{}, markdown);
    check(extract_err.ok(), "RTL PDF extracts: " + extract_err.message);
    check(markdown.find("Hello") != std::string::npos, "RTL PDF keeps Hello");
    check(has_rtl_or_logical(markdown, "שלום"), "RTL PDF round-trip keeps שלום: " + markdown);
    check(has_rtl_or_logical(markdown, "باب"), "RTL PDF round-trip keeps باب: " + markdown);
    check(has_rtl_or_logical(markdown, "لا"), "RTL PDF round-trip keeps لا: " + markdown);
}

void test_rtl_missing_font_path_errors() {
    ainiux::pdf::WriteOptions options;
    options.font_path = "build/no-such-rtl-font.ttf";
    std::string pdf;
    const ainiux::Error err = ainiux::pdf::from_markdown("שלום\n", options, pdf);
    check(!err.ok(), "missing --font path fails when Hebrew is present");
}

void test_pdf_md_pdf_hebrew_needles() {
    const std::string bytes = read_fixture("tests/pdf_files/hebrew-prose-sample.pdf");
    std::string markdown;
    const ainiux::Error extract_err = ainiux::pdf::to_markdown_bytes(bytes, ainiux::pdf::Options{}, markdown);
    check(extract_err.ok(), "Hebrew extract for reflow: " + extract_err.message);
    ainiux::pdf::WriteOptions options;
    options.font_path = write_placeholder_rtl_font();
    std::string pdf;
    const ainiux::Error write_err = ainiux::pdf::from_markdown(markdown, options, pdf);
    check(write_err.ok(), "Hebrew reflow write: " + write_err.message);
    std::string again;
    const ainiux::Error again_err = ainiux::pdf::to_markdown_bytes(pdf, ainiux::pdf::Options{}, again);
    check(again_err.ok(), "Hebrew reflow extract: " + again_err.message);
    check(again.find("Hebrew RTL sample") != std::string::npos, "Hebrew reflow keeps English caption");
    check(has_rtl_or_logical(again, "תהלים"), "Hebrew reflow keeps תהלים");
    check(has_rtl_or_logical(again, "מזמור לדוד"), "Hebrew reflow keeps מזמור לדוד");
    check(has_rtl_or_logical(again, "לא אחסר"), "Hebrew reflow keeps לא אחסר");
}

void test_pdf_md_pdf_arabic_needles() {
    const std::string bytes = read_fixture("tests/pdf_files/arabic-prose-sample.pdf");
    std::string markdown;
    const ainiux::Error extract_err = ainiux::pdf::to_markdown_bytes(bytes, ainiux::pdf::Options{}, markdown);
    check(extract_err.ok(), "Arabic extract for reflow: " + extract_err.message);
    ainiux::pdf::WriteOptions options;
    options.font_path = write_placeholder_rtl_font();
    std::string pdf;
    const ainiux::Error write_err = ainiux::pdf::from_markdown(markdown, options, pdf);
    check(write_err.ok(), "Arabic reflow write: " + write_err.message);
    std::string again;
    const ainiux::Error again_err = ainiux::pdf::to_markdown_bytes(pdf, ainiux::pdf::Options{}, again);
    check(again_err.ok(), "Arabic reflow extract: " + again_err.message);
    check(again.find("Ibn al-Muqaffa") != std::string::npos, "Arabic reflow keeps Latin name");
    check(again.find("https://example.org/kalila") != std::string::npos, "Arabic reflow keeps URL");
    check(has_rtl_or_logical(again, "كليلة"), "Arabic reflow keeps كليلة");
    check(has_rtl_or_logical(again, "دمنة"), "Arabic reflow keeps دمنة");
    check(has_rtl_or_logical(again, "شتربة"), "Arabic reflow keeps شتربة");
}

void test_empty_markdown_is_one_page() {
    ainiux::pdf::WriteOptions options;
    std::string pdf;
    const ainiux::Error err = ainiux::pdf::from_markdown("", options, pdf);
    check(err.ok(), "empty markdown writes PDF: " + err.message);
    ainiux::pdf::Document document;
    const ainiux::Error open_err = ainiux::pdf::Document::open_bytes(pdf, ainiux::pdf::Options{}, document);
    check(open_err.ok(), "empty PDF opens: " + open_err.message);
    check(document.page_count() == 1, "empty markdown yields one page");
}

void test_pdf_write_cancel() {
    ainiux::runtime::CancellationSource source;
    source.cancel();
    ainiux::pdf::WriteOptions options;
    options.cancellation = source.token();
    std::string pdf;
    const ainiux::Error err = ainiux::pdf::from_markdown("# A\n\n---\n\n# B\n", options, pdf);
    check(!err.ok() && err.code == ainiux::ErrorCode::Cancelled, "cancelled markdown-to-PDF returns Cancelled");
    check(pdf.empty(), "cancelled write leaves no PDF bytes");
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
    test_to_markdown_consumes_bfrange_destination_array();
    test_to_markdown_rejects_unterminated_bfchar();
    test_to_markdown_russian_needle();
    test_to_markdown_hebrew_needles();
    test_to_markdown_arabic_needles();
    test_arabic_shaping();
    test_hebrew_visual_order();
    test_to_markdown_traditional_chinese_needles();
    test_tj_kerning_does_not_split_words();
    test_tj_word_gap_inserts_one_space();
    test_tj_tex_word_glue_is_not_glued();
    test_tj_does_not_space_before_comma();
    test_td_word_gap_inserts_one_space();
    test_to_markdown_deepseek_does_not_split_words();
    test_from_markdown_print_margins_and_bold_italic();
    test_from_markdown_hello_roundtrip();
    test_from_markdown_thematic_break_is_rule_not_page_break();
    test_from_markdown_coalesces_line_into_one_show();
    test_to_markdown_does_not_insert_thematic_rules();
    test_from_markdown_llm_typical_needles();
    test_html_to_pdf_via_markdown();
    test_pdf_md_pdf_latin_needles();
    test_emoji_still_substituted();
    test_cjk_roundtrip_with_embedded_ttf();
    test_cjk_missing_font_path_errors();
    test_rtl_roundtrip_with_embedded_ttf();
    test_rtl_missing_font_path_errors();
    test_pdf_md_pdf_hebrew_needles();
    test_pdf_md_pdf_arabic_needles();
    test_empty_markdown_is_one_page();
    test_pdf_write_cancel();
}

}  // namespace ainiux::test::pdf
