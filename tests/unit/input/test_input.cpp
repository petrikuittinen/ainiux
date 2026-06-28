#include "input/test_input.hpp"
#include "support/test_support.hpp"
#include "input/input.hpp"
#include "json/json.hpp"
#include "provider/provider.hpp"
#include "runtime/runtime.hpp"
#include <fstream>
#include <string>
#include <vector>

namespace pkchat::test::input {

namespace {

using pkchat::test::check;
using pkchat::test::read_fixture;

void test_image_loading_and_chat_request() {
    const std::string path = "build/unit-image.PNG";
    std::string png("\x89PNG\r\n\x1a\n", 8);
    png += "abc";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(png.data(), static_cast<std::streamsize>(png.size()));
    }

    pkchat::input::FileType type;
    pkchat::Error err = pkchat::input::classify_file_type(path, type);
    check(err.ok(), "uppercase PNG input classifies before loading");
    pkchat::input::ImageData image;
    err = pkchat::input::load_image_file(path, type, 1024, image);
    check(err.ok(), "PNG image loads");
    check(image.mime_type == "image/png" && image.byte_size == png.size(), "loaded PNG metadata matches");
    check(image.base64_data == "iVBORw0KGgphYmM=", "PNG bytes use expected base64 encoding");

    pkchat::provider::RequestContext context;
    context.options.model = "vision-model";
    context.options.stream = false;
    std::vector<pkchat::provider::Message> messages = {
        {"user", "Describe this image", {{image.mime_type, image.base64_data}}},
    };
    const std::string request = pkchat::provider::serialize_chat_request(context, messages);
    pkchat::json::ParseResult parsed = pkchat::json::parse(request);
    check(parsed.error.ok(), "multimodal Chat Completions request is valid JSON");
    const pkchat::json::Value* request_messages = parsed.value.get("messages");
    const pkchat::json::Value* message = request_messages == nullptr ? nullptr : request_messages->at(0);
    const pkchat::json::Value* content = message == nullptr ? nullptr : message->get("content");
    check(content != nullptr && content->is_array() && content->array.size() == 2,
          "multimodal request uses text and image content parts");
    const pkchat::json::Value* image_url = content == nullptr ? nullptr : content->at(1);
    image_url = image_url == nullptr ? nullptr : image_url->get("image_url");
    const pkchat::json::Value* url = image_url == nullptr ? nullptr : image_url->get("url");
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
        err = pkchat::input::classify_file_type(item.path, type);
        check(err.ok(), std::string("image loader classifies ") + item.path);
        err = pkchat::input::load_image_file(item.path, type, 1024, image);
        check(err.ok() && image.mime_type == item.mime_type,
              std::string("image loader validates ") + item.mime_type);
    }

    err = pkchat::input::load_image_file(path, type, 4, image);
    check(!err.ok() && err.message.find("--max-image-bytes") != std::string::npos,
          "image loader enforces its byte limit");

    const std::string bad_path = "build/unit-bad.JPG";
    {
        std::ofstream output(bad_path, std::ios::binary | std::ios::trunc);
        output << "not a jpeg";
    }
    err = pkchat::input::classify_file_type(bad_path, type);
    check(err.ok(), "bad JPEG still classifies from its extension");
    err = pkchat::input::load_image_file(bad_path, type, 1024, image);
    check(!err.ok() && err.message.find("does not match") != std::string::npos,
          "image loader rejects mismatched content and extension");
}

void test_input_file_type_classification() {
    struct Case {
        const char* path;
        pkchat::input::Kind kind;
        const char* mime_type;
    };
    const Case cases[] = {
        {"README.MD", pkchat::input::Kind::Markdown, "text/markdown"},
        {"notes.TxT", pkchat::input::Kind::Plaintext, "text/plain"},
        {"page.HTML", pkchat::input::Kind::Html, "text/html"},
        {"image.PnG", pkchat::input::Kind::Image, "image/png"},
        {"photo.JPG", pkchat::input::Kind::Image, "image/jpeg"},
        {"photo.JpEg", pkchat::input::Kind::Image, "image/jpeg"},
        {"animation.GIF", pkchat::input::Kind::Image, "image/gif"},
        {"stdin", pkchat::input::Kind::Plaintext, "text/plain"},
    };
    for (const Case& item : cases) {
        pkchat::input::FileType type;
        const pkchat::Error err = pkchat::input::classify_file_type(item.path, type);
        check(err.ok(), std::string("input extension classifies: ") + item.path);
        check(type.kind == item.kind, std::string("input kind matches: ") + item.path);
        check(type.mime_type == item.mime_type, std::string("input MIME type matches: ") + item.path);
    }

    pkchat::input::FileType type;
    pkchat::Error err = pkchat::input::classify_file_type("picture.webp", type);
    check(!err.ok() && err.code == pkchat::ErrorCode::UnsupportedFeature,
          "WebP is rejected because common models do not support it reliably");
    err = pkchat::input::classify_file_type("video.webm", type);
    check(!err.ok() && err.code == pkchat::ErrorCode::UnsupportedFeature,
          "WebM is rejected instead of treated as an image");
    err = pkchat::input::classify_file_type("image-without-extension", type);
    check(!err.ok(), "input without a supported extension is rejected");
}

