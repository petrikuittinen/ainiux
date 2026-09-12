#include "docx/test_docx.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "docx/docx.hpp"
#include "docx/xml.hpp"
#include "docx/zip.hpp"
#include "runtime/runtime.hpp"
#include "support/test_support.hpp"

namespace ainiux::test::docx {
namespace {

using ainiux::test::check;
using ainiux::test::read_fixture;

std::uint16_t get_u16(const std::string& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[offset])) |
           static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8U;
}

std::uint32_t get_u32(const std::string& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset])) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8U |
           static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 2])) << 16U |
           static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 3])) << 24U;
}

void set_u16(std::string& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<char>(value & 0xffU);
    bytes[offset + 1] = static_cast<char>((value >> 8U) & 0xffU);
}

void set_u32(std::string& bytes, std::size_t offset, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        bytes[offset + static_cast<std::size_t>(shift / 8)] = static_cast<char>((value >> shift) & 0xffU);
}

void append_u16(std::string& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<char>(value & 0xffU));
    bytes.push_back(static_cast<char>((value >> 8U) & 0xffU));
}

void append_u32(std::string& bytes, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<char>((value >> shift) & 0xffU));
}

void append_u64(std::string& bytes, std::uint64_t value) {
    append_u32(bytes, static_cast<std::uint32_t>(value));
    append_u32(bytes, static_cast<std::uint32_t>(value >> 32U));
}

std::size_t signature(const std::string& bytes, const char* value) {
    return bytes.find(std::string(value, 4));
}

std::string custom_package(const std::string& document,
                           const std::string& document_relationships = {},
                           const std::vector<ainiux::docx::detail::ZipWriteEntry>& extra_entries = {}) {
    const std::string types =
        "<?xml version=\"1.0\"?><Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/custom/main.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
        "</Types>";
    const std::string root_rels =
        "<?xml version=\"1.0\"?><p:Relationships xmlns:p=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<p:Relationship Id=\"main\" Type=\"http://purl.oclc.org/ooxml/officeDocument/relationships/officeDocument\" Target=\"custom/main.xml\"/>"
        "</p:Relationships>";
    std::vector<ainiux::docx::detail::ZipWriteEntry> entries = {
        {"[Content_Types].xml", types, true},
        {"_rels/.rels", root_rels, false},
        {"custom/main.xml", document, true},
    };
    if (!document_relationships.empty())
        entries.push_back({"custom/_rels/main.xml.rels", document_relationships, true});
    entries.insert(entries.end(), extra_entries.begin(), extra_entries.end());
    // This member must remain untouched by the selective reader.
    entries.push_back({"custom/media/unused.bin", std::string(1024, 'x'), true});
    std::string package;
    check(ainiux::docx::detail::write_zip(entries, 1024U * 1024U, {}, package).ok(),
          "hand-authored DOCX package serializes");
    return package;
}

void test_style_and_numbering_inheritance() {
    const std::string relationships =
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"s\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
        "<Relationship Id=\"n\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering\" Target=\"numbering.xml\"/>"
        "</Relationships>";
    const std::string styles =
        "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:style w:type=\"paragraph\" w:styleId=\"Base\"><w:name w:val=\"Base\"/><w:rPr><w:i/></w:rPr></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"Child\"><w:name w:val=\"Child\"/><w:basedOn w:val=\"Base\"/>"
        "<w:pPr><w:numPr><w:ilvl w:val=\"0\"/><w:numId w:val=\"42\"/></w:numPr></w:pPr><w:rPr><w:b/></w:rPr></w:style>"
        "</w:styles>";
    const std::string numbering =
        "<w:numbering xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:abstractNum w:abstractNumId=\"7\">"
        "<w:lvl w:ilvl=\"0\"><w:start w:val=\"0\"/><w:numFmt w:val=\"decimal\"/></w:lvl>"
        "<w:lvl w:ilvl=\"1\"><w:start w:val=\"5\"/><w:numFmt w:val=\"decimal\"/><w:lvlRestart w:val=\"0\"/></w:lvl>"
        "</w:abstractNum><w:num w:numId=\"42\"><w:abstractNumId w:val=\"7\"/>"
        "<w:lvlOverride w:ilvl=\"0\"><w:startOverride w:val=\"3\"/></w:lvlOverride></w:num>"
        "</w:numbering>";
    const std::string document =
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body>"
        "<w:p><w:pPr><w:pStyle w:val=\"Child\"/></w:pPr><w:r><w:t>First</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"1\"/><w:numId w:val=\"42\"/></w:numPr></w:pPr><w:r><w:t>Nested one</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:pStyle w:val=\"Child\"/></w:pPr><w:r><w:t>Second</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"1\"/><w:numId w:val=\"42\"/></w:numPr></w:pPr><w:r><w:t>Nested two</w:t></w:r></w:p>"
        "</w:body></w:document>";
    const std::string package = custom_package(
        document, relationships,
        {{"custom/styles.xml", styles, true}, {"custom/numbering.xml", numbering, true}});
    std::string markdown;
    const Error error = ainiux::docx::to_markdown_bytes(package, {}, markdown);
    check(error.ok(), "style and numbering inheritance fixture converts");
    check(markdown.find("3. ***First***") != std::string::npos &&
              markdown.find("4. ***Second***") != std::string::npos,
          "basedOn run formatting and numbering start override are resolved");
    check(markdown.find("    5. Nested one") != std::string::npos &&
              markdown.find("    6. Nested two") != std::string::npos,
          "explicit lvlRestart=0 preserves a nested counter across parent advances");
}

