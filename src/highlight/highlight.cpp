#include "highlight/highlight.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <regex>

namespace pkchat::highlight {
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

}  // namespace

bool LineState::operator==(const LineState& other) const {
    return block == other.block && fence_character == other.fence_character &&
           fence_length == other.fence_length;
}

const char* language_name(Language language) {
    switch (language) {
        case Language::Text:
            return "text";
        case Language::Markdown:
            return "markdown";
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
    return false;
}

Language detect_language(const std::string& path) {
    if (path.empty()) {
        return Language::Text;
    }
    const std::string extension = lower_ascii(std::filesystem::path(path).extension().string());
    if (extension == ".md" || extension == ".markdown" || extension == ".mdown" ||
        extension == ".mkd") {
        return Language::Markdown;
    }
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
            append_candidate(high_priority, 0, line.size(), TokenRole::String);
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

}  // namespace pkchat::highlight
