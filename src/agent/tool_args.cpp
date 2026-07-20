#include "agent/tool_args.hpp"

#include <cctype>
#include <cstdlib>

namespace ainiux::agent {
namespace {

json::Value object_value() {
    json::Value value;
    value.type = json::Value::Type::Object;
    return value;
}

json::Value string_value(const std::string& text) {
    json::Value value;
    value.type = json::Value::Type::String;
    value.string = text;
    return value;
}

json::Value bool_value(bool boolean) {
    json::Value value;
    value.type = json::Value::Type::Bool;
    value.boolean = boolean;
    return value;
}

json::Value number_value(double number) {
    json::Value value;
    value.type = json::Value::Type::Number;
    value.number = number;
    return value;
}

json::Value array_value() {
    json::Value value;
    value.type = json::Value::Type::Array;
    return value;
}

std::string trim_ws(std::string text) {
    std::size_t begin = 0;
    while (begin < text.size() &&
           (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\n' ||
            text[begin] == '\r' || text[begin] == '\f' || text[begin] == '\v'))
        ++begin;
    std::size_t end = text.size();
    while (end > begin &&
           (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\n' ||
            text[end - 1] == '\r' || text[end - 1] == '\f' || text[end - 1] == '\v'))
        --end;
    return text.substr(begin, end - begin);
}

// Strip a single leading UTF-8 BOM if present.
void strip_bom(std::string& text) {
    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        text.erase(0, 3);
}

// Strip one surrounding Markdown fence: ```json ... ``` or ``` ... ```.
std::string strip_code_fence(std::string text) {
    text = trim_ws(std::move(text));
    if (text.size() < 6 || text.compare(0, 3, "```") != 0) return text;
    std::size_t line_end = text.find('\n');
    if (line_end == std::string::npos) return text;
    std::string language = trim_ws(text.substr(3, line_end - 3));
    for (char& ch : language)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    // Accept bare fence or a small set of JSON language tags.
    if (!language.empty() && language != "json" && language != "js" &&
        language != "javascript")
        return text;
    std::size_t closer = text.rfind("```");
    if (closer == std::string::npos || closer <= line_end) return text;
    return trim_ws(text.substr(line_end + 1, closer - line_end - 1));
}

// Extract exactly one balanced top-level {...} if present with junk around it.
// Rejects when two complete top-level objects are found.
bool extract_one_object(const std::string& text, std::string& object_text) {
    const std::size_t start = text.find('{');
    if (start == std::string::npos) return false;
    int depth = 0;
    bool in_string = false;
    bool escape = false;
    std::size_t end = std::string::npos;
    for (std::size_t i = start; i < text.size(); ++i) {
        const char ch = text[i];
        if (in_string) {
            if (escape) escape = false;
            else if (ch == '\\') escape = true;
            else if (ch == '"') in_string = false;
            continue;
        }
        if (ch == '"') {
            in_string = true;
            continue;
        }
        if (ch == '{') {
            ++depth;
        } else if (ch == '}') {
            --depth;
            if (depth == 0) {
                end = i;
                break;
            }
            if (depth < 0) return false;
        }
    }
    if (end == std::string::npos) return false;
    // Reject a second complete top-level object after the first.
    const std::size_t next = text.find('{', end + 1);
    if (next != std::string::npos) {
        int next_depth = 0;
        bool next_in_string = false;
        bool next_escape = false;
        for (std::size_t i = next; i < text.size(); ++i) {
            const char ch = text[i];
            if (next_in_string) {
                if (next_escape) next_escape = false;
                else if (ch == '\\') next_escape = true;
                else if (ch == '"') next_in_string = false;
                continue;
            }
            if (ch == '"') {
                next_in_string = true;
                continue;
            }
            if (ch == '{') ++next_depth;
            else if (ch == '}') {
                --next_depth;
                if (next_depth == 0) return false;  // second complete object
                if (next_depth < 0) break;
            }
        }
    }
    object_text = text.substr(start, end - start + 1);
    return true;
}

// One-pass JSON repair for common model mistakes. Not iterative.
std::string repair_json_once(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8);
    bool in_string = false;
    bool escape = false;
    char string_quote = '"';
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char ch = input[i];
        if (in_string) {
            if (escape) {
                out.push_back(ch);
                escape = false;
                continue;
            }
            if (ch == '\\') {
                out.push_back(ch);
                escape = true;
                continue;
            }
            if (ch == string_quote) {
                out.push_back('"');
                in_string = false;
                continue;
            }
            if (string_quote == '"' && (ch == '\n' || ch == '\r')) {
                out += "\\n";
                if (ch == '\r' && i + 1 < input.size() && input[i + 1] == '\n') ++i;
                continue;
            }
            if (string_quote == '\'' && ch == '"') {
                out += "\\\"";
                continue;
            }
            out.push_back(ch);
            continue;
        }

