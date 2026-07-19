#include "agent/index/scanner_extra.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

#include "common.hpp"

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
        if (ch == ' ') ++value;
        else if (ch == '\t') value += 8 - value % 8;
        else break;
    }
    return value;
}

std::string join_qualified(const std::vector<std::string>& parts, const std::string& name) {
    std::string output;
    for (const std::string& part : parts) {
        if (part.empty()) continue;
        if (!output.empty()) output += "::";
        output += part;
    }
    if (!output.empty() && !name.empty()) output += "::";
    output += name;
    return output;
}

std::string preceding_line_comment(const std::vector<std::string>& lines,
                                   std::size_t line,
                                   const std::vector<std::string>& prefixes) {
    std::vector<std::string> pieces;
    std::size_t pos = line;
    while (pos > 0) {
        std::string current = trim(lines[pos - 1]);
        bool matched = false;
        for (const std::string& prefix : prefixes) {
            if (current.rfind(prefix, 0) != 0) continue;
            current.erase(0, prefix.size());
            while (!current.empty() &&
                   (current.front() == prefix.front() || current.front() == '!' ||
                    current.front() == ' ')) {
                current.erase(0, 1);
            }
            pieces.push_back(trim(std::move(current)));
            --pos;
            matched = true;
            break;
        }
        if (!matched) break;
    }
    std::reverse(pieces.begin(), pieces.end());
    std::string output;
    for (const std::string& piece : pieces) {
        if (!output.empty()) output.push_back(' ');
        output += piece;
    }
    return bounded_documentation(std::move(output));
}

void append_symbol(ScanResult& result,
                   const std::vector<std::string>& original,
                   const std::string& kind,
                   const std::string& name,
                   const std::string& qualified_name,
                   const std::string& signature,
                   int line_start,
                   int line_end,
                   const std::string& documentation = {},
                   const std::string& parameters = {},
                   const std::string& return_type = {}) {
    Symbol symbol;
    symbol.kind = kind;
    symbol.name = name;
    symbol.qualified_name = qualified_name.empty() ? name : qualified_name;
    symbol.signature = collapse_space(signature);
    symbol.parameters = collapse_space(parameters);
    symbol.return_type = collapse_space(return_type);
    symbol.line_start = std::max(1, line_start);
    symbol.line_end = std::max(symbol.line_start, line_end);
    symbol.documentation = bounded_documentation(documentation);
    symbol.signature_hash = fnv1a(symbol.signature);
    symbol.body_hash = fnv1a(line_range(original, symbol.line_start, symbol.line_end));
    result.symbols.push_back(std::move(symbol));
}

struct MaskOptions {
    bool slash_comments = false;
    bool block_comments = false;
    bool hash_comments = false;
    bool dash_comments = false;
    bool semicolon_comments = false;
    bool powershell_blocks = false;
    bool backtick_strings = false;
    bool rust_lifetimes = false;
};

