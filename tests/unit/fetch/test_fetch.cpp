#include "fetch/test_fetch.hpp"
#include "support/test_support.hpp"
#include "fetch/fetch.hpp"
#include "html/html.hpp"
#include "json/json.hpp"
#include <cstdio>
#include <string>
#include <vector>

namespace ainiux::test::fetch {

namespace {

using ainiux::test::check;
using ainiux::test::read_fixture;

void test_safe_fetch_rejects_private_literal() {
    ainiux::fetch::Options options;
    std::string body;
    ainiux::Error err = ainiux::fetch::fetch_html("http://127.0.0.1/private", options, body);
    check(!err.ok() && err.code == ainiux::ErrorCode::BadUrl,
          "shared URL fetch rejects a private literal before transport");
    err = ainiux::fetch::fetch_html("file:///tmp/page.html", options, body);
    check(!err.ok() && err.code == ainiux::ErrorCode::BadUrl,
          "shared URL fetch rejects non-HTTP schemes");
}

void test_fetch_validation_edge_cases() {
    ainiux::fetch::Options options;
    check(options.max_bytes == 10L * 1024L * 1024L,
          "shared URL fetch defaults to a 10 MiB response cap");
    std::string body;
    ainiux::Error err = ainiux::fetch::fetch_html("", options, body);
    check(!err.ok() && err.code == ainiux::ErrorCode::BadArgs &&
              err.message.find("non-empty URL") != std::string::npos,
          "URL fetch rejects an empty URL");
    options.max_bytes = 0;
    err = ainiux::fetch::fetch_html("https://example.com/", options, body);
    check(!err.ok() && err.code == ainiux::ErrorCode::BadArgs &&
              err.message.find("greater than zero") != std::string::npos,
          "URL fetch rejects zero max-bytes limit");
    options.max_bytes = 1024;
    err = ainiux::fetch::fetch_html("http://localhost/page", options, body);
    check(!err.ok() && err.code == ainiux::ErrorCode::BadUrl,
          "URL fetch rejects localhost before transport");
    err = ainiux::fetch::fetch_html("http://10.0.0.1/internal", options, body);
    check(!err.ok() && err.code == ainiux::ErrorCode::BadUrl,
          "URL fetch rejects RFC1918 addresses before transport");
    err = ainiux::fetch::fetch_html("http://[::1]/", options, body);
    check(!err.ok() && err.code == ainiux::ErrorCode::BadUrl,
          "URL fetch rejects IPv6 loopback before transport");
    err = ainiux::fetch::fetch_html("not-a-url", options, body);
    check(!err.ok() && err.code == ainiux::ErrorCode::BadUrl,
          "URL fetch rejects malformed URLs before transport");
    options.proxy = "http://proxy.local:8080";
    err = ainiux::fetch::fetch_html("https://example.com/", options, body);
    check(!err.ok() && err.code == ainiux::ErrorCode::BadUrl &&
              err.message.find("proxy") != std::string::npos,
          "URL fetch rejects proxy use without private-fetch override");
}

void test_iso8859_1_html_to_utf8() {
    // Mirrors matti.naskali.net-style pages: meta charset=iso-8859-1 with 0xE4 = ä.
    const std::string latin1_html =
        "<html>\r\n<head>\r\n"
        "   <meta http-equiv=\"Content-Type\" content=\"text/html; charset=iso-8859-1\">\r\n"
        "   <title>Eino Leino - Lapin Kes\xe4</title>\r\n"
        "</head><body>Lapissa kaikki kukkii: kes\xe4</body></html>";
    check(!ainiux::html::is_valid_utf8(latin1_html),
          "fixture is intentionally not UTF-8");

    const std::string utf8 =
        ainiux::fetch::convert_fetched_body_to_utf8(latin1_html, "text/html");
    check(ainiux::html::is_valid_utf8(utf8), "converted body is valid UTF-8");
    check(utf8.find("Lapin Kesä") != std::string::npos,
          "ISO-8859-1 ä becomes UTF-8 in title");
    check(utf8.find("kesä") != std::string::npos, "ISO-8859-1 ä becomes UTF-8 in body");

    // JSON tool results must remain well-formed UTF-8 for local model servers.
    ainiux::json::Value root;
    root.type = ainiux::json::Value::Type::Object;
    ainiux::json::Value body_value;
    body_value.type = ainiux::json::Value::Type::String;
    body_value.string = utf8;
    root.object["body"] = std::move(body_value);
    const std::string json = ainiux::json::stringify(root);
    check(ainiux::html::is_valid_utf8(json), "JSON tool payload is valid UTF-8");
    check(json.find("Kesä") != std::string::npos, "JSON contains the poem title as UTF-8");
}

void test_windows1251_html_to_utf8() {
    const std::string html =
        "<html><head><meta charset=\"windows-1251\"></head><body>Ya \xff</body></html>";
    const std::string utf8 =
        ainiux::fetch::convert_fetched_body_to_utf8(html, "text/html");
    check(ainiux::html::is_valid_utf8(utf8), "CP1251 fetch body is UTF-8");
    check(utf8.find(u8"я") != std::string::npos, "declared windows-1251 uses the Cyrillic map");
}

std::string make_hello_pdf() {
    const std::string content = "BT /F1 12 Tf 72 720 Td (Hello PDF) Tj ET\n";
    const std::string obj4 =
        "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "endstream";
    std::vector<std::string> bodies = {
        "",
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R "
        "/Resources << /Font << /F1 5 0 R >> >> >>",
        obj4,
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    };
    std::string out = "%PDF-1.4\n";
    std::vector<std::size_t> offsets(bodies.size(), 0);
    auto xref_line = [](std::size_t offset, int gen, char flag) {
        char line[22];
        std::snprintf(line, sizeof(line), "%010zu %05d %c \n", offset, gen, flag);
        return std::string(line);
    };
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

void test_pdf_fetch_classification() {
    check(ainiux::fetch::media_type_is_pdf("application/pdf"), "application/pdf is a PDF type");
    check(ainiux::fetch::media_type_is_pdf("application/x-pdf"), "application/x-pdf is a PDF type");
    check(ainiux::fetch::fetched_media_type("application/pdf; charset=binary") == "application/pdf",
          "PDF media type strips parameters");
    check(ainiux::fetch::body_looks_like_pdf("%PDF-1.4\n"), "PDF magic is detected");
    check(ainiux::fetch::body_looks_like_pdf("\n  %PDF-1.7\n"), "PDF magic ignores leading whitespace");
    check(!ainiux::fetch::body_looks_like_pdf("<html>%PDF-"), "HTML with a PDF mention is not a PDF body");

    const std::string pdf = make_hello_pdf();
    std::string markdown;
    ainiux::fetch::DocumentKind kind = ainiux::fetch::DocumentKind::Html;
    ainiux::Error err =
        ainiux::fetch::markdown_from_fetched_bytes(pdf, "application/pdf", markdown, kind);
    check(err.ok() && kind == ainiux::fetch::DocumentKind::Pdf, "application/pdf converts to Markdown");
    check(markdown.find("Hello PDF") != std::string::npos, "converted PDF Markdown contains page text");

    markdown.clear();
    kind = ainiux::fetch::DocumentKind::Html;
    err = ainiux::fetch::markdown_from_fetched_bytes(pdf, "application/octet-stream", markdown, kind);
    check(err.ok() && kind == ainiux::fetch::DocumentKind::Pdf,
          "octet-stream with %PDF- is sniffed as PDF");

    markdown.clear();
    kind = ainiux::fetch::DocumentKind::Pdf;
    err = ainiux::fetch::markdown_from_fetched_bytes("<html><body>not a pdf</body></html>",
                                                    "text/html", markdown, kind);
    check(err.ok() && kind == ainiux::fetch::DocumentKind::Html,
          "HTML content-type is not treated as PDF even if the URL ended in .pdf");
    check(markdown.find("not a pdf") != std::string::npos, "HTML still converts to Markdown");

    markdown.clear();
    err = ainiux::fetch::markdown_from_fetched_bytes(pdf, "image/png", markdown, kind);
    check(!err.ok() && err.code == ainiux::ErrorCode::UnsupportedFeature,
          "non-document content types are rejected");
}

void test_json_escape_rejects_raw_latin1() {
    // Safety net: even if a caller embeds ISO-8859-1, escape_string must not emit
    // raw 0xE4 into the JSON string.
    const std::string latin1 = "Kes\xe4";
    const std::string escaped = ainiux::json::escape_string(latin1);
    check(escaped.find('\xe4') == std::string::npos,
          "escape_string does not emit raw latin1 high bytes");
    check(escaped.find("\\u00e4") != std::string::npos,
          "escape_string encodes latin1 high bytes as \\u00XX");
}

}  // namespace

void run_all() {
    test_safe_fetch_rejects_private_literal();
    test_fetch_validation_edge_cases();
    test_iso8859_1_html_to_utf8();
    test_windows1251_html_to_utf8();
    test_pdf_fetch_classification();
    test_json_escape_rejects_raw_latin1();
}

}  // namespace ainiux::test::fetch