        // Outside strings: single-quoted string open.
        if (ch == '\'' || ch == '"') {
            in_string = true;
            string_quote = ch;
            out.push_back('"');
            continue;
        }

        // Unquoted keys: identifier followed by optional space and ':'.
        if ((std::isalpha(static_cast<unsigned char>(ch)) || ch == '_' || ch == '$') &&
            (out.empty() || out.back() == '{' || out.back() == ',' || out.back() == ' ' ||
             out.back() == '\n' || out.back() == '\t')) {
            std::size_t j = i;
            while (j < input.size() &&
                   (std::isalnum(static_cast<unsigned char>(input[j])) ||
                    input[j] == '_' || input[j] == '$'))
                ++j;
            std::size_t k = j;
            while (k < input.size() &&
                   (input[k] == ' ' || input[k] == '\t' || input[k] == '\n' ||
                    input[k] == '\r'))
                ++k;
            if (k < input.size() && input[k] == ':') {
                out.push_back('"');
                out.append(input, i, j - i);
                out.push_back('"');
                i = j - 1;
                continue;
            }
        }

        // Trailing commas before } or ].
        if (ch == ',' ) {
            std::size_t k = i + 1;
            while (k < input.size() &&
                   (input[k] == ' ' || input[k] == '\t' || input[k] == '\n' ||
                    input[k] == '\r'))
                ++k;
            if (k < input.size() && (input[k] == '}' || input[k] == ']')) continue;
        }

        out.push_back(ch);
    }
    return out;
}

std::string to_lower(std::string text) {
    for (char& ch : text)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    return text;
}

// snake_case / camelCase / PascalCase normalization for comparison.
std::string normalize_identifier(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '-' || ch == ' ') {
            out.push_back('_');
            continue;
        }
        if (ch >= 'A' && ch <= 'Z') {
            if (!out.empty() && out.back() != '_') out.push_back('_');
            out.push_back(static_cast<char>(ch - 'A' + 'a'));
            continue;
        }
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')
            out.push_back(ch);
    }
    return out;
}

const json::Value* schema_type(const json::Value& schema) {
    if (!schema.is_object()) return nullptr;
    return schema.get("type");
}

bool type_is(const json::Value* type_node, const char* expected) {
    if (type_node == nullptr) return false;
    if (type_node->is_string()) return type_node->string == expected;
    if (type_node->is_array()) {
        for (const json::Value& item : type_node->array)
            if (item.is_string() && item.string == expected) return true;
    }
    return false;
}

bool parse_bool_string(const std::string& text, bool& out) {
    const std::string lower = to_lower(trim_ws(text));
    if (lower == "true" || lower == "1" || lower == "yes") {
        out = true;
        return true;
    }
    if (lower == "false" || lower == "0" || lower == "no") {
        out = false;
        return true;
    }
    return false;
}

