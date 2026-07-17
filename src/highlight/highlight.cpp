#include "highlight/highlight.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <regex>
#include <unordered_set>

namespace ainiux::highlight {
namespace {

struct Candidate {
    size_t start = 0;
    size_t end = 0;
    TokenRole role = TokenRole::Operator;
};

std::string lower_ascii(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

size_t indentation_end(const std::string& line) {
    size_t pos = 0;
    while (pos < line.size() && pos < 3 && line[pos] == ' ') {
        ++pos;
    }
    return pos;
}

bool only_ascii_space_after(const std::string& line, size_t pos) {
    while (pos < line.size()) {
        if (line[pos] != ' ' && line[pos] != '\t') {
            return false;
        }
        ++pos;
    }
    return true;
}

size_t repeated_run(const std::string& line, size_t pos, char ch) {
    const size_t start = pos;
    while (pos < line.size() && line[pos] == ch) {
        ++pos;
    }
    return pos - start;
}

bool fence_at(const std::string& line, size_t& start, char& character, size_t& length) {
    start = indentation_end(line);
    if (start >= line.size() || (line[start] != '`' && line[start] != '~')) {
        return false;
    }
    character = line[start];
    length = repeated_run(line, start, character);
    return length >= 3;
}

void append_candidate(std::vector<Candidate>& candidates,
                      size_t start,
                      size_t end,
                      TokenRole role) {
    if (end > start) {
        candidates.push_back({start, end, role});
    }
}

void append_regex_candidates(const std::string& line,
                             const std::regex& expression,
                             TokenRole role,
                             std::vector<Candidate>& candidates) {
    for (std::sregex_iterator match(line.begin(), line.end(), expression), end; match != end; ++match) {
        append_candidate(candidates,
                         static_cast<size_t>(match->position()),
                         static_cast<size_t>(match->position() + match->length()),
                         role);
    }
}

bool delimiter_is_escaped(const std::string& line, size_t pos) {
    size_t backslashes = 0;
    while (pos > 0 && line[pos - 1] == '\\') {
        --pos;
        ++backslashes;
    }
    return backslashes % 2 != 0;
}

void add_emphasis(const std::string& line, std::vector<Candidate>& inline_tokens) {
    size_t pos = 0;
    while (pos < line.size()) {
        const char marker = line[pos];
        if ((marker != '*' && marker != '_' && marker != '~') ||
            delimiter_is_escaped(line, pos)) {
            ++pos;
            continue;
        }

        const size_t opening_run = repeated_run(line, pos, marker);
        if (marker == '~' && opening_run < 2) {
            pos += opening_run;
            continue;
        }
        const size_t delimiter_length = marker == '~' ? size_t{2}
                                                       : std::min(opening_run, size_t{3});
        const std::string delimiter(delimiter_length, marker);
        size_t search = pos + delimiter_length;
        bool matched = false;
        while (search < line.size()) {
            const size_t close = line.find(delimiter, search);
            if (close == std::string::npos) {
                break;
            }
            if (close == pos + delimiter_length || delimiter_is_escaped(line, close)) {
                search = close + 1;
                continue;
            }

            size_t close_end = close + delimiter_length;
            // A longer closing run can close nested emphasis (for example
            // **bold *text***). Keep the complete run in the semantic span.
            while (close_end < line.size() && line[close_end] == marker) {
                ++close_end;
            }
            append_candidate(inline_tokens, pos, close_end, TokenRole::Emphasis);
            pos = close_end;
            matched = true;
            break;
        }
        if (!matched) {
            pos += opening_run;
        }
    }
}

void append_destination_candidate(const std::string& line,
                                  size_t start,
                                  size_t end,
                                  std::vector<Candidate>& inline_tokens) {
    while (start < end && (line[start] == ' ' || line[start] == '\t')) {
        ++start;
    }
    while (end > start && (line[end - 1] == ' ' || line[end - 1] == '\t')) {
        --end;
    }
    if (start >= end) {
        return;
    }
    if (line[start] == '<') {
        const size_t close = line.find('>', start + 1);
        if (close != std::string::npos && close < end) {
            append_candidate(inline_tokens, start + 1, close, TokenRole::Attribute);
            return;
        }
    }
    size_t url_end = start;
    while (url_end < end && line[url_end] != ' ' && line[url_end] != '\t') {
        ++url_end;
    }
    append_candidate(inline_tokens, start, url_end, TokenRole::Attribute);
}

void add_markdown_links(const std::string& line, std::vector<Candidate>& inline_tokens) {
    static const std::regex inline_link(
        R"(!?\[[^\]\n]*\]\(([^\)\n]*)\))",
        std::regex::ECMAScript | std::regex::optimize);
    static const std::regex reference_link(
        R"(!?\[[^\]\n]*\]\[[^\]\n]*\])",
        std::regex::ECMAScript | std::regex::optimize);
    static const std::regex reference_definition(
        R"(^ {0,3}\[[^\]\n]+\]:[\t ]*(\S+)(?:[\t ]+.*)?$)",
        std::regex::ECMAScript | std::regex::optimize);
    static const std::regex autolink(
        R"(<(?:https?://|mailto:)[^<>\n]+>)",
        std::regex::ECMAScript | std::regex::icase | std::regex::optimize);

    for (std::sregex_iterator match(line.begin(), line.end(), inline_link), end;
         match != end;
         ++match) {
        const size_t full_start = static_cast<size_t>(match->position());
        const size_t full_end = full_start + static_cast<size_t>(match->length());
        const size_t destination_start = static_cast<size_t>(match->position(1));
        append_destination_candidate(line,
                                     destination_start,
                                     destination_start + static_cast<size_t>(match->length(1)),
                                     inline_tokens);
        append_candidate(inline_tokens, full_start, full_end, TokenRole::Link);
    }
    for (std::sregex_iterator match(line.begin(), line.end(), reference_definition), end;
         match != end;
         ++match) {
        const size_t full_start = static_cast<size_t>(match->position());
        const size_t full_end = full_start + static_cast<size_t>(match->length());
        const size_t destination_start = static_cast<size_t>(match->position(1));
        append_destination_candidate(line,
                                     destination_start,
                                     destination_start + static_cast<size_t>(match->length(1)),
                                     inline_tokens);
        append_candidate(inline_tokens, full_start, full_end, TokenRole::Link);
    }
    for (std::sregex_iterator match(line.begin(), line.end(), autolink), end;
         match != end;
         ++match) {
        const size_t full_start = static_cast<size_t>(match->position());
        const size_t full_end = full_start + static_cast<size_t>(match->length());
        append_candidate(inline_tokens, full_start + 1, full_end - 1, TokenRole::Attribute);
        append_candidate(inline_tokens, full_start, full_end, TokenRole::Link);
    }
    append_regex_candidates(line, reference_link, TokenRole::Link, inline_tokens);
}

void add_html_comments(const std::string& line,
                       size_t start,
                       LineState& next_state,
                       std::vector<Candidate>& high_priority) {
    size_t pos = start;
    if (next_state.block == LineState::Block::HtmlComment) {
        const size_t close = line.find("-->", pos);
        if (close == std::string::npos) {
            append_candidate(high_priority, pos, line.size(), TokenRole::Comment);
            return;
        }
        append_candidate(high_priority, pos, close + 3, TokenRole::Comment);
        next_state.block = LineState::Block::None;
        pos = close + 3;
    }

    while (pos < line.size()) {
        const size_t open = line.find("<!--", pos);
        if (open == std::string::npos) {
            break;
        }
        const size_t close = line.find("-->", open + 4);
        if (close == std::string::npos) {
            append_candidate(high_priority, open, line.size(), TokenRole::Comment);
            next_state.block = LineState::Block::HtmlComment;
            break;
        }
        append_candidate(high_priority, open, close + 3, TokenRole::Comment);
        pos = close + 3;
    }
}

void add_inline_code(const std::string& line, std::vector<Candidate>& high_priority) {
    size_t pos = 0;
    while (pos < line.size()) {
        const size_t open = line.find('`', pos);
        if (open == std::string::npos) {
            break;
        }
        const size_t run = repeated_run(line, open, '`');
        const std::string delimiter(run, '`');
        const size_t close = line.find(delimiter, open + run);
        if (close == std::string::npos) {
            ++pos;
            continue;
        }
        append_candidate(high_priority, open, close + run, TokenRole::String);
        pos = close + run;
    }
}

void add_markdown_structure(const std::string& line, std::vector<Candidate>& structural) {
    const size_t indent = indentation_end(line);
    size_t pos = indent;
    if (pos < line.size() && line[pos] == '#') {
        const size_t count = repeated_run(line, pos, '#');
        if (count <= 6 && (pos + count == line.size() || line[pos + count] == ' ' ||
                           line[pos + count] == '\t')) {
            append_candidate(structural, pos, pos + count, TokenRole::Operator);
            size_t content = pos + count;
            while (content < line.size() && (line[content] == ' ' || line[content] == '\t')) {
                ++content;
            }
            size_t content_end = line.size();
            while (content_end > content &&
                   (line[content_end - 1] == ' ' || line[content_end - 1] == '\t')) {
                --content_end;
            }
            size_t closing = content_end;
            while (closing > content && line[closing - 1] == '#') {
                --closing;
            }
            if (closing < content_end && closing > content && line[closing - 1] == ' ') {
                append_candidate(structural, closing, content_end, TokenRole::Operator);
                content_end = closing - 1;
            }
            append_candidate(structural, content, content_end, TokenRole::Heading);
            return;
        }
    }

    if (pos < line.size() && line[pos] == '>') {
        append_candidate(structural, pos, pos + 1, TokenRole::Operator);
        ++pos;
        if (pos < line.size() && line[pos] == ' ') {
            append_candidate(structural, pos, pos + 1, TokenRole::Operator);
        }
    }

    static const std::regex list_marker(
        R"(^ {0,3}(?:[-+*]|[0-9]{1,9}[.)])[\t ]+)",
        std::regex::ECMAScript | std::regex::optimize);
    static const std::regex thematic_break(
        R"(^ {0,3}(?:(?:\*[\t ]*){3,}|(?:-[\t ]*){3,}|(?:_[\t ]*){3,})$)",
        std::regex::ECMAScript | std::regex::optimize);
    static const std::regex setext_underline(
        R"(^ {0,3}(?:=+|-+)[\t ]*$)",
        std::regex::ECMAScript | std::regex::optimize);
    std::smatch match;
    if (std::regex_match(line, thematic_break) || std::regex_match(line, setext_underline)) {
        append_candidate(structural, indent, line.size(), TokenRole::Operator);
    } else if (std::regex_search(line, match, list_marker)) {
        append_candidate(structural, indent, static_cast<size_t>(match.length()), TokenRole::Operator);
    }
}

void add_inline_markdown(const std::string& line, std::vector<Candidate>& inline_tokens) {
    static const std::regex html_tag(
        R"(</?[A-Za-z][^>\n]*>)",
        std::regex::ECMAScript | std::regex::optimize);
    static const std::regex entity(
        R"(&(?:#[0-9]+|#[xX][0-9A-Fa-f]+|[A-Za-z][A-Za-z0-9]+);)",
        std::regex::ECMAScript | std::regex::optimize);
    static const std::regex escape(R"(\\[!"#$%&'()*+,\-./:;<=>?@\[\]\\^_`{|}~])",
                                   std::regex::ECMAScript | std::regex::optimize);

    add_markdown_links(line, inline_tokens);
    append_regex_candidates(line, html_tag, TokenRole::Tag, inline_tokens);
    add_emphasis(line, inline_tokens);
    append_regex_candidates(line, entity, TokenRole::Literal, inline_tokens);
    append_regex_candidates(line, escape, TokenRole::Operator, inline_tokens);
}

std::vector<Span> resolve_candidates(size_t line_size,
                                     const std::vector<std::vector<Candidate>>& priorities) {
    std::vector<unsigned char> occupied(line_size, 0);
    std::vector<Span> spans;
    for (const std::vector<Candidate>& group : priorities) {
        for (const Candidate& candidate : group) {
            const size_t start = std::min(candidate.start, line_size);
            const size_t end = std::min(candidate.end, line_size);
            size_t pos = start;
            while (pos < end) {
                while (pos < end && occupied[pos] != 0) {
                    ++pos;
                }
                const size_t free_start = pos;
                while (pos < end && occupied[pos] == 0) {
                    occupied[pos] = 1;
                    ++pos;
                }
                if (pos > free_start) {
                    spans.push_back({free_start, pos, candidate.role});
                }
            }
        }
    }
    std::sort(spans.begin(), spans.end(), [](const Span& left, const Span& right) {
        return left.start < right.start || (left.start == right.start && left.end < right.end);
    });
    return spans;
}

bool setext_underline(const std::string& line) {
    static const std::regex expression(
        R"(^ {0,3}(?:=+|-+)[\t ]*$)",
        std::regex::ECMAScript | std::regex::optimize);
    return std::regex_match(line, expression);
}

bool setext_heading_candidate(const std::string& line) {
    const size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos || first > 3) {
        return false;
    }
    return line.rfind("#", first) != first && line.rfind(">", first) != first &&
           !setext_underline(line);
}

bool ascii_identifier_start(char ch) {
    const unsigned char value = static_cast<unsigned char>(ch);
    return std::isalpha(value) != 0 || ch == '_';
}

bool ascii_identifier_part(char ch) {
    const unsigned char value = static_cast<unsigned char>(ch);
    return std::isalnum(value) != 0 || ch == '_';
}

size_t skip_ascii_space(const std::string& line, size_t pos) {
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
        ++pos;
    }
    return pos;
}

size_t find_case_insensitive(const std::string& text, const std::string& needle, size_t start) {
    if (needle.empty() || start > text.size()) {
        return std::string::npos;
    }
    for (size_t pos = start; pos + needle.size() <= text.size(); ++pos) {
        size_t index = 0;
        while (index < needle.size() &&
               std::tolower(static_cast<unsigned char>(text[pos + index])) ==
                   std::tolower(static_cast<unsigned char>(needle[index]))) {
            ++index;
        }
        if (index == needle.size()) {
            return pos;
        }
    }
    return std::string::npos;
}

bool unescaped_at(const std::string& line, size_t pos) {
    return !delimiter_is_escaped(line, pos);
}

const std::unordered_set<std::string>& keywords_for(Language language) {
    static const std::unordered_set<std::string> empty;
    static const std::unordered_set<std::string> python = {
        "and", "as", "assert", "async", "await", "break", "case", "class", "continue",
        "def", "del", "elif", "else", "except", "finally", "for", "from", "global",
        "if", "import", "in", "is", "lambda", "match", "nonlocal", "not", "or", "pass",
        "raise", "return", "try", "while", "with", "yield"};
    static const std::unordered_set<std::string> c = {
        "auto", "break", "case", "const", "continue", "default", "do", "else", "enum",
        "extern", "for", "goto", "if", "inline", "register", "restrict", "return", "sizeof",
        "static", "struct", "switch", "typedef", "union", "volatile", "while", "_Alignas",
        "_Alignof", "_Atomic", "_Generic", "_Noreturn", "_Static_assert", "_Thread_local"};
    static const std::unordered_set<std::string> cpp = {
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "break",
        "case", "catch", "class", "compl", "concept", "const", "consteval", "constexpr",
        "constinit", "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype",
        "default", "delete", "do", "dynamic_cast", "else", "enum", "explicit", "export",
        "extern", "for", "friend", "goto", "if", "inline", "mutable", "namespace", "new",
        "noexcept", "not", "not_eq", "operator", "or", "or_eq", "private", "protected",
        "public", "register", "reinterpret_cast", "requires", "return", "sizeof", "static",
        "static_assert", "static_cast", "struct", "switch", "template", "this", "thread_local",
        "throw", "try", "typedef", "typeid", "typename", "union", "using", "virtual", "volatile",
        "while", "xor", "xor_eq"};
    static const std::unordered_set<std::string> csharp = {
        "abstract", "as", "async", "await", "base", "break", "case", "catch", "checked",
        "class", "const", "continue", "default", "delegate", "do", "else", "enum", "event",
        "explicit", "extern", "finally", "fixed", "for", "foreach", "goto", "if", "implicit",
        "in", "interface", "internal", "is", "lock", "namespace", "new", "operator", "out",
        "override", "params", "private", "protected", "public", "readonly", "record", "ref",
        "return", "sealed", "sizeof", "stackalloc", "static", "struct", "switch", "this",
        "throw", "try", "typeof", "unchecked", "unsafe", "using", "virtual", "volatile", "while",
        "yield", "get", "set", "init", "required", "when", "where", "with"};
    static const std::unordered_set<std::string> java = {
        "abstract", "assert", "break", "case", "catch", "class", "const", "continue", "default",
        "do", "else", "enum", "exports", "extends", "final", "finally", "for", "goto", "if",
        "implements", "import", "instanceof", "interface", "module", "native", "new", "non-sealed",
        "open", "opens", "package", "permits", "private", "protected", "provides", "public",
        "record", "requires", "return", "sealed", "static", "strictfp", "super", "switch",
        "synchronized", "this", "throw", "throws", "to", "transient", "transitive", "try", "uses",
        "var", "volatile", "while", "with", "yield"};
    static const std::unordered_set<std::string> javascript = {
        "async", "await", "break", "case", "catch", "class", "const", "continue", "debugger",
        "default", "delete", "do", "else", "export", "extends", "finally", "for", "from",
        "function", "get", "if", "import", "in", "instanceof", "let", "new", "of", "return",
        "set", "static", "super", "switch", "this", "throw", "try", "typeof", "var", "void",
        "while", "with", "yield"};
    static const std::unordered_set<std::string> typescript = {
        "abstract", "as", "asserts", "async", "await", "break", "case", "catch", "class",
        "const", "constructor", "continue", "debugger", "declare", "default", "delete", "do",
        "else", "enum", "export", "extends", "finally", "for", "from", "function", "get", "if",
        "implements", "import", "in", "infer", "instanceof", "interface", "is", "keyof", "let",
        "module", "namespace", "new", "of", "override", "private", "protected", "public", "readonly",
        "return", "satisfies", "set", "static", "super", "switch", "this", "throw", "try", "type",
        "typeof", "var", "void", "while", "with", "yield"};
    static const std::unordered_set<std::string> bash = {
        "case", "coproc", "do", "done", "elif", "else", "esac", "fi", "for", "function", "if",
        "in", "select", "then", "time", "until", "while", "declare", "export", "local", "readonly",
        "return", "set", "source", "typeset", "unset"};
    static const std::unordered_set<std::string> php = {
        "abstract", "and", "as", "break", "case", "catch", "class", "clone", "const",
        "continue", "declare", "default", "do", "echo", "else", "elseif", "empty", "enddeclare",
        "endfor", "endforeach", "endif", "endswitch", "endwhile", "enum", "eval", "exit",
        "extends", "final", "finally", "fn", "for", "foreach", "from", "function", "global",
        "get", "goto", "if", "implements", "include", "include_once", "instanceof", "insteadof",
        "interface", "isset", "list", "match", "namespace", "new", "or", "print", "private",
        "protected", "public", "readonly", "require", "require_once", "return", "set", "static", "switch",
        "throw", "trait", "try", "unset", "use", "var", "while", "xor", "yield"};
    static const std::unordered_set<std::string> perl = {
        "BEGIN", "END", "UNITCHECK", "CHECK", "INIT", "continue", "do", "else", "elsif", "eval",
        "for", "foreach", "given", "goto", "if", "last", "local", "my", "next", "no", "our",
        "package", "redo", "require", "return", "state", "sub", "unless", "until", "use", "when",
        "while", "say", "print", "map", "grep", "sort", "split", "defined", "delete", "exists"};
    static const std::unordered_set<std::string> ruby = {
        "BEGIN", "END", "alias", "and", "begin", "break", "case", "class", "def", "defined",
        "do", "else", "elsif", "end", "ensure", "false", "for", "if", "in", "module", "next",
        "nil", "not", "or", "redo", "rescue", "retry", "return", "self", "super", "then", "true",
        "undef", "unless", "until", "when", "while", "yield"};
    static const std::unordered_set<std::string> rust = {
        "as", "async", "await", "break", "const", "continue", "crate", "dyn", "else", "enum",
        "extern", "false", "fn", "for", "gen", "if", "impl", "in", "let", "loop", "macro_rules",
        "match", "mod", "move", "mut", "pub", "ref", "return", "self", "Self", "static", "struct",
        "super", "trait", "true", "type", "union", "unsafe", "use", "where", "while", "yield",
        "abstract", "become", "box", "do", "final", "macro", "override", "priv", "try", "typeof",
        "unsized", "virtual"};
    static const std::unordered_set<std::string> go = {
        "break", "case", "chan", "const", "continue", "default", "defer", "else", "fallthrough",
        "for", "func", "go", "goto", "if", "import", "interface", "map", "package", "range",
        "return", "select", "struct", "switch", "type", "var"};
    static const std::unordered_set<std::string> powershell = {
        "begin", "break", "catch", "class", "clean", "continue", "data", "define", "do", "dynamicparam",
        "else", "elseif", "end", "enum", "exit", "filter", "finally", "for", "foreach", "from",
        "function", "if", "in", "param", "process", "return", "switch", "throw", "trap", "try",
        "until", "using", "var", "while", "workflow", "parallel", "sequence", "inlinescript"};
    static const std::unordered_set<std::string> assembly = {
        "adc", "add", "and", "bl", "b", "call", "cmp", "dec", "div", "enter", "hlt", "imul",
        "inc", "int", "ja", "jae", "jb", "jbe", "je", "jg", "jge", "jl", "jle", "jmp", "jne",
        "lea", "leave", "ldr", "mov", "movsx", "movzx", "mul", "neg", "nop", "not", "or", "pop",
        "push", "ret", "rol", "ror", "sar", "sbb", "shl", "shr", "str", "sub", "svc", "syscall",
        "sysret", "test", "xor", "cbz", "cbnz", "beq", "bne", "bgt", "blt", "bge", "ble",
        "global", "extern", "section", "segment", "bits", "default", "db", "dw", "dd", "dq",
        "dt", "do", "dy", "dz", "resb", "resw", "resd", "resq", "equ", "times"};
    static const std::unordered_set<std::string> sql = {
        "add", "all", "alter", "analyze", "and", "any", "as", "asc", "begin", "between", "by",
        "case", "check", "column", "commit", "constraint", "create", "cross", "current", "database",
        "default", "delete", "desc", "distinct", "do", "drop", "else", "end", "except", "exists",
        "explain", "fetch", "filter", "for", "foreign", "from", "full", "function", "generated",
        "grant", "group", "having", "if", "ilike", "in", "index", "inner", "insert", "intersect",
        "into", "is", "join", "lateral", "left", "like", "limit", "materialized", "merge", "natural",
        "not", "nulls", "offset", "on", "or", "order", "outer", "over", "partition", "primary",
        "procedure", "recursive", "references", "returning", "revoke", "right", "rollback", "row",
        "rows", "schema", "select", "set", "table", "then", "trigger", "truncate", "union", "unique",
        "update", "using", "values", "view", "when", "where", "window", "with"};
    switch (language) {
        case Language::Python: return python;
        case Language::C: return c;
        case Language::Cpp: return cpp;
        case Language::CSharp: return csharp;
        case Language::Java: return java;
        case Language::JavaScript: return javascript;
        case Language::TypeScript: return typescript;
        case Language::Bash: return bash;
        case Language::Php: return php;
        case Language::Perl: return perl;
        case Language::Ruby: return ruby;
        case Language::Rust: return rust;
        case Language::Go: return go;
        case Language::PowerShell: return powershell;
        case Language::Assembly: return assembly;
        case Language::Sql: return sql;
        default: return empty;
    }
}

const std::unordered_set<std::string>& types_for(Language language) {
    static const std::unordered_set<std::string> empty;
    static const std::unordered_set<std::string> python = {
        "bool", "bytes", "complex", "dict", "float", "frozenset", "int", "list", "memoryview",
        "object", "range", "set", "str", "tuple", "type", "None"};
    static const std::unordered_set<std::string> c = {
        "bool", "char", "double", "float", "int", "long", "short", "signed", "unsigned", "void",
        "size_t", "ptrdiff_t", "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t",
        "uint32_t", "uint64_t"};
    static const std::unordered_set<std::string> cpp = {
        "bool", "char", "char8_t", "char16_t", "char32_t", "double", "float", "int", "long",
        "short", "signed", "unsigned", "void", "wchar_t", "size_t", "nullptr_t"};
    static const std::unordered_set<std::string> csharp = {
        "bool", "byte", "char", "decimal", "double", "dynamic", "float", "int", "long", "nint",
        "nuint", "object", "sbyte", "short", "string", "uint", "ulong", "ushort", "void"};
    static const std::unordered_set<std::string> java = {
        "boolean", "byte", "char", "double", "float", "int", "long", "short", "void", "String",
        "Object"};
    static const std::unordered_set<std::string> typescript = {
        "any", "bigint", "boolean", "never", "number", "object", "string", "symbol", "unknown",
        "void"};
    static const std::unordered_set<std::string> php = {
        "array", "bool", "callable", "false", "float", "int", "iterable", "mixed", "never", "null",
        "object", "parent", "self", "static", "string", "true", "void"};
    static const std::unordered_set<std::string> perl = {
        "ARRAY", "CODE", "GLOB", "HASH", "IO", "REF", "SCALAR", "Regexp"};
    static const std::unordered_set<std::string> rust = {
        "bool", "char", "f32", "f64", "i8", "i16", "i32", "i64", "i128", "isize", "str", "u8",
        "u16", "u32", "u64", "u128", "usize", "Option", "Result", "String", "Vec"};
    static const std::unordered_set<std::string> go = {
        "any", "bool", "byte", "comparable", "complex64", "complex128", "error", "float32", "float64",
        "int", "int8", "int16", "int32", "int64", "rune", "string", "uint", "uint8", "uint16",
        "uint32", "uint64", "uintptr"};
    static const std::unordered_set<std::string> powershell = {
        "array", "bool", "byte", "char", "datetime", "decimal", "double", "float", "guid", "hashtable",
        "int", "int16", "int32", "int64", "long", "object", "pscustomobject", "regex", "scriptblock",
        "single", "string", "switch", "timespan", "type", "uint", "uint16", "uint32", "uint64", "xml"};
    static const std::unordered_set<std::string> assembly = {
        "al", "ah", "ax", "eax", "rax", "bl", "bh", "bx", "ebx", "rbx", "cl", "ch", "cx",
        "ecx", "rcx", "dl", "dh", "dx", "edx", "rdx", "si", "esi", "rsi", "di", "edi", "rdi",
        "sp", "esp", "rsp", "bp", "ebp", "rbp", "rip", "x0", "x1", "x2", "x3", "x4", "x5",
        "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17",
        "x18", "x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28", "x29",
        "x30", "sp", "w0", "w1", "w2", "w3", "w4", "w5", "w6", "w7"};
    static const std::unordered_set<std::string> sql = {
        "bigint", "binary", "bit", "blob", "boolean", "char", "date", "decimal", "double", "float",
        "int", "integer", "interval", "json", "jsonb", "numeric", "real", "serial", "smallint",
        "text", "time", "timestamp", "uuid", "varchar"};
    switch (language) {
        case Language::Python: return python;
        case Language::C: return c;
        case Language::Cpp: return cpp;
        case Language::CSharp: return csharp;
        case Language::Java: return java;
        case Language::TypeScript: return typescript;
        case Language::Php: return php;
        case Language::Perl: return perl;
        case Language::Rust: return rust;
        case Language::Go: return go;
        case Language::PowerShell: return powershell;
        case Language::Assembly: return assembly;
        case Language::Sql: return sql;
        default: return empty;
    }
}

bool is_literal_word(Language language, const std::string& word) {
    if (language == Language::Python) {
        return word == "True" || word == "False" || word == "None" || word == "NotImplemented";
    }
    if (language == Language::Json) {
        return word == "true" || word == "false" || word == "null" || word == "Infinity" ||
               word == "NaN";
    }
    const std::string lower = lower_ascii(word);
    if (language == Language::Php || language == Language::Sql || language == Language::Toml) {
        return lower == "true" || lower == "false" || lower == "null";
    }
    if (language == Language::Ruby) {
        return word == "true" || word == "false" || word == "nil";
    }
    if (language == Language::Rust) {
        return word == "true" || word == "false";
    }
    if (language == Language::Go) {
        return word == "true" || word == "false" || word == "nil" || word == "iota";
    }
    if (language == Language::Perl) {
        return word == "undef";
    }
    if (language == Language::Yaml) {
        return lower == "true" || lower == "false" || lower == "null" || lower == "yes" ||
               lower == "no" || lower == "on" || lower == "off" || word == "~";
    }
    return word == "true" || word == "false" || word == "null" || word == "nullptr";
}

bool case_insensitive_words(Language language) {
    return language == Language::Php || language == Language::PowerShell ||
           language == Language::Assembly || language == Language::Sql;
}

void add_words_numbers_operators(Language language,
                                 const std::string& line,
                                 std::vector<Candidate>& words,
                                 std::vector<Candidate>& lower) {
    const auto& keywords = keywords_for(language);
    const auto& types = types_for(language);
    size_t pos = 0;
    while (pos < line.size()) {
        if (ascii_identifier_start(line[pos])) {
            const size_t start = pos++;
            while (pos < line.size() && ascii_identifier_part(line[pos])) {
                ++pos;
            }
            const std::string word = line.substr(start, pos - start);
            const std::string lookup = case_insensitive_words(language) ? lower_ascii(word) : word;
            if (is_literal_word(language, word)) {
                append_candidate(words, start, pos, TokenRole::Literal);
            } else if (keywords.find(lookup) != keywords.end()) {
                append_candidate(words, start, pos, TokenRole::Keyword);
            } else if (types.find(lookup) != types.end()) {
                append_candidate(words, start, pos, TokenRole::Type);
            } else {
                const size_t after = skip_ascii_space(line, pos);
                if (after < line.size() &&
                    (line[after] == '(' || (language == Language::Rust && line[after] == '!'))) {
                    append_candidate(lower, start, pos, TokenRole::Function);
                }
            }
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(line[pos])) != 0 ||
            (line[pos] == '.' && pos + 1 < line.size() &&
             std::isdigit(static_cast<unsigned char>(line[pos + 1])) != 0)) {
            const size_t start = pos++;
            while (pos < line.size()) {
                const char ch = line[pos];
                if (std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '.' || ch == '_') {
                    ++pos;
                } else if ((ch == '+' || ch == '-') && pos > start &&
                           (line[pos - 1] == 'e' || line[pos - 1] == 'E' ||
                            line[pos - 1] == 'p' || line[pos - 1] == 'P')) {
                    ++pos;
                } else {
                    break;
                }
            }
            append_candidate(lower, start, pos, TokenRole::Number);
            continue;
        }
        static const std::string operators = "+-*/%=!<>?:&|^~.,;()[]{}";
        if (operators.find(line[pos]) != std::string::npos) {
            append_candidate(lower, pos, pos + 1, TokenRole::Operator);
        }
        ++pos;
    }
}