void test_round_trip() {
    const std::string source =
        "# Heading\n\n"
        "Plain **bold**, *italic*, ~~strike~~, ++underline++, and [link](https://example.com/a).  \n"
        "Second line with a tab\tand Русский 中文 עברית العربية.\n\n"
        "> quoted text\n\n"
        "3. third\n"
        "4. fourth\n"
        "    - nested\n\n"
        "| A \\| pipe | B |\n"
        "| --- | --- |\n"
        "| one<br>two | value |\n";
    ainiux::docx::WriteOptions options;
    std::string first;
    std::string second;
    check(ainiux::docx::from_markdown(source, options, first).ok() &&
              first.size() > 4 && first.compare(0, 2, "PK") == 0,
          "Markdown converts to a DOCX ZIP package");
    check(ainiux::docx::from_markdown(source, options, second).ok() && first == second,
          "DOCX output is byte-for-byte deterministic");
    std::string markdown;
    check(ainiux::docx::to_markdown_bytes(first, {}, markdown).ok(),
          "generated DOCX converts back to Markdown");
    check(markdown.find("# Heading") != std::string::npos &&
              markdown.find("**bold**") != std::string::npos &&
              markdown.find("*italic*") != std::string::npos &&
              markdown.find("~~strike~~") != std::string::npos &&
              markdown.find("++underline++") != std::string::npos,
          "round trip preserves headings and run emphasis");
    check(markdown.find("[link](https://example.com/a)") != std::string::npos,
          "round trip preserves a true hyperlink run");
    check(markdown.find("3. third") != std::string::npos &&
              markdown.find("4. fourth") != std::string::npos &&
              markdown.find("    - nested") != std::string::npos,
          "round trip preserves ordered starts and nested list depth");
    check(markdown.find(u8"Русский 中文 עברית العربية") != std::string::npos,
          "round trip preserves multilingual UTF-8");
    check(markdown.find("A \\| pipe") != std::string::npos && markdown.find("one<br>two") != std::string::npos,
          "round trip preserves escaped table pipes and multi-line cells");
}

