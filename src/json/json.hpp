#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"

namespace ainiux::json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> array;
    std::map<std::string, Value> object;

    bool is_null() const { return type == Type::Null; }
    bool is_string() const { return type == Type::String; }
    bool is_array() const { return type == Type::Array; }
    bool is_object() const { return type == Type::Object; }

    const Value* get(const std::string& key) const;
    const Value* at(size_t index) const;
};

struct ParseResult {
    Value value;
    Error error;
};

struct ParsePrefixResult {
    Value value;
    Error error;
    size_t consumed = 0;
};

ParseResult parse(const std::string& input);
ParsePrefixResult parse_prefix(const std::string& input, size_t offset = 0);
std::string stringify(const Value& value);
std::string escape_string(const std::string& input);
std::string quote(const std::string& input);

// Token-copying pretty printer: preserves object key order and number lexemes.
// Does not round-trip through Value (which sorts keys and stores numbers as double).
Error pretty_print(std::string_view input, std::string& output);
// Pretty-print a single JSON value and wrap it in a ```json fence for Markdown.
Error to_markdown_bytes(std::string_view utf8, std::string& markdown);

}  // namespace ainiux::json