bool parse_number_string(const std::string& text, double& out) {
    const std::string trimmed = trim_ws(text);
    if (trimmed.empty()) return false;
    char* end = nullptr;
    out = std::strtod(trimmed.c_str(), &end);
    return end != nullptr && end != trimmed.c_str() && *end == '\0';
}

std::string lowercase_ascii(std::string text) {
    for (char& ch : text)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    return text;
}

bool find_tag_content(const std::string& text, const std::string& tag,
                      std::size_t from, std::string& content, std::size_t& after) {
    const std::string open = "<" + tag + ">";
    const std::string close = "</" + tag + ">";
    const std::string lower = lowercase_ascii(text);
    const std::string open_l = lowercase_ascii(open);
    const std::string close_l = lowercase_ascii(close);
    const std::size_t start = lower.find(open_l, from);
    if (start == std::string::npos) return false;
    const std::size_t content_start = start + open.size();
    const std::size_t end = lower.find(close_l, content_start);
    if (end == std::string::npos) return false;
    content = text.substr(content_start, end - content_start);
    after = end + close.size();
    return true;
}

}  // namespace

std::string truncate_tool_arguments_for_error(const std::string& text, std::size_t cap) {
    if (text.size() <= cap) return text;
    std::size_t end = cap;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) --end;
    return text.substr(0, end);
}

ToolArgParseResult parse_tool_arguments(const std::string& arguments_text) {
    ToolArgParseResult result;
    result.original_arguments = arguments_text;
    result.error = ok_error();

    std::string text = arguments_text;
    strip_bom(text);
    text = strip_code_fence(trim_ws(std::move(text)));

    // Stage 2: empty / whitespace-only -> {}.
    if (text.empty()) {
        result.value = object_value();
        result.stage = ToolArgStage::EmptyObject;
        result.normalized_arguments = "{}";
        return result;
    }

    // Stage 3: strict JSON.
    {
        json::ParseResult parsed = json::parse(text);
        if (parsed.error.ok() && parsed.value.is_object()) {
            result.value = std::move(parsed.value);
            result.stage = ToolArgStage::StrictJson;
            result.normalized_arguments = text;
            return result;
        }
    }

    // Stage 4: extract one balanced top-level object.
    std::string extracted;
    if (extract_one_object(text, extracted)) {
        json::ParseResult parsed = json::parse(extracted);
        if (parsed.error.ok() && parsed.value.is_object()) {
            result.value = std::move(parsed.value);
            result.stage = ToolArgStage::ExtractedObject;
            result.normalized_arguments = extracted;
            return result;
        }
        // Stage 5: one-pass repair on the extracted object.
        const std::string repaired = repair_json_once(extracted);
        parsed = json::parse(repaired);
        if (parsed.error.ok() && parsed.value.is_object()) {
            result.value = std::move(parsed.value);
            result.stage = ToolArgStage::RepairedJson;
            result.normalized_arguments = repaired;
            return result;
        }
    }

    // Stage 5 on full text (no extraction).
    {
        const std::string repaired = repair_json_once(text);
        json::ParseResult parsed = json::parse(repaired);
        if (parsed.error.ok() && parsed.value.is_object()) {
            result.value = std::move(parsed.value);
            result.stage = ToolArgStage::RepairedJson;
            result.normalized_arguments = repaired;
            return result;
        }
    }

    result.error = {ErrorCode::JsonParse,
                    "tool arguments must be a JSON object (strict parse, "
                    "single-object extraction, and one-pass repair all failed)"};
    return result;
}

