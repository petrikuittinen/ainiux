#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "chat/session.hpp"
#include "cli/args.hpp"
#include "context/context.hpp"
#include "editor/editor.hpp"
#include "editor/path_completion.hpp"
#include "fetch/fetch.hpp"
#include "html/html.hpp"
#include "http/http.hpp"
#include "input/input.hpp"
#include "json/json.hpp"
#include "markdown/markdown.hpp"
#include "output/thinking.hpp"
#include "provider/provider.hpp"
#include "runtime/runtime.hpp"
#include "tui/tui.hpp"

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

std::string read_fixture(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    check(input.is_open(), "fixture opens: " + path);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void test_cli_parse() {
    const char* argv[] = {"pkchat", "http://localhost:8000", "-p", "hello", "--no-stream", "--format", "json", "-v", "--save-chat", "chat.json"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(10, const_cast<char**>(argv));
    check(parsed.error.ok(), "CLI parse should succeed");
    check(parsed.options.positional_url == "http://localhost:8000", "positional URL parsed");
    check(parsed.options.prompt == "hello", "prompt parsed");
    check(!parsed.options.stream, "no-stream parsed");
    check(parsed.options.format == pkchat::cli::OutputFormat::Json, "json format parsed");
    check(parsed.options.verbose, "verbose parsed");
    check(parsed.options.save_chat_path == "chat.json", "save chat parsed");
}

void test_thinking_trace_splitter() {
    pkchat::output::ThinkingChunk split = pkchat::output::split_thinking_traces(
        "<think>internal trace</think>\n\nVisible answer");
    check(split.visible == "Visible answer", "thinking splitter keeps only visible response content");
    check(split.trace == "<think>internal trace</think>", "thinking splitter extracts trace with tags");

    pkchat::output::ThinkingTraceSplitter streaming;
    pkchat::output::ThinkingChunk first = streaming.feed("<thi");
    pkchat::output::ThinkingChunk second = streaming.feed("nk>split trace</TH");
    pkchat::output::ThinkingChunk third = streaming.feed("INK>\r\nanswer");
    pkchat::output::ThinkingChunk final = streaming.finish();
    check(first.visible.empty() && second.visible.empty(), "partial thinking tag never leaks as visible output");
    check(first.trace.empty(), "partial thinking tag waits for classification");
    check(second.trace == "<think>split trace", "streaming splitter extracts reasoning across chunks");
    check(third.trace == "</THINK>", "streaming splitter preserves closing trace tag");
    check(third.visible + final.visible == "answer", "streaming splitter removes trace separator newlines");

    split = pkchat::output::split_thinking_traces("Before <think>hidden</think> after");
    check(split.visible == "Before  after", "thinking splitter preserves visible text around trace");
    check(split.trace == "<think>hidden</think>", "thinking splitter extracts embedded trace");

    split = pkchat::output::split_thinking_traces("<think>unfinished");
    check(split.visible.empty(), "unfinished thinking trace does not leak into visible output");
    check(split.trace == "<think>unfinished", "unfinished thinking trace is sent to trace output");
}

void test_cli_provider_shortcut_parse() {
    const char* argv[] = {"pkchat", "openrouter", "-model", "provider/model", "-i"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(5, const_cast<char**>(argv));
    check(parsed.error.ok(), "provider shortcut args parse");
    check(parsed.options.positional_url == "openrouter", "provider shortcut stored as positional");
    check(parsed.options.model == "provider/model", "-model alias parsed");
    check(parsed.options.repl, "-i parsed for provider shortcut");
}

void test_cli_repl_parse() {
    const char* argv[] = {"pkchat", "--repl", "--load-chat", "chat.json"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(4, const_cast<char**>(argv));
    check(parsed.error.ok(), "REPL args parse");
    check(parsed.options.repl, "REPL flag parsed");
    check(parsed.options.load_chat_path == "chat.json", "load chat parsed");
}

void test_cli_chat_parse() {
    const char* argv[] = {"pkchat", "--chat", "lmstudio"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(3, const_cast<char**>(argv));
    check(parsed.error.ok(), "chat UI args parse");
    check(parsed.options.tui, "chat UI flag parsed");
    check(parsed.options.positional_url == "lmstudio", "chat UI positional profile parsed");

    const char* alias_argv[] = {"pkchat", "--tui", "lmstudio"};
    parsed = pkchat::cli::parse_args(3, const_cast<char**>(alias_argv));
    check(parsed.error.ok(), "legacy TUI alias args parse");
    check(parsed.options.tui, "legacy TUI alias flag parsed");
}

void test_cli_chat_nocolors_parse() {
    const char* argv[] = {"pkchat", "--chat", "--nocolors", "lmstudio"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(4, const_cast<char**>(argv));
    check(parsed.error.ok(), "chat UI nocolors args parse");
    check(parsed.options.tui, "chat UI flag parsed with nocolors");
    check(parsed.options.no_colors, "nocolors flag parsed");
    check(parsed.options.positional_url == "lmstudio", "chat UI nocolors positional profile parsed");
}

void test_cli_editor_parse() {
    const char* argv[] = {"pkchat", "--editor", "notes.txt", "--output", "saved.txt"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(5, const_cast<char**>(argv));
    check(parsed.error.ok(), "editor args parse");
    check(parsed.options.editor, "editor flag parsed");
    check(parsed.options.positional_url == "notes.txt", "editor positional file parsed");
    check(parsed.options.output_path == "saved.txt", "editor save-as output parsed");
}


void test_cli_html_extract_parse() {
    const char* argv[] = {"pkchat", "--fetch-url", "https://example.com/page", "--html-format", "markdown",
                          "--max-fetch-bytes", "123", "--allow-private-url-fetch", "--output", "page.md"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(10, const_cast<char**>(argv));
    check(parsed.error.ok(), "HTML fetch args parse");
    check(parsed.options.fetch_url == "https://example.com/page", "HTML fetch URL parsed");
    check(parsed.options.html_format == "markdown", "HTML output format parsed");
    check(parsed.options.max_fetch_bytes == 123, "HTML max fetch bytes parsed");
    check(parsed.options.allow_private_url_fetch, "HTML private fetch override parsed");
    check(parsed.options.output_path == "page.md", "HTML output path parsed");

    const char* file_argv[] = {"pkchat", "--input", "page.html", "--output-format", "plaintext"};
    parsed = pkchat::cli::parse_args(5, const_cast<char**>(file_argv));
    check(parsed.error.ok(), "input file args parse");
    check(parsed.options.input_path == "page.html", "input file path parsed");
    check(parsed.options.output_format == pkchat::markdown::OutputFormat::Plaintext, "input plaintext output format parsed");
    check(parsed.options.rendered_output_format_explicit, "input rendered output format marked explicit");

    const char* legacy_file_argv[] = {"pkchat", "--html-file", "page.html", "--html-format", "text"};
    parsed = pkchat::cli::parse_args(5, const_cast<char**>(legacy_file_argv));
    check(parsed.error.ok(), "legacy HTML file args parse");
    check(parsed.options.html_file == "page.html", "legacy HTML file path parsed");
    check(parsed.options.html_format == "text", "legacy HTML text format parsed");

    const char* image_argv[] = {"pkchat", "--input", "PHOTO.JPEG", "--max-image-bytes", "4096", "-p", "describe"};
    parsed = pkchat::cli::parse_args(7, const_cast<char**>(image_argv));
    check(parsed.error.ok(), "image input args parse");
    check(parsed.options.input_path == "PHOTO.JPEG", "image input path parsed");
    check(parsed.options.max_image_bytes == 4096, "image byte limit parsed");

    const char* attach_argv[] = {"pkchat", "-p", "compare", "--attach", "one.md", "--attach", "two.txt",
                                 "--max-input-bytes", "8192"};
    parsed = pkchat::cli::parse_args(9, const_cast<char**>(attach_argv));
    check(parsed.error.ok(), "repeatable attachment args parse");
    check(parsed.options.attachment_paths.size() == 2, "two attachment paths parsed");
    check(parsed.options.attachment_paths[0] == "one.md" && parsed.options.attachment_paths[1] == "two.txt",
          "attachment path order is preserved");
    check(parsed.options.max_input_bytes == 8192, "text input byte limit parsed");

    const char* context_argv[] = {"pkchat", "-p", "hello", "--context-policy", "summarize-middle",
                                  "--max-context-bytes", "4096", "--image-capability", "allow"};
    parsed = pkchat::cli::parse_args(9, const_cast<char**>(context_argv));
    check(parsed.error.ok(), "context and image capability args parse");
    check(parsed.options.context_policy == "summarize-middle", "context policy parsed");
    check(parsed.options.max_context_bytes == 4096, "context byte limit parsed");
    check(parsed.options.image_capability == "allow", "image capability override parsed");
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

void test_image_capability_detection() {
    pkchat::provider::RequestContext context;
    context.api_kind = pkchat::provider::ApiKind::ChatCompletions;
    context.profile.name = "custom_openai_chat";
    context.profile.capabilities.images = true;
    context.options.model = "Qwen3.5-35B-A3B";
    check(pkchat::provider::detected_capabilities_for(context).images,
          "Qwen3.5 model is detected as image capable");
    check(pkchat::provider::validate_image_input(context).ok(),
          "detected vision model accepts image input");

    context.options.model = "unknown-text-model";
    check(!pkchat::provider::validate_image_input(context).ok(),
          "unknown model requires an explicit image capability decision");
    context.options.image_capability = "allow";
    check(pkchat::provider::validate_image_input(context).ok(),
          "explicit image capability override allows a compatible unknown model");
    context.api_kind = pkchat::provider::ApiKind::Responses;
    check(!pkchat::provider::validate_image_input(context).ok(),
          "Responses image input remains rejected until its request schema is implemented");
}

void test_context_policies_preserve_full_messages() {
    std::vector<pkchat::provider::Message> messages = {
        {"system", "system"},
        {"user", std::string(400, 'a')},
        {"assistant", std::string(400, 'b')},
        {"user", std::string(400, 'c')},
        {"assistant", std::string(400, 'd')},
    };
    const std::vector<pkchat::provider::Message> original = messages;
    pkchat::context::PreparedMessages error = pkchat::context::prepare(messages, "error", 500);
    check(!error.error.ok(), "error context policy rejects an oversized request");

    pkchat::context::PreparedMessages truncated = pkchat::context::prepare(messages, "truncate-oldest", 500);
    check(truncated.error.ok() && truncated.compacted, "truncate-oldest compacts provider messages");
    check(truncated.event.messages_compacted > 0 && pkchat::context::estimated_text_bytes(truncated.messages) <= 500,
          "truncate-oldest respects the configured text budget");
    check(messages.size() == original.size() && messages[1].content == original[1].content,
          "context preparation leaves the full source transcript unchanged");

    pkchat::context::PreparedMessages summarized = pkchat::context::prepare(messages, "summarize-oldest", 600);
    check(summarized.error.ok() && summarized.compacted, "summarize-oldest compacts provider messages");
    check(pkchat::context::estimated_text_bytes(summarized.messages) <= 600,
          "summarize-oldest respects the configured text budget");
    bool summary_seen = false;
    for (const pkchat::provider::Message& message : summarized.messages) {
        summary_seen = summary_seen || message.content.find("Context summary of") != std::string::npos;
    }
    check(summary_seen, "summarize-oldest inserts a visible request-only summary");

    pkchat::context::PreparedMessages middle = pkchat::context::prepare(messages, "summarize-middle", 1000);
    check(middle.error.ok() && middle.compacted, "summarize-middle compacts middle provider messages");
    check(middle.messages.back().content == messages.back().content,
          "summarize-middle preserves the newest message");
    pkchat::context::PreparedMessages automatic = pkchat::context::prepare(messages, "provider-auto", 1);
    check(automatic.error.ok() && !automatic.compacted && automatic.messages.size() == messages.size(),
          "provider-auto delegates context management without changing messages");
}

void test_http_private_address_socket_block() {
    pkchat::http::Request request;
    request.url = "http://127.0.0.1:1/";
    request.connect_timeout_seconds = 1;
    request.block_private_addresses = true;
    pkchat::http::Result result = pkchat::http::perform(request, {});
    check(!result.error.ok() && result.error.code == pkchat::ErrorCode::BadUrl,
          "HTTP transport blocks the resolved loopback socket address");
    check(result.error.message.find("127.0.0.1") != std::string::npos,
          "resolved-address refusal identifies the blocked address");
}

void test_safe_fetch_rejects_private_literal() {
    pkchat::fetch::Options options;
    std::string body;
    pkchat::Error err = pkchat::fetch::fetch_html("http://127.0.0.1/private", options, body);
    check(!err.ok() && err.code == pkchat::ErrorCode::BadUrl,
          "shared URL fetch rejects a private literal before transport");
    err = pkchat::fetch::fetch_html("file:///tmp/page.html", options, body);
    check(!err.ok() && err.code == pkchat::ErrorCode::BadUrl,
          "shared URL fetch rejects non-HTTP schemes");
}

void test_cli_output_format_parse() {
    const char* argv[] = {"pkchat", "-p", "hello", "--output-format", "html", "--output", "answer.html"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(7, const_cast<char**>(argv));
    check(parsed.error.ok(), "CLI output-format args parse");
    check(parsed.options.output_format == pkchat::markdown::OutputFormat::Html, "HTML output format parsed");
    check(parsed.options.output_format_explicit, "output-format explicit flag parsed");
    check(parsed.options.output_path == "answer.html", "output path parsed with output-format");

    const char* plain_argv[] = {"pkchat", "-p", "hello", "--output-format", "plaintext"};
    parsed = pkchat::cli::parse_args(5, const_cast<char**>(plain_argv));
    check(parsed.error.ok(), "CLI plaintext output-format args parse");
    check(parsed.options.output_format == pkchat::markdown::OutputFormat::Plaintext, "plaintext output format parsed");

    const char* json_argv[] = {"pkchat", "-p", "hello", "--output-format", "json"};
    parsed = pkchat::cli::parse_args(5, const_cast<char**>(json_argv));
    check(parsed.error.ok(), "CLI json output-format args parse");
    check(parsed.options.format == pkchat::cli::OutputFormat::Json, "json output-format maps to JSON format");
    check(!parsed.options.rendered_output_format_explicit, "json output-format is not a rendered text format");

    const char* jsond_argv[] = {"pkchat", "-p", "hello", "--output-format", "jsond"};
    parsed = pkchat::cli::parse_args(5, const_cast<char**>(jsond_argv));
    check(parsed.error.ok(), "CLI jsond output-format args parse");
    check(parsed.options.format == pkchat::cli::OutputFormat::Ndjson, "jsond output-format maps to NDJSON format");

    const char* bad_argv[] = {"pkchat", "-p", "hello", "--output-format", "pdf"};
    parsed = pkchat::cli::parse_args(5, const_cast<char**>(bad_argv));
    check(!parsed.error.ok(), "CLI rejects bad output-format");
}

void test_html_markdown_conversion() {
    const std::string html =
        "<html><head><style>.x{}</style><script>bad()</script></head>"
        "<body><h1>Title &amp; More</h1><p>Hello <strong>bold</strong> and <em>em</em> "
        "<a href=\"https://example.com?q=1&amp;x=2\">link</a>.</p>"
        "<h2>Next</h2><p><b>heavy</b> <italic>tilt</italic></p></body></html>";
    const std::string out = pkchat::html::convert(html, pkchat::html::OutputFormat::Markdown);
    check(out.find("# Title & More") != std::string::npos, "HTML h1 converts to Markdown heading");
    check(out.find("Hello **bold** and *em* [link](https://example.com?q=1&x=2).") != std::string::npos,
          "HTML inline tags convert to Markdown");
    check(out.find("## Next") != std::string::npos, "HTML h2 converts to Markdown heading");
    check(out.find("**heavy** *tilt*") != std::string::npos, "HTML b and italic convert to Markdown emphasis");
    check(out.find("bad()") == std::string::npos, "HTML script content is ignored");
}

void test_html_text_conversion() {
    const std::string html =
        "<h1>Title &amp; More</h1><p>Hello <strong>bold</strong> and <em>em</em> "
        "<a href='https://example.com/docs'>docs</a>.</p>";
    const std::string out = pkchat::html::convert(html, pkchat::html::OutputFormat::Text);
    check(out.find("Title & More") != std::string::npos, "HTML text output keeps heading text");
    check(out.find("Hello bold and em docs (https://example.com/docs).") != std::string::npos,
          "HTML text output keeps link URL next to link text");
    check(out.find("**") == std::string::npos && out.find("[") == std::string::npos,
          "HTML text output does not include Markdown syntax");
}


void test_html_large_ignored_blocks() {
    std::string html = "<h1>Before</h1><script>";
    html += std::string(200000, '<');
    html += "</script><style>";
    html += std::string(200000, '>');
    html += "</style><p>After <a href=\"https://example.com\">link</a></p>";
    const std::string out = pkchat::html::convert(html, pkchat::html::OutputFormat::Markdown);
    check(out.find("# Before") != std::string::npos, "HTML large ignored block keeps preceding text");
    check(out.find("After [link](https://example.com)") != std::string::npos,
          "HTML large ignored block keeps following text");
}

void test_html_malformed_documents() {
    const std::string no_doctype = "<html><body><h1>No doctype</h1><p>Body text</p></body></html>";
    std::string out = pkchat::html::convert(no_doctype, pkchat::html::OutputFormat::Markdown);
    check(out.find("# No doctype") != std::string::npos, "HTML conversion does not require a DOCTYPE");
    check(out.find("Body text") != std::string::npos, "HTML without DOCTYPE keeps body text");

    const std::string unclosed = "<html><body><h1>I forgot to close this...<p>Next paragraph";
    out = pkchat::html::convert(unclosed, pkchat::html::OutputFormat::Markdown);
    check(out.find("# I forgot to close this...") != std::string::npos,
          "HTML unclosed heading keeps heading text");
    check(out.find("Next paragraph") != std::string::npos, "HTML unclosed tags keep following text");

    const std::string misquoted = "<p>Before <img width=\"100 height=\"100\"> after</p>";
    out = pkchat::html::convert(misquoted, pkchat::html::OutputFormat::Text);
    check(out.find("Before after") != std::string::npos,
          "HTML misquoted image attributes do not swallow surrounding text");
    check(out.find("width") == std::string::npos, "HTML misquoted image tag is stripped as a tag");

    const std::string misspelled = "<p><strnog>not bold</strnog> and <emphasis>not italic</emphasis></p>";
    out = pkchat::html::convert(misspelled, pkchat::html::OutputFormat::Markdown);
    check(out.find("not bold and not italic") != std::string::npos,
          "HTML misspelled tags are ignored while keeping text");
    check(out.find("**") == std::string::npos && out.find("*not italic*") == std::string::npos,
          "HTML misspelled formatting tags do not create Markdown emphasis");
}

void test_html_utf8_validation() {
    const std::string utf8 = u8"<h1>Привет 中文</h1>";
    size_t offset = 0;
    check(pkchat::html::is_valid_utf8(utf8, &offset), "HTML validator accepts valid UTF-8 Russian and Chinese text");
    check(offset == utf8.size(), "HTML validator reports end offset for valid UTF-8");

    const std::string windows1251_russian = std::string("<h1>") + "\xCF\xF0\xE8\xE2\xE5\xF2" + "</h1>";
    offset = 0;
    check(!pkchat::html::is_valid_utf8(windows1251_russian, &offset),
          "HTML validator rejects Windows-1251 Russian bytes");
    check(offset == 4, "HTML validator reports the first invalid Windows-1251 byte offset");

    const std::string gbk_chinese = std::string("<h1>") + "\xD6\xD0\xCE\xC4" + "</h1>";
    offset = 0;
    check(!pkchat::html::is_valid_utf8(gbk_chinese, &offset), "HTML validator rejects GBK Chinese bytes");
    check(offset == 4, "HTML validator reports the first invalid GBK byte offset");
}

void test_markdown_html_rendering() {
    const std::string md =
        "# Title & More\n\n"
        "Paragraph with **bold**, *em*, ++under++, [docs](https://example.com?a=1&b=2), and `code <x>`.\n\n"
        "- parent\n"
        "  - child\n\n"
        "1. first\n"
        "2. second\n\n"
        "| Name | Value |\n"
        "| --- | --- |\n"
        "| A | **B** |\n\n"
        "```cpp\n"
        "if (a < b) return;\n"
        "```\n\n"
        "<div>raw</div>\n";
    const std::string html = pkchat::markdown::to_html_fragment(md);
    check(html.find("<h1>Title &amp; More</h1>") != std::string::npos, "Markdown h1 converts to HTML");
    check(html.find("<strong>bold</strong>") != std::string::npos, "Markdown bold converts to strong");
    check(html.find("<em>em</em>") != std::string::npos, "Markdown italic converts to em");
    check(html.find("<u>under</u>") != std::string::npos, "Markdown underline converts to u");
    check(html.find(R"PK(<a href="https://example.com?a=1&amp;b=2">docs</a>)PK") != std::string::npos,
          "Markdown links become escaped anchors");
    check(html.find("<code>code &lt;x&gt;</code>") != std::string::npos, "Markdown inline code escapes HTML");
    check(html.find(R"PK(<ul>
<li>parent<ul>
<li>child</li>)PK") != std::string::npos,
          "Markdown nested unordered lists convert to nested ul/li");
    check(html.find(R"PK(<ol>
<li>first</li>
<li>second</li>)PK") != std::string::npos,
          "Markdown ordered lists convert to ol/li");
    check(html.find("<table>") != std::string::npos && html.find("<th>Name</th>") != std::string::npos &&
              html.find("<td><strong>B</strong></td>") != std::string::npos,
          "Markdown tables convert to HTML tables");
    check(html.find(R"PK(<pre><code class="language-cpp">if (a &lt; b) return;
</code></pre>)PK") != std::string::npos,
          "Markdown fenced code converts to escaped pre/code");
    check(html.find("<div>raw</div>") != std::string::npos, "Markdown raw HTML block is preserved");
}

void test_markdown_plaintext_and_document_rendering() {
    const std::string md = "## Heading\n\nParagraph with **bold** and [docs](https://example.com).\n\n```\n**not bold**\n```\n";
    const std::string plain = pkchat::markdown::to_plaintext(md);
    check(plain.find("Heading") != std::string::npos && plain.find("##") == std::string::npos,
          "Markdown plaintext strips heading marker");
    check(plain.find("Paragraph with bold and docs (https://example.com).") != std::string::npos,
          "Markdown plaintext strips inline markup and keeps link URL");
    check(plain.find("**not bold**") != std::string::npos, "Markdown plaintext keeps code block content");

    const std::string doc = pkchat::markdown::to_html_document("# Saved");
    check(doc.find("<!doctype html>") == 0, "Markdown HTML document starts with doctype");
    check(doc.find(R"PK(<meta charset="utf-8">)PK") != std::string::npos, "Markdown HTML document includes charset");
    check(doc.find(R"PK(name="viewport")PK") != std::string::npos, "Markdown HTML document includes viewport");
    check(doc.find("<h1>Saved</h1>") != std::string::npos, "Markdown HTML document includes rendered body");
}

void test_comprehensive_markdown_to_html_fixture() {
    const std::string input = read_fixture("tests/fixtures/comprehensive.md");
    const std::string output = pkchat::markdown::to_html_document(input);

    check(output.find("<h1>Comprehensive Markdown Fixture</h1>") != std::string::npos,
          "comprehensive Markdown converts level-one heading");
    check(output.find("<h2>Lists And Structure</h2>") != std::string::npos &&
              output.find("<h3>Third-Level Heading</h3>") != std::string::npos,
          "comprehensive Markdown converts three heading levels");
    check(output.find("<strong>bold text</strong>") != std::string::npos &&
              output.find("<em>italic text</em>") != std::string::npos &&
              output.find("<u>underlined text</u>") != std::string::npos,
          "comprehensive Markdown converts inline formatting");
    check(output.find("<ul>") != std::string::npos && output.find("<ol>") != std::string::npos,
          "comprehensive Markdown converts ordered and unordered lists");
    check(output.find(R"PK(<a href="https://example.com/docs?lang=en&amp;mode=test">normal link</a>)PK") !=
              std::string::npos,
          "comprehensive Markdown converts and escapes links");
    check(output.find(R"PK(<a href="https://example.com/gallery"><img src="https://example.com/assets/placeholder.png" alt="A linked placeholder image"></a>)PK") !=
              std::string::npos,
          "comprehensive Markdown converts a linked image");
    check(output.find("你好，世界") != std::string::npos && output.find("مرحبا بالعالم") != std::string::npos &&
              output.find("😀 🚀 ✅") != std::string::npos,
          "comprehensive Markdown preserves multilingual UTF-8 and emoji");
    check(output.find(R"PK(<code class="language-javascript">)PK") != std::string::npos &&
              output.find("<table>") != std::string::npos,
          "comprehensive Markdown converts fenced code and a table");
}

void test_comprehensive_html_to_markdown_fixture() {
    const std::string input = read_fixture("tests/fixtures/comprehensive.html");
    const std::string output = pkchat::html::convert(input, pkchat::html::OutputFormat::Markdown);

    check(output.find("# Comprehensive HTML Fixture") != std::string::npos,
          "comprehensive HTML converts level-one heading");
    check(output.find("## Languages And Emoji") != std::string::npos &&
              output.find("### Multilingual Content") != std::string::npos,
          "comprehensive HTML converts three heading levels");
    check(output.find("**bold text**") != std::string::npos && output.find("*italic text*") != std::string::npos &&
              output.find("++underlined text++") != std::string::npos,
          "comprehensive HTML converts inline formatting");
    check(output.find("- First item") != std::string::npos &&
              output.find("1. Prepare the fixture") != std::string::npos &&
              output.find("3. Verify the result ✅") != std::string::npos,
          "comprehensive HTML converts ordered and unordered lists");
    check(output.find("[a normal link](https://example.com/docs?lang=en&mode=test)") != std::string::npos,
          "comprehensive HTML converts links and decodes entities");
    check(output.find("[![A linked placeholder image](https://example.com/assets/placeholder.png)](https://example.com/gallery)") !=
              std::string::npos,
          "comprehensive HTML converts a linked image");
    check(output.find("你好，世界") != std::string::npos && output.find("مرحبا بالعالم") != std::string::npos &&
              output.find("😀 🚀 ✅") != std::string::npos,
          "comprehensive HTML preserves multilingual UTF-8 and emoji");
    check(output.find("fixtureGreeting") == std::string::npos && output.find("color-scheme") == std::string::npos,
          "comprehensive HTML excludes script and style contents");
}

void test_editor_piece_table_edits() {
    pkchat::editor::PieceTable table = pkchat::editor::PieceTable::from_string("alpha\nbeta\ngamma");
    check(table.size() == 16, "piece table initial size");
    check(table.line_count() == 3, "piece table initial line count");
    check(table.line_text(1) == "beta", "piece table line text");

    pkchat::Error err = table.insert(6, "wide\n");
    check(err.ok(), "piece table insert succeeds");
    check(table.str() == "alpha\nwide\nbeta\ngamma", "piece table insert preserves text");
    check(table.line_count() == 4, "piece table insert updates line count");

    err = table.erase(6, 5);
    check(err.ok(), "piece table erase succeeds");
    check(table.str() == "alpha\nbeta\ngamma", "piece table erase restores text");

    err = table.insert(table.size(), "\nlast");
    check(err.ok(), "piece table append succeeds");
    check(table.line_text(3) == "last", "piece table append line text");
}

void test_editor_rectangular_rendering() {
    pkchat::editor::EditorState state = pkchat::editor::EditorState::from_text("one\ntwo\nthree");
    pkchat::editor::Rect rect{4, 10, 2, 4};
    state.cursor = state.text.offset_for_line_column(1, 1);
    pkchat::editor::RenderedPanel rendered = state.render(rect);
    check(rendered.lines.size() == 2, "editor panel respects height");
    check(rendered.lines[0] == "one ", "editor panel pads first visible line");
    check(rendered.lines[1] == "two ", "editor panel pads second visible line");
    check(rendered.cursor.visible, "editor cursor visible in panel");
    check(rendered.cursor.row == 1 && rendered.cursor.col == 1, "editor cursor maps to panel coordinates");

    state.cursor = state.text.offset_for_line_column(2, 3);
    state.ensure_cursor_visible(rect);
    rendered = state.render(rect);
    check(state.scroll_line == 1, "editor vertical scroll follows cursor");
    check(rendered.lines[0] == "two ", "editor scrolled first line");
    check(rendered.lines[1] == "thre", "editor clips to panel width");
    check(rendered.cursor.visible && rendered.cursor.row == 1 && rendered.cursor.col == 3,
          "editor cursor remains visible after scroll");
}

void test_editor_word_wrap_rendering() {
    pkchat::editor::EditorState state = pkchat::editor::EditorState::from_text("abcdefghij");
    pkchat::editor::Rect rect{1, 1, 3, 4};
    pkchat::editor::RenderedPanel rendered = state.render(rect);
    check(rendered.lines.size() == 3, "editor wrapped panel respects height");
    check(rendered.lines[0] == "abcd", "editor hard-wraps long words first row");
    check(rendered.lines[1] == "efgh", "editor hard-wraps long words second row");
    check(rendered.lines[2] == "ij  ", "editor pads final wrapped row");

    state.cursor = state.text.offset_for_line_column(0, 8);
    state.ensure_cursor_visible({1, 1, 2, 4});
    rendered = state.render({1, 1, 2, 4});
    check(state.scroll_line == 1, "editor wrapped scroll follows cursor row");
    check(rendered.lines[0] == "efgh", "editor render starts at wrapped scroll row");
    check(rendered.lines[1] == "ij  ", "editor render includes next wrapped row");
    check(rendered.cursor.visible && rendered.cursor.row == 1 && rendered.cursor.col == 0,
          "editor cursor maps inside wrapped line");
}

void test_editor_word_wrap_breaks_on_spaces() {
    pkchat::editor::EditorState state = pkchat::editor::EditorState::from_text("alpha beta");
    pkchat::editor::RenderedPanel rendered = state.render({1, 1, 2, 8});
    check(rendered.lines[0] == "alpha   ", "editor wraps at a word break when available");
    check(rendered.lines[1] == "beta    ", "editor continues after the wrapped word break");
}

void test_editor_kill_to_line_end() {
    pkchat::editor::EditorState state = pkchat::editor::EditorState::from_text("alpha beta\ngamma");
    state.cursor = state.text.offset_for_line_column(0, 6);
    pkchat::Error err = state.kill_to_line_end();
    check(err.ok(), "editor kill to line end succeeds");
    check(state.text.str() == "alpha \ngamma", "editor kill to line end erases text before newline only");
    check(state.cursor == state.text.offset_for_line_column(0, 6), "editor kill to line end keeps cursor in place");
    check(state.dirty, "editor kill to line end marks dirty after deleting text");

    err = state.kill_to_line_end();
    check(err.ok(), "editor kill at end of line succeeds");
    check(state.text.str() == "alpha \ngamma", "editor kill at end of non-empty line leaves newline intact");

    pkchat::editor::EditorState middle = pkchat::editor::EditorState::from_text("alpha\n\ngamma");
    middle.cursor = middle.text.line_start(1);
    err = middle.kill_to_line_end();
    check(err.ok(), "editor kill empty middle line succeeds");
    check(middle.text.str() == "alpha\ngamma", "editor kill empty middle line removes that line");
    check(middle.cursor == middle.text.line_start(1), "editor kill empty middle line keeps cursor at next line start");

    pkchat::editor::EditorState last = pkchat::editor::EditorState::from_text("alpha\n");
    last.cursor = last.text.line_start(1);
    err = last.kill_to_line_end();
    check(err.ok(), "editor kill empty final line succeeds");
    check(last.text.str() == "alpha", "editor kill empty final line removes preceding newline");
    check(last.cursor == last.text.size(), "editor kill empty final line moves cursor to new end");

    pkchat::editor::EditorState only = pkchat::editor::EditorState::from_text("");
    err = only.kill_to_line_end();
    check(err.ok(), "editor kill single empty buffer succeeds");
    check(only.text.str().empty(), "editor kill single empty buffer is a no-op");
}

void test_editor_vertical_navigation_modes() {
    pkchat::editor::Rect rect{1, 1, 3, 4};
    pkchat::editor::EditorState logical = pkchat::editor::EditorState::from_text("abcdefghij\nXYZ");
    logical.cursor = logical.text.offset_for_line_column(0, 2);
    logical.preferred_column = 2;
    logical.move_down(rect);
    check(logical.cursor == logical.text.offset_for_line_column(1, 2),
          "editor default vertical movement uses logical lines");

    pkchat::editor::EditorState visual = pkchat::editor::EditorState::from_text("abcdefghij\nXYZ");
    visual.vertical_movement = pkchat::editor::VerticalMovementMode::VisualRow;
    visual.cursor = visual.text.offset_for_line_column(0, 2);
    visual.preferred_column = 2;
    visual.move_down(rect);
    check(visual.cursor == visual.text.line_start(0) + 6,
          "editor visual movement moves to wrapped row below within the same line");
    visual.move_down(rect);
    check(visual.cursor == visual.text.line_start(0) + 10,
          "editor visual movement moves to final short wrapped row");
    visual.move_down(rect);
    check(visual.cursor == visual.text.offset_for_line_column(1, 2),
          "editor visual movement crosses to next hard line after wrapped rows");
    visual.move_up(rect);
    check(visual.cursor == visual.text.line_start(0) + 10,
          "editor visual movement moves back up into previous line wrap overflow");
}

void test_editor_file_round_trip() {
    const std::string path = "build/unit-editor.txt";
    pkchat::editor::PieceTable table = pkchat::editor::PieceTable::from_string("first\nsecond");
    pkchat::Error err = pkchat::editor::save_file(path, table);
    check(err.ok(), "editor file saves");
    pkchat::editor::PieceTable loaded;
    err = pkchat::editor::load_file(path, loaded);
    check(err.ok(), "editor file loads");
    check(loaded.str() == "first\nsecond", "editor file round trip preserves text");
}

void test_editor_path_completion() {
    const std::string directory = "build/pkchat-tab-completion";
    std::error_code filesystem_error;
    std::filesystem::create_directories(directory + "/pkchat-folder", filesystem_error);
    check(!filesystem_error, "path completion fixture directory is created");

    const std::vector<std::string> files = {
        "pkchat-single-result.txt",
        "pkchat-cycle-alpha.txt",
        "pkchat-cycle-alpine.txt",
    };
    for (const std::string& name : files) {
        std::ofstream fixture(directory + "/" + name, std::ios::binary | std::ios::trunc);
        fixture << name;
        check(static_cast<bool>(fixture), "path completion fixture file is written: " + name);
    }

    pkchat::editor::PathCompleter completer;
    const std::string unique_prefix = "/insert " + directory + "/pkchat-single-r";
    pkchat::editor::EditorState unique = pkchat::editor::EditorState::from_text(unique_prefix);
    unique.cursor = unique.text.size();
    pkchat::editor::PathCompletionResult result = completer.complete(unique);
    check(result.error.ok() && result.match_count == 1, "path completion finds a unique file");
    check(unique.text.str() == "/insert " + directory + "/pkchat-single-result.txt",
          "one Tab fully completes a unique path");

    completer.reset();
    const std::string cycle_prefix = "/attach " + directory + "/pkchat-cy";
    pkchat::editor::EditorState cycling = pkchat::editor::EditorState::from_text(cycle_prefix);
    cycling.cursor = cycling.text.size();
    result = completer.complete(cycling);
    const std::string common = "/attach " + directory + "/pkchat-cycle-alp";
    check(result.error.ok() && result.match_count == 2 && !result.cycling,
          "first Tab reports multiple path matches");
    check(cycling.text.str() == common, "first Tab completes the unambiguous common path prefix");

    result = completer.complete(cycling);
    check(result.cycling && result.choice_index == 0,
          "second Tab selects the first sorted path choice");
    check(cycling.text.str() == "/attach " + directory + "/pkchat-cycle-alpha.txt",
          "second Tab inserts the first path choice");

    result = completer.complete(cycling);
    check(result.cycling && result.choice_index == 1,
          "third Tab selects the next path choice");
    check(cycling.text.str() == "/attach " + directory + "/pkchat-cycle-alpine.txt",
          "third Tab inserts the next path choice");

    result = completer.complete(cycling);
    check(result.cycling && result.choice_index == 0,
          "repeated Tab wraps path choices in sorted order");

    completer.reset();
    pkchat::editor::EditorState directory_state =
        pkchat::editor::EditorState::from_text(directory + "/pkchat-fol");
    directory_state.cursor = directory_state.text.size();
    result = completer.complete(directory_state);
    check(result.match_count == 1 && directory_state.text.str() == directory + "/pkchat-folder/",
          "directory completion appends a slash");

    completer.reset();
    pkchat::editor::EditorState missing =
        pkchat::editor::EditorState::from_text(directory + "/pkchat-does-not-exist");
    missing.cursor = missing.text.size();
    result = completer.complete(missing);
    check(result.error.ok() && result.match_count == 0 &&
              missing.text.str() == directory + "/pkchat-does-not-exist",
          "path completion leaves an unmatched path unchanged");

    pkchat::editor::EditorState cancelled =
        pkchat::editor::EditorState::from_text(directory + "/pkchat-single-r");
    cancelled.cursor = cancelled.text.size();
    result = completer.complete(cancelled, []() { return true; });
    check(result.error.code == pkchat::ErrorCode::Cancelled &&
              cancelled.text.str() == directory + "/pkchat-single-r",
          "a cancelled path scan leaves editor input unchanged");

    completer.reset();
    pkchat::editor::EditorState reset_cycle = pkchat::editor::EditorState::from_text(cycle_prefix);
    reset_cycle.cursor = reset_cycle.text.size();
    completer.complete(reset_cycle);
    completer.reset();
    result = completer.complete(reset_cycle);
    check(!result.cycling && reset_cycle.text.str() == common,
          "resetting completion prevents a later Tab from cycling stale choices");
}

void test_tui_layout_reserves_editor_input_panel() {
    pkchat::tui::Layout small = pkchat::tui::layout_for_terminal(8, 20);
    check(small.rows == 8 && small.cols == 20, "TUI layout clamps to requested small terminal");
    check(small.header_rows == 0 && small.history_row == 1, "TUI layout has no persistent header rows");
    check(small.history_rows >= 1, "TUI layout leaves room for chat history");
    check(small.input_rect.height == 3, "TUI layout keeps minimum multiline input height");
    check(small.input_rect.row + small.input_rect.height - 1 <= small.rows,
          "TUI input panel stays inside terminal rows");

    pkchat::tui::Layout large = pkchat::tui::layout_for_terminal(40, 100);
    check(large.input_rect.height == 8, "TUI layout uses one fifth of a large terminal for input");
    check(large.input_rect.width == 100, "TUI input panel tracks terminal width");
    check(large.history_rows > large.input_rect.height, "TUI layout keeps the editor from taking the full screen");
}

void test_tui_regeneration_plan_uses_last_user_turn() {
    pkchat::chat::Session session;
    session.messages.push_back({"system", "stay concise"});
    session.messages.push_back({"user", "first"});
    session.messages.push_back({"assistant", "one"});
    session.messages.push_back({"user", "second"});
    session.messages.push_back({"assistant", "two"});

    pkchat::tui::RegenerationPlan plan = pkchat::tui::regeneration_plan_for_session(session);
    check(plan.available, "TUI regeneration plan is available when a user turn exists");
    check(plan.erase_from == 3, "TUI regeneration plan erases from the last user turn");
    check(plan.prompt == "second", "TUI regeneration plan reuses the last user prompt");

    pkchat::chat::Session no_user;
    no_user.messages.push_back({"system", "only system"});
    plan = pkchat::tui::regeneration_plan_for_session(no_user);
    check(!plan.available, "TUI regeneration plan is unavailable without a user turn");
}

void test_tui_history_jump_helpers() {
    check(pkchat::tui::history_scroll_for_thread_beginning() > 1000000,
          "TUI Home jump requests a clamped scrollback maximum");
    check(pkchat::tui::history_scroll_for_thread_end() == 0,
          "TUI End jump returns to the live chat bottom");
}

void test_tui_thinking_trace_display() {
    const std::string raw = "<think>internal trace</think>\n\nVisible answer";
    pkchat::tui::ThinkingDisplay shown = pkchat::tui::thinking_display_text(raw, true);
    check(shown.text == raw, "TUI thinking trace mode keeps raw assistant text");

    pkchat::tui::ThinkingDisplay hidden = pkchat::tui::thinking_display_text(raw, false);
    check(hidden.saw_thinking_tag, "TUI thinking display detects hidden trace tags");
    check(!hidden.open_thinking_tag, "TUI thinking display detects closed trace tags");
    check(hidden.text == "Visible answer", "TUI thinking notrace hides closed trace blocks");

    hidden = pkchat::tui::thinking_display_text("<think>still reasoning", false);
    check(hidden.saw_thinking_tag, "TUI thinking display detects an open trace tag");
    check(hidden.open_thinking_tag, "TUI thinking display reports an open trace tag");
    check(hidden.text.empty(), "TUI thinking notrace hides an unfinished trace");

    hidden = pkchat::tui::thinking_display_text("Before <think>hidden</think> after", false);
    check(hidden.text == "Before  after", "TUI thinking notrace preserves visible text around a trace");
}

void test_tui_theme_parsing_and_contrast() {
    pkchat::tui::ThemeName theme = pkchat::tui::ThemeName::Dark;
    check(pkchat::tui::parse_theme_name("dark", theme), "TUI dark theme parses");
    check(theme == pkchat::tui::ThemeName::Dark, "TUI dark theme selected");
    check(pkchat::tui::parse_theme_name("Light", theme), "TUI light theme parses case-insensitively");
    check(theme == pkchat::tui::ThemeName::Light, "TUI light theme selected");
    check(!pkchat::tui::parse_theme_name("sepia", theme), "TUI rejects unknown theme");

    const std::vector<pkchat::tui::ThemeName> themes = {
        pkchat::tui::ThemeName::Dark,
        pkchat::tui::ThemeName::Light,
    };
    const std::vector<pkchat::tui::StyleRole> roles = {
        pkchat::tui::StyleRole::Text,
        pkchat::tui::StyleRole::Muted,
        pkchat::tui::StyleRole::ThinkingTrace,
        pkchat::tui::StyleRole::UserLabel,
        pkchat::tui::StyleRole::AssistantLabel,
        pkchat::tui::StyleRole::Error,
        pkchat::tui::StyleRole::Status,
        pkchat::tui::StyleRole::InputLabel,
    };

    for (pkchat::tui::ThemeName item : themes) {
        for (pkchat::tui::StyleRole role : roles) {
            const pkchat::tui::StylePair pair = pkchat::tui::style_pair_for(item, role);
            check(pkchat::tui::contrast_ratio(pair.foreground, pair.background) >= 4.5,
                  std::string("TUI theme contrast meets WCAG AA for ") + pkchat::tui::theme_name(item));
        }
    }

    const pkchat::tui::StylePair dark_text =
        pkchat::tui::style_pair_for(pkchat::tui::ThemeName::Dark, pkchat::tui::StyleRole::Text);
    const pkchat::tui::StylePair dark_thinking =
        pkchat::tui::style_pair_for(pkchat::tui::ThemeName::Dark, pkchat::tui::StyleRole::ThinkingTrace);
    check(pkchat::tui::contrast_ratio(dark_thinking.foreground, dark_thinking.background) <
              pkchat::tui::contrast_ratio(dark_text.foreground, dark_text.background),
          "TUI dark thinking trace text is dimmer than normal text");

    const pkchat::tui::StylePair light_text =
        pkchat::tui::style_pair_for(pkchat::tui::ThemeName::Light, pkchat::tui::StyleRole::Text);
    const pkchat::tui::StylePair light_thinking =
        pkchat::tui::style_pair_for(pkchat::tui::ThemeName::Light, pkchat::tui::StyleRole::ThinkingTrace);
    check(pkchat::tui::contrast_ratio(light_thinking.foreground, light_thinking.background) <
              pkchat::tui::contrast_ratio(light_text.foreground, light_text.background),
          "TUI light thinking trace text is less stark than normal text");
}

void test_cli_responses_parse() {
    const char* argv[] = {"pkchat", "--responses", "-p", "hello"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(4, const_cast<char**>(argv));
    check(parsed.error.ok(), "Responses API shortcut args parse");
    check(parsed.options.api == "responses", "--responses selects Responses API");
}

void test_provider_registry_resolves_added_profiles() {
    std::vector<pkchat::provider::Profile> profiles = pkchat::provider::built_in_profiles();
    check(profiles.size() >= 22, "provider registry includes offline and compatibility profiles");

    const char* grok_argv[] = {"pkchat", "grok", "--list-models", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult grok = pkchat::cli::parse_args(5, const_cast<char**>(grok_argv));
    check(grok.error.ok(), "grok alias args parse");
    pkchat::provider::ContextResult grok_ctx = pkchat::provider::build_context(grok.options);
    check(grok_ctx.error.ok(), "grok alias context builds");
    check(grok_ctx.context.profile.name == "xai", "grok alias resolves to xai");
    check(grok_ctx.context.base_url == "https://api.x.ai/v1", "xai base URL selected");

    const char* kimi_argv[] = {"pkchat", "--provider", "kimi", "--list-models", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult kimi = pkchat::cli::parse_args(6, const_cast<char**>(kimi_argv));
    check(kimi.error.ok(), "kimi alias args parse");
    pkchat::provider::ContextResult kimi_ctx = pkchat::provider::build_context(kimi.options);
    check(kimi_ctx.error.ok(), "kimi alias context builds");
    check(kimi_ctx.context.profile.name == "moonshot", "kimi alias resolves to moonshot");

    const char* llama_argv[] = {"pkchat", "llama.cpp", "--list-models"};
    pkchat::cli::ParseResult llama = pkchat::cli::parse_args(3, const_cast<char**>(llama_argv));
    check(llama.error.ok(), "llama.cpp alias args parse");
    pkchat::provider::ContextResult llama_ctx = pkchat::provider::build_context(llama.options);
    check(llama_ctx.error.ok(), "llama.cpp alias context builds");
    check(llama_ctx.context.profile.name == "llamacpp", "llama.cpp alias resolves to llamacpp");
    check(llama_ctx.context.profile.local_endpoint, "llamacpp is marked local");

    const char* vllm_argv[] = {"pkchat", "vllm", "--list-models"};
    pkchat::cli::ParseResult vllm = pkchat::cli::parse_args(3, const_cast<char**>(vllm_argv));
    check(vllm.error.ok(), "vllm shortcut args parse");
    pkchat::provider::ContextResult vllm_ctx = pkchat::provider::build_context(vllm.options);
    check(vllm_ctx.error.ok(), "vllm context builds");
    check(vllm_ctx.context.api_key == "token-abc123", "vllm uses configured dummy API key");

    const char* deepinfra_argv[] = {"pkchat", "--provider", "deepinfra", "--list-models", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult deepinfra = pkchat::cli::parse_args(6, const_cast<char**>(deepinfra_argv));
    check(deepinfra.error.ok(), "deepinfra args parse");
    pkchat::provider::ContextResult deepinfra_ctx = pkchat::provider::build_context(deepinfra.options);
    check(deepinfra_ctx.error.ok(), "deepinfra context builds");
    check(deepinfra_ctx.context.profile.key_envs.size() >= 2 && deepinfra_ctx.context.profile.key_envs[1] == "DEEPINFRA_TOKEN",
          "deepinfra registers alternate token env var");
}

void test_none_provider_allows_an_empty_endpoint() {
    const char* argv[] = {"pkchat", "--provider", "none", "--repl"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(4, const_cast<char**>(argv));
    check(parsed.error.ok(), "none provider parses without a positional endpoint");
    check(parsed.options.positional_url.empty(), "none provider keeps the omitted endpoint empty");
    check(pkchat::provider::validate_profile_name(parsed.options.provider).ok(),
          "none is a recognized provider name in standalone modes");

    pkchat::provider::ContextResult context = pkchat::provider::build_context(parsed.options);
    check(context.error.ok(), "none provider context builds without an endpoint");
    check(context.context.profile.name == "none" && context.context.profile.offline,
          "none resolves to the offline provider profile");
    check(context.context.base_url.empty() && context.context.chat_url.empty() &&
              context.context.responses_url.empty() && context.context.models_url.empty(),
          "none provider leaves every model endpoint empty");
    check(!pkchat::provider::capabilities_for(context.context).chat_completions &&
              !pkchat::provider::capabilities_for(context.context).model_listing,
          "none provider advertises no model capabilities");

    pkchat::provider::ModelsResult models;
    pkchat::Error err = pkchat::provider::list_models(context.context, models);
    check(err.code == pkchat::ErrorCode::UnsupportedFeature,
          "none provider rejects model listing before transport");

    pkchat::provider::ChatResult chat;
    err = pkchat::provider::send_chat_messages(
        context.context, {{"user", "hello"}},
        [](const std::string&) { return pkchat::ok_error(); }, chat);
    check(err.code == pkchat::ErrorCode::UnsupportedFeature,
          "none provider rejects chat before transport");

    const char* alias_argv[] = {"pkchat", "offline", "--repl"};
    pkchat::cli::ParseResult alias = pkchat::cli::parse_args(3, const_cast<char**>(alias_argv));
    pkchat::provider::ContextResult alias_context = pkchat::provider::build_context(alias.options);
    check(alias_context.error.ok() && alias_context.context.profile.name == "none",
          "offline positional alias resolves without an endpoint");

    const char* endpoint_argv[] = {
        "pkchat", "--provider", "none", "--base-url", "http://localhost:1234", "--repl"};
    pkchat::cli::ParseResult endpoint =
        pkchat::cli::parse_args(6, const_cast<char**>(endpoint_argv));
    pkchat::provider::ContextResult endpoint_context =
        pkchat::provider::build_context(endpoint.options);
    check(endpoint_context.error.code == pkchat::ErrorCode::BadArgs,
          "none provider rejects model endpoint overrides");
}

void test_provider_capabilities_and_responses_context() {
    const char* argv[] = {"pkchat", "--provider", "openai", "--api", "responses", "-p", "hello", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(9, const_cast<char**>(argv));
    check(parsed.error.ok(), "OpenAI Responses args parse");
    pkchat::provider::ContextResult ctx = pkchat::provider::build_context(parsed.options);
    check(ctx.error.ok(), "OpenAI Responses context builds");
    check(ctx.context.api_kind == pkchat::provider::ApiKind::Responses, "Responses API kind selected");
    check(pkchat::provider::active_request_url(ctx.context) == "https://api.openai.com/v1/responses",
          "OpenAI Responses endpoint selected");
    check(pkchat::provider::capabilities_for(ctx.context).responses_api, "OpenAI reports Responses capability");
    check(pkchat::provider::capabilities_for(ctx.context).chat_completions, "OpenAI reports Chat Completions capability");

    const char* shortcut_argv[] = {"pkchat", "--provider", "openai_responses", "-p", "hello", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult shortcut = pkchat::cli::parse_args(7, const_cast<char**>(shortcut_argv));
    check(shortcut.error.ok(), "openai_responses profile shortcut args parse");
    pkchat::provider::ContextResult shortcut_ctx = pkchat::provider::build_context(shortcut.options);
    check(shortcut_ctx.error.ok(), "openai_responses context builds");
    check(shortcut_ctx.context.profile.name == "openai", "openai_responses uses OpenAI profile");
    check(shortcut_ctx.context.api_kind == pkchat::provider::ApiKind::Responses, "openai_responses selects Responses API");
}

void test_explicit_chat_url_does_not_require_base_when_model_set() {
    const char* argv[] = {"pkchat", "--chat-url", "https://example.test/custom/chat", "-m", "model", "-p", "hello", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(9, const_cast<char**>(argv));
    check(parsed.error.ok(), "explicit chat URL args parse");
    pkchat::provider::ContextResult ctx = pkchat::provider::build_context(parsed.options);
    check(ctx.error.ok(), "explicit chat URL context builds without base URL when model is set");
    check(ctx.context.chat_url == "https://example.test/custom/chat", "explicit chat URL is preserved");
}

void test_provider_responses_unsupported_and_override() {
    const char* unsupported_argv[] = {"pkchat", "--provider", "openrouter", "--api", "responses", "-p", "hello", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult unsupported = pkchat::cli::parse_args(9, const_cast<char**>(unsupported_argv));
    check(unsupported.error.ok(), "unsupported Responses args parse");
    pkchat::provider::ContextResult unsupported_ctx = pkchat::provider::build_context(unsupported.options);
    check(!unsupported_ctx.error.ok(), "chat-only provider rejects built-in Responses API");
    check(unsupported_ctx.error.code == pkchat::ErrorCode::UnsupportedFeature, "Responses rejection uses unsupported feature error");

    const char* override_argv[] = {"pkchat", "--provider", "openrouter", "--api", "responses", "--responses-url", "https://example.test/v1/responses", "-p", "hello", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult override = pkchat::cli::parse_args(11, const_cast<char**>(override_argv));
    check(override.error.ok(), "Responses override args parse");
    pkchat::provider::ContextResult override_ctx = pkchat::provider::build_context(override.options);
    check(override_ctx.error.ok(), "Responses override context builds");
    check(override_ctx.context.responses_url == "https://example.test/v1/responses", "Responses override endpoint selected");
    check(pkchat::provider::capabilities_for(override_ctx.context).responses_api, "Responses override reports capability");
}

void test_cli_rejects_unknown() {
    const char* argv[] = {"pkchat", "--bogus"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(2, const_cast<char**>(argv));
    check(!parsed.error.ok(), "unknown option rejected");
    check(parsed.error.code == pkchat::ErrorCode::BadArgs, "unknown option is bad args");
}

void test_url_normalization() {
    bool changed = false;
    pkchat::Error err;
    std::string url = pkchat::provider::normalize_base_url("http://localhost:8000", &changed, err);
    check(err.ok(), "base URL without path is valid");
    check(changed, "base URL without path is changed");
    check(url == "http://localhost:8000/v1", "base URL appends /v1");
    url = pkchat::provider::normalize_base_url("http://localhost:8000/v1", &changed, err);
    check(err.ok(), "base URL with /v1 is valid");
    check(!changed, "base URL with /v1 unchanged");
    check(url == "http://localhost:8000/v1", "base URL with /v1 preserved");
    url = pkchat::provider::normalize_base_url("ftp://localhost", &changed, err);
    check(!err.ok() && err.code == pkchat::ErrorCode::BadUrl, "bad URL rejected");
}

void test_json_parse() {
    pkchat::json::ParseResult parsed = pkchat::json::parse("{\"data\":[{\"id\":\"m1\"}],\"text\":\"hi\\nthere\"}");
    check(parsed.error.ok(), "JSON parse succeeds");
    const pkchat::json::Value* data = parsed.value.get("data");
    check(data != nullptr && data->is_array(), "JSON data array");
    const pkchat::json::Value* id = data->at(0)->get("id");
    check(id != nullptr && id->string == "m1", "JSON nested string");
}

void test_chat_session_json_round_trip() {
    pkchat::provider::RequestContext context;
    context.profile.name = "custom_openai_chat";
    context.base_url = "http://localhost:8000/v1";
    context.options.model = "mock-model";
    context.options.stream = false;
    pkchat::chat::Session session = pkchat::chat::new_session(context);
    session.created_at = "2026-06-14T00:00:00Z";
    session.updated_at = session.created_at;
    session.messages.push_back({"user", "hello"});
    session.messages.push_back({"assistant", "Hello"});
    session.compaction_events.push_back({"2026-06-14T00:01:00Z", "truncate-oldest", 2, 1000, 500,
                                         "Context compacted for test"});

    const std::string encoded = pkchat::chat::session_to_json(session);
    pkchat::json::ParseResult parsed = pkchat::json::parse(encoded);
    check(parsed.error.ok(), "chat session JSON parses");
    const pkchat::json::Value* messages = parsed.value.get("messages");
    check(messages != nullptr && messages->is_array() && messages->array.size() == 2, "chat messages persisted");

    const std::string path = "build/unit-chat.json";
    pkchat::Error err = pkchat::chat::save_session_atomic(path, session);
    check(err.ok(), "chat session saves atomically");
    pkchat::chat::Session loaded;
    err = pkchat::chat::load_session(path, loaded);
    check(err.ok(), "chat session loads");
    check(loaded.messages.size() == 2, "loaded chat has messages");
    check(!loaded.messages.empty() && loaded.messages[0].content == "hello", "loaded user message preserved");
    check(loaded.compaction_events.size() == 1 && loaded.compaction_events[0].messages_compacted == 2,
          "loaded chat preserves compaction events");
}

void test_chat_session_rejects_corrupt_json() {
    const std::string path = "build/corrupt-chat.json";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "{bad json";
    out.close();
    pkchat::chat::Session session;
    pkchat::Error err = pkchat::chat::load_session(path, session);
    check(!err.ok(), "corrupt chat file rejected");
    check(err.code == pkchat::ErrorCode::JsonParse, "corrupt chat file reports JSON parse error");
}

void test_runtime_event_queue_and_job_cancel() {
    pkchat::runtime::EventQueue<int> queue;
    int value = 0;
    check(!queue.try_pop(value), "empty runtime queue has no event");
    queue.push(7);
    check(queue.try_pop(value) && value == 7, "runtime queue preserves event value");

    pkchat::runtime::JobHandle job;
    std::atomic<bool> entered{false};
    job.start([&](pkchat::runtime::CancellationToken token) {
        entered.store(true, std::memory_order_release);
        while (!token.cancelled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        queue.push(42);
    });
    for (int i = 0; i < 100 && !entered.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(job.running(), "runtime job reports running");
    job.cancel();
    check(queue.wait_pop_for(value, std::chrono::milliseconds(1000)) && value == 42, "runtime job observes cancellation");
    job.join();
    check(!job.running(), "runtime job reports stopped after join");
}

void test_openrouter_shortcut_context() {
    const char* argv[] = {"pkchat", "openrouter", "-model", "provider/model", "-p", "hello", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(8, const_cast<char**>(argv));
    check(parsed.error.ok(), "openrouter shortcut args parse");
    pkchat::provider::ContextResult ctx = pkchat::provider::build_context(parsed.options);
    check(ctx.error.ok(), "openrouter shortcut context builds with auth header");
    check(ctx.context.profile.name == "openrouter", "openrouter shortcut selects profile");
    check(ctx.context.base_url == "https://openrouter.ai/api/v1", "openrouter shortcut uses standard base URL");
}
void test_openai_context_allows_missing_model() {
    const char* argv[] = {"pkchat", "--provider", "openai", "-p", "hello", "--header", "Authorization: Bearer test"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(7, const_cast<char**>(argv));
    check(parsed.error.ok(), "openai args without model parse");
    pkchat::provider::ContextResult ctx = pkchat::provider::build_context(parsed.options);
    check(ctx.error.ok(), "openai context builds without model so caller can discover one");
    check(ctx.context.options.model.empty(), "openai context keeps missing model empty before discovery");
}

void test_lmstudio_shortcut_context() {
    const char* argv[] = {"pkchat", "lmstudio", "-i"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(3, const_cast<char**>(argv));
    check(parsed.error.ok(), "lmstudio shortcut args parse");
    pkchat::provider::ContextResult ctx = pkchat::provider::build_context(parsed.options);
    check(ctx.error.ok(), "lmstudio shortcut context builds without key or model");
    check(ctx.context.profile.name == "lm_studio", "lmstudio shortcut selects profile");
    check(ctx.context.base_url == "http://localhost:1234/v1", "lmstudio shortcut uses default base URL");
    check(ctx.context.options.model.empty(), "lmstudio shortcut does not require model");
}

void test_lmstudio_context() {
    const char* argv[] = {"pkchat", "--provider", "lmstudio", "--list-models"};
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(4, const_cast<char**>(argv));
    check(parsed.error.ok(), "lmstudio args parse");
    pkchat::provider::ContextResult ctx = pkchat::provider::build_context(parsed.options);
    check(ctx.error.ok(), "lmstudio context builds without key");
    check(ctx.context.profile.name == "lm_studio", "lmstudio alias normalized");
    check(ctx.context.base_url == "http://localhost:1234/v1", "lmstudio default base URL");
}

}  // namespace

int main() {
    test_thinking_trace_splitter();
    test_cli_parse();
    test_cli_rejects_unknown();
    test_cli_provider_shortcut_parse();
    test_cli_repl_parse();
    test_cli_chat_parse();
    test_cli_chat_nocolors_parse();
    test_cli_editor_parse();
    test_cli_html_extract_parse();
    test_input_file_type_classification();
    test_image_loading_and_chat_request();
    test_text_context_loading_and_cancellation();
    test_image_capability_detection();
    test_context_policies_preserve_full_messages();
    test_http_private_address_socket_block();
    test_safe_fetch_rejects_private_literal();
    test_cli_output_format_parse();
    test_html_markdown_conversion();
    test_html_text_conversion();
    test_html_large_ignored_blocks();
    test_html_malformed_documents();
    test_html_utf8_validation();
    test_markdown_html_rendering();
    test_markdown_plaintext_and_document_rendering();
    test_comprehensive_markdown_to_html_fixture();
    test_comprehensive_html_to_markdown_fixture();
    test_cli_responses_parse();
    test_url_normalization();
    test_json_parse();
    test_lmstudio_context();
    test_provider_registry_resolves_added_profiles();
    test_none_provider_allows_an_empty_endpoint();
    test_provider_capabilities_and_responses_context();
    test_explicit_chat_url_does_not_require_base_when_model_set();
    test_provider_responses_unsupported_and_override();
    test_openrouter_shortcut_context();
    test_openai_context_allows_missing_model();
    test_lmstudio_shortcut_context();
    test_chat_session_json_round_trip();
    test_chat_session_rejects_corrupt_json();
    test_runtime_event_queue_and_job_cancel();
    test_editor_piece_table_edits();
    test_editor_rectangular_rendering();
    test_editor_word_wrap_rendering();
    test_editor_word_wrap_breaks_on_spaces();
    test_editor_kill_to_line_end();
    test_editor_vertical_navigation_modes();
    test_editor_file_round_trip();
    test_editor_path_completion();
    test_tui_layout_reserves_editor_input_panel();
    test_tui_regeneration_plan_uses_last_user_turn();
    test_tui_history_jump_helpers();
    test_tui_thinking_trace_display();
    test_tui_theme_parsing_and_contrast();
    if (failures != 0) {
        std::cerr << failures << " unit test(s) failed\n";
        return 1;
    }
    std::cout << "unit tests passed\n";
    return 0;
}
