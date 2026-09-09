#include "pdf/token.hpp"

#include <cctype>

namespace ainiux::pdf {
namespace {

void append_utf8(std::string& out, unsigned int cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string utf16_to_utf8(const std::string& bytes) {
    if (bytes.size() < 2) {
        return bytes;
    }
    const bool le = static_cast<unsigned char>(bytes[0]) == 0xFF &&
                    static_cast<unsigned char>(bytes[1]) == 0xFE;
    const bool be = static_cast<unsigned char>(bytes[0]) == 0xFE &&
                    static_cast<unsigned char>(bytes[1]) == 0xFF;
    if (!le && !be) {
        return bytes;
    }
    std::string out;
    out.reserve(bytes.size());
    for (std::size_t i = 2; i + 1 < bytes.size(); i += 2) {
        unsigned int unit = le ? (static_cast<unsigned char>(bytes[i]) |
                                  (static_cast<unsigned char>(bytes[i + 1]) << 8))
                               : ((static_cast<unsigned char>(bytes[i]) << 8) |
                                  static_cast<unsigned char>(bytes[i + 1]));
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 3 < bytes.size()) {
            unsigned int low = le ? (static_cast<unsigned char>(bytes[i + 2]) |
                                     (static_cast<unsigned char>(bytes[i + 3]) << 8))
                                  : ((static_cast<unsigned char>(bytes[i + 2]) << 8) |
                                     static_cast<unsigned char>(bytes[i + 3]));
            if (low >= 0xDC00 && low <= 0xDFFF) {
                unit = 0x10000 + (((unit - 0xD800) << 10) | (low - 0xDC00));
                i += 2;
            }
        }
        if (unit != 0) {
            append_utf8(out, unit);
        }
    }
    return out;
}

int hex_nibble(int ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

bool is_delim(int ch) {
    return ch == '<' || ch == '>' || ch == '(' || ch == ')' || ch == '{' || ch == '}' || ch == '[' ||
           ch == ']' || ch == '/' || ch == '%';
}

}  // namespace

bool pdf_isspace(int ch) {
    return ch == 0 || ch == 9 || ch == 10 || ch == 12 || ch == 13 || ch == 32;
}

bool pdf_isdigit(int ch) {
    return ch >= '0' && ch <= '9';
}

void Tokenizer::push(Token token) {
    if (stack_.size() < 4) {
        stack_.push_back(std::move(token));
    }
}

Error Tokenizer::next(Token& token) {
    if (!stack_.empty()) {
        token = std::move(stack_.back());
        stack_.pop_back();
        return ok_error();
    }
    return read(token);
}

Error Tokenizer::read(Token& token) {
    token = Token{};
    std::size_t skipped = 0;
    int ch = 0;
    while ((ch = cursor_.get()) != -1) {
        ++skipped;
        if (ch == '%') {
            while ((ch = cursor_.get()) != -1) {
                ++skipped;
                if (ch == '\n' || ch == '\r') {
                    break;
                }
                if (skipped > kMaxWhitespaceRun) {
                    return {ErrorCode::FileRead, "PDF comment too long"};
                }
            }
            continue;
        }
        if (!pdf_isspace(ch)) {
            break;
        }
        if (skipped > kMaxWhitespaceRun) {
            return {ErrorCode::FileRead, "too much whitespace in PDF"};
        }
    }
    if (ch == -1) {
        token.kind = TokenKind::Eof;
        return ok_error();
    }

    if (ch == '[') {
        token.kind = TokenKind::ArrayStart;
        token.text = "[";
        return ok_error();
    }
    if (ch == ']') {
        token.kind = TokenKind::ArrayEnd;
        token.text = "]";
        return ok_error();
    }
    if (ch == '(') {
        token.kind = TokenKind::String;
        int parens = 0;
        std::string raw;
        raw.reserve(32);
        while ((ch = cursor_.get()) != -1) {
            if (ch == '\\') {
                const int esc = cursor_.get();
                if (esc == -1) {
                    break;
                }
                if (esc >= '0' && esc <= '7') {
                    int value = esc - '0';
                    for (int i = 0; i < 2; ++i) {
                        const int next = cursor_.peek();
                        if (next < '0' || next > '7') {
                            break;
                        }
                        value = (value << 3) | (cursor_.get() - '0');
                    }
                    raw.push_back(static_cast<char>(value));
                } else if (esc == 'n') {
                    raw.push_back('\n');
                } else if (esc == 'r') {
                    raw.push_back('\r');
                } else if (esc == 't') {
                    raw.push_back('\t');
                } else if (esc == 'b') {
                    raw.push_back('\b');
                } else if (esc == 'f') {
                    raw.push_back('\f');
                } else if (esc == '\n' || esc == '\r') {
                    if (esc == '\r' && cursor_.peek() == '\n') {
                        cursor_.get();
                    }
                } else {
                    raw.push_back(static_cast<char>(esc));
                }
            } else if (ch == '(') {
                ++parens;
                raw.push_back('(');
            } else if (ch == ')') {
                if (parens == 0) {
                    if (raw.size() >= 2) {
                        const auto b0 = static_cast<unsigned char>(raw[0]);
                        const auto b1 = static_cast<unsigned char>(raw[1]);
                        if ((b0 == 0xFE && b1 == 0xFF) || (b0 == 0xFF && b1 == 0xFE)) {
                            raw = utf16_to_utf8(raw);
                        }
                    }
                    token.text = std::move(raw);
                    return ok_error();
                }
                --parens;
                raw.push_back(')');
            } else {
                raw.push_back(static_cast<char>(ch));
            }
            if (raw.size() > kMaxTokenBytes) {
                return {ErrorCode::FileRead, "PDF string token too large"};
            }
        }
        return {ErrorCode::FileRead, "unterminated PDF string literal"};
    }
    if (ch == '/') {
        token.kind = TokenKind::Name;
        while ((ch = cursor_.peek()) != -1 && !pdf_isspace(ch) && !is_delim(ch)) {
            cursor_.get();
            if (ch == '#') {
                const int h1 = cursor_.get();
                const int h2 = cursor_.get();
                const int n1 = hex_nibble(h1);
                const int n2 = hex_nibble(h2);
                if (n1 < 0 || n2 < 0) {
                    return {ErrorCode::FileRead, "bad # escape in PDF name"};
                }
                token.text.push_back(static_cast<char>((n1 << 4) | n2));
            } else {
                token.text.push_back(static_cast<char>(ch));
            }
            if (token.text.size() > kMaxTokenBytes) {
                return {ErrorCode::FileRead, "PDF name token too large"};
            }
        }
        return ok_error();
    }
    if (ch == '<') {
        const int next = cursor_.peek();
        if (next == '<') {
            cursor_.get();
            token.kind = TokenKind::DictStart;
            token.text = "<<";
            return ok_error();
        }
        token.kind = TokenKind::Hex;
        bool high = true;
        int acc = 0;
        while ((ch = cursor_.get()) != -1) {
            if (ch == '>') {
                if (!high) {
                    token.text.push_back(static_cast<char>(acc));
                }
                return ok_error();
            }
            if (pdf_isspace(ch)) {
                continue;
            }
            const int n = hex_nibble(ch);
            if (n < 0) {
                return {ErrorCode::FileRead, "invalid hex string character in PDF"};
            }
            if (high) {
                acc = n << 4;
                high = false;
            } else {
                token.text.push_back(static_cast<char>(acc | n));
                high = true;
            }
            if (token.text.size() > kMaxTokenBytes) {
                return {ErrorCode::FileRead, "PDF hex string too large"};
            }
        }
        return {ErrorCode::FileRead, "unterminated PDF hex string"};
    }
    if (ch == '>') {
        if (cursor_.peek() == '>') {
            cursor_.get();
            token.kind = TokenKind::DictEnd;
            token.text = ">>";
            return ok_error();
        }
        token.kind = TokenKind::Keyword;
        token.text = ">";
        return ok_error();
    }

    const bool number = (ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '.';
    token.kind = number ? TokenKind::Number : TokenKind::Keyword;
    token.text.push_back(static_cast<char>(ch));
    while ((ch = cursor_.peek()) != -1 && !pdf_isspace(ch) && !is_delim(ch)) {
        if (number && !pdf_isdigit(ch) && ch != '.' && ch != 'e' && ch != 'E' && ch != '+' && ch != '-') {
            break;
        }
        token.text.push_back(static_cast<char>(cursor_.get()));
        if (token.text.size() > kMaxTokenBytes) {
            return {ErrorCode::FileRead, "PDF token too large"};
        }
    }
    return ok_error();
}

}  // namespace ainiux::pdf
