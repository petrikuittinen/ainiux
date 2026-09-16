#include "input/test_input.hpp"
#include "support/test_support.hpp"
#include "input/input.hpp"
#include "json/json.hpp"
#include "pptx/pptx.hpp"
#include "provider/provider.hpp"
#include "runtime/runtime.hpp"
#include <fstream>
#include <string>
#include <vector>

namespace ainiux::test::input {

namespace {

using ainiux::test::check;
using ainiux::test::read_fixture;

void test_image_loading_and_chat_request() {
    const std::string path = "build/unit-image.PNG";
    std::string png("\x89PNG\r\n\x1a\n", 8);
    png += "abc";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(png.data(), static_cast<std::streamsize>(png.size()));
    }

    ainiux::input::FileType type;
    ainiux::Error err = ainiux::input::classify_file_type(path, type);
    check(err.ok(), "uppercase PNG input classifies before loading");
    ainiux::input::ImageData image;
    err = ainiux::input::load_image_file(path, type, 1024, image);
    check(err.ok(), "PNG image loads");
    check(image.mime_type == "image/png" && image.byte_size == png.size(), "loaded PNG metadata matches");
    check(image.base64_data == "iVBORw0KGgphYmM=", "PNG bytes use expected base64 encoding");

    ainiux::provider::RequestContext context;
    context.options.model = "vision-model";
    context.options.stream = false;
    std::vector<ainiux::provider::Message> messages = {
        {"user", "Describe this image", {{image.mime_type, image.base64_data}}},
    };
    const std::string request = ainiux::provider::serialize_chat_request(context, messages);
    ainiux::json::ParseResult parsed = ainiux::json::parse(request);
    check(parsed.error.ok(), "multimodal Chat Completions request is valid JSON");
    const ainiux::json::Value* request_messages = parsed.value.get("messages");
    const ainiux::json::Value* message = request_messages == nullptr ? nullptr : request_messages->at(0);
    const ainiux::json::Value* content = message == nullptr ? nullptr : message->get("content");
    check(content != nullptr && content->is_array() && content->array.size() == 2,
          "multimodal request uses text and image content parts");
    const ainiux::json::Value* image_url = content == nullptr ? nullptr : content->at(1);
    image_url = image_url == nullptr ? nullptr : image_url->get("image_url");
    const ainiux::json::Value* url = image_url == nullptr ? nullptr : image_url->get("url");
    check(url != nullptr && url->is_string() &&
              url->string == "data:image/png;base64,iVBORw0KGgphYmM=",
          "multimodal request embeds the image as a data URL");

    struct ImageCase {
        const char* path;
        std::string bytes;
        const char* mime_type;
    };
    const ImageCase image_cases[] = {
        {"build/unit-image.JPEG", std::string("\xff\xd8\xff", 3) + "jpeg", "image/jpeg"},
        {"build/unit-image.GiF", "GIF89a-data", "image/gif"},
    };
    for (const ImageCase& item : image_cases) {
        {
            std::ofstream output(item.path, std::ios::binary | std::ios::trunc);
            output.write(item.bytes.data(), static_cast<std::streamsize>(item.bytes.size()));
        }
        err = ainiux::input::classify_file_type(item.path, type);
        check(err.ok(), std::string("image loader classifies ") + item.path);
        err = ainiux::input::load_image_file(item.path, type, 1024, image);
        check(err.ok() && image.mime_type == item.mime_type,
              std::string("image loader validates ") + item.mime_type);
    }

    err = ainiux::input::load_image_file(path, type, 4, image);
    check(!err.ok() && err.message.find("--max-image-bytes") != std::string::npos,
          "image loader enforces its byte limit");