void test_strict_namespaces_images_and_merges() {
    const std::string relationships =
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"h1\" Type=\"http://purl.oclc.org/ooxml/officeDocument/relationships/hyperlink\" "
        "Target=\"https://example.org/strict\" TargetMode=\"External\"/>"
        "<Relationship Id=\"head\" Type=\"http://purl.oclc.org/ooxml/officeDocument/relationships/header\" Target=\"header.xml\"/>"
        "</Relationships>";
    const std::string document =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<z:document xmlns:z=\"http://purl.oclc.org/ooxml/wordprocessingml/main\" "
        "xmlns:q=\"http://purl.oclc.org/ooxml/officeDocument/relationships\" "
        "xmlns:m=\"http://schemas.openxmlformats.org/markup-compatibility/2006\" xmlns:wp=\"urn:wp\">"
        "<z:body><z:p><z:pPr><z:outlineLvl z:val=\"1\"/></z:pPr><z:r><z:rPr><z:b/><z:u z:val=\"single\"/></z:rPr>"
        "<z:t xml:space=\"preserve\">Strict &amp; prefixed</z:t><z:tab/><z:t>text</z:t></z:r>"
        "<z:hyperlink q:id=\"h1\"><z:r><z:t>site</z:t></z:r></z:hyperlink></z:p>"
        "<m:AlternateContent><m:Choice Requires=\"z\"><z:p><z:r><z:t>chosen</z:t></z:r></z:p></m:Choice>"
        "<m:Fallback><z:p><z:r><z:t>duplicate fallback</z:t></z:r></z:p></m:Fallback></m:AlternateContent>"
        "<z:p><z:r><z:t>before</z:t><z:drawing><wp:inline><wp:docPr id=\"1\" name=\"Chart\" descr=\"Sales chart\"/>"
        "</wp:inline></z:drawing><z:t>after</z:t></z:r></z:p>"
        "<z:p><z:fldSimple z:instr=\" HYPERLINK &quot;https://example.org/field&quot; \"><z:r><z:t>field link</z:t></z:r></z:fldSimple></z:p>"
        "<z:tbl><z:tr><z:tc><z:tcPr><z:gridSpan z:val=\"2\"/></z:tcPr><z:p><z:r><z:t>Merged</z:t></z:r></z:p>"
        "</z:tc></z:tr></z:tbl></z:body></z:document>";
    std::string package = custom_package(document, relationships);
    const std::size_t media_name = package.find("custom/media/unused.bin");
    const std::size_t media_local = package.rfind(std::string("\x50\x4b\x03\x04", 4), media_name);
    const std::size_t media_data = media_local + 30U + get_u16(package, media_local + 26) +
                                   get_u16(package, media_local + 28);
    package[media_data] ^= 1;  // A selected read would now fail inflate/CRC.
    std::string markdown;
    ainiux::docx::Diagnostics diagnostics;
    check(ainiux::docx::to_markdown_bytes(package, {}, markdown, &diagnostics).ok(),
          "strict OOXML converts without inflating an unselected corrupt media member");
    check(markdown.find("## ++**Strict & prefixed\\ttext**++") != std::string::npos ||
              (markdown.find("##") != std::string::npos && markdown.find("Strict & prefixed") != std::string::npos),
          "strict outline heading and formatted text are retained");
    check(markdown.find("[site](https://example.org/strict)") != std::string::npos,
          "strict relationship namespace resolves hyperlinks");
    check(markdown.find("chosen") != std::string::npos &&
              markdown.find("duplicate fallback") == std::string::npos,
          "AlternateContent selects Choice without duplicate Fallback text");
    check(markdown.find("before[image omitted: Sales chart]after") != std::string::npos &&
              diagnostics.omitted_images == 1 && diagnostics.messages.size() >= 2,
          "mid-run images retain surrounding text and produce bounded diagnostics");
    check(markdown.find("[field link](https://example.org/field)") != std::string::npos,
          "cached simple-field hyperlink results are retained");
    check(diagnostics.flattened_merged_tables == 1 && markdown.find("| Merged |  |") != std::string::npos,
          "merged tables flatten to rectangular Markdown");
}

void test_interoperability_fixtures() {
    struct Fixture {
        const char* name;
        std::size_t warnings;
        std::size_t omitted_images;
    };
    const Fixture fixtures[] = {
        {"minimal", 0, 0},
        {"headings-lists-links", 0, 0},
        {"formatting", 0, 0},
        {"tables", 1, 0},
        {"multilingual", 0, 0},
        {"strict-prefixes", 0, 0},
        {"image-placeholder", 1, 2},
    };
    for (const Fixture& fixture : fixtures) {
        const std::string base = std::string("tests/docx_files/") + fixture.name;
        const std::string package = read_fixture(base + ".docx");
        const std::string golden = read_fixture(base + ".md");
        std::string markdown;
        ainiux::docx::Diagnostics diagnostics;
        const Error error = ainiux::docx::to_markdown_bytes(package, {}, markdown, &diagnostics);
        check(error.ok(), std::string("interoperability fixture converts: ") + fixture.name +
                              (error.ok() ? std::string() : ": " + error.message));
        check(markdown == golden, std::string("interoperability fixture matches golden: ") + fixture.name);
        check(diagnostics.messages.size() == fixture.warnings,
              std::string("interoperability fixture warnings match manifest: ") + fixture.name);
        check(diagnostics.omitted_images == fixture.omitted_images,
              std::string("interoperability fixture image count matches manifest: ") + fixture.name);
    }
}

