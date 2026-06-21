#include "config/config.hpp"

#include <array>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <locale>
#include <sstream>
#include <utility>

namespace pkchat::config {
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
    if (text == "true" || text == "false") {
        value.type = Value::Type::Boolean;
        value.boolean = text == "true";
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

            Error err = parse_line(line, line_number, section, document);
            if (!err.ok()) {
                return {{}, err};
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

    Error parse_line(const std::string& line,
                     size_t line_number,
                     std::string& section,
                     Document& document) const {
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
        Error err = parse_value(value_text, source_path_, line_number, value_begin + 1, value);
        if (!err.ok()) {
            return err;
        }

        const std::string qualified = section.empty() ? key : section + "." + key;
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

}  // namespace

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
    const std::filesystem::file_status status = std::filesystem::status(path, filesystem_error);
    if (filesystem_error || !std::filesystem::exists(status)) {
        return {{}, {ErrorCode::Config, "could not inspect config file: " + path}};
    }
    if (!std::filesystem::is_regular_file(status)) {
        return {{}, {ErrorCode::Config, "config path is not a regular file: " + path}};
    }
    const std::uintmax_t file_size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error) {
        return {{}, {ErrorCode::Config, "could not determine config file size: " + path}};
    }
    if (file_size > max_bytes) {
        return {{}, {ErrorCode::Config,
                     "config file exceeds " + std::to_string(max_bytes) + " byte limit: " + path}};
    }

    std::ifstream file(path, std::ios::binary);
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

}  // namespace pkchat::config
