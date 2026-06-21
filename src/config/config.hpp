#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

#include "common.hpp"

namespace pkchat::config {

constexpr size_t kMaxConfigBytes = 1024 * 1024;

struct SourceLocation {
    std::string path;
    size_t line = 1;
    size_t column = 1;
};

struct Value {
    enum class Type { Boolean, Integer, Float, String };

    Type type = Type::String;
    bool boolean = false;
    std::int64_t integer = 0;
    double floating = 0.0;
    std::string string;

    bool is_boolean() const { return type == Type::Boolean; }
    bool is_integer() const { return type == Type::Integer; }
    bool is_float() const { return type == Type::Float; }
    bool is_string() const { return type == Type::String; }
};

struct Entry {
    std::string section;
    std::string key;
    Value value;
    SourceLocation source;
};

struct Document {
    std::map<std::string, Entry> entries;

    const Entry* find(const std::string& qualified_key) const;
    const Entry* find(const std::string& section, const std::string& key) const;
};

struct ParseResult {
    Document document;
    Error error;
};

ParseResult parse(const std::string& input, const std::string& source_path = "<memory>");
ParseResult read_file(const std::string& path, size_t max_bytes = kMaxConfigBytes);
const char* value_type_name(Value::Type type);

}  // namespace pkchat::config