size_t quoted_end(const std::string& line, size_t start, char quote) {
    size_t pos = start + 1;
    while (pos < line.size()) {
        if (line[pos] == quote && unescaped_at(line, pos)) {
            return pos + 1;
        }
        ++pos;
    }
    return line.size();
}

bool javascript_regex_context(const std::string& line, size_t slash) {
    const size_t previous = slash == 0 ? std::string::npos : line.find_last_not_of(" \t", slash - 1);
    if (previous == std::string::npos ||
        std::string("=([{,:;!&|?").find(line[previous]) != std::string::npos) {
        return true;
    }
    if (!ascii_identifier_part(line[previous])) return false;
    size_t start = previous;
    while (start > 0 && ascii_identifier_part(line[start - 1])) --start;
    const std::string prior = line.substr(start, previous + 1 - start);
    return prior == "return" || prior == "case" || prior == "throw" || prior == "yield" ||
           prior == "await" || prior == "else" || prior == "do";
}

void scan_c_like_lexical(Language language,
                         const std::string& line,
                         LineState& next_state,
                         std::vector<Candidate>& high_priority) {
    const bool line_comments = language != Language::Css;
    const bool backticks = language == Language::JavaScript || language == Language::TypeScript;
    size_t pos = 0;
    if (next_state.block == LineState::Block::BlockComment) {
        const size_t close = line.find("*/");
        if (close == std::string::npos) {
            append_candidate(high_priority, 0, line.size(), TokenRole::Comment);
            return;
        }
        append_candidate(high_priority, 0, close + 2, TokenRole::Comment);
        next_state.block = LineState::Block::None;
        pos = close + 2;
    } else if (next_state.block == LineState::Block::TemplateString) {
        size_t close = pos;
        while ((close = line.find('`', close)) != std::string::npos && !unescaped_at(line, close)) {
            ++close;
        }
        if (close == std::string::npos) {
            append_candidate(high_priority, 0, line.size(), TokenRole::String);
            return;
        }
        append_candidate(high_priority, 0, close + 1, TokenRole::String);
        next_state.block = LineState::Block::None;
        pos = close + 1;
    } else if (next_state.block == LineState::Block::RawString) {
        const std::string close_marker = ")" + next_state.delimiter + "\"";
        const size_t close = line.find(close_marker);
        if (close == std::string::npos) {
            append_candidate(high_priority, 0, line.size(), TokenRole::String);
            return;
        }
        append_candidate(high_priority, 0, close + close_marker.size(), TokenRole::String);
        next_state.block = LineState::Block::None;
        next_state.delimiter.clear();
        pos = close + close_marker.size();
    }

    while (pos < line.size()) {
        if (pos + 1 < line.size() && line[pos] == '/' && line[pos + 1] == '*') {
            const size_t close = line.find("*/", pos + 2);
            if (close == std::string::npos) {
                append_candidate(high_priority, pos, line.size(), TokenRole::Comment);
                next_state.block = LineState::Block::BlockComment;
                return;
            }
            append_candidate(high_priority, pos, close + 2, TokenRole::Comment);
            pos = close + 2;
            continue;
        }
        if (line_comments && pos + 1 < line.size() && line[pos] == '/' && line[pos + 1] == '/') {
            append_candidate(high_priority, pos, line.size(), TokenRole::Comment);
            return;
        }
        if (language == Language::Cpp && pos + 2 < line.size() && line[pos] == 'R' &&
            line[pos + 1] == '"') {
            const size_t open = line.find('(', pos + 2);
            if (open != std::string::npos && open - (pos + 2) <= 16) {
                const std::string delimiter = line.substr(pos + 2, open - (pos + 2));
                const std::string close_marker = ")" + delimiter + "\"";
                const size_t close = line.find(close_marker, open + 1);
                if (close == std::string::npos) {
                    append_candidate(high_priority, pos, line.size(), TokenRole::String);
                    next_state.block = LineState::Block::RawString;
                    next_state.delimiter = delimiter;
                    return;
                }
                append_candidate(high_priority, pos, close + close_marker.size(), TokenRole::String);
                pos = close + close_marker.size();
                continue;
            }
        }
        if (line[pos] == '"' || line[pos] == '\'') {
            const size_t end = quoted_end(line, pos, line[pos]);
            append_candidate(high_priority, pos, end, TokenRole::String);
            pos = end;
            continue;
        }
        if (backticks && line[pos] == '`') {
            size_t close = pos + 1;
            while ((close = line.find('`', close)) != std::string::npos && !unescaped_at(line, close)) {
                ++close;
            }
            if (close == std::string::npos) {
                append_candidate(high_priority, pos, line.size(), TokenRole::String);
                next_state.block = LineState::Block::TemplateString;
                return;
            }
            append_candidate(high_priority, pos, close + 1, TokenRole::String);
            pos = close + 1;
            continue;
        }
        if ((language == Language::JavaScript || language == Language::TypeScript) &&
            line[pos] == '/' && javascript_regex_context(line, pos)) {
            size_t close = pos + 1;
            bool bracket = false;
            bool matched = false;
            while (close < line.size()) {
                if (line[close] == '[' && unescaped_at(line, close)) bracket = true;
                if (line[close] == ']' && unescaped_at(line, close)) bracket = false;
                if (line[close] == '/' && !bracket && unescaped_at(line, close)) {
                    ++close;
                    while (close < line.size() && std::isalpha(static_cast<unsigned char>(line[close]))) {
                        ++close;
                    }
                    append_candidate(high_priority, pos, close, TokenRole::String);
                    pos = close;
                    matched = true;
                    break;
                }
                ++close;
            }
            if (!matched) {
                ++pos;
            }
            continue;
        }
        ++pos;
    }
}

