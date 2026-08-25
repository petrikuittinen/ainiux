#include "config/config.hpp"

#include "agent/compact.hpp"
#include "chat/generation_settings.hpp"
#include "cli/option_values.hpp"
#include "context/policy.hpp"
#include "config/image_catalog.hpp"
#include "config/model_catalog.hpp"
#include "json/json.hpp"
#include "platform/environment.hpp"
#include "editor/autosave.hpp"
#include "editor/editor_prompts.hpp"
#include "embedded_editor_commands.hpp"
#include "embedded_images_config.hpp"
#include "embedded_models_config.hpp"
#include "ainiux/model_setting.hpp"
#include "tui/theme_registry.hpp"

#include <array>
#include <cerrno>
#include <optional>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <locale>
#include <regex>
#include <sstream>
#include <utility>

namespace ainiux::config {
namespace {

bool is_horizontal_space(char ch) {
    return ch == ' ' || ch == '\t';
}

std::string trim_horizontal(const std::string& text) {
    size_t begin = 0;
    while (begin < text.size() && is_horizontal_space(text[begin])) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && is_horizontal_space(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

size_t first_non_space(const std::string& text) {
    size_t pos = 0;
    while (pos < text.size() && is_horizontal_space(text[pos])) {
        ++pos;
    }
    return pos;
}

bool valid_name(const std::string& name) {
    if (name.empty()) {
        return false;
    }
    const auto valid_first = [](char ch) {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
    };
    const auto valid_rest = [&](char ch) {
        return valid_first(ch) || (ch >= '0' && ch <= '9') || ch == '-';
    };
    if (!valid_first(name.front())) {
        return false;
    }
    for (size_t i = 1; i < name.size(); ++i) {
        if (!valid_rest(name[i])) {
            return false;
        }
    }
    return true;
}

bool valid_section_name(const std::string& section) {
    if (section.empty()) {
        return false;
    }
    size_t begin = 0;
    while (begin < section.size()) {
        const size_t dot = section.find('.', begin);
        const size_t end = dot == std::string::npos ? section.size() : dot;
        if (!valid_name(section.substr(begin, end - begin))) {
            return false;
        }
        if (dot == std::string::npos) {
            return true;
        }
        begin = dot + 1;
    }
    return false;
}

Error parse_error(const std::string& path, size_t line, size_t column, const std::string& detail) {
    return {ErrorCode::Config,
            path + ":" + std::to_string(line) + ":" + std::to_string(column) +
                ": config parse error: " + detail};
}

void line_and_column(const std::string& input, size_t offset, size_t& line, size_t& column) {
    line = 1;
    column = 1;
    const size_t end = offset < input.size() ? offset : input.size();
    for (size_t i = 0; i < end; ++i) {
        if (input[i] == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
}

bool valid_utf8(const std::string& input, size_t& error_offset) {
    const auto continuation = [&](size_t offset) {
        if (offset >= input.size()) {
            return false;
        }
        const unsigned char ch = static_cast<unsigned char>(input[offset]);
        return ch >= 0x80 && ch <= 0xBF;
    };

    size_t i = 0;
    while (i < input.size()) {
        const unsigned char ch = static_cast<unsigned char>(input[i]);
        if (ch <= 0x7F) {
            ++i;
        } else if (ch >= 0xC2 && ch <= 0xDF) {
            if (!continuation(i + 1)) {
                error_offset = i;
                return false;
            }
            i += 2;
        } else if (ch == 0xE0) {
            if (i + 2 >= input.size() || static_cast<unsigned char>(input[i + 1]) < 0xA0 ||
                static_cast<unsigned char>(input[i + 1]) > 0xBF || !continuation(i + 2)) {
                error_offset = i;
                return false;
            }
            i += 3;
        } else if ((ch >= 0xE1 && ch <= 0xEC) || (ch >= 0xEE && ch <= 0xEF)) {
            if (!continuation(i + 1) || !continuation(i + 2)) {
                error_offset = i;
                return false;
            }
            i += 3;
        } else if (ch == 0xED) {
            if (i + 2 >= input.size() || static_cast<unsigned char>(input[i + 1]) < 0x80 ||
                static_cast<unsigned char>(input[i + 1]) > 0x9F || !continuation(i + 2)) {
                error_offset = i;
                return false;
            }
            i += 3;
        } else if (ch == 0xF0) {
            if (i + 3 >= input.size() || static_cast<unsigned char>(input[i + 1]) < 0x90 ||
                static_cast<unsigned char>(input[i + 1]) > 0xBF || !continuation(i + 2) ||
                !continuation(i + 3)) {
                error_offset = i;
                return false;
            }
            i += 4;
        } else if (ch >= 0xF1 && ch <= 0xF3) {
            if (!continuation(i + 1) || !continuation(i + 2) || !continuation(i + 3)) {
                error_offset = i;
                return false;
            }
            i += 4;
        } else if (ch == 0xF4) {
            if (i + 3 >= input.size() || static_cast<unsigned char>(input[i + 1]) < 0x80 ||
                static_cast<unsigned char>(input[i + 1]) > 0x8F || !continuation(i + 2) ||
                !continuation(i + 3)) {
                error_offset = i;
                return false;
            }
            i += 4;
        } else {
            error_offset = i;
            return false;
        }
    }
    return true;
}

bool integer_syntax(const std::string& text) {
    size_t pos = 0;
    if (!text.empty() && (text[pos] == '+' || text[pos] == '-')) {
        ++pos;
    }
    if (pos == text.size()) {
        return false;
    }
    for (; pos < text.size(); ++pos) {
        if (text[pos] < '0' || text[pos] > '9') {
            return false;
        }
    }
    return true;
}

bool float_syntax(const std::string& text) {
    size_t pos = 0;
    if (!text.empty() && (text[pos] == '+' || text[pos] == '-')) {
        ++pos;
    }
    const size_t integer_begin = pos;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        ++pos;
    }
    if (pos == integer_begin) {
        return false;
    }
    bool has_fraction = false;
    bool has_exponent = false;
    if (pos < text.size() && text[pos] == '.') {
        has_fraction = true;
        ++pos;
        const size_t fraction_begin = pos;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            ++pos;
        }
        if (pos == fraction_begin) {
            return false;
        }
    }
    if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
        has_exponent = true;
        ++pos;
        if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
            ++pos;
        }
        const size_t exponent_begin = pos;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            ++pos;
        }
        if (pos == exponent_begin) {
            return false;
        }
    }
    return pos == text.size() && (has_fraction || has_exponent);
}

Error parse_quoted(const std::string& text,
                   const std::string& path,
                   size_t line,
                   size_t value_column,
                   Value& value) {
    std::string decoded;
    for (size_t pos = 1; pos < text.size(); ++pos) {
        const char ch = text[pos];
        if (ch == '"') {
            if (pos + 1 != text.size()) {
                return parse_error(path, line, value_column + pos + 1,
                                   "unexpected text after quoted string");
            }
            value.type = Value::Type::String;
            value.string = std::move(decoded);
            return ok_error();
        }
        if (ch != '\\') {
            decoded.push_back(ch);
            continue;
        }
        if (++pos >= text.size()) {
            return parse_error(path, line, value_column + pos, "incomplete quoted-string escape");
        }
        switch (text[pos]) {
            case '\\':
                decoded.push_back('\\');
                break;
            case '"':
                decoded.push_back('"');
                break;
            case 'n':
                decoded.push_back('\n');
                break;
            case 'r':
                decoded.push_back('\r');
                break;
            case 't':
                decoded.push_back('\t');
                break;
            default:
                return parse_error(path, line, value_column + pos,
                                   std::string("unsupported quoted-string escape: \\") + text[pos]);
        }
    }
    return parse_error(path, line, value_column, "unterminated quoted string");
}

Error parse_value(const std::string& text,
                  const std::string& path,
                  size_t line,
                  size_t column,
                  Value& value) {
    if (!text.empty() && text.front() == '"') {
        return parse_quoted(text, path, line, column, value);
    }
    if (text == "on" || text == "off") {
        value.type = Value::Type::Boolean;
        value.boolean = text == "on";
        return ok_error();
    }
    if (integer_syntax(text)) {
        errno = 0;
        char* end = nullptr;
        const long long parsed = std::strtoll(text.c_str(), &end, 10);
        if (errno == ERANGE || end == nullptr || *end != '\0') {
            return parse_error(path, line, column, "integer is outside the signed 64-bit range");
        }
        value.type = Value::Type::Integer;
        value.integer = static_cast<std::int64_t>(parsed);
        return ok_error();
    }
    if (float_syntax(text)) {
        std::istringstream stream(text);
        stream.imbue(std::locale::classic());
        double parsed = 0.0;
        stream >> parsed;
        if (!stream || stream.peek() != std::char_traits<char>::eof() || !std::isfinite(parsed)) {
            return parse_error(path, line, column, "floating point value is outside the finite range");
        }
        value.type = Value::Type::Float;
        value.floating = parsed;
        return ok_error();
    }
    value.type = Value::Type::String;
    value.string = text;
    return ok_error();
}

class Parser {
   public:
    Parser(std::string input, std::string source_path)
        : input_(std::move(input)), source_path_(std::move(source_path)) {}

    ParseResult run() {
        size_t utf8_error = 0;
        if (!valid_utf8(input_, utf8_error)) {
            size_t line = 1;
            size_t column = 1;
            line_and_column(input_, utf8_error, line, column);
            return {{}, parse_error(source_path_, line, column, "invalid UTF-8")};
        }
        if (input_.size() >= 3 && static_cast<unsigned char>(input_[0]) == 0xEF &&
            static_cast<unsigned char>(input_[1]) == 0xBB && static_cast<unsigned char>(input_[2]) == 0xBF) {
            input_.erase(0, 3);
        }

        Document document;
        std::string section;
        size_t line_number = 1;
        size_t offset = 0;
        while (offset <= input_.size()) {
            const size_t newline = input_.find('\n', offset);
            const size_t end = newline == std::string::npos ? input_.size() : newline;
            std::string line = input_.substr(offset, end - offset);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const size_t stray_cr = line.find('\r');
            if (stray_cr != std::string::npos) {
                return {{}, parse_error(source_path_, line_number, stray_cr + 1,
                                        "carriage return must be followed by a newline")};
            }
            for (size_t i = 0; i < line.size(); ++i) {
                const unsigned char ch = static_cast<unsigned char>(line[i]);
                if (ch == 0 || (ch < 0x20 && ch != '\t')) {
                    return {{}, parse_error(source_path_, line_number, i + 1,
                                            "unescaped control character")};
                }
            }

            std::optional<Value> multiline_value;
            size_t next_offset = 0;
            size_t next_line_number = 0;
            const size_t content = first_non_space(line);
            if (content < line.size() && line[content] != '#' && line[content] != '[') {
                const size_t equals = line.find('=', content);
                if (equals != std::string::npos) {
                    size_t value_begin = equals + 1;
                    while (value_begin < line.size() && is_horizontal_space(line[value_begin])) {
                        ++value_begin;
                    }
                    if (line.compare(value_begin, 3, "\"\"\"") == 0) {
                        Value value;
                        Error multiline_error = parse_multiline_quoted(
                            offset + value_begin,
                            line_number,
                            value_begin + 1,
                            value,
                            next_offset,
                            next_line_number);
                        if (!multiline_error.ok()) {
                            return {{}, multiline_error};
                        }
                        multiline_value = std::move(value);
                    }
                }
            }

            Error err = parse_line(line, line_number, section, document,
                                   multiline_value ? &*multiline_value : nullptr);
            if (!err.ok()) {
                return {{}, err};
            }
            if (multiline_value.has_value()) {
                offset = next_offset;
                line_number = next_line_number;
                if (offset >= input_.size()) {
                    break;
                }
                continue;
            }
            if (newline == std::string::npos) {
                break;
            }
            offset = newline + 1;
            ++line_number;
        }
        return {std::move(document), ok_error()};
    }

   private:
    std::string input_;
    std::string source_path_;
    std::map<std::string, size_t> repeatable_section_next_instance_;
    std::map<std::string, size_t> repeatable_section_current_instance_;

    static bool is_repeatable_section(const std::string& name) {
        return name == "command" || name == "theme" || name == "model" ||
               name == "preset" || name == "image";
    }

    Error parse_multiline_quoted(size_t opening_offset,
                                 size_t opening_line,
                                 size_t opening_column,
                                 Value& value,
                                 size_t& next_offset,
                                 size_t& next_line) const {
        std::string decoded;
        size_t pos = opening_offset + 3;
        size_t line = opening_line;
        size_t column = opening_column + 3;

        if (pos < input_.size() && input_[pos] == '\n') {
            ++pos;
            ++line;
            column = 1;
        } else if (pos + 1 < input_.size() && input_[pos] == '\r' &&
                   input_[pos + 1] == '\n') {
            pos += 2;
            ++line;
            column = 1;
        }

        while (pos < input_.size()) {
            if (input_.compare(pos, 3, "\"\"\"") == 0) {
                pos += 3;
                column += 3;
                while (pos < input_.size() && is_horizontal_space(input_[pos])) {
                    ++pos;
                    ++column;
                }
                if (pos == input_.size()) {
                    value.type = Value::Type::String;
                    value.string = std::move(decoded);
                    next_offset = pos;
                    next_line = line;
                    return ok_error();
                }
                if (input_[pos] == '\n') {
                    value.type = Value::Type::String;
                    value.string = std::move(decoded);
                    next_offset = pos + 1;
                    next_line = line + 1;
                    return ok_error();
                }
                if (input_[pos] == '\r') {
                    if (pos + 1 < input_.size() && input_[pos + 1] == '\n') {
                        value.type = Value::Type::String;
                        value.string = std::move(decoded);
                        next_offset = pos + 2;
                        next_line = line + 1;
                        return ok_error();
                    }
                    return parse_error(source_path_, line, column,
                                       "carriage return must be followed by a newline");
                }
                return parse_error(source_path_, line, column,
                                   "unexpected text after multiline quoted string");
            }

            const char ch = input_[pos];
            if (ch == '\\') {
                const size_t escape_line = line;
                const size_t escape_column = column;
                ++pos;
                ++column;
                if (pos >= input_.size()) {
                    return parse_error(source_path_, escape_line, escape_column,
                                       "incomplete multiline quoted-string escape");
                }
                switch (input_[pos]) {
                    case '\\':
                        decoded.push_back('\\');
                        break;
                    case '"':
                        decoded.push_back('"');
                        break;
                    case 'n':
                        decoded.push_back('\n');
                        break;
                    case 'r':
                        decoded.push_back('\r');
                        break;
                    case 't':
                        decoded.push_back('\t');
                        break;
                    default:
                        return parse_error(source_path_, line, column,
                                           std::string("unsupported quoted-string escape: \\") +
                                               input_[pos]);
                }
                ++pos;
                ++column;
                continue;
            }
            if (ch == '\n') {
                decoded.push_back('\n');
                ++pos;
                ++line;
                column = 1;
                continue;
            }
            if (ch == '\r') {
                if (pos + 1 >= input_.size() || input_[pos + 1] != '\n') {
                    return parse_error(source_path_, line, column,
                                       "carriage return must be followed by a newline");
                }
                decoded.push_back('\n');
                pos += 2;
                ++line;
                column = 1;
                continue;
            }
            const unsigned char byte = static_cast<unsigned char>(ch);
            if (byte == 0 || (byte < 0x20 && ch != '\t')) {
                return parse_error(source_path_, line, column, "unescaped control character");
            }
            decoded.push_back(ch);
            ++pos;
            ++column;
        }

        return parse_error(source_path_, opening_line, opening_column,
                           "unterminated multiline quoted string");
    }