void test_input_file_io_and_unicode_edge_cases() {
    pkchat::input::FileType type;
    pkchat::Error err = pkchat::input::classify_file_type("-", type);
    check(!err.ok() && err.code == pkchat::ErrorCode::BadArgs,
          "stdin dash path is rejected because extension cannot be inferred");

    err = pkchat::input::classify_file_type("build/does-not-exist.png", type);
    check(err.ok(), "missing image path still classifies from extension");
    pkchat::input::ImageData image;
    err = pkchat::input::load_image_file("build/does-not-exist.png", type, 1024, image);
    check(!err.ok() && err.code == pkchat::ErrorCode::FileRead,
          "missing image path reports a file-read error");

    const std::string empty_png = "build/unit-empty.png";
    {
        std::ofstream output(empty_png, std::ios::binary | std::ios::trunc);
    }
    err = pkchat::input::classify_file_type(empty_png, type);
    check(err.ok(), "empty PNG still classifies from extension");
    err = pkchat::input::load_image_file(empty_png, type, 1024, image);
    check(!err.ok() && err.code == pkchat::ErrorCode::UnsupportedFeature &&
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
    pkchat::input::TextContext loaded;
    err = pkchat::input::load_text_context_file(binary_txt, 1024, loaded);
    check(!err.ok() && err.code == pkchat::ErrorCode::UnsupportedFeature &&
              err.message.find("binary") != std::string::npos,
          "text loader rejects binary NUL bytes");

    const std::string invalid_utf8_txt = "build/unit-invalid-utf8.txt";
    {
        std::ofstream output(invalid_utf8_txt, std::ios::binary | std::ios::trunc);
        output << "plain\xfftext";
    }
    err = pkchat::input::load_text_context_file(invalid_utf8_txt, 1024, loaded);
    check(!err.ok() && err.code == pkchat::ErrorCode::UnsupportedFeature &&
              err.message.find("UTF-8") != std::string::npos,
          "text loader rejects invalid UTF-8 bytes");

    const std::string unicode_md = "build/unit-arabic-chinese.md";
    const std::string unicode_body = u8"# عنوان\n\n你好 👨‍👩‍👧‍👦\n";
    {
        std::ofstream output(unicode_md, std::ios::binary | std::ios::trunc);
        output << unicode_body;
    }
    err = pkchat::input::load_text_context_file(unicode_md, 1024, loaded);
    check(err.ok() && loaded.content.find(u8"你好") != std::string::npos &&
              loaded.content.find(u8"👨‍👩‍👧‍👦") != std::string::npos,
          "text loader preserves Arabic, Chinese, and complex emoji content");
    check(pkchat::input::text_context_message(loaded).find(u8"你好") != std::string::npos,
          "text context message preserves Unicode content");

    const std::string long_txt = "build/unit-long.txt";
    const std::string long_body(2000, 'x');
    {
        std::ofstream output(long_txt, std::ios::binary | std::ios::trunc);
        output << long_body;
    }
    err = pkchat::input::load_text_context_file(long_txt, 1000, loaded);
    check(!err.ok() && err.message.find("max-input-bytes") != std::string::npos,
          "text loader enforces max byte limit on long files");
    err = pkchat::input::load_text_context_file("build/missing-context.md", 1024, loaded);
    check(!err.ok() && err.code == pkchat::ErrorCode::FileRead,
          "missing text context path reports a file-read error");
}

void test_text_context_loading_and_cancellation() {
    pkchat::input::TextContext loaded;
    pkchat::Error err = pkchat::input::load_text_context_file(
        "tests/fixtures/comprehensive.html", 1024 * 1024, loaded);
    check(err.ok(), "shared text context loader reads HTML");
    check(loaded.kind == pkchat::input::Kind::Html && loaded.content.find("# Comprehensive HTML") != std::string::npos,
          "shared text context loader converts HTML to Markdown");
    check(pkchat::input::text_context_message(loaded).find("Input context from file") != std::string::npos,
          "shared text context loader creates provider context message");

    pkchat::runtime::CancellationSource source;
    source.cancel();
    err = pkchat::input::load_text_context_file(
        "tests/fixtures/comprehensive.html", 1024 * 1024, loaded, source.token());
    check(!err.ok() && err.code == pkchat::ErrorCode::Cancelled,
          "shared text context loader observes cancellation");
}

}  // namespace

void run_all() {
    test_image_loading_and_chat_request();
    test_input_file_type_classification();
    test_input_file_io_and_unicode_edge_cases();
    test_text_context_loading_and_cancellation();
}

}  // namespace pkchat::test::input