void scan_python_lexical(const std::string& line,
                         LineState& next_state,
                         std::vector<Candidate>& high_priority) {
    size_t pos = 0;
    if (next_state.block == LineState::Block::TripleSingleString ||
        next_state.block == LineState::Block::TripleDoubleString) {
        const std::string delimiter = next_state.block == LineState::Block::TripleSingleString
                                          ? "'''"
                                          : "\"\"\"";
        const size_t close = line.find(delimiter);
        if (close == std::string::npos) {
            append_candidate(high_priority, 0, line.size(), TokenRole::String);
            return;
        }
        append_candidate(high_priority, 0, close + 3, TokenRole::String);
        next_state.block = LineState::Block::None;
        pos = close + 3;
    }
    while (pos < line.size()) {
        if (line[pos] == '#') {
            append_candidate(high_priority, pos, line.size(), TokenRole::Comment);
            return;
        }
        if (line.compare(pos, 3, "'''") == 0 || line.compare(pos, 3, "\"\"\"") == 0) {
            const std::string delimiter = line.substr(pos, 3);
            const size_t close = line.find(delimiter, pos + 3);
            if (close == std::string::npos) {
                append_candidate(high_priority, pos, line.size(), TokenRole::String);
                next_state.block = delimiter[0] == '\'' ? LineState::Block::TripleSingleString
                                                         : LineState::Block::TripleDoubleString;
                return;
            }
            append_candidate(high_priority, pos, close + 3, TokenRole::String);
            pos = close + 3;
            continue;
        }
        if (line[pos] == '\'' || line[pos] == '"') {
            const size_t end = quoted_end(line, pos, line[pos]);
            append_candidate(high_priority, pos, end, TokenRole::String);
            pos = end;
            continue;
        }
        ++pos;
    }
}

void scan_json_lexical(const std::string& line,
                       LineState& next_state,
                       std::vector<Candidate>& high_priority,
                       std::vector<Candidate>& structural) {
    size_t pos = 0;
    if (next_state.block == LineState::Block::BlockComment) {
        const size_t close = line.find("*/");
        if (close == std::string::npos) {
            append_candidate(high_priority, 0, line.size(), TokenRole::Comment);
            return;
        }
        append_candidate(high_priority, 0, close + 2, TokenRole::Comment);
        next_state.block = LineState::Block::None;
        pos = close + 2;
    }
    while (pos < line.size()) {
        if (pos + 1 < line.size() && line[pos] == '/' && line[pos + 1] == '*') {
            const size_t close = line.find("*/", pos + 2);
            if (close == std::string::npos) {
                append_candidate(high_priority, pos, line.size(), TokenRole::Comment);
                next_state.block = LineState::Block::BlockComment;
                return;
            }
            append_candidate(high_priority, pos, close + 2, TokenRole::Comment);
            pos = close + 2;
            continue;
        }
        if (pos + 1 < line.size() && line[pos] == '/' && line[pos + 1] == '/') {
            append_candidate(high_priority, pos, line.size(), TokenRole::Comment);
            return;
        }
        if (line[pos] != '"' && line[pos] != '\'') {
            ++pos;
            continue;
        }
        const size_t end = quoted_end(line, pos, line[pos]);
        const size_t after = skip_ascii_space(line, end);
        append_candidate(high_priority,
                         pos,
                         end,
                         after < line.size() && line[after] == ':' ? TokenRole::Property
                                                                  : TokenRole::String);
        pos = end;
    }
    (void)structural;
}

