#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::fetch {

struct Options {
    long connect_timeout_seconds = 10;
    long timeout_seconds = 30;
    // Cap on downloaded response body (raw HTML/bytes before conversion).
    long max_bytes = 10L * 1024L * 1024L;
    std::string proxy;
    bool insecure_tls = false;
    bool trace_http = false;
    bool allow_private = false;
    // Follow HTTP redirects (each hop still uses private-address socket checks).
    bool follow_redirects = true;
    // PNG, JPEG, and GIF stay raw bytes. Other callers leave this off.
    bool allow_images = false;
};

enum class DocumentKind {
    Html,
    Plaintext,
    Pdf,
    Docx,
    Xlsx,
    Pptx,
    Csv,
    Json,
    Image,
};

struct FetchedDocument {
    DocumentKind kind = DocumentKind::Html;
    std::string content_type;
    // UTF-8 HTML or plaintext, native CSV/JSON, or raw image bytes.
    std::string body;
    // Markdown for Pdf/Docx/Xlsx/Pptx/Csv/Json. HTML conversion is left to the caller.
    std::string markdown;
    std::size_t source_bytes = 0;
    // Bounded conversion diagnostics produced by OOXML conversion.
    std::vector<std::string> warnings;
};

Error fetch_html(const std::string& url,
                 const Options& options,
                 std::string& html,
                 runtime::CancellationToken cancellation = runtime::CancellationToken());
Error fetch_markdown(const std::string& url,
                     const Options& options,
                     std::string& markdown,
                     runtime::CancellationToken cancellation = runtime::CancellationToken(),
                     std::vector<std::string>* warnings = nullptr);
Error fetch_text(const std::string& url,
                 const Options& options,
                 std::string& text,
                 runtime::CancellationToken cancellation = runtime::CancellationToken(),
                 std::vector<std::string>* warnings = nullptr);
// HTML, PDF, DOCX, XLSX, PPTX, CSV, or JSON. Binary/structured documents are converted
// to Markdown in `markdown`; HTML stays in `body`.
Error fetch_document(const std::string& url,
                     const Options& options,
                     FetchedDocument& document,
                     runtime::CancellationToken cancellation = runtime::CancellationToken());

// Decode a downloaded body to valid UTF-8 using Content-Type / HTML meta charset.
// Built-in maps and allowlisted iconv names are used; already-valid UTF-8 is unchanged.
std::string convert_fetched_body_to_utf8(std::string body, const std::string& content_type);

// Classification helpers for tests and callers that already have a downloaded body.
std::string fetched_media_type(std::string content_type);
bool media_type_is_pdf(const std::string& media_type);
bool media_type_is_docx(const std::string& media_type);
bool media_type_is_xlsx(const std::string& media_type);
bool media_type_is_pptx(const std::string& media_type);
bool media_type_is_csv(const std::string& media_type);
bool media_type_is_json(const std::string& media_type);
bool media_type_is_html(const std::string& media_type);
bool media_type_is_plain(const std::string& media_type);
bool body_looks_like_pdf(std::string_view body);
bool body_looks_like_docx(std::string_view body);
bool body_looks_like_xlsx(std::string_view body);
bool body_looks_like_pptx(std::string_view body);
Error markdown_from_fetched_bytes(std::string_view body,
                                  const std::string& content_type,
                                  std::string& markdown,
                                  DocumentKind& kind,
                                  runtime::CancellationToken cancellation = runtime::CancellationToken(),
                                  std::vector<std::string>* warnings = nullptr,
                                  const std::string& source_url = {});

}  // namespace ainiux::fetch
