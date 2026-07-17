#include "html/html.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <regex>
#include <string>
#include <vector>

#include "common.hpp"

namespace ainiux::html {
namespace {

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

bool starts_with(const std::string& text, const std::string& prefix) {
    return text.rfind(prefix, 0) == 0;
}

std::string trim_ascii(std::string text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    return text;
}

std::string first_word(const std::string& text) {
    const std::string stripped = trim_ascii(text);
    size_t end = 0;
    while (end < stripped.size() && !std::isspace(static_cast<unsigned char>(stripped[end]))) {
        ++end;
    }
    return stripped.substr(0, end);
}

std::string trim_capture_text(std::string text) {
    while (!text.empty() && (text.front() == '\n' || text.front() == '\r' || text.front() == ' ')) {
        text.erase(text.begin());
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
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
    return ascii_lower(match[1].str());
}

bool is_closing_tag(const std::string& tag) {
    return std::regex_search(tag, std::regex(R"(^<\s*/)", std::regex::icase));
}

bool is_self_closing_tag(const std::string& tag) {
    return std::regex_search(tag, std::regex(R"(/\s*>$)", std::regex::icase));
}

std::string attribute_value(const std::string& tag, const std::string& name) {
    std::smatch match;
    const std::regex attribute_re("\\b" + name + R"PK(\s*=\s*("([^"]*)"|'([^']*)'|([^\s>]+)))PK",
                                  std::regex::icase);
    if (!std::regex_search(tag, match, attribute_re)) {
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

int heading_level(const std::string& name) {
    if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') {
        return name[1] - '0';
    }
    return 0;
}

class Writer {
   public:
    explicit Writer(OutputFormat format) : format_(format) {}

    void begin_capture() { capture_depth_ += 1; }

    std::string end_capture() {
        if (capture_depth_ == 0) {
            return "";
        }
        --capture_depth_;
        return std::move(capture_buffer_);
    }

    bool capturing() const { return capture_depth_ > 0; }

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
            append_formatted_text(normalized);
        }
    }

    void append_raw(const std::string& text) {
        pending_space_ = false;
        write_output(text);
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
            write_output(std::string(static_cast<size_t>(level), '#') + " ");
        }
    }

    void open_strong() {
        flush_pending_space();
        if (format_ == OutputFormat::Markdown) {
            write_output("**");
        }
        strong_depth_ += 1;
    }

    void close_strong() {
        if (strong_depth_ > 0) {
            --strong_depth_;
        }
        if (format_ == OutputFormat::Markdown) {
            trim_trailing_spaces();
            write_output("**");
        }
    }

    void open_emphasis() {
        flush_pending_space();
        if (format_ == OutputFormat::Markdown) {
            write_output("*");
        }
        emphasis_depth_ += 1;
    }

    void close_emphasis() {
        if (emphasis_depth_ > 0) {
            --emphasis_depth_;
        }
        if (format_ == OutputFormat::Markdown) {
            trim_trailing_spaces();
            write_output("*");
        }
    }

    void open_strikethrough() {
        flush_pending_space();
        if (format_ == OutputFormat::Markdown) {
            write_output("~~");
        }
    }

    void close_strikethrough() {
        if (format_ == OutputFormat::Markdown) {
            trim_trailing_spaces();
            write_output("~~");
        }
    }

    void open_code() {
        flush_pending_space();
        if (format_ == OutputFormat::Markdown) {
            write_output("`");
        }
    }

    void close_code() {
        if (format_ == OutputFormat::Markdown) {
            trim_trailing_spaces();
            write_output("`");
        }
    }

    void open_underline() {
        flush_pending_space();
        if (format_ == OutputFormat::Markdown) {
            write_output("++");
        }
    }

    void close_underline() {
        if (format_ == OutputFormat::Markdown) {
            trim_trailing_spaces();
            write_output("++");
        }
    }

    void append_image(const std::string& src, const std::string& alt) {
        flush_pending_space();
        if (format_ == OutputFormat::Markdown) {
            write_output("![" + markdown_escape_text(alt) + "](" + markdown_escape_url(src) + ")");
        } else {
            write_output(alt);
            if (!src.empty()) {
                write_output(" (" + src + ")");
            }
        }
    }

    void open_link(const std::string& href) {
        flush_pending_space();
        links_.push_back(href);
        if (format_ == OutputFormat::Markdown) {
            write_output("[");
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
                write_output("]");
            }
            return;
        }
        if (format_ == OutputFormat::Markdown) {
            trim_trailing_spaces();
            write_output("](" + markdown_escape_url(href) + ")");
        } else {
            trim_trailing_spaces();
            write_output(" (" + href + ")");
        }
    }

