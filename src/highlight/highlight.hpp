#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace pkchat::highlight {

enum class Language {
    Text,
    Markdown,
    Python,
    C,
    Cpp,
    CSharp,
    Java,
    JavaScript,
    TypeScript,
    Html,
    Css,
    Xml,
    Json,
    Bash,
};

enum class TokenRole {
    Comment,
    Keyword,
    Type,
    String,
    Number,
    Literal,
    Function,
    Variable,
    Operator,
    Preprocessor,
    Tag,
    Attribute,
    Property,
    Heading,
    Emphasis,
    Link,
};

struct Span {
    size_t start = 0;
    size_t end = 0;
    TokenRole role = TokenRole::Operator;
};

struct LineState {
    enum class Block {
        None,
        HtmlComment,
        Fence,
        BlockComment,
        TripleSingleString,
        TripleDoubleString,
        TemplateString,
        RawString,
        Heredoc,
        CData,
        Script,
        Style,
    };

    Block block = Block::None;
    char fence_character = 0;
    size_t fence_length = 0;
    Language embedded_language = Language::Text;
    Block nested_block = Block::None;
    std::string nested_delimiter;
    bool nested_strip_tabs = false;
    std::string delimiter;
    bool strip_tabs = false;

    bool operator==(const LineState& other) const;
    bool operator!=(const LineState& other) const { return !(*this == other); }
};

struct HighlightedLine {
    std::vector<Span> spans;
    LineState next_state;
    bool work_limited = false;
};

constexpr size_t kMaximumHighlightedLineBytes = 64 * 1024;
constexpr size_t kDefaultFrameBudgetBytes = 256 * 1024;

const char* language_name(Language language);
bool parse_language(const std::string& text, Language& language);
Language detect_language(const std::string& path);

HighlightedLine highlight_line(Language language,
                               const std::string& line,
                               const LineState& state = {},
                               size_t byte_budget = kMaximumHighlightedLineBytes);

class DocumentCache {
   public:
    void update(const std::vector<std::string>& lines, Language language);
    bool highlight_through(size_t line, size_t& byte_budget);
    const HighlightedLine* line(size_t index) const;
    void invalidate_from(size_t line);
    void clear();

    size_t valid_line_count() const { return valid_line_count_; }
    size_t source_line_count() const { return source_lines_.size(); }

   private:
    std::vector<std::string> source_lines_;
    std::vector<HighlightedLine> highlighted_lines_;
    Language language_ = Language::Text;
    size_t valid_line_count_ = 0;
};

std::vector<std::string> split_lines(const std::string& text);
std::vector<HighlightedLine> highlight_document(Language language,
                                                const std::string& text,
                                                size_t byte_budget = kDefaultFrameBudgetBytes);

}  // namespace pkchat::highlight
