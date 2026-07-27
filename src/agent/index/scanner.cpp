#include "agent/index/index.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <utility>

#include "common.hpp"
#include "highlight/highlight.hpp"
#include "agent/index/scanner_extra.hpp"

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

bool javascript_regex_start(const std::string& source, std::size_t slash) {
    std::size_t pos = slash;
    while (pos > 0 && std::isspace(static_cast<unsigned char>(source[pos - 1]))) --pos;
    if (pos == 0) return true;
    const char previous = source[pos - 1];
    if (std::string("=([{,:;!&|?+-*%^~<>").find(previous) != std::string::npos) return true;
    if (std::isalnum(static_cast<unsigned char>(previous)) || previous == '_' || previous == '$') {
        std::size_t begin = pos - 1;
        while (begin > 0) {
            const unsigned char byte = static_cast<unsigned char>(source[begin - 1]);
            if (!std::isalnum(byte) && source[begin - 1] != '_' && source[begin - 1] != '$') break;
            --begin;
        }
        const std::string word = source.substr(begin, pos - begin);
        return word == "return" || word == "case" || word == "throw" || word == "else" ||
               word == "do" || word == "typeof" || word == "instanceof" || word == "in" ||
               word == "of" || word == "yield" || word == "await";
    }
    return false;
}