    void append_horizontal_rule() {
        blank_line();
        if (format_ == OutputFormat::Markdown) {
            write_output("---");
        }
        newline();
        blank_line();
    }

    void append_fenced_code(const std::string& code, const std::string& language) {
        blank_line();
        if (format_ == OutputFormat::Markdown) {
            write_output("```" + language);
            newline();
            write_output(code);
            if (!code.empty() && code.back() != '\n') {
                newline();
            }
            write_output("```");
        } else {
            write_output(code);
            if (!code.empty() && code.back() != '\n') {
                newline();
            }
        }
        newline();
        blank_line();
    }

    void append_blockquote_lines(const std::string& text) {
        blank_line();
        const std::vector<std::string> lines = split_lines_crlf(trim_capture_text(text));
        for (const std::string& line : lines) {
            if (trim_ascii(line).empty()) {
                continue;
            }
            if (format_ == OutputFormat::Markdown) {
                write_output("> " + line);
            } else {
                write_output(line);
            }
            newline();
        }
        blank_line();
    }

    void append_markdown_table(const std::vector<std::vector<std::string>>& rows) {
        if (rows.empty()) {
            return;
        }
        blank_line();
        if (format_ != OutputFormat::Markdown) {
            for (const std::vector<std::string>& row : rows) {
                for (size_t i = 0; i < row.size(); ++i) {
                    if (i != 0) {
                        write_output("\t");
                    }
                    write_output(row[i]);
                }
                newline();
            }
            blank_line();
            return;
        }
        size_t columns = 0;
        for (const std::vector<std::string>& row : rows) {
            columns = std::max(columns, row.size());
        }
        for (size_t row_index = 0; row_index < rows.size(); ++row_index) {
            write_output("|");
            for (size_t col = 0; col < columns; ++col) {
                const std::string cell = col < rows[row_index].size() ? rows[row_index][col] : "";
                write_output(" " + cell + " |");
            }
            newline();
            if (row_index == 0) {
                write_output("|");
                for (size_t col = 0; col < columns; ++col) {
                    write_output(" --- |");
                }
                newline();
            }
        }
        blank_line();
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
    std::string capture_buffer_;
    std::vector<std::string> links_;
    bool pending_space_ = false;
    int capture_depth_ = 0;
    int strong_depth_ = 0;
    int emphasis_depth_ = 0;

    void append_formatted_text(const std::string& text) {
        write_output(format_ == OutputFormat::Markdown ? markdown_escape_text(text) : text);
    }

    void write_output(const std::string& text) {
        if (capture_depth_ > 0) {
            capture_buffer_ += text;
            return;
        }
        out_ += text;
    }

    bool needs_space_before_text() const {
        const std::string& target = capture_depth_ > 0 ? capture_buffer_ : out_;
        return !target.empty() && target.back() != '\n' && target.back() != ' ';
    }

    void flush_pending_space() {
        if (pending_space_ && needs_space_before_text()) {
            write_output(" ");
        }
        pending_space_ = false;
    }

    void trim_trailing_spaces() {
        std::string& target = capture_depth_ > 0 ? capture_buffer_ : out_;
        while (!target.empty() && target.back() == ' ') {
            target.pop_back();
        }
    }
};

bool is_block_tag(const std::string& name) {
    return name == "p" || name == "div" || name == "section" || name == "article" || name == "main" ||
           name == "header" || name == "footer" || name == "nav" || name == "ul" || name == "ol" ||
           name == "blockquote" || name == "pre" || name == "table" || name == "tr" || name == "thead" ||
           name == "tbody" || name == "tfoot" || name == "h1" || name == "h2" || name == "h3" ||
           name == "h4" || name == "h5" || name == "h6";
}

struct TableState {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> current_row;
};

}  // namespace