Error coerce_tool_arguments(json::Value& args_object,
                            const json::Value& schema_properties) {
    if (!args_object.is_object())
        return {ErrorCode::JsonParse, "tool arguments must be a JSON object"};
    if (!schema_properties.is_object()) return ok_error();

    for (auto& field : args_object.object) {
        const json::Value* property = schema_properties.get(field.first);
        if (property == nullptr || !property->is_object()) continue;
        const json::Value* type = schema_type(*property);
        json::Value& value = field.second;

        if (value.is_string()) {
            if (type_is(type, "boolean")) {
                bool parsed = false;
                if (parse_bool_string(value.string, parsed)) value = bool_value(parsed);
            } else if (type_is(type, "integer") || type_is(type, "number")) {
                double number = 0;
                if (parse_number_string(value.string, number)) {
                    if (type_is(type, "integer") &&
                        number != static_cast<double>(static_cast<long long>(number)))
                        continue;
                    value = number_value(number);
                }
            } else if (type_is(type, "array")) {
                json::Value array = array_value();
                array.array.push_back(value);
                value = std::move(array);
            }
        } else if (!value.is_array() && type_is(type, "array") &&
                   (value.type == json::Value::Type::Bool ||
                    value.type == json::Value::Type::Number ||
                    value.is_string() || value.is_object() || value.is_null())) {
            json::Value array = array_value();
            array.array.push_back(std::move(value));
            value = std::move(array);
        }
    }
    return ok_error();
}

std::string repair_tool_name(const std::string& requested,
                             const std::vector<std::string>& known_names) {
    if (requested.empty() || known_names.empty()) return {};
    for (const std::string& name : known_names)
        if (name == requested) return name;
    const std::string lower = to_lower(requested);
    for (const std::string& name : known_names)
        if (to_lower(name) == lower) return name;
    const std::string normalized = normalize_identifier(requested);
    for (const std::string& name : known_names)
        if (normalize_identifier(name) == normalized) return name;
    return {};
}

std::string invalid_arguments_tool_result(const std::string& tool_name,
                                          const std::string& message,
                                          const std::string& received_arguments) {
    json::Value root = object_value();
    root.object["ok"] = bool_value(false);
    json::Value error = object_value();
    error.object["code"] = string_value("invalid_arguments");
    error.object["message"] = string_value(message);
    root.object["error"] = std::move(error);
    json::Value data = object_value();
    data.object["tool"] = string_value(tool_name);
    data.object["received_arguments"] =
        string_value(truncate_tool_arguments_for_error(received_arguments));
    root.object["data"] = std::move(data);
    root.object["warnings"] = array_value();
    root.object["truncated"] = bool_value(false);
    root.object["metadata"] = object_value();
    return json::stringify(root);
}

XmlToolCallParseResult parse_xml_tool_call(const std::string& assistant_text) {
    XmlToolCallParseResult result;
    const std::string lower = lowercase_ascii(assistant_text);
    const std::string open = "<tool_call>";
    const std::string close = "</tool_call>";
    const std::size_t first = lower.find(open);
    if (first == std::string::npos) {
        result.error = ok_error();
        return result;
    }
    const std::size_t second = lower.find(open, first + open.size());
    if (second != std::string::npos) {
        result.error = {ErrorCode::ProviderSchema,
                        "XML tool channel allows exactly one <tool_call> block per turn"};
        return result;
    }
    const std::size_t end = lower.find(close, first + open.size());
    if (end == std::string::npos) {
        result.error = {ErrorCode::ProviderSchema, "unterminated <tool_call> block"};
        return result;
    }
    const std::string body =
        assistant_text.substr(first + open.size(), end - (first + open.size()));
    std::string name;
    std::string args;
    std::size_t after_name = 0;
    std::size_t after_args = 0;
    if (!find_tag_content(body, "name", 0, name, after_name)) {
        result.error = {ErrorCode::ProviderSchema, "<tool_call> is missing <name>"};
        return result;
    }
    if (!find_tag_content(body, "args", 0, args, after_args)) {
        result.error = {ErrorCode::ProviderSchema, "<tool_call> is missing <args>"};
        return result;
    }
    result.found = true;
    result.name = trim_ws(name);
    result.arguments_text = args;
    result.error = ok_error();
    return result;
}

}  // namespace ainiux::agent
