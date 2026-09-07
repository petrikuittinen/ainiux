#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "common.hpp"

namespace ainiux::agent::index {

constexpr std::size_t kMaximumDocumentationBytes = 512;

inline std::uint64_t fnv1a(const std::string& text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

inline std::string trim(std::string text) {
    return ascii_trim(std::move(text));
}

inline std::string collapse_space(const std::string& text) {
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

inline std::string bounded_documentation(std::string text) {
    text = collapse_space(text);
    if (text.size() > kMaximumDocumentationBytes) {
        text.resize(kMaximumDocumentationBytes - 3);
        text += "...";
    }
    return text;
}

inline std::vector<std::string> source_lines(const std::string& source) {
    return split_lines_crlf(source);
}

inline std::string line_range(const std::vector<std::string>& lines,
                              int first,
                              int last) {
    std::string output;
    for (int line = first; line <= last && line <= static_cast<int>(lines.size()); ++line) {
        output += lines[static_cast<std::size_t>(line - 1)];
        output.push_back('\n');
    }
    return output;
}

inline int indentation(const std::string& line) {
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

inline std::string join_qualified(const std::vector<std::string>& parts,
                                  const std::string& name) {
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

inline int matching_brace_line(const std::vector<std::string>& masked,
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

inline LogicalStatement collect_statement(const std::vector<std::string>& masked,
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
    if (statement.end_line >= masked.size())
        statement.end_line = masked.empty() ? 0 : masked.size() - 1;
    statement.masked = collapse_space(statement.masked);
    statement.original = collapse_space(statement.original);
    return statement;
}

inline int statement_end(const LogicalStatement& statement,
                         const std::vector<std::string>& masked) {
    return statement.delimiter == '{'
               ? matching_brace_line(masked, statement.end_line,
                                     statement.delimiter_column)
               : static_cast<int>(statement.end_line + 1);
}

}  // namespace ainiux::agent::index
