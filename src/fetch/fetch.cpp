#include "fetch/fetch.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <utility>
#include <vector>

#include "encoding/encoding.hpp"
#include "html/html.hpp"
#include "http/http.hpp"
#include "pdf/pdf.hpp"

namespace ainiux::fetch {
namespace {

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool starts_with(const std::string& text, const std::string& prefix) {
    return text.rfind(prefix, 0) == 0;
}

std::string normalize_body_to_utf8(std::string body, const std::string& content_type) {
    if (body.empty()) return body;
    std::string utf8;
    Error err = encoding::decode_web_bytes(body, content_type, utf8);
    if (err.ok()) {
        return utf8;
    }
    // Last resort: Windows-1252 so tool JSON never embeds ill-formed UTF-8.
    if (encoding::to_utf8(body, encoding::Encoding::Windows1252, utf8).ok()) {
        return utf8;
    }
    return body;
}

}  // namespace (charset helpers)

std::string convert_fetched_body_to_utf8(std::string body, const std::string& content_type) {
    return normalize_body_to_utf8(std::move(body), content_type);
}

namespace {

bool parse_ipv4_literal(const std::string& host, std::vector<int>& parts) {
    parts.clear();
    size_t start = 0;
    while (start <= host.size()) {
        const size_t dot = host.find('.', start);
        const size_t end = dot == std::string::npos ? host.size() : dot;
        if (end == start) {
            return false;
        }
        int value = 0;
        for (size_t i = start; i < end; ++i) {
            if (host[i] < '0' || host[i] > '9') {
                return false;
            }
            value = value * 10 + (host[i] - '0');
            if (value > 255) {
                return false;
            }
        }
        parts.push_back(value);
        if (dot == std::string::npos) {
            break;
        }
        start = dot + 1;
    }
    return parts.size() == 4;
}

bool is_blocked_ipv4(const std::vector<int>& ip) {
    if (ip.size() != 4) {
        return false;
    }
    return ip[0] == 0 || ip[0] == 10 || ip[0] == 127 ||
           (ip[0] == 100 && ip[1] >= 64 && ip[1] <= 127) ||
           (ip[0] == 169 && ip[1] == 254) ||
           (ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31) ||
           (ip[0] == 192 && ip[1] == 168) || ip[0] >= 224;
}

Error parse_url_host(const std::string& url, std::string& host) {
    const size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        return {ErrorCode::BadUrl, "URL fetch expects an absolute http:// or https:// URL: " + url};
    }
    const std::string scheme = ascii_lower(url.substr(0, scheme_end));
    if (scheme != "http" && scheme != "https") {
        return {ErrorCode::BadUrl, "URL fetch only supports http:// and https:// URLs: " + url};
    }
    const size_t authority_start = scheme_end + 3;
    const size_t authority_end = url.find_first_of("/?#", authority_start);
    std::string authority = authority_end == std::string::npos
                                ? url.substr(authority_start)
                                : url.substr(authority_start, authority_end - authority_start);
    if (authority.empty()) {
        return {ErrorCode::BadUrl, "URL fetch URL has no host: " + url};
    }
    const size_t at = authority.rfind('@');
    if (at != std::string::npos) {
        authority = authority.substr(at + 1);
    }
    if (!authority.empty() && authority.front() == '[') {
        const size_t close = authority.find(']');
        if (close == std::string::npos) {
            return {ErrorCode::BadUrl, "URL fetch has an unterminated IPv6 host: " + url};
        }
        host = authority.substr(1, close - 1);
    } else {
        const size_t colon = authority.rfind(':');
        host = colon == std::string::npos ? authority : authority.substr(0, colon);
    }
    host = ascii_lower(host);
    while (!host.empty() && host.back() == '.') {
        host.pop_back();
    }
    if (host.empty()) {
        return {ErrorCode::BadUrl, "URL fetch URL has no host: " + url};
    }
    return ok_error();
}

bool is_private_or_loopback_host(std::string host) {
    host = ascii_lower(std::move(host));
    while (!host.empty() && host.back() == '.') {
        host.pop_back();
    }
    if (host == "localhost" || ends_with(host, ".localhost") ||
        host == "metadata.google.internal" || host == "metadata" || host == "metadata.local") {
        return true;
    }
    std::vector<int> ipv4;
    if (parse_ipv4_literal(host, ipv4)) {
        return is_blocked_ipv4(ipv4);
    }
    if (starts_with(host, "::ffff:") && parse_ipv4_literal(host.substr(7), ipv4)) {
        return is_blocked_ipv4(ipv4);
    }
    if (host.find(':') != std::string::npos) {
        if (host == "::1" || host == "0:0:0:0:0:0:0:1") {
            return true;
        }
        const std::string first = host.substr(0, host.find(':'));
        if (starts_with(first, "fc") || starts_with(first, "fd") || starts_with(first, "ff")) {
            return true;
        }
        if (first.size() >= 3 && first[0] == 'f' && first[1] == 'e' &&
            (first[2] == '8' || first[2] == '9' || first[2] == 'a' || first[2] == 'b')) {
            return true;
        }
    }
    return false;
}

enum class FetchAccept {
    HtmlOnly,
    HtmlOrPlain,
    HtmlOrPdf,
    Document,  // HTML, plaintext, or PDF
};

std::string media_type_of(std::string content_type) {
    content_type = ascii_lower(std::move(content_type));
    const size_t semi = content_type.find(';');
    if (semi != std::string::npos) {
        content_type = content_type.substr(0, semi);
    }
    return ascii_trim(std::move(content_type));
}

bool classify_fetched_kind(const std::string& media_type,
                           const std::string& body,
                           FetchAccept accept,
                           DocumentKind& kind) {
    const bool allow_plain = accept == FetchAccept::HtmlOrPlain || accept == FetchAccept::Document;
    const bool allow_pdf = accept == FetchAccept::HtmlOrPdf || accept == FetchAccept::Document;
    if (media_type_is_pdf(media_type)) {
        kind = DocumentKind::Pdf;
        return allow_pdf;
    }
    if (media_type.empty() || media_type == "application/octet-stream") {
        if (body_looks_like_pdf(body)) {
            kind = DocumentKind::Pdf;
            return allow_pdf;
        }
        kind = DocumentKind::Html;
        return true;
    }
    if (media_type_is_html(media_type)) {
        kind = DocumentKind::Html;
        return true;
    }
    if (allow_plain && media_type_is_plain(media_type)) {
        kind = DocumentKind::Plaintext;
        return true;
    }
    return false;
}

const char* accept_header_for(FetchAccept accept) {
    switch (accept) {
        case FetchAccept::HtmlOrPlain:
            return "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,"
                   "text/plain;q=0.8,application/pdf;q=0.7,image/avif,image/webp,*/*;q=0.8";
        case FetchAccept::HtmlOrPdf:
            return "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,"
                   "application/pdf;q=0.8,image/avif,image/webp,image/apng,*/*;q=0.8";
        case FetchAccept::Document:
            return "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,"
                   "text/plain;q=0.8,application/pdf;q=0.8,image/avif,image/webp,*/*;q=0.8";
        case FetchAccept::HtmlOnly:
        default:
            return "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,"
                   "application/pdf;q=0.7,image/avif,image/webp,image/apng,*/*;q=0.8";
    }
}

const char* unsupported_type_message(FetchAccept accept) {
    switch (accept) {
        case FetchAccept::HtmlOrPlain:
            return "a supported text content type: ";
        case FetchAccept::HtmlOrPdf:
            return "an HTML or PDF content type: ";
        case FetchAccept::Document:
            return "a supported HTML, PDF, or text content type: ";
        case FetchAccept::HtmlOnly:
        default:
            return "an HTML content type: ";
    }
}

Error fetch_body(const std::string& url,
                 const Options& options,
                 FetchAccept accept,
                 std::string& body,
                 std::string& content_type,
                 DocumentKind& kind,
                 runtime::CancellationToken cancellation) {
    if (url.empty()) {
        return {ErrorCode::BadArgs, "URL fetch requires a non-empty URL"};
    }
    if (options.max_bytes <= 0) {
        return {ErrorCode::BadArgs, "--max-fetch-bytes must be greater than zero for URL fetch"};
    }
    std::string host;
    Error err = parse_url_host(url, host);
    if (!err.ok()) {
        return err;
    }
    if (!options.allow_private && is_private_or_loopback_host(host)) {
        return {ErrorCode::BadUrl,
                "refusing to fetch private, loopback, link-local, multicast, or metadata URL without "
                "--allow-private-url-fetch: " + url};
    }
    if (!options.allow_private && !options.proxy.empty()) {
        return {ErrorCode::BadUrl,
                "refusing URL fetch through --proxy because the target DNS address cannot be verified; "
                "use --allow-private-url-fetch only when the proxy and target are trusted"};
    }

    // Emulate a normal desktop browser. Some sites return 403/empty shells to
    // bare library UAs; a current Firefox profile reduces that without a real browser.
    constexpr const char* kBrowserUserAgent =
        "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0";
    http::Request request;
    request.method = "GET";
    request.url = url;
    request.headers.push_back(std::string("User-Agent: ") + kBrowserUserAgent);
    request.headers.push_back(accept_header_for(accept));
    request.headers.push_back("Accept-Language: en-US,en;q=0.9");
    request.headers.push_back("Upgrade-Insecure-Requests: 1");
    request.headers.push_back("Sec-Fetch-Dest: document");
    request.headers.push_back("Sec-Fetch-Mode: navigate");
    request.headers.push_back("Sec-Fetch-Site: none");
    request.headers.push_back("Sec-Fetch-User: ?1");
    request.headers.push_back("DNT: 1");
    request.connect_timeout_seconds = options.connect_timeout_seconds;
    request.timeout_seconds = options.timeout_seconds > 0 ? options.timeout_seconds : 30;
    request.proxy = options.proxy;
    request.insecure_tls = options.insecure_tls;
    request.trace = options.trace_http;
    request.block_private_addresses = !options.allow_private;
    request.max_body_bytes = options.max_bytes;
    // Default on: bare trailing-slash 301s (WordPress etc.) are common. Private
    // destinations remain blocked via CURLOPT_OPENSOCKETFUNCTION on each hop.
    request.follow_redirects = options.follow_redirects;
    request.cancellation = cancellation;

    http::Result result = http::perform(request, {});
    if (!result.error.ok()) {
        if (result.error.code == ErrorCode::FileRead &&
            result.error.message.find("HTTP response exceeded maximum body size") !=
                std::string::npos) {
            return {ErrorCode::FileRead,
                    result.error.message + " (limit " + std::to_string(options.max_bytes) +
                        " bytes); increase --max-fetch-bytes for a trusted larger document"};
        }
        return result.error;
    }
    if (result.response.status < 200 || result.response.status >= 300) {
        return {ErrorCode::HttpStatus,
                "HTTP " + std::to_string(result.response.status) + " while fetching URL: " + url};
    }
    if (!classify_fetched_kind(media_type_of(result.response.content_type), result.response.body,
                               accept, kind)) {
        return {ErrorCode::UnsupportedFeature,
                std::string("fetched URL did not return ") + unsupported_type_message(accept) + url +
                    " (Content-Type: " + result.response.content_type + ")"};
    }
    content_type = result.response.content_type;
    body = std::move(result.response.body);
    return ok_error();
}

Error convert_pdf_body(const std::string& url,
                       std::string body,
                       const Options& options,
                       std::string& markdown,
                       runtime::CancellationToken cancellation) {
    pdf::Options pdf_options;
    pdf_options.max_bytes = options.max_bytes > 0 ? static_cast<std::size_t>(options.max_bytes)
                                                  : body.size();
    pdf_options.cancellation = cancellation;
    Error err = pdf::to_markdown_bytes(body, pdf_options, markdown);
    if (!err.ok()) {
        return err;
    }
    if (cancellation.cancelled()) {
        return {ErrorCode::Cancelled, "URL fetch cancelled: " + url};
    }
    return ok_error();
}

}  // namespace

