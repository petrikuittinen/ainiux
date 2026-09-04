#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

#include "server/limits.hpp"

namespace ainiux::server::http {

struct Request {
    std::string method;
    std::string target;
    std::string path;
    std::string query;
    std::string body;
    std::unordered_map<std::string, std::string> headers;
    bool keep_alive = true;
};

struct ParseError {
    int status = 400;
    std::string message;
};

enum class ParseState { NeedMore, Complete, Failed };

// Incremental parser for Ainiux's deliberately small HTTP/1.1 subset. A parser
// owns only one request; bytes after it are returned by take_remaining().
class Parser {
   public:
    explicit Parser(std::size_t body_limit = Limits::json_body_bytes);

    ParseState feed(std::string_view bytes);
    ParseState finish();
    const Request& request() const { return request_; }
    const ParseError& error() const { return error_; }
    bool headers_complete() const { return header_end_ != std::string::npos; }
    std::size_t content_length() const { return content_length_; }
    std::string take_remaining();

   private:
    ParseState parse_available();
    ParseState fail(int status, std::string message);

    std::size_t body_limit_;
    std::string buffer_;
    std::size_t header_end_ = std::string::npos;
    std::size_t expected_size_ = 0;
    std::size_t content_length_ = 0;
    Request request_;
    ParseError error_;
    ParseState state_ = ParseState::NeedMore;
};

}  // namespace ainiux::server::http
