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
};

enum class DocumentKind {
    Html,
    Plaintext,
    Pdf,
    Docx,
    Xlsx,
    Csv,
    Json,
};

struct FetchedDocument {
    DocumentKind kind = DocumentKind::Html;
    std::string content_type;
    // UTF-8 HTML or plaintext. Empty for binary documents converted to Markdown.
    std::string body;
    // Markdown for Pdf/Docx/Xlsx/Csv/Json (always) and for Html/Plaintext after conversion.
    std::string markdown;
    // Bounded conversion diagnostics, currently produced by DOCX/XLSX conversion.
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
// HTML, PDF, DOCX, XLSX, CSV, or JSON. Binary/structured documents are converted
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
bool media_type_is_csv(const std::string& media_type);
bool media_type_is_json(const std::string& media_type);
bool media_type_is_html(const std::string& media_type);
bool media_type_is_plain(const std::string& media_type);
bool body_looks_like_pdf(std::string_view body);
bool body_looks_like_docx(std::string_view body);
bool body_looks_like_xlsx(std::string_view body);
Error markdown_from_fetched_bytes(std::string_view body,
                                  const std::string& content_type,
                                  std::string& markdown,
                                  DocumentKind& kind,
                                  runtime::CancellationToken cancellation = runtime::CancellationToken(),
                                  std::vector<std::string>* warnings = nullptr,
                                  const std::string& source_url = {});

}  // namespace ainiux::fetch
