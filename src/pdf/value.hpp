#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "common.hpp"
#include "pdf/token.hpp"

namespace ainiux::pdf {

struct Ref {
    std::uint32_t number = 0;
    std::uint16_t generation = 0;
};

enum class ValueType {
    Null,
    Bool,
    Number,
    Name,
    String,
    Binary,
    Ref,
    Array,
    Dict,
};

struct Value {
    ValueType type = ValueType::Null;
    bool boolean = false;
    double number = 0;
    std::string text;
    std::vector<std::uint8_t> bytes;
    Ref ref;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> dict;
};

const Value* dict_get(const Value& dict, const char* key);
bool dict_name(const Value& dict, const char* key, std::string& out);
bool dict_number(const Value& dict, const char* key, double& out);
bool dict_int(const Value& dict, const char* key, std::int64_t& out);
bool dict_bool(const Value& dict, const char* key, bool& out);
const Value* dict_dict(const Value& dict, const char* key);
const Value* dict_array(const Value& dict, const char* key);
bool dict_ref(const Value& dict, const char* key, Ref& out);

Error read_value(Tokenizer& tokens, Value& value, int depth);

}  // namespace ainiux::pdf
