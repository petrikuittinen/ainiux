#include "agent/index/index.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <regex>
#include <sstream>
#include <utility>

#include "common.hpp"
#include "highlight/highlight.hpp"

namespace ainiux::agent::index {
namespace {

constexpr std::size_t kMaximumDocumentationBytes = 512;

std::uint64_t fnv1a(const std::string& text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string trim(std::string text) {
    return ascii_trim(std::move(text));
}

std::string collapse_space(const std::string& text) {
    std::string output;
    output.reserve(text.size());
    bool spacing = false;
    for (unsigned char byte : text) {
        if (std::isspace(byte)) {
            spacing = !output.empty();
        } else {
            if (spacing) output.push_back(' ');
            output.push_back(static_cast<char>(byte));
            spacing = false;
        }
    }
    return trim(std::move(output));
}

std::string bounded_documentation(std::string text) {
    text = collapse_space(text);
    if (text.size() > kMaximumDocumentationBytes) {
        text.resize(kMaximumDocumentationBytes - 3);
        text += "...";
    }
    return text;
}

std::vector<std::string> source_lines(const std::string& source) {
    return split_lines_crlf(source);
}

std::string line_range(const std::vector<std::string>& lines, int first, int last) {
    std::string output;
    for (int line = first; line <= last && line <= static_cast<int>(lines.size()); ++line) {
        output += lines[static_cast<std::size_t>(line - 1)];
        output.push_back('\n');
    }
    return output;
}

int indentation(const std::string& line) {
    int value = 0;
    for (char ch : line) {
        if (ch == ' ') {
            ++value;
        } else if (ch == '\t') {
            value += 8 - value % 8;
        } else {
            break;
        }
    }
    return value;
}

std::string preceding_python_comment(const std::vector<std::string>& lines, std::size_t line) {
    if (line == 0) return {};
    std::vector<std::string> pieces;
    std::size_t pos = line;
    while (pos > 0) {
        std::string current = trim(lines[pos - 1]);
        if (!current.empty() && current.front() == '@') {
            --pos;
            continue;
        }
        break;
    }
    while (pos > 0) {
        std::string current = trim(lines[pos - 1]);
        if (current.rfind("#", 0) != 0) break;
        current.erase(0, 1);
        if (!current.empty() && current.front() == ' ') current.erase(0, 1);
        pieces.push_back(std::move(current));
        --pos;
    }
    std::reverse(pieces.begin(), pieces.end());
    std::string result;
    for (const std::string& piece : pieces) {
        if (!result.empty()) result.push_back(' ');
        result += piece;
    }
    return bounded_documentation(std::move(result));
}

std::string python_docstring(const std::vector<std::string>& lines,
                             std::size_t after_line,
                             int parent_indent) {
    for (std::size_t pos = after_line; pos < lines.size(); ++pos) {
        const std::string stripped = trim(lines[pos]);
        if (stripped.empty()) continue;
        if (indentation(lines[pos]) <= parent_indent) return {};
        std::size_t begin = std::string::npos;
        std::string delimiter;
        if ((begin = stripped.find("\"\"\"")) != std::string::npos) delimiter = "\"\"\"";
        else if ((begin = stripped.find("'''")) != std::string::npos) delimiter = "'''";
        else return {};
        std::string doc = stripped.substr(begin + 3);
        std::size_t end = doc.find(delimiter);
        if (end != std::string::npos) {
            doc.resize(end);
            return bounded_documentation(std::move(doc));
        }
        for (++pos; pos < lines.size(); ++pos) {
            end = lines[pos].find(delimiter);
            if (!doc.empty()) doc.push_back(' ');
            doc += end == std::string::npos ? lines[pos] : lines[pos].substr(0, end);
            if (end != std::string::npos) break;
            if (doc.size() > kMaximumDocumentationBytes * 2) break;
        }
        return bounded_documentation(std::move(doc));
    }
    return {};
}

std::string mask_python(const std::string& source) {
    std::string masked = source;
    enum class State { Code, Single, Double, TripleSingle, TripleDouble, Comment };
    State state = State::Code;
    bool escaped = false;
    for (std::size_t i = 0; i < source.size(); ++i) {
        const char ch = source[i];
        if (state == State::Comment) {
            if (ch == '\n') state = State::Code;
            else masked[i] = ' ';
            continue;
        }
        if (state == State::Code) {
            if (ch == '#') {
                masked[i] = ' ';
                state = State::Comment;
            } else if (i + 2 < source.size() && source.compare(i, 3, "'''" ) == 0) {
                masked[i] = masked[i + 1] = masked[i + 2] = ' ';
                i += 2;
                state = State::TripleSingle;
            } else if (i + 2 < source.size() && source.compare(i, 3, "\"\"\"") == 0) {
                masked[i] = masked[i + 1] = masked[i + 2] = ' ';
                i += 2;
                state = State::TripleDouble;
            } else if (ch == '\'') {
                masked[i] = ' ';
                state = State::Single;
                escaped = false;
            } else if (ch == '"') {
                masked[i] = ' ';
                state = State::Double;
                escaped = false;
            }
            continue;
        }
        if (ch != '\n') masked[i] = ' ';
        if (state == State::Single || state == State::Double) {
            const char delimiter = state == State::Single ? '\'' : '"';
            if (!escaped && ch == delimiter) state = State::Code;
            if (!escaped && ch == '\\') escaped = true;
            else escaped = false;
        } else {
            const char* delimiter = state == State::TripleSingle ? "'''" : "\"\"\"";
            if (i + 2 < source.size() && source.compare(i, 3, delimiter) == 0) {
                masked[i] = masked[i + 1] = masked[i + 2] = ' ';
                i += 2;
                state = State::Code;
            }
        }
    }
    return masked;
}

int python_block_end(const std::vector<std::string>& masked_lines,
                     std::size_t start,
                     int parent_indent) {
    int last = static_cast<int>(start + 1);
    for (std::size_t pos = start + 1; pos < masked_lines.size(); ++pos) {
        if (trim(masked_lines[pos]).empty()) continue;
        if (indentation(masked_lines[pos]) <= parent_indent) break;
        last = static_cast<int>(pos + 1);
    }
    return last;
}

std::string join_qualified(const std::vector<std::string>& parts, const std::string& name) {
    std::string result;
    for (const std::string& part : parts) {
        if (part.empty()) continue;
        if (!result.empty()) result += "::";
        result += part;
    }
    if (!result.empty() && !name.empty()) result += "::";
    result += name;
    return result;
}

ScanResult scan_python(const std::string& source) {
    ScanResult result;
    result.language = Language::Python;
    const std::vector<std::string> original = source_lines(source);
    const std::vector<std::string> masked = source_lines(mask_python(source));
    struct Scope { int indent; std::string name; std::string kind; };
    std::vector<Scope> scopes;
    static const std::regex declaration(
        R"(^\s*(?:(async)\s+)?(class|def)\s+([A-Za-z_][A-Za-z0-9_]*)\s*(.*)$)",
        std::regex::optimize);
    static const std::regex assignment(
        R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*(?::[^=]+)?=)",
        std::regex::optimize);

    for (std::size_t line = 0; line < masked.size(); ++line) {
        if (trim(masked[line]).empty()) continue;
        const int indent = indentation(masked[line]);
        while (!scopes.empty() && indent <= scopes.back().indent) scopes.pop_back();

        std::string logical_masked = masked[line];
        std::string logical_original = original[line];
        int parens = 0;
        auto count_parens = [&](const std::string& text) {
            for (char ch : text) {
                if (ch == '(' || ch == '[' || ch == '{') ++parens;
                else if ((ch == ')' || ch == ']' || ch == '}') && parens > 0) --parens;
            }
        };
        count_parens(logical_masked);
        std::size_t end_line = line;
        while ((parens > 0 || trim(logical_masked).back() != ':') && end_line + 1 < masked.size() &&
               end_line - line < 32) {
            if (logical_masked.find("def ") == std::string::npos &&
                logical_masked.find("class ") == std::string::npos) break;
            ++end_line;
            logical_masked += "\n" + masked[end_line];
            logical_original += "\n" + original[end_line];
            count_parens(masked[end_line]);
        }

        std::smatch match;
        const std::string declaration_text = collapse_space(logical_masked);
        if (std::regex_match(declaration_text, match, declaration)) {
            const std::string declared = match[2].str();
            const std::string name = match[3].str();
            bool inside_class = false;
            bool function_between = false;
            std::vector<std::string> parts;
            for (const Scope& scope : scopes) {
                parts.push_back(scope.name);
                if (scope.kind == "class") {
                    inside_class = true;
                    function_between = false;
                } else if (scope.kind == "function") {
                    function_between = true;
                }
            }
            Symbol symbol;
            symbol.name = name;
            symbol.kind = declared == "class" ? "class"
                          : (inside_class && !function_between ? "method" : "function");
            symbol.qualified_name = join_qualified(parts, name);
            std::string decorated_signature;
            std::size_t decorator_line = line;
            while (decorator_line > 0 &&
                   indentation(original[decorator_line - 1]) == indent &&
                   trim(original[decorator_line - 1]).rfind("@", 0) == 0) {
                --decorator_line;
            }
            for (std::size_t pos = decorator_line; pos < line; ++pos) {
                decorated_signature += original[pos];
                decorated_signature.push_back(' ');
            }
            decorated_signature += logical_original;
            symbol.signature = collapse_space(decorated_signature);
            if (!symbol.signature.empty() && symbol.signature.back() == ':') symbol.signature.pop_back();
            const std::size_t open = symbol.signature.find('(');
            const std::size_t close = symbol.signature.rfind(')');
            if (open != std::string::npos && close != std::string::npos && close > open) {
                symbol.parameters = symbol.signature.substr(open + 1, close - open - 1);
                const std::size_t arrow = symbol.signature.find("->", close);
                if (arrow != std::string::npos) symbol.return_type = trim(symbol.signature.substr(arrow + 2));
            }
            symbol.line_start = static_cast<int>(line + 1);
            symbol.line_end = python_block_end(masked, end_line, indent);
            symbol.documentation = python_docstring(original, end_line + 1, indent);
            if (symbol.documentation.empty()) symbol.documentation = preceding_python_comment(original, line);
            symbol.signature_hash = fnv1a(symbol.signature);
            symbol.body_hash = fnv1a(line_range(original, symbol.line_start, symbol.line_end));
            result.symbols.push_back(std::move(symbol));
            scopes.push_back({indent, name, declared == "class" ? "class" : "function"});
            line = end_line;
            continue;
        }

        bool in_function = false;
        bool in_class = false;
        std::vector<std::string> parts;
        for (const Scope& scope : scopes) {
            parts.push_back(scope.name);
            in_function = in_function || scope.kind == "function";
            in_class = in_class || scope.kind == "class";
        }
        if (!in_function && std::regex_search(masked[line], match, assignment)) {
            Symbol symbol;
            symbol.name = match[1].str();
            const bool constant = symbol.name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") ==
                                  std::string::npos;
            symbol.kind = in_class ? "field" : (constant ? "constant" : "global");
            symbol.qualified_name = join_qualified(parts, symbol.name);
            symbol.signature = collapse_space(original[line]);
            symbol.line_start = symbol.line_end = static_cast<int>(line + 1);
            symbol.documentation = preceding_python_comment(original, line);
            symbol.signature_hash = fnv1a(symbol.signature);
            symbol.body_hash = symbol.signature_hash;
            result.symbols.push_back(std::move(symbol));
        }
    }
    return result;
}

std::string mask_c_family(const std::string& source) {
    std::string masked = source;
    enum class State { Code, LineComment, BlockComment, Single, Double };
    State state = State::Code;
    bool escaped = false;
    for (std::size_t i = 0; i < source.size(); ++i) {
        const char ch = source[i];
        const char next = i + 1 < source.size() ? source[i + 1] : '\0';
        if (state == State::Code) {
            if (ch == 'R' && next == '"') {
                const std::size_t delimiter_end = source.find('(', i + 2);
                if (delimiter_end != std::string::npos && delimiter_end - (i + 2) <= 16) {
                    const std::string delimiter = source.substr(i + 2, delimiter_end - i - 2);
                    const std::string closing = ")" + delimiter + "\"";
                    const std::size_t close = source.find(closing, delimiter_end + 1);
                    const std::size_t last = close == std::string::npos
                                                 ? source.size()
                                                 : close + closing.size();
                    for (std::size_t pos = i; pos < last; ++pos) {
                        if (source[pos] != '\n') masked[pos] = ' ';
                    }
                    i = last == 0 ? 0 : last - 1;
                }
            } else if (ch == '/' && next == '/') {
                masked[i] = masked[i + 1] = ' ';
                ++i;
                state = State::LineComment;
            } else if (ch == '/' && next == '*') {
                masked[i] = masked[i + 1] = ' ';
                ++i;
                state = State::BlockComment;
            } else if (ch == '\'') {
                masked[i] = ' ';
                state = State::Single;
                escaped = false;
            } else if (ch == '"') {
                masked[i] = ' ';
                state = State::Double;
                escaped = false;
            }
            continue;
        }
        if (ch != '\n') masked[i] = ' ';
        if (state == State::LineComment) {
            if (ch == '\n') state = State::Code;
        } else if (state == State::BlockComment) {
            if (ch == '*' && next == '/') {
                masked[i + 1] = ' ';
                ++i;
                state = State::Code;
            }
        } else {
            const char delimiter = state == State::Single ? '\'' : '"';
            if (!escaped && ch == delimiter) state = State::Code;
            if (!escaped && ch == '\\') escaped = true;
            else escaped = false;
        }
    }
    return masked;
}

std::string preceding_c_comment(const std::vector<std::string>& lines, std::size_t line) {
    if (line == 0) return {};
    std::vector<std::string> pieces;
    std::size_t pos = line;
    while (pos > 0) {
        std::string current = trim(lines[pos - 1]);
        if (current.rfind("//", 0) == 0) {
            current.erase(0, 2);
            if (!current.empty() && (current.front() == '/' || current.front() == '!')) current.erase(0, 1);
            pieces.push_back(trim(std::move(current)));
            --pos;
            continue;
        }
        break;
    }
    if (pieces.empty() && pos > 0) {
        std::string current = trim(lines[pos - 1]);
        if (current.size() >= 2 && current.compare(current.size() - 2, 2, "*/") == 0) {
            while (pos > 0) {
                current = trim(lines[pos - 1]);
                if (current.size() >= 2 && current.compare(current.size() - 2, 2, "*/") == 0)
                    current.resize(current.size() - 2);
                if (current.rfind("/*", 0) == 0) {
                    current.erase(0, 2);
                    if (!current.empty() && (current.front() == '*' || current.front() == '!'))
                        current.erase(0, 1);
                    pieces.push_back(trim(std::move(current)));
                    break;
                }
                if (!current.empty() && current.front() == '*') current.erase(0, 1);
                pieces.push_back(trim(std::move(current)));
                --pos;
            }
        }
    }
    std::reverse(pieces.begin(), pieces.end());
    std::string result;
    for (const std::string& piece : pieces) {
        if (!result.empty()) result.push_back(' ');
        result += piece;
    }
    return bounded_documentation(std::move(result));
}

std::string simple_name(const std::string& name) {
    const std::size_t separator = name.rfind("::");
    return separator == std::string::npos ? name : name.substr(separator + 2);
}

bool is_cpp_header(const std::string& path, const std::string& masked) {
    if (ascii_lower(std::filesystem::path(path).extension().string()) != ".h") return false;
    static const std::regex marker(
        R"((^|[^A-Za-z0-9_])(namespace|class|template|constexpr|consteval|constinit|noexcept|nullptr|using)[^A-Za-z0-9_]|::|\b(public|private|protected)\s*:)",
        std::regex::optimize);
    return std::regex_search(masked, marker);
}

ScanResult scan_c_family(const std::string& path, const std::string& source, Language language) {
    const std::string masked_source = mask_c_family(source);
    if (language == Language::C && is_cpp_header(path, masked_source)) language = Language::Cpp;
    ScanResult result;
    result.language = language;
    const std::vector<std::string> original = source_lines(source);
    const std::vector<std::string> masked = source_lines(masked_source);
    struct Scope { int end_line; std::string name; std::string kind; };
    std::vector<Scope> scopes;
    bool preprocessor_continuation = false;

    static const std::regex namespace_pattern(
        R"(^\s*(?:inline\s+)?namespace(?:\s+([A-Za-z_][A-Za-z0-9_:]*))?\s*\{)",
        std::regex::optimize);
    static const std::regex type_pattern(
        R"((?:^|\s)(class|struct|union|enum(?:\s+class|\s+struct)?)\s+([A-Za-z_][A-Za-z0-9_]*))",
        std::regex::optimize);
    static const std::regex typedef_pattern(
        R"(^\s*typedef\s+.+\s+([A-Za-z_][A-Za-z0-9_]*)\s*;\s*$)",
        std::regex::optimize);
    static const std::regex using_pattern(
        R"(^\s*using\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+);\s*$)",
        std::regex::optimize);
    static const std::regex function_pattern(
        R"(^\s*(.*?)\b([A-Za-z_~][A-Za-z0-9_:~]*|operator\s*[^\s(]+)\s*\((.*)\)\s*([^;{]*)([;{])\s*$)",
        std::regex::optimize);
    static const std::regex variable_pattern(
        R"(([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[[^\]]*\])?\s*(?:=[^;]*)?;\s*$)",
        std::regex::optimize);
    static const std::regex enum_member_pattern(
        R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*(?:=[^,}]*)?[,}]?)",
        std::regex::optimize);

    auto qualified_parts = [&]() {
        std::vector<std::string> parts;
        for (const Scope& scope : scopes) {
            if (scope.kind != "function" && !scope.name.empty()) parts.push_back(scope.name);
        }
        return parts;
    };
    auto matching_brace_line = [&](std::size_t start_line, std::size_t open_column) {
        int depth = 0;
        for (std::size_t row = start_line; row < masked.size(); ++row) {
            const std::size_t begin = row == start_line ? open_column : 0;
            for (std::size_t column = begin; column < masked[row].size(); ++column) {
                if (masked[row][column] == '{') ++depth;
                else if (masked[row][column] == '}' && --depth == 0) return static_cast<int>(row + 1);
            }
        }
        return static_cast<int>(masked.size());
    };

    for (std::size_t line = 0; line < masked.size(); ++line) {
        while (!scopes.empty() && scopes.back().end_line < static_cast<int>(line + 1)) scopes.pop_back();
        std::string stripped = trim(masked[line]);
        const bool preprocessor_line = !stripped.empty() && stripped.front() == '#';
        if (preprocessor_line || preprocessor_continuation) {
            const std::string without_space = trim(masked[line]);
            preprocessor_continuation = !without_space.empty() && without_space.back() == '\\';
            continue;
        }
        if (stripped.empty()) continue;
        if (stripped == "public:" || stripped == "private:" || stripped == "protected:") continue;
        bool in_function = false;
        for (const Scope& scope : scopes) in_function = in_function || scope.kind == "function";
        if (in_function) continue;

        if (!scopes.empty() && scopes.back().kind == "enum" && stripped.find('(') == std::string::npos &&
            stripped.find(';') == std::string::npos) {
            std::smatch member;
            if (std::regex_search(masked[line], member, enum_member_pattern) && member[1].str() != "enum") {
                Symbol symbol;
                symbol.kind = "enum-member";
                symbol.name = member[1].str();
                symbol.qualified_name = join_qualified(qualified_parts(), symbol.name);
                symbol.signature = collapse_space(original[line]);
                symbol.line_start = symbol.line_end = static_cast<int>(line + 1);
                symbol.documentation = preceding_c_comment(original, line);
                symbol.signature_hash = symbol.body_hash = fnv1a(symbol.signature);
                result.symbols.push_back(std::move(symbol));
            }
            continue;
        }

        std::size_t end_line = line;
        int parentheses = 0;
        std::string statement_masked;
        std::string statement_original;
        std::size_t delimiter_column = std::string::npos;
        char delimiter = '\0';
        for (; end_line < masked.size() && end_line - line < 64; ++end_line) {
            const std::string& current = masked[end_line];
            for (std::size_t column = 0; column < current.size(); ++column) {
                const char ch = current[column];
                if (ch == '(' || ch == '[') ++parentheses;
                else if ((ch == ')' || ch == ']') && parentheses > 0) --parentheses;
                else if (parentheses == 0 && (ch == ';' || ch == '{')) {
                    delimiter = ch;
                    delimiter_column = column;
                    break;
                }
            }
            if (!statement_masked.empty()) {
                statement_masked.push_back(' ');
                statement_original.push_back(' ');
            }
            const std::size_t segment_size = delimiter == '\0' ? current.size() : delimiter_column + 1;
            statement_masked += trim(current.substr(0, segment_size));
            statement_original += trim(original[end_line].substr(0, segment_size));
            if (delimiter != '\0') break;
        }
        if (delimiter == '\0') continue;
        statement_masked = collapse_space(statement_masked);
        statement_original = collapse_space(statement_original);
        const int body_end = delimiter == '{' ? matching_brace_line(end_line, delimiter_column)
                                              : static_cast<int>(end_line + 1);
        std::smatch match;
        const std::vector<std::string> parts = qualified_parts();

        if (std::regex_search(statement_masked, match, namespace_pattern)) {
            const std::string name = match[1].matched ? match[1].str() : std::string{};
            Symbol symbol;
            symbol.kind = "namespace";
            symbol.name = name.empty() ? "(anonymous)" : name;
            symbol.qualified_name = join_qualified(parts, symbol.name);
            symbol.signature = statement_original;
            symbol.line_start = static_cast<int>(line + 1);
            symbol.line_end = body_end;
            symbol.documentation = preceding_c_comment(original, line);
            symbol.signature_hash = fnv1a(symbol.signature);
            symbol.body_hash = fnv1a(line_range(original, symbol.line_start, symbol.line_end));
            result.symbols.push_back(std::move(symbol));
            scopes.push_back({body_end, name, "namespace"});
            line = end_line;
            continue;
        }
        if (std::regex_search(statement_masked, match, type_pattern)) {
            const std::string type = match[1].str();
            const std::string name = match[2].str();
            Symbol symbol;
            symbol.kind = type.rfind("enum", 0) == 0 ? "enum" : type;
            symbol.name = name;
            symbol.qualified_name = join_qualified(parts, name);
            symbol.signature = statement_original;
            symbol.line_start = static_cast<int>(line + 1);
            symbol.line_end = body_end;
            symbol.documentation = preceding_c_comment(original, line);
            symbol.signature_hash = fnv1a(symbol.signature);
            symbol.body_hash = fnv1a(line_range(original, symbol.line_start, symbol.line_end));
            result.symbols.push_back(std::move(symbol));
            if (delimiter == '{') {
                scopes.push_back({body_end, name, type.rfind("enum", 0) == 0 ? "enum" : "type"});
            }
            line = end_line;
            continue;
        }
        if (std::regex_match(statement_masked, match, typedef_pattern)) {
            Symbol symbol;
            symbol.kind = "typedef";
            symbol.name = match[1].str();
            symbol.qualified_name = join_qualified(parts, symbol.name);
            symbol.signature = statement_original;
            symbol.line_start = symbol.line_end = static_cast<int>(line + 1);
            symbol.documentation = preceding_c_comment(original, line);
            symbol.signature_hash = symbol.body_hash = fnv1a(symbol.signature);
            result.symbols.push_back(std::move(symbol));
            line = end_line;
            continue;
        }
        if (language == Language::Cpp && std::regex_match(statement_masked, match, using_pattern)) {
            Symbol symbol;
            symbol.kind = "alias";
            symbol.name = match[1].str();
            symbol.qualified_name = join_qualified(parts, symbol.name);
            symbol.signature = statement_original;
            symbol.return_type = match[2].str();
            symbol.line_start = symbol.line_end = static_cast<int>(line + 1);
            symbol.documentation = preceding_c_comment(original, line);
            symbol.signature_hash = symbol.body_hash = fnv1a(symbol.signature);
            result.symbols.push_back(std::move(symbol));
            line = end_line;
            continue;
        }
        if (std::regex_match(statement_masked, match, function_pattern)) {
            const std::string full_name = collapse_space(match[2].str());
            const std::string name = simple_name(full_name);
            static const char* controls[] = {"if", "for", "while", "switch", "catch", "sizeof", "alignof"};
            bool control = false;
            for (const char* value : controls) control = control || name == value;
            if (!control) {
                bool method = full_name.find("::") != std::string::npos;
                for (const Scope& scope : scopes) method = method || scope.kind == "type";
                Symbol symbol;
                symbol.kind = method ? "method" : "function";
                symbol.name = name;
                symbol.qualified_name = full_name.find("::") != std::string::npos
                                            ? join_qualified(parts, full_name)
                                            : join_qualified(parts, name);
                symbol.signature = statement_original;
                symbol.parameters = collapse_space(match[3].str());
                symbol.return_type = collapse_space(match[1].str());
                symbol.line_start = static_cast<int>(line + 1);
                symbol.line_end = body_end;
                symbol.documentation = preceding_c_comment(original, line);
                symbol.signature_hash = fnv1a(symbol.signature);
                symbol.body_hash = fnv1a(line_range(original, symbol.line_start, symbol.line_end));
                result.symbols.push_back(std::move(symbol));
                if (delimiter == '{') scopes.push_back({body_end, name, "function"});
                line = end_line;
                continue;
            }
        }
        if (std::regex_search(statement_masked, match, variable_pattern) &&
            statement_masked.find('(') == std::string::npos &&
            statement_masked.rfind("using ", 0) != 0 && statement_masked.rfind("return ", 0) != 0) {
            Symbol symbol;
            symbol.name = match[1].str();
            bool field = false;
            for (const Scope& scope : scopes) field = field || scope.kind == "type";
            const bool constant = statement_masked.find("const ") != std::string::npos ||
                                  statement_masked.find("constexpr ") != std::string::npos;
            symbol.kind = field ? "field" : (constant ? "constant" : "global");
            symbol.qualified_name = join_qualified(parts, symbol.name);
            symbol.signature = statement_original;
            symbol.line_start = static_cast<int>(line + 1);
            symbol.line_end = static_cast<int>(end_line + 1);
            symbol.documentation = preceding_c_comment(original, line);
            symbol.signature_hash = symbol.body_hash = fnv1a(symbol.signature);
            result.symbols.push_back(std::move(symbol));
        }
        line = end_line;
    }
    return result;
}

}  // namespace

const char* language_name(Language language) {
    switch (language) {
        case Language::Python: return "Python";
        case Language::C: return "C";
        case Language::Cpp: return "C++";
    }
    return "Unknown";
}

bool language_for_path(const std::string& path, Language& language) {
    switch (highlight::detect_language(path)) {
        case highlight::Language::Python: language = Language::Python; return true;
        case highlight::Language::C: language = Language::C; return true;
        case highlight::Language::Cpp: language = Language::Cpp; return true;
        default: return false;
    }
}

ScanResult scan_source(const std::string& path, const std::string& source, Language language) {
    return language == Language::Python ? scan_python(source)
                                        : scan_c_family(path, source, language);
}

}  // namespace ainiux::agent::index