    Error parse_line(const std::string& line,
                     size_t line_number,
                     std::string& section,
                     Document& document,
                     const Value* pre_parsed_value) {
        const size_t content = first_non_space(line);
        if (content == line.size() || line[content] == '#') {
            return ok_error();
        }
        const std::string trimmed = trim_horizontal(line);
        if (line[content] == '[') {
            if (trimmed.size() < 2 || trimmed.back() != ']') {
                return parse_error(source_path_, line_number, content + 1,
                                   "section header must end with ]");
            }
            const std::string name = trimmed.substr(1, trimmed.size() - 2);
            if (!valid_section_name(name)) {
                return parse_error(source_path_, line_number, content + 2, "invalid section name");
            }
            section = name;
            if (is_repeatable_section(name)) {
                repeatable_section_current_instance_[name] = repeatable_section_next_instance_[name]++;
            }
            return ok_error();
        }

        const size_t equals = line.find('=', content);
        if (equals == std::string::npos) {
            return parse_error(source_path_, line_number, content + 1,
                               "expected key = value assignment");
        }
        const std::string key = trim_horizontal(line.substr(content, equals - content));
        if (!valid_name(key)) {
            return parse_error(source_path_, line_number, content + 1, "invalid key name");
        }
        size_t value_begin = equals + 1;
        while (value_begin < line.size() && is_horizontal_space(line[value_begin])) {
            ++value_begin;
        }
        const std::string value_text = trim_horizontal(line.substr(value_begin));
        Value value;
        if (pre_parsed_value != nullptr) {
            value = *pre_parsed_value;
        } else {
            Error err = parse_value(value_text, source_path_, line_number, value_begin + 1, value);
            if (!err.ok()) {
                return err;
            }
        }

        std::string qualified;
        if (is_repeatable_section(section)) {
            const auto current = repeatable_section_current_instance_.find(section);
            if (current == repeatable_section_current_instance_.end()) {
                return parse_error(source_path_, line_number, content + 1,
                                   "repeatable section [" + section + "] requires a section header before keys");
            }
            qualified = section + "." + std::to_string(current->second) + "." + key;
        } else {
            qualified = section.empty() ? key : section + "." + key;
        }
        const auto duplicate = document.entries.find(qualified);
        if (duplicate != document.entries.end()) {
            const SourceLocation& previous = duplicate->second.source;
            return parse_error(source_path_, line_number, content + 1,
                               "duplicate key " + qualified + "; first defined at " + previous.path + ":" +
                                   std::to_string(previous.line) + ":" + std::to_string(previous.column));
        }
        Entry entry;
        entry.section = section;
        entry.key = key;
        entry.value = std::move(value);
        entry.source = {source_path_, line_number, content + 1};
        document.entries.emplace(qualified, std::move(entry));
        return ok_error();
    }
};

Error schema_error(const Entry& entry, const std::string& detail) {
    const std::string qualified = entry.section.empty() ? entry.key : entry.section + "." + entry.key;
    return {ErrorCode::Config,
            entry.source.path + ":" + std::to_string(entry.source.line) + ":" +
                std::to_string(entry.source.column) + ": invalid config setting " + qualified + ": " + detail};
}

Error require_type(const Entry& entry, Value::Type expected) {
    if (entry.value.type == expected) {
        return ok_error();
    }
    if (expected == Value::Type::Boolean) {
        return schema_error(entry, "expected on or off");
    }
    return schema_error(entry, std::string("expected ") + value_type_name(expected) + ", got " +
                                   value_type_name(entry.value.type));
}

Error nonnegative_long(const Entry& entry, long& output) {
    Error err = require_type(entry, Value::Type::Integer);
    if (!err.ok()) {
        return err;
    }
    if (entry.value.integer < 0 ||
        static_cast<std::uint64_t>(entry.value.integer) > static_cast<std::uint64_t>(std::numeric_limits<long>::max())) {
        return schema_error(entry, "expected a non-negative integer in the platform long range");
    }
    output = static_cast<long>(entry.value.integer);
    return ok_error();
}

Error nonnegative_int(const Entry& entry, int& output) {
    Error err = require_type(entry, Value::Type::Integer);
    if (!err.ok()) {
        return err;
    }
    if (entry.value.integer < 0 || entry.value.integer > std::numeric_limits<int>::max()) {
        return schema_error(entry, "expected a non-negative integer in the platform int range");
    }
    output = static_cast<int>(entry.value.integer);
    return ok_error();
}

Error nonnegative_long_long(const Entry& entry, long long& output) {
    Error err = require_type(entry, Value::Type::Integer);
    if (!err.ok()) {
        return err;
    }
    if (entry.value.integer < 0) {
        return schema_error(entry, "expected a non-negative integer");
    }
    output = static_cast<long long>(entry.value.integer);
    return ok_error();
}

Error editor_file_size_limit(const Entry& entry, long long& output) {
    Error err = require_type(entry, Value::Type::Integer);
    if (!err.ok()) {
        return err;
    }
    if (entry.value.integer < -1) {
        return schema_error(entry, "expected -1 or a non-negative byte limit");
    }
    output = static_cast<long long>(entry.value.integer);
    return ok_error();
}

Error auto_save_byte_size(const Entry& entry, long long& output) {
    if (entry.value.is_integer()) {
        if (entry.value.integer < 0) {
            return schema_error(entry, "expected a non-negative byte size");
        }
        output = entry.value.integer;
        return ok_error();
    }
    if (!entry.value.is_string()) {
        return schema_error(entry, "expected an integer or byte size such as 10M");
    }
    Error err = ainiux::editor::parse_byte_size(entry.value.string, output);
    if (!err.ok()) {
        return schema_error(entry, err.message);
    }
    return ok_error();
}

Error numeric_double(const Entry& entry, double& output) {
    if (entry.value.is_float()) {
        output = entry.value.floating;
        return ok_error();
    }
    if (entry.value.is_integer()) {
        output = static_cast<double>(entry.value.integer);
        return ok_error();
    }
    return schema_error(entry, std::string("expected integer or float, got ") +
                                   value_type_name(entry.value.type));
}

Error enum_string(const Entry& entry,
                  const std::vector<std::string>& allowed,
                  std::string& output,
                  const std::string& description) {
    Error err = require_type(entry, Value::Type::String);
    if (!err.ok()) {
        return err;
    }
    for (const std::string& value : allowed) {
        if (entry.value.string == value) {
            output = value;
            return ok_error();
        }
    }
    return schema_error(entry, "expected " + description);
}

Error context_window_tokens(const Entry& entry, long long& output) {
    if (entry.value.is_integer()) {
        if (entry.value.integer < 0) {
            return schema_error(entry, "expected zero or a positive token count");
        }
        output = entry.value.integer;
        return ok_error();
    }
    if (!entry.value.is_string()) {
        return schema_error(entry, "expected an integer or token count such as 64k or 1M");
    }
    const std::string& text = entry.value.string;
    if (text.empty()) {
        return schema_error(entry, "expected zero or a token count such as 64k or 1M");
    }
    size_t digits = text.size();
    long long multiplier = 1;
    const char suffix = text.back();
    if (suffix == 'k' || suffix == 'K') {
        multiplier = 1024;
        --digits;
    } else if (suffix == 'm' || suffix == 'M') {
        multiplier = 1000000;
        --digits;
    }
    if (digits == 0) {
        return schema_error(entry, "expected zero or a token count such as 64k or 1M");
    }
    long long value = 0;
    for (size_t i = 0; i < digits; ++i) {
        const char ch = text[i];
        if (ch < '0' || ch > '9') {
            return schema_error(entry, "expected zero or a token count with an optional k or M suffix");
        }
        const int digit = ch - '0';
        if (value > (std::numeric_limits<long long>::max() - digit) / 10) {
            return schema_error(entry, "token count is too large");
        }
        value = value * 10 + digit;
    }
    if (value > std::numeric_limits<long long>::max() / multiplier) {
        return schema_error(entry, "token count is too large");
    }
    output = value * multiplier;
    return ok_error();
}

bool absolute_path(const std::string& path) {
    return !path.empty() && std::filesystem::u8path(path).is_absolute();
}

std::string environment_value(const char* name) {
    return platform::environment_value(name);
}

// Installed share lookup for make install PREFIX and scripts/install.sh --user.
// Order: $XDG_DATA_HOME/ainiux, else ~/.local/share/ainiux, then /usr/local and /usr.
// User config still lives under ~/.config/ainiux (or $XDG_CONFIG_HOME); these paths
// are only the installed/share data layer, not a second system config root.
void append_installed_share_paths(std::vector<std::string>& paths, const char* filename) {
    const std::string xdg_data = environment_value("XDG_DATA_HOME");
    if (absolute_path(xdg_data)) {
        paths.emplace_back((std::filesystem::u8path(xdg_data) / "ainiux" / filename).u8string());
    } else {
        const std::string home = platform::home_directory();
        if (absolute_path(home)) {
            paths.emplace_back(
                (std::filesystem::u8path(home) / ".local" / "share" / "ainiux" / filename)
                    .u8string());
        }
    }
    paths.emplace_back(std::string("/usr/local/share/ainiux/") + filename);
    paths.emplace_back(std::string("/usr/share/ainiux/") + filename);
}

// Prefer PREFIX/share next to bin/ (FHS / Windows package layout). Also try a
// side-by-side share/ directory used by some staged package trees.
void append_executable_share_path(std::vector<std::string>& paths, const char* filename) {
    const std::string executable_dir = platform::executable_directory();
    if (executable_dir.empty()) {
        return;
    }
    const std::filesystem::path exe = std::filesystem::u8path(executable_dir);
    paths.emplace_back((exe / ".." / "share" / "ainiux" / filename).lexically_normal().u8string());
    paths.emplace_back((exe / "share" / "ainiux" / filename).u8string());
}

std::string trim_config_ascii(std::string text) {
    auto is_ws = [](unsigned char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    };
    while (!text.empty() && is_ws(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
    }
    while (!text.empty() && is_ws(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    return text;
}

std::string lower_config_ascii(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

Error auto_save_mode(const Entry& entry, bool& output) {
    if (entry.value.is_boolean()) {
        output = entry.value.boolean;
        return ok_error();
    }
    return schema_error(entry, "expected on or off");
}

ainiux::editor::EditorAssistCommand* find_assist_command_by_name(ainiux::editor::EditorAssistConfig& config,
                                                                const std::string& command) {
    std::string normalized = lower_config_ascii(trim_config_ascii(command));
    while (!normalized.empty() && normalized.front() == '/') {
        normalized.erase(normalized.begin());
    }
    for (ainiux::editor::EditorAssistCommand& entry : config.commands) {
        std::string entry_name = lower_config_ascii(trim_config_ascii(entry.command));
        while (!entry_name.empty() && entry_name.front() == '/') {
            entry_name.erase(entry_name.begin());
        }
        if (entry_name == normalized) {
            return &entry;
        }
    }
    return nullptr;
}

Error parse_assist_command_modes(const Entry& entry, std::vector<ainiux::editor::AssistCommandMode>& modes) {
    Error err = require_type(entry, Value::Type::String);
    if (!err.ok()) {
        return err;
    }
    modes.clear();
    std::string remaining = entry.value.string;
    while (!remaining.empty()) {
        const size_t comma = remaining.find(',');
        std::string token = trim_config_ascii(comma == std::string::npos ? remaining
                                                                         : remaining.substr(0, comma));
        remaining = comma == std::string::npos ? "" : remaining.substr(comma + 1);
        token = lower_config_ascii(std::move(token));
        if (token.empty()) {
            continue;
        }
        if (token == "continue") {
            modes.push_back(ainiux::editor::AssistCommandMode::Continue);
        } else if (token == "selection") {
            modes.push_back(ainiux::editor::AssistCommandMode::Selection);
        } else if (token == "all") {
            modes.push_back(ainiux::editor::AssistCommandMode::All);
        } else if (token == "insert" || token == "local_insert" || token == "localinsert") {
            modes.push_back(ainiux::editor::AssistCommandMode::Insert);
        } else if (token == "fact") {
            modes.push_back(ainiux::editor::AssistCommandMode::Fact);
        } else if (token == "newbuffer" || token == "new" || token == "n") {
            modes.push_back(ainiux::editor::AssistCommandMode::NewBuffer);
        } else {
            return schema_error(entry, "expected continue, selection, all, insert, newbuffer, or fact");
        }
    }
    if (modes.empty()) {
        return schema_error(entry, "expected at least one mode");
    }
    return ok_error();
}

Error validate_assist_command_string(const Entry& entry, std::string& command) {
    Error err = require_type(entry, Value::Type::String);
    if (!err.ok()) {
        return err;
    }
    command = trim_config_ascii(entry.value.string);
    while (!command.empty() && command.front() == '/') {
        command.erase(command.begin());
    }
    if (command.empty()) {
        return schema_error(entry, "command string must include a name after /");
    }
    return ok_error();
}

void merge_assist_command(ainiux::editor::EditorAssistConfig& config, ainiux::editor::EditorAssistCommand command) {
    if (ainiux::editor::EditorAssistCommand* existing = find_assist_command_by_name(config, command.command)) {
        *existing = std::move(command);
        return;
    }
    config.commands.push_back(std::move(command));
}

Error apply_legacy_assist_prompt(const Entry& entry,
                                 ainiux::editor::EditorAssistConfig& config,
                                 const std::string& command_name) {
    Error err = require_type(entry, Value::Type::String);
    if (!err.ok()) {
        return err;
    }
    if (ainiux::editor::EditorAssistCommand* command = find_assist_command_by_name(config, command_name)) {
        command->prompt = entry.value.string;
    }
    return ok_error();
}

Error apply_theme_color_key(const Entry& entry, const std::string& key, tui::Rgb& out) {
    Error err = require_type(entry, Value::Type::String);
    if (!err.ok()) {
        return err;
    }
    if (!tui::parse_rgb_color(entry.value.string, out)) {
        return schema_error(entry, "expected color in #RRGGBB form for " + key);
    }
    return ok_error();
}

Error apply_configured_themes(const Document& document, cli::Options& candidate) {
    struct PartialTheme {
        std::optional<std::string> name;
        std::optional<tui::Rgb> background;
        std::optional<tui::Rgb> text;
        std::optional<tui::Rgb> muted;
        std::optional<tui::Rgb> thinking_trace;
        std::optional<tui::Rgb> user_label;
        std::optional<tui::Rgb> assistant_label;
        std::optional<tui::Rgb> error;
        std::optional<tui::Rgb> status_foreground;
        std::optional<tui::Rgb> status_background;
        std::optional<tui::Rgb> thinking_activity;
        std::optional<tui::Rgb> streaming_activity;
        std::optional<tui::Rgb> panel_title;
        std::optional<tui::Rgb> panel_border;
        std::optional<tui::Rgb> panel_hint;
        std::optional<tui::Rgb> panel_highlight;
        std::optional<tui::Rgb> panel_body;
        std::optional<tui::Rgb> panel_background;
        std::map<std::string, tui::Rgb> syntax_colors;
        SourceLocation source;
    };

    std::map<size_t, PartialTheme> partial_themes;
    for (const auto& item : document.entries) {
        const std::string& name = item.first;
        if (name.rfind("theme.", 0) != 0) {
            continue;
        }
        const std::string tail = name.substr(std::string("theme.").size());
        const size_t dot = tail.find('.');
        if (dot == std::string::npos) {
            continue;
        }
        size_t index = 0;
        try {
            index = static_cast<size_t>(std::stoul(tail.substr(0, dot)));
        } catch (const std::exception&) {
            continue;
        }
        const std::string key = tail.substr(dot + 1);
        PartialTheme& partial = partial_themes[index];
        if (partial.source.path.empty()) {
            partial.source = item.second.source;
        }
        const Entry& entry = item.second;
        tui::Rgb color{};
        if (key == "name") {
            if (entry.value.is_boolean() && !entry.value.boolean) {
                return schema_error(entry, "theme name 'off' is reserved for disabling colors");
            }
            Error err = require_type(entry, Value::Type::String);
            if (!err.ok()) {
                return err;
            }
            const std::string theme_name = trim_config_ascii(entry.value.string);
            if (theme_name.empty()) {
                return schema_error(entry, "name must not be empty");
            }
            if (lower_config_ascii(theme_name) == "off") {
                return schema_error(entry, "theme name 'off' is reserved for disabling colors");
            }
            partial.name = theme_name;
        } else if (key == "background") {
            Error err = apply_theme_color_key(entry, key, color);
            if (!err.ok()) {
                return err;
            }
            partial.background = color;
        } else if (key == "text") {
            Error err = apply_theme_color_key(entry, key, color);
            if (!err.ok()) {
                return err;
            }
            partial.text = color;
        } else if (key == "muted") {
            Error err = apply_theme_color_key(entry, key, color);
            if (!err.ok()) {
                return err;
            }
            partial.muted = color;
        } else if (key == "thinking_trace") {
            Error err = apply_theme_color_key(entry, key, color);
            if (!err.ok()) {
                return err;
            }
            partial.thinking_trace = color;
        } else if (key == "user_label") {
            Error err = apply_theme_color_key(entry, key, color);
            if (!err.ok()) {
                return err;
            }
            partial.user_label = color;
        } else if (key == "assistant_label") {
            Error err = apply_theme_color_key(entry, key, color);
            if (!err.ok()) {
                return err;
            }
            partial.assistant_label = color;
        } else if (key == "error") {
            Error err = apply_theme_color_key(entry, key, color);
            if (!err.ok()) {
                return err;
            }
            partial.error = color;
        } else if (key == "status_foreground") {
            Error err = apply_theme_color_key(entry, key, color);
            if (!err.ok()) {
                return err;
            }
            partial.status_foreground = color;
        } else if (key == "status_background") {
            Error err = apply_theme_color_key(entry, key, color);
            if (!err.ok()) {
                return err;
            }
            partial.status_background = color;
        } else if (key == "thinking_activity") {
            Error err = apply_theme_color_key(entry, key, color);
            if (!err.ok()) {
                return err;
            }
            partial.thinking_activity = color;
        } else if (key == "streaming_activity") {
            Error err = apply_theme_color_key(entry, key, color);
            if (!err.ok()) {
                return err;
            }
            partial.streaming_activity = color;
        } else if (key == "panel_title") {
            Error err = apply_theme_color_key(entry, key, color);
            if (!err.ok()) {
                return err;
            }
            partial.panel_title = color;
        } else if (key == "panel_border") {
            Error err = apply_theme_color_key(entry, key, color);
            if (!err.ok()) {
                return err;
            }
            partial.panel_border = color;
        } else if (key == "panel_hint") {
            Error err = apply_theme_color_key(entry, key, color);
            if (!err.ok()) {
                return err;
            }
            partial.panel_hint = color;
        } else if (key == "panel_highlight") {
            Error err = apply_theme_color_key(entry, key, color);
            if (!err.ok()) {
                return err;
            }
            partial.panel_highlight = color;
        } else if (key == "panel_body") {
            Error err = apply_theme_color_key(entry, key, color);
            if (!err.ok()) {
                return err;
            }
            partial.panel_body = color;
        } else if (key == "panel_background") {
            Error err = apply_theme_color_key(entry, key, color);
            if (!err.ok()) {
                return err;
            }
            partial.panel_background = color;
        } else if (key.rfind("syntax_", 0) == 0) {
            Error err = apply_theme_color_key(entry, key, color);
            if (!err.ok()) {
                return err;
            }
            tui::ThemePalette validation;
            if (!tui::set_syntax_theme_color(validation, key, color)) {
                return schema_error(entry, "unknown syntax theme color key: " + key);
            }
            partial.syntax_colors[key] = color;
        } else {
            return schema_error(entry,
                                "unknown [theme] key; expected name, background, text, muted, thinking_trace, "
                                "user_label, assistant_label, error, status_foreground, status_background, "
                                "thinking_activity, streaming_activity, panel_title, panel_border, panel_hint, "
                                "panel_highlight, panel_body, panel_background, or a syntax_* color");
        }
    }

    auto required_color = [&](const PartialTheme& partial, const std::optional<tui::Rgb>& value, const char* key) -> Error {
        if (value.has_value()) {
            return ok_error();
        }
        return {ErrorCode::Config,
                partial.source.path + ":" + std::to_string(partial.source.line) + ":" +
                    std::to_string(partial.source.column) + ": invalid config setting [theme] " +
                    (partial.name.has_value() ? *partial.name : "<unnamed>") + ": " + key + " is required"};
    };

    for (const auto& item : partial_themes) {
        const PartialTheme& partial = item.second;
        if (!partial.name.has_value()) {
            return {ErrorCode::Config,
                    partial.source.path + ":" + std::to_string(partial.source.line) + ":" +
                        std::to_string(partial.source.column) +
                        ": invalid config setting [theme]: name is required"};
        }
        Error err = required_color(partial, partial.background, "background");
        if (!err.ok()) {
            return err;
        }
        err = required_color(partial, partial.text, "text");
        if (!err.ok()) {
            return err;
        }
        err = required_color(partial, partial.muted, "muted");
        if (!err.ok()) {
            return err;
        }
        err = required_color(partial, partial.thinking_trace, "thinking_trace");
        if (!err.ok()) {
            return err;
        }
        err = required_color(partial, partial.user_label, "user_label");
        if (!err.ok()) {
            return err;
        }
        err = required_color(partial, partial.assistant_label, "assistant_label");
        if (!err.ok()) {
            return err;
        }
        err = required_color(partial, partial.error, "error");
        if (!err.ok()) {
            return err;
        }
        err = required_color(partial, partial.status_foreground, "status_foreground");
        if (!err.ok()) {
            return err;
        }
        err = required_color(partial, partial.status_background, "status_background");
        if (!err.ok()) {
            return err;
        }
        err = required_color(partial, partial.thinking_activity, "thinking_activity");
        if (!err.ok()) {
            return err;
        }
        err = required_color(partial, partial.streaming_activity, "streaming_activity");
        if (!err.ok()) {
            return err;
        }
        err = required_color(partial, partial.panel_title, "panel_title");
        if (!err.ok()) {
            return err;
        }
        err = required_color(partial, partial.panel_border, "panel_border");
        if (!err.ok()) {
            return err;
        }
        err = required_color(partial, partial.panel_hint, "panel_hint");
        if (!err.ok()) {
            return err;
        }
        err = required_color(partial, partial.panel_highlight, "panel_highlight");
        if (!err.ok()) {
            return err;
        }
        err = required_color(partial, partial.panel_body, "panel_body");
        if (!err.ok()) {
            return err;
        }
        err = required_color(partial, partial.panel_background, "panel_background");
        if (!err.ok()) {
            return err;
        }

        tui::ThemePalette palette;
        palette.name = *partial.name;
        palette.background = *partial.background;
        palette.text = *partial.text;
        palette.muted = *partial.muted;
        palette.thinking_trace = *partial.thinking_trace;
        palette.user_label = *partial.user_label;
        palette.assistant_label = *partial.assistant_label;
        palette.error = *partial.error;
        palette.status_foreground = *partial.status_foreground;
        palette.status_background = *partial.status_background;
        palette.thinking_activity = *partial.thinking_activity;
        palette.streaming_activity = *partial.streaming_activity;
        palette.panel_title = *partial.panel_title;
        palette.panel_border = *partial.panel_border;
        palette.panel_hint = *partial.panel_hint;
        palette.panel_highlight = *partial.panel_highlight;
        palette.panel_body = *partial.panel_body;
        palette.panel_background = *partial.panel_background;
        tui::derive_syntax_theme_colors(palette);
        for (const auto& syntax_color : partial.syntax_colors) {
            tui::set_syntax_theme_color(palette, syntax_color.first, syntax_color.second);
        }
        candidate.tui_themes.merge(std::move(palette));
    }
    return ok_error();
}

bool is_themes_entry(const std::string& name) {
    return name.rfind("theme.", 0) == 0;
}

bool is_editor_commands_entry(const std::string& name) {
    if (name.rfind("command.", 0) == 0) {
        return true;
    }
    return name == "editor.assist_behavior" || name == "editor.assist_spell" ||
           name == "editor.assist_grammar" || name == "editor.assist_continue" ||
           name == "editor.assist_fact" || name == "editor.assist_comment" ||
           name == "editor.assist_rewrite" || name == "editor.assist_english" ||
           name == "editor.assist_chinese" || name == "editor.assist_finnish";
}

Error apply_editor_assist_entry(const std::string& name,
                                const Entry& entry,
                                cli::Options& candidate) {
    if (name == "editor.assist_behavior") {
        Error err = require_type(entry, Value::Type::String);
        if (err.ok()) {
            candidate.editor_assist_config.behavior_rules = entry.value.string;
        }
        return err;
    }
    if (name == "editor.assist_spell") {
        return apply_legacy_assist_prompt(entry, candidate.editor_assist_config, "/spell");
    }
    if (name == "editor.assist_grammar") {
        return apply_legacy_assist_prompt(entry, candidate.editor_assist_config, "/grammar");
    }
    if (name == "editor.assist_continue") {
        return apply_legacy_assist_prompt(entry, candidate.editor_assist_config, "/continue");
    }
    if (name == "editor.assist_fact") {
        return apply_legacy_assist_prompt(entry, candidate.editor_assist_config, "/fact");
    }
    if (name == "editor.assist_comment") {
        return apply_legacy_assist_prompt(entry, candidate.editor_assist_config, "/comment");
    }
    if (name == "editor.assist_rewrite") {
        return apply_legacy_assist_prompt(entry, candidate.editor_assist_config, "/rewrite");
    }
    if (name == "editor.assist_english") {
        return apply_legacy_assist_prompt(entry, candidate.editor_assist_config, "/English");
    }
    if (name == "editor.assist_chinese") {
        return apply_legacy_assist_prompt(entry, candidate.editor_assist_config, "/Chinese");
    }
    if (name == "editor.assist_finnish") {
        return apply_legacy_assist_prompt(entry, candidate.editor_assist_config, "/Finnish");
    }
    return schema_error(entry, "unknown editor-commands setting");
}

bool repeatable_entry_parts(const std::string& name,
                            const std::string& section,
                            size_t& index,
                            std::string& key) {
    const std::string prefix = section + ".";
    if (name.rfind(prefix, 0) != 0) return false;
    const std::string tail = name.substr(prefix.size());
    const size_t dot = tail.find('.');
    if (dot == std::string::npos) return false;
    try {
        index = static_cast<size_t>(std::stoull(tail.substr(0, dot)));
    } catch (const std::exception&) {
        return false;
    }
    key = tail.substr(dot + 1);
    return true;
}

Error catalog_required(const SourceLocation& source,
                       const std::string& section,
                       const char* field) {
    return {ErrorCode::Config,
            source.path + ":" + std::to_string(source.line) + ":" +
                std::to_string(source.column) + ": invalid config setting [" + section +
                "]: " + field + " is required"};
}

Error reasoning_selection_entry(const Entry& entry,
                                ReasoningSelection& selection,
                                bool allow_auto = false) {
    std::string text;
    if (entry.value.is_integer()) {
        if (entry.value.integer < 0) {
            return schema_error(entry, "reasoning token budget must be non-negative");
        }
        text = std::to_string(entry.value.integer);
    } else if (entry.value.is_string()) {
        text = entry.value.string;
    } else if (entry.value.is_boolean() && !entry.value.boolean) {
        // `off` is the canonical user spelling and is tokenized as a boolean
        // by the TOML-like parser, but reasoning resolves it semantically.
        text = "off";
    } else {
        return schema_error(entry, "reasoning must be an ASCII value or integer token budget");
    }
    Error err = parse_reasoning_selection(text, selection, allow_auto);
    return err.ok() ? err : schema_error(entry, err.message);
}

Error reasoning_options_entry(const Entry& entry,
                              std::vector<ReasoningSelection>& options) {
    Error err = require_type(entry, Value::Type::String);
    if (!err.ok()) return err;

    options.clear();
    size_t begin = 0;
    while (begin <= entry.value.string.size()) {
        const size_t separator = entry.value.string.find('|', begin);
        const size_t end = separator == std::string::npos
                               ? entry.value.string.size()
                               : separator;
        const std::string token =
            trim_config_ascii(entry.value.string.substr(begin, end - begin));
        if (token.empty()) {
            return schema_error(entry,
                                "reasoning values must be separated by | and must not be empty");
        }
        ReasoningSelection selection;
        err = parse_reasoning_selection(token, selection, false);
        if (!err.ok()) return schema_error(entry, err.message);
        if (std::find(options.begin(), options.end(), selection) != options.end()) {
            return schema_error(entry, "duplicate reasoning value " + token);
        }
        options.push_back(std::move(selection));
        if (separator == std::string::npos) break;
        begin = separator + 1;
    }
    return ok_error();
}

template <typename T, typename Predicate>
void erase_matching(std::vector<T>& values, Predicate predicate) {
    values.erase(std::remove_if(values.begin(), values.end(), predicate), values.end());
}

Error apply_configured_model_catalog(const Document& document, cli::Options& candidate) {
    struct PartialModel {
        std::optional<std::string> id;
        std::string provider = "any";
        std::string api = "any";
        std::optional<std::string> regex;
        std::optional<long long> context_window_tokens;
        int priority = 0;
        std::optional<ReasoningProtocol> protocol;
        std::optional<ReasoningSelection> reasoning_default;
        std::vector<ReasoningSelection> reasoning_options;
        TemperatureSupport temperature = TemperatureSupport::Unknown;
        std::optional<double> temperature_max;
        bool web_search = false;
        std::string web_search_name = "web_search";
        std::optional<bool> images;
        bool enabled = true;
        SourceLocation source;
    };
    struct PartialPreset {
        ModelSetting preset;
        bool have_model_id = false;
        bool have_purpose = false;
        SourceLocation source;
    };

    std::map<size_t, PartialModel> models;
    std::map<size_t, PartialPreset> presets;
    for (const auto& item : document.entries) {
        const Entry& entry = item.second;
        size_t index = 0;
        std::string key;
        if (repeatable_entry_parts(item.first, "model", index, key)) {
            PartialModel& partial = models[index];
            if (partial.source.path.empty()) partial.source = entry.source;
            if (key == "id" || key == "provider" || key == "api" || key == "model") {
                Error err = require_type(entry, Value::Type::String);
                if (!err.ok()) return err;
                if (entry.value.string.empty()) return schema_error(entry, key + " must not be empty");
                if (key == "id") partial.id = entry.value.string;
                else if (key == "provider") partial.provider = entry.value.string;
                else if (key == "api") {
                    const std::string api = lower_config_ascii(entry.value.string);
                    if (api != "any" && api != "chat" && api != "responses") {
                        return schema_error(entry, "api must be any, chat, or responses");
                    }
                    partial.api = api;
                } else partial.regex = entry.value.string;
            } else if (key == "value") {
                Error err = reasoning_options_entry(entry, partial.reasoning_options);
                if (!err.ok()) return err;
            } else if (key == "context_window") {
                long long value = 0;
                Error err = context_window_tokens(entry, value);
                if (!err.ok()) return err;
                partial.context_window_tokens = value;
            } else if (key == "priority") {
                Error err = require_type(entry, Value::Type::Integer);
                if (!err.ok()) return err;
                if (entry.value.integer < std::numeric_limits<int>::min() ||
                    entry.value.integer > std::numeric_limits<int>::max()) {
                    return schema_error(entry, "priority is outside the supported integer range");
                }
                partial.priority = static_cast<int>(entry.value.integer);
            } else if (key == "reasoning_protocol") {
                Error err = require_type(entry, Value::Type::String);
                if (!err.ok()) return err;
                ReasoningProtocol protocol;
                if (!parse_reasoning_protocol(entry.value.string, protocol)) {
                    return schema_error(entry, "unknown reasoning protocol; expected " + reasoning_protocol_names());
                }
                partial.protocol = protocol;
            } else if (key == "reasoning_default") {
                ReasoningSelection value;
                Error err = reasoning_selection_entry(entry, value, false);
                if (!err.ok()) return err;
                partial.reasoning_default = value;
            } else if (key == "temperature") {
                Error err = require_type(entry, Value::Type::String);
                if (!err.ok()) return err;
                if (!parse_temperature_support(entry.value.string, partial.temperature)) {
                    return schema_error(entry,
                                        "temperature must be unknown, supported, unsupported, or reasoning_none_only");
                }
            } else if (key == "temperature_max") {
                double value = 0.0;
                Error err = numeric_double(entry, value);
                if (!err.ok()) return err;
                if (!std::isfinite(value) || value <= 0.0) {
                    return schema_error(entry, "temperature_max must be a finite number greater than 0");
                }
                partial.temperature_max = value;
            } else if (key == "enabled") {
                Error err = require_type(entry, Value::Type::Boolean);
                if (!err.ok()) return err;
                partial.enabled = entry.value.boolean;
            } else if (key == "web_search") {
                Error err = require_type(entry, Value::Type::Boolean);
                if (!err.ok()) return err;
                partial.web_search = entry.value.boolean;
            } else if (key == "web_search_name") {
                Error err = require_type(entry, Value::Type::String);
                if (!err.ok()) return err;
                if (entry.value.string.empty())
                    return schema_error(entry, "web_search_name must not be empty");
                partial.web_search_name = entry.value.string;
            } else if (key == "images") {
                Error err = require_type(entry, Value::Type::Boolean);
                if (!err.ok()) return err;
                partial.images = entry.value.boolean;
            } else {
                return schema_error(entry, "unknown [model] key");
            }
            continue;
        }
        if (repeatable_entry_parts(item.first, "preset", index, key)) {
            PartialPreset& partial = presets[index];
            if (partial.source.path.empty()) partial.source = entry.source;
            if (key == "model_id" || key == "purpose" || key == "default_system_prompt") {
                Error err = require_type(entry, Value::Type::String);
                if (!err.ok()) return err;
                if (key == "model_id") {
                    partial.preset.model_id = entry.value.string;
                    partial.have_model_id = true;
                } else if (key == "purpose") {
                    if (!chat::generation::is_chat_purpose(entry.value.string)) {
                        return schema_error(entry, "purpose must be " + chat::generation::chat_purpose_description());
                    }
                    partial.preset.purpose = entry.value.string;
                    partial.have_purpose = true;
                } else partial.preset.default_system_prompt = entry.value.string;
            } else if (key == "temperature" || key == "top_p" || key == "min_p" ||
                       key == "repeat_penalty" || key == "presence_penalty") {
                double value = 0.0;
                Error err = numeric_double(entry, value);
                if (!err.ok()) return err;
                if (key == "temperature") partial.preset.temperature = value;
                else if (key == "top_p") partial.preset.top_p = value;
                else if (key == "min_p") partial.preset.min_p = value;
                else if (key == "repeat_penalty") partial.preset.repeat_penalty = value;
                else partial.preset.presence_penalty = value;
            } else if (key == "top_k") {
                int value = 0;
                Error err = nonnegative_int(entry, value);
                if (!err.ok()) return err;
                partial.preset.top_k = value;
            } else if (key == "reasoning") {
                ReasoningSelection value;
                Error err = reasoning_selection_entry(entry, value, true);
                if (!err.ok()) return err;
                partial.preset.reasoning = value;
            } else if (key == "enabled") {
                Error err = require_type(entry, Value::Type::Boolean);
                if (!err.ok()) return err;
                partial.preset.enabled = entry.value.boolean;
            } else {
                return schema_error(entry, "unknown [preset] key");
            }
        }
    }

    for (const auto& item : models) {
        const PartialModel& partial = item.second;
        if (!partial.id.has_value()) return catalog_required(partial.source, "model", "id");
        erase_matching(candidate.model_catalog.models,
                       [&](const ModelCapability& value) { return value.id == *partial.id; });
        if (!partial.enabled) continue;
        if (!partial.regex.has_value()) return catalog_required(partial.source, "model", "model");
        if (!partial.protocol.has_value()) return catalog_required(partial.source, "model", "reasoning_protocol");
        try {
            (void)std::regex(*partial.regex, std::regex::ECMAScript | std::regex::icase);
        } catch (const std::regex_error& err) {
            return {ErrorCode::Config,
                    partial.source.path + ":" + std::to_string(partial.source.line) + ":" +
                        std::to_string(partial.source.column) +
                        ": invalid model regex for [model] " + *partial.id + ": " + err.what()};
        }
        ModelCapability capability;
        capability.id = *partial.id;
        capability.provider = partial.provider;
        capability.api = partial.api;
        capability.model_regex = *partial.regex;
        capability.context_window_tokens = partial.context_window_tokens;
        capability.priority = partial.priority;
        capability.reasoning_protocol = *partial.protocol;
        capability.reasoning_default = partial.reasoning_default;
        capability.reasoning_options = partial.reasoning_options;
        capability.temperature = partial.temperature;
        capability.temperature_max = partial.temperature_max;
        capability.web_search = partial.web_search;
        capability.web_search_name = partial.web_search_name;
        capability.images = partial.images;
        capability.load_order = candidate.model_catalog.next_load_order++;
        candidate.model_catalog.models.push_back(std::move(capability));
    }
    for (const auto& item : presets) {
        const PartialPreset& partial = item.second;
        if (!partial.have_model_id) return catalog_required(partial.source, "preset", "model_id");
        if (!partial.have_purpose) return catalog_required(partial.source, "preset", "purpose");
        erase_matching(candidate.model_catalog.presets, [&](const ModelSetting& value) {
            return value.model_id == partial.preset.model_id && value.purpose == partial.preset.purpose;
        });
        if (partial.preset.enabled) candidate.model_catalog.presets.push_back(partial.preset);
    }
    return ok_error();
}

Error split_pipe_list(const Entry& entry,
                      std::vector<std::string>& values,
                      const char* what,
                      bool lowercase = true) {
    Error err = require_type(entry, Value::Type::String);
    if (!err.ok()) return err;
    values.clear();
    std::string text = trim_config_ascii(entry.value.string);
    if (text.empty()) return ok_error();
    size_t begin = 0;
    while (begin <= text.size()) {
        const size_t separator = text.find('|', begin);
        std::string token = trim_config_ascii(separator == std::string::npos
                                                 ? text.substr(begin)
                                                 : text.substr(begin, separator - begin));
        if (lowercase) token = lower_config_ascii(std::move(token));
        if (token.empty()) {
            return schema_error(entry, std::string(what) + " must not contain an empty entry");
        }
        const std::string lookup = lowercase ? token : lower_config_ascii(token);
        for (const std::string& existing : values) {
            if ((lowercase ? existing : lower_config_ascii(existing)) == lookup) {
                return schema_error(entry, std::string(what) + " contains duplicate " + token);
            }
        }
        values.push_back(token);
        if (separator == std::string::npos) break;
        begin = separator + 1;
    }
    return ok_error();
}

Error parse_size_classes_entry(const Entry& entry,
                               std::vector<std::pair<std::string, int>>& classes) {
    std::vector<std::string> tokens;
    Error err = split_pipe_list(entry, tokens, "size_classes");
    if (!err.ok()) return err;
    classes.clear();
    for (const std::string& token : tokens) {
        const size_t colon = token.find(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= token.size()) {
            return schema_error(entry, "size_classes entries must be NAME:EDGE (for example 2k:2048)");
        }
        const std::string name = token.substr(0, colon);
        int edge = 0;
        try {
            const long parsed = std::stol(token.substr(colon + 1));
            if (parsed <= 0 || parsed > std::numeric_limits<int>::max()) {
                return schema_error(entry, "size_classes edge must be a positive integer");
            }
            edge = static_cast<int>(parsed);
        } catch (const std::exception&) {
            return schema_error(entry, "size_classes edge must be a positive integer");
        }
        classes.emplace_back(name, edge);
    }
    return ok_error();
}

Error parse_size_enum_entry(const Entry& entry,
                            std::vector<std::pair<std::string, std::string>>& mapped) {
    std::vector<std::string> tokens;
    Error err = split_pipe_list(entry, tokens, "size_enum", false);
    if (!err.ok()) return err;
    mapped.clear();
    for (const std::string& token : tokens) {
        const size_t colon = token.find(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= token.size()) {
            return schema_error(entry, "size_enum entries must be CLASS:TOKEN (for example 2k:2K)");
        }
        const std::string name = lower_config_ascii(trim_config_ascii(token.substr(0, colon)));
        const std::string value = trim_config_ascii(token.substr(colon + 1));
        if (name.empty() || value.empty()) {
            return schema_error(entry, "size_enum entries must be CLASS:TOKEN (for example 2k:2K)");
        }
        for (const auto& existing : mapped) {
            if (existing.first == name) {
                return schema_error(entry, "size_enum contains duplicate " + name);
            }
        }
        mapped.emplace_back(name, value);
    }
    return ok_error();
}

Error parse_size_map_entry(const Entry& entry, std::vector<ImageSizeMapEntry>& mapped) {
    std::vector<std::string> tokens;
    Error err = split_pipe_list(entry, tokens, "size_map");
    if (!err.ok()) return err;
    mapped.clear();
    for (const std::string& token : tokens) {
        const size_t colon = token.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= token.size()) {
            return schema_error(entry, "size_map entries must be CLASS,AR:WIDTHxHEIGHT");
        }
        ImageSizeMapEntry item;
        item.output = token.substr(colon + 1);
        const std::string left = token.substr(0, colon);
        const size_t comma = left.find(',');
        if (comma == std::string::npos) {
            item.size_class = left;
        } else {
            item.size_class = left.substr(0, comma);
            item.aspect = left.substr(comma + 1);
        }
        if (item.size_class.empty() || item.output.empty()) {
            return schema_error(entry, "size_map entries must be CLASS,AR:WIDTHxHEIGHT");
        }
        mapped.push_back(std::move(item));
    }
    return ok_error();
}

Error apply_configured_image_catalog(const Document& document, cli::Options& candidate) {
    struct PartialImage {
        std::optional<std::string> id;
        std::string provider = "any";
        std::optional<std::string> regex;
        std::string api_model;
        std::optional<ImageProtocol> protocol;
        bool default_for_provider = false;
        bool edits = false;
        int max_input_images = 0;
        ImageSizeMode size_mode = ImageSizeMode::Pixels;
        std::vector<std::pair<std::string, int>> size_classes;
        std::vector<std::pair<std::string, std::string>> size_enum;
        std::vector<ImageSizeMapEntry> size_map;
        std::vector<std::string> sizes;
        std::vector<std::string> aspect_ratios;
        int max_edge = 0;
        long long min_pixels = 0;
        long long max_pixels = 0;
        int max_ratio = 0;
        int multiple = 16;
        bool have_multiple = false;
        std::vector<std::string> quality;
        std::vector<std::string> format;
        std::string format_default = "png";
        std::string prompt_field = "prompt";
        std::string width_field;
        std::string height_field;
        std::string size_field;
        std::string aspect_field;
        std::string format_field;
        std::string quality_field;
        std::string images_field;
        std::string defaults_json;
        int priority = 0;
        bool enabled = true;
        SourceLocation source;
    };

    std::map<size_t, PartialImage> images;
    for (const auto& item : document.entries) {
        const Entry& entry = item.second;
        size_t index = 0;
        std::string key;
        if (!repeatable_entry_parts(item.first, "image", index, key)) continue;
        PartialImage& partial = images[index];
        if (partial.source.path.empty()) partial.source = entry.source;
        if (key == "id" || key == "provider" || key == "model" || key == "api_model" ||
            key == "format_default" || key == "prompt_field" || key == "width_field" ||
            key == "height_field" || key == "size_field" || key == "aspect_field" ||
            key == "format_field" || key == "quality_field" || key == "images_field" ||
            key == "defaults_json") {
            Error err = require_type(entry, Value::Type::String);
            if (!err.ok()) return err;
            if (entry.value.string.empty()) return schema_error(entry, key + " must not be empty");
            if (key == "id") partial.id = entry.value.string;
            else if (key == "provider") partial.provider = entry.value.string;
            else if (key == "model") partial.regex = entry.value.string;
            else if (key == "api_model") partial.api_model = entry.value.string;
            else if (key == "format_default") {
                partial.format_default = lower_config_ascii(entry.value.string);
            } else if (key == "prompt_field") {
                partial.prompt_field = entry.value.string;
            } else if (key == "width_field") {
                partial.width_field = entry.value.string;
            } else if (key == "height_field") {
                partial.height_field = entry.value.string;
            } else if (key == "size_field") {
                partial.size_field = entry.value.string;
            } else if (key == "aspect_field") {
                partial.aspect_field = entry.value.string;
            } else if (key == "format_field") {
                partial.format_field = entry.value.string;
            } else if (key == "quality_field") {
                partial.quality_field = entry.value.string;
            } else if (key == "images_field") {
                partial.images_field = entry.value.string;
            } else {
                const json::ParseResult parsed = json::parse(entry.value.string);
                if (!parsed.error.ok() || !parsed.value.is_object()) {
                    return schema_error(entry, "defaults_json must be a JSON object");
                }
                partial.defaults_json = entry.value.string;
            }
        } else if (key == "protocol") {
            Error err = require_type(entry, Value::Type::String);
            if (!err.ok()) return err;
            ImageProtocol protocol;
            if (!parse_image_protocol(entry.value.string, protocol)) {
                return schema_error(entry, "unknown image protocol; expected " + image_protocol_names());
            }
            partial.protocol = protocol;
        } else if (key == "size_mode") {
            Error err = require_type(entry, Value::Type::String);
            if (!err.ok()) return err;
            ImageSizeMode mode;
            if (!parse_image_size_mode(entry.value.string, mode)) {
                return schema_error(entry, "size_mode must be pixels, enum, aspect, or width_height");
            }
            partial.size_mode = mode;
        } else if (key == "default" || key == "edits" || key == "enabled") {
            Error err = require_type(entry, Value::Type::Boolean);
            if (!err.ok()) return err;
            if (key == "default") partial.default_for_provider = entry.value.boolean;
            else if (key == "edits") partial.edits = entry.value.boolean;
            else partial.enabled = entry.value.boolean;
        } else if (key == "max_input_images" || key == "max_edge" || key == "max_ratio" ||
                   key == "multiple" || key == "priority") {
            int value = 0;
            Error err = nonnegative_int(entry, value);
            if (!err.ok()) return err;
            if (key == "max_input_images") partial.max_input_images = value;
            else if (key == "max_edge") partial.max_edge = value;
            else if (key == "max_ratio") partial.max_ratio = value;
            else if (key == "multiple") {
                if (value <= 0) return schema_error(entry, "multiple must be greater than zero");
                partial.multiple = value;
                partial.have_multiple = true;
            } else partial.priority = value;
        } else if (key == "min_pixels" || key == "max_pixels") {
            long long value = 0;
            Error err = nonnegative_long_long(entry, value);
            if (!err.ok()) return err;
            if (key == "min_pixels") partial.min_pixels = value;
            else partial.max_pixels = value;
        } else if (key == "size_classes") {
            Error err = parse_size_classes_entry(entry, partial.size_classes);
            if (!err.ok()) return err;
        } else if (key == "size_enum") {
            Error err = parse_size_enum_entry(entry, partial.size_enum);
            if (!err.ok()) return err;
        } else if (key == "size_map") {
            Error err = parse_size_map_entry(entry, partial.size_map);
            if (!err.ok()) return err;
        } else if (key == "sizes") {
            Error err = split_pipe_list(entry, partial.sizes, "sizes", false);
            if (!err.ok()) return err;
        } else if (key == "aspect_ratios") {
            Error err = split_pipe_list(entry, partial.aspect_ratios, "aspect_ratios");
            if (!err.ok()) return err;
        } else if (key == "quality") {
            Error err = split_pipe_list(entry, partial.quality, "quality");
            if (!err.ok()) return err;
        } else if (key == "format") {
            Error err = split_pipe_list(entry, partial.format, "format");
            if (!err.ok()) return err;
        } else {
            return schema_error(entry, "unknown [image] key");
        }
    }

    for (const auto& item : images) {
        const PartialImage& partial = item.second;
        if (!partial.id.has_value()) return catalog_required(partial.source, "image", "id");
        erase_matching(candidate.image_catalog.models,
                       [&](const ImageCapability& value) { return value.id == *partial.id; });
        if (!partial.enabled) continue;
        if (!partial.regex.has_value()) return catalog_required(partial.source, "image", "model");
        if (!partial.protocol.has_value()) return catalog_required(partial.source, "image", "protocol");
        if (partial.default_for_provider && partial.api_model.empty()) {
            return {ErrorCode::Config,
                    partial.source.path + ":" + std::to_string(partial.source.line) +
                        ": [image] " + *partial.id + " sets default = on but omits api_model"};
        }
        if (*partial.protocol == ImageProtocol::ReplicatePredictions && partial.api_model.empty()) {
            return {ErrorCode::Config,
                    partial.source.path + ":" + std::to_string(partial.source.line) +
                        ": [image] " + *partial.id +
                        " uses replicate_predictions but omits api_model (owner/name)"};
        }
        try {
            (void)std::regex(*partial.regex, std::regex::ECMAScript | std::regex::icase);
        } catch (const std::regex_error& err) {
            return {ErrorCode::Config,
                    partial.source.path + ":" + std::to_string(partial.source.line) + ":" +
                        std::to_string(partial.source.column) +
                        ": invalid model regex for [image] " + *partial.id + ": " + err.what()};
        }
        ImageCapability capability;
        capability.id = *partial.id;
        capability.provider = partial.provider;
        capability.model_regex = *partial.regex;
        capability.api_model = partial.api_model;
        capability.protocol = *partial.protocol;
        capability.default_for_provider = partial.default_for_provider;
        capability.edits = partial.edits;
        capability.max_input_images = partial.max_input_images;
        capability.size_mode = partial.size_mode;
        capability.size_classes = partial.size_classes;
        capability.size_enum = partial.size_enum;
        capability.size_map = partial.size_map;
        capability.sizes = partial.sizes;
        capability.aspect_ratios = partial.aspect_ratios;
        capability.max_edge = partial.max_edge;
        capability.min_pixels = partial.min_pixels;
        capability.max_pixels = partial.max_pixels;
        capability.max_ratio = partial.max_ratio;
        capability.multiple = partial.multiple;
        capability.quality = partial.quality;
        capability.format = partial.format;
        capability.format_default = partial.format_default;
        capability.prompt_field = partial.prompt_field;
        capability.width_field = partial.width_field;
        capability.height_field = partial.height_field;
        capability.size_field = partial.size_field;
        capability.aspect_field = partial.aspect_field;
        capability.format_field = partial.format_field;
        capability.quality_field = partial.quality_field;
        capability.images_field = partial.images_field;
        capability.defaults_json = partial.defaults_json;
        capability.priority = partial.priority;
        capability.load_order = candidate.image_catalog.next_load_order++;
        candidate.image_catalog.models.push_back(std::move(capability));
    }
    return ok_error();
}

Error apply_configured_assist_commands(const Document& document, cli::Options& candidate) {
    struct PartialCommand {
        std::optional<std::string> string;
        std::optional<std::vector<ainiux::editor::AssistCommandMode>> modes;
        std::optional<std::string> prompt;
        SourceLocation source;
    };

    std::map<size_t, PartialCommand> partial_commands;
    for (const auto& item : document.entries) {
        const std::string& name = item.first;
        if (name.rfind("command.", 0) != 0) {
            continue;
        }
        const std::string tail = name.substr(std::string("command.").size());
        const size_t dot = tail.find('.');
        if (dot == std::string::npos) {
            continue;
        }
        size_t index = 0;
        try {
            index = static_cast<size_t>(std::stoul(tail.substr(0, dot)));
        } catch (const std::exception&) {
            continue;
        }
        const std::string key = tail.substr(dot + 1);
        PartialCommand& partial = partial_commands[index];
        if (partial.source.path.empty()) {
            partial.source = item.second.source;
        }
        const Entry& entry = item.second;
        if (key == "string") {
            std::string command;
            Error err = validate_assist_command_string(entry, command);
            if (!err.ok()) {
                return err;
            }
            partial.string = std::move(command);
        } else if (key == "modes") {
            std::vector<ainiux::editor::AssistCommandMode> modes;
            Error err = parse_assist_command_modes(entry, modes);
            if (!err.ok()) {
                return err;
            }
            partial.modes = std::move(modes);
        } else if (key == "prompt") {
            Error err = require_type(entry, Value::Type::String);
            if (!err.ok()) {
                return err;
            }
            partial.prompt = trim_config_ascii(entry.value.string);
            if (partial.prompt->empty()) {
                return schema_error(entry, "prompt must not be empty");
            }
        } else {
            return schema_error(entry, "unknown [command] key; expected string, modes, or prompt");
        }
    }

    for (const auto& item : partial_commands) {
        const PartialCommand& partial = item.second;
        if (!partial.string.has_value()) {
            return {ErrorCode::Config,
                    partial.source.path + ":" + std::to_string(partial.source.line) + ":" +
                        std::to_string(partial.source.column) +
                        ": invalid config setting [command]: string is required"};
        }
        if (!partial.prompt.has_value()) {
            return {ErrorCode::Config,
                    partial.source.path + ":" + std::to_string(partial.source.line) + ":" +
                        std::to_string(partial.source.column) +
                        ": invalid config setting [command] " + *partial.string + ": prompt is required"};
        }
        const std::vector<ainiux::editor::AssistCommandMode>& modes =
            partial.modes.has_value() ? *partial.modes : ainiux::editor::standard_assist_modes();
        merge_assist_command(candidate.editor_assist_config,
                             {*partial.string, modes, *partial.prompt});
    }
    return ok_error();
}

Error apply_config_file(const std::string& path, cli::Options& options) {
    ParseResult parsed = read_file(path);
    if (!parsed.error.ok()) {
        return parsed.error;
    }
    return apply_document(parsed.document, options);
}

Error apply_editor_commands_document_impl(const Document& document, cli::Options& options) {
    cli::Options candidate = options;
    for (const auto& item : document.entries) {
        const std::string& name = item.first;
        if (name.rfind("command.", 0) == 0) {
            continue;
        }
        if (!is_editor_commands_entry(name)) {
            return schema_error(item.second, "unknown editor-commands setting");
        }
        Error err = apply_editor_assist_entry(name, item.second, candidate);
        if (!err.ok()) {
            return err;
        }
    }
    Error command_err = apply_configured_assist_commands(document, candidate);
    if (!command_err.ok()) {
        return command_err;
    }
    options = std::move(candidate);
    return ok_error();
}

Error apply_themes_document_impl(const Document& document, cli::Options& options) {
    cli::Options candidate = options;
    for (const auto& item : document.entries) {
        if (!is_themes_entry(item.first)) {
            return schema_error(item.second, "unknown themes setting");
        }
    }
    Error theme_err = apply_configured_themes(document, candidate);
    if (!theme_err.ok()) {
        return theme_err;
    }
    options = std::move(candidate);
    return ok_error();
}

Error apply_benchmarks_document_impl(const Document& document, cli::Options& options) {
    cli::Options candidate = options;
    constexpr const char* kPlaceholder = "{{benchmark_case_json}}";
    for (const auto& item : document.entries) {
        const std::string& name = item.first;
        const Entry& entry = item.second;
        if (name == "config_version") {
            Error err = require_type(entry, Value::Type::Integer);
            if (!err.ok()) {
                return err;
            }
            if (entry.value.integer != 1) {
                return schema_error(entry,
                                    "unsupported config version " +
                                        std::to_string(entry.value.integer) +
                                        "; supported version is 1");
            }
            continue;
        }
        if (name != "grading.system_prompt" && name != "grading.case_prompt") {
            return schema_error(entry, "unknown benchmark-prompt setting");
        }
        Error err = require_type(entry, Value::Type::String);
        if (!err.ok()) {
            return err;
        }
        if (trim_config_ascii(entry.value.string).empty()) {
            return schema_error(entry, "grading prompt must not be empty");
        }
        if (name == "grading.system_prompt") {
            candidate.benchmark_grading_prompts.system_prompt = entry.value.string;
            continue;
        }
        const size_t first = entry.value.string.find(kPlaceholder);
        if (first == std::string::npos) {
            return schema_error(entry,
                                "grading.case_prompt must contain {{benchmark_case_json}} exactly once");
        }
        if (entry.value.string.find(kPlaceholder, first + std::char_traits<char>::length(kPlaceholder)) !=
            std::string::npos) {
            return schema_error(entry,
                                "grading.case_prompt must contain {{benchmark_case_json}} exactly once");
        }
        candidate.benchmark_grading_prompts.case_prompt = entry.value.string;
    }
    options = std::move(candidate);
    return ok_error();
}

Error apply_models_document_impl(const Document& document, cli::Options& options) {
    cli::Options candidate = options;
    for (const auto& item : document.entries) {
        const std::string& name = item.first;
        const Entry& entry = item.second;
        if (name == "config_version") {
            Error err = require_type(entry, Value::Type::Integer);
            if (!err.ok()) return err;
            if (entry.value.integer != 1) {
                return schema_error(entry,
                                    "unsupported models config version " +
                                        std::to_string(entry.value.integer) +
                                        "; supported version is 1");
            }
            continue;
        }
        if (name.rfind("model.", 0) != 0 && name.rfind("preset.", 0) != 0) {
            return schema_error(entry,
                                "unknown models setting; expected [model] or [preset]");
        }
    }
    Error err = apply_configured_model_catalog(document, candidate);
    if (!err.ok()) return err;
    options = std::move(candidate);
    return ok_error();
}

Error apply_images_document_impl(const Document& document, cli::Options& options) {
    cli::Options candidate = options;
    for (const auto& item : document.entries) {
        const std::string& name = item.first;
        const Entry& entry = item.second;
        if (name == "config_version") {
            Error err = require_type(entry, Value::Type::Integer);
            if (!err.ok()) return err;
            if (entry.value.integer != 1) {
                return schema_error(entry,
                                    "unsupported images config version " +
                                        std::to_string(entry.value.integer) +
                                        "; supported version is 1");
            }
            continue;
        }
        if (name.rfind("image.", 0) != 0) {
            return schema_error(entry, "unknown images setting; expected [image]");
        }
    }
    Error err = apply_configured_image_catalog(document, candidate);
    if (!err.ok()) return err;
    options = std::move(candidate);
    return ok_error();
}

}  // namespace

Error apply_editor_commands_document(const Document& document, cli::Options& options) {
    return apply_editor_commands_document_impl(document, options);
}

Error apply_themes_document(const Document& document, cli::Options& options) {
    return apply_themes_document_impl(document, options);
}

Error apply_benchmarks_document(const Document& document, cli::Options& options) {
    return apply_benchmarks_document_impl(document, options);
}

Error apply_models_document(const Document& document, cli::Options& options) {
    return apply_models_document_impl(document, options);
}

Error apply_images_document(const Document& document, cli::Options& options) {
    return apply_images_document_impl(document, options);
}

Error validate_benchmark_grading_prompts(const cli::BenchmarkGradingPrompts& prompts) {
    if (trim_config_ascii(prompts.system_prompt).empty()) {
        return {ErrorCode::Config,
                "benchmark grading system prompt is unavailable; install config/benchmarks.conf "
                "or share/ainiux/benchmarks.conf, or configure [grading].system_prompt"};
    }
    if (trim_config_ascii(prompts.case_prompt).empty()) {
        return {ErrorCode::Config,
                "benchmark grading case prompt is unavailable; install config/benchmarks.conf "
                "or share/ainiux/benchmarks.conf, or configure [grading].case_prompt"};
    }
    constexpr const char* kPlaceholder = "{{benchmark_case_json}}";
    const size_t first = prompts.case_prompt.find(kPlaceholder);
    if (first == std::string::npos ||
        prompts.case_prompt.find(kPlaceholder,
                                 first + std::char_traits<char>::length(kPlaceholder)) !=
            std::string::npos) {
        return {ErrorCode::Config,
                "benchmark grading case prompt must contain {{benchmark_case_json}} exactly once"};
    }
    return ok_error();
}

const Entry* Document::find(const std::string& qualified_key) const {
    const auto it = entries.find(qualified_key);
    return it == entries.end() ? nullptr : &it->second;
}

const Entry* Document::find(const std::string& section, const std::string& key) const {
    return find(section.empty() ? key : section + "." + key);
}

ParseResult parse(const std::string& input, const std::string& source_path) {
    return Parser(input, source_path).run();
}

ParseResult read_file(const std::string& path, size_t max_bytes) {
    std::error_code filesystem_error;
    const std::filesystem::path native_path = std::filesystem::u8path(path);
    const std::filesystem::file_status status =
        std::filesystem::status(native_path, filesystem_error);
    if (filesystem_error || !std::filesystem::exists(status)) {
        return {{}, {ErrorCode::Config, "could not inspect config file: " + path}};
    }
    if (!std::filesystem::is_regular_file(status)) {
        return {{}, {ErrorCode::Config, "config path is not a regular file: " + path}};
    }
    const std::uintmax_t file_size =
        std::filesystem::file_size(native_path, filesystem_error);
    if (filesystem_error) {
        return {{}, {ErrorCode::Config, "could not determine config file size: " + path}};
    }
    if (file_size > max_bytes) {
        return {{}, {ErrorCode::Config,
                     "config file exceeds " + std::to_string(max_bytes) + " byte limit: " + path}};
    }

    std::ifstream file(native_path, std::ios::binary);
    if (!file) {
        return {{}, {ErrorCode::Config, "could not open config file for reading: " + path}};
    }
    std::string input;
    input.reserve(static_cast<size_t>(file_size));
    std::array<char, 8192> buffer{};
    while (file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = file.gcount();
        if (count <= 0) {
            break;
        }
        const size_t chunk_size = static_cast<size_t>(count);
        if (input.size() > max_bytes || chunk_size > max_bytes - input.size()) {
            return {{}, {ErrorCode::Config,
                         "config file exceeds " + std::to_string(max_bytes) + " byte limit: " + path}};
        }
        input.append(buffer.data(), chunk_size);
    }
    if (file.bad()) {
        return {{}, {ErrorCode::Config, "could not read config file: " + path}};
    }
    return parse(input, path);
}

Error apply_document(const Document& document, cli::Options& options, bool user_layer) {
    cli::Options candidate = options;
    for (const auto& item : document.entries) {
        const std::string& name = item.first;
        const Entry& entry = item.second;
        Error err = ok_error();

        if (name == "config_version") {
            err = require_type(entry, Value::Type::Integer);
            if (err.ok() && entry.value.integer != 1) {
                err = schema_error(entry, "unsupported config version " + std::to_string(entry.value.integer) +
                                              "; supported version is 1");
            }
        } else if (name == "provider") {
            err = require_type(entry, Value::Type::String);
            if (err.ok() && entry.value.string.empty()) {
                err = schema_error(entry, "provider must not be empty");
            } else if (err.ok()) {
                candidate.provider = entry.value.string;
            }
        } else if (name == "model") {
            err = require_type(entry, Value::Type::String);
            if (err.ok()) candidate.model = entry.value.string;
        } else if (name == "api") {
            std::string value;
            err = enum_string(entry, {"chat", "responses"}, value, "chat or responses");
            if (err.ok()) {
                candidate.api = value;
                if (user_layer) candidate.api_explicit = true;
            }
        } else if (name == "endpoint.base_url") {
            err = require_type(entry, Value::Type::String);
            if (err.ok()) candidate.base_url = entry.value.string;
        } else if (name == "endpoint.chat_url") {
            err = require_type(entry, Value::Type::String);
            if (err.ok()) candidate.chat_url = entry.value.string;
        } else if (name == "endpoint.models_url") {
            err = require_type(entry, Value::Type::String);
            if (err.ok()) candidate.models_url = entry.value.string;
        } else if (name == "endpoint.responses_url") {
            err = require_type(entry, Value::Type::String);
            if (err.ok()) candidate.responses_url = entry.value.string;
        } else if (name == "generation.stream") {
            err = require_type(entry, Value::Type::Boolean);
            if (err.ok()) {
                candidate.stream = entry.value.boolean;
                candidate.stream_explicit = true;
            }
        } else if (name == "generation.temperature") {
            err = numeric_double(entry, candidate.temperature);
            if (err.ok()) {
                candidate.has_temperature = true;
                candidate.temperature_preset_applied = false;
            }
        } else if (name == "generation.top_p") {
            err = numeric_double(entry, candidate.top_p);
            if (err.ok()) candidate.has_top_p = true;
        } else if (name == "generation.max_output_tokens") {
            err = nonnegative_int(entry, candidate.max_output_tokens);
            if (err.ok()) candidate.has_max_output_tokens = true;
        } else if (name == "generation.reasoning") {
            err = reasoning_selection_entry(entry, candidate.reasoning, true);
            if (err.ok()) candidate.reasoning_explicit = true;
        } else if (name == "context.window_tokens") {
            err = context_window_tokens(entry, candidate.context_tokens);
            if (err.ok()) {
                // 0 means automatic discovery from /models or the model catalog.
                // Only positive values lock an explicit override.
                candidate.has_context_tokens = candidate.context_tokens > 0;
            }
        } else if (name == "context.max_bytes") {
            err = nonnegative_long(entry, candidate.max_context_bytes);
        } else if (name == "context.policy") {
            err = enum_string(entry,
                              context::policy::value_strings(),
                              candidate.context_policy,
                              context::policy::usage_description());
        } else if (name == "network.connect_timeout_seconds") {
            err = nonnegative_long(entry, candidate.connect_timeout_seconds);
        } else if (name == "network.request_timeout_seconds") {
            err = nonnegative_long(entry, candidate.timeout_seconds);
        } else if (name == "network.proxy") {
            err = require_type(entry, Value::Type::String);
            if (err.ok()) candidate.proxy = entry.value.string;
        } else if (name == "network.insecure_tls") {
            err = require_type(entry, Value::Type::Boolean);
            if (err.ok()) candidate.insecure_tls = entry.value.boolean;
        } else if (name == "credentials.key_env") {
            err = require_type(entry, Value::Type::String);
            if (err.ok()) candidate.key_env = entry.value.string;
        } else if (name == "credentials.key_file") {
            err = require_type(entry, Value::Type::String);
            if (err.ok()) candidate.key_file = entry.value.string;
        } else if (name == "output.format") {
            std::string value;
            err = enum_string(entry, {"text", "json", "ndjson", "jsond"}, value,
                              "text, json, ndjson, or jsond");
            if (err.ok()) {
                candidate.format = value == "text" ? cli::OutputFormat::Text
                                   : value == "json" ? cli::OutputFormat::Json
                                                     : cli::OutputFormat::Ndjson;
            }
        } else if (name == "output.render_format") {
            err = require_type(entry, Value::Type::String);
            if (err.ok() && !ainiux::markdown::parse_output_format(entry.value.string, candidate.output_format)) {
                err = schema_error(entry, "expected html, md, or plaintext");
            } else if (err.ok()) {
                const bool rendered = candidate.output_format != ainiux::markdown::OutputFormat::Markdown;
                candidate.output_format_explicit = rendered;
                candidate.rendered_output_format_explicit = rendered;
            }
        } else if (name == "input.max_input_bytes") {
            err = nonnegative_long(entry, candidate.max_input_bytes);
        } else if (name == "input.max_image_bytes") {
            err = nonnegative_long(entry, candidate.max_image_bytes);
        } else if (name == "index.max_source_code_file_size") {
            long long value = 0;
            err = auto_save_byte_size(entry, value);
            if (err.ok() && static_cast<unsigned long long>(value) >
                                static_cast<unsigned long long>(std::numeric_limits<size_t>::max())) {
                err = schema_error(entry, "byte size is too large for this platform");
            }
            if (err.ok()) candidate.max_source_code_file_size = static_cast<size_t>(value);
        } else if (name == "agent.max_parallel_agents") {
            err = nonnegative_int(entry, candidate.max_parallel_agents);
            if (err.ok() && (candidate.max_parallel_agents < 1 ||
                             candidate.max_parallel_agents > 32)) {
                err = schema_error(entry, "expected an integer from 1 through 32");
            }
        } else if (name == "agent.max_turns") {
            err = nonnegative_int(entry, candidate.agent_max_turns);
            if (err.ok() && (candidate.agent_max_turns < 1 ||
                             candidate.agent_max_turns > 500)) {
                err = schema_error(entry, "expected an integer from 1 through 500");
            }
        } else if (name == "agent.security_review_batch_size") {
            long long value = 0;
            err = auto_save_byte_size(entry, value);
            constexpr long long kMaximumReviewBatchBytes = 16LL * 1024LL * 1024LL;
            if (err.ok() && (value <= 0 || value > kMaximumReviewBatchBytes)) {
                err = schema_error(entry, "expected a positive byte size no larger than 16M");
            }
            if (err.ok()) candidate.security_review_batch_size = static_cast<size_t>(value);
        } else if (name == "agent.security_review_log_enabled") {
            err = require_type(entry, Value::Type::Boolean);
            if (err.ok()) candidate.security_review_log_enabled = entry.value.boolean;
        } else if (name == "agent.security_review_log_keep_runs") {
            err = nonnegative_int(entry, candidate.security_review_log_keep_runs);
            if (err.ok() && candidate.security_review_log_keep_runs > 1000)
                err = schema_error(entry, "expected an integer from 0 through 1000");
        } else if (name == "agent.history_backup_enabled") {
            err = require_type(entry, Value::Type::Boolean);
            if (err.ok()) candidate.agent_history_backup_enabled = entry.value.boolean;
        } else if (name == "agent.history_backup_max_bytes") {
            long long value = 0;
            err = auto_save_byte_size(entry, value);
            if (err.ok() && value < 0)
                err = schema_error(entry, "expected a non-negative byte size");
            if (err.ok())
                candidate.agent_history_backup_max_bytes = static_cast<size_t>(value);
        } else if (name == "agent.history_backup_ttl_days") {
            err = nonnegative_int(entry, candidate.agent_history_backup_ttl_days);
            if (err.ok() && candidate.agent_history_backup_ttl_days > 3650)
                err = schema_error(entry, "expected an integer from 0 through 3650");
        } else if (name == "agent.auto_compact") {
            err = require_type(entry, Value::Type::Boolean);
            if (err.ok()) candidate.agent_auto_compact = entry.value.boolean;
        } else if (name == "agent.compact_strategy") {
            err = require_type(entry, Value::Type::String);
            if (err.ok() &&
                !agent::parse_compaction_strategy(entry.value.string,
                                                  candidate.agent_compact_strategy)) {
                err = schema_error(entry, "expected fast, smart, or summary");
            }
        } else if (name == "agent.compact_limit") {
            err = nonnegative_int(entry, candidate.agent_compact_limit);
            if (err.ok() && candidate.agent_compact_limit > 100)
                err = schema_error(entry, "expected an integer from 0 through 100 (0=auto)");
        } else if (name == "agent.show_command_output") {
            err = require_type(entry, Value::Type::Boolean);
            if (err.ok()) candidate.agent_show_command_output = entry.value.boolean;
        } else if (name == "agent.max_response_bytes") {
            long long value = 0;
            err = auto_save_byte_size(entry, value);
            if (err.ok() &&
                static_cast<unsigned long long>(value) >
                    static_cast<unsigned long long>(std::numeric_limits<long>::max())) {
                err = schema_error(entry, "expected a non-negative byte size in the platform long range");
            }
            if (err.ok()) candidate.agent_max_response_bytes = static_cast<long>(value);
        } else if (name == "input.image_capability") {
            err = enum_string(entry,
                              cli::option_values::image_capability_strings(),
                              candidate.image_capability,
                              cli::option_values::image_capability_description());
        } else if (name == "input.auto-convert-html-to-md") {
            err = auto_save_mode(entry, candidate.auto_convert_html_to_markdown);
        } else if (name == "media.expiration_days") {
            err = nonnegative_int(entry, candidate.media_expiration_days);
        } else if (name == "media.auto_expiration_days") {
            err = nonnegative_int(entry, candidate.media_auto_expiration_days);
        } else if (name == "media.max_size_to_store_to_db") {
            err = nonnegative_long(entry, candidate.media_max_size_to_store_to_db);
        } else if (name == "editor.undo_limit") {
            err = nonnegative_int(entry, candidate.editor_undo_limit);
        } else if (name == "editor.huge_file_size_warning") {
            err = nonnegative_long_long(entry, candidate.editor_huge_file_size_warning);
        } else if (name == "editor.file_size_limit") {
            err = editor_file_size_limit(entry, candidate.editor_file_size_limit);
        } else if (name == "editor.auto-save-mode") {
            err = auto_save_mode(entry, candidate.editor_auto_save_mode);
        } else if (name == "editor.auto-save-postfix") {
            err = require_type(entry, Value::Type::String);
            if (err.ok()) {
                candidate.editor_auto_save_postfix = entry.value.string;
            }
        } else if (name == "editor.auto-save-threshold") {
            int threshold = 0;
            err = nonnegative_int(entry, threshold);
            if (err.ok()) {
                candidate.editor_auto_save_threshold = static_cast<size_t>(threshold);
            }
        } else if (name == "editor.auto-save-timeout") {
            err = nonnegative_int(entry, candidate.editor_auto_save_timeout_seconds);
        } else if (name == "editor.auto-save-size-limit") {
            err = auto_save_byte_size(entry, candidate.editor_auto_save_size_limit);
        } else if (name == "editor.tab-width") {
            int width = 0;
            err = nonnegative_int(entry, width);
            if (err.ok() &&
                (width < 1 || width > static_cast<int>(editor::kMaxTabWidth))) {
                err = schema_error(entry, "expected an integer from 1 through 32");
            }
            if (err.ok()) {
                candidate.editor_tab_width = static_cast<size_t>(width);
            }
        } else if (name == "editor.alignment-width" || name == "editor.text-align-width") {
            int width = 0;
            err = nonnegative_int(entry, width);
            if (err.ok() &&
                (width <= static_cast<int>(editor::kMinTextAlignWidthExclusive) ||
                 width > static_cast<int>(editor::kMaxTextAlignWidth))) {
                err = schema_error(entry,
                                   "expected an integer greater than 20 and at most 1000");
            }
            if (err.ok()) {
                candidate.editor_text_align_width = static_cast<size_t>(width);
            }
        } else if (name == "editor.tab-style") {
            err = require_type(entry, Value::Type::String);
            if (err.ok() &&
                !editor::parse_tab_style(entry.value.string, candidate.editor_tab_style)) {
                err = schema_error(entry, "expected spaces or tab");
            }
        } else if (name == "editor.linebreak") {
            err = require_type(entry, Value::Type::String);
            if (err.ok() &&
                !editor::parse_linebreak(entry.value.string, candidate.editor_linebreak)) {
                err = schema_error(entry, "expected lf, cr, or crlf");
            }
        } else if (name == "editor.continue_prefix_max_chars") {
            long long value = 0;
            err = nonnegative_long_long(entry, value);
            if (err.ok() && static_cast<unsigned long long>(value) >
                                static_cast<unsigned long long>(
                                    std::numeric_limits<size_t>::max())) {
                err = schema_error(entry, "integer is too large for this platform");
            }
            if (err.ok()) {
                candidate.editor_ai_continue_prefix_max_chars = static_cast<size_t>(value);
            }
        } else if (name == "editor.continue_postfix_max_chars") {
            long long value = 0;
            err = nonnegative_long_long(entry, value);
            if (err.ok() && static_cast<unsigned long long>(value) >
                                static_cast<unsigned long long>(
                                    std::numeric_limits<size_t>::max())) {
                err = schema_error(entry, "integer is too large for this platform");
            }
            if (err.ok()) {
                candidate.editor_ai_continue_postfix_max_chars = static_cast<size_t>(value);
            }
        } else if (name == "editor.continue_prose_prefix_max_chars") {
            long long value = 0;
            err = nonnegative_long_long(entry, value);
            if (err.ok() && static_cast<unsigned long long>(value) >
                                static_cast<unsigned long long>(
                                    std::numeric_limits<size_t>::max())) {
                err = schema_error(entry, "integer is too large for this platform");
            }
            if (err.ok()) {
                candidate.editor_ai_continue_prose_prefix_max_chars = static_cast<size_t>(value);
            }
        } else if (name == "editor.continue_prose_postfix_max_chars") {
            long long value = 0;
            err = nonnegative_long_long(entry, value);
            if (err.ok() && static_cast<unsigned long long>(value) >
                                static_cast<unsigned long long>(
                                    std::numeric_limits<size_t>::max())) {
                err = schema_error(entry, "integer is too large for this platform");
            }
            if (err.ok()) {
                candidate.editor_ai_continue_prose_postfix_max_chars = static_cast<size_t>(value);
            }
        } else if (name == "editor.continue_max_tokens") {
            err = nonnegative_int(entry, candidate.editor_ai_continue_max_tokens);
        } else if (name == "editor.assist_behavior") {
            err = require_type(entry, Value::Type::String);
            if (err.ok()) candidate.editor_assist_config.behavior_rules = entry.value.string;
        } else if (name == "editor.assist_spell") {
            err = apply_legacy_assist_prompt(entry, candidate.editor_assist_config, "/spell");
        } else if (name == "editor.assist_grammar") {
            err = apply_legacy_assist_prompt(entry, candidate.editor_assist_config, "/grammar");
        } else if (name == "editor.assist_continue") {
            err = apply_legacy_assist_prompt(entry, candidate.editor_assist_config, "/continue");
        } else if (name == "editor.assist_fact") {
            err = apply_legacy_assist_prompt(entry, candidate.editor_assist_config, "/fact");
        } else if (name == "editor.assist_comment") {
            err = apply_legacy_assist_prompt(entry, candidate.editor_assist_config, "/comment");
        } else if (name == "editor.assist_rewrite") {
            err = apply_legacy_assist_prompt(entry, candidate.editor_assist_config, "/rewrite");
        } else if (name == "editor.assist_english") {
            err = apply_legacy_assist_prompt(entry, candidate.editor_assist_config, "/English");
        } else if (name == "editor.assist_chinese") {
            err = apply_legacy_assist_prompt(entry, candidate.editor_assist_config, "/Chinese");
        } else if (name == "editor.assist_finnish") {
            err = apply_legacy_assist_prompt(entry, candidate.editor_assist_config, "/Finnish");
        } else if (name.rfind("command.", 0) == 0) {
            continue;
        } else if (name.rfind("theme.", 0) == 0) {
            continue;
        } else if (name == "url_fetch.max_bytes") {
            err = nonnegative_long(entry, candidate.max_fetch_bytes);
        } else if (name == "url_fetch.allow_private_addresses") {
            err = require_type(entry, Value::Type::Boolean);
            if (err.ok()) candidate.allow_private_url_fetch = entry.value.boolean;
        } else if (name == "web_search.builtin") {
            err = require_type(entry, Value::Type::Boolean);
            if (err.ok()) candidate.builtin_web_search = entry.value.boolean;
        } else if (name == "web_search.max_results") {
            err = nonnegative_int(entry, candidate.max_web_search_results);
            if (err.ok() && candidate.max_web_search_results <= 0) {
                err = schema_error(entry, "web_search.max_results expects a positive integer");
            }
        } else if (name == "web_search.provider") {
            err = require_type(entry, Value::Type::String);
            if (err.ok()) candidate.web_search_provider = entry.value.string;
        } else if (name == "web_search.tavily_key_env") {
            err = require_type(entry, Value::Type::String);
            if (err.ok()) candidate.tavily_key_env = entry.value.string;
        } else if (name == "web_search.firecrawl_key_env") {
            err = require_type(entry, Value::Type::String);
            if (err.ok()) candidate.firecrawl_key_env = entry.value.string;
        } else if (name == "web_search.exa_key_env") {
            err = require_type(entry, Value::Type::String);
            if (err.ok()) candidate.exa_key_env = entry.value.string;
        } else if (name == "web_search.exa_base_url") {
            err = require_type(entry, Value::Type::String);
            if (err.ok()) candidate.exa_base_url = entry.value.string;
        } else if (name == "web_search.searxng_base_url") {
            err = require_type(entry, Value::Type::String);
            if (err.ok()) candidate.searxng_base_url = entry.value.string;
        } else if (name == "tui.colors") {
            err = require_type(entry, Value::Type::Boolean);
            if (err.ok()) candidate.no_colors = !entry.value.boolean;
        } else if (name == "tui.color_mode") {
            tui::ColorModePreference preference = tui::ColorModePreference::Auto;
            if (entry.value.is_integer()) {
                // Bare 16 / 256 are integers in the TOML-alike grammar.
                if (entry.value.integer == 16) {
                    preference = tui::ColorModePreference::Ansi16;
                    err = ok_error();
                } else if (entry.value.integer == 256) {
                    preference = tui::ColorModePreference::Ansi256;
                    err = ok_error();
                } else {
                    err = schema_error(
                        entry, "tui.color_mode expects auto, truecolor, 256, or 16");
                }
            } else {
                err = require_type(entry, Value::Type::String);
                if (err.ok() &&
                    !tui::parse_color_mode_preference(entry.value.string, preference)) {
                    err = schema_error(
                        entry, "tui.color_mode expects auto, truecolor, 256, or 16");
                }
            }
            if (err.ok()) {
                candidate.color_mode = preference;
            }
        } else if (name == "tui.highlight") {
            err = auto_save_mode(entry, candidate.tui_highlight);
        } else if (name == "tui.theme") {
            if (entry.value.is_boolean() && !entry.value.boolean) {
                candidate.no_colors = true;
                err = ok_error();
            } else {
                err = require_type(entry, Value::Type::String);
            }
            if (err.ok() && entry.value.is_string()) {
                const std::string raw = trim_config_ascii(entry.value.string);
                if (lower_config_ascii(raw) == "off") {
                    // Same as /theme off and --theme off: keep the palette name.
                    candidate.no_colors = true;
                } else {
                    std::string normalized;
                    if (!candidate.tui_themes.normalize_name(raw, normalized)) {
                        err = schema_error(
                            entry,
                            "unknown theme; available: off, " +
                                tui::format_theme_list(candidate.tui_themes));
                    } else {
                        candidate.tui_theme = normalized;
                    }
                }
            }
        } else if (name == "tui.thinking_traces") {
            err = require_type(entry, Value::Type::Boolean);
            if (err.ok()) candidate.show_thinking_traces = entry.value.boolean;
        } else if (name == "tui.agent_input_max_height_percent") {
            err = nonnegative_int(entry, candidate.agent_input_max_height_percent);
            if (err.ok() && (candidate.agent_input_max_height_percent < 10 ||
                             candidate.agent_input_max_height_percent > 80)) {
                err = schema_error(
                    entry,
                    "tui.agent_input_max_height_percent expects an integer from 10 through 80");
            }
        } else if (name == "tui.agent_thinking_preview_max_chars") {
            err = nonnegative_int(entry, candidate.agent_thinking_preview_max_chars);
            if (err.ok() && candidate.agent_thinking_preview_max_chars > 1000) {
                err = schema_error(
                    entry,
                    "tui.agent_thinking_preview_max_chars expects an integer from 0 through 1000");
            }
        } else if (name == "tui.agent_thinking_idle_preview_seconds") {
            err = nonnegative_int(entry, candidate.agent_thinking_idle_preview_seconds);
            if (err.ok() && candidate.agent_thinking_idle_preview_seconds > 3600) {
                err = schema_error(
                    entry,
                    "tui.agent_thinking_idle_preview_seconds expects an integer from 0 through 3600");
            }
        } else if (name == "tui.agent_thinking_token_refresh_seconds") {
            err = nonnegative_int(entry, candidate.agent_thinking_token_refresh_seconds);
            if (err.ok() && candidate.agent_thinking_token_refresh_seconds > 3600) {
                err = schema_error(
                    entry,
                    "tui.agent_thinking_token_refresh_seconds expects an integer from 0 through 3600");
            }
        } else {
            err = schema_error(entry, "unknown section or key");
        }
        if (!err.ok()) {
            return err;
        }
    }
    Error command_err = apply_configured_assist_commands(document, candidate);
    if (!command_err.ok()) {
        return command_err;
    }
    options = std::move(candidate);
    return ok_error();
}

Environment process_environment() {
    return {platform::environment_value("XDG_CONFIG_HOME"), platform::home_directory()};
}

std::string user_config_path(const Environment& environment) {
    if (absolute_path(environment.xdg_config_home)) {
        return (std::filesystem::u8path(environment.xdg_config_home) / "ainiux" /
                "config.conf")
            .u8string();
    }
    if (!absolute_path(environment.home)) {
        return {};
    }
    return (std::filesystem::u8path(environment.home) / ".config" / "ainiux" /
            "config.conf")
        .u8string();
}

std::vector<std::string> bundled_config_paths() {
    std::vector<std::string> paths;
    // Development tree first so an in-repo binary picks up local templates.
    paths.emplace_back("config/ainiux.conf");
    append_executable_share_path(paths, "config.conf");
    append_installed_share_paths(paths, "config.conf");
    return paths;
}

std::string user_editor_commands_path(const Environment& environment) {
    if (absolute_path(environment.xdg_config_home)) {
        return (std::filesystem::u8path(environment.xdg_config_home) / "ainiux" /
                "editor-commands.conf")
            .u8string();
    }
    if (!absolute_path(environment.home)) {
        return {};
    }
    return (std::filesystem::u8path(environment.home) / ".config" / "ainiux" /
            "editor-commands.conf")
        .u8string();
}

std::vector<std::string> bundled_editor_commands_paths() {
    std::vector<std::string> paths;
    const std::string override_path = environment_value("AINIUX_EDITOR_COMMANDS");
    if (!override_path.empty()) paths.push_back(override_path);
    paths.emplace_back("config/editor-commands.conf");
    append_executable_share_path(paths, "editor-commands.conf");
    append_installed_share_paths(paths, "editor-commands.conf");
    return paths;
}

std::string user_themes_path(const Environment& environment) {
    if (absolute_path(environment.xdg_config_home)) {
        return (std::filesystem::u8path(environment.xdg_config_home) / "ainiux" /
                "themes.conf")
            .u8string();
    }
    if (!absolute_path(environment.home)) {
        return {};
    }
    return (std::filesystem::u8path(environment.home) / ".config" / "ainiux" /
            "themes.conf")
        .u8string();
}

std::vector<std::string> bundled_themes_paths() {
    std::vector<std::string> paths;
    const std::string override_path = environment_value("AINIUX_THEMES");
    if (!override_path.empty()) paths.push_back(override_path);
    paths.emplace_back("config/themes.conf");
    append_executable_share_path(paths, "themes.conf");
    append_installed_share_paths(paths, "themes.conf");
    return paths;
}

std::string user_benchmarks_path(const Environment& environment) {
    if (absolute_path(environment.xdg_config_home)) {
        return (std::filesystem::u8path(environment.xdg_config_home) / "ainiux" /
                "benchmarks.conf")
            .u8string();
    }
    if (!absolute_path(environment.home)) {
        return {};
    }
    return (std::filesystem::u8path(environment.home) / ".config" / "ainiux" /
            "benchmarks.conf")
        .u8string();
}

std::vector<std::string> bundled_benchmarks_paths() {
    std::vector<std::string> paths;
    const std::string override_path = environment_value("AINIUX_BENCHMARKS");
    if (!override_path.empty()) paths.push_back(override_path);
    paths.emplace_back("config/benchmarks.conf");
    append_executable_share_path(paths, "benchmarks.conf");
    append_installed_share_paths(paths, "benchmarks.conf");
    return paths;
}

std::string user_models_path(const Environment& environment) {
    if (absolute_path(environment.xdg_config_home)) {
        return (std::filesystem::u8path(environment.xdg_config_home) / "ainiux" /
                "models.conf")
            .u8string();
    }
    if (!absolute_path(environment.home)) return {};
    return (std::filesystem::u8path(environment.home) / ".config" / "ainiux" /
            "models.conf")
        .u8string();
}

std::vector<std::string> bundled_models_paths() {
    std::vector<std::string> paths;
    const std::string override_path = environment_value("AINIUX_MODELS");
    if (!override_path.empty()) paths.push_back(override_path);
    paths.emplace_back("config/models.conf");
    append_executable_share_path(paths, "models.conf");
    append_installed_share_paths(paths, "models.conf");
    return paths;
}

std::string user_images_path(const Environment& environment) {
    if (absolute_path(environment.xdg_config_home)) {
        return (std::filesystem::u8path(environment.xdg_config_home) / "ainiux" /
                "images.conf")
            .u8string();
    }
    if (!absolute_path(environment.home)) return {};
    return (std::filesystem::u8path(environment.home) / ".config" / "ainiux" /
            "images.conf")
        .u8string();
}

std::vector<std::string> bundled_images_paths() {
    std::vector<std::string> paths;
    const std::string override_path = environment_value("AINIUX_IMAGES");
    if (!override_path.empty()) paths.push_back(override_path);
    paths.emplace_back("config/images.conf");
    append_executable_share_path(paths, "images.conf");
    append_installed_share_paths(paths, "images.conf");
    return paths;
}

LoadResult load_automatic(const cli::Options& base_options,
                          const Environment& environment,
                          bool load_user_config) {
    LoadResult result{base_options, {}, {}, ok_error()};
    result.options.editor_assist_config = ainiux::editor::empty_editor_assist_config();
    result.options.tui_themes = tui::default_theme_registry();

    auto load_models_path = [&](const std::string& path, ConfigScope scope, bool& loaded) -> Error {
        std::error_code filesystem_error;
        const bool exists = std::filesystem::exists(path, filesystem_error);
        if (filesystem_error) {
            result.diagnostics.push_back({scope, ConfigFileKind::Models, ConfigFileState::Error, path});
            return {ErrorCode::Config, "could not inspect models file: " + path};
        }
        if (!exists) {
            result.diagnostics.push_back({scope, ConfigFileKind::Models, ConfigFileState::Missing, path});
            return ok_error();
        }
        ParseResult parsed = read_file(path);
        if (!parsed.error.ok()) {
            result.diagnostics.push_back({scope, ConfigFileKind::Models, ConfigFileState::Error, path});
            return parsed.error;
        }
        Error err = apply_models_document(parsed.document, result.options);
        if (!err.ok()) {
            result.diagnostics.push_back({scope, ConfigFileKind::Models, ConfigFileState::Error, path});
            return err;
        }
        loaded = true;
        result.loaded_paths.push_back(path);
        result.diagnostics.push_back({scope, ConfigFileKind::Models, ConfigFileState::Loaded, path});
        return ok_error();
    };

    bool bundled_models_loaded = false;
    for (const std::string& path : bundled_models_paths()) {
        Error err = load_models_path(path, ConfigScope::Bundled, bundled_models_loaded);
        if (!err.ok()) { result.error = std::move(err); return result; }
        if (bundled_models_loaded) break;
    }
    if (!bundled_models_loaded) {
        ParseResult parsed = parse(kEmbeddedModelsConfig, "embedded models.conf");
        if (!parsed.error.ok()) {
            result.diagnostics.push_back(
                {ConfigScope::Bundled, ConfigFileKind::Models,
                 ConfigFileState::Error, "embedded models.conf"});
            result.error = std::move(parsed.error);
            return result;
        }
        Error err = apply_models_document(parsed.document, result.options);
        if (!err.ok()) {
            result.diagnostics.push_back(
                {ConfigScope::Bundled, ConfigFileKind::Models,
                 ConfigFileState::Error, "embedded models.conf"});
            result.error = std::move(err);
            return result;
        }
        result.loaded_paths.push_back("embedded models.conf");
        result.diagnostics.push_back(
            {ConfigScope::Bundled, ConfigFileKind::Models,
             ConfigFileState::Loaded, "embedded models.conf"});
    }
    const std::string user_models = user_models_path(environment);
    if (user_models.empty()) {
        result.diagnostics.push_back(
            {ConfigScope::User, ConfigFileKind::Models, ConfigFileState::Unavailable, {}});
    } else if (!load_user_config) {
        result.diagnostics.push_back(
            {ConfigScope::User, ConfigFileKind::Models, ConfigFileState::Skipped, user_models});
    } else {
        bool user_models_loaded = false;
        Error err = load_models_path(user_models, ConfigScope::User, user_models_loaded);
        if (!err.ok()) { result.error = std::move(err); return result; }
        (void)user_models_loaded;
    }

    auto load_images_path = [&](const std::string& path, ConfigScope scope, bool& loaded) -> Error {
        std::error_code filesystem_error;
        const bool exists = std::filesystem::exists(path, filesystem_error);
        if (filesystem_error) {
            result.diagnostics.push_back({scope, ConfigFileKind::Images, ConfigFileState::Error, path});
            return {ErrorCode::Config, "could not inspect images file: " + path};
        }
        if (!exists) {
            result.diagnostics.push_back({scope, ConfigFileKind::Images, ConfigFileState::Missing, path});
            return ok_error();
        }
        ParseResult parsed = read_file(path);
        if (!parsed.error.ok()) {
            result.diagnostics.push_back({scope, ConfigFileKind::Images, ConfigFileState::Error, path});
            return parsed.error;
        }
        Error err = apply_images_document(parsed.document, result.options);
        if (!err.ok()) {
            result.diagnostics.push_back({scope, ConfigFileKind::Images, ConfigFileState::Error, path});
            return err;
        }
        loaded = true;
        result.loaded_paths.push_back(path);
        result.diagnostics.push_back({scope, ConfigFileKind::Images, ConfigFileState::Loaded, path});
        return ok_error();
    };

    bool bundled_images_loaded = false;
    for (const std::string& path : bundled_images_paths()) {
        Error err = load_images_path(path, ConfigScope::Bundled, bundled_images_loaded);
        if (!err.ok()) { result.error = std::move(err); return result; }
        if (bundled_images_loaded) break;
    }
    if (!bundled_images_loaded) {
        ParseResult parsed = parse(kEmbeddedImagesConfig, "embedded images.conf");
        if (!parsed.error.ok()) {
            result.diagnostics.push_back(
                {ConfigScope::Bundled, ConfigFileKind::Images,
                 ConfigFileState::Error, "embedded images.conf"});
            result.error = std::move(parsed.error);
            return result;
        }
        Error err = apply_images_document(parsed.document, result.options);
        if (!err.ok()) {
            result.diagnostics.push_back(
                {ConfigScope::Bundled, ConfigFileKind::Images,
                 ConfigFileState::Error, "embedded images.conf"});
            result.error = std::move(err);
            return result;
        }
        result.loaded_paths.push_back("embedded images.conf");
        result.diagnostics.push_back(
            {ConfigScope::Bundled, ConfigFileKind::Images,
             ConfigFileState::Loaded, "embedded images.conf"});
    }
    const std::string user_images = user_images_path(environment);
    if (user_images.empty()) {
        result.diagnostics.push_back(
            {ConfigScope::User, ConfigFileKind::Images, ConfigFileState::Unavailable, {}});
    } else if (!load_user_config) {
        result.diagnostics.push_back(
            {ConfigScope::User, ConfigFileKind::Images, ConfigFileState::Skipped, user_images});
    } else {
        bool user_images_loaded = false;
        Error err = load_images_path(user_images, ConfigScope::User, user_images_loaded);
        if (!err.ok()) { result.error = std::move(err); return result; }
        (void)user_images_loaded;
    }

    auto load_benchmarks_path = [&](const std::string& path,
                                    ConfigScope scope,
                                    bool& loaded) -> Error {
        std::error_code filesystem_error;
        const bool exists = std::filesystem::exists(path, filesystem_error);
        if (filesystem_error) {
            result.diagnostics.push_back(
                {scope, ConfigFileKind::Benchmarks, ConfigFileState::Error, path});
            return {ErrorCode::Config, "could not inspect benchmark-prompt file: " + path};
        }
        if (!exists) {
            result.diagnostics.push_back(
                {scope, ConfigFileKind::Benchmarks, ConfigFileState::Missing, path});
            return ok_error();
        }
        ParseResult parsed = read_file(path);
        if (!parsed.error.ok()) {
            result.diagnostics.push_back(
                {scope, ConfigFileKind::Benchmarks, ConfigFileState::Error, path});
            return parsed.error;
        }
        Error err = apply_benchmarks_document(parsed.document, result.options);
        if (!err.ok()) {
            result.diagnostics.push_back(
                {scope, ConfigFileKind::Benchmarks, ConfigFileState::Error, path});
            return err;
        }
        loaded = true;
        result.loaded_paths.push_back(path);
        result.diagnostics.push_back(
            {scope, ConfigFileKind::Benchmarks, ConfigFileState::Loaded, path});
        return ok_error();
    };

    bool bundled_benchmarks_loaded = false;
    for (const std::string& path : bundled_benchmarks_paths()) {
        Error err = load_benchmarks_path(path, ConfigScope::Bundled,
                                         bundled_benchmarks_loaded);
        if (!err.ok()) {
            result.error = std::move(err);
            return result;
        }
        if (bundled_benchmarks_loaded) {
            break;
        }
    }
    bool benchmark_prompts_loaded = bundled_benchmarks_loaded;
    const std::string user_benchmarks = user_benchmarks_path(environment);
    if (user_benchmarks.empty()) {
        result.diagnostics.push_back(
            {ConfigScope::User, ConfigFileKind::Benchmarks,
             ConfigFileState::Unavailable, {}});
    } else if (!load_user_config) {
        result.diagnostics.push_back(
            {ConfigScope::User, ConfigFileKind::Benchmarks, ConfigFileState::Skipped,
             user_benchmarks});
    } else {
        Error err = load_benchmarks_path(user_benchmarks, ConfigScope::User,
                                         benchmark_prompts_loaded);
        if (!err.ok()) {
            result.error = std::move(err);
            return result;
        }
    }

    auto load_themes_path = [&](const std::string& path, ConfigScope scope, bool& any_loaded) -> Error {
        std::error_code filesystem_error;
        const bool exists = std::filesystem::exists(path, filesystem_error);
        if (filesystem_error) {
            result.diagnostics.push_back({scope, ConfigFileKind::Themes, ConfigFileState::Error, path});
            return {ErrorCode::Config, "could not inspect themes file: " + path};
        }
        if (!exists) {
            result.diagnostics.push_back({scope, ConfigFileKind::Themes, ConfigFileState::Missing, path});
            return ok_error();
        }
        ParseResult parsed = read_file(path);
        if (!parsed.error.ok()) {
            result.diagnostics.push_back({scope, ConfigFileKind::Themes, ConfigFileState::Error, path});
            return parsed.error;
        }
        Error err = apply_themes_document(parsed.document, result.options);
        if (!err.ok()) {
            result.diagnostics.push_back({scope, ConfigFileKind::Themes, ConfigFileState::Error, path});
            return err;
        }
        any_loaded = true;
        result.loaded_paths.push_back(path);
        result.diagnostics.push_back({scope, ConfigFileKind::Themes, ConfigFileState::Loaded, path});
        return ok_error();
    };

    bool themes_loaded = false;
    for (const std::string& path : bundled_themes_paths()) {
        std::error_code filesystem_error;
        if (!std::filesystem::exists(path, filesystem_error) || filesystem_error) {
            continue;
        }
        Error err = load_themes_path(path, ConfigScope::Bundled, themes_loaded);
        if (!err.ok()) {
            result.error = std::move(err);
            return result;
        }
        if (themes_loaded) break;
    }

    const std::string user_themes = user_themes_path(environment);
    if (user_themes.empty()) {
        result.diagnostics.push_back(
            {ConfigScope::User, ConfigFileKind::Themes, ConfigFileState::Unavailable, {}});
    } else if (!load_user_config) {
        result.diagnostics.push_back(
            {ConfigScope::User, ConfigFileKind::Themes, ConfigFileState::Skipped, user_themes});
    } else {
        Error err = load_themes_path(user_themes, ConfigScope::User, themes_loaded);
        if (!err.ok()) {
            result.error = std::move(err);
            return result;
        }
    }

    if (!themes_loaded) {
        result.options.tui_themes = tui::default_theme_registry();
    }

    auto load_editor_commands_path = [&](const std::string& path, ConfigScope scope) -> Error {
        std::error_code filesystem_error;
        const bool exists = std::filesystem::exists(path, filesystem_error);
        if (filesystem_error) {
            result.diagnostics.push_back(
                {scope, ConfigFileKind::EditorCommands, ConfigFileState::Error, path});
            return {ErrorCode::Config, "could not inspect editor-commands file: " + path};
        }
        if (!exists) {
            result.diagnostics.push_back(
                {scope, ConfigFileKind::EditorCommands, ConfigFileState::Missing, path});
            return ok_error();
        }
        ParseResult parsed = read_file(path);
        if (!parsed.error.ok()) {
            result.diagnostics.push_back(
                {scope, ConfigFileKind::EditorCommands, ConfigFileState::Error, path});
            return parsed.error;
        }
        Error err = apply_editor_commands_document(parsed.document, result.options);
        if (!err.ok()) {
            result.diagnostics.push_back(
                {scope, ConfigFileKind::EditorCommands, ConfigFileState::Error, path});
            return err;
        }
        result.loaded_paths.push_back(path);
        result.diagnostics.push_back(
            {scope, ConfigFileKind::EditorCommands, ConfigFileState::Loaded, path});
        return ok_error();
    };

    // Load order matches models.conf: bundled (or embedded) first so the full
    // built-in command set is always available, then user overrides merge
    // on top. Previously a missing/outdated cwd or XDG file fell back to a
    // minimal C++ subset and dropped commands like /style-formal and /marketing.
    bool bundled_editor_commands_loaded = false;
    for (const std::string& path : bundled_editor_commands_paths()) {
        std::error_code filesystem_error;
        if (!std::filesystem::exists(path, filesystem_error) || filesystem_error) {
            continue;
        }
        Error err = load_editor_commands_path(path, ConfigScope::Bundled);
        if (!err.ok()) {
            result.error = std::move(err);
            return result;
        }
        bundled_editor_commands_loaded = true;
        break;
    }
    if (!bundled_editor_commands_loaded) {
        ParseResult parsed =
            parse(kEmbeddedEditorCommandsConfig, "embedded editor-commands.conf");
        if (!parsed.error.ok()) {
            result.diagnostics.push_back(
                {ConfigScope::Bundled, ConfigFileKind::EditorCommands, ConfigFileState::Error,
                 "embedded editor-commands.conf"});
            result.error = std::move(parsed.error);
            return result;
        }
        Error err = apply_editor_commands_document(parsed.document, result.options);
        if (!err.ok()) {
            result.diagnostics.push_back(
                {ConfigScope::Bundled, ConfigFileKind::EditorCommands, ConfigFileState::Error,
                 "embedded editor-commands.conf"});
            result.error = std::move(err);
            return result;
        }
        result.loaded_paths.push_back("embedded editor-commands.conf");
        result.diagnostics.push_back(
            {ConfigScope::Bundled, ConfigFileKind::EditorCommands, ConfigFileState::Loaded,
             "embedded editor-commands.conf"});
    }
    const std::string user_editor_commands = user_editor_commands_path(environment);
    if (user_editor_commands.empty()) {
        result.diagnostics.push_back(
            {ConfigScope::User, ConfigFileKind::EditorCommands, ConfigFileState::Unavailable, {}});
    } else if (!load_user_config) {
        result.diagnostics.push_back({ConfigScope::User,
                                      ConfigFileKind::EditorCommands,
                                      ConfigFileState::Skipped,
                                      user_editor_commands});
    } else {
        Error err = load_editor_commands_path(user_editor_commands, ConfigScope::User);
        if (!err.ok()) {
            result.error = std::move(err);
            return result;
        }
    }

    bool bundled_config_loaded = false;
    for (const std::string& path : bundled_config_paths()) {
        std::error_code filesystem_error;
        const bool exists = std::filesystem::exists(path, filesystem_error);
        if (filesystem_error) {
            continue;
        }
        if (!exists) {
            continue;
        }
        Error err = apply_config_file(path, result.options);
        if (!err.ok()) {
            result.diagnostics.push_back(
                {ConfigScope::Bundled, ConfigFileKind::Config, ConfigFileState::Error, path});
            result.error = std::move(err);
            return result;
        }
        result.loaded_paths.push_back(path);
        result.diagnostics.push_back(
            {ConfigScope::Bundled, ConfigFileKind::Config, ConfigFileState::Loaded, path});
        bundled_config_loaded = true;
        break;
    }
    if (!bundled_config_loaded) {
        result.diagnostics.push_back(
            {ConfigScope::Bundled, ConfigFileKind::Config, ConfigFileState::Unavailable,
             "config/ainiux.conf, ~/.local/share/ainiux/config.conf, "
             "/usr/local/share/ainiux/config.conf, /usr/share/ainiux/config.conf"});
    }

    const std::string user_path = user_config_path(environment);
    if (user_path.empty()) {
        result.diagnostics.push_back(
            {ConfigScope::User, ConfigFileKind::Config, ConfigFileState::Unavailable, {}});
        return result;
    }
    if (!load_user_config) {
        result.diagnostics.push_back(
            {ConfigScope::User, ConfigFileKind::Config, ConfigFileState::Skipped, user_path});
        return result;
    }

    std::error_code filesystem_error;
    const bool exists = std::filesystem::exists(user_path, filesystem_error);
    if (filesystem_error) {
        result.diagnostics.push_back(
            {ConfigScope::User, ConfigFileKind::Config, ConfigFileState::Error, user_path});
        result.error = {ErrorCode::Config, "could not inspect config file: " + user_path};
        return result;
    }
    if (!exists) {
        result.diagnostics.push_back(
            {ConfigScope::User, ConfigFileKind::Config, ConfigFileState::Missing, user_path});
        return result;
    }
    ParseResult user_parsed = read_file(user_path);
    if (!user_parsed.error.ok()) {
        result.diagnostics.push_back(
            {ConfigScope::User, ConfigFileKind::Config, ConfigFileState::Error, user_path});
        result.error = std::move(user_parsed.error);
        return result;
    }
    Error err = apply_document(user_parsed.document, result.options, true);
    if (!err.ok()) {
        result.diagnostics.push_back(
            {ConfigScope::User, ConfigFileKind::Config, ConfigFileState::Error, user_path});
        result.error = std::move(err);
        return result;
    }
    result.loaded_paths.push_back(user_path);
    result.diagnostics.push_back(
        {ConfigScope::User, ConfigFileKind::Config, ConfigFileState::Loaded, user_path});
    return result;
}

const char* value_type_name(Value::Type type) {
    switch (type) {
        case Value::Type::Boolean:
            return "boolean";
        case Value::Type::Integer:
            return "integer";
        case Value::Type::Float:
            return "float";
        case Value::Type::String:
            return "string";
    }
    return "string";
}

}  // namespace ainiux::config