std::string fetched_media_type(std::string content_type) {
    return media_type_of(std::move(content_type));
}

bool media_type_is_pdf(const std::string& media_type) {
    return media_type == "application/pdf" || media_type == "application/x-pdf";
}

bool media_type_is_html(const std::string& media_type) {
    return media_type.empty() || media_type == "text/html" ||
           media_type == "application/xhtml+xml";
}

bool media_type_is_plain(const std::string& media_type) {
    return media_type == "text/plain";
}

bool body_looks_like_pdf(std::string_view body) {
    size_t i = 0;
    while (i < body.size() &&
           (body[i] == ' ' || body[i] == '\t' || body[i] == '\r' || body[i] == '\n')) {
        ++i;
    }
    return body.size() - i >= 5 && body.substr(i, 5) == "%PDF-";
}

Error markdown_from_fetched_bytes(std::string_view body,
                                  const std::string& content_type,
                                  std::string& markdown,
                                  DocumentKind& kind,
                                  runtime::CancellationToken cancellation) {
    markdown.clear();
    const std::string media = media_type_of(content_type);
    const bool sniffable = media.empty() || media == "application/octet-stream";
    if (media_type_is_pdf(media) || (sniffable && body_looks_like_pdf(body))) {
        kind = DocumentKind::Pdf;
        pdf::Options pdf_options;
        pdf_options.max_bytes = body.size();
        pdf_options.cancellation = cancellation;
        return pdf::to_markdown_bytes(body, pdf_options, markdown);
    }
    if (media_type_is_plain(media)) {
        kind = DocumentKind::Plaintext;
    } else if (media_type_is_html(media) || sniffable) {
        kind = DocumentKind::Html;
    } else {
        return {ErrorCode::UnsupportedFeature,
                "fetched body is not HTML, PDF, or plain text (Content-Type: " + content_type +
                    ")"};
    }
    if (cancellation.cancelled()) {
        return {ErrorCode::Cancelled, "fetched conversion cancelled"};
    }
    const std::string utf8 = normalize_body_to_utf8(std::string(body), content_type);
    markdown = kind == DocumentKind::Plaintext
                   ? utf8
                   : html::convert(utf8, html::OutputFormat::Markdown);
    return ok_error();
}