bool is_valid_utf8(const std::string& input, size_t* error_offset) {
    auto fail = [&](size_t offset) {
        if (error_offset != nullptr) {
            *error_offset = offset;
        }
        return false;
    };
    auto is_continuation = [&](size_t offset) {
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
            continue;
        }
        if (ch >= 0xC2 && ch <= 0xDF) {
            if (!is_continuation(i + 1)) {
                return fail(i);
            }
            i += 2;
            continue;
        }
        if (ch == 0xE0) {
            if (i + 2 >= input.size()) {
                return fail(i);
            }
            const unsigned char b1 = static_cast<unsigned char>(input[i + 1]);
            if (b1 < 0xA0 || b1 > 0xBF || !is_continuation(i + 2)) {
                return fail(i);
            }
            i += 3;
            continue;
        }
        if ((ch >= 0xE1 && ch <= 0xEC) || (ch >= 0xEE && ch <= 0xEF)) {
            if (!is_continuation(i + 1) || !is_continuation(i + 2)) {
                return fail(i);
            }
            i += 3;
            continue;
        }
        if (ch == 0xED) {
            if (i + 2 >= input.size()) {
                return fail(i);
            }
            const unsigned char b1 = static_cast<unsigned char>(input[i + 1]);
            if (b1 < 0x80 || b1 > 0x9F || !is_continuation(i + 2)) {
                return fail(i);
            }
            i += 3;
            continue;
        }
        if (ch == 0xF0) {
            if (i + 3 >= input.size()) {
                return fail(i);
            }
            const unsigned char b1 = static_cast<unsigned char>(input[i + 1]);
            if (b1 < 0x90 || b1 > 0xBF || !is_continuation(i + 2) || !is_continuation(i + 3)) {
                return fail(i);
            }
            i += 4;
            continue;
        }
        if (ch >= 0xF1 && ch <= 0xF3) {
            if (!is_continuation(i + 1) || !is_continuation(i + 2) || !is_continuation(i + 3)) {
                return fail(i);
            }
            i += 4;
            continue;
        }
        if (ch == 0xF4) {
            if (i + 3 >= input.size()) {
                return fail(i);
            }
            const unsigned char b1 = static_cast<unsigned char>(input[i + 1]);
            if (b1 < 0x80 || b1 > 0x8F || !is_continuation(i + 2) || !is_continuation(i + 3)) {
                return fail(i);
            }
            i += 4;
            continue;
        }
        return fail(i);
    }
    if (error_offset != nullptr) {
        *error_offset = input.size();
    }
    return true;
}