void scan_bash_lexical(const std::string& line,
                       LineState& next_state,
                       std::vector<Candidate>& high_priority,
                       std::vector<Candidate>& structural,
                       std::vector<Candidate>& lower) {
    if (next_state.block == LineState::Block::Heredoc) {
        size_t compare = 0;
        if (next_state.strip_tabs) {
            while (compare < line.size() && line[compare] == '\t') ++compare;
        }
        if (line.compare(compare, next_state.delimiter.size(), next_state.delimiter) == 0 &&
            only_ascii_space_after(line, compare + next_state.delimiter.size())) {
            append_candidate(structural, compare, compare + next_state.delimiter.size(),
                             TokenRole::Preprocessor);
            next_state = {};
        } else {
            append_candidate(high_priority, 0, line.size(), TokenRole::String);
        }
        return;
    }
    size_t pos = 0;
    while (pos < line.size()) {
        if (line[pos] == '#') {
            append_candidate(high_priority, pos, line.size(), TokenRole::Comment);
            break;
        }
        if (line[pos] == '\'' || line[pos] == '"') {
            const size_t end = quoted_end(line, pos, line[pos]);
            append_candidate(high_priority, pos, end, TokenRole::String);
            pos = end;
            continue;
        }
        if (line[pos] == '$') {
            size_t end = pos + 1;
            if (end < line.size() && line[end] == '{') {
                const size_t close = line.find('}', end + 1);
                end = close == std::string::npos ? line.size() : close + 1;
            } else {
                if (end < line.size() && std::string("@*#?$!-0123456789").find(line[end]) !=
                                             std::string::npos) {
                    ++end;
                } else {
                    while (end < line.size() && ascii_identifier_part(line[end])) ++end;
                }
            }
            append_candidate(lower, pos, end, TokenRole::Variable);
            pos = end;
            continue;
        }
        ++pos;
    }
    static const std::regex heredoc(
        R"(<<(-)?[\t ]*(['"]?)([A-Za-z_][A-Za-z0-9_]*)\2)",
        std::regex::ECMAScript | std::regex::optimize);
    std::smatch match;
    if (std::regex_search(line, match, heredoc)) {
        const size_t heredoc_start = static_cast<size_t>(match.position());
        const bool hidden = std::any_of(high_priority.begin(), high_priority.end(),
                                        [&](const Candidate& candidate) {
                                            return candidate.start <= heredoc_start &&
                                                   heredoc_start < candidate.end;
                                        });
        if (hidden) return;
        append_candidate(structural,
                         heredoc_start,
                         static_cast<size_t>(match.position() + match.length()),
                         TokenRole::Preprocessor);
        next_state.block = LineState::Block::Heredoc;
        next_state.delimiter = match.str(3);
        next_state.strip_tabs = match[1].matched;
    }
}

size_t identifier_end(const std::string& line, size_t pos) {
    while (pos < line.size() && ascii_identifier_part(line[pos])) ++pos;
    return pos;
}

bool heredoc_opener(const std::string& line,
                    size_t marker,
                    size_t marker_length,
                    bool modifiers,
                    std::string& delimiter,
                    size_t& end) {
    size_t pos = marker + marker_length;
    if (modifiers) {
        while (pos < line.size() && (line[pos] == '-' || line[pos] == '~')) ++pos;
    }
    pos = skip_ascii_space(line, pos);
    char quote = 0;
    if (pos < line.size() && (line[pos] == '\'' || line[pos] == '"' || line[pos] == '`')) {
        quote = line[pos++];
    }
    if (pos >= line.size() || !ascii_identifier_start(line[pos])) return false;
    const size_t start = pos;
    pos = identifier_end(line, pos + 1);
    delimiter = line.substr(start, pos - start);
    if (quote != 0) {
        if (pos >= line.size() || line[pos] != quote) return false;
        ++pos;
    }
    end = pos;
    return true;
}

void add_sigil_variable(const std::string& line,
                        size_t& pos,
                        const std::string& sigils,
                        std::vector<Candidate>& lower) {
    if (sigils.find(line[pos]) == std::string::npos) return;
    const size_t start = pos++;
    while (pos < line.size() && sigils.find(line[pos]) != std::string::npos) ++pos;
    if (pos < line.size() && line[pos] == '{') {
        const size_t close = line.find('}', pos + 1);
        pos = close == std::string::npos ? line.size() : close + 1;
    } else if (pos < line.size() &&
               (std::isdigit(static_cast<unsigned char>(line[pos])) != 0 ||
                std::string("!/?_^|&`'+-.:").find(line[pos]) != std::string::npos)) {
        ++pos;
    } else {
        pos = identifier_end(line, pos);
    }
    if (pos > start + 1) append_candidate(lower, start, pos, TokenRole::Variable);
}

void scan_php_lexical(const std::string& line,
                      LineState& next_state,
                      std::vector<Candidate>& high_priority,
                      std::vector<Candidate>& structural,
                      std::vector<Candidate>& lower) {
    if (next_state.block == LineState::Block::Heredoc) {
        const size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos &&
            line.compare(first, next_state.delimiter.size(), next_state.delimiter) == 0 &&
            (first + next_state.delimiter.size() == line.size() ||
             line[first + next_state.delimiter.size()] == ';')) {
            append_candidate(structural, first, first + next_state.delimiter.size(),
                             TokenRole::Preprocessor);
            next_state = {};
        } else {
            append_candidate(high_priority, 0, line.size(), TokenRole::String);
        }
        return;
    }
    size_t pos = 0;
    if (next_state.block == LineState::Block::BlockComment) {
        const size_t close = line.find("*/");
        if (close == std::string::npos) {
            append_candidate(high_priority, 0, line.size(), TokenRole::Comment);
            return;
        }
        append_candidate(high_priority, 0, close + 2, TokenRole::Comment);
        next_state = {};
        pos = close + 2;
    }
    while (pos < line.size()) {
        if (line.compare(pos, 5, "<?php") == 0) {
            append_candidate(structural, pos, pos + 5, TokenRole::Preprocessor);
            pos += 5;
            continue;
        }
        if (line.compare(pos, 3, "<?=") == 0) {
            append_candidate(structural, pos, pos + 3, TokenRole::Preprocessor);
            pos += 3;
            continue;
        }
        if (line.compare(pos, 2, "?>") == 0) {
            append_candidate(structural, pos, pos + 2, TokenRole::Preprocessor);
            pos += 2;
            continue;
        }
        if (line.compare(pos, 2, "/*") == 0) {
            const size_t close = line.find("*/", pos + 2);
            if (close == std::string::npos) {
                append_candidate(high_priority, pos, line.size(), TokenRole::Comment);
                next_state.block = LineState::Block::BlockComment;
                return;
            }
            append_candidate(high_priority, pos, close + 2, TokenRole::Comment);
            pos = close + 2;
            continue;
        }
        if (line.compare(pos, 2, "#[") == 0) {
            append_candidate(structural, pos, pos + 2, TokenRole::Preprocessor);
            const size_t close = line.find(']', pos + 2);
            if (close != std::string::npos) {
                append_candidate(structural, close, close + 1, TokenRole::Preprocessor);
            }
            pos += 2;
            continue;
        }
        if (line.compare(pos, 2, "//") == 0 ||
            (line[pos] == '#' && line.compare(pos, 2, "#[") != 0)) {
            append_candidate(high_priority, pos, line.size(), TokenRole::Comment);
            return;
        }
        if (line.compare(pos, 3, "<<<") == 0) {
            std::string delimiter;
            size_t end = pos;
            if (heredoc_opener(line, pos, 3, false, delimiter, end)) {
                append_candidate(structural, pos, end, TokenRole::Preprocessor);
                next_state.block = LineState::Block::Heredoc;
                next_state.delimiter = delimiter;
                pos = end;
                continue;
            }
        }
        if (line[pos] == '\'' || line[pos] == '"' || line[pos] == '`') {
            const size_t end = quoted_end(line, pos, line[pos]);
            append_candidate(high_priority, pos, end, TokenRole::String);
            pos = end;
            continue;
        }
        if (line[pos] == '$') {
            add_sigil_variable(line, pos, "$", lower);
            continue;
        }
        ++pos;
    }
}

void scan_perl_ruby_lexical(Language language,
                            const std::string& line,
                            LineState& next_state,
                            std::vector<Candidate>& high_priority,
                            std::vector<Candidate>& structural,
                            std::vector<Candidate>& lower) {
    if (next_state.block == LineState::Block::LineDelimitedComment) {
        const size_t first = line.find_first_not_of(" \t");
        append_candidate(high_priority, 0, line.size(), TokenRole::Comment);
        if (first != std::string::npos &&
            line.compare(first, next_state.delimiter.size(), next_state.delimiter) == 0) {
            next_state = {};
        }
        return;
    }
    if (next_state.block == LineState::Block::Heredoc) {
        size_t compare = 0;
        if (next_state.strip_tabs) {
            while (compare < line.size() && (line[compare] == ' ' || line[compare] == '\t')) ++compare;
        }
        if (line.compare(compare, next_state.delimiter.size(), next_state.delimiter) == 0 &&
            only_ascii_space_after(line, compare + next_state.delimiter.size())) {
            append_candidate(structural, compare, compare + next_state.delimiter.size(),
                             TokenRole::Preprocessor);
            next_state = {};
        } else {
            append_candidate(high_priority, 0, line.size(), TokenRole::String);
        }
        return;
    }
    const size_t first = line.find_first_not_of(" \t");
    if (language == Language::Perl && first != std::string::npos && line[first] == '=' &&
        (line.compare(first, 4, "=pod") == 0 || line.compare(first, 6, "=head1") == 0 ||
         line.compare(first, 6, "=begin") == 0)) {
        append_candidate(high_priority, first, line.size(), TokenRole::Comment);
        next_state.block = LineState::Block::LineDelimitedComment;
        next_state.delimiter = "=cut";
        return;
    }
    if (language == Language::Ruby && first != std::string::npos &&
        line.compare(first, 6, "=begin") == 0) {
        append_candidate(high_priority, first, line.size(), TokenRole::Comment);
        next_state.block = LineState::Block::LineDelimitedComment;
        next_state.delimiter = "=end";
        return;
    }
    size_t pos = 0;
    while (pos < line.size()) {
        if (line[pos] == '#') {
            append_candidate(high_priority, pos, line.size(), TokenRole::Comment);
            return;
        }
        if (line[pos] == '\'' || line[pos] == '"' || line[pos] == '`') {
            const size_t end = quoted_end(line, pos, line[pos]);
            append_candidate(high_priority, pos, end, TokenRole::String);
            pos = end;
            continue;
        }
        if (language == Language::Perl &&
            (line.compare(pos, 2, "m/") == 0 || line.compare(pos, 3, "qr/") == 0 ||
             line.compare(pos, 2, "s/") == 0)) {
            const bool substitution = line.compare(pos, 2, "s/") == 0;
            const size_t slash = line.find('/', pos);
            size_t end = slash + 1;
            const size_t parts = substitution ? 2 : 1;
            bool complete = true;
            for (size_t part = 0; part < parts; ++part) {
                while (end < line.size() && (line[end] != '/' || !unescaped_at(line, end))) ++end;
                if (end >= line.size()) {
                    complete = false;
                    break;
                }
                ++end;
            }
            if (complete) {
                while (end < line.size() && std::isalpha(static_cast<unsigned char>(line[end])) != 0) ++end;
                append_candidate(high_priority, pos, end, TokenRole::String);
                pos = end;
                continue;
            }
        }
        if (language == Language::Ruby && line.compare(pos, 3, "%r{") == 0) {
            size_t end = pos + 3;
            while (end < line.size() && (line[end] != '}' || !unescaped_at(line, end))) ++end;
            if (end < line.size()) {
                ++end;
                while (end < line.size() && std::isalpha(static_cast<unsigned char>(line[end])) != 0) ++end;
                append_candidate(high_priority, pos, end, TokenRole::String);
                pos = end;
                continue;
            }
        }
        if (language == Language::Ruby && line[pos] == '/') {
            const size_t previous = pos == 0 ? std::string::npos : line.find_last_not_of(" \t", pos - 1);
            if (previous == std::string::npos ||
                std::string("=~([{,:;!&|?").find(line[previous]) != std::string::npos) {
                size_t end = pos + 1;
                while (end < line.size() && (line[end] != '/' || !unescaped_at(line, end))) ++end;
                if (end < line.size()) {
                    ++end;
                    while (end < line.size() && std::isalpha(static_cast<unsigned char>(line[end])) != 0) ++end;
                    append_candidate(high_priority, pos, end, TokenRole::String);
                    pos = end;
                    continue;
                }
            }
        }
        if (line.compare(pos, 2, "<<") == 0) {
            std::string delimiter;
            size_t end = pos;
            if (heredoc_opener(line, pos, 2, true, delimiter, end)) {
                append_candidate(structural, pos, end, TokenRole::Preprocessor);
                next_state.block = LineState::Block::Heredoc;
                next_state.delimiter = delimiter;
                next_state.strip_tabs = line.find('-', pos + 2) == pos + 2 ||
                                        line.find('~', pos + 2) == pos + 2;
                pos = end;
                continue;
            }
        }
        if ((language == Language::Perl && std::string("$@%").find(line[pos]) != std::string::npos) ||
            (language == Language::Ruby && (line[pos] == '$' || line[pos] == '@'))) {
            add_sigil_variable(line, pos, language == Language::Perl ? "$@%" : "$@", lower);
            continue;
        }
        if (language == Language::Ruby && line[pos] == ':' && pos + 1 < line.size() &&
            ascii_identifier_start(line[pos + 1])) {
            const size_t end = identifier_end(line, pos + 2);
            append_candidate(lower, pos, end, TokenRole::Literal);
            pos = end;
            continue;
        }
        ++pos;
    }
}

