#include "json/json.hpp"

#include <cerrno>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <utility>

namespace pkchat::json {

const Value* Value::get(const std::string& key) const {
    if (type != Type::Object) {
        return nullptr;
    }
    const auto it = object.find(key);
    if (it == object.end()) {
        return nullptr;
    }
    return &it->second;
}

const Value* Value::at(size_t index) const {
    if (type != Type::Array || index >= array.size()) {
        return nullptr;
    }
    return &array[index];
}

namespace {

class Parser {
   public:
    explicit Parser(const std::string& input, size_t offset = 0) : input_(input), pos_(offset) {}

    ParseResult run() {
        skip_ws();
        Value value;
        Error err = parse_value(value);
        if (!err.ok()) {
            return {Value{}, err};
        }
        skip_ws();
        if (pos_ != input_.size()) {
            return {Value{}, fail("unexpected trailing JSON data")};
        }
        return {value, ok_error()};
    }

    ParsePrefixResult run_prefix() {
        skip_ws();
        Value value;
        Error err = parse_value(value);
        if (!err.ok()) {
            return {Value{}, err, pos_};
        }
        skip_ws();
        return {std::move(value), ok_error(), pos_};
    }

   private:
    const std::string& input_;
    size_t pos_ = 0;

    Error fail(const std::string& detail) const {
        return {ErrorCode::JsonParse, "JSON parse error at byte " + std::to_string(pos_) + ": " + detail};
    }

    void skip_ws() {
        while (pos_ < input_.size()) {
            const char ch = input_[pos_];
            if (ch != ' ' && ch != '\n' && ch != '\r' && ch != '\t') {
                break;
            }
            ++pos_;
        }
    }

