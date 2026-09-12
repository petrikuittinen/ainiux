#include "pdf/value.hpp"

#include "pdf/limits.hpp"

#include <cmath>
#include <charconv>
#include <cstdlib>

namespace ainiux::pdf {
namespace {

bool parse_number_text(const std::string& text, double& out) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    out = std::strtod(text.c_str(), &end);
    return end != text.c_str() && *end == '\0' && std::isfinite(out);
}

Error read_array(Tokenizer& tokens, Value& value, int depth);
Error read_dict(Tokenizer& tokens, Value& value, int depth);

}  // namespace

const Value* dict_get(const Value& dict, const char* key) {
    if (dict.type != ValueType::Dict || key == nullptr) {
        return nullptr;
    }
    for (const auto& pair : dict.dict) {
        if (pair.first == key) {
            return &pair.second;
        }
    }
    return nullptr;
}

bool dict_name(const Value& dict, const char* key, std::string& out) {
    const Value* value = dict_get(dict, key);
    if (value == nullptr || value->type != ValueType::Name) {
        return false;
    }
    out = value->text;
    return true;
}

bool dict_number(const Value& dict, const char* key, double& out) {
    const Value* value = dict_get(dict, key);
    if (value == nullptr || value->type != ValueType::Number) {
        return false;
    }
    out = value->number;
    return true;
}

bool number_to_integer(double number, std::int64_t& out) {
    // INT64_MAX rounds up to 2^63 as a double, so the upper bound is exclusive.
    // These comparisons also reject NaN and both infinities.
    if (!(number >= -0x1p63 && number < 0x1p63)) {
        return false;
    }
    const auto integer = static_cast<std::int64_t>(number);
    if (static_cast<double>(integer) != number) {
        return false;
    }
    out = integer;
    return true;
}

bool parse_unsigned_integer(std::string_view text, std::uint64_t limit, std::uint64_t& out) {
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return false;
    }
    std::uint64_t integer = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), integer);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || integer > limit) {
        return false;
    }
    out = integer;
    return true;
}

bool dict_int(const Value& dict, const char* key, std::int64_t& out) {
    double number = 0;
    if (!dict_number(dict, key, number)) {
        return false;
    }
    return number_to_integer(number, out);
}

bool dict_bool(const Value& dict, const char* key, bool& out) {
    const Value* value = dict_get(dict, key);
    if (value == nullptr || value->type != ValueType::Bool) {
        return false;
    }
    out = value->boolean;
    return true;
}

const Value* dict_dict(const Value& dict, const char* key) {
    const Value* value = dict_get(dict, key);
    if (value == nullptr || value->type != ValueType::Dict) {
        return nullptr;
    }
    return value;
}

const Value* dict_array(const Value& dict, const char* key) {
    const Value* value = dict_get(dict, key);
    if (value == nullptr || value->type != ValueType::Array) {
        return nullptr;
    }
    return value;
}

bool dict_ref(const Value& dict, const char* key, Ref& out) {
    const Value* value = dict_get(dict, key);
    if (value == nullptr || value->type != ValueType::Ref) {
        return false;
    }
    out = value->ref;
    return true;
}

Error read_value(Tokenizer& tokens, Value& value, int depth) {
    if (depth > static_cast<int>(kMaxDepth)) {
        return {ErrorCode::FileRead, "PDF value nested too deeply"};
    }
    Token token;
    Error err = tokens.next(token);
    if (!err.ok()) {
        return err;
    }
    value = Value{};
    switch (token.kind) {
        case TokenKind::Eof:
            return {ErrorCode::FileRead, "unexpected end of PDF while reading a value"};
        case TokenKind::DictStart:
            return read_dict(tokens, value, depth + 1);
        case TokenKind::ArrayStart:
            return read_array(tokens, value, depth + 1);
        case TokenKind::Name:
            value.type = ValueType::Name;
            value.text = std::move(token.text);
            return ok_error();
        case TokenKind::String:
            value.type = ValueType::String;
            value.text = std::move(token.text);
            return ok_error();
        case TokenKind::Hex:
            value.type = ValueType::Binary;
            value.bytes.assign(token.text.begin(), token.text.end());
            return ok_error();
        case TokenKind::Keyword:
            if (token.text == "true" || token.text == "false") {
                value.type = ValueType::Bool;
                value.boolean = token.text == "true";
                return ok_error();
            }
            if (token.text == "null") {
                value.type = ValueType::Null;
                return ok_error();
            }
            return {ErrorCode::FileRead, "unexpected PDF token '" + token.text + "'"};
        case TokenKind::Number: {
            Token generation;
            err = tokens.next(generation);
            if (!err.ok()) {
                return err;
            }
            if (generation.kind == TokenKind::Number) {
                Token r;
                err = tokens.next(r);
                if (!err.ok()) {
                    return err;
                }
                if (r.kind == TokenKind::Keyword && r.text == "R") {
                    // References use integer syntax. Parse directly to avoid
                    // both floating-point rounding and two strtod calls.
                    std::uint64_t n = 0;
                    std::uint64_t g = 0;
                    if (!parse_unsigned_integer(token.text, UINT32_MAX, n) || n == 0 ||
                        !parse_unsigned_integer(generation.text, UINT16_MAX, g)) {
                        return {ErrorCode::FileRead, "invalid PDF indirect reference"};
                    }
                    value.type = ValueType::Ref;
                    value.ref.number = static_cast<std::uint32_t>(n);
                    value.ref.generation = static_cast<std::uint16_t>(g);
                    return ok_error();
                }
                tokens.push(std::move(r));
                tokens.push(std::move(generation));
            } else {
                tokens.push(std::move(generation));
            }
            value.type = ValueType::Number;
            if (!parse_number_text(token.text, value.number)) {
                return {ErrorCode::FileRead, "invalid PDF number '" + token.text + "'"};
            }
            return ok_error();
        }
        default:
            return {ErrorCode::FileRead, "unexpected PDF token"};
    }
}

namespace {

Error read_array(Tokenizer& tokens, Value& value, int depth) {
    value.type = ValueType::Array;
    for (;;) {
        Token token;
        Error err = tokens.next(token);
        if (!err.ok()) {
            return err;
        }
        if (token.kind == TokenKind::ArrayEnd) {
            return ok_error();
        }
        if (token.kind == TokenKind::Eof) {
            return {ErrorCode::FileRead, "unterminated PDF array"};
        }
        tokens.push(std::move(token));
        Value item;
        err = read_value(tokens, item, depth);
        if (!err.ok()) {
            return err;
        }
        value.array.push_back(std::move(item));
    }
}

Error read_dict(Tokenizer& tokens, Value& value, int depth) {
    value.type = ValueType::Dict;
    for (;;) {
        Token key;
        Error err = tokens.next(key);
        if (!err.ok()) {
            return err;
        }
        if (key.kind == TokenKind::DictEnd) {
            return ok_error();
        }
        if (key.kind != TokenKind::Name) {
            return {ErrorCode::FileRead, "PDF dictionary key is not a name"};
        }
        Value item;
        err = read_value(tokens, item, depth);
        if (!err.ok()) {
            return {ErrorCode::FileRead, "missing value for PDF dictionary key '/" + key.text + "'"};
        }
        bool exists = false;
        for (const auto& pair : value.dict) {
            if (pair.first == key.text) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            value.dict.emplace_back(std::move(key.text), std::move(item));
        }
    }
}

}  // namespace

}  // namespace ainiux::pdf