size_t scan_nested_comment(const std::string& line,
                           size_t pos,
                           size_t& depth,
                           std::vector<Candidate>& high_priority) {
    const size_t start = pos;
    while (pos < line.size()) {
        if (line.compare(pos, 2, "/*") == 0) {
            ++depth;
            pos += 2;
        } else if (line.compare(pos, 2, "*/") == 0) {
            pos += 2;
            if (--depth == 0) {
                append_candidate(high_priority, start, pos, TokenRole::Comment);
                return pos;
            }
        } else {
            ++pos;
        }
    }
    append_candidate(high_priority, start, line.size(), TokenRole::Comment);
    return line.size();
}

void scan_rust_go_lexical(Language language,
                          const std::string& line,
                          LineState& next_state,
                          std::vector<Candidate>& high_priority) {
    size_t pos = 0;
    if (next_state.block == LineState::Block::BlockComment) {
        size_t depth = std::max(next_state.block_depth, size_t{1});
        pos = language == Language::Rust
                  ? scan_nested_comment(line, 0, depth, high_priority)
                  : (line.find("*/") == std::string::npos ? line.size() : line.find("*/") + 2);
        if (language == Language::Go) {
            if (pos == line.size() && line.find("*/") == std::string::npos) {
                append_candidate(high_priority, 0, line.size(), TokenRole::Comment);
                return;
            }
            append_candidate(high_priority, 0, pos, TokenRole::Comment);
            next_state = {};
        } else if (depth != 0) {
            next_state.block_depth = depth;
            return;
        } else {
            next_state = {};
        }
    } else if (next_state.block == LineState::Block::RawString ||
               next_state.block == LineState::Block::HereString) {
        const size_t close = line.find(next_state.delimiter);
        if (close == std::string::npos) {
            append_candidate(high_priority, 0, line.size(), TokenRole::String);
            return;
        }
        append_candidate(high_priority, 0, close + next_state.delimiter.size(), TokenRole::String);
        pos = close + next_state.delimiter.size();
        next_state = {};
    }
    while (pos < line.size()) {
        if (line.compare(pos, 2, "//") == 0) {
            append_candidate(high_priority, pos, line.size(), TokenRole::Comment);
            return;
        }
        if (line.compare(pos, 2, "/*") == 0) {
            if (language == Language::Rust) {
                size_t depth = 0;
                const size_t end = scan_nested_comment(line, pos, depth, high_priority);
                if (depth != 0) {
                    next_state.block = LineState::Block::BlockComment;
                    next_state.block_depth = depth;
                    return;
                }
                pos = end;
            } else {
                const size_t close = line.find("*/", pos + 2);
                if (close == std::string::npos) {
                    append_candidate(high_priority, pos, line.size(), TokenRole::Comment);
                    next_state.block = LineState::Block::BlockComment;
                    return;
                }
                append_candidate(high_priority, pos, close + 2, TokenRole::Comment);
                pos = close + 2;
            }
            continue;
        }
        if (language == Language::Rust && line[pos] == 'r') {
            size_t marker = pos + 1;
            while (marker < line.size() && line[marker] == '#') ++marker;
            if (marker < line.size() && line[marker] == '"') {
                const std::string close = "\"" + std::string(marker - pos - 1, '#');
                const size_t end = line.find(close, marker + 1);
                if (end == std::string::npos) {
                    append_candidate(high_priority, pos, line.size(), TokenRole::String);
                    next_state.block = LineState::Block::RawString;
                    next_state.delimiter = close;
                    return;
                }
                append_candidate(high_priority, pos, end + close.size(), TokenRole::String);
                pos = end + close.size();
                continue;
            }
        }
        if (language == Language::Go && line[pos] == '`') {
            const size_t close = line.find('`', pos + 1);
            if (close == std::string::npos) {
                append_candidate(high_priority, pos, line.size(), TokenRole::String);
                next_state.block = LineState::Block::HereString;
                next_state.delimiter = "`";
                return;
            }
            append_candidate(high_priority, pos, close + 1, TokenRole::String);
            pos = close + 1;
            continue;
        }
        if (line[pos] == '"' || (line[pos] == '\'' && language == Language::Go)) {
            const size_t end = quoted_end(line, pos, line[pos]);
            append_candidate(high_priority, pos, end, TokenRole::String);
            pos = end;
            continue;
        }
        if (language == Language::Rust && line[pos] == '\'' && pos + 2 < line.size()) {
            const size_t close = line.find('\'', pos + 1);
            if (close != std::string::npos && close - pos <= 6) {
                append_candidate(high_priority, pos, close + 1, TokenRole::String);
                pos = close + 1;
                continue;
            }
        }
        ++pos;
    }
}

void scan_powershell_lexical(const std::string& line,
                             LineState& next_state,
                             std::vector<Candidate>& high_priority,
                             std::vector<Candidate>& structural,
                             std::vector<Candidate>& lower) {
    if (next_state.block == LineState::Block::HereString) {
        if (line.compare(0, next_state.delimiter.size(), next_state.delimiter) == 0) {
            append_candidate(structural, 0, next_state.delimiter.size(), TokenRole::Preprocessor);
            next_state = {};
        } else {
            append_candidate(high_priority, 0, line.size(), TokenRole::String);
        }
        return;
    }
    size_t pos = 0;
    if (next_state.block == LineState::Block::BlockComment) {
        const size_t close = line.find("#>");
        if (close == std::string::npos) {
            append_candidate(high_priority, 0, line.size(), TokenRole::Comment);
            return;
        }
        append_candidate(high_priority, 0, close + 2, TokenRole::Comment);
        next_state = {};
        pos = close + 2;
    }
    while (pos < line.size()) {
        if (line.compare(pos, 2, "<#") == 0) {
            const size_t close = line.find("#>", pos + 2);
            if (close == std::string::npos) {
                append_candidate(high_priority, pos, line.size(), TokenRole::Comment);
                next_state.block = LineState::Block::BlockComment;
                return;
            }
            append_candidate(high_priority, pos, close + 2, TokenRole::Comment);
            pos = close + 2;
            continue;
        }
        if (line[pos] == '#') {
            append_candidate(high_priority, pos, line.size(), TokenRole::Comment);
            break;
        }
        if (line.compare(pos, 2, "@\"") == 0 || line.compare(pos, 2, "@'") == 0) {
            append_candidate(structural, pos, pos + 2, TokenRole::Preprocessor);
            next_state.block = LineState::Block::HereString;
            next_state.delimiter = line[pos + 1] == '"' ? "\"@" : "'@";
            pos += 2;
            continue;
        }
        if (line[pos] == '[') {
            const size_t close = line.find(']', pos + 1);
            if (close != std::string::npos && close > pos + 1) {
                append_candidate(structural, pos + 1, close, TokenRole::Type);
            }
        }
        if (line[pos] == '\'' || line[pos] == '"') {
            const size_t end = quoted_end(line, pos, line[pos]);
            append_candidate(high_priority, pos, end, TokenRole::String);
            pos = end;
            continue;
        }
        if (line[pos] == '$') {
            const size_t start = pos++;
            if (pos < line.size() && line[pos] == '{') {
                const size_t close = line.find('}', pos + 1);
                pos = close == std::string::npos ? line.size() : close + 1;
            } else {
                while (pos < line.size() && (ascii_identifier_part(line[pos]) || line[pos] == ':' ||
                                             line[pos] == '?')) ++pos;
            }
            const std::string variable = lower_ascii(line.substr(start, pos - start));
            append_candidate(lower, start, pos,
                             variable == "$true" || variable == "$false" || variable == "$null"
                                 ? TokenRole::Literal
                                 : TokenRole::Variable);
            continue;
        }
        if (line[pos] == '-' && pos + 1 < line.size() && ascii_identifier_start(line[pos + 1])) {
            const size_t end = identifier_end(line, pos + 2);
            append_candidate(lower, pos, end, TokenRole::Operator);
            pos = end;
            continue;
        }
        ++pos;
    }
    pos = 0;
    while (pos < line.size()) {
        if (!ascii_identifier_start(line[pos])) {
            ++pos;
            continue;
        }
        const size_t start = pos;
        pos = identifier_end(line, pos + 1);
        if (pos < line.size() && line[pos] == '-' && pos + 1 < line.size() &&
            ascii_identifier_start(line[pos + 1])) {
            ++pos;
            pos = identifier_end(line, pos + 1);
            append_candidate(lower, start, pos, TokenRole::Function);
        }
    }
}

size_t sql_quoted_end(const std::string& line, size_t start, char quote) {
    size_t pos = start + 1;
    while (pos < line.size()) {
        if (line[pos] == quote) {
            if (pos + 1 < line.size() && line[pos + 1] == quote) {
                pos += 2;
                continue;
            }
            return pos + 1;
        }
        ++pos;
    }
    return line.size();
}

void scan_sql_lexical(const std::string& line,
                      LineState& next_state,
                      std::vector<Candidate>& high_priority) {
    size_t pos = 0;
    if (next_state.block == LineState::Block::BlockComment) {
        const size_t close = line.find("*/");
        if (close == std::string::npos) {
            append_candidate(high_priority, 0, line.size(), TokenRole::Comment);
            return;
        }
        append_candidate(high_priority, 0, close + 2, TokenRole::Comment);
        next_state = {};
        pos = close + 2;
    } else if (next_state.block == LineState::Block::HereString) {
        const size_t close = line.find(next_state.delimiter);
        if (close == std::string::npos) {
            append_candidate(high_priority, 0, line.size(), TokenRole::String);
            return;
        }
        append_candidate(high_priority, 0, close + next_state.delimiter.size(), TokenRole::String);
        pos = close + next_state.delimiter.size();
        next_state = {};
    }
    while (pos < line.size()) {
        if (line.compare(pos, 2, "--") == 0 || line[pos] == '#') {
            append_candidate(high_priority, pos, line.size(), TokenRole::Comment);
            return;
        }
        if (line.compare(pos, 2, "/*") == 0) {
            const size_t close = line.find("*/", pos + 2);
            if (close == std::string::npos) {
                append_candidate(high_priority, pos, line.size(), TokenRole::Comment);
                next_state.block = LineState::Block::BlockComment;
                return;
            }
            append_candidate(high_priority, pos, close + 2, TokenRole::Comment);
            pos = close + 2;
            continue;
        }
        if (line[pos] == '$') {
            size_t tag_end = pos + 1;
            while (tag_end < line.size() && ascii_identifier_part(line[tag_end])) ++tag_end;
            if (tag_end < line.size() && line[tag_end] == '$') {
                const std::string delimiter = line.substr(pos, tag_end + 1 - pos);
                const size_t close = line.find(delimiter, tag_end + 1);
                if (close == std::string::npos) {
                    append_candidate(high_priority, pos, line.size(), TokenRole::String);
                    next_state.block = LineState::Block::HereString;
                    next_state.delimiter = delimiter;
                    return;
                }
                append_candidate(high_priority, pos, close + delimiter.size(), TokenRole::String);
                pos = close + delimiter.size();
                continue;
            }
        }
        if (line[pos] == '\'' || line[pos] == '"' || line[pos] == '`') {
            const size_t end = sql_quoted_end(line, pos, line[pos]);
            append_candidate(high_priority, pos, end, TokenRole::String);
            pos = end;
            continue;
        }
        ++pos;
    }
}

size_t leading_indentation(const std::string& line) {
    size_t pos = 0;
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
    return pos;
}