bool parse_output_format(const std::string& text, OutputFormat& out) {
    const std::string normalized = ascii_lower(text);
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
    struct ListState {
        bool ordered = false;
        size_t next_number = 1;
    };
    std::vector<ListState> lists;
    std::vector<TableState> tables;
    size_t pre_depth = 0;
    std::string pre_language;
    size_t blockquote_depth = 0;
    size_t code_depth = 0;
    size_t cell_depth = 0;
    size_t pos = 0;
    for (; it != end; ++it) {
        const std::smatch& match = *it;
        const size_t tag_pos = static_cast<size_t>(match.position());
        if (tag_pos > pos) {
            const std::string text = cleaned.substr(pos, tag_pos - pos);
            if (pre_depth > 0) {
                writer.append_raw(text);
            } else {
                writer.append_text(text);
            }
        }
        const std::string tag = match.str();
        const std::string name = tag_name(tag);
        const bool closing = is_closing_tag(tag);
        const bool self_closing = is_self_closing_tag(tag);
        if (name.empty() || name[0] == '!') {
            pos = tag_pos + tag.size();
            continue;
        }
        if (pre_depth > 0) {
            if (!closing && name == "code" && pre_language.empty()) {
                const std::string class_name = first_word(attribute_value(tag, "class"));
                if (starts_with(class_name, "language-")) {
                    pre_language = class_name.substr(9);
                } else {
                    pre_language = class_name;
                }
            } else if (closing && name == "pre") {
                --pre_depth;
                std::string code = writer.end_capture();
                while (!code.empty() && (code.back() == '\n' || code.back() == '\r')) {
                    code.pop_back();
                }
                writer.append_fenced_code(code, pre_language);
                pre_language.clear();
            }
            pos = tag_pos + tag.size();
            continue;
        }
        if (!closing) {
            const int level = heading_level(name);
            if (level != 0) {
                writer.heading_prefix(level);
            } else if (name == "br") {
                writer.newline();
            } else if (name == "hr") {
                writer.append_horizontal_rule();
            } else if (name == "strong" || name == "b") {
                writer.open_strong();
            } else if (name == "em" || name == "i" || name == "italic") {
                writer.open_emphasis();
            } else if (name == "del" || name == "s" || name == "strike") {
                writer.open_strikethrough();
            } else if (name == "code") {
                if (code_depth == 0) {
                    writer.open_code();
                }
                ++code_depth;
            } else if (name == "pre") {
                writer.begin_capture();
                ++pre_depth;
                pre_language = first_word(attribute_value(tag, "class"));
                if (starts_with(pre_language, "language-")) {
                    pre_language = pre_language.substr(9);
                }
            } else if (name == "blockquote") {
                if (blockquote_depth == 0) {
                    writer.begin_capture();
                }
                ++blockquote_depth;
            } else if (name == "u") {
                writer.open_underline();
            } else if (name == "a") {
                writer.open_link(attribute_value(tag, "href"));
            } else if (name == "img") {
                writer.append_image(attribute_value(tag, "src"), attribute_value(tag, "alt"));
            } else if (name == "table") {
                writer.blank_line();
                tables.push_back(TableState{});
            } else if (name == "tr") {
                if (!tables.empty()) {
                    if (cell_depth > 0) {
                        tables.back().current_row.push_back(trim_capture_text(writer.end_capture()));
                        cell_depth = 0;
                    }
                    if (!tables.back().current_row.empty()) {
                        tables.back().rows.push_back(tables.back().current_row);
                    }
                    tables.back().current_row.clear();
                }
            } else if (name == "th" || name == "td") {
                if (!tables.empty()) {
                    if (cell_depth > 0) {
                        tables.back().current_row.push_back(trim_capture_text(writer.end_capture()));
                        --cell_depth;
                    }
                    writer.begin_capture();
                    ++cell_depth;
                }
            } else if (name == "ul" || name == "ol") {
                writer.blank_line();
                lists.push_back({name == "ol", 1});
            } else if (name == "li") {
                writer.newline();
                if (format == OutputFormat::Markdown) {
                    writer.append_raw(std::string(lists.empty() ? 0 : (lists.size() - 1) * 2, ' '));
                    if (!lists.empty() && lists.back().ordered) {
                        writer.append_raw(std::to_string(lists.back().next_number++) + ". ");
                    } else {
                        writer.append_raw("- ");
                    }
                }
            } else if (is_block_tag(name) && blockquote_depth == 0) {
                writer.blank_line();
            }
            if (self_closing && name == "a") {
                writer.close_link();
            }
        } else {
            if (heading_level(name) != 0) {
                writer.blank_line();
            } else if (name == "strong" || name == "b") {
                writer.close_strong();
            } else if (name == "em" || name == "i" || name == "italic") {
                writer.close_emphasis();
            } else if (name == "del" || name == "s" || name == "strike") {
                writer.close_strikethrough();
            } else if (name == "code") {
                if (code_depth > 0) {
                    --code_depth;
                }
                if (code_depth == 0) {
                    writer.close_code();
                }
            } else if (name == "blockquote") {
                if (blockquote_depth > 0) {
                    --blockquote_depth;
                }
                if (blockquote_depth == 0) {
                    writer.append_blockquote_lines(writer.end_capture());
                }
            } else if (name == "p" && blockquote_depth > 0) {
                writer.append_raw("\n");
            } else if (name == "u") {
                writer.close_underline();
            } else if (name == "a") {
                writer.close_link();
            } else if (name == "th" || name == "td") {
                if (!tables.empty() && cell_depth > 0) {
                    --cell_depth;
                    tables.back().current_row.push_back(trim_capture_text(writer.end_capture()));
                }
            } else if (name == "tr") {
                if (!tables.empty() && !tables.back().current_row.empty()) {
                    tables.back().rows.push_back(tables.back().current_row);
                    tables.back().current_row.clear();
                }
            } else if (name == "table") {
                if (!tables.empty()) {
                    writer.append_markdown_table(tables.back().rows);
                    tables.pop_back();
                }
            } else if (name == "li") {
                writer.newline();
            } else if (name == "ul" || name == "ol") {
                if (!lists.empty()) {
                    lists.pop_back();
                }
                writer.blank_line();
            } else if (is_block_tag(name) && blockquote_depth == 0) {
                writer.blank_line();
            }
        }
        pos = tag_pos + tag.size();
    }
    if (pos < cleaned.size()) {
        const std::string text = cleaned.substr(pos);
        if (pre_depth > 0) {
            writer.append_raw(text);
        } else {
            writer.append_text(text);
        }
    }
    if (pre_depth > 0) {
        pre_depth = 0;
        std::string code = writer.end_capture();
        while (!code.empty() && (code.back() == '\n' || code.back() == '\r')) {
            code.pop_back();
        }
        writer.append_fenced_code(code, pre_language);
    }
    if (blockquote_depth > 0) {
        blockquote_depth = 0;
        writer.append_blockquote_lines(writer.end_capture());
    }
    if (!tables.empty()) {
        if (!tables.back().current_row.empty()) {
            tables.back().rows.push_back(tables.back().current_row);
            tables.back().current_row.clear();
        }
        writer.append_markdown_table(tables.back().rows);
        tables.clear();
    }
    return writer.finish();
}

}  // namespace ainiux::html
