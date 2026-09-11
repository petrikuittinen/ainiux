#pragma once

#include <string>
#include <string_view>

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
};

struct FetchedDocument {
    DocumentKind kind = DocumentKind::Html;
    std::string content_type;
    // UTF-8 HTML or plaintext. Empty when kind is Pdf (converted to markdown).
    std::string body;
    // Markdown for Pdf (always) and for Html/Plaintext after fetch_markdown/fetch_text.
    std::string markdown;
};

Error fetch_html(const std::string& url,
                 const Options& options,
                 std::string& html,
                 runtime::CancellationToken cancellation = runtime::CancellationToken());
Error fetch_markdown(const std::string& url,
                     const Options& options,
                     std::string& markdown,
                     runtime::CancellationToken cancellation = runtime::CancellationToken());
Error fetch_text(const std::string& url,
                 const Options& options,
                 std::string& text,
                 runtime::CancellationToken cancellation = runtime::CancellationToken());
// HTML or PDF. PDF bytes are converted to Markdown in `markdown`; HTML stays in `body`.
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
bool media_type_is_html(const std::string& media_type);
bool media_type_is_plain(const std::string& media_type);
bool body_looks_like_pdf(std::string_view body);
Error markdown_from_fetched_bytes(std::string_view body,
                                  const std::string& content_type,
                                  std::string& markdown,
                                  DocumentKind& kind,
                                  runtime::CancellationToken cancellation = runtime::CancellationToken());

}  // namespace ainiux::fetch