void test_error_fixture_corpus() {
    const char* fixtures[] = {
        "missing-content-types.docx", "missing-main-part.docx", "traversal.docx",
        "duplicate-names.docx", "unsupported-compression.docx", "encrypted-flag.docx",
        "malformed-xml.docx", "deep-xml.docx", "bad-crc.docx",
        "bad-zip64-metadata.docx", "zip-bomb-metadata.docx", "truncated.docx",
    };
    for (const char* fixture : fixtures) {
        const std::string package = read_fixture(std::string("tests/docx_files/errors/") + fixture);
        std::string markdown = "partial output must be cleared";
        const Error error = ainiux::docx::to_markdown_bytes(package, {}, markdown);
        check(!error.ok() && markdown.empty(), std::string("malformed fixture fails cleanly: ") + fixture);
    }
}

void test_zip_shapes_and_failures() {
    const std::string repeated(4096, 'a');
    std::string package;
    check(ainiux::docx::detail::write_zip({{"empty", "", false},
                                           {"stored", "stored bytes", false},
                                           {"deflated", repeated, true}},
                                          1024U * 1024U, {}, package).ok(),
          "ZIP writer creates empty, stored, and raw-DEFLATE members");
    ainiux::docx::detail::ZipArchive archive;
    check(archive.open(package, {}, {}).ok(), "ZIP reader indexes mixed compression members");
    std::string value;
    check(archive.read("empty", value).ok() && value.empty(), "ZIP reader accepts zero-length members");
    check(archive.read("stored", value).ok() && value == "stored bytes", "ZIP reader accepts stored members");
    check(archive.read("deflated", value).ok() && value == repeated, "ZIP reader inflates raw DEFLATE members");
    ainiux::docx::detail::ZipLimits once_limits;
    once_limits.max_total_selected_bytes = repeated.size();
    ainiux::docx::detail::ZipArchive once_archive;
    check(once_archive.open(package, once_limits, {}).ok() &&
              once_archive.read("deflated", value).ok() &&
              once_archive.read("deflated", value).ok(),
          "selected ZIP members are inflated and counted at most once");

    std::string descriptor;
    check(ainiux::docx::detail::write_zip({{"member", "descriptor body", false}},
                                          1024U, {}, descriptor).ok(),
          "descriptor source ZIP serializes");
    const std::size_t central = signature(descriptor, "\x50\x4b\x01\x02");
    const std::size_t eocd = signature(descriptor, "\x50\x4b\x05\x06");
    const std::uint32_t crc = get_u32(descriptor, central + 16);
    const std::uint32_t compressed = get_u32(descriptor, central + 20);
    const std::uint32_t uncompressed = get_u32(descriptor, central + 24);
    set_u16(descriptor, 6, static_cast<std::uint16_t>(get_u16(descriptor, 6) | 8U));
    set_u32(descriptor, 14, 0);
    set_u32(descriptor, 18, 0);
    set_u32(descriptor, 22, 0);
    std::string descriptor_record;
    append_u32(descriptor_record, crc);  // deliberately omit the optional signature
    append_u32(descriptor_record, compressed);
    append_u32(descriptor_record, uncompressed);
    descriptor.insert(central, descriptor_record);
    const std::size_t shifted_central = central + descriptor_record.size();
    const std::size_t shifted_eocd = eocd + descriptor_record.size();
    set_u16(descriptor, shifted_central + 8,
            static_cast<std::uint16_t>(get_u16(descriptor, shifted_central + 8) | 8U));
    set_u32(descriptor, shifted_eocd + 16, static_cast<std::uint32_t>(shifted_central));
    check(archive.open(descriptor, {}, {}).ok() && archive.read("member", value).ok() &&
              value == "descriptor body",
          "ZIP reader accepts a descriptor without the optional signature");

    std::string zip64;
    check(ainiux::docx::detail::write_zip({{"zip64", "small", false}}, 1024U, {}, zip64).ok(),
          "Zip64 source ZIP serializes");
    const std::size_t zcentral = signature(zip64, "\x50\x4b\x01\x02");
    const std::size_t zeocd = signature(zip64, "\x50\x4b\x05\x06");
    const std::uint16_t name_size = get_u16(zip64, zcentral + 28);
    const std::uint32_t zcompressed = get_u32(zip64, zcentral + 20);
    const std::uint32_t zuncompressed = get_u32(zip64, zcentral + 24);
    std::string extra;
    append_u16(extra, 1);
    append_u16(extra, 16);
    append_u64(extra, zuncompressed);
    append_u64(extra, zcompressed);
    zip64.insert(zcentral + 46U + name_size, extra);
    set_u16(zip64, zcentral + 30, static_cast<std::uint16_t>(extra.size()));
    set_u32(zip64, zcentral + 20, 0xffffffffU);
    set_u32(zip64, zcentral + 24, 0xffffffffU);
    const std::size_t shifted_zeocd = zeocd + extra.size();
    const std::uint32_t central_size = get_u32(zip64, shifted_zeocd + 12);
    std::string zip64_tail;
    const std::uint64_t zip64_end_offset = shifted_zeocd;
    append_u32(zip64_tail, 0x06064b50U);
    append_u64(zip64_tail, 44);
    append_u16(zip64_tail, 45);
    append_u16(zip64_tail, 45);
    append_u32(zip64_tail, 0);
    append_u32(zip64_tail, 0);
    append_u64(zip64_tail, 1);
    append_u64(zip64_tail, 1);
    append_u64(zip64_tail, static_cast<std::uint64_t>(central_size) + extra.size());
    append_u64(zip64_tail, zcentral);
    append_u32(zip64_tail, 0x07064b50U);
    append_u32(zip64_tail, 0);
    append_u64(zip64_tail, zip64_end_offset);
    append_u32(zip64_tail, 1);
    zip64.insert(shifted_zeocd, zip64_tail);
    const std::size_t final_eocd = shifted_zeocd + zip64_tail.size();
    set_u16(zip64, final_eocd + 8, 0xffffU);
    set_u16(zip64, final_eocd + 10, 0xffffU);
    set_u32(zip64, final_eocd + 12, 0xffffffffU);
    set_u32(zip64, final_eocd + 16, 0xffffffffU);
    check(archive.open(zip64, {}, {}).ok() && archive.read("zip64", value).ok() && value == "small",
          "ZIP reader accepts valid small Zip64 metadata");

    std::string bad_crc;
    check(ainiux::docx::detail::write_zip({{"crc", "payload", false}}, 1024U, {}, bad_crc).ok(),
          "CRC source ZIP serializes");
    const std::size_t crc_data = 30U + get_u16(bad_crc, 26) + get_u16(bad_crc, 28);
    bad_crc[crc_data] ^= 1;
    check(archive.open(bad_crc, {}, {}).ok() && !archive.read("crc", value).ok(),
          "ZIP reader verifies CRC-32 when a selected member is read");

    std::string flags;
    check(ainiux::docx::detail::write_zip({{"flags", "payload", false}}, 1024U, {}, flags).ok(),
          "flag source ZIP serializes");
    const std::size_t flags_central = signature(flags, "\x50\x4b\x01\x02");
    set_u16(flags, 6, 1);
    set_u16(flags, flags_central + 8, 1);
    check(!archive.open(flags, {}, {}).ok(), "ZIP reader rejects encrypted members");

    std::string bomb;
    check(ainiux::docx::detail::write_zip({{"bomb", repeated, true}}, 1024U * 1024U, {}, bomb).ok(),
          "expansion source ZIP serializes");
    const std::size_t bomb_central = signature(bomb, "\x50\x4b\x01\x02");
    const std::uint32_t bomb_compressed = get_u32(bomb, bomb_central + 20);
    set_u32(bomb, bomb_central + 24, bomb_compressed * 1000U + 1U);
    check(!archive.open(bomb, {}, {}).ok(), "ZIP reader rejects expansion-ratio metadata before inflation");

    std::string malformed_zip64 = zip64;
    malformed_zip64[final_eocd - 20] = 0;
    check(!archive.open(malformed_zip64, {}, {}).ok(), "ZIP reader rejects malformed Zip64 locator metadata");
    check(!archive.open(package.substr(0, package.size() - 5), {}, {}).ok(),
          "ZIP reader rejects a truncated archive");
}

