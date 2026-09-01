#include "server/http_parser.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <utility>

#include "common.hpp"

namespace ainiux::server::http {
namespace {

bool token_char(unsigned char c) {
    if (std::isalnum(c)) return true;
    switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '*':
        case '+': case '-': case '.': case '^': case '_': case '`': case '|': case '~':
            return true;
        default: return false;
    }
}

bool valid_target(const std::string& target, std::string& path, std::string& query) {
    if (target.empty() || target.front() != '/' || target.find('#') != std::string::npos ||
        target.find('%') != std::string::npos || target.find('\\') != std::string::npos) {
        return false;
    }
    for (unsigned char c : target) {
        if (c <= 0x20U || c == 0x7fU || c >= 0x80U) return false;
    }
    const std::size_t question = target.find('?');
    path = target.substr(0, question);
    query = question == std::string::npos ? std::string() : target.substr(question + 1);
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::string segment = path.substr(start, slash - start);
        if (segment == "." || segment == "..") return false;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

bool valid_line_endings(const std::string& buffer) {
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        if (buffer[i] == '\n' && (i == 0 || buffer[i - 1] != '\r')) return false;
        if (buffer[i] == '\r' && i + 1 < buffer.size() && buffer[i + 1] != '\n') return false;
    }
    return true;
}

}  // namespace

Parser::Parser(std::size_t body_limit) : body_limit_(body_limit) {}

ParseState Parser::fail(int status, std::string message) {
    error_.status = status;
    error_.message = std::move(message);
    state_ = ParseState::Failed;
    return state_;
}

ParseState Parser::feed(std::string_view bytes) {
    if (state_ != ParseState::NeedMore) return state_;
    buffer_.append(bytes.data(), bytes.size());
    return parse_available();
}

ParseState Parser::finish() {
    if (state_ == ParseState::NeedMore) {
        return fail(400, "connection closed before the HTTP request was complete");
    }
    return state_;
}

ParseState Parser::parse_available() {
    if (!valid_line_endings(buffer_)) return fail(400, "HTTP request must use CRLF line endings");

    if (header_end_ == std::string::npos) {
        const std::size_t first_line = buffer_.find("\r\n");
        if (first_line == std::string::npos && buffer_.size() > Limits::request_line_bytes) {
            return fail(414, "HTTP request line exceeds 8 KiB");
        }
        if (first_line != std::string::npos && first_line > Limits::request_line_bytes) {
            return fail(414, "HTTP request line exceeds 8 KiB");
        }
        header_end_ = buffer_.find("\r\n\r\n");
        if (header_end_ == std::string::npos) {
            if (buffer_.size() > Limits::header_bytes + Limits::request_line_bytes) {
                return fail(431, "HTTP request headers exceed 32 KiB");
            }
            return state_;
        }
        if (header_end_ + 4U > Limits::header_bytes + Limits::request_line_bytes) {
            return fail(431, "HTTP request headers exceed 32 KiB");
        }

        const std::string request_line = buffer_.substr(0, first_line);
        const std::size_t first_space = request_line.find(' ');
        const std::size_t second_space = request_line.find(' ', first_space + 1);
        if (first_space == std::string::npos || second_space == std::string::npos ||
            request_line.find(' ', second_space + 1) != std::string::npos) {
            return fail(400, "malformed HTTP request line");
        }
        request_.method = request_line.substr(0, first_space);
        for (unsigned char c : request_.method) {
            if (!token_char(c)) return fail(400, "invalid HTTP method token");
        }
        request_.target = request_line.substr(first_space + 1, second_space - first_space - 1);
        if (request_line.substr(second_space + 1) != "HTTP/1.1") {
            return fail(505, "only HTTP/1.1 is supported");
        }
        if (!valid_target(request_.target, request_.path, request_.query)) {
            return fail(400, "HTTP target must be an unambiguous origin-form path");
        }

        std::size_t header_count = 0;
        std::size_t cursor = first_line + 2;
        bool content_length_seen = false;
        std::size_t content_length = 0;
        while (cursor < header_end_) {
            const std::size_t end = buffer_.find("\r\n", cursor);
            if (end == std::string::npos || end > header_end_) return fail(400, "malformed headers");
            const std::string line = buffer_.substr(cursor, end - cursor);
            if (line.empty() || line.front() == ' ' || line.front() == '\t') {
                return fail(400, "obsolete folded or empty HTTP header is not accepted");
            }
            if (++header_count > Limits::header_count) return fail(431, "too many HTTP headers");
            const std::size_t colon = line.find(':');
            if (colon == std::string::npos || colon == 0) return fail(400, "malformed HTTP header");
            std::string name = line.substr(0, colon);
            for (unsigned char c : name) {
                if (!token_char(c)) return fail(400, "invalid HTTP header name");
            }
            name = ascii_lower(name);
            std::string value = ascii_trim(line.substr(colon + 1));
            for (unsigned char c : value) {
                if ((c < 0x20U && c != '\t') || c == 0x7fU) {
                    return fail(400, "invalid control byte in HTTP header value");
                }
            }
            if (name == "transfer-encoding") {
                return fail(501, "Transfer-Encoding is not supported; send one Content-Length");
            }
            if (name == "content-length") {
                if (content_length_seen || value.empty() ||
                    !std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); })) {
                    return fail(400, "Content-Length must appear once as decimal digits");
                }
                content_length_seen = true;
                std::size_t parsed = 0;
                for (unsigned char c : value) {
                    const unsigned digit = c - '0';
                    if (parsed > (std::numeric_limits<std::size_t>::max() - digit) / 10U) {
                        return fail(413, "Content-Length is too large");
                    }
                    parsed = parsed * 10U + digit;
                }
                content_length = parsed;
            }
            if (request_.headers.find(name) != request_.headers.end()) {
                return fail(400, "duplicate HTTP header is not accepted: " + name);
            }
            request_.headers.emplace(std::move(name), std::move(value));
            cursor = end + 2;
        }
        if (request_.headers.find("host") == request_.headers.end()) {
            return fail(400, "HTTP/1.1 Host header is required");
        }
        if (content_length > body_limit_) return fail(413, "HTTP request body exceeds its route limit");
        expected_size_ = header_end_ + 4U + content_length;
        const auto connection = request_.headers.find("connection");
        request_.keep_alive = connection == request_.headers.end() ||
                              ascii_lower(connection->second) != "close";
    }

    if (buffer_.size() < expected_size_) return state_;
    request_.body.assign(buffer_.data() + header_end_ + 4U, expected_size_ - header_end_ - 4U);
    state_ = ParseState::Complete;
    return state_;
}

std::string Parser::take_remaining() {
    if (state_ != ParseState::Complete || buffer_.size() <= expected_size_) return {};
    return buffer_.substr(expected_size_);
}

}  // namespace ainiux::server::http
