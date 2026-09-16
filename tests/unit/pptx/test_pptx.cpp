#include "pptx/test_pptx.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "ooxml/zip.hpp"
#include "pptx/pptx.hpp"
#include "runtime/runtime.hpp"
#include "security/hash.hpp"
#include "support/test_support.hpp"

namespace ainiux::test::pptx {
namespace {

using ainiux::test::check;
using ainiux::test::read_fixture;

std::size_t slide_count(const std::string& markdown) {
    std::size_t count = 1;
    std::size_t start = 0;
    while (start <= markdown.size()) {
        const std::size_t newline = markdown.find('\n', start);
        const std::size_t end = newline == std::string::npos ? markdown.size() : newline;
        if (markdown.substr(start, end - start) == "---") ++count;
        if (newline == std::string::npos) break;
        start = newline + 1;
    }
    return count;
}

std::string tiny_png() {
    std::string bytes("\x89PNG\r\n\x1a\n", 8);
    bytes += std::string("\0\0\0\rIHDR", 8);
    bytes += std::string("\0\0\0\x02\0\0\0\x01", 8);
    bytes += std::string("\x08\x06\0\0\0", 5);
    bytes += "ainiux-pptx-test-image";
    return bytes;
}

void test_round_trip() {
    const std::string image = tiny_png();
    const std::string digest = security::sha256_hex(image);
    ainiux::pptx::MediaAsset media;
    media.markdown_target = "ainiux-media:" + digest + ".png";
    media.content_type = "image/png";
    media.extension = "png";
    media.sha256 = digest;
    media.bytes = image;
    const std::string source =
        "<!-- ainiux-pptx width=9144000 height=6858000 -->\n\n"
        "# 多言語 العربية עברית Русский 👩🏽‍💻\n\n"
        "Plain **bold**, *italic*, ~~strike~~, ++underline++, and "
        "[link](https://example.com/deck).  \nHard break.\n\n"
        "3. ordered\n  - nested\n\n"
        "| Name | Value |\n| --- | --- |\n| café | 7 |\n\n"
        "![pixel](ainiux-media:" + digest + ".png)\n\n"
        "![pixel copy](ainiux-media:" + digest + ".png)\n\n"
        "> **Speaker notes:**\n>\n> Presenter note.\n\n"
        "---\n\n# 第二張\n\n```text\n---\n```\n";
    ainiux::pptx::WriteOptions write;
    write.collected_media.push_back(media);
    std::string first;
    std::string second;
    ainiux::pptx::Diagnostics write_diagnostics;
    check(ainiux::pptx::from_markdown(source, write, first, &write_diagnostics).ok() &&
              first.size() > 4 && first.compare(0, 2, "PK") == 0,
          "Markdown converts to a PPTX package");
    check(ainiux::pptx::from_markdown(source, write, second).ok() && first == second,
          "PPTX writer is byte-for-byte deterministic");

    ainiux::pptx::ReadOptions read;
    read.collect_media = true;
    std::string markdown;
    std::vector<ainiux::pptx::MediaAsset> extracted;
    ainiux::pptx::Diagnostics diagnostics;
    const Error error = ainiux::pptx::to_markdown_bytes(first, read, markdown, &diagnostics, &extracted);
    check(error.ok(), "generated PPTX converts back to Markdown: " + error.message);
    check(slide_count(markdown) == 2 &&
              markdown.find(u8"多言語 العربية עברית Русский 👩🏽‍💻") != std::string::npos &&
              markdown.find("# 第二張") != std::string::npos,
          "round trip preserves slide count, titles, and multilingual UTF-8");
    check(markdown.find("**bold**") != std::string::npos &&
              markdown.find("*italic*") != std::string::npos &&
              markdown.find("~~strike~~") != std::string::npos &&
              markdown.find("++underline++") != std::string::npos &&
              markdown.find("[link](https://example.com/deck)") != std::string::npos,
          "round trip preserves formatted runs and hyperlinks");
    check(markdown.find("| **Name** | **Value** |") != std::string::npos &&
              markdown.find("> **Speaker notes:**") != std::string::npos &&
              markdown.find("Presenter note.") != std::string::npos,
          "round trip preserves tables and speaker notes");
    check(extracted.size() == 1 && extracted[0].bytes == image &&
              extracted[0].sha256 == digest && diagnostics.collected_images == 1,
          "round trip deduplicates repeated images and extracts their bytes exactly");

    ooxml::ZipArchive archive;
    check(archive.open(first, {}, {}).ok() && archive.contains("ppt/presentation.xml") &&
              archive.contains("ppt/slides/slide1.xml") &&
              archive.contains("ppt/slides/slide2.xml") &&
              archive.contains("ppt/media/image1.png") &&
              !archive.contains("ppt/media/image2.png"),
          "generated package has presentation, ordered slides, and one deduplicated media part");

    ainiux::pptx::WriteOptions rewrite;
    rewrite.collected_media = extracted;
    std::string rewritten;
    std::string fixed_point;
    std::vector<ainiux::pptx::MediaAsset> fixed_media;
    check(ainiux::pptx::from_markdown(markdown, rewrite, rewritten).ok() &&
              ainiux::pptx::to_markdown_bytes(
                  rewritten, read, fixed_point, nullptr, &fixed_media).ok() &&
              fixed_point == markdown,
          "canonical Markdown is a fixed point across PPTX round trips");
}

void test_slide_boundaries() {
    std::string bytes;
    check(ainiux::pptx::from_markdown("", {}, bytes).ok(),
          "empty Markdown produces a blank PPTX slide");
    std::string markdown;
    check(ainiux::pptx::to_markdown_bytes(bytes, {}, markdown).ok() && slide_count(markdown) == 1,
          "empty Markdown round trip has one slide");
    check(ainiux::pptx::from_markdown("---\n---", {}, bytes).ok() &&
              ainiux::pptx::to_markdown_bytes(bytes, {}, markdown).ok() && slide_count(markdown) == 3,
          "blank segments around top-level delimiters remain blank slides");
    check(ainiux::pptx::from_markdown("# One\n\n```\n---\n```\n", {}, bytes).ok() &&
              ainiux::pptx::to_markdown_bytes(bytes, {}, markdown).ok() && slide_count(markdown) == 1,
          "slide delimiters inside fenced code do not split slides");
}

void test_generated_fixtures() {
    const std::string package = read_fixture("tests/pptx_files/features.pptx");
    ainiux::pptx::ReadOptions read;
    read.collect_media = true;
    std::string markdown;
    std::vector<ainiux::pptx::MediaAsset> media;
    ainiux::pptx::Diagnostics diagnostics;
    Error error = ainiux::pptx::to_markdown_bytes(package, read, markdown, &diagnostics, &media);
    check(error.ok() && slide_count(markdown) == 2 &&
              markdown.find("# Second slide") < markdown.find(u8"# 简体中文") &&
              markdown.find(u8"العربية עברית Русский") != std::string::npos,
          "stdlib feature fixture follows presentation relationship order and preserves multilingual text");
    check(markdown.find("**Bold**") != std::string::npos &&
              markdown.find("https://example.com/pptx") != std::string::npos &&
              markdown.find("3. nested ordered") != std::string::npos &&
              markdown.find("| café |  |") != std::string::npos &&
              markdown.find("Remember the demonstration") != std::string::npos,
          "stdlib feature fixture preserves runs, lists, hyperlinks, merged tables, and notes");
    std::vector<std::string> extensions;
    for (const ainiux::pptx::MediaAsset& asset : media) extensions.push_back(asset.extension);
    std::sort(extensions.begin(), extensions.end());
    check(media.size() == 3 &&
              extensions == std::vector<std::string>({"gif", "jpg", "png"}) &&
              markdown.find("![fixture PNG](ainiux-media:") != std::string::npos &&
              markdown.find("![fixture JPEG](ainiux-media:") != std::string::npos &&
              markdown.find("![fixture GIF](ainiux-media:") != std::string::npos &&
              diagnostics.flattened_merged_tables == 1,
          "stdlib feature fixture extracts PNG, JPEG, and GIF media and diagnoses a flattened merge");

    const char* rejected[] = {
        "truncated.pptx", "missing-root.pptx", "missing-main-relationship.pptx",
        "malformed-xml.pptx", "deep-xml.pptx",
        "unsafe-traversal.pptx", "duplicate-members.pptx", "bad-crc.pptx",
        "encrypted-flag.pptx", "unsupported-compression.pptx",
        "expansion-bomb-metadata.pptx",
    };
    for (const char* name : rejected) {
        markdown = "partial";
        error = ainiux::pptx::to_markdown_bytes(
            read_fixture(std::string("tests/pptx_files/") + name), {}, markdown);
        check(!error.ok() && markdown.empty(),
              std::string("malformed fixture fails without partial output: ") + name);
    }

    diagnostics = {};
    markdown.clear();
    error = ainiux::pptx::to_markdown_bytes(
        read_fixture("tests/pptx_files/missing-image-target.pptx"), {}, markdown, &diagnostics);
    check(error.ok() && markdown.find("[image omitted: fixture PNG]") != std::string::npos &&
              diagnostics.missing_relationships == 1,
          "an isolated missing image target becomes a warning and visible placeholder");

    diagnostics = {};
    markdown.clear();
    error = ainiux::pptx::to_markdown_bytes(
        read_fixture("tests/pptx_files/missing-slide-target.pptx"), {}, markdown, &diagnostics);
    check(error.ok() && markdown.find("[missing PPTX slide part]") != std::string::npos &&
              diagnostics.missing_relationships == 1,
          "an isolated missing slide target becomes a warning and visible placeholder");
}

void test_large_decks() {
    struct Deck {
        const char* path;
        std::size_t slides;
        const char* representative;
    };
    const Deck decks[] = {
        {"tests/pptx_files/IntroductiontoPython3v2.pptx", 143, "Python"},
        {"tests/pptx_files/JavaScriptBasics.pptx", 220, "JavaScript"},
        {"tests/pptx_files/LinuxAdministrationAndUsage.pptx", 124, "Linux"},
    };
    for (const Deck& deck : decks) {
        const std::string package = read_fixture(deck.path);
        std::string markdown;
        ainiux::pptx::Diagnostics diagnostics;
        const Error error = ainiux::pptx::to_markdown_bytes(package, {}, markdown, &diagnostics);
        check(error.ok(), std::string("large PPTX converts: ") + deck.path + ": " + error.message);
        check(error.ok() && slide_count(markdown) == deck.slides,
              std::string("large PPTX preserves slide order/count: ") + deck.path);
        check(error.ok() && markdown.find(deck.representative) != std::string::npos,
              std::string("large PPTX retains representative text: ") + deck.path);
        check(diagnostics.messages.size() <= 32,
              std::string("large PPTX diagnostics stay bounded: ") + deck.path);
    }
}

void test_failures_and_cancellation() {
    std::string markdown = "partial";
    check(!ainiux::pptx::to_markdown_bytes("not a zip", {}, markdown).ok() && markdown.empty(),
          "non-ZIP PPTX fails without partial output");

    std::string valid;
    check(ainiux::pptx::from_markdown("# title\n", {}, valid).ok(),
          "failure source PPTX serializes");
    markdown = "partial";
    check(!ainiux::pptx::to_markdown_bytes(valid.substr(0, valid.size() - 8), {}, markdown).ok() &&
              markdown.empty(),
          "truncated PPTX fails without partial output");

    std::string missing_root;
    check(ooxml::write_zip({{"[Content_Types].xml", "<Types/>", true},
                             {"ppt/presentation.xml", "<p:presentation/>", true}},
                            4096, {}, missing_root).ok() &&
              !ainiux::pptx::to_markdown_bytes(missing_root, {}, markdown).ok(),
          "PPTX without package relationships is rejected");

    ainiux::runtime::CancellationSource cancelled;
    cancelled.cancel();
    ainiux::pptx::ReadOptions read;
    read.cancellation = cancelled.token();
    markdown = "partial";
    check(ainiux::pptx::to_markdown_bytes(valid, read, markdown).code == ErrorCode::Cancelled &&
              markdown.empty(),
          "pre-cancelled PPTX read stops cleanly");
    ainiux::pptx::WriteOptions write;
    write.cancellation = cancelled.token();
    check(ainiux::pptx::from_markdown("# title", write, valid).code == ErrorCode::Cancelled,
          "pre-cancelled PPTX write stops cleanly");

    check(!ooxml::write_zip({{"../escape.xml", "x", true}}, 4096, {}, valid).ok(),
          "shared OOXML writer rejects traversal member names");
    check(!ooxml::write_zip({{"same", "a", true}, {"same", "b", true}}, 4096, {}, valid).ok(),
          "shared OOXML writer rejects duplicate member names");
}

}  // namespace

void run_all() {
    test_round_trip();
    test_slide_boundaries();
    test_generated_fixtures();
    test_large_decks();
    test_failures_and_cancellation();
}

int run_case(const char* name) {
    const std::string selected = name == nullptr ? std::string() : name;
    if (selected == "round-trip") test_round_trip();
    else if (selected == "boundaries") test_slide_boundaries();
    else if (selected == "fixtures") test_generated_fixtures();
    else if (selected == "large") test_large_decks();
    else if (selected == "errors") test_failures_and_cancellation();
    else return 2;
    return ainiux::test::failures == 0 ? 0 : 1;
}

}  // namespace ainiux::test::pptx