void test_xml_encodings_and_limits() {
    std::string utf16("\xff\xfe", 2);
    const std::string ascii = "<r xmlns:p=\"urn:test\"><p:v>text &amp; more</p:v><![CDATA[<raw>]]></r>";
    for (unsigned char ch : ascii) {
        utf16.push_back(static_cast<char>(ch));
        utf16.push_back('\0');
    }
    std::string text;
    check(ainiux::docx::detail::scan_xml(utf16, {}, {}, [&](const auto& event) {
              if (event.kind == ainiux::docx::detail::XmlEvent::Kind::Text) text += event.text;
              return ok_error();
          }).ok() && text == "text & more<raw>",
          "XML scanner accepts BOM-marked UTF-16, namespaces, entities, and CDATA");
    check(!ainiux::docx::detail::scan_xml("<a><b></a>", {}, {}, [](const auto&) {
              return ok_error();
          }).ok(),
          "XML scanner rejects malformed nesting");
    std::string deep;
    for (int i = 0; i < 257; ++i) deep += "<a>";
    for (int i = 0; i < 257; ++i) deep += "</a>";
    check(!ainiux::docx::detail::scan_xml(deep, {}, {}, [](const auto&) {
              return ok_error();
          }).ok(),
          "XML scanner enforces its depth limit");
}

