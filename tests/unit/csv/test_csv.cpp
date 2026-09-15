#include "csv/test_csv.hpp"
#include "support/test_support.hpp"
#include "csv/csv.hpp"
#include "runtime/runtime.hpp"
#include <string>

namespace ainiux::test::csv {
namespace {

using ainiux::test::check;

std::string convert(const std::string& input, ainiux::csv::Diagnostics* diagnostics = nullptr) {
    std::string markdown;
    const ainiux::Error error = ainiux::csv::to_markdown_bytes(input, {}, markdown, diagnostics);
    check(error.ok(), "CSV converts to Markdown: " + error.message);
    return markdown;
}

void test_simple_table() {
    const std::string markdown = convert("Name,Value\nHello,7\n");
    check(markdown.find("| Name") != std::string::npos && markdown.find("| Value") != std::string::npos,
          "CSV header becomes a GFM table header");
    check(markdown.find("| Hello") != std::string::npos && markdown.find("| 7") != std::string::npos,
          "CSV body rows become GFM table rows");
    check(markdown.find("---") != std::string::npos, "CSV table includes a separator row");
}

void test_quoted_comma_and_quotes() {
    const std::string markdown = convert("a,b\n\"x,y\",\"he said \"\"hi\"\"\"\n");
    check(markdown.find("x,y") != std::string::npos, "quoted CSV commas stay inside the cell");
    check(markdown.find("he said \"hi\"") != std::string::npos, "CSV doubled quotes unescape");
}

void test_quoted_newline() {
    const std::string markdown = convert("h1,h2\n\"line1\nline2\",ok\n");
    check(markdown.find("<br>") != std::string::npos, "quoted CSV newlines become <br> in GFM cells");
    check(markdown.find("| ok") != std::string::npos, "row continues after a quoted newline");
}

void test_crlf() {
    const std::string markdown = convert("a,b\r\n1,2\r\n");
    check(markdown.find("| 1") != std::string::npos && markdown.find("| 2") != std::string::npos,
          "CRLF CSV records parse as two rows");
}

void test_ragged_rows() {
    ainiux::csv::Diagnostics diagnostics;
    const std::string markdown = convert("a,b,c\n1,2\n", &diagnostics);
    check(markdown.find("| 1") != std::string::npos, "ragged CSV still converts");
    check(diagnostics.ragged_rows == 1 && !diagnostics.messages.empty(),
          "ragged CSV rows produce a diagnostic");
}

void test_pipe_escaping() {
    const std::string markdown = convert("a,b\n1|2,3\n");
    check(markdown.find("1\\|2") != std::string::npos, "pipe characters in CSV cells are escaped for GFM");
}

void test_utf8() {
    const std::string markdown = convert(u8"nimi,arvo\nkäyttäjä,7\n");
    check(markdown.find(u8"käyttäjä") != std::string::npos, "CSV preserves UTF-8 cell text");
}

void test_bom() {
    const std::string markdown = convert("\xEF\xBB\xBF" "a,b\n1,2\n");
    check(markdown.find("| a") != std::string::npos, "UTF-8 BOM is skipped");
}

void test_empty_and_invalid() {
    std::string markdown;
    ainiux::Error err = ainiux::csv::to_markdown_bytes("", {}, markdown);
    check(!err.ok(), "empty CSV is rejected");
    err = ainiux::csv::to_markdown_bytes("\"unterminated", {}, markdown);
    check(!err.ok() && err.message.find("unclosed quoted field") != std::string::npos,
          "unclosed quoted CSV field is rejected");
}

void test_cancellation() {
    ainiux::runtime::CancellationSource source;
    source.cancel();
    ainiux::csv::ReadOptions options;
    options.cancellation = source.token();
    std::string markdown;
    const ainiux::Error err = ainiux::csv::to_markdown_bytes("a,b\n1,2\n", options, markdown);
    check(!err.ok() && err.code == ainiux::ErrorCode::Cancelled, "CSV conversion observes cancellation");
}

}  // namespace

void run_all() {
    test_simple_table();
    test_quoted_comma_and_quotes();
    test_quoted_newline();
    test_crlf();
    test_ragged_rows();
    test_pipe_escaping();
    test_utf8();
    test_bom();
    test_empty_and_invalid();
    test_cancellation();
}

}  // namespace ainiux::test::csv