    const std::string bad_path = "build/unit-bad.JPG";
    {
        std::ofstream output(bad_path, std::ios::binary | std::ios::trunc);
        output << "not a jpeg";
    }
    err = ainiux::input::classify_file_type(bad_path, type);
    check(err.ok(), "bad JPEG still classifies from its extension");
    err = ainiux::input::load_image_file(bad_path, type, 1024, image);
    check(!err.ok() && err.message.find("does not match") != std::string::npos,
          "image loader rejects mismatched content and extension");
}

void test_extract_local_image_path_candidates() {
    check(ainiux::input::path_has_supported_image_extension("a.PNG") &&
              ainiux::input::path_has_supported_image_extension("x/y.jpg") &&
              !ainiux::input::path_has_supported_image_extension("a.webp") &&
              !ainiux::input::path_has_supported_image_extension("readme.md"),
          "supported image extensions are recognized case-insensitively");

    const std::string text =
        "What is the temperature in file tests/image_files/temperature_meter.jpg?\n"
        "Also compare \"docs/shot.PNG\" and https://example.com/remote.jpg and notes.md.";
    const std::vector<std::string> paths =
        ainiux::input::extract_local_image_path_candidates(text);
    check(paths.size() == 2, "extracts two local image paths (skips URL and non-image)");
    // Quoted paths are scanned first, then bare tokens.
    check(paths[0] == "docs/shot.PNG", "quoted PNG path is collected");
    check(paths[1] == "tests/image_files/temperature_meter.jpg",
          "bare jpg path is collected without trailing punctuation");

    const std::vector<std::string> empty =
        ainiux::input::extract_local_image_path_candidates("no images here");
    check(empty.empty(), "text without image paths yields no candidates");
}

void test_input_file_type_classification() {
    struct Case {
        const char* path;
        ainiux::input::Kind kind;
        const char* mime_type;
    };
    const Case cases[] = {
        {"README.MD", ainiux::input::Kind::Markdown, "text/markdown"},
        {"notes.TxT", ainiux::input::Kind::Plaintext, "text/plain"},
        {"page.HTML", ainiux::input::Kind::Html, "text/html"},
        {"paper.pdf", ainiux::input::Kind::Pdf, "application/pdf"},
        {"Paper.PDF", ainiux::input::Kind::Pdf, "application/pdf"},
        {"letter.docx", ainiux::input::Kind::Docx,
         "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {"Letter.DOCX", ainiux::input::Kind::Docx,
         "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {"sheet.xlsx", ainiux::input::Kind::Xlsx,
         "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        {"Sheet.XLSX", ainiux::input::Kind::Xlsx,
         "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        {"deck.pptx", ainiux::input::Kind::Pptx, ainiux::pptx::kMimeType},
        {"Deck.PPTX", ainiux::input::Kind::Pptx, ainiux::pptx::kMimeType},
        {"data.csv", ainiux::input::Kind::Csv, "text/csv"},
        {"DATA.CSV", ainiux::input::Kind::Csv, "text/csv"},
        {"payload.json", ainiux::input::Kind::Json, "application/json"},
        {"Payload.JSON", ainiux::input::Kind::Json, "application/json"},
        {"image.PnG", ainiux::input::Kind::Image, "image/png"},
        {"photo.JPG", ainiux::input::Kind::Image, "image/jpeg"},
        {"photo.JpEg", ainiux::input::Kind::Image, "image/jpeg"},
        {"animation.GIF", ainiux::input::Kind::Image, "image/gif"},
        {"stdin", ainiux::input::Kind::Plaintext, "text/plain"},
    };
    for (const Case& item : cases) {
        ainiux::input::FileType type;
        const ainiux::Error err = ainiux::input::classify_file_type(item.path, type);
        check(err.ok(), std::string("input extension classifies: ") + item.path);
        check(type.kind == item.kind, std::string("input kind matches: ") + item.path);
        check(type.mime_type == item.mime_type, std::string("input MIME type matches: ") + item.path);
    }

    ainiux::input::FileType type;
    ainiux::Error err = ainiux::input::classify_file_type("picture.webp", type);
    check(!err.ok() && err.code == ainiux::ErrorCode::UnsupportedFeature,
          "WebP is rejected because common models do not support it reliably");
    err = ainiux::input::classify_file_type("video.webm", type);
    check(!err.ok() && err.code == ainiux::ErrorCode::UnsupportedFeature,
          "WebM is rejected instead of treated as an image");
    err = ainiux::input::classify_file_type("image-without-extension", type);
    check(!err.ok(), "input without a supported extension is rejected");
    err = ainiux::input::classify_file_type("events.jsonl", type);
    check(!err.ok(), "JSONL is not treated as JSON input");
    err = ainiux::input::classify_file_type("sheet.tsv", type);
    check(!err.ok(), "TSV is not treated as CSV input");
    for (const char* unsupported : {"legacy.ppt", "macro.pptm", "show.ppsx", "template.potx"}) {
        err = ainiux::input::classify_file_type(unsupported, type);
        check(!err.ok() && err.code == ainiux::ErrorCode::UnsupportedFeature &&
                  err.message.find("unencrypted .pptx") != std::string::npos,
              std::string("unsupported PowerPoint variant is rejected explicitly: ") + unsupported);
    }
}

void test_input_file_io_and_unicode_edge_cases() {
    ainiux::input::FileType type;
    ainiux::Error err = ainiux::input::classify_file_type("-", type);
    check(!err.ok() && err.code == ainiux::ErrorCode::BadArgs,
          "stdin dash path is rejected because extension cannot be inferred");

    err = ainiux::input::classify_file_type("build/does-not-exist.png", type);
    check(err.ok(), "missing image path still classifies from extension");
    ainiux::input::ImageData image;
    err = ainiux::input::load_image_file("build/does-not-exist.png", type, 1024, image);
    check(!err.ok() && err.code == ainiux::ErrorCode::FileRead,
          "missing image path reports a file-read error");

    const std::string empty_png = "build/unit-empty.png";
    {
        std::ofstream output(empty_png, std::ios::binary | std::ios::trunc);
    }
    err = ainiux::input::classify_file_type(empty_png, type);
    check(err.ok(), "empty PNG still classifies from extension");
    err = ainiux::input::load_image_file(empty_png, type, 1024, image);
    check(!err.ok() && err.code == ainiux::ErrorCode::UnsupportedFeature &&
              err.message.find("empty") != std::string::npos,
          "empty image file is rejected");

    const std::string binary_txt = "build/unit-binary.txt";
    {
        std::string bytes = "before";
        bytes.push_back('\0');
        bytes += "after";
        std::ofstream output(binary_txt, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    ainiux::input::TextContext loaded;
    err = ainiux::input::load_text_context_file(binary_txt, 1024, loaded);
    check(!err.ok() && err.code == ainiux::ErrorCode::UnsupportedFeature &&
              err.message.find("binary") != std::string::npos,
          "text loader rejects binary NUL bytes");

    const std::string invalid_utf8_txt = "build/unit-invalid-utf8.txt";
    {
        std::ofstream output(invalid_utf8_txt, std::ios::binary | std::ios::trunc);
        output << "plain\xfftext";
    }
    err = ainiux::input::load_text_context_file(invalid_utf8_txt, 1024, loaded);
    check(!err.ok() && err.code == ainiux::ErrorCode::UnsupportedFeature &&
              err.message.find("--encoding") != std::string::npos,
          "text loader rejects unlabeled invalid UTF-8 and names --encoding");
    err = ainiux::input::load_text_context_file(invalid_utf8_txt, 1024, loaded,
                                               ainiux::runtime::CancellationToken(),
                                               "windows-1252");
    check(err.ok() && loaded.content.find(u8"ÿ") != std::string::npos,
          "text loader converts --encoding windows-1252");

    const std::string utf16_txt = "build/unit-utf16.txt";
    {
        std::ofstream output(utf16_txt, std::ios::binary | std::ios::trunc);
        const char le[] = {'H', 0, 'e', 0, 'l', 0, 'l', 0, 'o', 0};
        output.write(le, 10);
    }
    err = ainiux::input::load_text_context_file(utf16_txt, 1024, loaded);
    check(err.ok() && loaded.content == "Hello",
          "text loader auto-converts UTF-16 LE without treating NULs as binary");

    const std::string unicode_md = "build/unit-arabic-chinese.md";
    const std::string unicode_body = u8"# عنوان\n\n你好 👨‍👩‍👧‍👦\n";
    {
        std::ofstream output(unicode_md, std::ios::binary | std::ios::trunc);
        output << unicode_body;
    }
    err = ainiux::input::load_text_context_file(unicode_md, 1024, loaded);
    check(err.ok() && loaded.content.find(u8"你好") != std::string::npos &&
              loaded.content.find(u8"👨‍👩‍👧‍👦") != std::string::npos,
          "text loader preserves Arabic, Chinese, and complex emoji content");
    check(ainiux::input::text_context_message(loaded).find(u8"你好") != std::string::npos,
          "text context message preserves Unicode content");

    const std::string long_txt = "build/unit-long.txt";
    const std::string long_body(2000, 'x');
    {
        std::ofstream output(long_txt, std::ios::binary | std::ios::trunc);
        output << long_body;
    }
    err = ainiux::input::load_text_context_file(long_txt, 1000, loaded);
    check(!err.ok() && err.message.find("max-input-bytes") != std::string::npos,
          "text loader enforces max byte limit on long files");
    err = ainiux::input::load_text_context_file("build/missing-context.md", 1024, loaded);
    check(!err.ok() && err.code == ainiux::ErrorCode::FileRead,
          "missing text context path reports a file-read error");
}

void test_text_context_loading_and_cancellation() {
    ainiux::input::TextContext loaded;
    ainiux::Error err = ainiux::input::load_text_context_file(
        "tests/fixtures/comprehensive.html", 1024 * 1024, loaded);
    check(err.ok(), "shared text context loader reads HTML");
    check(loaded.kind == ainiux::input::Kind::Html && loaded.content.find("# Comprehensive HTML") != std::string::npos,
          "shared text context loader converts HTML to Markdown");
    check(ainiux::input::text_context_message(loaded).find("Input context from file") != std::string::npos,
          "shared text context loader creates provider context message");

    ainiux::runtime::CancellationSource source;
    source.cancel();
    err = ainiux::input::load_text_context_file(
        "tests/fixtures/comprehensive.html", 1024 * 1024, loaded, source.token());
    check(!err.ok() && err.code == ainiux::ErrorCode::Cancelled,
          "shared text context loader observes cancellation");
}

void test_insert_source_accepts_any_utf8_file_ending() {
    const std::string path = "build/unit-insert-source.weird-extension";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << u8"first\r\n你好\rthird\n";
    }
    ainiux::input::InsertSourceOptions options;
    options.max_file_bytes = 1024;
    ainiux::input::InsertSource inserted;
    ainiux::Error err = ainiux::input::load_insert_source(path, options, inserted);
    check(err.ok(), "/insert accepts an arbitrary file extension when its contents are UTF-8");
    check(!inserted.url && !inserted.converted_html &&
              inserted.content == u8"first\n你好\nthird\n",
          "/insert preserves UTF-8 text and normalizes CR, LF, and CRLF internally");

    options.max_file_bytes = 4;
    err = ainiux::input::load_insert_source(path, options, inserted);
    check(!err.ok() && err.message.find("max_input_bytes") != std::string::npos,
          "/insert enforces the configured local-file byte limit");

    const std::string invalid_path = "build/unit-insert-invalid.any";
    {
        std::ofstream output(invalid_path, std::ios::binary | std::ios::trunc);
        output << "valid" << static_cast<char>(0xff);
    }
    options.max_file_bytes = 1024;
    err = ainiux::input::load_insert_source(invalid_path, options, inserted);
    check(!err.ok() && err.message.find("UTF-8") != std::string::npos,
          "/insert rejects non-UTF-8 files regardless of their extension");

    check(ainiux::input::is_http_url("HTTPS://example.com/page"),
          "/insert detects HTTP URL schemes case-insensitively");
    check(!ainiux::input::is_http_url("notes/http://example.com"),
          "/insert does not mistake a local path containing a URL for a URL");
    err = ainiux::input::load_insert_source("ftp://example.com/file", options, inserted);
    check(!err.ok() && err.code == ainiux::ErrorCode::BadUrl,
          "/insert rejects unsupported URL schemes explicitly");

    ainiux::runtime::CancellationSource cancellation;
    cancellation.cancel();
    err = ainiux::input::load_insert_source(path, options, inserted, cancellation.token());
    check(!err.ok() && err.code == ainiux::ErrorCode::Cancelled,
          "/insert local-file loading observes cancellation");
}

}  // namespace

void test_local_csv_and_json_pass_through() {
    const std::string csv_path = "build/unit-native.csv";
    const std::string csv_body = "name,value\nhello,7\n";
    {
        std::ofstream output(csv_path, std::ios::binary | std::ios::trunc);
        output << csv_body;
    }
    ainiux::input::TextContext loaded;
    ainiux::Error err = ainiux::input::load_text_context_file(csv_path, 1024, loaded);
    check(err.ok() && loaded.kind == ainiux::input::Kind::Csv && loaded.content == csv_body,
          "local CSV is attached as native text, not converted to a Markdown table");
    check(ainiux::input::text_context_message(loaded).find("Format: csv") != std::string::npos,
          "CSV context is labeled csv");

    const std::string json_path = "build/unit-native.json";
    const std::string json_body = "{\"z\":1,\"a\":9007199254740993}";
    {
        std::ofstream output(json_path, std::ios::binary | std::ios::trunc);
        output << json_body;
    }
    err = ainiux::input::load_text_context_file(json_path, 1024, loaded);
    check(err.ok() && loaded.kind == ainiux::input::Kind::Json && loaded.content == json_body,
          "local JSON is attached as native text, not pretty-printed or fenced");
    check(ainiux::input::text_context_message(loaded).find("Format: json") != std::string::npos,
          "JSON context is labeled json");

    std::string attached;
    err = ainiux::input::read_local_text_file_for_attach(csv_path, 1024, attached);
    check(err.ok() && attached == csv_body, "/attach reads local CSV without converting it");
}

void run_all() {
    test_image_loading_and_chat_request();
    test_extract_local_image_path_candidates();
    test_input_file_type_classification();
    test_input_file_io_and_unicode_edge_cases();
    test_text_context_loading_and_cancellation();
    test_insert_source_accepts_any_utf8_file_ending();
    test_local_csv_and_json_pass_through();
}

}  // namespace ainiux::test::input