    bool consume(char expected) {
        if (pos_ < input_.size() && input_[pos_] == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    Error parse_value(Value& out) {
        skip_ws();
        if (pos_ >= input_.size()) {
            return fail("unexpected end of input");
        }
        const char ch = input_[pos_];
        if (ch == 'n') {
            return parse_literal("null", Value::Type::Null, out);
        }
        if (ch == 't') {
            Error err = parse_literal("true", Value::Type::Bool, out);
            out.boolean = true;
            return err;
        }
        if (ch == 'f') {
            Error err = parse_literal("false", Value::Type::Bool, out);
            out.boolean = false;
            return err;
        }
        if (ch == '"') {
            out.type = Value::Type::String;
            return parse_string(out.string);
        }
        if (ch == '[') {
            return parse_array(out);
        }
        if (ch == '{') {
            return parse_object(out);
        }
        if (ch == '-' || (ch >= '0' && ch <= '9')) {
            return parse_number(out);
        }
        return fail("unexpected character");
    }

    Error parse_literal(const char* literal, Value::Type type, Value& out) {
        const std::string text(literal);
        if (input_.compare(pos_, text.size(), text) != 0) {
            return fail("invalid literal");
        }
        pos_ += text.size();
        out.type = type;
        return ok_error();
    }

    static void append_utf8(std::string& out, unsigned int cp) {
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

    Error parse_hex4(unsigned int& cp) {
        cp = 0;
        if (pos_ + 4 > input_.size()) {
            return fail("incomplete unicode escape");
        }
        for (int i = 0; i < 4; ++i) {
            const char ch = input_[pos_++];
            cp <<= 4;
            if (ch >= '0' && ch <= '9') {
                cp += static_cast<unsigned int>(ch - '0');
            } else if (ch >= 'a' && ch <= 'f') {
                cp += static_cast<unsigned int>(ch - 'a' + 10);
            } else if (ch >= 'A' && ch <= 'F') {
                cp += static_cast<unsigned int>(ch - 'A' + 10);
            } else {
                return fail("invalid unicode escape");
            }
        }
        return ok_error();
    }

    Error parse_string(std::string& out) {
        if (!consume('"')) {
            return fail("expected string");
        }
        while (pos_ < input_.size()) {
            const unsigned char ch = static_cast<unsigned char>(input_[pos_++]);
            if (ch == '"') {
                return ok_error();
            }
            if (ch < 0x20) {
                return fail("unescaped control character in string");
            }
            if (ch != '\\') {
                out.push_back(static_cast<char>(ch));
                continue;
            }
            if (pos_ >= input_.size()) {
                return fail("incomplete escape");
            }
            const char esc = input_[pos_++];
            switch (esc) {
                case '"':
                case '\\':
                case '/':
                    out.push_back(esc);
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u': {
                    unsigned int cp = 0;
                    Error err = parse_hex4(cp);
                    if (!err.ok()) {
                        return err;
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (pos_ + 2 > input_.size() || input_[pos_++] != '\\' || input_[pos_++] != 'u') {
                            return fail("missing low surrogate");
                        }
                        unsigned int low = 0;
                        err = parse_hex4(low);
                        if (!err.ok()) {
                            return err;
                        }
                        if (low < 0xDC00 || low > 0xDFFF) {
                            return fail("invalid low surrogate");
                        }
                        cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail("unexpected low surrogate");
                    }
                    append_utf8(out, cp);
                    break;
                }
                default:
                    return fail("invalid escape");
            }
        }
        return fail("unterminated string");
    }

    Error parse_number(Value& out) {
        const size_t start = pos_;
        if (consume('-') && pos_ >= input_.size()) {
            return fail("incomplete number");
        }
        if (consume('0')) {
        } else {
            if (pos_ >= input_.size() || input_[pos_] < '1' || input_[pos_] > '9') {
                return fail("invalid number");
            }
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') {
                ++pos_;
            }
        }
        if (consume('.')) {
            if (pos_ >= input_.size() || input_[pos_] < '0' || input_[pos_] > '9') {
                return fail("invalid fractional number");
            }
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') {
                ++pos_;
            }
        }
        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) {
                ++pos_;
            }
            if (pos_ >= input_.size() || input_[pos_] < '0' || input_[pos_] > '9') {
                return fail("invalid exponent");
            }
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9') {
                ++pos_;
            }
        }
        errno = 0;
        char* end = nullptr;
        const std::string token = input_.substr(start, pos_ - start);
        const double parsed = std::strtod(token.c_str(), &end);
        if (errno != 0 || end == token.c_str()) {
            return fail("number conversion failed");
        }
        out.type = Value::Type::Number;
        out.number = parsed;
        return ok_error();
    }

    Error parse_array(Value& out) {
        consume('[');
        out.type = Value::Type::Array;
        skip_ws();
        if (consume(']')) {
            return ok_error();
        }
        while (true) {
            Value item;
            Error err = parse_value(item);
            if (!err.ok()) {
                return err;
            }
            out.array.push_back(std::move(item));
            skip_ws();
            if (consume(']')) {
                return ok_error();
            }
            if (!consume(',')) {
                return fail("expected comma or array end");
            }
        }
    }

    Error parse_object(Value& out) {
        consume('{');
        out.type = Value::Type::Object;
        skip_ws();
        if (consume('}')) {
            return ok_error();
        }
        while (true) {
            std::string key;
            Error err = parse_string(key);
            if (!err.ok()) {
                return err;
            }
            skip_ws();
            if (!consume(':')) {
                return fail("expected object colon");
            }
            Value item;
            err = parse_value(item);
            if (!err.ok()) {
                return err;
            }
            out.object.emplace(std::move(key), std::move(item));
            skip_ws();
            if (consume('}')) {
                return ok_error();
            }
            if (!consume(',')) {
                return fail("expected comma or object end");
            }
            skip_ws();
        }
    }
};

}  // namespace

ParseResult parse(const std::string& input) { return Parser(input).run(); }

ParsePrefixResult parse_prefix(const std::string& input, size_t offset) {
    return Parser(input, offset).run_prefix();
}

std::string escape_string(const std::string& input) {
    std::ostringstream out;
    for (unsigned char ch : input) {
        switch (ch) {
            case '"':
                out << "\\\"";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '\b':
                out << "\\b";
                break;
            case '\f':
                out << "\\f";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                if (ch < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch);
                } else {
                    out << static_cast<char>(ch);
                }
                break;
        }
    }
    return out.str();
}

std::string quote(const std::string& input) { return "\"" + escape_string(input) + "\""; }

}  // namespace pkchat::json