std::string mask_source(const std::string& source, const MaskOptions& options) {
    std::string masked = source;
    enum class State { Code, LineComment, BlockComment, PowerBlock, Single, Double, Backtick };
    State state = State::Code;
    bool escaped = false;
    for (std::size_t i = 0; i < source.size(); ++i) {
        const char ch = source[i];
        const char next = i + 1 < source.size() ? source[i + 1] : '\0';
        if (state == State::Code) {
            if (options.powershell_blocks && ch == '<' && next == '#') {
                masked[i] = masked[i + 1] = ' ';
                ++i;
                state = State::PowerBlock;
            } else if (options.slash_comments && ch == '/' && next == '/') {
                masked[i] = masked[i + 1] = ' ';
                ++i;
                state = State::LineComment;
            } else if (options.block_comments && ch == '/' && next == '*') {
                masked[i] = masked[i + 1] = ' ';
                ++i;
                state = State::BlockComment;
            } else if (options.dash_comments && ch == '-' && next == '-') {
                masked[i] = masked[i + 1] = ' ';
                ++i;
                state = State::LineComment;
            } else if (options.hash_comments && ch == '#') {
                masked[i] = ' ';
                state = State::LineComment;
            } else if (options.semicolon_comments && ch == ';') {
                masked[i] = ' ';
                state = State::LineComment;
            } else if (ch == '\'' && options.rust_lifetimes &&
                       std::isalpha(static_cast<unsigned char>(next)) &&
                       (i + 2 >= source.size() || source[i + 2] != '\'')) {
                continue;
            } else if (ch == '\'') {
                masked[i] = ' ';
                state = State::Single;
                escaped = false;
            } else if (ch == '"') {
                masked[i] = ' ';
                state = State::Double;
                escaped = false;
            } else if (options.backtick_strings && ch == '`') {
                masked[i] = ' ';
                state = State::Backtick;
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
        } else if (state == State::PowerBlock) {
            if (ch == '#' && next == '>') {
                masked[i + 1] = ' ';
                ++i;
                state = State::Code;
            }
        } else {
            const char delimiter = state == State::Single ? '\'' :
                                   (state == State::Double ? '"' : '`');
            if (!escaped && ch == delimiter) state = State::Code;
            if (!escaped && ch == '\\') escaped = true;
            else escaped = false;
        }
    }
    return masked;
}

int matching_brace_line(const std::vector<std::string>& masked,
                        std::size_t start_line,
                        std::size_t open_column) {
    int depth = 0;
    for (std::size_t row = start_line; row < masked.size(); ++row) {
        const std::size_t begin = row == start_line ? open_column : 0;
        for (std::size_t column = begin; column < masked[row].size(); ++column) {
            if (masked[row][column] == '{') ++depth;
            else if (masked[row][column] == '}' && depth > 0 && --depth == 0)
                return static_cast<int>(row + 1);
        }
    }
    return static_cast<int>(masked.size());
}

struct LogicalStatement {
    std::string masked;
    std::string original;
    std::size_t end_line = 0;
    std::size_t delimiter_column = std::string::npos;
    char delimiter = '\0';
};

LogicalStatement collect_statement(const std::vector<std::string>& masked,
                                   const std::vector<std::string>& original,
                                   std::size_t start) {
    LogicalStatement statement;
    statement.end_line = start;
    int grouping = 0;
    for (; statement.end_line < masked.size() && statement.end_line - start < 64;
         ++statement.end_line) {
        const std::string& current = masked[statement.end_line];
        for (std::size_t column = 0; column < current.size(); ++column) {
            const char ch = current[column];
            if (ch == '(' || ch == '[') ++grouping;
            else if ((ch == ')' || ch == ']') && grouping > 0) --grouping;
            else if (grouping == 0 && (ch == ';' || ch == '{')) {
                statement.delimiter = ch;
                statement.delimiter_column = column;
                break;
            }
        }
        if (!statement.masked.empty()) {
            statement.masked.push_back(' ');
            statement.original.push_back(' ');
        }
        const std::size_t length = statement.delimiter == '\0'
                                       ? current.size()
                                       : statement.delimiter_column + 1;
        statement.masked += trim(current.substr(0, length));
        statement.original += trim(original[statement.end_line].substr(0, length));
        if (statement.delimiter != '\0') break;
    }
    if (statement.end_line >= masked.size()) statement.end_line = masked.empty() ? 0 : masked.size() - 1;
    statement.masked = collapse_space(statement.masked);
    statement.original = collapse_space(statement.original);
    return statement;
}

int statement_end(const LogicalStatement& statement, const std::vector<std::string>& masked) {
    return statement.delimiter == '{'
               ? matching_brace_line(masked, statement.end_line, statement.delimiter_column)
               : static_cast<int>(statement.end_line + 1);
}

ScanResult scan_markdown(const std::string& source) {
    ScanResult result;
    result.language = Language::Markdown;
    const std::vector<std::string> lines = source_lines(source);
    std::vector<std::string> headings(6);
    bool fenced = false;
    char fence_character = '\0';
    std::size_t fence_length = 0;
    static const std::regex atx(R"(^\s*(#{1,6})\s+(.+?)\s*#*\s*$)", std::regex::optimize);
    static const std::regex reference(
        R"(^\s*\[([^\]]+)\]:\s*(\S.*)$)", std::regex::optimize);
    for (std::size_t line = 0; line < lines.size(); ++line) {
        const std::string stripped = trim(lines[line]);
        std::size_t run = 0;
        while (run < stripped.size() &&
               (stripped[run] == '`' || stripped[run] == '~') &&
               (run == 0 || stripped[run] == stripped[0])) ++run;
        if (run >= 3) {
            if (!fenced) {
                fenced = true;
                fence_character = stripped[0];
                fence_length = run;
            } else if (stripped[0] == fence_character && run >= fence_length) {
                fenced = false;
            }
            continue;
        }
        if (fenced) continue;
        std::smatch match;
        int level = 0;
        std::string name;
        if (std::regex_match(lines[line], match, atx)) {
            level = static_cast<int>(match[1].str().size());
            name = trim(match[2].str());
        } else if (line + 1 < lines.size() && !stripped.empty()) {
            const std::string underline = trim(lines[line + 1]);
            if (underline.size() >= 3 &&
                underline.find_first_not_of('=') == std::string::npos) level = 1;
            else if (underline.size() >= 3 &&
                     underline.find_first_not_of('-') == std::string::npos) level = 2;
            if (level != 0) name = stripped;
        }
        if (level != 0) {
            headings[static_cast<std::size_t>(level - 1)] = name;
            for (std::size_t pos = static_cast<std::size_t>(level); pos < headings.size(); ++pos)
                headings[pos].clear();
            std::vector<std::string> parents;
            for (int pos = 0; pos < level - 1; ++pos) {
                if (!headings[static_cast<std::size_t>(pos)].empty())
                    parents.push_back(headings[static_cast<std::size_t>(pos)]);
            }
            int end_line = static_cast<int>(lines.size());
            for (std::size_t pos = line + 1; pos < lines.size(); ++pos) {
                std::smatch next;
                if (std::regex_match(lines[pos], next, atx) &&
                    static_cast<int>(next[1].str().size()) <= level) {
                    end_line = static_cast<int>(pos);
                    break;
                }
            }
            append_symbol(result, lines, "heading", name, join_qualified(parents, name),
                          lines[line], static_cast<int>(line + 1), end_line);
            if (line + 1 < lines.size() &&
                (trim(lines[line + 1]).find_first_not_of("=-") == std::string::npos)) ++line;
            continue;
        }
        if (std::regex_match(lines[line], match, reference)) {
            append_symbol(result, lines, "link-reference", match[1].str(), match[1].str(),
                          lines[line], static_cast<int>(line + 1), static_cast<int>(line + 1));
        }
    }
    return result;
}

ScanResult scan_managed(const std::string& source, Language language) {
    ScanResult result;
    result.language = language;
    const std::vector<std::string> original = source_lines(source);
    const std::vector<std::string> masked = source_lines(mask_source(
        source, MaskOptions{true, true, false, false, false, false, false, false}));
    struct Scope { int end_line; std::string name; std::string kind; };
    std::vector<Scope> scopes;
    static const std::regex namespace_pattern(
        R"(^\s*(?:namespace|package)\s+([A-Za-z_][A-Za-z0-9_.]*)\s*[;{]\s*$)",
        std::regex::optimize);
    static const std::regex type_pattern(
        R"((?:^|\s)(class|struct|interface|enum|record)(?:\s+class|\s+struct)?\s+([A-Za-z_$][A-Za-z0-9_$]*))",
        std::regex::optimize);
    static const std::regex callable_pattern(
        R"(^\s*(.*?)\b(~?[A-Za-z_$][A-Za-z0-9_$]*)\s*\((.*)\)\s*(?:(?:throws|where)\s+[^;{]+)?\s*[;{]\s*$)",
        std::regex::optimize);
    static const std::regex property_pattern(
        R"(^\s*.*\b([A-Za-z_$][A-Za-z0-9_$]*)\s*\{\s*$)", std::regex::optimize);
    static const std::regex field_pattern(
        R"(^\s*(.*?)\b([A-Za-z_$][A-Za-z0-9_$]*)\s*(?:=[^;]*)?;\s*$)",
        std::regex::optimize);
    for (std::size_t line = 0; line < masked.size(); ++line) {
        while (!scopes.empty() && scopes.back().end_line < static_cast<int>(line + 1)) scopes.pop_back();
        const std::string stripped = trim(masked[line]);
        if (stripped.empty() || stripped.front() == '@' || stripped.front() == '[' ||
            stripped.front() == '}') continue;
        bool in_function = false;
        std::vector<std::string> parts;
        for (const Scope& scope : scopes) {
            if (scope.kind == "function") in_function = true;
            else if (!scope.name.empty()) parts.push_back(scope.name);
        }
        if (in_function) continue;
        const LogicalStatement statement = collect_statement(masked, original, line);
        if (statement.delimiter == '\0') continue;
        const int body_end = statement_end(statement, masked);
        std::smatch match;
        if (std::regex_match(statement.masked, match, namespace_pattern)) {
            const std::string name = match[1].str();
            const std::string kind = language == Language::Java ? "package" : "namespace";
            append_symbol(result, original, kind, name, name, statement.original,
                          static_cast<int>(line + 1), body_end,
                          preceding_line_comment(original, line, {"//"}));
            scopes.push_back({statement.delimiter == '{' ? body_end : static_cast<int>(masked.size()),
                              name, "namespace"});
            line = statement.end_line;
            continue;
        }
        if (std::regex_search(statement.masked, match, type_pattern)) {
            const std::string kind = match[1].str();
            const std::string name = match[2].str();
            append_symbol(result, original, kind, name, join_qualified(parts, name),
                          statement.original, static_cast<int>(line + 1), body_end,
                          preceding_line_comment(original, line, {"//"}));
            if (statement.delimiter == '{') scopes.push_back({body_end, name, "type"});
            line = statement.end_line;
            continue;
        }
        bool in_type = false;
        for (const Scope& scope : scopes) in_type = in_type || scope.kind == "type";
        if (std::regex_match(statement.masked, match, callable_pattern)) {
            const std::string prefix = collapse_space(match[1].str());
            const std::string name = match[2].str();
            static const char* controls[] = {"if", "for", "while", "switch", "catch", "using",
                                             "lock", "return", "new"};
            bool control = prefix.find('=') != std::string::npos;
            for (const char* item : controls) control = control || name == item;
            if (!control) {
                append_symbol(result, original, in_type ? "method" : "function", name,
                              join_qualified(parts, name), statement.original,
                              static_cast<int>(line + 1), body_end,
                              preceding_line_comment(original, line, {"//"}), match[3].str(), prefix);
                if (statement.delimiter == '{') scopes.push_back({body_end, name, "function"});
                line = statement.end_line;
                continue;
            }
        }
        if (language == Language::CSharp && in_type && statement.delimiter == '{' &&
            statement.masked.find('(') == std::string::npos &&
            std::regex_match(statement.masked, match, property_pattern)) {
            const std::string name = match[1].str();
            append_symbol(result, original, "property", name, join_qualified(parts, name),
                          statement.original, static_cast<int>(line + 1), body_end,
                          preceding_line_comment(original, line, {"//"}));
            line = statement.end_line;
            continue;
        }
        if (std::regex_match(statement.masked, match, field_pattern) &&
            statement.masked.find('(') == std::string::npos &&
            statement.masked.rfind("import ", 0) != 0 && statement.masked.rfind("using ", 0) != 0) {
            const std::string name = match[2].str();
            const bool constant = statement.masked.find(" const ") != std::string::npos ||
                                  statement.masked.rfind("const ", 0) == 0 ||
                                  (statement.masked.find(" final ") != std::string::npos &&
                                   statement.masked.find(" static ") != std::string::npos);
            append_symbol(result, original, constant ? "constant" : (in_type ? "field" : "global"),
                          name, join_qualified(parts, name), statement.original,
                          static_cast<int>(line + 1), static_cast<int>(statement.end_line + 1),
                          preceding_line_comment(original, line, {"//"}));
        }
        line = statement.end_line;
    }
    return result;
}

ScanResult scan_rust(const std::string& source) {
    ScanResult result;
    result.language = Language::Rust;
    const std::vector<std::string> original = source_lines(source);
    const std::vector<std::string> masked = source_lines(mask_source(
        source, MaskOptions{true, true, false, false, false, false, false, true}));
    struct Scope { int end_line; std::string name; std::string kind; };
    std::vector<Scope> scopes;
    static const std::regex type_pattern(
        R"(^\s*(?:pub(?:\s*\([^)]*\))?\s+)?(mod|struct|enum|trait|union)\s+([A-Za-z_][A-Za-z0-9_]*))",
        std::regex::optimize);
    static const std::regex alias_pattern(
        R"(^\s*(?:pub(?:\s*\([^)]*\))?\s+)?type\s+([A-Za-z_][A-Za-z0-9_]*)\b)",
        std::regex::optimize);
    static const std::regex impl_pattern(
        R"(^\s*(?:unsafe\s+)?impl(?:\s*<[^>{;]*>)?\s+(.+?)(?:\s+where\s+[^\{]+)?\s*\{\s*$)",
        std::regex::optimize);
    static const std::regex function_pattern(
        R"(^\s*(?:pub(?:\s*\([^)]*\))?\s+)?(?:const\s+)?(?:async\s+)?(?:unsafe\s+)?(?:extern\s+)?fn\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?:<[^>{;]*>)?\s*\((.*)\)\s*(?:->\s*([^\{;]+))?\s*(?:where\s+[^\{;]+)?\s*[\{;]\s*$)",
        std::regex::optimize);
    static const std::regex value_pattern(
        R"(^\s*(?:pub(?:\s*\([^)]*\))?\s+)?(const|static)\s+(?:mut\s+)?([A-Za-z_][A-Za-z0-9_]*)\b)",
        std::regex::optimize);
    for (std::size_t line = 0; line < masked.size(); ++line) {
        while (!scopes.empty() && scopes.back().end_line < static_cast<int>(line + 1)) scopes.pop_back();
        if (trim(masked[line]).empty() || trim(masked[line]).front() == '#' ||
            trim(masked[line]).front() == '}') continue;
        bool in_function = false;
        std::vector<std::string> parts;
        for (const Scope& scope : scopes) {
            if (scope.kind == "function") in_function = true;
            else if (!scope.name.empty()) parts.push_back(scope.name);
        }
        if (in_function) continue;
        const LogicalStatement statement = collect_statement(masked, original, line);
        if (statement.delimiter == '\0') continue;
        const int body_end = statement_end(statement, masked);
        std::smatch match;
        if (std::regex_search(statement.masked, match, type_pattern)) {
            const std::string kind = match[1].str();
            const std::string name = match[2].str();
            append_symbol(result, original, kind, name, join_qualified(parts, name),
                          statement.original, static_cast<int>(line + 1), body_end,
                          preceding_line_comment(original, line, {"//"}));
            if (statement.delimiter == '{')
                scopes.push_back({body_end, name, kind == "mod" ? "namespace" : "type"});
            line = statement.end_line;
            continue;
        }
        if (std::regex_search(statement.masked, match, alias_pattern)) {
            const std::string name = match[1].str();
            append_symbol(result, original, "type-alias", name, join_qualified(parts, name),
                          statement.original, static_cast<int>(line + 1),
                          static_cast<int>(statement.end_line + 1),
                          preceding_line_comment(original, line, {"//"}));
            line = statement.end_line;
            continue;
        }
        if (std::regex_match(statement.masked, match, impl_pattern)) {
            std::string target = collapse_space(match[1].str());
            const std::size_t for_pos = target.rfind(" for ");
            if (for_pos != std::string::npos) target = trim(target.substr(for_pos + 5));
            const std::size_t generic = target.find('<');
            if (generic != std::string::npos) target.resize(generic);
            append_symbol(result, original, "impl", target, join_qualified(parts, target),
                          statement.original, static_cast<int>(line + 1), body_end,
                          preceding_line_comment(original, line, {"//"}));
            scopes.push_back({body_end, target, "type"});
            line = statement.end_line;
            continue;
        }
        if (std::regex_match(statement.masked, match, function_pattern)) {
            const std::string name = match[1].str();
            bool method = false;
            for (const Scope& scope : scopes) method = method || scope.kind == "type";
            append_symbol(result, original, method ? "method" : "function", name,
                          join_qualified(parts, name), statement.original,
                          static_cast<int>(line + 1), body_end,
                          preceding_line_comment(original, line, {"//"}), match[2].str(),
                          match[3].matched ? match[3].str() : std::string{});
            if (statement.delimiter == '{') scopes.push_back({body_end, name, "function"});
            line = statement.end_line;
            continue;
        }
        if (std::regex_search(statement.masked, match, value_pattern)) {
            const std::string name = match[2].str();
            append_symbol(result, original, "constant", name, join_qualified(parts, name),
                          statement.original, static_cast<int>(line + 1),
                          static_cast<int>(statement.end_line + 1),
                          preceding_line_comment(original, line, {"//"}));
        }
        line = statement.end_line;
    }
    return result;
}

ScanResult scan_go(const std::string& source) {
    ScanResult result;
    result.language = Language::Go;
    const std::vector<std::string> original = source_lines(source);
    const std::vector<std::string> masked = source_lines(mask_source(
        source, MaskOptions{true, true, false, false, false, false, true, false}));
    std::string package_name;
    struct Scope { int end_line; };
    std::vector<Scope> functions;
    static const std::regex package_pattern(
        R"(^\s*package\s+([A-Za-z_][A-Za-z0-9_]*)\s*$)", std::regex::optimize);
    static const std::regex type_pattern(
        R"(^\s*type\s+([A-Za-z_][A-Za-z0-9_]*)\s+(struct|interface|[^=\s]+|=))",
        std::regex::optimize);
    static const std::regex function_pattern(
        R"(^\s*func\s*(?:\(\s*[A-Za-z_][A-Za-z0-9_]*\s+\*?([A-Za-z_][A-Za-z0-9_]*)[^)]*\)\s*)?([A-Za-z_][A-Za-z0-9_]*)\s*\((.*)\)\s*(.*?)\s*\{\s*$)",
        std::regex::optimize);
    static const std::regex value_pattern(
        R"(^\s*(const|var)\s+([A-Za-z_][A-Za-z0-9_]*)\b)", std::regex::optimize);
    for (std::size_t line = 0; line < masked.size(); ++line) {
        while (!functions.empty() && functions.back().end_line < static_cast<int>(line + 1))
            functions.pop_back();
        if (trim(masked[line]).empty() || !functions.empty()) continue;
        std::smatch match;
        if (std::regex_match(masked[line], match, package_pattern)) {
            package_name = match[1].str();
            append_symbol(result, original, "package", package_name, package_name, original[line],
                          static_cast<int>(line + 1), static_cast<int>(line + 1),
                          preceding_line_comment(original, line, {"//"}));
            continue;
        }
        const LogicalStatement statement = collect_statement(masked, original, line);
        const std::vector<std::string> package = package_name.empty()
                                                     ? std::vector<std::string>{}
                                                     : std::vector<std::string>{package_name};
        if (std::regex_search(masked[line], match, type_pattern)) {
            const std::string name = match[1].str();
            const std::string form = match[2].str();
            const std::string kind = form == "=" ? "type-alias" : (form == "interface" ? "interface" : "type");
            const std::size_t open = masked[line].find('{');
            const int type_end = open == std::string::npos
                                     ? static_cast<int>(line + 1)
                                     : matching_brace_line(masked, line, open);
            append_symbol(result, original, kind, name, join_qualified(package, name),
                          original[line], static_cast<int>(line + 1), type_end,
                          preceding_line_comment(original, line, {"//"}));
            continue;
        }
        if (std::regex_search(masked[line], match, value_pattern)) {
            const std::string name = match[2].str();
            append_symbol(result, original, match[1].str() == "const" ? "constant" : "global",
                          name, join_qualified(package, name), original[line],
                          static_cast<int>(line + 1), static_cast<int>(line + 1),
                          preceding_line_comment(original, line, {"//"}));
            continue;
        }
        if (statement.delimiter == '\0') continue;
        const int body_end = statement_end(statement, masked);
        if (std::regex_match(statement.masked, match, function_pattern)) {
            const std::string receiver = match[1].matched ? match[1].str() : std::string{};
            const std::string name = match[2].str();
            std::vector<std::string> parts = package;
            if (!receiver.empty()) parts.push_back(receiver);
            append_symbol(result, original, receiver.empty() ? "function" : "method", name,
                          join_qualified(parts, name), statement.original,
                          static_cast<int>(line + 1), body_end,
                          preceding_line_comment(original, line, {"//"}), match[3].str(),
                          match[4].str());
            functions.push_back({body_end});
            line = statement.end_line;
            continue;
        }
        line = statement.end_line;
    }
    return result;
}

ScanResult scan_php(const std::string& source) {
    ScanResult result;
    result.language = Language::Php;
    const std::vector<std::string> original = source_lines(source);
    const std::vector<std::string> masked = source_lines(mask_source(
        source, MaskOptions{true, true, true, false, false, false, true, false}));
    struct Scope { int end_line; std::string name; std::string kind; };
    std::vector<Scope> scopes;
    static const std::regex namespace_pattern(
        R"(^\s*namespace\s+([A-Za-z_\\][A-Za-z0-9_\\]*)\s*[;{]\s*$)",
        std::regex::optimize | std::regex::icase);
    static const std::regex type_pattern(
        R"((?:^|\s)(class|interface|trait|enum)\s+([A-Za-z_][A-Za-z0-9_]*))",
        std::regex::optimize | std::regex::icase);
    static const std::regex function_pattern(
        R"(^\s*(?:(?:public|protected|private|static|final|abstract|readonly)\s+)*function\s*&?\s*([A-Za-z_][A-Za-z0-9_]*)\s*\((.*)\)\s*(?::\s*([^\{;]+))?\s*[\{;]\s*$)",
        std::regex::optimize | std::regex::icase);
    static const std::regex property_pattern(
        R"(^\s*(?:(?:public|protected|private|static|readonly|var)\s+)*(?:[?A-Za-z_\\][A-Za-z0-9_\\|?]*\s+)?\$([A-Za-z_][A-Za-z0-9_]*)\b)",
        std::regex::optimize | std::regex::icase);
    static const std::regex constant_pattern(
        R"(^\s*(?:(?:public|protected|private|final)\s+)*const\s+([A-Za-z_][A-Za-z0-9_]*)\b)",
        std::regex::optimize | std::regex::icase);
    for (std::size_t line = 0; line < masked.size(); ++line) {
        while (!scopes.empty() && scopes.back().end_line < static_cast<int>(line + 1)) scopes.pop_back();
        const std::string stripped = trim(masked[line]);
        if (stripped.empty() || stripped.rfind("<?", 0) == 0 || stripped.rfind("?>", 0) == 0 ||
            stripped.front() == '}') continue;
        bool in_function = false;
        bool in_type = false;
        std::vector<std::string> parts;
        for (const Scope& scope : scopes) {
            in_function = in_function || scope.kind == "function";
            in_type = in_type || scope.kind == "type";
            if (scope.kind != "function" && !scope.name.empty()) parts.push_back(scope.name);
        }
        if (in_function) continue;
        const LogicalStatement statement = collect_statement(masked, original, line);
        if (statement.delimiter == '\0') continue;
        const int body_end = statement_end(statement, masked);
        std::smatch match;
        if (std::regex_match(statement.masked, match, namespace_pattern)) {
            const std::string name = match[1].str();
            append_symbol(result, original, "namespace", name, name, statement.original,
                          static_cast<int>(line + 1), body_end,
                          preceding_line_comment(original, line, {"//", "#"}));
            scopes.push_back({statement.delimiter == '{' ? body_end : static_cast<int>(masked.size()),
                              name, "namespace"});
            line = statement.end_line;
            continue;
        }
        if (std::regex_search(statement.masked, match, type_pattern)) {
            const std::string kind = ascii_lower(match[1].str());
            const std::string name = match[2].str();
            append_symbol(result, original, kind, name, join_qualified(parts, name),
                          statement.original, static_cast<int>(line + 1), body_end,
                          preceding_line_comment(original, line, {"//", "#"}));
            if (statement.delimiter == '{') scopes.push_back({body_end, name, "type"});
            line = statement.end_line;
            continue;
        }
        if (std::regex_match(statement.masked, match, function_pattern)) {
            const std::string name = match[1].str();
            append_symbol(result, original, in_type ? "method" : "function", name,
                          join_qualified(parts, name), statement.original,
                          static_cast<int>(line + 1), body_end,
                          preceding_line_comment(original, line, {"//", "#"}), match[2].str(),
                          match[3].matched ? match[3].str() : std::string{});
            if (statement.delimiter == '{') scopes.push_back({body_end, name, "function"});
            line = statement.end_line;
            continue;
        }
        if (std::regex_search(statement.masked, match, constant_pattern)) {
            const std::string name = match[1].str();
            append_symbol(result, original, "constant", name, join_qualified(parts, name),
                          statement.original, static_cast<int>(line + 1),
                          static_cast<int>(statement.end_line + 1),
                          preceding_line_comment(original, line, {"//", "#"}));
        } else if (std::regex_search(statement.masked, match, property_pattern)) {
            const std::string name = match[1].str();
            append_symbol(result, original, in_type ? "field" : "global", name,
                          join_qualified(parts, name), statement.original,
                          static_cast<int>(line + 1), static_cast<int>(statement.end_line + 1),
                          preceding_line_comment(original, line, {"//", "#"}));
        }
        line = statement.end_line;
    }
    return result;
}

ScanResult scan_bash(const std::string& source) {
    ScanResult result;
    result.language = Language::Bash;
    const std::vector<std::string> original = source_lines(source);
    const std::vector<std::string> masked = source_lines(mask_source(
        source, MaskOptions{false, false, true, false, false, false, true, false}));
    struct Scope { int end_line; std::string name; };
    std::vector<Scope> functions;
    static const std::regex function_pattern(
        R"(^\s*(?:function\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*(?:\(\s*\))?\s*\{)",
        std::regex::optimize);
    static const std::regex assignment_pattern(
        R"(^\s*(?:(?:export|readonly|declare|typeset|local)\s+(?:-[A-Za-z]+\s+)*)*([A-Za-z_][A-Za-z0-9_]*)\s*=)",
        std::regex::optimize);
    for (std::size_t line = 0; line < masked.size(); ++line) {
        while (!functions.empty() && functions.back().end_line < static_cast<int>(line + 1))
            functions.pop_back();
        if (trim(masked[line]).empty()) continue;
        std::smatch match;
        if (functions.empty() && std::regex_search(masked[line], match, function_pattern)) {
            const std::string name = match[1].str();
            const std::size_t open = masked[line].find('{', static_cast<std::size_t>(match.position(0)));
            const int end = matching_brace_line(masked, line, open);
            append_symbol(result, original, "function", name, name, original[line],
                          static_cast<int>(line + 1), end,
                          preceding_line_comment(original, line, {"#"}));
            functions.push_back({end, name});
            continue;
        }
        if (functions.empty() && std::regex_search(masked[line], match, assignment_pattern)) {
            const std::string name = match[1].str();
            const bool constant = masked[line].find("readonly") != std::string::npos ||
                                  name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") ==
                                      std::string::npos;
            append_symbol(result, original, constant ? "constant" : "global", name, name,
                          original[line], static_cast<int>(line + 1), static_cast<int>(line + 1),
                          preceding_line_comment(original, line, {"#"}));
        }
    }
    return result;
}

ScanResult scan_perl(const std::string& source) {
    ScanResult result;
    result.language = Language::Perl;
    const std::vector<std::string> original = source_lines(source);
    const std::vector<std::string> masked = source_lines(mask_source(
        source, MaskOptions{false, false, true, false, false, false, true, false}));
    std::string package_name;
    struct Scope { int end_line; };
    std::vector<Scope> functions;
    static const std::regex package_pattern(
        R"(^\s*package\s+([A-Za-z_][A-Za-z0-9_:]*)\s*;)", std::regex::optimize);
    static const std::regex sub_pattern(
        R"(^\s*sub\s+([A-Za-z_][A-Za-z0-9_]*)\b[^\{]*\{)", std::regex::optimize);
    static const std::regex constant_pattern(
        R"(^\s*use\s+constant\s+([A-Za-z_][A-Za-z0-9_]*)\b)", std::regex::optimize);
    static const std::regex global_pattern(
        R"(^\s*our\s+(?:\([^)]*)?[$@%]([A-Za-z_][A-Za-z0-9_]*)\b)", std::regex::optimize);
    for (std::size_t line = 0; line < masked.size(); ++line) {
        while (!functions.empty() && functions.back().end_line < static_cast<int>(line + 1))
            functions.pop_back();
        if (trim(masked[line]).empty()) continue;
        std::smatch match;
        if (functions.empty() && std::regex_search(masked[line], match, package_pattern)) {
            package_name = match[1].str();
            append_symbol(result, original, "package", package_name, package_name, original[line],
                          static_cast<int>(line + 1), static_cast<int>(line + 1),
                          preceding_line_comment(original, line, {"#"}));
            continue;
        }
        if (functions.empty() && std::regex_search(masked[line], match, sub_pattern)) {
            const std::string name = match[1].str();
            const std::size_t open = masked[line].find('{', static_cast<std::size_t>(match.position(0)));
            const int end = matching_brace_line(masked, line, open);
            const std::vector<std::string> parts = package_name.empty()
                                                       ? std::vector<std::string>{}
                                                       : std::vector<std::string>{package_name};
            append_symbol(result, original, "function", name, join_qualified(parts, name),
                          original[line], static_cast<int>(line + 1), end,
                          preceding_line_comment(original, line, {"#"}));
            functions.push_back({end});
            continue;
        }
        if (!functions.empty()) continue;
        const std::vector<std::string> parts = package_name.empty()
                                                   ? std::vector<std::string>{}
                                                   : std::vector<std::string>{package_name};
        if (std::regex_search(masked[line], match, constant_pattern)) {
            const std::string name = match[1].str();
            append_symbol(result, original, "constant", name, join_qualified(parts, name),
                          original[line], static_cast<int>(line + 1), static_cast<int>(line + 1),
                          preceding_line_comment(original, line, {"#"}));
        } else if (std::regex_search(masked[line], match, global_pattern)) {
            const std::string name = match[1].str();
            append_symbol(result, original, "global", name, join_qualified(parts, name),
                          original[line], static_cast<int>(line + 1), static_cast<int>(line + 1),
                          preceding_line_comment(original, line, {"#"}));
        }
    }
    return result;
}

int ruby_block_end(const std::vector<std::string>& masked, std::size_t start) {
    static const std::regex opener(
        R"(^\s*(?:class|module|def|if|unless|case|begin|while|until|for)\b|\bdo\s*(?:\|[^|]*\|)?\s*$)",
        std::regex::optimize);
    static const std::regex closer(R"(^\s*end\b)", std::regex::optimize);
    int depth = 0;
    for (std::size_t line = start; line < masked.size(); ++line) {
        if (std::regex_search(masked[line], opener)) ++depth;
        if (std::regex_search(masked[line], closer) && depth > 0 && --depth == 0)
            return static_cast<int>(line + 1);
    }
    return static_cast<int>(masked.size());
}

ScanResult scan_ruby(const std::string& source) {
    ScanResult result;
    result.language = Language::Ruby;
    const std::vector<std::string> original = source_lines(source);
    const std::vector<std::string> masked = source_lines(mask_source(
        source, MaskOptions{false, false, true, false, false, false, true, false}));
    struct Scope { int end_line; std::string name; std::string kind; };
    std::vector<Scope> scopes;
    static const std::regex type_pattern(
        R"(^\s*(class|module)\s+([A-Za-z_][A-Za-z0-9_:]*))", std::regex::optimize);
    static const std::regex function_pattern(
        R"(^\s*def\s+(?:self\.)?([A-Za-z_][A-Za-z0-9_]*[!?=]?)\s*(?:\((.*)\)|(.*))?$)",
        std::regex::optimize);
    static const std::regex constant_pattern(
        R"(^\s*([A-Z][A-Z0-9_]*)\s*=)", std::regex::optimize);
    for (std::size_t line = 0; line < masked.size(); ++line) {
        while (!scopes.empty() && scopes.back().end_line < static_cast<int>(line + 1)) scopes.pop_back();
        if (trim(masked[line]).empty()) continue;
        bool in_function = false;
        std::vector<std::string> parts;
        for (const Scope& scope : scopes) {
            in_function = in_function || scope.kind == "function";
            if (scope.kind != "function" && !scope.name.empty()) parts.push_back(scope.name);
        }
        if (in_function) continue;
        std::smatch match;
        if (std::regex_search(masked[line], match, type_pattern)) {
            const std::string kind = match[1].str();
            const std::string full_name = match[2].str();
            const std::size_t separator = full_name.rfind("::");
            const std::string name = separator == std::string::npos
                                         ? full_name
                                         : full_name.substr(separator + 2);
            const int end = ruby_block_end(masked, line);
            append_symbol(result, original, kind, name,
                          separator == std::string::npos ? join_qualified(parts, name) : full_name,
                          original[line], static_cast<int>(line + 1), end,
                          preceding_line_comment(original, line, {"#"}));
            scopes.push_back({end, name, "type"});
            continue;
        }
        if (std::regex_match(masked[line], match, function_pattern)) {
            const std::string name = match[1].str();
            const int end = ruby_block_end(masked, line);
            bool method = false;
            for (const Scope& scope : scopes) method = method || scope.kind == "type";
            const std::string parameters = match[2].matched ? match[2].str() : match[3].str();
            append_symbol(result, original, method ? "method" : "function", name,
                          join_qualified(parts, name), original[line], static_cast<int>(line + 1), end,
                          preceding_line_comment(original, line, {"#"}), parameters);
            scopes.push_back({end, name, "function"});
            continue;
        }
        if (std::regex_search(masked[line], match, constant_pattern)) {
            const std::string name = match[1].str();
            append_symbol(result, original, "constant", name, join_qualified(parts, name),
                          original[line], static_cast<int>(line + 1), static_cast<int>(line + 1),
                          preceding_line_comment(original, line, {"#"}));
        }
    }
    return result;
}

ScanResult scan_powershell(const std::string& source) {
    ScanResult result;
    result.language = Language::PowerShell;
    const std::vector<std::string> original = source_lines(source);
    const std::vector<std::string> masked = source_lines(mask_source(
        source, MaskOptions{false, false, true, false, false, true, false, false}));
    struct Scope { int end_line; std::string name; std::string kind; };
    std::vector<Scope> scopes;
    static const std::regex type_pattern(
        R"(^\s*(class|enum)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{)",
        std::regex::optimize | std::regex::icase);
    static const std::regex function_pattern(
        R"(^\s*(function|filter)\s+([A-Za-z_][A-Za-z0-9_-]*)\b[^\{]*\{)",
        std::regex::optimize | std::regex::icase);
    static const std::regex method_pattern(
        R"(^\s*(?:\[[^\]]+\]\s*)?([A-Za-z_][A-Za-z0-9_]*)\s*\((.*)\)\s*\{)",
        std::regex::optimize);
    static const std::regex property_pattern(
        R"(^\s*(?:\[[^\]]+\]\s*)?\$([A-Za-z_][A-Za-z0-9_]*)\b)", std::regex::optimize);
    static const std::regex global_pattern(
        R"(^\s*\$(?:(global|script):)?([A-Za-z_][A-Za-z0-9_]*)\s*=)",
        std::regex::optimize | std::regex::icase);
    for (std::size_t line = 0; line < masked.size(); ++line) {
        while (!scopes.empty() && scopes.back().end_line < static_cast<int>(line + 1)) scopes.pop_back();
        if (trim(masked[line]).empty()) continue;
        bool in_function = false;
        bool in_type = false;
        std::vector<std::string> parts;
        for (const Scope& scope : scopes) {
            in_function = in_function || scope.kind == "function";
            in_type = in_type || scope.kind == "type";
            if (scope.kind != "function" && !scope.name.empty()) parts.push_back(scope.name);
        }
        if (in_function) continue;
        std::smatch match;
        if (std::regex_search(masked[line], match, type_pattern)) {
            const std::string kind = ascii_lower(match[1].str());
            const std::string name = match[2].str();
            const std::size_t open = masked[line].find('{');
            const int end = matching_brace_line(masked, line, open);
            append_symbol(result, original, kind, name, join_qualified(parts, name), original[line],
                          static_cast<int>(line + 1), end,
                          preceding_line_comment(original, line, {"#"}));
            scopes.push_back({end, name, "type"});
            continue;
        }
        if (std::regex_search(masked[line], match, function_pattern)) {
            const std::string name = match[2].str();
            const std::size_t open = masked[line].find('{');
            const int end = matching_brace_line(masked, line, open);
            append_symbol(result, original, "function", name, join_qualified(parts, name),
                          original[line], static_cast<int>(line + 1), end,
                          preceding_line_comment(original, line, {"#"}));
            scopes.push_back({end, name, "function"});
            continue;
        }
        if (in_type && std::regex_search(masked[line], match, method_pattern)) {
            const std::string name = match[1].str();
            const std::size_t open = masked[line].find('{');
            const int end = matching_brace_line(masked, line, open);
            append_symbol(result, original, "method", name, join_qualified(parts, name),
                          original[line], static_cast<int>(line + 1), end,
                          preceding_line_comment(original, line, {"#"}), match[2].str());
            scopes.push_back({end, name, "function"});
            continue;
        }
        if (in_type && std::regex_search(masked[line], match, property_pattern)) {
            const std::string name = match[1].str();
            append_symbol(result, original, "property", name, join_qualified(parts, name),
                          original[line], static_cast<int>(line + 1), static_cast<int>(line + 1),
                          preceding_line_comment(original, line, {"#"}));
        } else if (!in_type && std::regex_search(masked[line], match, global_pattern)) {
            const std::string name = match[2].str();
            append_symbol(result, original, "global", name, name, original[line],
                          static_cast<int>(line + 1), static_cast<int>(line + 1),
                          preceding_line_comment(original, line, {"#"}));
        }
    }
    return result;
}

ScanResult scan_assembly(const std::string& source) {
    ScanResult result;
    result.language = Language::Assembly;
    const std::vector<std::string> original = source_lines(source);
    const std::vector<std::string> masked = source_lines(mask_source(
        source, MaskOptions{true, false, false, false, true, false, false, false}));
    std::set<std::string> functions;
    static const std::regex type_function(
        R"(^\s*\.type\s+([.$A-Za-z_][.$A-Za-z0-9_]*)\s*,\s*[%@]?function\b)",
        std::regex::optimize | std::regex::icase);
    static const std::regex label_pattern(
        R"(^\s*([.$A-Za-z_][.$A-Za-z0-9_]*):)", std::regex::optimize);
    static const std::regex constant_pattern(
        R"(^\s*(?:\.equ|\.set)\s+([.$A-Za-z_][.$A-Za-z0-9_]*)\s*,|^\s*([.$A-Za-z_][.$A-Za-z0-9_]*)\s+equ\b)",
        std::regex::optimize | std::regex::icase);
    for (const std::string& line : masked) {
        std::smatch match;
        if (std::regex_search(line, match, type_function)) functions.insert(match[1].str());
    }
    for (std::size_t line = 0; line < masked.size(); ++line) {
        std::smatch match;
        if (std::regex_search(masked[line], match, label_pattern)) {
            const std::string name = match[1].str();
            if (name.rfind(".L", 0) == 0) continue;
            append_symbol(result, original, functions.count(name) == 0 ? "label" : "function",
                          name, name, original[line], static_cast<int>(line + 1),
                          static_cast<int>(line + 1),
                          preceding_line_comment(original, line, {"#", ";"}));
        } else if (std::regex_search(masked[line], match, constant_pattern)) {
            const std::string name = match[1].matched ? match[1].str() : match[2].str();
            append_symbol(result, original, "constant", name, name, original[line],
                          static_cast<int>(line + 1), static_cast<int>(line + 1),
                          preceding_line_comment(original, line, {"#", ";"}));
        }
    }
    return result;
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

std::size_t markup_tag_end(const std::string& source, std::size_t start) {
    char quote = '\0';
    for (std::size_t pos = start; pos < source.size(); ++pos) {
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

std::string markup_attribute(const std::string& tag, const std::string& wanted) {
    auto attribute_character = [](unsigned char byte) {
        return std::isalnum(byte) || byte == '-' || byte == '_' || byte == ':';
    };
    std::size_t pos = 1;
    while (pos < tag.size() && !std::isspace(static_cast<unsigned char>(tag[pos])) &&
           tag[pos] != '>' && tag[pos] != '/') ++pos;
    while (pos < tag.size()) {
        while (pos < tag.size() &&
               (std::isspace(static_cast<unsigned char>(tag[pos])) || tag[pos] == '/')) ++pos;
        const std::size_t name_begin = pos;
        while (pos < tag.size() && attribute_character(static_cast<unsigned char>(tag[pos]))) ++pos;
        if (pos == name_begin) {
            ++pos;
            continue;
        }
        const std::string name = ascii_lower(tag.substr(name_begin, pos - name_begin));
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
        if (name == wanted) return tag.substr(value_begin, pos - value_begin);
        if (quote != '\0' && pos < tag.size()) ++pos;
    }
    return {};
}

ScanResult scan_xml(const std::string& source) {
    ScanResult result;
    result.language = Language::Xml;
    const std::vector<std::string> original = source_lines(source);
    const SourceLineMap line_map(source);
    static const std::regex opening_tag(
        R"(^<\s*([A-Za-z_][A-Za-z0-9_.:-]*))", std::regex::optimize);
    bool root_recorded = false;
    for (std::size_t pos = 0; pos < source.size();) {
        const std::size_t open = source.find('<', pos);
        if (open == std::string::npos) break;
        if (source.compare(open, 4, "<!--") == 0) {
            const std::size_t close = source.find("-->", open + 4);
            pos = close == std::string::npos ? source.size() : close + 3;
            continue;
        }
        if (source.compare(open, 9, "<![CDATA[") == 0) {
            const std::size_t close = source.find("]]>", open + 9);
            pos = close == std::string::npos ? source.size() : close + 3;
            continue;
        }
        if (open + 1 < source.size() &&
            (source[open + 1] == '/' || source[open + 1] == '!' || source[open + 1] == '?')) {
            const std::size_t end = markup_tag_end(source, open + 1);
            pos = end == std::string::npos ? source.size() : end + 1;
            continue;
        }
        const std::size_t end = markup_tag_end(source, open + 1);
        if (end == std::string::npos) break;
        const std::string tag = source.substr(open, end - open + 1);
        std::smatch match;
        if (!std::regex_search(tag, match, opening_tag)) {
            pos = end + 1;
            continue;
        }
        const std::string tag_name = match[1].str();
        const std::string local_name = tag_name.substr(tag_name.rfind(':') == std::string::npos
                                                           ? 0
                                                           : tag_name.rfind(':') + 1);
        const std::string id = markup_attribute(tag, "id");
        const std::string declared_name = markup_attribute(tag, "name");
        std::string kind;
        std::string name;
        std::string qualified;
        if (!id.empty()) {
            kind = "element";
            name = id;
            qualified = "#" + id;
        } else if (!declared_name.empty() &&
                   (local_name == "element" || local_name == "attribute" ||
                    local_name == "complexType" || local_name == "simpleType" ||
                    local_name == "template" || local_name == "symbol")) {
            kind = "declaration";
            name = declared_name;
            qualified = tag_name + "::" + declared_name;
        } else if (!root_recorded) {
            root_recorded = true;
            kind = "element";
            name = tag_name;
            qualified = tag_name;
        }
        if (!name.empty()) {
            append_symbol(result, original, kind, name, qualified, tag, line_map.line_at(open),
                          line_map.line_at(end));
        }
        pos = end + 1;
    }
    return result;
}

std::string json_key_name(const std::string& source, std::size_t begin, std::size_t end) {
    std::string output;
    for (std::size_t pos = begin; pos < end; ++pos) {
        if (source[pos] == '\\' && pos + 1 < end) {
            const char escaped = source[++pos];
            if (escaped == 'n') output.push_back('\n');
            else if (escaped == 'r') output.push_back('\r');
            else if (escaped == 't') output.push_back('\t');
            else output.push_back(escaped);
        } else {
            output.push_back(source[pos]);
        }
    }
    return output;
}

ScanResult scan_json(const std::string& source) {
    ScanResult result;
    result.language = Language::Json;
    const std::vector<std::string> original = source_lines(source);
    const SourceLineMap line_map(source);
    struct Frame { char kind; std::string component; };
    std::vector<Frame> frames;
    std::string pending_key;
    for (std::size_t pos = 0; pos < source.size();) {
        if (source[pos] == '/' && pos + 1 < source.size() && source[pos + 1] == '/') {
            const std::size_t end = source.find('\n', pos + 2);
            pos = end == std::string::npos ? source.size() : end + 1;
            continue;
        }
        if (source[pos] == '/' && pos + 1 < source.size() && source[pos + 1] == '*') {
            const std::size_t end = source.find("*/", pos + 2);
            pos = end == std::string::npos ? source.size() : end + 2;
            continue;
        }
        if (source[pos] == '"') {
            const std::size_t quote = pos++;
            const std::size_t begin = pos;
            bool escaped = false;
            while (pos < source.size()) {
                if (!escaped && source[pos] == '"') break;
                if (!escaped && source[pos] == '\\') escaped = true;
                else escaped = false;
                ++pos;
            }
            if (pos >= source.size()) break;
            const std::size_t end = pos++;
            std::size_t after = pos;
            while (after < source.size() &&
                   std::isspace(static_cast<unsigned char>(source[after]))) ++after;
            if (after < source.size() && source[after] == ':') {
                const std::string name = json_key_name(source, begin, end);
                std::vector<std::string> parts;
                for (const Frame& frame : frames) {
                    if (!frame.component.empty()) parts.push_back(frame.component);
                }
                append_symbol(result, original, "key", name, join_qualified(parts, name),
                              source.substr(quote, after - quote + 1), line_map.line_at(quote),
                              line_map.line_at(after));
                pending_key = name;
                pos = after + 1;
            }
            continue;
        }
        const char ch = source[pos++];
        if (ch == '{' || ch == '[') {
            frames.push_back({ch, pending_key});
            pending_key.clear();
        } else if (ch == '}' || ch == ']') {
            if (!frames.empty()) frames.pop_back();
            pending_key.clear();
        } else if (ch == ',') {
            pending_key.clear();
        }
    }
    return result;
}

ScanResult scan_sql(const std::string& source) {
    ScanResult result;
    result.language = Language::Sql;
    const std::vector<std::string> original = source_lines(source);
    const std::vector<std::string> masked = source_lines(mask_source(
        source, MaskOptions{false, true, false, true, false, false, false, false}));
    static const std::regex declaration(
        R"(^\s*CREATE\s+(?:OR\s+REPLACE\s+)?(?:TEMP(?:ORARY)?\s+)?(MATERIALIZED\s+VIEW|UNIQUE\s+INDEX|TABLE|VIEW|INDEX|TRIGGER|FUNCTION|PROCEDURE|TYPE|SCHEMA|SEQUENCE)\s+(?:IF\s+NOT\s+EXISTS\s+)?([A-Za-z_][A-Za-z0-9_.$]*))",
        std::regex::optimize | std::regex::icase);
    for (std::size_t line = 0; line < masked.size(); ++line) {
        if (trim(masked[line]).empty()) continue;
        const LogicalStatement statement = collect_statement(masked, original, line);
        std::smatch match;
        if (std::regex_search(statement.masked, match, declaration)) {
            std::string kind = ascii_lower(collapse_space(match[1].str()));
            std::replace(kind.begin(), kind.end(), ' ', '-');
            const std::string full_name = match[2].str();
            const std::size_t separator = full_name.rfind('.');
            const std::string name = separator == std::string::npos
                                         ? full_name
                                         : full_name.substr(separator + 1);
            append_symbol(result, original, kind, name, full_name, statement.original,
                          static_cast<int>(line + 1),
                          statement.delimiter == '\0' ? static_cast<int>(line + 1)
                                                       : static_cast<int>(statement.end_line + 1),
                          preceding_line_comment(original, line, {"--"}));
        }
        if (statement.end_line > line) line = statement.end_line;
    }
    return result;
}

ScanResult scan_toml(const std::string& source) {
    ScanResult result;
    result.language = Language::Toml;
    const std::vector<std::string> original = source_lines(source);
    const std::vector<std::string> masked = source_lines(mask_source(
        source, MaskOptions{false, false, true, false, false, false, false, false}));
    std::string table;
    static const std::regex table_pattern(
        R"(^\s*(\[\[?)([A-Za-z0-9_.-]+)\]\]?\s*$)", std::regex::optimize);
    static const std::regex key_pattern(
        R"(^\s*([A-Za-z0-9_-]+)\s*=)", std::regex::optimize);
    for (std::size_t line = 0; line < masked.size(); ++line) {
        std::smatch match;
        if (std::regex_match(masked[line], match, table_pattern)) {
            table = match[2].str();
            append_symbol(result, original, match[1].str() == "[[" ? "array-table" : "table",
                          table, table, original[line], static_cast<int>(line + 1),
                          static_cast<int>(line + 1),
                          preceding_line_comment(original, line, {"#"}));
        } else if (std::regex_search(masked[line], match, key_pattern)) {
            const std::string name = match[1].str();
            const std::vector<std::string> parts = table.empty()
                                                       ? std::vector<std::string>{}
                                                       : std::vector<std::string>{table};
            append_symbol(result, original, "key", name, join_qualified(parts, name), original[line],
                          static_cast<int>(line + 1), static_cast<int>(line + 1),
                          preceding_line_comment(original, line, {"#"}));
        }
    }
    return result;
}

ScanResult scan_yaml(const std::string& source) {
    ScanResult result;
    result.language = Language::Yaml;
    const std::vector<std::string> original = source_lines(source);
    const std::vector<std::string> masked = source_lines(mask_source(
        source, MaskOptions{false, false, true, false, false, false, false, false}));
    struct Scope { int indent; std::string name; };
    std::vector<Scope> scopes;
    int block_scalar_parent = -1;
    static const std::regex mapping(
        R"(^\s*(?:-\s+)?([A-Za-z_][A-Za-z0-9_.-]*)\s*:\s*(.*)$)", std::regex::optimize);
    for (std::size_t line = 0; line < masked.size(); ++line) {
        if (trim(masked[line]).empty()) continue;
        const int indent = indentation(masked[line]);
        if (block_scalar_parent >= 0 && indent > block_scalar_parent) continue;
        block_scalar_parent = -1;
        while (!scopes.empty() && indent <= scopes.back().indent) scopes.pop_back();
        std::smatch match;
        if (!std::regex_match(masked[line], match, mapping)) continue;
        const std::string name = match[1].str();
        const std::string value = trim(match[2].str());
        std::vector<std::string> parts;
        for (const Scope& scope : scopes) parts.push_back(scope.name);
        const bool container = value.empty();
        int end_line = static_cast<int>(line + 1);
        if (container) {
            end_line = static_cast<int>(masked.size());
            for (std::size_t pos = line + 1; pos < masked.size(); ++pos) {
                if (trim(masked[pos]).empty()) continue;
                if (indentation(masked[pos]) <= indent) {
                    end_line = static_cast<int>(pos);
                    break;
                }
            }
        }
        append_symbol(result, original, container ? "mapping" : "key", name,
                      join_qualified(parts, name), original[line], static_cast<int>(line + 1), end_line,
                      preceding_line_comment(original, line, {"#"}));
        if (container) scopes.push_back({indent, name});
        if (!value.empty() && (value.front() == '|' || value.front() == '>'))
            block_scalar_parent = indent;
    }
    return result;
}

ScanResult scan_ini(const std::string& source) {
    ScanResult result;
    result.language = Language::Ini;
    const std::vector<std::string> original = source_lines(source);
    const std::vector<std::string> masked = source_lines(mask_source(
        source, MaskOptions{false, false, true, false, true, false, false, false}));
    std::string section;
    static const std::regex section_pattern(
        R"(^\s*\[([^\]]+)\]\s*$)", std::regex::optimize);
    static const std::regex key_pattern(
        R"(^\s*([A-Za-z_][A-Za-z0-9_.-]*)\s*[=:])", std::regex::optimize);
    for (std::size_t line = 0; line < masked.size(); ++line) {
        std::smatch match;
        if (std::regex_match(masked[line], match, section_pattern)) {
            section = trim(match[1].str());
            append_symbol(result, original, "section", section, section, original[line],
                          static_cast<int>(line + 1), static_cast<int>(line + 1),
                          preceding_line_comment(original, line, {";", "#"}));
        } else if (std::regex_search(masked[line], match, key_pattern)) {
            const std::string name = match[1].str();
            const std::vector<std::string> parts = section.empty()
                                                       ? std::vector<std::string>{}
                                                       : std::vector<std::string>{section};
            append_symbol(result, original, "key", name, join_qualified(parts, name), original[line],
                          static_cast<int>(line + 1), static_cast<int>(line + 1),
                          preceding_line_comment(original, line, {";", "#"}));
        }
    }
    return result;
}

}  // namespace

ScanResult scan_additional_source(const std::string& source, Language language) {
    switch (language) {
        case Language::Markdown: return scan_markdown(source);
        case Language::CSharp:
        case Language::Java: return scan_managed(source, language);
        case Language::Xml: return scan_xml(source);
        case Language::Json: return scan_json(source);
        case Language::Bash: return scan_bash(source);
        case Language::Php: return scan_php(source);
        case Language::Perl: return scan_perl(source);
        case Language::Ruby: return scan_ruby(source);
        case Language::Rust: return scan_rust(source);
        case Language::Go: return scan_go(source);
        case Language::PowerShell: return scan_powershell(source);
        case Language::Assembly: return scan_assembly(source);
        case Language::Sql: return scan_sql(source);
        case Language::Toml: return scan_toml(source);
        case Language::Yaml: return scan_yaml(source);
        case Language::Ini: return scan_ini(source);
        default: {
            ScanResult result;
            result.language = language;
            return result;
        }
    }
}

}  // namespace ainiux::agent::index