std::string mask_ecmascript(const std::string& source) {
    std::string masked = source;
    enum class State { Code, LineComment, BlockComment, Single, Double, Template, Regex };
    State state = State::Code;
    bool escaped = false;
    bool regex_class = false;
    for (std::size_t i = 0; i < source.size(); ++i) {
        const char ch = source[i];
        const char next = i + 1 < source.size() ? source[i + 1] : '\0';
        if (state == State::Code) {
            if (ch == '/' && next == '/') {
                masked[i] = masked[i + 1] = ' ';
                ++i;
                state = State::LineComment;
            } else if (ch == '/' && next == '*') {
                masked[i] = masked[i + 1] = ' ';
                ++i;
                state = State::BlockComment;
            } else if (ch == '\'' || ch == '"' || ch == '`') {
                masked[i] = ' ';
                state = ch == '\'' ? State::Single : (ch == '"' ? State::Double : State::Template);
                escaped = false;
            } else if (ch == '/' && javascript_regex_start(source, i)) {
                masked[i] = ' ';
                state = State::Regex;
                escaped = false;
                regex_class = false;
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
        } else if (state == State::Regex) {
            if (!escaped && ch == '[') regex_class = true;
            else if (!escaped && ch == ']') regex_class = false;
            else if (!escaped && ch == '/' && !regex_class) state = State::Code;
            if (!escaped && ch == '\\') escaped = true;
            else escaped = false;
        } else {
            const char delimiter = state == State::Single ? '\'' : (state == State::Double ? '"' : '`');
            if (!escaped && ch == delimiter) state = State::Code;
            if (!escaped && ch == '\\') escaped = true;
            else escaped = false;
        }
    }
    return masked;
}

ScanResult scan_ecmascript(const std::string& source, Language language) {
    ScanResult result;
    result.language = language;
    const std::vector<std::string> original = source_lines(source);
    const std::vector<std::string> masked = source_lines(mask_ecmascript(source));
    struct Scope { int end_line; std::string name; std::string kind; };
    std::vector<Scope> scopes;

    static const std::regex namespace_pattern(
        R"(^\s*(?:(?:export|declare)\s+)*(?:namespace|module)\s+([$A-Za-z_][$A-Za-z0-9_]*)[^\{]*\{)",
        std::regex::optimize);
    static const std::regex type_pattern(
        R"(^\s*(?:(?:export|default|declare|abstract)\s+)*(class|interface|enum)\s+([$A-Za-z_][$A-Za-z0-9_]*)[^\{;]*\{)",
        std::regex::optimize);
    static const std::regex type_alias_pattern(
        R"(^\s*(?:(?:export|declare)\s+)*type\s+([$A-Za-z_][$A-Za-z0-9_]*)(?:\s*<[^;=]*>)?\s*=\s*(.+);\s*$)",
        std::regex::optimize);
    static const std::regex function_pattern(
        R"(^\s*(?:(?:export|default|declare)\s+)*(?:async\s+)?function\s*\*?\s*([$A-Za-z_][$A-Za-z0-9_]*)\s*(?:<[^\{;]*>)?\s*\((.*)\)\s*(?::\s*([^\{;]+))?\s*([\{;])\s*$)",
        std::regex::optimize);
    static const std::regex method_pattern(
        R"(^\s*(?:(?:public|private|protected|static|readonly|abstract|async|override|declare|get|set)\s+)*\*?\s*(constructor|[$A-Za-z_][$A-Za-z0-9_]*)\s*(?:<[^\{;]*>)?\s*\((.*)\)\s*(?::\s*([^\{;]+))?\s*([\{;])\s*$)",
        std::regex::optimize);
    static const std::regex arrow_pattern(
        R"(^\s*(?:(?:export|declare)\s+)*(const|let|var)\s+([$A-Za-z_][$A-Za-z0-9_]*)\s*(?::[^=]+)?=\s*(?:async\s+)?(\([^\)]*\)|[$A-Za-z_][$A-Za-z0-9_]*)\s*(?::\s*[^=]+)?=>)",
        std::regex::optimize);
    static const std::regex function_expression_pattern(
        R"(^\s*(?:(?:export|declare)\s+)*(const|let|var)\s+([$A-Za-z_][$A-Za-z0-9_]*)\s*(?::[^=]+)?=\s*(?:async\s+)?function\s*\*?\s*(?:[$A-Za-z_][$A-Za-z0-9_]*)?\s*\(([^\)]*)\))",
        std::regex::optimize);
    static const std::regex field_pattern(
        R"(^\s*(?:(?:public|private|protected|static|readonly|declare|abstract|override)\s+)*(#?[$A-Za-z_][$A-Za-z0-9_]*)[!?]?\s*(?::[^=;]+)?(?:=[^;]*)?;\s*$)",
        std::regex::optimize);
    static const std::regex variable_pattern(
        R"(^\s*(?:(?:export|declare)\s+)*(const|let|var)\s+([$A-Za-z_][$A-Za-z0-9_]*)\b)",
        std::regex::optimize);
    static const std::regex enum_member_pattern(
        R"(^\s*([$A-Za-z_][$A-Za-z0-9_]*)\s*(?:=[^,}]*)?[,}]?)",
        std::regex::optimize);

    auto qualified_parts = [&]() {
        std::vector<std::string> parts;
        for (const Scope& scope : scopes) {
            if (!scope.name.empty()) parts.push_back(scope.name);
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
    auto add_symbol = [&](const std::string& kind,
                          const std::string& name,
                          const std::string& signature,
                          const std::string& parameters,
                          const std::string& return_type,
                          std::size_t line,
                          int body_end) {
        Symbol symbol;
        symbol.kind = kind;
        symbol.name = name;
        symbol.qualified_name = join_qualified(qualified_parts(), name);
        symbol.signature = signature;
        symbol.parameters = parameters;
        symbol.return_type = return_type;
        symbol.line_start = static_cast<int>(line + 1);
        symbol.line_end = body_end;
        symbol.documentation = preceding_c_comment(original, line);
        symbol.signature_hash = fnv1a(symbol.signature);
        symbol.body_hash = fnv1a(line_range(original, symbol.line_start, symbol.line_end));
        result.symbols.push_back(std::move(symbol));
    };

    for (std::size_t line = 0; line < masked.size(); ++line) {
        while (!scopes.empty() && scopes.back().end_line < static_cast<int>(line + 1)) scopes.pop_back();
        std::string stripped = trim(masked[line]);
        if (stripped.empty() || stripped.front() == '@' || stripped.front() == '}') continue;

        if (!scopes.empty() && scopes.back().kind == "enum" &&
            stripped.find('(') == std::string::npos && stripped.find(';') == std::string::npos) {
            std::smatch member;
            if (std::regex_search(masked[line], member, enum_member_pattern)) {
                const std::string signature = collapse_space(original[line]);
                add_symbol("enum-member", member[1].str(), signature, {}, {}, line,
                           static_cast<int>(line + 1));
            }
            continue;
        }

        std::size_t end_line = line;
        int grouping = 0;
        std::size_t delimiter_column = std::string::npos;
        char delimiter = '\0';
        std::string statement_masked;
        std::string statement_original;
        for (; end_line < masked.size() && end_line - line < 64; ++end_line) {
            const std::string& current = masked[end_line];
            for (std::size_t column = 0; column < current.size(); ++column) {
                const char ch = current[column];
                if (ch == '(' || ch == '[') ++grouping;
                else if ((ch == ')' || ch == ']') && grouping > 0) --grouping;
                else if (grouping == 0 && (ch == ';' || ch == '{')) {
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

        if (language == Language::TypeScript && std::regex_match(statement_masked, match, namespace_pattern)) {
            const std::string name = match[1].str();
            add_symbol("namespace", name, statement_original, {}, {}, line, body_end);
            scopes.push_back({body_end, name, "namespace"});
            line = end_line;
            continue;
        }
        if (std::regex_match(statement_masked, match, type_pattern)) {
            const std::string kind = match[1].str();
            const std::string name = match[2].str();
            add_symbol(kind, name, statement_original, {}, {}, line, body_end);
            scopes.push_back({body_end, name, kind == "enum" ? "enum" : "type"});
            line = end_line;
            continue;
        }
        if (language == Language::TypeScript && std::regex_match(statement_masked, match, type_alias_pattern)) {
            add_symbol("type-alias", match[1].str(), statement_original, {}, collapse_space(match[2].str()),
                       line, static_cast<int>(end_line + 1));
            line = end_line;
            continue;
        }
        if (std::regex_match(statement_masked, match, function_pattern)) {
            const std::string name = match[1].str();
            add_symbol("function", name, statement_original, collapse_space(match[2].str()),
                       collapse_space(match[3].str()), line, body_end);
            if (delimiter == '{') scopes.push_back({body_end, name, "function"});
            line = end_line;
            continue;
        }

        bool in_function = false;
        bool directly_in_type = false;
        if (!scopes.empty()) directly_in_type = scopes.back().kind == "type";
        for (const Scope& scope : scopes) in_function = in_function || scope.kind == "function";
        if (directly_in_type && !in_function && std::regex_match(statement_masked, match, method_pattern)) {
            const std::string name = match[1].str();
            add_symbol("method", name, statement_original, collapse_space(match[2].str()),
                       collapse_space(match[3].str()), line, body_end);
            if (delimiter == '{') scopes.push_back({body_end, name, "function"});
            line = end_line;
            continue;
        }
        if (!in_function && std::regex_search(statement_masked, match, arrow_pattern)) {
            const std::string name = match[2].str();
            std::string parameters = match[3].str();
            if (parameters.size() >= 2 && parameters.front() == '(' && parameters.back() == ')')
                parameters = parameters.substr(1, parameters.size() - 2);
            add_symbol("function", name, statement_original, collapse_space(parameters), {}, line, body_end);
            if (delimiter == '{') scopes.push_back({body_end, name, "function"});
            line = end_line;
            continue;
        }
        if (!in_function && std::regex_search(statement_masked, match, function_expression_pattern)) {
            const std::string name = match[2].str();
            add_symbol("function", name, statement_original, collapse_space(match[3].str()), {}, line, body_end);
            if (delimiter == '{') scopes.push_back({body_end, name, "function"});
            line = end_line;
            continue;
        }
        if (directly_in_type && !in_function && std::regex_match(statement_masked, match, field_pattern)) {
            add_symbol("field", match[1].str(), statement_original, {}, {}, line,
                       static_cast<int>(end_line + 1));
            line = end_line;
            continue;
        }
        if (!in_function && !directly_in_type && std::regex_search(statement_masked, match, variable_pattern)) {
            const std::string kind = match[1].str() == "const" ? "constant" : "global";
            add_symbol(kind, match[2].str(), statement_original, {}, {}, line, body_end);
        }
        line = end_line;
    }
    return result;
}

std::string mask_css(const std::string& source) {
    std::string masked = source;
    enum class State { Code, BlockComment, Single, Double };
    State state = State::Code;
    bool escaped = false;
    for (std::size_t i = 0; i < source.size(); ++i) {
        const char ch = source[i];
        const char next = i + 1 < source.size() ? source[i + 1] : '\0';
        if (state == State::Code) {
            if (ch == '/' && next == '*') {
                masked[i] = masked[i + 1] = ' ';
                ++i;
                state = State::BlockComment;
            } else if (ch == '\'' || ch == '"') {
                masked[i] = ' ';
                state = ch == '\'' ? State::Single : State::Double;
                escaped = false;
            }
            continue;
        }
        if (ch != '\n') masked[i] = ' ';
        if (state == State::BlockComment) {
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

class SourceLineMap {
   public:
    explicit SourceLineMap(const std::string& source) {
        for (std::size_t pos = 0; pos < source.size(); ++pos) {
            if (source[pos] == '\n') newlines_.push_back(pos);
        }
    }

    int line_at(std::size_t offset) const {
        return 1 + static_cast<int>(
                       std::lower_bound(newlines_.begin(), newlines_.end(), offset) - newlines_.begin());
    }

   private:
    std::vector<std::size_t> newlines_;
};

ScanResult scan_css(const std::string& source) {
    ScanResult result;
    result.language = Language::Css;
    const std::string masked = mask_css(source);
    const std::vector<std::string> original_lines = source_lines(source);
    const SourceLineMap line_map(source);
    std::vector<std::pair<std::size_t, std::size_t>> brace_pairs;
    std::vector<std::size_t> open_stack;
    for (std::size_t pos = 0; pos < masked.size(); ++pos) {
        if (masked[pos] == '{') open_stack.push_back(pos);
        else if (masked[pos] == '}' && !open_stack.empty()) {
            brace_pairs.emplace_back(open_stack.back(), pos);
            open_stack.pop_back();
        }
    }
    std::sort(brace_pairs.begin(), brace_pairs.end());
    struct Scope { std::size_t end; std::string name; };
    std::vector<Scope> scopes;
    static const std::regex keyframes_pattern(
        R"(^@(?:-[A-Za-z]+-)?keyframes\s+([$A-Za-z_][$A-Za-z0-9_-]*))",
        std::regex::icase | std::regex::optimize);
    std::size_t brace_index = 0;
    for (std::size_t pos = 0; pos < masked.size(); ++pos) {
        if (masked[pos] != '{') continue;
        while (!scopes.empty() && pos > scopes.back().end) scopes.pop_back();
        std::size_t begin = pos;
        while (begin > 0 && masked[begin - 1] != ';' && masked[begin - 1] != '{' &&
               masked[begin - 1] != '}') --begin;
        while (begin < pos && std::isspace(static_cast<unsigned char>(masked[begin]))) ++begin;
        const std::string prelude = collapse_space(masked.substr(begin, pos - begin));
        if (prelude.empty()) continue;
        while (brace_index < brace_pairs.size() && brace_pairs[brace_index].first < pos) ++brace_index;
        const std::size_t close = brace_index < brace_pairs.size() && brace_pairs[brace_index].first == pos
                                      ? brace_pairs[brace_index].second
                                      : pos;
        Symbol symbol;
        std::smatch match;
        if (std::regex_search(prelude, match, keyframes_pattern)) {
            symbol.kind = "keyframes";
            symbol.name = match[1].str();
        } else if (prelude.front() == '@') {
            symbol.kind = "at-rule";
            symbol.name = prelude;
        } else {
            symbol.kind = "selector";
            symbol.name = prelude;
        }
        std::vector<std::string> parts;
        for (const Scope& scope : scopes) parts.push_back(scope.name);
        symbol.qualified_name = join_qualified(parts, symbol.name);
        symbol.signature = prelude + " {";
        symbol.line_start = line_map.line_at(begin);
        symbol.line_end = line_map.line_at(close);
        symbol.documentation = preceding_c_comment(original_lines,
                                                   static_cast<std::size_t>(symbol.line_start - 1));
        symbol.signature_hash = fnv1a(symbol.signature);
        symbol.body_hash = fnv1a(line_range(original_lines, symbol.line_start, symbol.line_end));
        result.symbols.push_back(std::move(symbol));
        scopes.push_back({close, result.symbols.back().name});
    }

    static const std::regex custom_property(
        R"((--[A-Za-z_][A-Za-z0-9_-]*)\s*:)", std::regex::optimize);
    const std::vector<std::string> masked_lines = source_lines(masked);
    for (std::size_t line = 0; line < masked_lines.size(); ++line) {
        for (std::sregex_iterator it(masked_lines[line].begin(), masked_lines[line].end(), custom_property), end;
             it != end; ++it) {
            Symbol symbol;
            symbol.kind = "custom-property";
            symbol.name = (*it)[1].str();
            symbol.qualified_name = symbol.name;
            symbol.signature = collapse_space(original_lines[line]);
            symbol.line_start = symbol.line_end = static_cast<int>(line + 1);
            symbol.signature_hash = symbol.body_hash = fnv1a(symbol.signature);
            result.symbols.push_back(std::move(symbol));
        }
    }
    std::stable_sort(result.symbols.begin(), result.symbols.end(), [](const Symbol& left, const Symbol& right) {
        return left.line_start < right.line_start;
    });
    return result;
}

std::size_t html_tag_end(const std::string& source, std::size_t begin) {
    char quote = '\0';
    for (std::size_t pos = begin; pos < source.size(); ++pos) {
        const char ch = source[pos];
        if (quote != '\0') {
            if (ch == quote) quote = '\0';
        } else if (ch == '\'' || ch == '"') {
            quote = ch;
        } else if (ch == '>') {
            return pos;
        }
    }
    return std::string::npos;
}

std::string html_attribute(const std::string& tag, const std::string& name) {
    auto attribute_character = [](unsigned char byte) {
        return std::isalnum(byte) || byte == '-' || byte == '_' || byte == ':';
    };
    std::size_t pos = 1;
    while (pos < tag.size() && !std::isspace(static_cast<unsigned char>(tag[pos])) &&
           tag[pos] != '>' && tag[pos] != '/') ++pos;
    while (pos < tag.size()) {
        while (pos < tag.size() &&
               (std::isspace(static_cast<unsigned char>(tag[pos])) || tag[pos] == '/')) ++pos;
        const std::size_t attribute_begin = pos;
        while (pos < tag.size() && attribute_character(static_cast<unsigned char>(tag[pos]))) ++pos;
        if (pos == attribute_begin) {
            ++pos;
            continue;
        }
        const std::string attribute_name = ascii_lower(tag.substr(attribute_begin, pos - attribute_begin));
        while (pos < tag.size() && std::isspace(static_cast<unsigned char>(tag[pos]))) ++pos;
        if (pos >= tag.size() || tag[pos] != '=') continue;
        ++pos;
        while (pos < tag.size() && std::isspace(static_cast<unsigned char>(tag[pos]))) ++pos;
        const char quote = pos < tag.size() && (tag[pos] == '\'' || tag[pos] == '"') ? tag[pos++] : '\0';
        const std::size_t value_begin = pos;
        if (quote != '\0') {
            while (pos < tag.size() && tag[pos] != quote) ++pos;
        } else {
            while (pos < tag.size() && !std::isspace(static_cast<unsigned char>(tag[pos])) &&
                   tag[pos] != '>') ++pos;
        }
        if (attribute_name == name) return tag.substr(value_begin, pos - value_begin);
        if (quote != '\0' && pos < tag.size()) ++pos;
    }
    return {};
}

void append_embedded_symbols(ScanResult& destination, ScanResult embedded, int line_offset) {
    for (Symbol& symbol : embedded.symbols) {
        symbol.line_start += line_offset;
        symbol.line_end += line_offset;
        destination.symbols.push_back(std::move(symbol));
    }
}

ScanResult scan_html(const std::string& source, bool embedded_languages = true) {
    ScanResult result;
    result.language = embedded_languages ? Language::Html : Language::HtmlOnly;
    const std::string lower = ascii_lower(source);
    const SourceLineMap line_map(source);
    static const std::regex opening_tag(
        R"(^<\s*([A-Za-z][A-Za-z0-9:-]*))", std::regex::optimize);
    for (std::size_t pos = 0; pos < source.size();) {
        const std::size_t open = source.find('<', pos);
        if (open == std::string::npos) break;
        if (lower.compare(open, 4, "<!--") == 0) {
            const std::size_t close = lower.find("-->", open + 4);
            pos = close == std::string::npos ? source.size() : close + 3;
            continue;
        }
        if (open + 1 < source.size() && (source[open + 1] == '/' || source[open + 1] == '!')) {
            const std::size_t end = html_tag_end(source, open + 1);
            pos = end == std::string::npos ? source.size() : end + 1;
            continue;
        }
        const std::size_t end = html_tag_end(source, open + 1);
        if (end == std::string::npos) break;
        const std::string tag = source.substr(open, end - open + 1);
        std::smatch match;
        if (!std::regex_search(tag, match, opening_tag)) {
            pos = end + 1;
            continue;
        }
        const std::string tag_name = ascii_lower(match[1].str());
        const std::string id = html_attribute(tag, "id");
        if (!id.empty() || tag_name.find('-') != std::string::npos) {
            Symbol symbol;
            symbol.kind = id.empty() ? "custom-element" : "element";
            symbol.name = id.empty() ? tag_name : id;
            symbol.qualified_name = id.empty() ? tag_name : "#" + id;
            symbol.signature = collapse_space(tag);
            symbol.line_start = line_map.line_at(open);
            symbol.line_end = line_map.line_at(end);
            symbol.signature_hash = symbol.body_hash = fnv1a(symbol.signature);
            result.symbols.push_back(std::move(symbol));
        }

        const bool raw_script = tag_name == "script";
        const bool raw_style = tag_name == "style";
        if (!embedded_languages && (raw_script || raw_style)) {
            const std::string closing_tag = "</" + tag_name;
            const std::size_t close = lower.find(closing_tag, end + 1);
            if (close == std::string::npos) break;
            const std::size_t closing_end = html_tag_end(source, close + 2);
            pos = closing_end == std::string::npos ? source.size() : closing_end + 1;
            continue;
        }
        const bool script = embedded_languages && raw_script;
        const bool style = embedded_languages && raw_style;
        if (!script && !style) {
            pos = end + 1;
            continue;
        }
        const std::string closing_tag = "</" + tag_name;
        const std::size_t close = lower.find(closing_tag, end + 1);
        const std::size_t content_end = close == std::string::npos ? source.size() : close;
        const std::size_t content_begin = end + 1;
        const std::string embedded_source = source.substr(content_begin, content_end - content_begin);
        const int line_offset = line_map.line_at(content_begin) - 1;
        if (style) {
            append_embedded_symbols(result, scan_css(embedded_source), line_offset);
        } else {
            const std::string type = ascii_lower(html_attribute(tag, "type"));
            const bool data_script = type == "application/json" || type == "application/ld+json" ||
                                     type == "importmap" || type == "speculationrules";
            if (!data_script) {
                const bool typescript = type.find("typescript") != std::string::npos;
                append_embedded_symbols(result,
                                        scan_ecmascript(embedded_source,
                                                           typescript ? Language::TypeScript
                                                                      : Language::JavaScript),
                                        line_offset);
            }
        }
        if (close == std::string::npos) break;
        const std::size_t closing_end = html_tag_end(source, close + 2);
        pos = closing_end == std::string::npos ? source.size() : closing_end + 1;
    }
    std::stable_sort(result.symbols.begin(), result.symbols.end(), [](const Symbol& left, const Symbol& right) {
        return left.line_start < right.line_start;
    });
    return result;
}

}  // namespace

const char* language_name(Language language) {
    switch (language) {
        case Language::Markdown: return "Markdown";
        case Language::Python: return "Python";
        case Language::C: return "C";
        case Language::Cpp: return "C++";
        case Language::CSharp: return "C#";
        case Language::Java: return "Java";
        case Language::JavaScript: return "JavaScript";
        case Language::TypeScript: return "TypeScript";
        case Language::Html: return "HTML";
        case Language::HtmlOnly: return "HTML-only";
        case Language::Css: return "CSS";
        case Language::Xml: return "XML";
        case Language::Json: return "JSON";
        case Language::Bash: return "Bash";
        case Language::Php: return "PHP";
        case Language::Perl: return "Perl";
        case Language::Ruby: return "Ruby";
        case Language::Rust: return "Rust";
        case Language::Go: return "Go";
        case Language::PowerShell: return "PowerShell";
        case Language::Assembly: return "Assembly";
        case Language::Sql: return "SQL";
        case Language::Toml: return "TOML";
        case Language::Yaml: return "YAML";
        case Language::Ini: return "INI";
    }
    return "Unknown";
}

bool language_for_path(const std::string& path, Language& language) {
    switch (highlight::detect_language(path)) {
        case highlight::Language::Markdown: language = Language::Markdown; return true;
        case highlight::Language::Python: language = Language::Python; return true;
        case highlight::Language::C: language = Language::C; return true;
        case highlight::Language::Cpp: language = Language::Cpp; return true;
        case highlight::Language::CSharp: language = Language::CSharp; return true;
        case highlight::Language::Java: language = Language::Java; return true;
        case highlight::Language::JavaScript: language = Language::JavaScript; return true;
        case highlight::Language::TypeScript: language = Language::TypeScript; return true;
        case highlight::Language::Html: language = Language::Html; return true;
        case highlight::Language::HtmlOnly: language = Language::HtmlOnly; return true;
        case highlight::Language::Css: language = Language::Css; return true;
        case highlight::Language::Xml: language = Language::Xml; return true;
        case highlight::Language::Json: language = Language::Json; return true;
        case highlight::Language::Bash: language = Language::Bash; return true;
        case highlight::Language::Php: language = Language::Php; return true;
        case highlight::Language::Perl: language = Language::Perl; return true;
        case highlight::Language::Ruby: language = Language::Ruby; return true;
        case highlight::Language::Rust: language = Language::Rust; return true;
        case highlight::Language::Go: language = Language::Go; return true;
        case highlight::Language::PowerShell: language = Language::PowerShell; return true;
        case highlight::Language::Assembly: language = Language::Assembly; return true;
        case highlight::Language::Sql: language = Language::Sql; return true;
        case highlight::Language::Toml: language = Language::Toml; return true;
        case highlight::Language::Yaml: language = Language::Yaml; return true;
        case highlight::Language::Ini: language = Language::Ini; return true;
        default: return false;
    }
}

namespace {

bool callable_symbol(const Symbol& symbol) {
    return symbol.kind == "function" || symbol.kind == "method";
}

int enclosing_symbol_index(const std::vector<Symbol>& symbols, int line) {
    int best = -1;
    int best_span = std::numeric_limits<int>::max();
    bool best_callable = false;
    for (std::size_t index = 0; index < symbols.size(); ++index) {
        const Symbol& symbol = symbols[index];
        if (line < symbol.line_start || line > symbol.line_end) continue;
        const bool candidate_callable = callable_symbol(symbol);
        const int span = symbol.line_end - symbol.line_start;
        if (best < 0 || (candidate_callable && !best_callable) ||
            (candidate_callable == best_callable && span < best_span)) {
            best = static_cast<int>(index);
            best_span = span;
            best_callable = candidate_callable;
        }
    }
    return best;
}

int declaration_symbol_index(const std::vector<Symbol>& symbols,
                             int line,
                             const std::string& kind = {}) {
    for (std::size_t index = 0; index < symbols.size(); ++index) {
        const Symbol& symbol = symbols[index];
        if (symbol.line_start == line && (kind.empty() || symbol.kind == kind))
            return static_cast<int>(index);
    }
    return -1;
}

std::string reference_evidence(const std::string& line) {
    std::string evidence = collapse_space(line);
    if (evidence.size() > 200) {
        evidence.resize(197);
        evidence += "...";
    }
    return evidence;
}

std::string last_component(const std::string& target) {
    std::size_t separator = target.rfind("::");
    const std::size_t dot = target.rfind('.');
    const std::size_t arrow = target.rfind("->");
    if (dot != std::string::npos && (separator == std::string::npos || dot > separator))
        separator = dot;
    if (arrow != std::string::npos &&
        (separator == std::string::npos || arrow + 1 > separator))
        return target.substr(arrow + 2);
    return separator == std::string::npos
               ? target
               : target.substr(separator + (target.compare(separator, 2, "::") == 0 ? 2 : 1));
}

std::string target_qualifier(const std::string& target) {
    const std::string simple = last_component(target);
    if (simple.size() >= target.size()) return {};
    std::size_t size = target.size() - simple.size();
    if (size >= 2 && target.compare(size - 2, 2, "::") == 0) size -= 2;
    else if (size >= 2 && target.compare(size - 2, 2, "->") == 0) size -= 2;
    else if (size >= 1) --size;
    return target.substr(0, size);
}

bool ignored_call_word(const std::string& name) {
    static const std::set<std::string> ignored = {
        "if",       "for",       "while",    "switch",   "catch",
        "sizeof",   "alignof",   "decltype", "typeof",   "return",
        "new",      "delete",    "class",    "def",      "assert",
        "with",     "lambda",    "match",    "case",     "static_cast",
        "const_cast", "dynamic_cast", "reinterpret_cast"};
    return ignored.find(name) != ignored.end();
}

void append_reference(ScanResult& result,
                      std::set<std::string>& seen,
                      Reference reference) {
    if (reference.target_spelling.empty()) return;
    const std::string key =
        reference.kind + "\n" + reference.target_spelling + "\n" +
        std::to_string(reference.line) + "\n" +
        std::to_string(reference.source_symbol_index);
    if (!seen.insert(key).second) return;
    result.references.push_back(std::move(reference));
}

void scan_python_references(const std::string& source, ScanResult& result) {
    const std::vector<std::string> original = source_lines(source);
    const std::vector<std::string> masked = source_lines(mask_python(source));
    std::set<std::string> seen;
    std::map<std::pair<int, std::string>, std::string> receiver_types;
    static const std::regex import_pattern(
        R"(^\s*import\s+([A-Za-z_][A-Za-z0-9_\.]*))", std::regex::optimize);
    static const std::regex from_pattern(
        R"(^\s*from\s+([A-Za-z_\.][A-Za-z0-9_\.]*)\s+import\s+([A-Za-z_*][A-Za-z0-9_]*))",
        std::regex::optimize);
    static const std::regex inheritance_pattern(
        R"(^\s*class\s+[A-Za-z_][A-Za-z0-9_]*\s*\(([^\)]*)\))",
        std::regex::optimize);
    static const std::regex inferred_type_pattern(
        R"(\b([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([A-Z][A-Za-z0-9_\.]*)\s*\()",
        std::regex::optimize);
    static const std::regex call_pattern(
        R"(([A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*)\s*\()",
        std::regex::optimize);

    for (std::size_t row = 0; row < masked.size(); ++row) {
        const int line = static_cast<int>(row + 1);
        const int enclosing = enclosing_symbol_index(result.symbols, line);
        const std::string evidence = reference_evidence(masked[row]);
        std::smatch match;
        const bool has_import =
            masked[row].find("import") != std::string::npos;
        const bool has_call = masked[row].find('(') != std::string::npos;
        if (has_import &&
            std::regex_search(masked[row], match, from_pattern)) {
            Reference reference;
            reference.kind = "import";
            reference.target_spelling = match[1].str() + "." + match[2].str();
            reference.qualifier = match[1].str();
            reference.line = line;
            reference.source_symbol_index = enclosing;
            reference.confidence = 0.95;
            reference.evidence = evidence;
            append_reference(result, seen, std::move(reference));
        } else if (has_import &&
                   std::regex_search(masked[row], match, import_pattern)) {
            Reference reference;
            reference.kind = "import";
            reference.target_spelling = match[1].str();
            reference.line = line;
            reference.source_symbol_index = enclosing;
            reference.confidence = 0.95;
            reference.evidence = evidence;
            append_reference(result, seen, std::move(reference));
        }
        if (masked[row].find("class") != std::string::npos &&
            std::regex_search(masked[row], match, inheritance_pattern)) {
            const int source_class = declaration_symbol_index(result.symbols, line, "class");
            std::istringstream bases(match[1].str());
            std::string base;
            while (std::getline(bases, base, ',')) {
                base = trim(std::move(base));
                const std::size_t bracket = base.find('[');
                if (bracket != std::string::npos) base.resize(bracket);
                if (base.empty() || base.find('=') != std::string::npos) continue;
                Reference reference;
                reference.kind = "inherit";
                reference.target_spelling = base;
                reference.qualifier = target_qualifier(base);
                reference.line = line;
                reference.source_symbol_index = source_class;
                reference.confidence = 0.85;
                reference.evidence = evidence;
                append_reference(result, seen, std::move(reference));
            }
        }
        if (has_call && masked[row].find('=') != std::string::npos) {
            for (std::sregex_iterator it(masked[row].begin(),
                                         masked[row].end(),
                                         inferred_type_pattern),
                 end;
                 it != end; ++it) {
                receiver_types[{enclosing, (*it)[1].str()}] =
                    (*it)[2].str();
            }
        }
        if (!has_call) continue;
        for (std::sregex_iterator it(masked[row].begin(), masked[row].end(),
                                     call_pattern),
             end;
             it != end; ++it) {
            const std::string target = (*it)[1].str();
            const std::string simple = last_component(target);
            if (ignored_call_word(simple)) continue;
            if (enclosing >= 0) {
                const Symbol& source_symbol =
                    result.symbols[static_cast<std::size_t>(enclosing)];
                if (source_symbol.line_start == line && source_symbol.name == simple)
                    continue;
            }
            Reference reference;
            reference.kind =
                !target.empty() && target_qualifier(target).empty() &&
                        std::isupper(static_cast<unsigned char>(target.front()))
                    ? "instantiate"
                    : "call";
            reference.target_spelling = target;
            reference.qualifier = target_qualifier(target);
            reference.line = line;
            reference.source_symbol_index = enclosing;
            reference.confidence = reference.kind == "instantiate" ? 0.75 : 0.55;
            reference.evidence = evidence;
            if (!reference.qualifier.empty()) {
                if (reference.qualifier == "self" && enclosing >= 0) {
                    const std::string& qualified =
                        result.symbols[static_cast<std::size_t>(enclosing)]
                            .qualified_name;
                    const std::size_t parent = qualified.rfind("::");
                    if (parent != std::string::npos)
                        reference.receiver_type = qualified.substr(0, parent);
                } else {
                    const auto inferred =
                        receiver_types.find({enclosing, reference.qualifier});
                    if (inferred != receiver_types.end())
                        reference.receiver_type = inferred->second;
                    else if (std::isupper(
                                 static_cast<unsigned char>(
                                     reference.qualifier.front())))
                        reference.receiver_type = reference.qualifier;
                }
                if (!reference.receiver_type.empty()) reference.confidence = 0.72;
            }
            append_reference(result, seen, std::move(reference));
        }
    }
}

void scan_c_family_references(const std::string& source, ScanResult& result) {
    const std::vector<std::string> original = source_lines(source);
    const std::vector<std::string> masked = source_lines(mask_c_family(source));
    std::set<std::string> seen;
    std::map<std::pair<int, std::string>, std::string> receiver_types;
    static const std::regex include_pattern(
        R"(^\s*#\s*include\s*[<"]([^>"]+)[>"])", std::regex::optimize);
    static const std::regex include_marker(
        R"(^\s*#\s*include\b)", std::regex::optimize);
    static const std::regex inheritance_pattern(
        R"(\b(?:class|struct)\s+[A-Za-z_][A-Za-z0-9_]*\s*:\s*([^\{]+)\{)",
        std::regex::optimize);
    static const std::regex inheritance_access_pattern(
        R"(\b(public|private|protected|virtual)\b)", std::regex::optimize);
    static const std::regex new_pattern(
        R"(\bnew\s+([A-Za-z_][A-Za-z0-9_:<>]*)\s*[\(\{])",
        std::regex::optimize);
    static const std::regex variable_type_pattern(
        R"(\b([A-Z][A-Za-z0-9_:<>]*)\s*[*&]?\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?:[=\(\{;]))",
        std::regex::optimize);
    static const std::regex direct_instance_pattern(
        R"(\b([A-Z][A-Za-z0-9_:<>]*)\s+([A-Za-z_][A-Za-z0-9_]*)\s*[\(\{])",
        std::regex::optimize);
    static const std::regex call_pattern(
        R"(([A-Za-z_][A-Za-z0-9_]*(?:(?:::|\.|->)[A-Za-z_][A-Za-z0-9_]*)*)\s*\()",
        std::regex::optimize);
    static const std::regex new_prefix_pattern(
        R"(\bnew\s*$)", std::regex::optimize);

    for (std::size_t row = 0; row < masked.size(); ++row) {
        const int line = static_cast<int>(row + 1);
        const int enclosing = enclosing_symbol_index(result.symbols, line);
        const std::string evidence = reference_evidence(masked[row]);
        const bool has_call = masked[row].find('(') != std::string::npos;
        std::smatch match;
        if (masked[row].find('#') != std::string::npos &&
            std::regex_search(masked[row], include_marker) &&
            std::regex_search(original[row], match, include_pattern)) {
            Reference reference;
            reference.kind = "include";
            reference.target_spelling = match[1].str();
            reference.line = line;
            reference.source_symbol_index = enclosing;
            reference.confidence = 0.98;
            reference.evidence = evidence;
            append_reference(result, seen, std::move(reference));
        }
        if (masked[row].find(':') != std::string::npos &&
            (masked[row].find("class") != std::string::npos ||
             masked[row].find("struct") != std::string::npos) &&
            std::regex_search(masked[row], match, inheritance_pattern)) {
            const int source_type = declaration_symbol_index(result.symbols, line);
            std::istringstream bases(match[1].str());
            std::string base;
            while (std::getline(bases, base, ',')) {
                base = std::regex_replace(
                    base, inheritance_access_pattern, " ");
                base = trim(std::move(base));
                if (base.empty()) continue;
                Reference reference;
                reference.kind = "inherit";
                reference.target_spelling = base;
                reference.qualifier = target_qualifier(base);
                reference.line = line;
                reference.source_symbol_index = source_type;
                reference.confidence = 0.9;
                reference.evidence = evidence;
                append_reference(result, seen, std::move(reference));
            }
        }
        if (masked[row].find_first_of("=;({") != std::string::npos) {
            for (std::sregex_iterator it(masked[row].begin(),
                                         masked[row].end(),
                                         variable_type_pattern),
                 end;
                 it != end; ++it) {
                receiver_types[{enclosing, (*it)[2].str()}] =
                    (*it)[1].str();
            }
        }
        if (masked[row].find("new") != std::string::npos) {
            for (std::sregex_iterator it(masked[row].begin(),
                                         masked[row].end(), new_pattern),
                 end;
                 it != end; ++it) {
                Reference reference;
                reference.kind = "instantiate";
                reference.target_spelling = (*it)[1].str();
                reference.qualifier =
                    target_qualifier(reference.target_spelling);
                reference.line = line;
                reference.source_symbol_index = enclosing;
                reference.confidence = 0.9;
                reference.evidence = evidence;
                append_reference(result, seen, std::move(reference));
            }
        }
        if (has_call || masked[row].find('{') != std::string::npos) {
            for (std::sregex_iterator it(masked[row].begin(),
                                         masked[row].end(),
                                         direct_instance_pattern),
                 end;
                 it != end; ++it) {
                const std::string variable = (*it)[2].str();
                const int declaration =
                    declaration_symbol_index(result.symbols, line);
                if (declaration >= 0 &&
                    result.symbols[static_cast<std::size_t>(declaration)]
                            .name == variable)
                    continue;
                Reference reference;
                reference.kind = "instantiate";
                reference.target_spelling = (*it)[1].str();
                reference.qualifier =
                    target_qualifier(reference.target_spelling);
                reference.line = line;
                reference.source_symbol_index = enclosing;
                reference.confidence = 0.72;
                reference.evidence = evidence;
                append_reference(result, seen, std::move(reference));
            }
        }
        if (!has_call) continue;
        for (std::sregex_iterator it(masked[row].begin(), masked[row].end(), call_pattern),
             end;
             it != end; ++it) {
            const std::string target = (*it)[1].str();
            const std::string simple = last_component(target);
            if (ignored_call_word(simple)) continue;
            if (enclosing >= 0) {
                const Symbol& source_symbol =
                    result.symbols[static_cast<std::size_t>(enclosing)];
                if (source_symbol.line_start == line && source_symbol.name == simple)
                    continue;
            }
            // `new Type(` is already represented as an instantiation.
            const std::size_t match_pos = static_cast<std::size_t>((*it).position());
            const std::string prefix = masked[row].substr(0, match_pos);
            if (std::regex_search(prefix, new_prefix_pattern)) continue;
            Reference reference;
            reference.kind = "call";
            reference.target_spelling = target;
            reference.qualifier = target_qualifier(target);
            reference.line = line;
            reference.source_symbol_index = enclosing;
            reference.confidence = 0.58;
            reference.evidence = evidence;
            if (!reference.qualifier.empty()) {
                if (reference.qualifier == "this" && enclosing >= 0) {
                    const std::string& qualified =
                        result.symbols[static_cast<std::size_t>(enclosing)]
                            .qualified_name;
                    const std::size_t parent = qualified.rfind("::");
                    if (parent != std::string::npos)
                        reference.receiver_type = qualified.substr(0, parent);
                } else {
                    const auto inferred =
                        receiver_types.find({enclosing, reference.qualifier});
                    if (inferred != receiver_types.end())
                        reference.receiver_type = inferred->second;
                    else if (target.find("::") != std::string::npos)
                        reference.receiver_type = reference.qualifier;
                }
                if (!reference.receiver_type.empty()) reference.confidence = 0.75;
            }
            append_reference(result, seen, std::move(reference));
        }
    }
}

}  // namespace

ScanResult scan_source(const std::string& path, const std::string& source, Language language) {
    ScanResult result;
    switch (language) {
        case Language::Markdown:
        case Language::CSharp:
        case Language::Java:
        case Language::Xml:
        case Language::Json:
        case Language::Bash:
        case Language::Php:
        case Language::Perl:
        case Language::Ruby:
        case Language::Rust:
        case Language::Go:
        case Language::PowerShell:
        case Language::Assembly:
        case Language::Sql:
        case Language::Toml:
        case Language::Yaml:
        case Language::Ini: result = scan_additional_source(source, language); break;
        case Language::Python: result = scan_python(source); break;
        case Language::C:
        case Language::Cpp: result = scan_c_family(path, source, language); break;
        case Language::JavaScript:
        case Language::TypeScript: result = scan_ecmascript(source, language); break;
        case Language::Html: result = scan_html(source); break;
        case Language::HtmlOnly: result = scan_html(source, false); break;
        case Language::Css: result = scan_css(source); break;
    }
    if (result.language == Language::Python)
        scan_python_references(source, result);
    else if (result.language == Language::C || result.language == Language::Cpp)
        scan_c_family_references(source, result);
    return result;
}

}  // namespace ainiux::agent::index
