#include "html/html.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <regex>
#include <string>
#include <vector>

namespace pkchat::html {
namespace {

std::string lower_ascii(std::string text) {
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return text;
}

bool ascii_iequal_at(const std::string& text, size_t pos, const std::string& needle) {
    if (pos + needle.size() > text.size()) {
        return false;
    }
    for (size_t i = 0; i < needle.size(); ++i) {
        char a = text[pos + i];
        char b = needle[i];
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<char>(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = static_cast<char>(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

size_t find_ascii_ci(const std::string& text, const std::string& needle, size_t start) {
    if (needle.empty() || start >= text.size()) {
        return std::string::npos;
    }
    for (size_t pos = start; pos + needle.size() <= text.size(); ++pos) {
        if (ascii_iequal_at(text, pos, needle)) {
            return pos;
        }
    }
    return std::string::npos;
}

bool is_tag_boundary(char ch) {
    return ch == '>' || ch == '/' || std::isspace(static_cast<unsigned char>(ch));
}

bool ignored_block_at(const std::string& input, size_t pos, const std::string& name, size_t& next_pos) {
    if (pos >= input.size() || input[pos] != '<') {
        return false;
    }
    const size_t name_pos = pos + 1;
    if (!ascii_iequal_at(input, name_pos, name)) {
        return false;
    }
    const size_t after_name = name_pos + name.size();
    if (after_name >= input.size() || !is_tag_boundary(input[after_name])) {
        return false;
    }
    const size_t open_end = input.find('>', after_name);
    if (open_end == std::string::npos) {
        next_pos = input.size();
        return true;
    }
    const std::string close_prefix = "</" + name;
    const size_t close = find_ascii_ci(input, close_prefix, open_end + 1);
    if (close == std::string::npos) {
        next_pos = input.size();
        return true;
    }
    const size_t close_end = input.find('>', close + close_prefix.size());
    next_pos = close_end == std::string::npos ? input.size() : close_end + 1;
    return true;
}

std::string remove_ignored_blocks(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    size_t pos = 0;
    while (pos < input.size()) {
        if (ascii_iequal_at(input, pos, "<!--")) {
            const size_t end = input.find("-->", pos + 4);
            out.push_back(' ');
            pos = end == std::string::npos ? input.size() : end + 3;
            continue;
        }
        size_t next_pos = pos;
        if (ignored_block_at(input, pos, "script", next_pos) ||
            ignored_block_at(input, pos, "style", next_pos) ||
            ignored_block_at(input, pos, "noscript", next_pos)) {
            out.push_back(' ');
            pos = next_pos;
            continue;
        }
        out.push_back(input[pos]);
        ++pos;
    }
    return out;
}

std::string decode_entities(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] != '&') {
            out.push_back(input[i]);
            continue;
        }
        const size_t semi = input.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 16) {
            out.push_back(input[i]);
            continue;
        }
        const std::string entity = input.substr(i + 1, semi - i - 1);
        if (entity == "amp") {
            out.push_back('&');
        } else if (entity == "lt") {
            out.push_back('<');
        } else if (entity == "gt") {
            out.push_back('>');
        } else if (entity == "quot") {
            out.push_back('"');
        } else if (entity == "apos" || entity == "#39") {
            out.push_back('\'');
        } else if (entity == "nbsp") {
            out.push_back(' ');
        } else if (!entity.empty() && entity[0] == '#') {
            char* end = nullptr;
            long cp = 0;
            if (entity.size() > 2 && (entity[1] == 'x' || entity[1] == 'X')) {
                cp = std::strtol(entity.c_str() + 2, &end, 16);
            } else {
                cp = std::strtol(entity.c_str() + 1, &end, 10);
            }
            if (end != nullptr && *end == '\0' && cp > 0 && cp < 128) {
                out.push_back(static_cast<char>(cp));
            } else {
                out += '&' + entity + ';';
            }
        } else {
            out += '&' + entity + ';';
        }
        i = semi;
    }
    return out;
}

std::string markdown_escape_text(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (char ch : input) {
        if (ch == '\\' || ch == '[' || ch == ']') {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    return out;
}

std::string markdown_escape_url(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (char ch : input) {
        if (ch == ')' || ch == '\\') {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    return out;
}

std::string tag_name(const std::string& tag) {
    std::smatch match;
    static const std::regex name_re(R"(^<\s*/?\s*([A-Za-z][A-Za-z0-9:-]*))");
    if (!std::regex_search(tag, match, name_re)) {
        return "";
    }
    return lower_ascii(match[1].str());
}

bool is_closing_tag(const std::string& tag) {
    return std::regex_search(tag, std::regex(R"(^<\s*/)", std::regex::icase));
}

bool is_self_closing_tag(const std::string& tag) {
    return std::regex_search(tag, std::regex(R"(/\s*>$)", std::regex::icase));
}

std::string href_attribute(const std::string& tag) {
    std::smatch match;
    static const std::regex href_re(R"PK(\bhref\s*=\s*("([^"]*)"|'([^']*)'|([^\s>]+)))PK", std::regex::icase);
    if (!std::regex_search(tag, match, href_re)) {
        return "";
    }
    if (match[2].matched) {
        return decode_entities(match[2].str());
    }
    if (match[3].matched) {
        return decode_entities(match[3].str());
    }
    return decode_entities(match[4].str());
}

class Writer {
   public:
    explicit Writer(OutputFormat format) : format_(format) {}

    void append_text(const std::string& raw) {
        const std::string decoded = decode_entities(raw);
        std::string normalized;
        normalized.reserve(decoded.size());
        for (unsigned char ch : decoded) {
            if (std::isspace(ch)) {
                pending_space_ = true;
                continue;
            }
            flush_pending_space();
            normalized.clear();
            normalized.push_back(static_cast<char>(ch));
            out_ += format_ == OutputFormat::Markdown ? markdown_escape_text(normalized) : normalized;
        }
    }

    void append_raw(const std::string& text) {
        pending_space_ = false;
        out_ += text;
    }

    void newline() {
        pending_space_ = false;
        trim_trailing_spaces();
        if (!out_.empty() && out_.back() != '\n') {
            out_.push_back('\n');
        }
    }

    void blank_line() {
        pending_space_ = false;
        trim_trailing_spaces();
        while (!out_.empty() && out_.back() == '\n') {
            if (out_.size() >= 2 && out_[out_.size() - 2] == '\n') {
                return;
            }
            break;
        }
        if (!out_.empty() && out_.back() != '\n') {
            out_.push_back('\n');
        }
        if (out_.empty() || out_.back() != '\n' || out_.size() < 2 || out_[out_.size() - 2] != '\n') {
            out_.push_back('\n');
        }
    }

    void heading_prefix(int level) {
        blank_line();
        if (format_ == OutputFormat::Markdown) {
            out_ += level == 1 ? "# " : "## ";
        }
    }

    void open_strong() {
        flush_pending_space();
        if (format_ == OutputFormat::Markdown) {
            out_ += "**";
        }
    }

    void close_strong() {
        if (format_ == OutputFormat::Markdown) {
            trim_trailing_spaces();
            out_ += "**";
        }
    }

    void open_emphasis() {
        flush_pending_space();
        if (format_ == OutputFormat::Markdown) {
            out_ += "*";
        }
    }

    void close_emphasis() {
        if (format_ == OutputFormat::Markdown) {
            trim_trailing_spaces();
            out_ += "*";
        }
    }

    void open_link(const std::string& href) {
        flush_pending_space();
        links_.push_back(href);
        if (format_ == OutputFormat::Markdown) {
            out_ += "[";
        }
    }

    void close_link() {
        if (links_.empty()) {
            return;
        }
        const std::string href = links_.back();
        links_.pop_back();
        if (href.empty()) {
            if (format_ == OutputFormat::Markdown) {
                out_ += "]";
            }
            return;
        }
        if (format_ == OutputFormat::Markdown) {
            trim_trailing_spaces();
            out_ += "](" + markdown_escape_url(href) + ")";
        } else {
            trim_trailing_spaces();
            out_ += " (" + href + ")";
        }
    }

    std::string finish() {
        pending_space_ = false;
        while (!links_.empty()) {
            close_link();
        }
        trim_trailing_spaces();
        while (!out_.empty() && out_.front() == '\n') {
            out_.erase(out_.begin());
        }
        while (!out_.empty() && out_.back() == '\n') {
            out_.pop_back();
        }
        if (!out_.empty()) {
            out_.push_back('\n');
        }
        return out_;
    }

   private:
    OutputFormat format_;
    std::string out_;
    std::vector<std::string> links_;
    bool pending_space_ = false;

    bool needs_space_before_text() const {
        return !out_.empty() && out_.back() != '\n' && out_.back() != ' ';
    }

    void flush_pending_space() {
        if (pending_space_ && needs_space_before_text()) {
            out_.push_back(' ');
        }
        pending_space_ = false;
    }

    void trim_trailing_spaces() {
        while (!out_.empty() && out_.back() == ' ') {
            out_.pop_back();
        }
    }
};

bool is_block_tag(const std::string& name) {
    return name == "p" || name == "div" || name == "section" || name == "article" || name == "main" ||
           name == "header" || name == "footer" || name == "nav" || name == "ul" || name == "ol" ||
           name == "blockquote" || name == "pre" || name == "table" || name == "tr";
}

}  // namespace

bool parse_output_format(const std::string& text, OutputFormat& out) {
    const std::string normalized = lower_ascii(text);
    if (normalized == "text" || normalized == "plain" || normalized == "plaintext") {
        out = OutputFormat::Text;
        return true;
    }
    if (normalized == "markdown" || normalized == "md") {
        out = OutputFormat::Markdown;
        return true;
    }
    return false;
}

const char* output_format_name(OutputFormat format) {
    switch (format) {
        case OutputFormat::Text:
            return "text";
        case OutputFormat::Markdown:
            return "markdown";
    }
    return "markdown";
}

std::string convert(const std::string& input, OutputFormat format) {
    const std::string cleaned = remove_ignored_blocks(input);
    Writer writer(format);
    static const std::regex tag_re(R"(<[^>]+>)");
    std::sregex_iterator it(cleaned.begin(), cleaned.end(), tag_re);
    const std::sregex_iterator end;
    size_t pos = 0;
    for (; it != end; ++it) {
        const std::smatch& match = *it;
        const size_t tag_pos = static_cast<size_t>(match.position());
        if (tag_pos > pos) {
            writer.append_text(cleaned.substr(pos, tag_pos - pos));
        }
        const std::string tag = match.str();
        const std::string name = tag_name(tag);
        const bool closing = is_closing_tag(tag);
        const bool self_closing = is_self_closing_tag(tag);
        if (name.empty() || name[0] == '!') {
            pos = tag_pos + tag.size();
            continue;
        }
        if (!closing) {
            if (name == "h1") {
                writer.heading_prefix(1);
            } else if (name == "h2") {
                writer.heading_prefix(2);
            } else if (name == "br") {
                writer.newline();
            } else if (name == "strong" || name == "b") {
                writer.open_strong();
            } else if (name == "em" || name == "i" || name == "italic") {
                writer.open_emphasis();
            } else if (name == "a") {
                writer.open_link(href_attribute(tag));
            } else if (name == "li") {
                writer.newline();
                if (format == OutputFormat::Markdown) {
                    writer.append_raw("- ");
                }
            } else if (is_block_tag(name)) {
                writer.blank_line();
            }
            if (self_closing && name == "a") {
                writer.close_link();
            }
        } else {
            if (name == "h1" || name == "h2") {
                writer.blank_line();
            } else if (name == "strong" || name == "b") {
                writer.close_strong();
            } else if (name == "em" || name == "i" || name == "italic") {
                writer.close_emphasis();
            } else if (name == "a") {
                writer.close_link();
            } else if (name == "li") {
                writer.newline();
            } else if (is_block_tag(name)) {
                writer.blank_line();
            }
        }
        pos = tag_pos + tag.size();
    }
    if (pos < cleaned.size()) {
        writer.append_text(cleaned.substr(pos));
    }
    return writer.finish();
}

}  // namespace pkchat::html