Error fetch_html(const std::string& url,
                 const Options& options,
                 std::string& html_body,
                 runtime::CancellationToken cancellation) {
    std::string content_type;
    DocumentKind kind = DocumentKind::Html;
    Error err = fetch_body(url, options, FetchAccept::HtmlOnly, html_body, content_type, kind,
                           cancellation);
    if (!err.ok()) return err;
    if (kind != DocumentKind::Html) {
        return {ErrorCode::UnsupportedFeature,
                "fetched URL did not return an HTML content type: " + url +
                    " (Content-Type: " + content_type + ")"};
    }
    html_body = normalize_body_to_utf8(std::move(html_body), content_type);
    return ok_error();
}

Error fetch_document(const std::string& url,
                     const Options& options,
                     FetchedDocument& document,
                     runtime::CancellationToken cancellation) {
    document = FetchedDocument{};
    std::string body;
    std::string content_type;
    DocumentKind kind = DocumentKind::Html;
    Error err = fetch_body(url, options, FetchAccept::HtmlOrPdf, body, content_type, kind,
                           cancellation);
    if (!err.ok()) return err;
    document.kind = kind;
    document.content_type = content_type;
    if (kind == DocumentKind::Pdf) {
        return convert_pdf_body(url, std::move(body), options, document.markdown, cancellation);
    }
    document.body = normalize_body_to_utf8(std::move(body), content_type);
    return ok_error();
}

