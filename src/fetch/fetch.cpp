#include "fetch/fetch.hpp"

#include <cctype>
#include <utility>
#include <vector>

#include "html/html.hpp"
#include "http/http.hpp"

namespace pkchat::fetch {
namespace {

std::string lower_ascii(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::string trim_ascii(std::string text) {
    auto is_ws = [](unsigned char ch) { return ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t'; };
    while (!text.empty() && is_ws(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
    }
    while (!text.empty() && is_ws(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    return text;
}

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool starts_with(const std::string& text, const std::string& prefix) {
    return text.rfind(prefix, 0) == 0;
}

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
    const std::string scheme = lower_ascii(url.substr(0, scheme_end));
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
    host = lower_ascii(host);
    while (!host.empty() && host.back() == '.') {
        host.pop_back();
    }
    if (host.empty()) {
        return {ErrorCode::BadUrl, "URL fetch URL has no host: " + url};
    }
    return ok_error();
}

bool is_private_or_loopback_host(std::string host) {
    host = lower_ascii(std::move(host));
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
    content_type = lower_ascii(std::move(content_type));
    const size_t semi = content_type.find(';');
    if (semi != std::string::npos) {
        content_type = content_type.substr(0, semi);
    }
    content_type = trim_ascii(std::move(content_type));
    return content_type == "text/html" || content_type == "application/xhtml+xml";
}

bool supported_plain_content_type(std::string content_type) {
    content_type = lower_ascii(std::move(content_type));
    const size_t semi = content_type.find(';');
    if (semi != std::string::npos) {
        content_type = content_type.substr(0, semi);
    }
    return trim_ascii(std::move(content_type)) == "text/plain";
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

    http::Request request;
    request.method = "GET";
    request.url = url;
    request.headers.push_back(
        "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0");
    request.headers.push_back(allow_plaintext
                                  ? "Accept: text/plain,text/html;q=0.9,application/xhtml+xml;q=0.8"
                                  : "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");
    request.headers.push_back("Accept-Language: en-US,en;q=0.5");
    request.headers.push_back("Upgrade-Insecure-Requests: 1");
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
    return fetch_body(url, options, false, html_body, content_type, cancellation);
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
    size_t invalid_offset = 0;
    if (!html::is_valid_utf8(html_body, &invalid_offset)) {
        return {ErrorCode::UnsupportedFeature,
                "HTML extraction expects UTF-8 input; charset conversion is not implemented yet for URL " + url +
                    " (invalid byte at offset " + std::to_string(invalid_offset) +
                    "). Convert the document to UTF-8 and try again."};
    }
    if (cancellation.cancelled()) {
        return {ErrorCode::Cancelled, "URL fetch cancelled: " + url};
    }
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
    size_t invalid_offset = 0;
    if (!html::is_valid_utf8(body, &invalid_offset)) {
        return {ErrorCode::UnsupportedFeature,
                "text fetch expects UTF-8 input from " + url + " (invalid byte at offset " +
                    std::to_string(invalid_offset) + ")"};
    }
    if (cancellation.cancelled()) {
        return {ErrorCode::Cancelled, "URL fetch cancelled: " + url};
    }
    text = supported_plain_content_type(content_type)
               ? std::move(body)
               : html::convert(body, html::OutputFormat::Markdown);
    return ok_error();
}

}  // namespace pkchat::fetch
