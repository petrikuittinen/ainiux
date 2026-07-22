#include "fetch/fetch.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <utility>
#include <vector>

#include "html/html.hpp"
#include "http/http.hpp"

namespace ainiux::fetch {
namespace {

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool starts_with(const std::string& text, const std::string& prefix) {
    return text.rfind(prefix, 0) == 0;
}

std::string ascii_lower_copy(std::string text) {
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return text;
}

void append_utf8_codepoint(std::string& out, std::uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Map Windows-1252 / common "latin1" web bytes to Unicode. Browsers treat
// ISO-8859-1 HTML as Windows-1252 for the 0x80-0x9F range.
std::uint32_t windows1252_codepoint(unsigned char byte) {
    static const std::uint32_t kMap80[32] = {
        0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
        0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
        0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};
    if (byte < 0x80) return byte;
    if (byte >= 0xA0) return byte;  // ISO-8859-1 / Latin-1
    return kMap80[byte - 0x80];
}

std::string windows1252_to_utf8(const std::string& input) {
    std::string out;
    out.reserve(input.size() + input.size() / 4 + 8);
    for (unsigned char byte : input) {
        append_utf8_codepoint(out, windows1252_codepoint(byte));
    }
    return out;
}

std::string extract_charset_token(const std::string& lower_source) {
    // Accept charset=VALUE, charset = "VALUE", charset='VALUE'
    const size_t pos = lower_source.find("charset");
    if (pos == std::string::npos) return {};
    size_t i = pos + 7;
    while (i < lower_source.size() &&
           (lower_source[i] == ' ' || lower_source[i] == '\t'))
        ++i;
    if (i >= lower_source.size() || lower_source[i] != '=') return {};
    ++i;
    while (i < lower_source.size() &&
           (lower_source[i] == ' ' || lower_source[i] == '\t'))
        ++i;
    if (i >= lower_source.size()) return {};
    char quote = 0;
    if (lower_source[i] == '"' || lower_source[i] == '\'') {
        quote = lower_source[i];
        ++i;
    }
    const size_t start = i;
    while (i < lower_source.size()) {
        const char ch = lower_source[i];
        if (quote != 0) {
            if (ch == quote) break;
        } else if (ch == ' ' || ch == '\t' || ch == ';' || ch == '"' || ch == '\'' ||
                   ch == '>') {
            break;
        }
        ++i;
    }
    if (start >= i) return {};
    std::string value = lower_source.substr(start, i - start);
    // Drop trailing junk.
    while (!value.empty() && (value.back() == '"' || value.back() == '\'' || value.back() == ';'))
        value.pop_back();
    return value;
}

std::string charset_from_content_type(const std::string& content_type) {
    return extract_charset_token(ascii_lower_copy(content_type));
}

std::string charset_from_html_meta(const std::string& body) {
    // Only scan the head-ish prefix to avoid large bodies.
    const size_t scan = std::min(body.size(), static_cast<size_t>(8192));
    const std::string head = ascii_lower_copy(body.substr(0, scan));
    // <meta charset="utf-8">
    size_t pos = 0;
    while (pos < head.size()) {
        const size_t meta = head.find("<meta", pos);
        if (meta == std::string::npos) break;
        const size_t end = head.find('>', meta);
        if (end == std::string::npos) break;
        const std::string tag = head.substr(meta, end - meta + 1);
        std::string found = extract_charset_token(tag);
        if (!found.empty()) return found;
        // http-equiv content-type with charset in content=
        if (tag.find("content-type") != std::string::npos) {
            const size_t content = tag.find("content=");
            if (content != std::string::npos) {
                found = extract_charset_token(tag.substr(content));
                if (!found.empty()) return found;
            }
        }
        pos = end + 1;
    }
    return {};
}

bool is_utf8_charset(const std::string& charset) {
    return charset == "utf-8" || charset == "utf8" || charset == "us-ascii" || charset == "ascii";
}

bool is_legacy_8bit_charset(const std::string& charset) {
    return charset == "iso-8859-1" || charset == "iso8859-1" || charset == "latin1" ||
           charset == "latin-1" || charset == "windows-1252" || charset == "cp1252" ||
           charset == "windows1252" || charset == "ansi_x3.4-1968";
}

// Ensure tool/model-facing body is valid UTF-8. Declared ISO-8859-1 / Windows-1252
// (and mislabeled legacy HTML) is converted; already-valid UTF-8 is left alone.
std::string normalize_body_to_utf8(std::string body, const std::string& content_type) {
    if (body.empty()) return body;
    if (html::is_valid_utf8(body)) return body;

    std::string charset = charset_from_content_type(content_type);
    if (charset.empty()) charset = charset_from_html_meta(body);

    if (charset.empty() || is_legacy_8bit_charset(charset) || is_utf8_charset(charset)) {
        // Invalid UTF-8 with empty/legacy/utf-8 label: treat bytes as Windows-1252.
        // (Declared utf-8 that is actually latin1 is common on old Finnish sites.)
        return windows1252_to_utf8(body);
    }
    // Unknown non-UTF-8 charset: still attempt Windows-1252 rather than inject
    // ill-formed UTF-8 into JSON tool results.
    return windows1252_to_utf8(body);
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

bool supported_html_content_type(std::string content_type) {
    if (content_type.empty()) {
        return true;
    }
    content_type = ascii_lower(std::move(content_type));
    const size_t semi = content_type.find(';');
    if (semi != std::string::npos) {
        content_type = content_type.substr(0, semi);
    }
    content_type = ascii_trim(std::move(content_type));
    return content_type == "text/html" || content_type == "application/xhtml+xml";
}

bool supported_plain_content_type(std::string content_type) {
    content_type = ascii_lower(std::move(content_type));
    const size_t semi = content_type.find(';');
    if (semi != std::string::npos) {
        content_type = content_type.substr(0, semi);
    }
    return ascii_trim(std::move(content_type)) == "text/plain";
}

Error fetch_body(const std::string& url,
                 const Options& options,
                 bool allow_plaintext,
                 std::string& body,
                 std::string& content_type,
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
    request.headers.push_back(
        allow_plaintext
            ? "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,"
              "text/plain;q=0.8,image/avif,image/webp,*/*;q=0.8"
            : "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,"
              "image/avif,image/webp,image/apng,*/*;q=0.8");
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
    request.cancellation = cancellation;

    http::Result result = http::perform(request, {});
    if (!result.error.ok()) {
        return result.error;
    }
    if (result.response.status < 200 || result.response.status >= 300) {
        return {ErrorCode::HttpStatus,
                "HTTP " + std::to_string(result.response.status) + " while fetching URL: " + url};
    }
    if (!supported_html_content_type(result.response.content_type) &&
        !(allow_plaintext && supported_plain_content_type(result.response.content_type))) {
        return {ErrorCode::UnsupportedFeature,
                std::string("fetched URL did not return ") +
                    (allow_plaintext ? "a supported text content type: " : "an HTML content type: ") + url +
                    " (Content-Type: " + result.response.content_type + ")"};
    }
    content_type = result.response.content_type;
    body = std::move(result.response.body);
    return ok_error();
}

}  // namespace

Error fetch_html(const std::string& url,
                 const Options& options,
                 std::string& html_body,
                 runtime::CancellationToken cancellation) {
    std::string content_type;
    Error err = fetch_body(url, options, false, html_body, content_type, cancellation);
    if (!err.ok()) return err;
    html_body = normalize_body_to_utf8(std::move(html_body), content_type);
    return ok_error();
}

Error fetch_markdown(const std::string& url,
                     const Options& options,
                     std::string& markdown,
                     runtime::CancellationToken cancellation) {
    std::string html_body;
    Error err = fetch_html(url, options, html_body, cancellation);
    if (!err.ok()) {
        return err;
    }
    if (cancellation.cancelled()) {
        return {ErrorCode::Cancelled, "URL fetch cancelled: " + url};
    }
    // fetch_html already normalized to UTF-8.
    markdown = html::convert(html_body, html::OutputFormat::Markdown);
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
    Error err = fetch_body(url, options, true, body, content_type, cancellation);
    if (!err.ok()) {
        return err;
    }
    body = normalize_body_to_utf8(std::move(body), content_type);
    if (cancellation.cancelled()) {
        return {ErrorCode::Cancelled, "URL fetch cancelled: " + url};
    }
    text = supported_plain_content_type(content_type)
               ? std::move(body)
               : html::convert(body, html::OutputFormat::Markdown);
    return ok_error();
}

}  // namespace ainiux::fetch