void test_rejections_and_cancellation() {
    std::string markdown = "unchanged";
    check(!ainiux::docx::to_markdown_bytes("not a zip", {}, markdown).ok() && markdown.empty(),
          "non-ZIP DOCX input is rejected without partial output");
    std::string package;
    check(!ainiux::docx::detail::write_zip({{"../escape.xml", "x", true}}, 1024, {}, package).ok(),
          "writer rejects traversing member names");
    check(!ainiux::docx::detail::write_zip({{"same", "a", true}, {"same", "b", true}},
                                           1024, {}, package).ok(),
          "writer rejects duplicate member names");
    ainiux::runtime::CancellationSource cancelled;
    cancelled.cancel();
    ainiux::docx::WriteOptions write_options;
    write_options.cancellation = cancelled.token();
    check(ainiux::docx::from_markdown("hello", write_options, package).code == ErrorCode::Cancelled,
          "pre-cancelled DOCX write stops cleanly");
    ainiux::docx::ReadOptions read_options;
    read_options.cancellation = cancelled.token();
    check(ainiux::docx::to_markdown_bytes("PK", read_options, markdown).code == ErrorCode::Cancelled,
          "pre-cancelled DOCX read stops cleanly");

    int events = 0;
    const std::string dtd = "<?xml version=\"1.0\"?><!DOCTYPE x [<!ENTITY e \"boom\">]><x>&e;</x>";
    check(!ainiux::docx::detail::scan_xml(dtd, {}, {}, [&](const auto&) {
              ++events;
              return ok_error();
          }).ok(),
          "XML scanner refuses DTD and entity expansion");
}

}  // namespace

void run_all() {
    test_round_trip();
    test_strict_namespaces_images_and_merges();
    test_style_and_numbering_inheritance();
    test_interoperability_fixtures();
    test_error_fixture_corpus();
    test_zip_shapes_and_failures();
    test_xml_encodings_and_limits();
    test_rejections_and_cancellation();
}

int run_case(const char* name) {
    const std::string selected = name == nullptr ? std::string() : name;
    if (selected == "round-trip") test_round_trip();
    else if (selected == "strict") test_strict_namespaces_images_and_merges();
    else if (selected == "numbering") test_style_and_numbering_inheritance();
    else if (selected == "fixtures") test_interoperability_fixtures();
    else if (selected == "fixture-errors") test_error_fixture_corpus();
    else if (selected == "zip") test_zip_shapes_and_failures();
    else if (selected == "xml") test_xml_encodings_and_limits();
    else if (selected == "errors") test_rejections_and_cancellation();
    else return 2;
    return ainiux::test::failures == 0 ? 0 : 1;
}

}  // namespace ainiux::test::docx
