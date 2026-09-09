#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common.hpp"
#include "pdf/limits.hpp"

namespace ainiux::pdf {

enum class TokenKind {
    Eof,
    Number,
    Keyword,
    Name,
    String,
    Hex,
    DictStart,
    DictEnd,
    ArrayStart,
    ArrayEnd,
};

struct Token {
    TokenKind kind = TokenKind::Eof;
    std::string text;
};

struct Cursor {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t pos = 0;

    bool at_end() const { return pos >= size; }
    int peek() const {
        if (pos >= size) {
            return -1;
        }
        return data[pos];
    }
    int peek_at(std::size_t offset) const {
        if (pos + offset >= size) {
            return -1;
        }
        return data[pos + offset];
    }
    int get() {
        if (pos >= size) {
            return -1;
        }
        return data[pos++];
    }
    void unget() {
        if (pos > 0) {
            --pos;
        }
    }
    std::size_t tell() const { return pos; }
    bool seek(std::size_t absolute) {
        if (absolute > size) {
            return false;
        }
        pos = absolute;
        return true;
    }
};

class Tokenizer {
   public:
    Tokenizer() = default;
    explicit Tokenizer(Cursor cursor) : cursor_(cursor) {}

    Cursor& cursor() { return cursor_; }
    const Cursor& cursor() const { return cursor_; }

    void push(Token token);
    Error next(Token& token);

   private:
    Error read(Token& token);

    Cursor cursor_;
    std::vector<Token> stack_;
};

bool pdf_isspace(int ch);
bool pdf_isdigit(int ch);

}  // namespace ainiux::pdf