void scan_config_lexical(Language language,
                         const std::string& line,
                         LineState& next_state,
                         std::vector<Candidate>& high_priority,
                         std::vector<Candidate>& structural,
                         std::vector<Candidate>& lower) {
    size_t pos = 0;
    if (language == Language::Toml && next_state.block == LineState::Block::HereString) {
        const size_t close = line.find(next_state.delimiter);
        if (close == std::string::npos) {
            append_candidate(high_priority, 0, line.size(), TokenRole::String);
            return;
        }
        append_candidate(high_priority, 0, close + next_state.delimiter.size(), TokenRole::String);
        pos = close + next_state.delimiter.size();
        next_state = {};
    } else if (language == Language::Yaml &&
               next_state.block == LineState::Block::IndentedString) {
        const size_t indent = leading_indentation(line);
        if (line.find_first_not_of(" \t") == std::string::npos || indent > next_state.block_depth) {
            append_candidate(high_priority, 0, line.size(), TokenRole::String);
            return;
        }
        next_state = {};
    }
    const size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos) return;
    if (language == Language::Ini && (line[first] == ';' || line[first] == '#')) {
        append_candidate(high_priority, first, line.size(), TokenRole::Comment);
        return;
    }
    if (language == Language::Ini && line[first] == '[') {
        const size_t close = line.find(']', first + 1);
        append_candidate(structural, first,
                         close == std::string::npos ? line.size() : close + 1,
                         TokenRole::Heading);
    }
    if (language == Language::Toml && line[first] == '[') {
        const size_t close = line.rfind(']');
        append_candidate(structural, first,
                         close == std::string::npos ? line.size() : close + 1,
                         TokenRole::Heading);
    }
    while (pos < line.size()) {
        if ((language == Language::Toml && line[pos] == '#') ||
            (language == Language::Yaml && line[pos] == '#' &&
             (pos == 0 || std::isspace(static_cast<unsigned char>(line[pos - 1])) != 0))) {
            append_candidate(high_priority, pos, line.size(), TokenRole::Comment);
            break;
        }
        if (language == Language::Ini && line[pos] == ';') {
            append_candidate(high_priority, pos, line.size(), TokenRole::Comment);
            break;
        }
        if (language == Language::Toml &&
            (line.compare(pos, 3, "\"\"\"") == 0 || line.compare(pos, 3, "'''") == 0)) {
            const std::string delimiter = line.substr(pos, 3);
            const size_t close = line.find(delimiter, pos + 3);
            if (close == std::string::npos) {
                append_candidate(high_priority, pos, line.size(), TokenRole::String);
                next_state.block = LineState::Block::HereString;
                next_state.delimiter = delimiter;
                return;
            }
            append_candidate(high_priority, pos, close + 3, TokenRole::String);
            pos = close + 3;
            continue;
        }
        if (line[pos] == '\'' || line[pos] == '"') {
            const size_t end = quoted_end(line, pos, line[pos]);
            const size_t after = skip_ascii_space(line, end);
            const char key_separator = language == Language::Yaml ? ':' : '=';
            append_candidate(high_priority,
                             pos,
                             end,
                             after < line.size() && line[after] == key_separator
                                 ? TokenRole::Property
                                 : TokenRole::String);
            pos = end;
            continue;
        }
        ++pos;
    }
    const char separator = language == Language::Yaml ? ':' : '=';
    size_t key_start = first;
    if (language == Language::Yaml && line[key_start] == '-') {
        key_start = skip_ascii_space(line, key_start + 1);
    }
    const size_t separator_pos = line.find(separator, key_start);
    if (separator_pos != std::string::npos &&
        !(language != Language::Yaml && line[first] == '[')) {
        size_t key_end = separator_pos;
        while (key_end > key_start && std::isspace(static_cast<unsigned char>(line[key_end - 1])) != 0) --key_end;
        append_candidate(structural, key_start, key_end, TokenRole::Property);
        if (language == Language::Yaml) {
            const size_t value = skip_ascii_space(line, separator_pos + 1);
            if (value < line.size() && (line[value] == '|' || line[value] == '>')) {
                append_candidate(lower, value, value + 1, TokenRole::Operator);
                next_state.block = LineState::Block::IndentedString;
                next_state.block_depth = leading_indentation(line);
            }
        }
    }
    if (language == Language::Yaml) {
        for (size_t index = 0; index < line.size(); ++index) {
            if ((line[index] == '&' || line[index] == '*' || line[index] == '!') &&
                index + 1 < line.size() && ascii_identifier_start(line[index + 1])) {
                const size_t end = identifier_end(line, index + 2);
                append_candidate(lower, index, end,
                                 line[index] == '!' ? TokenRole::Preprocessor : TokenRole::Variable);
                index = end - 1;
            }
        }
    }
}

void scan_assembly_lexical(const std::string& line,
                           std::vector<Candidate>& high_priority,
                           std::vector<Candidate>& structural,
                           std::vector<Candidate>& lower) {
    size_t pos = 0;
    while (pos < line.size()) {
        const bool hash_immediate = line[pos] == '#' && pos + 1 < line.size() &&
                                    (std::isdigit(static_cast<unsigned char>(line[pos + 1])) != 0 ||
                                     line[pos + 1] == '+' || line[pos + 1] == '-');
        if (line[pos] == ';' || (line[pos] == '#' && !hash_immediate)) {
            append_candidate(high_priority, pos, line.size(), TokenRole::Comment);
            break;
        }
        if (line[pos] == '\'' || line[pos] == '"') {
            const size_t end = quoted_end(line, pos, line[pos]);
            append_candidate(high_priority, pos, end, TokenRole::String);
            pos = end;
            continue;
        }
        ++pos;
    }
    const size_t first = line.find_first_not_of(" \t");
    if (first != std::string::npos && line[first] == '.') {
        size_t end = first + 1;
        while (end < line.size() && ascii_identifier_part(line[end])) ++end;
        append_candidate(structural, first, end, TokenRole::Preprocessor);
    }
    const size_t colon = line.find(':');
    if (colon != std::string::npos && first != std::string::npos && colon > first) {
        append_candidate(structural, first, colon, TokenRole::Function);
    }
    for (size_t index = 0; index < line.size(); ++index) {
        if ((line[index] == '%' || line[index] == '$' || line[index] == '#') &&
            index + 1 < line.size()) {
            size_t end = index + 1;
            while (end < line.size() && (ascii_identifier_part(line[end]) ||
                                         std::isxdigit(static_cast<unsigned char>(line[end])) != 0)) ++end;
            append_candidate(lower, index, end,
                             line[index] == '%' ? TokenRole::Type : TokenRole::Number);
            index = end > 0 ? end - 1 : end;
        }
    }
}

void add_preprocessor(Language language,
                      const std::string& line,
                      std::vector<Candidate>& structural) {
    const size_t first = line.find_first_not_of(" \t");
    if ((language == Language::C || language == Language::Cpp) &&
        first != std::string::npos && line[first] == '#') {
        append_candidate(structural, first, line.size(), TokenRole::Preprocessor);
    } else if (language == Language::Rust && first != std::string::npos &&
               (line.compare(first, 2, "#[") == 0 || line.compare(first, 3, "#![") == 0)) {
        const size_t prefix = line.compare(first, 3, "#![") == 0 ? 3 : 2;
        append_candidate(structural, first, first + prefix, TokenRole::Preprocessor);
        const size_t close = line.rfind(']');
        if (close != std::string::npos) {
            append_candidate(structural, close, close + 1, TokenRole::Preprocessor);
        }
    }
}

void add_bash_commands(const std::string& line, std::vector<Candidate>& lower) {
    size_t pos = 0;
    while (pos < line.size()) {
        if (!ascii_identifier_start(line[pos])) {
            ++pos;
            continue;
        }
        const size_t start = pos++;
        while (pos < line.size() && (ascii_identifier_part(line[pos]) || line[pos] == '-')) ++pos;
        size_t previous = start;
        while (previous > 0 && (line[previous - 1] == ' ' || line[previous - 1] == '\t')) --previous;
        bool command = previous == 0 || std::string(";|&(").find(line[previous - 1]) != std::string::npos;
        if (!command && previous > 0) {
            size_t word_end = previous;
            size_t word_start = word_end;
            while (word_start > 0 && ascii_identifier_part(line[word_start - 1])) --word_start;
            const std::string prior = line.substr(word_start, word_end - word_start);
            command = prior == "do" || prior == "then" || prior == "else" || prior == "elif";
        }
        if (command && keywords_for(Language::Bash).find(line.substr(start, pos - start)) ==
                           keywords_for(Language::Bash).end() &&
            (skip_ascii_space(line, pos) >= line.size() || line[skip_ascii_space(line, pos)] != '=')) {
            append_candidate(lower, start, pos, TokenRole::Function);
        }
    }
}

void add_css_tokens(const std::string& line,
                    std::vector<Candidate>& structural,
                    std::vector<Candidate>& words,
                    std::vector<Candidate>& lower) {
    const size_t first = line.find_first_not_of(" \t");
    if (first != std::string::npos && line[first] == '@') {
        size_t end = first + 1;
        while (end < line.size() && (ascii_identifier_part(line[end]) || line[end] == '-')) ++end;
        append_candidate(words, first, end, TokenRole::Keyword);
    }
    size_t pos = 0;
    while (pos < line.size()) {
        if ((ascii_identifier_start(line[pos]) || (line[pos] == '-' && pos + 1 < line.size())) &&
            (pos == 0 || line[pos - 1] == '{' || line[pos - 1] == ';' ||
             std::isspace(static_cast<unsigned char>(line[pos - 1])) != 0)) {
            const size_t start = pos++;
            while (pos < line.size() && (ascii_identifier_part(line[pos]) || line[pos] == '-')) ++pos;
            const size_t after = skip_ascii_space(line, pos);
            if (after < line.size() && line[after] == ':') {
                append_candidate(structural, start, pos, TokenRole::Property);
            }
            continue;
        }
        if (line[pos] == '#') {
            size_t end = pos + 1;
            while (end < line.size() && std::isxdigit(static_cast<unsigned char>(line[end]))) ++end;
            if (end - pos == 4 || end - pos == 7 || end - pos == 9) {
                append_candidate(words, pos, end, TokenRole::Literal);
            }
        }
        ++pos;
    }
    add_words_numbers_operators(Language::Css, line, words, lower);
}

size_t markup_tag_end(const std::string& line, size_t start) {
    char quote = 0;
    for (size_t pos = start + 1; pos < line.size(); ++pos) {
        if (quote != 0) {
            if (line[pos] == quote) quote = 0;
        } else if (line[pos] == '\'' || line[pos] == '"') {
            quote = line[pos];
        } else if (line[pos] == '>') {
            return pos + 1;
        }
    }
    return line.size();
}

std::string markup_tag_name(const std::string& line, size_t start, size_t end) {
    size_t pos = start + 1;
    if (pos < end && line[pos] == '/') ++pos;
    pos = skip_ascii_space(line, pos);
    const size_t name_start = pos;
    while (pos < end && (ascii_identifier_part(line[pos]) || line[pos] == ':' || line[pos] == '-')) ++pos;
    return lower_ascii(line.substr(name_start, pos - name_start));
}

HighlightedLine highlight_code(Language language,
                               const std::string& line,
                               const LineState& state,
                               size_t byte_budget);

void append_embedded_candidates(Language language,
                                const std::string& line,
                                size_t start,
                                size_t end,
                                size_t byte_budget,
                                std::vector<Candidate>& embedded_priority) {
    if (end <= start) return;
    const HighlightedLine highlighted =
        highlight_code(language, line.substr(start, end - start), {}, byte_budget);
    for (const Span& span : highlighted.spans) {
        append_candidate(embedded_priority, start + span.start, start + span.end, span.role);
    }
}

void add_markup_attributes(const std::string& line,
                           size_t pos,
                           size_t end,
                           bool multi_language,
                           size_t byte_budget,
                           std::vector<Candidate>& embedded_priority,
                           std::vector<Candidate>& high_priority,
                           std::vector<Candidate>& structural,
                           std::vector<Candidate>& lower) {
    while (pos < end) {
        pos = skip_ascii_space(line, pos);
        if (pos >= end || line[pos] == '>') break;
        if (line[pos] == '/') {
            append_candidate(structural, pos, pos + 1, TokenRole::Tag);
            ++pos;
            continue;
        }
        if (ascii_identifier_start(line[pos]) || line[pos] == ':') {
            const size_t attr_start = pos++;
            while (pos < end && (ascii_identifier_part(line[pos]) || line[pos] == ':' ||
                                 line[pos] == '-' || line[pos] == '.')) ++pos;
            const std::string attribute = lower_ascii(line.substr(attr_start, pos - attr_start));
            append_candidate(structural, attr_start, pos, TokenRole::Attribute);
            pos = skip_ascii_space(line, pos);
            if (pos < end && line[pos] == '=') {
                append_candidate(lower, pos, pos + 1, TokenRole::Operator);
                pos = skip_ascii_space(line, pos + 1);
                if (pos < end && (line[pos] == '\'' || line[pos] == '"')) {
                    const char quote = line[pos];
                    const size_t value_start = pos;
                    const size_t value_end = std::min(quoted_end(line, pos, quote), end);
                    append_candidate(high_priority, value_start, value_end, TokenRole::String);
                    const bool closed = value_end > value_start + 1 && line[value_end - 1] == quote;
                    const size_t content_start = value_start + 1;
                    const size_t content_end = closed ? value_end - 1 : value_end;
                    if (multi_language && attribute == "style") {
                        append_embedded_candidates(Language::Css,
                                                   line,
                                                   content_start,
                                                   content_end,
                                                   byte_budget,
                                                   embedded_priority);
                    } else if (multi_language && attribute.size() > 2 &&
                               attribute.rfind("on", 0) == 0) {
                        append_embedded_candidates(Language::JavaScript,
                                                   line,
                                                   content_start,
                                                   content_end,
                                                   byte_budget,
                                                   embedded_priority);
                    }
                    pos = value_end;
                }
            }
            continue;
        }
        ++pos;
    }
}

void add_markup_tag_tokens(const std::string& line,
                           size_t start,
                           size_t end,
                           bool multi_language,
                           size_t byte_budget,
                           std::vector<Candidate>& embedded_priority,
                           std::vector<Candidate>& high_priority,
                           std::vector<Candidate>& structural,
                           std::vector<Candidate>& lower) {
    size_t pos = start;
    append_candidate(structural, pos, std::min(pos + size_t{1}, end), TokenRole::Tag);
    ++pos;
    if (pos < end && line[pos] == '/') {
        append_candidate(structural, pos, pos + 1, TokenRole::Tag);
        ++pos;
    }
    pos = skip_ascii_space(line, pos);
    const size_t name_start = pos;
    while (pos < end && (ascii_identifier_part(line[pos]) || line[pos] == ':' || line[pos] == '-')) ++pos;
    append_candidate(structural, name_start, pos, TokenRole::Tag);
    add_markup_attributes(line,
                          pos,
                          end,
                          multi_language,
                          byte_budget,
                          embedded_priority,
                          high_priority,
                          structural,
                          lower);
    if (end > start && line[end - 1] == '>') {
        append_candidate(structural, end - 1, end, TokenRole::Tag);
    }
}

