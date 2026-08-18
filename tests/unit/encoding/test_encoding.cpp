#include "encoding/test_encoding.hpp"

#include "encoding/encoding.hpp"
#include "html/html.hpp"
#include "support/test_support.hpp"

#include <string>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace ainiux::test::encoding {
namespace {

using ainiux::test::check;

std::string u8s(const char* text) {
    return std::string(text);
}

void test_aliases() {
    ainiux::encoding::Encoding enc = ainiux::encoding::Encoding::Unknown;
    std::string canonical;
    check(ainiux::encoding::parse_encoding_name("cp1252", enc, canonical).ok() &&
              enc == ainiux::encoding::Encoding::Windows1252 && canonical == "windows-1252",
          "cp1252 alias");
    check(ainiux::encoding::parse_encoding_name("windows-1252", enc, canonical).ok() &&
              enc == ainiux::encoding::Encoding::Windows1252,
          "windows-1252 alias");
    check(ainiux::encoding::parse_encoding_name("latin-1", enc, canonical).ok() &&
              enc == ainiux::encoding::Encoding::Iso88591 && canonical == "iso-8859-1",
          "latin-1 alias");
    check(ainiux::encoding::parse_encoding_name("iso-latin-1", enc, canonical).ok(),
          "iso-latin-1 alias");
    check(ainiux::encoding::parse_encoding_name("iso-8859-1", enc, canonical).ok(),
          "iso-8859-1 alias");
    check(ainiux::encoding::parse_encoding_name("koi8r", enc, canonical).ok() &&
              enc == ainiux::encoding::Encoding::Koi8r && canonical == "koi8-r",
          "koi8r alias");
    check(ainiux::encoding::parse_encoding_name("GBK", enc, canonical).ok() &&
              enc == ainiux::encoding::Encoding::External && canonical == "gbk",
          "GBK is an allowlisted external encoding");
    check(!ainiux::encoding::parse_encoding_name("$(reboot)", enc, canonical).ok(),
          "rejects shell-like encoding names");
    check(!ainiux::encoding::parse_encoding_name("gbk; rm", enc, canonical).ok(),
          "rejects encoding names with punctuation");
}

void test_builtin_maps() {
    std::string out;
    check(ainiux::encoding::to_utf8_named(std::string(1, '\xe4'), "iso-8859-1", out).ok() &&
              out == u8s(u8"ä"),
          "Latin-1 0xE4 is ä");
    check(ainiux::encoding::to_utf8_named(std::string(1, '\x80'), "cp1252", out).ok() &&
              out == u8s(u8"€"),
          "CP1252 0x80 is euro");
    check(ainiux::encoding::to_utf8_named(std::string(1, '\xb3'), "iso-8859-2", out).ok() &&
              out == u8s(u8"ł"),
          "Latin-2 0xB3 is ł");
    check(ainiux::encoding::to_utf8_named(std::string(1, '\xff'), "cp1251", out).ok() &&
              out == u8s(u8"я"),
          "CP1251 0xFF is я");
    check(ainiux::encoding::to_utf8_named(std::string(1, '\xc1'), "koi8-r", out).ok() &&
              out == u8s(u8"а"),
          "KOI8-R 0xC1 is а");
    check(ainiux::encoding::to_utf8_named(std::string(1, '\xa6'), "koi8-u", out).ok() &&
              out == u8s(u8"і"),
          "KOI8-U 0xA6 is Ukrainian і");
    check(ainiux::encoding::to_utf8_named(std::string("Kes") + '\xe4', "latin1", out).ok() &&
              out == u8s(u8"Kesä"),
          "Latin-1 word converts");
}

void test_utf16() {
    std::string le;
    le.push_back('\xff');
    le.push_back('\xfe');
    le.push_back('H');
    le.push_back('\0');
    le.push_back('i');
    le.push_back('\0');
    std::string out;
    check(ainiux::encoding::to_utf8_named(le, "utf-16", out).ok() && out == "Hi",
          "UTF-16 LE BOM Hello");

    std::string be;
    be.push_back('\xfe');
    be.push_back('\xff');
    be.push_back('\0');
    be.push_back('H');
    be.push_back('\0');
    be.push_back('i');
    check(ainiux::encoding::to_utf8_named(be, "utf-16be", out).ok() && out == "Hi",
          "UTF-16 BE BOM Hi");

    std::string no_bom;
    const char hello[] = {'H', 0, 'e', 0, 'l', 0, 'l', 0, 'o', 0};
    no_bom.assign(hello, hello + sizeof(hello));
    const ainiux::encoding::DetectedEncoding detected = ainiux::encoding::detect(no_bom);
    check(detected.confident && detected.encoding == ainiux::encoding::Encoding::Utf16Le,
          "UTF-16 LE without BOM is detected");
    check(ainiux::encoding::to_utf8(no_bom, detected.encoding, out).ok() && out == "Hello",
          "UTF-16 LE without BOM decodes Hello");

    check(!ainiux::encoding::to_utf8_named(std::string("abc"), "utf-16le", out).ok(),
          "odd-length UTF-16 is rejected");
}

void test_utf8_bom_and_passthrough() {
    std::string bom;
    bom.push_back('\xef');
    bom.push_back('\xbb');
    bom.push_back('\xbf');
    bom += "ok";
    const ainiux::encoding::DetectedEncoding detected = ainiux::encoding::detect(bom);
    check(detected.confident && detected.encoding == ainiux::encoding::Encoding::Utf8 &&
              detected.stripped_bom,
          "UTF-8 BOM is detected");
    std::string out;
    check(ainiux::encoding::to_utf8(bom, ainiux::encoding::Encoding::Utf8, out).ok() &&
              out == "ok",
          "UTF-8 BOM is stripped");
    check(ainiux::encoding::to_utf8_named("plain", "utf-8", out).ok() && out == "plain",
          "valid UTF-8 is unchanged");
}

void test_detect_does_not_guess_8bit() {
    const std::string cp1251_ya(1, '\xff');
    const ainiux::encoding::DetectedEncoding detected = ainiux::encoding::detect(cp1251_ya);
    check(!detected.confident, "unlabeled CP1251 is not guessed");
}

void test_web_decode() {
    const std::string latin1_html =
        "<html>\r\n<head>\r\n"
        "   <meta http-equiv=\"Content-Type\" content=\"text/html; charset=iso-8859-1\">\r\n"
        "   <title>Eino Leino - Lapin Kes\xe4</title>\r\n"
        "</head><body>Lapissa kaikki kukkii: kes\xe4</body></html>";
    std::string utf8;
    check(ainiux::encoding::decode_web_bytes(latin1_html, "text/html", utf8).ok(),
          "web decode accepts ISO-8859-1 HTML");
    check(ainiux::html::is_valid_utf8(utf8), "web decode emits UTF-8");
    check(utf8.find(u8"Lapin Kesä") != std::string::npos, "web ISO-8859-1 uses 1252 euro/latin map");

    const std::string cp1251_html =
        "<html><head><meta charset=\"windows-1251\"></head><body>\xff</body></html>";
    check(ainiux::encoding::decode_web_bytes(cp1251_html, "text/html", utf8).ok() &&
              utf8.find(u8"я") != std::string::npos,
          "declared windows-1251 uses the CP1251 map, not 1252");
}

void test_incoming_requires_encoding() {
    ainiux::encoding::DecodeOptions options;
    std::string utf8;
    ainiux::encoding::DetectedEncoding used;
    const std::string latin1 = std::string("Kes") + '\xe4';
    const ainiux::Error err =
        ainiux::encoding::decode_incoming_text(latin1, options, utf8, used);
    check(!err.ok() && err.message.find("--encoding") != std::string::npos,
          "unlabeled 8-bit text asks for --encoding");

    options.encoding_name = "koi8-r";
    check(ainiux::encoding::decode_incoming_text(std::string(1, '\xc1'), options, utf8, used).ok() &&
              utf8 == u8s(u8"а"),
          "--encoding koi8-r converts");
}

void test_picker_labels() {
    const std::vector<ainiux::encoding::EncodingChoice> choices =
        ainiux::encoding::encoding_picker_choices();
    bool saw_as_is = false;
    bool saw_1251 = false;
    for (const auto& choice : choices) {
        if (choice.name.empty()) {
            saw_as_is = true;
        }
        if (choice.name == "windows-1251") {
            saw_1251 = true;
        }
    }
    check(saw_as_is && saw_1251, "picker lists built-in encodings and Open as-is");
}

void test_optional_iconv() {
#if defined(_WIN32)
    return;
#else
    const char* path = "/usr/bin/iconv";
    if (access(path, X_OK) != 0) {
        return;
    }
    // ASCII through the GBK allowlist path still succeeds when iconv is present.
    std::string out;
    const ainiux::Error err = ainiux::encoding::to_utf8_named("ABC", "gbk", out);
    check(err.ok() && out == "ABC", "iconv GBK identity for ASCII when iconv exists");
#endif
}

bool iconv_available() {
#if defined(_WIN32)
    return true;
#else
    return access("/usr/bin/iconv", X_OK) == 0;
#endif
}

void test_texts_fixtures() {
    struct Sample {
        const char* stem;
        const char* encoding;
        bool needs_iconv;
    };
    static const Sample kSamples[] = {
        {"utf-8", "utf-8", false},
        {"utf-16", "utf-16", false},
        {"utf-16le", "utf-16le", false},
        {"utf-16be", "utf-16be", false},
        {"cp1250", "cp1250", false},
        {"cp1251", "cp1251", false},
        {"cp1252", "cp1252", false},
        {"iso-latin-1", "iso-latin-1", false},
        {"iso-latin-2", "iso-latin-2", false},
        {"koi8-r", "koi8-r", false},
        {"koi8-u", "koi8-u", false},
        {"gbk", "gbk", true},
        {"gb2312", "gb2312", true},
        {"gb18030", "gb18030", true},
        {"big5", "big5", true},
        {"shift_jis", "shift_jis", true},
        {"euc-jp", "euc-jp", true},
        {"euc-kr", "euc-kr", true},
    };
    const bool have_iconv = iconv_available();
    for (const Sample& sample : kSamples) {
        if (sample.needs_iconv && !have_iconv) {
            continue;
        }
        const std::string encoded_path = std::string("tests/texts/") + sample.stem + ".txt";
        const std::string expected_path =
            std::string("tests/texts/expected/") + sample.stem + ".txt";
        const std::string bytes = ainiux::test::read_fixture(encoded_path);
        const std::string expected = ainiux::test::read_fixture(expected_path);
        check(!bytes.empty(), std::string("fixture is non-empty: ") + encoded_path);
        std::string out;
        const ainiux::Error err = ainiux::encoding::to_utf8_named(bytes, sample.encoding, out);
        check(err.ok() && out == expected,
              std::string("tests/texts/") + sample.stem + ".txt converts with " + sample.encoding);
    }
}

}  // namespace

void run_all() {
    test_aliases();
    test_builtin_maps();
    test_utf16();
    test_utf8_bom_and_passthrough();
    test_detect_does_not_guess_8bit();
    test_web_decode();
    test_incoming_requires_encoding();
    test_picker_labels();
    test_optional_iconv();
    test_texts_fixtures();
}

}  // namespace ainiux::test::encoding
