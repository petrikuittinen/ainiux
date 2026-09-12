#include "pdf/test_pdf.hpp"

#include "pdf/document.hpp"
#include "pdf/limits.hpp"
#include "pdf/pdf.hpp"
#include "pdf/stream.hpp"
#include "pdf/token.hpp"
#include "pdf/value.hpp"
#include "runtime/subprocess.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/resource.h>
#endif

namespace ainiux::test::pdf {
namespace {

using ainiux::test::check;
using ainiux::test::read_fixture;

std::string runner_path;

std::string control_pdf() {
    std::string bytes = read_fixture("tests/fixtures/pdf_errors/error-unsupported-version-3.0.pdf");
    check(bytes.compare(0, 8, "%PDF-3.0") == 0, "control fixture has expected version header");
    bytes.replace(0, 8, "%PDF-1.4");
    return bytes;
}

std::string stream_object(const std::string& content, const std::string& extra = "") {
    return "<< /Length " + std::to_string(content.size()) + " " + extra +
           " >>\nstream\n" + content + "\nendstream";
}

std::vector<std::string> control_objects() {
    return {"", "<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
            "<< /Type /Page /Parent 2 0 R /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>",
            stream_object("BT /F1 12 Tf (Control text) Tj ET"),
            "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"};
}

std::string objects_pdf(const std::vector<std::string>& objects, bool compressed_length = false) {
    std::string out = "%PDF-1.7\n";
    std::vector<std::size_t> offsets(objects.size(), 0);
    for (std::size_t i = 1; i < objects.size(); ++i) {
        offsets[i] = out.size();
        out += std::to_string(i) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const auto xref = out.size();
    if (compressed_length) {
        // Object 6 is the stream length stored in object stream 7; object 8 is xref.
        std::string rows;
        for (std::size_t i = 0; i < 9; ++i) {
            rows.push_back(i == 0 ? 0 : i == 6 ? 2 : 1);
            const auto offset = i == 6 ? 7 : i == 8 ? xref : offsets[i];
            for (int shift = 24; shift >= 0; shift -= 8) rows.push_back(static_cast<char>(offset >> shift));
            rows.append(2, i == 0 ? static_cast<char>(0xff) : '\0');
        }
        out += "8 0 obj\n" + stream_object(rows, "/Type /XRef /Size 9 /Root 1 0 R /W [1 4 2]") + "\nendobj\n";
    } else {
        out += "xref\n0 " + std::to_string(objects.size()) + "\n0000000000 65535 f \n";
        for (std::size_t i = 1; i < objects.size(); ++i) {
            char line[32];
            const int size = std::snprintf(line, sizeof(line), "%010zu 00000 n \n", offsets[i]);
            check(size == 20, "generated xref entry has fixed width");
            out.append(line, 20);
        }
        out += "trailer\n<< /Size " + std::to_string(objects.size()) + " /Root 1 0 R >>\n";
    }
    return out + "startxref\n" + std::to_string(xref) + "\n%%EOF\n";
}

void expect_pdf_error(const std::string& bytes, const std::string& label) {
    std::string md;
    const auto err = ainiux::pdf::to_markdown_bytes(bytes, {}, md);
    check(err.code == ErrorCode::FileRead || err.code == ErrorCode::UnsupportedFeature ||
              (err.ok() && md.rfind("[page 1: ", 0) == 0), label + ": explicit error, not silent success");
}

ainiux::Error value_of(const std::string& text, ainiux::pdf::Value& value) {
    ainiux::pdf::Tokenizer tokens(ainiux::pdf::Cursor{
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size(), 0});
    return ainiux::pdf::read_value(tokens, value, 0);
}

void numeric_values() {
    for (const char* text : {"1e999", "-1e999", "4294967296 0 R", "1e100 0 R",
                             "1 65536 R", "1 1e100 R", "1.5 0 R", "1 0.5 R", "0 0 R", "-1 0 R",
                             "1.0 0 R", "1e0 0 R", "18446744073709551616 0 R"}) {
        ainiux::pdf::Value value;
        const ainiux::Error err = value_of(text, value);
        check(err.code == ErrorCode::FileRead, std::string("reject unrepresentable PDF value: ") + text);
    }
    ainiux::pdf::Value value;
    check(value_of("42 0 R", value).ok() && value.ref.number == 42,
          "ordinary indirect reference still parses");
    check(value_of("+00042 +0 R", value).ok() && value.ref.number == 42,
          "reference integer syntax permits plus and leading zeros");
    check(value_of("4294967295 65535 R", value).ok() && value.ref.number == UINT32_MAX &&
              value.ref.generation == UINT16_MAX, "largest representable reference still parses");
    check(value_of("-12.5", value).ok() && value.number == -12.5,
          "ordinary real number still parses");
}

void integer_dictionary_values() {
    ainiux::pdf::Value dict;
    dict.type = ainiux::pdf::ValueType::Dict;
    ainiux::pdf::Value item;
    item.type = ainiux::pdf::ValueType::Number;
    dict.dict.emplace_back("Length", item);
    for (double number : {1e100, -1e100, std::numeric_limits<double>::infinity(),
                          std::numeric_limits<double>::quiet_NaN(), 1.5, 0x1p63,
                          std::nextafter(-0x1p63, -std::numeric_limits<double>::infinity())}) {
        dict.dict[0].second.number = number;
        std::int64_t result = 123;
        check(!ainiux::pdf::dict_int(dict, "Length", result),
              "integer dictionary accessor rejects out-of-range/non-integral value " + std::to_string(number));
        check(result == 123, "failed integer conversion leaves output unchanged");
    }
    dict.dict[0].second.number = 123;
    std::int64_t result = 0;
    check(ainiux::pdf::dict_int(dict, "Length", result) && result == 123,
          "ordinary integer dictionary value still parses");
    check(ainiux::pdf::number_to_integer(-0x1p63, result) && result == INT64_MIN,
          "minimum signed integer is representable");
    check(ainiux::pdf::number_to_integer(std::nextafter(0x1p63, 0.0), result) && result > 0,
          "largest double below signed integer upper bound is representable");
}

void tokenizer_bounds() {
    const std::string inputs[] = {
        "(unterminated", "(trailing\\", "<0G>", "<1234", "/Bad#Q0", "[1 2",
        "<< /Key", std::string(ainiux::pdf::kMaxDepth + 2, '[') + "0" +
                       std::string(ainiux::pdf::kMaxDepth + 2, ']'),
        "(" + std::string(ainiux::pdf::kMaxTokenBytes + 1, 'x') + ")",
    };
    for (const std::string& text : inputs) {
        ainiux::pdf::Value value;
        check(value_of(text, value).code == ErrorCode::FileRead,
              "reject truncated, excessively nested or oversized token: " + text.substr(0, 40));
    }
    const std::uint8_t byte = 42;
    const ainiux::pdf::Cursor cursor{&byte, 1, 1};
    check(cursor.peek_at(std::numeric_limits<std::size_t>::max()) == -1,
          "lookahead cannot wrap around the end of the input");
    const std::string first = "1 2";
    const std::string second = "3";
    ainiux::pdf::Tokenizer tokens(ainiux::pdf::Cursor{
        reinterpret_cast<const std::uint8_t*>(first.data()), first.size(), 0});
    ainiux::pdf::Value value;
    check(ainiux::pdf::read_value(tokens, value, 0).ok() && value.number == 1, "read value with pending lookahead");
    tokens.reset(ainiux::pdf::Cursor{reinterpret_cast<const std::uint8_t*>(second.data()), second.size(), 0});
    check(ainiux::pdf::read_value(tokens, value, 0).ok() && value.number == 3, "object slice reset discards lookahead");
}

void decoder_bounds() {
    const std::string raw(65536, 'A');
    std::string compressed;
    check(ainiux::pdf::deflate_flate(raw, compressed).ok(), "generate small compressed expansion fixture");
    std::string out;
    auto inflate = [&](std::size_t size, std::size_t limit) {
        return ainiux::pdf::inflate_flate(reinterpret_cast<const std::uint8_t*>(compressed.data()),
                                         size, out, limit);
    };
    check(inflate(compressed.size(), raw.size()).ok() && out == raw, "valid compressed stream control");
    check(inflate(compressed.size(), 1024).code == ErrorCode::FileRead && out.size() <= 1024,
          "high-ratio Flate input cannot exceed the requested decoded limit");
    check(inflate(compressed.size() / 2, raw.size()).code == ErrorCode::FileRead,
          "truncated Flate stream is rejected");
    for (std::size_t removed = 1; removed <= 4; ++removed) {
        check(inflate(compressed.size() - removed, raw.size()).code == ErrorCode::FileRead,
              "Flate stream with missing checksum bytes is rejected");
    }
    check(inflate(0, raw.size()).code == ErrorCode::FileRead, "empty compressed input is rejected");
    check(ainiux::pdf::deflate_flate("", compressed).ok() && inflate(compressed.size(), 0).ok() && out.empty(),
          "complete Flate stream encoding empty output is valid");
    ainiux::pdf::Value dict;
    check(value_of("<< /Filter /ASCII85Decode >>", dict).ok(), "ASCII85 filter dictionary parses");
    check(ainiux::pdf::decode_stream_bytes("zzzz", dict, out, 8).code == ErrorCode::FileRead,
          "ASCII85 zero-run expansion respects the decoded limit");
    check(ainiux::pdf::decode_stream_bytes("!z", dict, out, 32).code == ErrorCode::FileRead,
          "ASCII85 zero abbreviation inside a tuple is rejected");
}

void predictor_overflow() {
    std::string data("\0AB\0CD", 6);
    check(ainiux::pdf::apply_png_predictor(data, 2, 1, 8).ok() && data == "ABCD",
          "ordinary PNG predictor rows decode");
    data.assign(4, '\0');
    const auto columns = std::numeric_limits<std::size_t>::max() / 8 + 2;
    check(ainiux::pdf::apply_png_predictor(data, columns, 1, 8).code == ErrorCode::FileRead,
          "overflowing PNG row-size multiplication is rejected");
    data.assign(2, '\0');
    check(ainiux::pdf::apply_png_predictor(data, 2, 1, 8).code == ErrorCode::FileRead,
          "partial predictor row is rejected");
    data.clear();
    check(ainiux::pdf::apply_png_predictor(data, 2, 1, 8).ok() && data.empty(),
          "zero predictor rows need no row allocation");
    ainiux::pdf::Value dict;
    check(value_of("<< /DecodeParms << /Predictor 12 /Colors 1e100 >> >>", dict).ok(), "predictor dictionary parses");
    check(ainiux::pdf::decode_stream_bytes("abc", dict, data, 1024).code == ErrorCode::FileRead,
          "unrepresentable predictor parameter rejected before narrowing cast");
}

void input_byte_limit() {
    const std::string bytes = control_pdf();
    ainiux::pdf::Options options;
    options.max_bytes = bytes.size() - 1;
    std::string md;
    check(ainiux::pdf::to_markdown_bytes(bytes, options, md).code == ErrorCode::FileRead && md.empty(),
          "in-memory PDF input enforces max_bytes before parsing");
    ainiux::pdf::Document doc;
    check(ainiux::pdf::Document::open_bytes(bytes, options, doc).code == ErrorCode::FileRead &&
              doc.page_count() == 0, "document byte entry point also enforces max_bytes");
    options.max_bytes = bytes.size();
    check(ainiux::pdf::to_markdown_bytes(bytes, options, md).ok() && md.find("Valid minimal PDF") != std::string::npos,
          "PDF at the byte limit still extracts");
}

void object_stream_bounds() {
    auto objects = control_objects();
    const std::string content = "BT /F1 12 Tf (Control text) Tj ET";
    objects[4] = "<< /Length 6 0 R >>\nstream\n" + content + "\nendstream";
    objects.resize(8);
    objects[6] = "null"; // Superseded by the compressed xref entry.
    const std::string value = std::to_string(content.size());
    // Padding tests that offsets, not sequential token reads, locate objects.
    const std::string header = "6 0 5000 " + std::to_string(value.size() + 5) + " ";
    const std::string body = header + value + "     42";
    objects[7] = stream_object(body, "/Type /ObjStm /N 2 /First " + std::to_string(header.size()));
    const auto valid = objects_pdf(objects, true);
    ainiux::pdf::Document document;
    check(ainiux::pdf::Document::open_bytes(valid, {}, document).ok(), "object stream grows stable object storage");
    const auto* extra = document.object_value(5000);
    check(extra != nullptr && extra->type == ainiux::pdf::ValueType::Number && extra->number == 42,
          "object stream uses declared offset for sparse object number");
    std::string md;
    check(ainiux::pdf::to_markdown_bytes(valid, {}, md).ok() && md == "Control text\n",
          "compressed indirect Length control extracts");
    const std::string xref_fields = "/W [1 4 2]";
    for (const char* replacement : {"/W [1e100 4 2]", "/W [-1 4 2]", "/W [1.5 4 2]",
                                    "/W [1 4 2] /Index [1e100 1]", "/W [1 4 2] /Index [0 1e100]",
                                    "/W [1 4 2] /Index [0 9 4]", "/W [1 4 2] /Index [1000000 10]"}) {
        std::string damaged = valid;
        const auto at = damaged.rfind(xref_fields);
        check(at != std::string::npos, "generated xref has expected field widths");
        if (at == std::string::npos) continue;
        damaged.replace(at, xref_fields.size(), replacement);
        expect_pdf_error(damaged, "invalid xref stream integer fields");
    }
    for (const char* parameters : {"/N 1e100 /First 10", "/N 2 /First -1", "/N 2 /First 1e100",
                                   "/N 2 /First 10000", "/N 2.5 /First 10"}) {
        objects[7] = stream_object(body, std::string("/Type /ObjStm ") + parameters);
        expect_pdf_error(objects_pdf(objects, true), "invalid object stream N/First");
    }
    for (const std::string bad_header : {"6 0 5000 0 ", "6 0 5000 1e100 ", "6 0 7 5 ", "6 -1 "}) {
        const int count = bad_header == "6 -1 " ? 1 : 2;
        objects[7] = stream_object(bad_header + value + "     42", "/Type /ObjStm /N " +
            std::to_string(count) + " /First " + std::to_string(bad_header.size()));
        expect_pdf_error(objects_pdf(objects, true), "invalid object stream entry");
    }
}

void recursive_load_recovery() {
    const auto bytes = read_fixture("tests/fixtures/pdf_errors/error-self-referencing-length.pdf");
    ainiux::pdf::Document document;
    check(ainiux::pdf::Document::open_bytes(bytes, {}, document).ok(), "cyclic content Length loads lazily");
    for (int retry = 0; retry < 3; ++retry) {
        check(document.load_object(4).code == ErrorCode::FileRead, "recursive object load safely rejects on retry");
        check(document.load_object(5).ok(), "failed recursive load does not poison unrelated objects");
    }
    auto objects = control_objects();
    // Deep acyclic Length dependencies must also be bounded.
    objects.resize(5 + ainiux::pdf::kMaxDepth + 2, "0");
    for (std::size_t i = 4; i + 1 < objects.size(); ++i) {
        if (i == 5) continue;
        const auto next = i == 4 ? 6 : i + 1;
        objects[i] = "<< /Length " + std::to_string(next) + " 0 R >>\nstream\nx\nendstream";
    }
    expect_pdf_error(objects_pdf(objects), "deep acyclic object load");
    check(ainiux::pdf::Document::open_bytes(objects_pdf(objects), {}, document).ok(), "deep dependency opens lazily");
    check(document.load_object(4).message.find("nested PDF object") != std::string::npos,
          "deep acyclic dependencies hit the nesting guard");
    const auto repeated = read_fixture("tests/fixtures/pdf_errors/error-repeated-page-tree.pdf");
    expect_pdf_error(repeated, "repeated page tree without max_pages is bounded too");
    ainiux::runtime::CancellationSource source;
    ainiux::pdf::Options options;
    options.cancellation = source.token();
    check(ainiux::pdf::Document::open_bytes(control_pdf(), options, document).ok(), "open before lazy cancellation");
    source.cancel();
    check(document.load_object(4).code == ErrorCode::Cancelled, "lazy object load retains cancellation token");
}

void font_and_geometry_bounds() {
    for (const char* entry : {"/FirstChar 1e100", "/FirstChar -1", "/LastChar 1e100",
                              "/FirstChar 255 /Widths [500 500]", "/Encoding << /Differences [1e100 /A] >>"}) {
        auto objects = control_objects();
        objects[5] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica " + std::string(entry) + " >>";
        expect_pdf_error(objects_pdf(objects), "invalid simple font integer");
    }
    auto objects = control_objects();
    objects[5] = "<< /Type /Font /Subtype /Type0 /Encoding /Identity-H /DescendantFonts [6 0 R] >>";
    objects.push_back("<< /Type /Font /Subtype /CIDFontType2 /W [65535 65535 500] >>");
    std::string md;
    check(ainiux::pdf::to_markdown_bytes(objects_pdf(objects), {}, md).ok() && md.rfind("[page 1: ", 0) != 0,
          "largest valid CID width endpoint terminates normally");
    for (const char* range : {"-1 5 500", "1.5 5 500", "0 65536 500", "65535 [500 500]", "5 4 500"}) {
        objects[6] = "<< /Type /Font /Subtype /CIDFontType2 /W [" + std::string(range) + "] >>";
        expect_pdf_error(objects_pdf(objects), "invalid CID width endpoints");
    }
    std::string repeated;
    for (int i = 0; i < 17; ++i) repeated += "0 65535 500 ";
    objects[6] = "<< /Type /Font /Subtype /CIDFontType2 /W [" + repeated + "] >>";
    expect_pdf_error(objects_pdf(objects), "overlapping CID ranges exhaust the total work budget");
    for (const char* content : {"BT /F1 1e999 Tf (A) Tj ET", "BT /F1 12 Tf 1e308 0 0 1e308 0 0 Tm (AA) Tj ET"}) {
        objects = control_objects();
        objects[4] = stream_object(content);
        expect_pdf_error(objects_pdf(objects), "non-finite text geometry");
    }
    objects = control_objects();
    objects[4] = stream_object("BT /F1 12 Tf 1 0 0 1 10 100 Tm (C) Tj "
                               "1 0 0 1 17 101.5 Tm (B) Tj 1 0 0 1 24 103 Tm (A) Tj ET");
    check(ainiux::pdf::to_markdown_bytes(objects_pdf(objects), {}, md).ok() && md == "BAC\n",
          "overlapping baseline tolerances have deterministic strict ordering");
}

void truncations_and_mutations() {
    const std::string bytes = control_pdf();
    auto probe = [](const std::string& data) {
        std::string md;
        const auto err = ainiux::pdf::to_markdown_bytes(data, ainiux::pdf::Options{}, md);
        check(err.ok() || err.code == ErrorCode::FileRead || err.code == ErrorCode::UnsupportedFeature,
              "damaged tiny PDF has a supported result/error");
        check(md.size() < 4096, "damaged tiny PDF does not expand into unbounded output");
    };
    // Fixed deterministic samples; damage may legitimately be recovered by xref repair.
    for (std::size_t n = 0; n < bytes.size(); n += 11) probe(bytes.substr(0, n));
    for (std::size_t n = 0; n < bytes.size(); n += 17) {
        for (const char ch : {'\0', static_cast<char>(0xff), '[', '('}) {
            std::string changed = bytes;
            changed[n] = ch;
            probe(changed);
        }
    }
    std::string md;
    check(ainiux::pdf::to_markdown_bytes(bytes, ainiux::pdf::Options{}, md).ok() &&
              md.find("Valid minimal PDF") != std::string::npos,
          "valid document still extracts after failed/repaired parses");
}

struct Case {
    const char* name;
    // Non-null for a standalone PDF fixture. Empty message means any explicit
    // file/page error is acceptable; a scanned-page placeholder is not an error.
    const char* message;
    void (*test)();
};

const Case cases[] = {
    {"error-self-referencing-length", "", nullptr},
    {"error-cyclic-stream-lengths", "", nullptr},
    {"error-stream-length-past-eof", "Invalid stream Length", nullptr},
    {"error-indirect-length-overflow", "", nullptr},
    {"error-page-tree-cycle", "Recursive or repeated PDF page tree", nullptr},
    {"error-cid-width-range-overflow", "", nullptr},
    {"error-unterminated-cmap-array", "unterminated ToUnicode", nullptr},
    {"error-oversized-cmap-range", "Too many ToUnicode mappings", nullptr},
    {"error-corrupt-flate-stream", "unable to decompress", nullptr},
    {"error-unsupported-content-filter", "unsupported stream filter", nullptr},
    {"error-unterminated-content-string", "unterminated PDF string", nullptr},
    {"error-invalid-content-hex", "invalid hex string", nullptr},
    {"error-unterminated-pages-array", "Unable to read value", nullptr},
    {"error-repeated-page-tree", "", nullptr},
    {"numeric-values", nullptr, numeric_values},
    {"integer-dictionary-values", nullptr, integer_dictionary_values},
    {"tokenizer-bounds", nullptr, tokenizer_bounds},
    {"decoder-bounds", nullptr, decoder_bounds},
    {"predictor-overflow", nullptr, predictor_overflow},
    {"input-byte-limit", nullptr, input_byte_limit},
    {"object-stream-bounds", nullptr, object_stream_bounds},
    {"recursive-load-recovery", nullptr, recursive_load_recovery},
    {"font-and-geometry-bounds", nullptr, font_and_geometry_bounds},
    {"truncations-and-mutations", nullptr, truncations_and_mutations},
};

void run_fixture(const Case& item) {
    const std::string path = std::string("tests/fixtures/pdf_errors/") + item.name + ".pdf";
    const std::string bytes = read_fixture(path);
    check(!bytes.empty() && bytes.size() < 4096, path + " stays a small fixture");
    ainiux::pdf::Options options;
    const bool repeated_tree = std::string(item.name) == "error-repeated-page-tree";
    if (repeated_tree) options.max_pages = 1;
    std::string md;
    const auto err = ainiux::pdf::to_markdown_bytes(bytes, options, md);
    if (repeated_tree && err.ok()) {
        check(md == "Parser control text\n", "max_pages=1 bounds repeated page-tree traversal");
        return;
    }
    const bool file_error = err.code == ErrorCode::FileRead || err.code == ErrorCode::UnsupportedFeature;
    const bool page_error = err.ok() && md.rfind("[page 1: ", 0) == 0;
    check(file_error || page_error, path + " must report a file/page error, not silently succeed");
    check((file_error ? err.message : md).find(item.message) != std::string::npos,
          path + " reports expected diagnostic: " + err.message + md);
    check(!file_error || md.empty(), path + " leaves no Markdown on file-open failure");
    check(md.size() < 4096, path + " keeps diagnostics bounded");
    // Reuse the output buffer after a rejection to catch sticky failure state.
    check(ainiux::pdf::to_markdown_bytes(control_pdf(), options, md).ok() &&
              md.find("Valid minimal PDF") != std::string::npos,
          "valid PDF extracts after " + path);
}

void configure_child() {
#if defined(_WIN32)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#else
    struct rlimit limit{};
    if (getrlimit(RLIMIT_CORE, &limit) == 0) {
        limit.rlim_cur = 0;
        check(setrlimit(RLIMIT_CORE, &limit) == 0, "disable core dumps for hostile-PDF child");
    }
#endif
}

}  // namespace

void set_runner_path(const char* path) {
    runner_path = std::filesystem::absolute(std::filesystem::u8path(path)).u8string();
}

int run_adversarial_case(const char* name) {
    configure_child();
    for (const auto& item : cases) {
        if (std::string(name) != item.name) continue;
        try {
            if (item.test) item.test();
            else run_fixture(item);
        } catch (const std::exception& err) {
            check(false, std::string(name) + " threw an exception: " + err.what());
        }
        return ainiux::test::failures ? 1 : 0;
    }
    std::cerr << "Unknown PDF case: " << name << '\n';
    return 2;
}

void run_adversarial() {
    check(!runner_path.empty(), "PDF child runner path is initialized");
    if (runner_path.empty()) return;
    std::size_t passed = 0;
    long scale = 1;
    if (const char* value = std::getenv("AINIUX_TEST_TIME_SCALE")) {
        scale = std::clamp(std::strtol(value, nullptr, 10), 1L, 10L);
    }
    for (const auto& item : cases) {
        ainiux::runtime::SubprocessOptions options;
        options.executable = runner_path;
        options.arguments = {"--pdf-case", item.name};
        options.timeout_ms = 1500 * scale;
        options.stdout_limit = options.stderr_limit = 8192;
        // Preserve only runtime lookup needed by native Windows or instrumented
        // test builds. Provider credentials are never copied into children.
        for (const char* key : {"PATH", "SystemRoot", "WINDIR", "LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH",
                                "ASAN_OPTIONS", "UBSAN_OPTIONS"}) {
            if (const char* value = std::getenv(key)) options.environment.push_back(std::string(key) + "=" + value);
        }
        ainiux::runtime::SubprocessResult result;
        const auto err = ainiux::runtime::run_subprocess(options, result);
        const bool ok = err.ok() && result.termination == ainiux::runtime::SubprocessTerminationReason::Exited &&
                        result.exit_code == 0;
        std::string detail;
        if (result.termination == ainiux::runtime::SubprocessTerminationReason::TimedOut) {
            detail = "timeout after " + std::to_string(options.timeout_ms) + " ms";
        } else if (result.signal) {
            detail = "signal " + std::to_string(result.signal);
        } else {
            detail = "exit " + std::to_string(result.exit_code);
        }
        check(ok, std::string("PDF adversarial ") + item.name + ": " + detail + " " + err.message +
                      "\n" + result.stderr_text);
        if (ok) ++passed;
        std::cout << (ok ? "PASS " : "FAIL ") << item.name << " (" << result.duration_ms << " ms)\n";
    }
    std::cout << "PDF adversarial cases: " << passed << '/' << (sizeof(cases) / sizeof(cases[0])) << " passed\n";
}

}  // namespace ainiux::test::pdf