HighlightedLine highlight_markup(Language language,
                                 const std::string& line,
                                 const LineState& state,
                                 size_t byte_budget) {
    HighlightedLine result;
    result.next_state = state;
    const bool multi_language = language == Language::Html;
    std::vector<Candidate> embedded_priority;
    std::vector<Candidate> high_priority;
    std::vector<Candidate> structural;
    std::vector<Candidate> lower;
    size_t pos = 0;

    if (state.block == LineState::Block::Script || state.block == LineState::Block::Style) {
        const bool script = state.block == LineState::Block::Script;
        const std::string closing = script ? "</script" : "</style";
        const size_t close = find_case_insensitive(line, closing, 0);
        const size_t code_end = close == std::string::npos ? line.size() : close;
        if (multi_language) {
            LineState nested;
            nested.block = state.nested_block;
            nested.delimiter = state.nested_delimiter;
            nested.strip_tabs = state.nested_strip_tabs;
            nested.block_depth = state.nested_block_depth;
            const HighlightedLine embedded =
                highlight_code(script ? Language::JavaScript : Language::Css,
                               line.substr(0, code_end),
                               nested,
                               byte_budget);
            for (const Span& span : embedded.spans) {
                append_candidate(embedded_priority, span.start, span.end, span.role);
            }
            result.next_state.nested_block = embedded.next_state.block;
            result.next_state.nested_delimiter = embedded.next_state.delimiter;
            result.next_state.nested_strip_tabs = embedded.next_state.strip_tabs;
            result.next_state.nested_block_depth = embedded.next_state.block_depth;
        } else {
            append_candidate(high_priority, 0, code_end, TokenRole::String);
        }
        if (close == std::string::npos) {
            result.spans =
                resolve_candidates(line.size(), {embedded_priority, high_priority});
            return result;
        }
        result.next_state = {};
        pos = close;
    } else if (state.block == LineState::Block::HtmlComment) {
        const size_t close = line.find("-->");
        if (close == std::string::npos) {
            append_candidate(high_priority, 0, line.size(), TokenRole::Comment);
            result.spans = resolve_candidates(line.size(), {high_priority});
            return result;
        }
        append_candidate(high_priority, 0, close + 3, TokenRole::Comment);
        result.next_state = {};
        pos = close + 3;
    } else if (state.block == LineState::Block::CData) {
        const size_t close = line.find("]]>");
        if (close == std::string::npos) {
            append_candidate(high_priority, 0, line.size(), TokenRole::String);
            result.spans = resolve_candidates(line.size(), {high_priority});
            return result;
        }
        append_candidate(high_priority, 0, close + 3, TokenRole::String);
        result.next_state = {};
        pos = close + 3;
    } else if (state.block == LineState::Block::Tag) {
        char quote = 0;
        size_t end = line.size();
        for (size_t index = 0; index < line.size(); ++index) {
            if (quote != 0) {
                if (line[index] == quote && unescaped_at(line, index)) quote = 0;
            } else if (line[index] == '\'' || line[index] == '"') {
                quote = line[index];
            } else if (line[index] == '>') {
                end = index + 1;
                break;
            }
        }
        add_markup_attributes(line,
                              0,
                              end,
                              multi_language,
                              byte_budget,
                              embedded_priority,
                              high_priority,
                              structural,
                              lower);
        const bool closed = end > 0 && end <= line.size() && line[end - 1] == '>';
        if (!closed) {
            result.spans = resolve_candidates(
                line.size(), {embedded_priority, high_priority, structural, lower});
            return result;
        }
        append_candidate(structural, end - 1, end, TokenRole::Tag);
        const std::string pending_name = state.delimiter;
        const bool closing_tag = state.strip_tabs;
        const bool self_closing = end >= 2 && line[end - 2] == '/';
        result.next_state = {};
        pos = end;
        if (!closing_tag && !self_closing &&
            (pending_name == "script" || pending_name == "style")) {
            result.next_state.block = pending_name == "script" ? LineState::Block::Script
                                                               : LineState::Block::Style;
            if (pos < line.size()) {
                const bool script = pending_name == "script";
                const std::string closing = script ? "</script" : "</style";
                const size_t close = find_case_insensitive(line, closing, pos);
                const size_t code_end = close == std::string::npos ? line.size() : close;
                if (multi_language) {
                    const HighlightedLine embedded =
                        highlight_code(script ? Language::JavaScript : Language::Css,
                                       line.substr(pos, code_end - pos),
                                       {},
                                       byte_budget);
                    for (const Span& span : embedded.spans) {
                        append_candidate(embedded_priority,
                                         pos + span.start,
                                         pos + span.end,
                                         span.role);
                    }
                    result.next_state.nested_block = embedded.next_state.block;
                    result.next_state.nested_delimiter = embedded.next_state.delimiter;
                    result.next_state.nested_strip_tabs = embedded.next_state.strip_tabs;
                    result.next_state.nested_block_depth = embedded.next_state.block_depth;
                } else {
                    append_candidate(high_priority, pos, code_end, TokenRole::String);
                }
                if (close == std::string::npos) {
                    result.spans = resolve_candidates(
                        line.size(), {embedded_priority, high_priority, structural, lower});
                    return result;
                }
                result.next_state = {};
                pos = close;
            }
        }
    }

    while (pos < line.size()) {
        const size_t open = line.find('<', pos);
        if (open == std::string::npos) break;
        if (line.compare(open, 4, "<!--") == 0) {
            const size_t close = line.find("-->", open + 4);
            if (close == std::string::npos) {
                append_candidate(high_priority, open, line.size(), TokenRole::Comment);
                result.next_state.block = LineState::Block::HtmlComment;
                break;
            }
            append_candidate(high_priority, open, close + 3, TokenRole::Comment);
            pos = close + 3;
            continue;
        }
        if (line.compare(open, 9, "<![CDATA[") == 0) {
            const size_t close = line.find("]]>", open + 9);
            if (close == std::string::npos) {
                append_candidate(high_priority, open, line.size(), TokenRole::String);
                result.next_state.block = LineState::Block::CData;
                break;
            }
            append_candidate(high_priority, open, close + 3, TokenRole::String);
            pos = close + 3;
            continue;
        }
        const size_t end = markup_tag_end(line, open);
        const bool tag_closed = end > open && end <= line.size() && line[end - 1] == '>';
        const std::string name = markup_tag_name(line, open, end);
        if (line.compare(open, 2, "<!") == 0 || line.compare(open, 2, "<?") == 0) {
            append_candidate(structural, open, end, TokenRole::Preprocessor);
        } else {
            add_markup_tag_tokens(line,
                                  open,
                                  end,
                                  multi_language,
                                  byte_budget,
                                  embedded_priority,
                                  high_priority,
                                  structural,
                                  lower);
        }
        const bool closing_tag = open + 1 < line.size() && line[open + 1] == '/';
        const bool self_closing = end >= 2 && line[end - 2] == '/';
        if (!tag_closed) {
            result.next_state.block = LineState::Block::Tag;
            result.next_state.delimiter = name;
            result.next_state.strip_tabs = closing_tag;
            break;
        }
        pos = end;
        if (language != Language::Xml && !closing_tag && !self_closing &&
            (name == "script" || name == "style")) {
            result.next_state.block = name == "script" ? LineState::Block::Script
                                                       : LineState::Block::Style;
            result.next_state.nested_block = LineState::Block::None;
            const std::string closing = name == "script" ? "</script" : "</style";
            const size_t close = find_case_insensitive(line, closing, pos);
            const size_t code_end = close == std::string::npos ? line.size() : close;
            if (multi_language) {
                const HighlightedLine embedded =
                    highlight_code(name == "script" ? Language::JavaScript : Language::Css,
                                   line.substr(pos, code_end - pos),
                                   {},
                                   byte_budget);
                for (const Span& span : embedded.spans) {
                    append_candidate(embedded_priority,
                                     pos + span.start,
                                     pos + span.end,
                                     span.role);
                }
                result.next_state.nested_block = embedded.next_state.block;
                result.next_state.nested_delimiter = embedded.next_state.delimiter;
                result.next_state.nested_strip_tabs = embedded.next_state.strip_tabs;
                result.next_state.nested_block_depth = embedded.next_state.block_depth;
            } else {
                append_candidate(high_priority, pos, code_end, TokenRole::String);
            }
            if (close == std::string::npos) {
                break;
            }
            result.next_state = {};
            pos = close;
        }
    }
    result.spans = resolve_candidates(
        line.size(), {embedded_priority, high_priority, structural, lower});
    return result;
}

HighlightedLine highlight_code(Language language,
                               const std::string& line,
                               const LineState& state,
                               size_t byte_budget) {
    HighlightedLine result;
    result.next_state = state;
    if (line.size() > kMaximumHighlightedLineBytes || line.size() > byte_budget) {
        result.work_limited = true;
        return result;
    }
    if (language == Language::Html || language == Language::HtmlOnly ||
        language == Language::Xml) {
        return highlight_markup(language, line, state, byte_budget);
    }
    std::vector<Candidate> high_priority;
    std::vector<Candidate> structural;
    std::vector<Candidate> words;
    std::vector<Candidate> lower;
    if (language == Language::Python) {
        scan_python_lexical(line, result.next_state, high_priority);
    } else if (language == Language::Bash) {
        scan_bash_lexical(line, result.next_state, high_priority, structural, lower);
    } else if (language == Language::Php) {
        scan_php_lexical(line, result.next_state, high_priority, structural, lower);
    } else if (language == Language::Perl || language == Language::Ruby) {
        scan_perl_ruby_lexical(language, line, result.next_state, high_priority, structural, lower);
    } else if (language == Language::Rust || language == Language::Go) {
        scan_rust_go_lexical(language, line, result.next_state, high_priority);
    } else if (language == Language::PowerShell) {
        scan_powershell_lexical(line, result.next_state, high_priority, structural, lower);
    } else if (language == Language::Sql) {
        scan_sql_lexical(line, result.next_state, high_priority);
    } else if (language == Language::Toml || language == Language::Yaml ||
               language == Language::Ini) {
        scan_config_lexical(language,
                            line,
                            result.next_state,
                            high_priority,
                            structural,
                            lower);
    } else if (language == Language::Assembly) {
        scan_assembly_lexical(line, high_priority, structural, lower);
    } else if (language == Language::Json) {
        scan_json_lexical(line, result.next_state, high_priority, structural);
        size_t key = 0;
        while (key < line.size()) {
            if (!ascii_identifier_start(line[key])) {
                ++key;
                continue;
            }
            const size_t start = key++;
            while (key < line.size() && (ascii_identifier_part(line[key]) || line[key] == '-')) ++key;
            const size_t after = skip_ascii_space(line, key);
            if (after < line.size() && line[after] == ':') {
                append_candidate(structural, start, key, TokenRole::Property);
            }
        }
    } else {
        scan_c_like_lexical(language, line, result.next_state, high_priority);
    }
    add_preprocessor(language, line, structural);
    if (language == Language::Bash) {
        add_bash_commands(line, lower);
    }
    if (language == Language::JavaScript || language == Language::TypeScript ||
        language == Language::Php) {
        size_t tag = 0;
        while ((tag = line.find('<', tag)) != std::string::npos) {
            size_t name = tag + 1;
            if (name < line.size() && line[name] == '/') ++name;
            if (name >= line.size() || !ascii_identifier_start(line[name])) {
                ++tag;
                continue;
            }
            const size_t end = markup_tag_end(line, tag);
            if (end == line.size() && (line.empty() || line.back() != '>')) break;
            std::vector<Candidate> embedded_priority;
            add_markup_tag_tokens(line,
                                  tag,
                                  end,
                                  false,
                                  byte_budget,
                                  embedded_priority,
                                  high_priority,
                                  structural,
                                  lower);
            tag = end;
        }
    }
    if (language == Language::Css) {
        add_css_tokens(line, structural, words, lower);
    } else {
        add_words_numbers_operators(language, line, words, lower);
    }
    result.spans = resolve_candidates(line.size(), {high_priority, structural, words, lower});
    return result;
}

}  // namespace

bool LineState::operator==(const LineState& other) const {
    return block == other.block && fence_character == other.fence_character &&
           fence_length == other.fence_length && embedded_language == other.embedded_language &&
           nested_block == other.nested_block && nested_delimiter == other.nested_delimiter &&
           nested_strip_tabs == other.nested_strip_tabs &&
           nested_block_depth == other.nested_block_depth &&
           nested_inner_block == other.nested_inner_block &&
           nested_inner_delimiter == other.nested_inner_delimiter &&
           nested_inner_strip_tabs == other.nested_inner_strip_tabs &&
           nested_inner_block_depth == other.nested_inner_block_depth && delimiter == other.delimiter &&
           strip_tabs == other.strip_tabs && block_depth == other.block_depth;
}

const char* language_name(Language language) {
    switch (language) {
        case Language::Text:
            return "text";
        case Language::Markdown:
            return "markdown";
        case Language::Python:
            return "python";
        case Language::C:
            return "c";
        case Language::Cpp:
            return "cpp";
        case Language::CSharp:
            return "csharp";
        case Language::Java:
            return "java";
        case Language::JavaScript:
            return "javascript";
        case Language::TypeScript:
            return "typescript";
        case Language::Html:
            return "html";
        case Language::HtmlOnly:
            return "htmlonly";
        case Language::Css:
            return "css";
        case Language::Xml:
            return "xml";
        case Language::Json:
            return "json";
        case Language::Bash:
            return "bash";
        case Language::Php:
            return "php";
        case Language::Perl:
            return "perl";
        case Language::Ruby:
            return "ruby";
        case Language::Rust:
            return "rust";
        case Language::Go:
            return "go";
        case Language::PowerShell:
            return "powershell";
        case Language::Assembly:
            return "assembly";
        case Language::Sql:
            return "sql";
        case Language::Toml:
            return "toml";
        case Language::Yaml:
            return "yaml";
        case Language::Ini:
            return "ini";
    }
    return "text";
}

