#include "xlsx/test_xlsx.hpp"

#include <string>
#include <vector>

#include "docx/zip.hpp"
#include "html/html.hpp"
#include "runtime/runtime.hpp"
#include "support/test_support.hpp"
#include "xlsx/xlsx.hpp"

namespace ainiux::test::xlsx {
namespace {

using ainiux::test::check;
using ainiux::test::read_fixture;

std::string convert_file(const std::string& path, ainiux::xlsx::Diagnostics* diagnostics = nullptr) {
    const std::string bytes = read_fixture(path);
    std::string markdown;
    const Error error = ainiux::xlsx::to_markdown_bytes(bytes, {}, markdown, diagnostics);
    check(error.ok(), path + (error.ok() ? std::string() : ": " + error.message));
    return markdown;
}

void test_round_trip() {
    const std::string markdown =
        "# People\n\n"
        "| Name | Value | Flag |\n"
        "| --- | --- | --- |\n"
        "| Alice | 42 | TRUE |\n"
        "| Bob | 3.14 | FALSE |\n";
    std::string package;
    Error error = ainiux::xlsx::from_markdown(markdown, {}, package);
    check(error.ok() && ainiux::xlsx::looks_like_xlsx(package),
          "Markdown table converts to an XLSX ZIP package");
    std::string again;
    error = ainiux::xlsx::from_markdown(markdown, {}, again);
    check(error.ok() && again == package, "XLSX output is byte-for-byte deterministic");
    std::string round;
    ainiux::xlsx::Diagnostics diagnostics;
    error = ainiux::xlsx::to_markdown_bytes(package, {}, round, &diagnostics);
    check(error.ok(), error.ok() ? "round-trip reads" : error.message);
    check(round.find("# People") != std::string::npos, "round-trip keeps the sheet heading");
    check(round.find("| Alice |") != std::string::npos && round.find("| 42 |") != std::string::npos,
          "round-trip keeps string and integer cells");
    check(round.find("| 3.14 |") != std::string::npos, "round-trip keeps fractional numbers");
    check(round.find("| TRUE |") != std::string::npos && round.find("| FALSE |") != std::string::npos,
          "round-trip keeps booleans");
    check(diagnostics.messages.empty(), "simple round-trip has no diagnostics");

    std::string missing;
    error = ainiux::xlsx::from_markdown("# Title\n\nNo table here.\n", {}, missing);
    check(!error.ok() && error.code == ErrorCode::BadArgs && missing.empty(),
          "Markdown without a table is rejected");
}

void test_multi_sheet_and_pipes() {
    const std::string markdown =
        "# First\n\n| A | B |\n| --- | --- |\n| 1 | a\\|b |\n\n"
        "# Second\n\n| X | Y |\n| --- | --- |\n| 9 | 8 |\n";
    std::string package;
    check(ainiux::xlsx::from_markdown(markdown, {}, package).ok(), "multi-table Markdown writes");
    std::string round;
    check(ainiux::xlsx::to_markdown_bytes(package, {}, round).ok(), "multi-table XLSX reads");
    check(round.find("# First") != std::string::npos && round.find("# Second") != std::string::npos,
          "each Markdown table becomes a worksheet heading");
    check(round.find("a\\|b") != std::string::npos || round.find("a|b") != std::string::npos,
          "pipe characters in cells survive the Markdown table encoding");
}

void test_interoperability_fixtures() {
    ainiux::xlsx::Diagnostics diagnostics;
    const std::string three = convert_file("tests/xlsx_files/threesheets.xlsx", &diagnostics);
    check(three.find("# ") != std::string::npos, "threesheets emits headings");
    check(three.find("First") != std::string::npos && three.find("Second") != std::string::npos &&
              three.find("Third") != std::string::npos,
          "threesheets keeps per-sheet labels");

    const std::string blank = convert_file("tests/xlsx_files/blankcell.xlsx");
    check(blank.find('|') != std::string::npos, "blankcell becomes a Markdown table");

    const std::string noref = convert_file("tests/xlsx_files/without_cell_reference.xlsx");
    check(noref.find("| 1 |") != std::string::npos && noref.find("| 2 |") != std::string::npos,
          "cells without r attributes still decode in order");

    const std::string explicit_date = convert_file("tests/xlsx_files/explicitdate.xlsx");
    check(explicit_date.find("2021-12-31") != std::string::npos,
          "t=d ISO dates are preserved as date text");
    check(explicit_date.find("| 42 |") != std::string::npos, "explicitdate keeps numeric cells");

    const std::string cal1900 = convert_file("tests/xlsx_files/1900_Calendar.xlsx");
    check(cal1900.find('|') != std::string::npos, "1900 calendar converts");
    const std::string cal1904 = convert_file("tests/xlsx_files/1904_Calendar.xlsx");
    check(cal1904.find('|') != std::string::npos, "1904 calendar converts");

    for (const char* name : {"namespacestd.xlsx", "namespacepurl.xlsx", "namespacenonstd.xlsx"}) {
        const std::string markdown = convert_file(std::string("tests/xlsx_files/") + name);
        check(markdown.find('|') != std::string::npos, std::string(name) + " converts despite namespace variants");
    }

    const std::string utf16 = convert_file("tests/xlsx_files/utf16be.xlsx");
    check(utf16.find('|') != std::string::npos, "UTF-16 BE worksheet XML converts");
    const std::string utf16bom = convert_file("tests/xlsx_files/utf16be.bom.xlsx");
    check(utf16bom.find('|') != std::string::npos, "UTF-16 BE BOM worksheet XML converts");

    const std::string shared = convert_file("tests/xlsx_files/sharedformulae.xlsx");
    check(shared.find("| 1 |") != std::string::npos && shared.find("| 2 |") != std::string::npos,
          "shared formulae emit cached values");
    check(shared.find("| x1 |") != std::string::npos || shared.find("|x1|") != std::string::npos,
          "formula string results use the cached <v> text");

    const std::string windows = convert_file("tests/xlsx_files/Zip-Windows-Directory-Separator.xlsx");
    check(windows.find('|') != std::string::npos, "backslash ZIP member names resolve");

    const std::string rooted = convert_file("tests/xlsx_files/rootZipFiles.xlsx");
    check(rooted.find('#') != std::string::npos && rooted.find('|') != std::string::npos,
          "non-standard workbook part paths still convert");
}

void test_error_fixture_corpus() {
    const char* fixtures[] = {
        "truncated.xlsx",
        "utf8entity.dontuse",
    };
    for (const char* fixture : fixtures) {
        const std::string package = read_fixture(std::string("tests/xlsx_files/errors/") + fixture);
        std::string markdown = "partial output must be cleared";
        const Error error = ainiux::xlsx::to_markdown_bytes(package, {}, markdown);
        check(!error.ok() && markdown.empty(), std::string("malformed fixture fails cleanly: ") + fixture);
    }
    {
        const std::string package = read_fixture("tests/xlsx_files/errors/sec-j47r.dontuse");
        std::string markdown = "partial output must be cleared";
        const Error error = ainiux::xlsx::to_markdown_bytes(package, {}, markdown);
        check(error.ok() || markdown.empty(), "hostile package must not crash or return partial Markdown");
    }
    std::string markdown = "partial";
    const Error xls = ainiux::xlsx::to_markdown_bytes(std::string("\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1", 8), {},
                                                     markdown);
    check(!xls.ok() && xls.code == ErrorCode::UnsupportedFeature && markdown.empty(),
          "legacy BIFF .xls bytes are rejected");
}

void test_sniff_and_docx_collision() {
    check(ainiux::xlsx::looks_like_xlsx(read_fixture("tests/xlsx_files/blankcell.xlsx")),
          "XLSX packages sniff as xlsx");
    const std::string docx = read_fixture("tests/docx_files/minimal.docx");
    check(!ainiux::xlsx::looks_like_xlsx(docx), "DOCX packages do not sniff as xlsx");
    check(!ainiux::xlsx::looks_like_xlsx("PK\x03\x04generic zip"),
          "a generic ZIP signature is not enough to sniff XLSX");
}

std::size_t table_row_count(const std::string& markdown) {
    std::size_t rows = 0;
    std::size_t start = 0;
    while (start < markdown.size()) {
        const std::size_t end = markdown.find('\n', start);
        const std::string line = markdown.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.front() == '|' && line.find("---") == std::string::npos) ++rows;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return rows;
}

void test_countries_population_fixture() {
    const std::string html = read_fixture("tests/xlsx_files/countries-population.html");
    const std::string markdown = ainiux::html::convert(html, ainiux::html::OutputFormat::Markdown);
    check(markdown.find("| Location | Population |") != std::string::npos,
          "snipped Wikipedia HTML converts to a GFM population table");
    check(markdown.find("| World | 8,232,000,000 |") != std::string::npos &&
              markdown.find("| India | 1,429,404,000 |") != std::string::npos &&
              markdown.find("| China | 1,404,890,000 |") != std::string::npos &&
              markdown.find("| Sweden | 10,613,531 |") != std::string::npos,
          "HTML→Markdown keeps World, India, China, and Sweden population cells");
    check(table_row_count(markdown) >= 240,
          "HTML→Markdown keeps the full country list (header plus 240 body rows)");

    const std::string committed = read_fixture("tests/xlsx_files/countries-population.md");
    check(committed.find("| Sweden | 10,613,531 |") != std::string::npos &&
              table_row_count(committed) >= 240,
          "committed Markdown fixture matches the snipped table");

    std::string package;
    Error error = ainiux::xlsx::from_markdown(committed, {}, package);
    check(error.ok() && ainiux::xlsx::looks_like_xlsx(package) && package.size() > 8000,
          "Markdown population table writes a larger XLSX workbook");
    std::string round;
    error = ainiux::xlsx::to_markdown_bytes(package, {}, round);
    check(error.ok() && round.find("| Sweden | 10,613,531 |") != std::string::npos &&
              round.find("| World | 8,232,000,000 |") != std::string::npos &&
              table_row_count(round) >= 240,
          "XLSX round-trip keeps the country population rows");

    const std::string stored = convert_file("tests/xlsx_files/countries-population.xlsx");
    check(stored.find("| Sweden | 10,613,531 |") != std::string::npos &&
              stored.find("| United States | 341,784,857 |") != std::string::npos &&
              table_row_count(stored) >= 240,
          "committed XLSX fixture reads back the snipped Wikipedia table");
}

void test_hello_world_scripts_fixture() {
    const std::string html = read_fixture("tests/xlsx_files/hello-world.html");
    const std::string markdown = ainiux::html::convert(html, ainiux::html::OutputFormat::Markdown);
    const char* needles[] = {
        "Hello world", "Hei maailma", "Hola mundo", "Olá mundo", "你好，世界", "哈囉，世界",
        "مرحبا بالعالم", "שלום עולם", "Привет, мир", "Γεια σου κόσμε", "안녕하세요, 세계",
        "こんにちは世界", "สวัสดีชาวโลก", "नमस्ते दुनिया",
    };
    for (const char* needle : needles) {
        check(markdown.find(needle) != std::string::npos,
              std::string("HTML→Markdown keeps Hello-world sample: ") + needle);
    }

    const std::string committed = read_fixture("tests/xlsx_files/hello-world.md");
    std::string package;
    Error error = ainiux::xlsx::from_markdown(committed, {}, package);
    check(error.ok() && ainiux::xlsx::looks_like_xlsx(package),
          "Hello-world Markdown writes an XLSX workbook");
    std::string round;
    error = ainiux::xlsx::to_markdown_bytes(package, {}, round);
    check(error.ok(), error.ok() ? "Hello-world XLSX reads" : error.message);
    for (const char* needle : needles) {
        check(round.find(needle) != std::string::npos,
              std::string("XLSX round-trip keeps Hello-world sample: ") + needle);
    }

    const std::string stored = convert_file("tests/xlsx_files/hello-world.xlsx");
    check(stored.find("Hei maailma") != std::string::npos &&
              stored.find("哈囉，世界") != std::string::npos &&
              stored.find("שלום עולם") != std::string::npos &&
              stored.find("Привет, мир") != std::string::npos,
          "committed Hello-world XLSX keeps Latin, Han, Hebrew, and Cyrillic cells");
}

void test_cancellation() {
    ainiux::runtime::CancellationSource cancelled;
    cancelled.cancel();
    std::string package;
    ainiux::xlsx::WriteOptions write_options;
    write_options.cancellation = cancelled.token();
    check(ainiux::xlsx::from_markdown("| A | B |\n| --- | --- |\n| 1 | 2 |\n", write_options, package).code ==
              ErrorCode::Cancelled,
          "pre-cancelled XLSX write stops cleanly");
    ainiux::xlsx::ReadOptions read_options;
    read_options.cancellation = cancelled.token();
    std::string markdown;
    check(ainiux::xlsx::to_markdown_bytes("PK", read_options, markdown).code == ErrorCode::Cancelled,
          "pre-cancelled XLSX read stops cleanly");
}

}  // namespace

void run_all() {
    test_round_trip();
    test_multi_sheet_and_pipes();
    test_interoperability_fixtures();
    test_error_fixture_corpus();
    test_sniff_and_docx_collision();
    test_countries_population_fixture();
    test_hello_world_scripts_fixture();
    test_cancellation();
}

int run_case(const char* name) {
    const std::string selected = name == nullptr ? std::string() : name;
    if (selected == "round-trip") test_round_trip();
    else if (selected == "multi") test_multi_sheet_and_pipes();
    else if (selected == "fixtures") test_interoperability_fixtures();
    else if (selected == "errors") test_error_fixture_corpus();
    else if (selected == "sniff") test_sniff_and_docx_collision();
    else if (selected == "countries") test_countries_population_fixture();
    else if (selected == "hello") test_hello_world_scripts_fixture();
    else if (selected == "cancel") test_cancellation();
    else return 2;
    return ainiux::test::failures == 0 ? 0 : 1;
}

}  // namespace ainiux::test::xlsx