Error fetch_markdown(const std::string& url,
                     const Options& options,
                     std::string& markdown,
                     runtime::CancellationToken cancellation) {
    FetchedDocument document;
    Error err = fetch_document(url, options, document, cancellation);
    if (!err.ok()) {
        return err;
    }
    if (cancellation.cancelled()) {
        return {ErrorCode::Cancelled, "URL fetch cancelled: " + url};
    }
    if (document.kind == DocumentKind::Pdf) {
        markdown = std::move(document.markdown);
        return ok_error();
    }
    markdown = html::convert(document.body, html::OutputFormat::Markdown);
    if (cancellation.cancelled()) {
        return {ErrorCode::Cancelled, "URL fetch cancelled: " + url};
    }
    return ok_error();
}

Error fetch_text(const std::string& url,
                 const Options& options,
                 std::string& text,
                 runtime::CancellationToken cancellation) {
    std::string body;
    std::string content_type;
    DocumentKind kind = DocumentKind::Html;
    Error err = fetch_body(url, options, FetchAccept::Document, body, content_type, kind,
                           cancellation);
    if (!err.ok()) {
        return err;
    }
    if (kind == DocumentKind::Pdf) {
        return convert_pdf_body(url, std::move(body), options, text, cancellation);
    }
    body = normalize_body_to_utf8(std::move(body), content_type);
    if (cancellation.cancelled()) {
        return {ErrorCode::Cancelled, "URL fetch cancelled: " + url};
    }
    text = kind == DocumentKind::Plaintext
               ? std::move(body)
               : html::convert(body, html::OutputFormat::Markdown);
    return ok_error();
}

}  // namespace ainiux::fetch