bool parse_language(const std::string& text, Language& language) {
    const std::string mode = lower_ascii(text);
    if (mode == "text" || mode == "txt" || mode == "plain" || mode == "plaintext") {
        language = Language::Text;
        return true;
    }
    if (mode == "markdown" || mode == "md") {
        language = Language::Markdown;
        return true;
    }
    if (mode == "python" || mode == "py") {
        language = Language::Python;
        return true;
    }
    if (mode == "c") {
        language = Language::C;
        return true;
    }
    if (mode == "cpp" || mode == "c++" || mode == "cxx") {
        language = Language::Cpp;
        return true;
    }
    if (mode == "csharp" || mode == "c#" || mode == "cs") {
        language = Language::CSharp;
        return true;
    }
    if (mode == "java") {
        language = Language::Java;
        return true;
    }
    if (mode == "javascript" || mode == "js") {
        language = Language::JavaScript;
        return true;
    }
    if (mode == "typescript" || mode == "ts") {
        language = Language::TypeScript;
        return true;
    }
    if (mode == "html" || mode == "html5" || mode == "html-multi" ||
        mode == "htmlmulti") {
        language = Language::Html;
        return true;
    }
    if (mode == "htmlonly" || mode == "html-only") {
        language = Language::HtmlOnly;
        return true;
    }
    if (mode == "css" || mode == "css3") {
        language = Language::Css;
        return true;
    }
    if (mode == "xml") {
        language = Language::Xml;
        return true;
    }
    if (mode == "json" || mode == "jsonl" || mode == "ndjson") {
        language = Language::Json;
        return true;
    }
    if (mode == "bash" || mode == "sh" || mode == "shell") {
        language = Language::Bash;
        return true;
    }
    if (mode == "php") {
        language = Language::Php;
        return true;
    }
    if (mode == "perl" || mode == "pl") {
        language = Language::Perl;
        return true;
    }
    if (mode == "ruby" || mode == "rb") {
        language = Language::Ruby;
        return true;
    }
    if (mode == "rust" || mode == "rs") {
        language = Language::Rust;
        return true;
    }
    if (mode == "go" || mode == "golang") {
        language = Language::Go;
        return true;
    }
    if (mode == "powershell" || mode == "pwsh" || mode == "ps1") {
        language = Language::PowerShell;
        return true;
    }
    if (mode == "assembly" || mode == "assembler" || mode == "asm") {
        language = Language::Assembly;
        return true;
    }
    if (mode == "sql") {
        language = Language::Sql;
        return true;
    }
    if (mode == "toml") {
        language = Language::Toml;
        return true;
    }
    if (mode == "yaml" || mode == "yml") {
        language = Language::Yaml;
        return true;
    }
    if (mode == "ini" || mode == "dosini") {
        language = Language::Ini;
        return true;
    }
    return false;
}

Language detect_language(const std::string& path) {
    if (path.empty()) {
        return Language::Text;
    }
    const std::filesystem::path file_path(path);
    const std::string extension = lower_ascii(file_path.extension().string());
    const std::string filename = lower_ascii(file_path.filename().string());
    if (extension == ".md" || extension == ".markdown" || extension == ".mdown" ||
        extension == ".mkd") {
        return Language::Markdown;
    }
    if (extension == ".py" || extension == ".pyw" || extension == ".pyi") return Language::Python;
    if (extension == ".c" || extension == ".h") return Language::C;
    if (extension == ".cc" || extension == ".cpp" || extension == ".cxx" ||
        extension == ".c++" || extension == ".hh" || extension == ".hpp" ||
        extension == ".hxx" || extension == ".h++" || extension == ".ipp" ||
        extension == ".tpp" || extension == ".inl") return Language::Cpp;
    if (extension == ".cs") return Language::CSharp;
    if (extension == ".java") return Language::Java;
    if (extension == ".js" || extension == ".mjs" || extension == ".cjs" ||
        extension == ".jsx") return Language::JavaScript;
    if (extension == ".ts" || extension == ".mts" || extension == ".cts" ||
        extension == ".tsx") return Language::TypeScript;
    if (extension == ".html" || extension == ".htm") return Language::Html;
    if (extension == ".xhtml") return Language::HtmlOnly;
    if (extension == ".css") return Language::Css;
    if (extension == ".xml" || extension == ".xsd" || extension == ".xsl" ||
        extension == ".xslt" || extension == ".svg") return Language::Xml;
    if (extension == ".json" || extension == ".jsonl" || extension == ".ndjson" ||
        extension == ".geojson" || extension == ".json5") return Language::Json;
    if (extension == ".sh" || extension == ".bash" || filename == ".bashrc" ||
        filename == ".bash_profile" || filename == ".bash_login" ||
        filename == ".bash_logout" || filename == ".profile" || filename == "bashrc" ||
        filename == "bash_profile" || filename == "bash_login" || filename == "bash_logout" ||
        filename == "profile") return Language::Bash;
    if (extension == ".php" || extension == ".php3" || extension == ".php4" ||
        extension == ".php5" || extension == ".php7" || extension == ".php8" ||
        extension == ".phtml" || extension == ".phps") return Language::Php;
    if (extension == ".pl" || extension == ".pm" || extension == ".pod" || extension == ".t")
        return Language::Perl;
    if (extension == ".rb" || extension == ".rake" || extension == ".gemspec" ||
        filename == "gemfile" || filename == "rakefile") return Language::Ruby;
    if (extension == ".rs") return Language::Rust;
    if (extension == ".go") return Language::Go;
    if (extension == ".ps1" || extension == ".psm1" || extension == ".psd1")
        return Language::PowerShell;
    if (extension == ".asm" || extension == ".s") return Language::Assembly;
    if (extension == ".sql") return Language::Sql;
    if (extension == ".toml") return Language::Toml;
    if (extension == ".yaml" || extension == ".yml") return Language::Yaml;
    if (extension == ".ini" || extension == ".cfg" || filename == ".editorconfig" ||
        filename == ".gitconfig") return Language::Ini;
    return Language::Text;
}

HighlightedLine highlight_line(Language language,
                               const std::string& line,
                               const LineState& state,
                               size_t byte_budget) {
    HighlightedLine result;
    result.next_state = state;
    if (language == Language::Text) {
        result.next_state = {};
        return result;
    }
    if (language != Language::Markdown) {
        return highlight_code(language, line, state, byte_budget);
    }
    if (line.size() > kMaximumHighlightedLineBytes || line.size() > byte_budget) {
        result.work_limited = true;
        return result;
    }

    std::vector<Candidate> high_priority;
    std::vector<Candidate> structural;
    std::vector<Candidate> inline_tokens;

    if (result.next_state.block == LineState::Block::Fence) {
        size_t start = 0;
        char character = 0;
        size_t length = 0;
        if (fence_at(line, start, character, length) && character == result.next_state.fence_character &&
            length >= result.next_state.fence_length && only_ascii_space_after(line, start + length)) {
            append_candidate(high_priority, start, line.size(), TokenRole::Preprocessor);
            result.next_state = {};
        } else {
            LineState embedded_state;
            embedded_state.block = result.next_state.nested_block;
            embedded_state.delimiter = result.next_state.nested_delimiter;
            embedded_state.strip_tabs = result.next_state.nested_strip_tabs;
            embedded_state.block_depth = result.next_state.nested_block_depth;
            embedded_state.nested_block = result.next_state.nested_inner_block;
            embedded_state.nested_delimiter = result.next_state.nested_inner_delimiter;
            embedded_state.nested_strip_tabs = result.next_state.nested_inner_strip_tabs;
            embedded_state.nested_block_depth = result.next_state.nested_inner_block_depth;
            const HighlightedLine embedded = highlight_line(result.next_state.embedded_language,
                                                            line,
                                                            embedded_state,
                                                            byte_budget);
            result.spans = embedded.spans;
            result.next_state.nested_block = embedded.next_state.block;
            result.next_state.nested_delimiter = embedded.next_state.delimiter;
            result.next_state.nested_strip_tabs = embedded.next_state.strip_tabs;
            result.next_state.nested_block_depth = embedded.next_state.block_depth;
            result.next_state.nested_inner_block = embedded.next_state.nested_block;
            result.next_state.nested_inner_delimiter = embedded.next_state.nested_delimiter;
            result.next_state.nested_inner_strip_tabs = embedded.next_state.nested_strip_tabs;
            result.next_state.nested_inner_block_depth = embedded.next_state.nested_block_depth;
            result.work_limited = embedded.work_limited;
            return result;
        }
        result.spans = resolve_candidates(line.size(), {high_priority});
        return result;
    }

    if (result.next_state.block == LineState::Block::HtmlComment) {
        add_html_comments(line, 0, result.next_state, high_priority);
        if (result.next_state.block == LineState::Block::HtmlComment) {
            result.spans = resolve_candidates(line.size(), {high_priority});
            return result;
        }
    }

    size_t fence_start = 0;
    char fence_character = 0;
    size_t fence_length = 0;
    if (fence_at(line, fence_start, fence_character, fence_length)) {
        append_candidate(high_priority, fence_start, line.size(), TokenRole::Preprocessor);
        result.next_state.block = LineState::Block::Fence;
        result.next_state.fence_character = fence_character;
        result.next_state.fence_length = fence_length;
        const size_t info_start = skip_ascii_space(line, fence_start + fence_length);
        size_t info_end = info_start;
        while (info_end < line.size() && line[info_end] != ' ' && line[info_end] != '\t' &&
               line[info_end] != '{' && line[info_end] != ',') {
            ++info_end;
        }
        Language embedded = Language::Text;
        if (info_end > info_start &&
            parse_language(line.substr(info_start, info_end - info_start), embedded)) {
            result.next_state.embedded_language = embedded;
        } else {
            result.next_state.embedded_language = Language::Text;
        }
        result.spans = resolve_candidates(line.size(), {high_priority});
        return result;
    }

    add_html_comments(line, 0, result.next_state, high_priority);
    add_inline_code(line, high_priority);
    add_markdown_structure(line, structural);
    add_inline_markdown(line, inline_tokens);
    result.spans = resolve_candidates(line.size(), {high_priority, structural, inline_tokens});
    return result;
}

void DocumentCache::clear() {
    source_lines_.clear();
    highlighted_lines_.clear();
    valid_line_count_ = 0;
    language_ = Language::Text;
}

void DocumentCache::invalidate_from(size_t line_index) {
    valid_line_count_ = std::min(valid_line_count_, line_index > 0 ? line_index - 1 : size_t{0});
}

void DocumentCache::update(const std::vector<std::string>& lines, Language language) {
    if (language != language_) {
        language_ = language;
        source_lines_ = lines;
        highlighted_lines_.assign(lines.size(), {});
        valid_line_count_ = 0;
        return;
    }

    size_t changed = 0;
    const size_t common = std::min(source_lines_.size(), lines.size());
    while (changed < common && source_lines_[changed] == lines[changed]) {
        ++changed;
    }
    if (changed == source_lines_.size() && changed == lines.size()) {
        return;
    }
    source_lines_ = lines;
    highlighted_lines_.resize(lines.size());
    invalidate_from(changed);
}

bool DocumentCache::highlight_through(size_t target_line, size_t& byte_budget) {
    if (source_lines_.empty()) {
        return true;
    }
    target_line = std::min(target_line, source_lines_.size() - 1);
    while (valid_line_count_ <= target_line) {
        const size_t index = valid_line_count_;
        const size_t cost = std::min(source_lines_[index].size(), kMaximumHighlightedLineBytes);
        if (cost > byte_budget) {
            return false;
        }
        const LineState state = index == 0 ? LineState{} : highlighted_lines_[index - 1].next_state;
        highlighted_lines_[index] = highlight_line(language_, source_lines_[index], state, byte_budget);
        byte_budget -= cost;
        ++valid_line_count_;

        if (language_ == Language::Markdown && index > 0 && setext_underline(source_lines_[index]) &&
            setext_heading_candidate(source_lines_[index - 1]) &&
            highlighted_lines_[index - 1].next_state.block == LineState::Block::None) {
            highlighted_lines_[index - 1].spans.clear();
            highlighted_lines_[index - 1].spans.push_back(
                {0, source_lines_[index - 1].size(), TokenRole::Heading});
        }
    }
    return true;
}

const HighlightedLine* DocumentCache::line(size_t index) const {
    if (index >= valid_line_count_ || index >= highlighted_lines_.size()) {
        return nullptr;
    }
    return &highlighted_lines_[index];
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

std::vector<HighlightedLine> highlight_document(Language language,
                                                const std::string& text,
                                                size_t byte_budget) {
    DocumentCache cache;
    const std::vector<std::string> lines = split_lines(text);
    cache.update(lines, language);
    if (!lines.empty()) {
        cache.highlight_through(lines.size() - 1, byte_budget);
    }
    std::vector<HighlightedLine> result(lines.size());
    for (size_t i = 0; i < lines.size(); ++i) {
        if (const HighlightedLine* highlighted = cache.line(i)) {
            result[i] = *highlighted;
        } else {
            result[i].work_limited = true;
        }
    }
    return result;
}

}  // namespace ainiux::highlight
