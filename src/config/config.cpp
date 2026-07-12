#include "config/config.hpp"

#include "chat/generation_settings.hpp"
#include "cli/option_values.hpp"
#include "context/policy.hpp"
#include "editor/autosave.hpp"
#include "editor/editor_prompts.hpp"
#include "pkchat/model_setting.hpp"
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
    std::map<std::string, size_t> repeatable_section_next_instance_;
    std::map<std::string, size_t> repeatable_section_current_instance_;

    static bool is_repeatable_section(const std::string& name) {
        return name == "command" || name == "Model-setting" || name == "theme";
    }

    Error parse_line(const std::string& line,
                     size_t line_number,
                     std::string& section,
                     Document& document) {
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
        Error err = parse_value(value_text, source_path_, line_number, value_begin + 1, value);
        if (!err.ok()) {
            return err;
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
    Error err = pkchat::editor::parse_byte_size(entry.value.string, output);
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
    return !path.empty() && std::filesystem::path(path).is_absolute();
}

std::string environment_value(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
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
    if (!entry.value.is_string()) {
        return schema_error(entry, "expected boolean or on/off");
    }
    const std::string value = lower_config_ascii(trim_config_ascii(entry.value.string));
    if (value == "on" || value == "yes" || value == "true") {
        output = true;
        return ok_error();
    }
    if (value == "off" || value == "no" || value == "false") {
        output = false;
        return ok_error();
    }
    return schema_error(entry, "expected on, off, true, false, yes, or no");
}

pkchat::editor::EditorAssistCommand* find_assist_command_by_name(pkchat::editor::EditorAssistConfig& config,
                                                                const std::string& command) {
    std::string normalized = lower_config_ascii(trim_config_ascii(command));
    while (!normalized.empty() && normalized.front() == '/') {
        normalized.erase(normalized.begin());
    }
    for (pkchat::editor::EditorAssistCommand& entry : config.commands) {
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

Error parse_assist_command_modes(const Entry& entry, std::vector<pkchat::editor::AssistCommandMode>& modes) {
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
            modes.push_back(pkchat::editor::AssistCommandMode::Continue);
        } else if (token == "selection") {
            modes.push_back(pkchat::editor::AssistCommandMode::Selection);
        } else if (token == "all") {
            modes.push_back(pkchat::editor::AssistCommandMode::All);
        } else if (token == "insert" || token == "local_insert" || token == "localinsert") {
            modes.push_back(pkchat::editor::AssistCommandMode::Insert);
        } else if (token == "fact") {
            modes.push_back(pkchat::editor::AssistCommandMode::Fact);
        } else {
            return schema_error(entry, "expected continue, selection, all, insert, or fact");
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
    if (command.empty()) {
        return schema_error(entry, "command string must not be empty");
    }
    if (command.front() != '/') {
        command.insert(command.begin(), '/');
    }
    std::string normalized = command;
    normalized.erase(normalized.begin());
    if (normalized.empty()) {
        return schema_error(entry, "command string must include a name after /");
    }
    return ok_error();
}

void merge_assist_command(pkchat::editor::EditorAssistConfig& config, pkchat::editor::EditorAssistCommand command) {
    if (pkchat::editor::EditorAssistCommand* existing = find_assist_command_by_name(config, command.command)) {
        *existing = std::move(command);
        return;
    }
    config.commands.push_back(std::move(command));
}

Error apply_legacy_assist_prompt(const Entry& entry,
                                 pkchat::editor::EditorAssistConfig& config,
                                 const std::string& command_name) {
    Error err = require_type(entry, Value::Type::String);
    if (!err.ok()) {
        return err;
    }
    if (pkchat::editor::EditorAssistCommand* command = find_assist_command_by_name(config, command_name)) {
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
            Error err = require_type(entry, Value::Type::String);
            if (!err.ok()) {
                return err;
            }
            const std::string theme_name = trim_config_ascii(entry.value.string);
            if (theme_name.empty()) {
                return schema_error(entry, "name must not be empty");
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
        } else {
            return schema_error(entry,
                                "unknown [theme] key; expected name, background, text, muted, thinking_trace, "
                                "user_label, assistant_label, error, status_foreground, status_background, "
                                "thinking_activity, streaming_activity, panel_title, panel_border, panel_hint, "
                                "panel_highlight, panel_body, or panel_background");
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

std::vector<std::string> config_paths_from_dirs(const std::string& dirs, const char* filename) {
    std::vector<std::string> priority_order;
    size_t begin = 0;
    while (begin <= dirs.size()) {
        const size_t colon = dirs.find(':', begin);
        const size_t end = colon == std::string::npos ? dirs.size() : colon;
        const std::string dir = dirs.substr(begin, end - begin);
        if (absolute_path(dir)) {
            priority_order.push_back((std::filesystem::path(dir) / "pkchat" / filename).string());
        }
        if (colon == std::string::npos) {
            break;
        }
        begin = colon + 1;
    }
    return std::vector<std::string>(priority_order.rbegin(), priority_order.rend());
}

void merge_model_setting(std::vector<ModelSetting>& settings, ModelSetting setting) {
    for (ModelSetting& existing : settings) {
        if (existing.model == setting.model && existing.purpose == setting.purpose) {
            existing = std::move(setting);
            return;
        }
    }
    settings.push_back(std::move(setting));
}

Error apply_configured_model_settings(const Document& document, cli::Options& candidate) {
    struct PartialModelSetting {
        std::optional<std::string> model;
        std::optional<std::string> purpose;
        std::optional<std::string> default_system_prompt;
        std::optional<double> temperature;
        std::optional<int> top_k;
        std::optional<double> top_p;
        std::optional<double> min_p;
        std::optional<double> repeat_penalty;
        std::optional<double> presence_penalty;
        std::optional<std::string> thinking_budget;
        SourceLocation source;
    };

    std::map<size_t, PartialModelSetting> partial_settings;
    for (const auto& item : document.entries) {
        const std::string& name = item.first;
        if (name.rfind("Model-setting.", 0) != 0) {
            continue;
        }
        const std::string tail = name.substr(std::string("Model-setting.").size());
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
        PartialModelSetting& partial = partial_settings[index];
        if (partial.source.path.empty()) {
            partial.source = item.second.source;
        }
        const Entry& entry = item.second;
        if (key == "model") {
            Error err = require_type(entry, Value::Type::String);
            if (!err.ok()) {
                return err;
            }
            if (entry.value.string.empty()) {
                return schema_error(entry, "model must not be empty");
            }
            partial.model = entry.value.string;
        } else if (key == "purpose") {
            std::string purpose;
            Error err = enum_string(entry,
                                    chat::generation::chat_purpose_strings(),
                                    purpose,
                                    chat::generation::chat_purpose_description());
            if (!err.ok()) {
                return err;
            }
            partial.purpose = std::move(purpose);
        } else if (key == "default_system_prompt") {
            Error err = require_type(entry, Value::Type::String);
            if (!err.ok()) {
                return err;
            }
            partial.default_system_prompt = entry.value.string;
        } else if (key == chat::generation::kTemperature) {
            double value = 0.0;
            Error err = numeric_double(entry, value);
            if (!err.ok()) {
                return err;
            }
            partial.temperature = value;
        } else if (key == chat::generation::kTopK) {
            int value = 0;
            Error err = nonnegative_int(entry, value);
            if (!err.ok()) {
                return err;
            }
            partial.top_k = value;
        } else if (key == chat::generation::kTopP) {
            double value = 0.0;
            Error err = numeric_double(entry, value);
            if (!err.ok()) {
                return err;
            }
            partial.top_p = value;
        } else if (key == chat::generation::kMinP) {
            double value = 0.0;
            Error err = numeric_double(entry, value);
            if (!err.ok()) {
                return err;
            }
            partial.min_p = value;
        } else if (key == chat::generation::kRepeatPenalty) {
            double value = 0.0;
            Error err = numeric_double(entry, value);
            if (!err.ok()) {
                return err;
            }
            partial.repeat_penalty = value;
        } else if (key == chat::generation::kPresencePenalty) {
            double value = 0.0;
            Error err = numeric_double(entry, value);
            if (!err.ok()) {
                return err;
            }
            partial.presence_penalty = value;
        } else if (key == chat::generation::kThinkingBudget) {
            if (entry.value.is_string()) {
                if (entry.value.string.empty()) {
                    return schema_error(entry, "thinking_budget must not be empty");
                }
                partial.thinking_budget = entry.value.string;
            } else if (entry.value.is_integer()) {
                if (entry.value.integer < 0) {
                    return schema_error(entry, "thinking_budget token count must be non-negative");
                }
                partial.thinking_budget = std::to_string(entry.value.integer);
            } else {
                return schema_error(entry, "thinking_budget must be a token count or verbal label string");
            }
        } else {
            return schema_error(entry,
                                "unknown [Model-setting] key; expected " +
                                    chat::generation::model_setting_keys_description());
        }
    }

    for (const auto& item : partial_settings) {
        const PartialModelSetting& partial = item.second;
        const auto required_error = [&](const char* field) {
            return Error{ErrorCode::Config,
                         partial.source.path + ":" + std::to_string(partial.source.line) + ":" +
                             std::to_string(partial.source.column) +
                             ": invalid config setting [Model-setting]: " + field + " is required"};
        };
        if (!partial.model.has_value()) {
            return required_error("model");
        }
        if (!partial.purpose.has_value()) {
            return required_error("purpose");
        }
        if (!partial.default_system_prompt.has_value()) {
            return required_error("default_system_prompt");
        }
        if (!partial.temperature.has_value()) {
            return required_error("temperature");
        }
        if (!partial.top_k.has_value()) {
            return required_error("top_k");
        }
        if (!partial.top_p.has_value()) {
            return required_error("top_p");
        }
        if (!partial.min_p.has_value()) {
            return required_error("min_p");
        }
        if (!partial.repeat_penalty.has_value()) {
            return required_error("repeat_penalty");
        }
        if (!partial.presence_penalty.has_value()) {
            return required_error("presence_penalty");
        }
        ModelSetting setting{*partial.model,
                             *partial.purpose,
                             *partial.default_system_prompt,
                             *partial.temperature,
                             *partial.top_k,
                             *partial.top_p,
                             *partial.min_p,
                             *partial.repeat_penalty,
                             *partial.presence_penalty,
                             partial.thinking_budget.value_or("")};
        merge_model_setting(candidate.model_settings, std::move(setting));
    }
    return ok_error();
}

Error apply_configured_assist_commands(const Document& document, cli::Options& candidate) {
    struct PartialCommand {
        std::optional<std::string> string;
        std::optional<std::vector<pkchat::editor::AssistCommandMode>> modes;
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
            std::vector<pkchat::editor::AssistCommandMode> modes;
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
        if (!partial.modes.has_value()) {
            return {ErrorCode::Config,
                    partial.source.path + ":" + std::to_string(partial.source.line) + ":" +
                        std::to_string(partial.source.column) +
                        ": invalid config setting [command] " + *partial.string + ": modes is required"};
        }
        if (!partial.prompt.has_value()) {
            return {ErrorCode::Config,
                    partial.source.path + ":" + std::to_string(partial.source.line) + ":" +
                        std::to_string(partial.source.column) +
                        ": invalid config setting [command] " + *partial.string + ": prompt is required"};
        }
        merge_assist_command(candidate.editor_assist_config,
                             {*partial.string, *partial.modes, *partial.prompt});
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

}  // namespace

Error apply_editor_commands_document(const Document& document, cli::Options& options) {
    return apply_editor_commands_document_impl(document, options);
}

Error apply_themes_document(const Document& document, cli::Options& options) {
    return apply_themes_document_impl(document, options);
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

Error apply_document(const Document& document, cli::Options& options) {
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
            if (err.ok()) candidate.api = value;
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
            if (err.ok()) candidate.has_temperature = true;
        } else if (name == "generation.top_p") {
            err = numeric_double(entry, candidate.top_p);
            if (err.ok()) candidate.has_top_p = true;
        } else if (name == "generation.max_output_tokens") {
            err = nonnegative_int(entry, candidate.max_output_tokens);
            if (err.ok()) candidate.has_max_output_tokens = true;
        } else if (name == "context.window_tokens") {
            err = context_window_tokens(entry, candidate.context_tokens);
            if (err.ok()) {
                candidate.has_context_tokens = true;
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
            if (err.ok() && !pkchat::markdown::parse_output_format(entry.value.string, candidate.output_format)) {
                err = schema_error(entry, "expected html, md, or plaintext");
            } else if (err.ok()) {
                const bool rendered = candidate.output_format != pkchat::markdown::OutputFormat::Markdown;
                candidate.output_format_explicit = rendered;
                candidate.rendered_output_format_explicit = rendered;
            }
        } else if (name == "input.max_input_bytes") {
            err = nonnegative_long(entry, candidate.max_input_bytes);
        } else if (name == "input.max_image_bytes") {
            err = nonnegative_long(entry, candidate.max_image_bytes);
        } else if (name == "input.image_capability") {
            err = enum_string(entry,
                              cli::option_values::image_capability_strings(),
                              candidate.image_capability,
                              cli::option_values::image_capability_description());
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
        } else if (name == "editor.continue_read_chars") {
            long long value = 0;
            err = nonnegative_long_long(entry, value);
            if (err.ok()) {
                candidate.editor_ai_continue_read_chars = static_cast<size_t>(value);
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
        } else if (name.rfind("Model-setting.", 0) == 0) {
            continue;
        } else if (name == "url_fetch.max_bytes") {
            err = nonnegative_long(entry, candidate.max_fetch_bytes);
        } else if (name == "url_fetch.allow_private_addresses") {
            err = require_type(entry, Value::Type::Boolean);
            if (err.ok()) candidate.allow_private_url_fetch = entry.value.boolean;
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
        } else if (name == "tui.theme") {
            err = require_type(entry, Value::Type::String);
            if (err.ok()) {
                std::string normalized;
                if (!candidate.tui_themes.normalize_name(entry.value.string, normalized)) {
                    err = schema_error(entry,
                                         "unknown theme; available: " + tui::format_theme_list(candidate.tui_themes));
                } else {
                    candidate.tui_theme = normalized;
                }
            }
        } else if (name == "tui.thinking_traces") {
            err = require_type(entry, Value::Type::Boolean);
            if (err.ok()) candidate.show_thinking_traces = entry.value.boolean;
        } else {
            err = schema_error(entry, "unknown section or key");
        }
        if (!err.ok()) {
            return err;
        }
    }
    Error model_setting_err = apply_configured_model_settings(document, candidate);
    if (!model_setting_err.ok()) {
        return model_setting_err;
    }
    Error command_err = apply_configured_assist_commands(document, candidate);
    if (!command_err.ok()) {
        return command_err;
    }
    options = std::move(candidate);
    return ok_error();
}

Environment process_environment() {
    return {environment_value("XDG_CONFIG_HOME"), environment_value("XDG_CONFIG_DIRS"),
            environment_value("HOME")};
}

std::string user_config_path(const Environment& environment) {
    if (absolute_path(environment.xdg_config_home)) {
        return (std::filesystem::path(environment.xdg_config_home) / "pkchat" / "config.conf").string();
    }
    if (!absolute_path(environment.home)) {
        return {};
    }
    return (std::filesystem::path(environment.home) / ".config" / "pkchat" / "config.conf").string();
}

std::vector<std::string> system_config_paths(const Environment& environment) {
    const std::string dirs = environment.xdg_config_dirs.empty() ? "/etc/xdg" : environment.xdg_config_dirs;
    return config_paths_from_dirs(dirs, "config.conf");
}

std::string user_editor_commands_path(const Environment& environment) {
    if (absolute_path(environment.xdg_config_home)) {
        return (std::filesystem::path(environment.xdg_config_home) / "pkchat" / "editor-commands.conf")
            .string();
    }
    if (!absolute_path(environment.home)) {
        return {};
    }
    return (std::filesystem::path(environment.home) / ".config" / "pkchat" / "editor-commands.conf")
        .string();
}

std::vector<std::string> system_editor_commands_paths(const Environment& environment) {
    const std::string dirs = environment.xdg_config_dirs.empty() ? "/etc/xdg" : environment.xdg_config_dirs;
    return config_paths_from_dirs(dirs, "editor-commands.conf");
}

std::vector<std::string> bundled_editor_commands_paths() {
    std::vector<std::string> paths;
    if (const char* override_path = std::getenv("PKCHAT_EDITOR_COMMANDS")) {
        if (override_path[0] != '\0') {
            paths.emplace_back(override_path);
        }
    }
    paths.emplace_back("config/editor-commands.conf");
    paths.emplace_back("/usr/local/share/pkchat/editor-commands.conf");
    paths.emplace_back("/usr/share/pkchat/editor-commands.conf");
    return paths;
}

std::string user_themes_path(const Environment& environment) {
    if (absolute_path(environment.xdg_config_home)) {
        return (std::filesystem::path(environment.xdg_config_home) / "pkchat" / "themes.conf").string();
    }
    if (!absolute_path(environment.home)) {
        return {};
    }
    return (std::filesystem::path(environment.home) / ".config" / "pkchat" / "themes.conf").string();
}

std::vector<std::string> system_themes_paths(const Environment& environment) {
    const std::string dirs = environment.xdg_config_dirs.empty() ? "/etc/xdg" : environment.xdg_config_dirs;
    return config_paths_from_dirs(dirs, "themes.conf");
}

std::vector<std::string> bundled_themes_paths() {
    std::vector<std::string> paths;
    if (const char* override_path = std::getenv("PKCHAT_THEMES")) {
        if (override_path[0] != '\0') {
            paths.emplace_back(override_path);
        }
    }
    paths.emplace_back("config/themes.conf");
    paths.emplace_back("/usr/local/share/pkchat/themes.conf");
    paths.emplace_back("/usr/share/pkchat/themes.conf");
    return paths;
}

LoadResult load_automatic(const cli::Options& base_options,
                          const Environment& environment,
                          bool load_user_config) {
    LoadResult result{base_options, {}, {}, ok_error()};
    result.options.editor_assist_config = pkchat::editor::empty_editor_assist_config();
    result.options.tui_themes = tui::default_theme_registry();

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
    for (const std::string& path : system_themes_paths(environment)) {
        Error err = load_themes_path(path, ConfigScope::System, themes_loaded);
        if (!err.ok()) {
            result.error = std::move(err);
            return result;
        }
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
        for (const std::string& path : bundled_themes_paths()) {
            std::error_code filesystem_error;
            if (!std::filesystem::exists(path, filesystem_error) || filesystem_error) {
                continue;
            }
            Error err = load_themes_path(path, ConfigScope::System, themes_loaded);
            if (!err.ok()) {
                result.error = std::move(err);
                return result;
            }
            if (themes_loaded) {
                break;
            }
        }
    }

    if (!themes_loaded) {
        result.options.tui_themes = tui::default_theme_registry();
    }

    auto load_editor_commands_path = [&](const std::string& path,
                                         ConfigScope scope,
                                         bool& any_loaded) -> Error {
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
        any_loaded = true;
        result.loaded_paths.push_back(path);
        result.diagnostics.push_back(
            {scope, ConfigFileKind::EditorCommands, ConfigFileState::Loaded, path});
        return ok_error();
    };

    bool editor_commands_loaded = false;
    for (const std::string& path : system_editor_commands_paths(environment)) {
        Error err = load_editor_commands_path(path, ConfigScope::System, editor_commands_loaded);
        if (!err.ok()) {
            result.error = std::move(err);
            return result;
        }
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
        Error err = load_editor_commands_path(user_editor_commands,
                                              ConfigScope::User,
                                              editor_commands_loaded);
        if (!err.ok()) {
            result.error = std::move(err);
            return result;
        }
    }

    if (!editor_commands_loaded) {
        for (const std::string& path : bundled_editor_commands_paths()) {
            std::error_code filesystem_error;
            if (!std::filesystem::exists(path, filesystem_error) || filesystem_error) {
                continue;
            }
            Error err = load_editor_commands_path(path, ConfigScope::System, editor_commands_loaded);
            if (!err.ok()) {
                result.error = std::move(err);
                return result;
            }
            if (editor_commands_loaded) {
                break;
            }
        }
    }

    if (!editor_commands_loaded) {
        result.options.editor_assist_config = pkchat::editor::default_editor_assist_config();
    }

    const std::vector<std::string> paths = system_config_paths(environment);
    for (const std::string& path : paths) {
        std::error_code filesystem_error;
        const bool exists = std::filesystem::exists(path, filesystem_error);
        if (filesystem_error) {
            result.diagnostics.push_back(
                {ConfigScope::System, ConfigFileKind::Config, ConfigFileState::Error, path});
            result.error = {ErrorCode::Config, "could not inspect config file: " + path};
            return result;
        }
        if (!exists) {
            result.diagnostics.push_back(
                {ConfigScope::System, ConfigFileKind::Config, ConfigFileState::Missing, path});
            continue;
        }
        Error err = apply_config_file(path, result.options);
        if (!err.ok()) {
            result.diagnostics.push_back(
                {ConfigScope::System, ConfigFileKind::Config, ConfigFileState::Error, path});
            result.error = std::move(err);
            return result;
        }
        result.loaded_paths.push_back(path);
        result.diagnostics.push_back(
            {ConfigScope::System, ConfigFileKind::Config, ConfigFileState::Loaded, path});
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
    Error err = apply_config_file(user_path, result.options);
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

}  // namespace pkchat::config
