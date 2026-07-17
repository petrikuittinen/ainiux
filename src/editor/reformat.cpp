#include "editor/reformat.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace ainiux::editor {
namespace {

enum class Profile {
    Unsupported,
    Brace,
    Ruby,
    Bash,
    Markup,
    Sql,
    Topology,
    Assembly,
};

Profile profile_for(highlight::Language language) {
    using highlight::Language;
    switch (language) {
        case Language::C:
        case Language::Cpp:
        case Language::CSharp:
        case Language::Java:
        case Language::JavaScript:
        case Language::TypeScript:
        case Language::Css:
        case Language::Json:
        case Language::Php:
        case Language::Perl:
        case Language::Rust:
        case Language::Go:
        case Language::PowerShell:
            return Profile::Brace;
        case Language::Ruby:
            return Profile::Ruby;
        case Language::Bash:
            return Profile::Bash;
        case Language::Html:
        case Language::HtmlOnly:
        case Language::Xml:
            return Profile::Markup;
        case Language::Sql:
            return Profile::Sql;
        case Language::Python:
        case Language::Yaml:
        case Language::Markdown:
        case Language::Toml:
        case Language::Ini:
            return Profile::Topology;
        case Language::Assembly:
            return Profile::Assembly;
        case Language::Text:
            return Profile::Unsupported;
    }
    return Profile::Unsupported;
}

size_t leading_bytes(const std::string& line) {
    size_t count = 0;
    while (count < line.size() && (line[count] == ' ' || line[count] == '\t')) {
        ++count;
    }
    return count;
}

size_t leading_column(const std::string& line, size_t tab_width) {
    size_t column = 0;
    for (size_t i = 0; i < leading_bytes(line); ++i) {
        if (line[i] == '\t') {
            column += tab_width - (column % tab_width);
        } else {
            ++column;
        }
    }
    return column;
}

bool blank_line(const std::string& line) {
    return leading_bytes(line) == line.size();
}

std::string ascii_lower_copy(std::string value) {
    for (char& ch : value) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte >= 'A' && byte <= 'Z') {
            ch = static_cast<char>(byte - 'A' + 'a');
        }
    }
    return value;
}

std::vector<std::string> words(const std::string& code) {
    std::vector<std::string> result;
    size_t position = 0;
    while (position < code.size()) {
        const unsigned char byte = static_cast<unsigned char>(code[position]);
        const bool ascii_letter = (byte >= 'A' && byte <= 'Z') ||
                                  (byte >= 'a' && byte <= 'z');
        if (!(ascii_letter || byte == '_')) {
            ++position;
            continue;
        }
        const size_t start = position++;
        while (position < code.size()) {
            const unsigned char next = static_cast<unsigned char>(code[position]);
            const bool ascii_alnum = (next >= 'A' && next <= 'Z') ||
                                     (next >= 'a' && next <= 'z') ||
                                     (next >= '0' && next <= '9');
            if (!(ascii_alnum || next == '_')) {
                break;
            }
            ++position;
        }
        result.push_back(ascii_lower_copy(code.substr(start, position - start)));
    }
    return result;
}

bool contains(const std::set<std::string>& values, const std::string& value) {
    return values.find(value) != values.end();
}

std::string structural_code(const std::string& line,
                            const highlight::HighlightedLine& highlighted) {
    std::string code = line;
    for (const highlight::Span& span : highlighted.spans) {
        if (span.role != highlight::TokenRole::Comment &&
            span.role != highlight::TokenRole::String) {
            continue;
        }
        const size_t end = std::min(span.end, code.size());
        for (size_t i = std::min(span.start, end); i < end; ++i) {
            code[i] = ' ';
        }
    }
    return code;
}

bool protected_multiline(const highlight::LineState& incoming) {
    using Block = highlight::LineState::Block;
    switch (incoming.block) {
        case Block::Fence:
        case Block::BlockComment:
        case Block::TripleSingleString:
        case Block::TripleDoubleString:
        case Block::TemplateString:
        case Block::RawString:
        case Block::Heredoc:
        case Block::HereString:
        case Block::IndentedString:
        case Block::HtmlComment:
        case Block::CData:
        case Block::LineDelimitedComment:
            return true;
        case Block::None:
        case Block::Tag:
        case Block::Script:
        case Block::Style:
            return false;
    }
    return false;
}

size_t leading_closers(const std::string& code) {
    size_t position = leading_bytes(code);
    size_t count = 0;
    while (position < code.size()) {
        const char ch = code[position];
        if (ch != '}' && ch != ']' && ch != ')') {
            break;
        }
        ++count;
        ++position;
        while (position < code.size() && (code[position] == ' ' || code[position] == '\t')) {
            ++position;
        }
    }
    return count;
}

void brace_counts(const std::string& code, size_t& opens, size_t& closes) {
    opens = 0;
    closes = 0;
    for (char ch : code) {
        if (ch == '{' || ch == '[' || ch == '(') {
            ++opens;
        } else if (ch == '}' || ch == ']' || ch == ')') {
            ++closes;
        }
    }
}

bool markup_void_tag(std::string_view name) {
    static const std::set<std::string> names = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr",
    };
    return names.find(std::string(name)) != names.end();
}

void markup_counts(const std::string& code,
                   size_t& leading_closes,
                   size_t& opens,
                   size_t& closes) {
    leading_closes = 0;
    opens = 0;
    closes = 0;
    bool saw_content = false;
    size_t position = 0;
    while ((position = code.find('<', position)) != std::string::npos) {
        if (position + 1 >= code.size() || code.compare(position, 4, "<!--") == 0 ||
            code[position + 1] == '!' || code[position + 1] == '?') {
            ++position;
            continue;
        }
        const bool closing = code[position + 1] == '/';
        size_t name_start = position + (closing ? 2 : 1);
        while (name_start < code.size() && std::isspace(static_cast<unsigned char>(code[name_start]))) {
            ++name_start;
        }
        size_t name_end = name_start;
        while (name_end < code.size()) {
            const unsigned char ch = static_cast<unsigned char>(code[name_end]);
            if (!(std::isalnum(ch) || ch == ':' || ch == '-' || ch == '_')) {
                break;
            }
            ++name_end;
        }
        if (name_end == name_start) {
            ++position;
            continue;
        }
        const size_t tag_end = code.find('>', name_end);
        if (tag_end == std::string::npos) {
            break;
        }
        const std::string name = ascii_lower_copy(code.substr(name_start, name_end - name_start));
        size_t before_end = tag_end;
        while (before_end > name_end && std::isspace(static_cast<unsigned char>(code[before_end - 1]))) {
            --before_end;
        }
        const bool self_closing = before_end > name_end && code[before_end - 1] == '/';
        if (closing) {
            ++closes;
            if (!saw_content) {
                ++leading_closes;
            }
        } else if (!self_closing && !markup_void_tag(name)) {
            ++opens;
        }
        saw_content = true;
        position = tag_end + 1;
    }
}

std::string indentation(size_t depth,
                        size_t tab_width,
                        TabStyle style,
                        bool force_spaces) {
    if (!force_spaces && style == TabStyle::Tab) {
        return std::string(depth, '\t');
    }
    if (depth > std::numeric_limits<size_t>::max() / tab_width) {
        throw std::length_error("formatted indentation is too large");
    }
    return std::string(depth * tab_width, ' ');
}

struct StructuralState {
    size_t depth = 0;
    size_t embedded_depth = 0;
    std::vector<size_t> topology_columns{0};
    bool topology_initialized = false;
};

size_t topology_depth(StructuralState& state, size_t column) {
    if (!state.topology_initialized) {
        state.topology_columns.front() = column;
        state.topology_initialized = true;
        return 0;
    }
    if (column < state.topology_columns.front()) {
        state.topology_columns.assign(1, column);
        return 0;
    }
    while (state.topology_columns.size() > 1 && column < state.topology_columns.back()) {
        state.topology_columns.pop_back();
    }
    if (column > state.topology_columns.back()) {
        state.topology_columns.push_back(column);
    }
    return state.topology_columns.size() - 1;
}

size_t desired_depth(Profile profile,
                     const std::string& original,
                     const std::string& code,
                     StructuralState& state,
                     size_t tab_width,
                     bool& middle_branch) {
    middle_branch = false;
    if (profile == Profile::Topology) {
        return topology_depth(state, leading_column(original, tab_width));
    }
    if (profile == Profile::Assembly) {
        const size_t start = leading_bytes(code);
        const size_t colon = code.find(':', start);
        const size_t whitespace = code.find_first_of(" \t", start);
        return colon != std::string::npos &&
                       (whitespace == std::string::npos || colon < whitespace)
                   ? 0
                   : 1;
    }
    if (profile == Profile::Brace) {
        return state.depth >= leading_closers(code) ? state.depth - leading_closers(code) : 0;
    }
    if (profile == Profile::Markup) {
        size_t leading = 0;
        size_t opens = 0;
        size_t closes = 0;
        markup_counts(code, leading, opens, closes);
        (void)opens;
        (void)closes;
        return state.depth >= leading ? state.depth - leading : 0;
    }

    const std::vector<std::string> tokens = words(code);
    if (tokens.empty()) {
        return state.depth;
    }
    static const std::set<std::string> ruby_closers = {
        "end", "else", "elsif", "when", "rescue", "ensure",
    };
    static const std::set<std::string> ruby_middle = {
        "else", "elsif", "when", "rescue", "ensure",
    };
    static const std::set<std::string> bash_closers = {
        "fi", "done", "esac", "else", "elif",
    };
    static const std::set<std::string> bash_middle = {"else", "elif"};
    static const std::set<std::string> sql_closers = {"end"};
    static const std::set<std::string> sql_middle;
    const std::set<std::string>* closers = &sql_closers;
    const std::set<std::string>* middles = &sql_middle;
    if (profile == Profile::Ruby) {
        closers = &ruby_closers;
        middles = &ruby_middle;
    } else if (profile == Profile::Bash) {
        closers = &bash_closers;
        middles = &bash_middle;
    }
    const bool closes = contains(*closers, tokens.front());
    middle_branch = contains(*middles, tokens.front());
    return closes && state.depth > 0 ? state.depth - 1 : state.depth;
}

void advance_state(Profile profile,
                   const std::string& code,
                   StructuralState& state,
                   bool middle_branch) {
    if (profile == Profile::Topology || profile == Profile::Assembly) {
        return;
    }
    if (profile == Profile::Brace) {
        size_t opens = 0;
        size_t closes = 0;
        brace_counts(code, opens, closes);
        state.depth = opens >= closes ? state.depth + (opens - closes)
                                     : (state.depth >= closes - opens ? state.depth - (closes - opens) : 0);
        return;
    }
    if (profile == Profile::Markup) {
        size_t leading = 0;
        size_t opens = 0;
        size_t closes = 0;
        markup_counts(code, leading, opens, closes);
        (void)leading;
        state.depth = opens >= closes ? state.depth + (opens - closes)
                                     : (state.depth >= closes - opens ? state.depth - (closes - opens) : 0);
        return;
    }

    const std::vector<std::string> tokens = words(code);
    if (tokens.empty()) {
        return;
    }
    static const std::set<std::string> ruby_openers = {
        "begin", "case", "class", "def", "do", "for", "if", "module", "unless", "until", "while",
    };
    static const std::set<std::string> bash_openers = {
        "case", "for", "if", "select", "until", "while",
    };
    static const std::set<std::string> sql_openers = {"begin", "case", "loop"};
    const std::set<std::string>* openers = &sql_openers;
    if (profile == Profile::Ruby) {
        openers = &ruby_openers;
    } else if (profile == Profile::Bash) {
        openers = &bash_openers;
    }
    const std::string& first = tokens.front();
    bool closes = first == "end" || first == "fi" || first == "done" || first == "esac" ||
                  middle_branch;
    if (closes && state.depth > 0) {
        --state.depth;
    }
    const bool ruby_do = profile == Profile::Ruby &&
                         std::find(tokens.begin(), tokens.end(), "do") != tokens.end();
    const bool sql_case = profile == Profile::Sql &&
                          std::find(tokens.begin(), tokens.end(), "case") != tokens.end();
    if (contains(*openers, first) || ruby_do || sql_case || middle_branch) {
        ++state.depth;
    }
}

}  // namespace

ReformatResult reformat_indentation(const ReformatRequest& request,
                                    const runtime::CancellationToken& cancellation) {
    ReformatResult result;
    result.first_line = request.first_line;
    result.last_line = request.last_line;
    const Profile profile = profile_for(request.language);
    if (profile == Profile::Unsupported) {
        result.error = {ErrorCode::UnsupportedFeature,
                        "Cannot reformat text mode. Choose a programming language with /mode first"};
        return result;
    }
    if (request.tab_width < 1 || request.tab_width > kMaxTabWidth) {
        result.error = {ErrorCode::BadArgs, "Reformat tab width must be between 1 and 32"};
        return result;
    }
    if (cancellation.cancelled()) {
        result.error = {ErrorCode::Cancelled, "Reformat cancelled"};
        return result;
    }

    try {
        const std::vector<std::string> lines = highlight::split_lines(request.content);
        if (lines.empty() || request.first_line >= lines.size() ||
            request.last_line < request.first_line || request.last_line >= lines.size()) {
            result.error = {ErrorCode::BadArgs, "Reformat line range is outside the active buffer"};
            return result;
        }
        std::vector<std::string> formatted = lines;
        StructuralState structural;
        highlight::LineState lexical;
        bool warned_limited = false;
        bool classification_unsafe = false;
        for (size_t line_index = 0; line_index <= request.last_line; ++line_index) {
            if (cancellation.cancelled()) {
                result.error = {ErrorCode::Cancelled, "Reformat cancelled"};
                return result;
            }
            const std::string& line = lines[line_index];
            const highlight::LineState incoming = lexical;
            const highlight::HighlightedLine highlighted =
                highlight::highlight_line(request.language, line, lexical,
                                          std::max(line.size(), highlight::kMaximumHighlightedLineBytes));
            lexical = highlighted.next_state;
            if (highlighted.work_limited) {
                warned_limited = true;
                classification_unsafe = true;
            }
            const std::string code = structural_code(line, highlighted);
            if (blank_line(line)) {
                continue;
            }

            bool middle_branch = false;
            using Block = highlight::LineState::Block;
            const bool embedded_markup =
                profile == Profile::Markup &&
                (incoming.block == Block::Script || incoming.block == Block::Style) &&
                code.find('<') == std::string::npos;
            const size_t depth = embedded_markup
                                     ? structural.depth +
                                           (structural.embedded_depth >= leading_closers(code)
                                                ? structural.embedded_depth - leading_closers(code)
                                                : 0)
                                     : desired_depth(profile,
                                                     line,
                                                     code,
                                                     structural,
                                                     request.tab_width,
                                                     middle_branch);
            const bool preserve = protected_multiline(incoming) || classification_unsafe;
            if (line_index >= request.first_line && !preserve) {
                const bool force_spaces = request.language == highlight::Language::Yaml;
                std::string replacement_indent =
                    indentation(depth, request.tab_width, request.tab_style, force_spaces);
                formatted[line_index] = std::move(replacement_indent) + line.substr(leading_bytes(line));
            }
            if (embedded_markup) {
                size_t opens = 0;
                size_t closes = 0;
                brace_counts(code, opens, closes);
                structural.embedded_depth =
                    opens >= closes
                        ? structural.embedded_depth + (opens - closes)
                        : (structural.embedded_depth >= closes - opens
                               ? structural.embedded_depth - (closes - opens)
                               : 0);
            } else {
                if (profile == Profile::Markup) {
                    structural.embedded_depth = 0;
                }
                advance_state(profile, code, structural, middle_branch);
            }
        }

        size_t replace_start = 0;
        for (size_t i = 0; i < request.first_line; ++i) {
            replace_start += lines[i].size() + 1;
        }
        size_t replace_end = replace_start;
        for (size_t i = request.first_line; i <= request.last_line; ++i) {
            replace_end += lines[i].size();
            if (i + 1 < lines.size()) {
                ++replace_end;
            }
        }
        std::string replacement;
        for (size_t i = request.first_line; i <= request.last_line; ++i) {
            replacement += formatted[i];
            if (i + 1 < lines.size()) {
                replacement.push_back('\n');
            }
        }
        result.replace_start = replace_start;
        result.replace_count = replace_end - replace_start;
        result.replacement = std::move(replacement);
        result.changed = request.content.compare(result.replace_start,
                                                 result.replace_count,
                                                 result.replacement) != 0;
        if (warned_limited) {
            result.warning = "Some pathological long lines were preserved because they could not be classified safely";
        }
    } catch (const std::bad_alloc&) {
        result.error = {ErrorCode::Internal, "Not enough memory to reformat the selected code"};
    } catch (const std::length_error&) {
        result.error = {ErrorCode::Internal, "Reformatted indentation is too large for this platform"};
    }
    return result;
}

Error build_reformat_request(const EditorState& state, bool all, ReformatRequest& request) {
    if (!all && !state.selection.has_range()) {
        return {ErrorCode::BadArgs,
                "/reformat requires a selected block; use /reformat-all for the entire buffer"};
    }
    request.content = state.text.str();
    request.language = state.language;
    request.tab_width = state.tab_width;
    request.tab_style = state.tab_style;
    request.first_line = all ? 0 : state.text.line_for_offset(state.selection.start());
    request.last_line = all ? state.text.line_count() - 1
                            : state.text.line_for_offset(state.selection.end());
    if (!all && state.selection.end() > state.selection.start() &&
        state.selection.end() == state.text.line_start(request.last_line) &&
        request.last_line > request.first_line) {
        --request.last_line;
    }
    return ok_error();
}

Error apply_reformat_result(EditorState& state, const ReformatResult& result, bool all) {
    if (!result.error.ok()) {
        return result.error;
    }
    const size_t cursor_line = state.text.line_for_offset(state.cursor);
    const size_t cursor_column = state.text.display_column_for_offset(state.cursor, state.tab_width);
    const bool reverse = state.selection.has_range() && state.selection.anchor > state.selection.active;
    if (result.changed) {
        Error error = state.replace(result.replace_start, result.replace_count, result.replacement);
        if (!error.ok()) {
            return error;
        }
    }
    if (all) {
        state.cursor = state.text.offset_for_line_column(
            std::min(cursor_line, state.text.line_count() - 1), cursor_column, state.tab_width);
        state.preferred_column = state.text.display_column_for_offset(state.cursor, state.tab_width);
        state.selection.clear(state.cursor);
    } else {
        const size_t start = state.text.line_start(result.first_line);
        const size_t end = result.last_line + 1 < state.text.line_count()
                               ? state.text.line_start(result.last_line + 1)
                               : state.text.size();
        state.selection.anchor = reverse ? end : start;
        state.selection.active = reverse ? start : end;
        state.cursor = state.selection.active;
        state.preferred_column = state.text.display_column_for_offset(state.cursor, state.tab_width);
    }
    return ok_error();
}

void start_reformat_job(ReformatRequest request,
                        runtime::EventQueue<ReformatEvent>& events,
                        runtime::JobHandle& job) {
    job.start([request = std::move(request), &events](runtime::CancellationToken token) mutable {
        ReformatEvent event;
        try {
            event.result = reformat_indentation(request, token);
        } catch (const std::bad_alloc&) {
            event.result.error = {ErrorCode::Internal,
                                  "Not enough memory to run the reformat job"};
        } catch (...) {
            event.result.error = {ErrorCode::Internal,
                                  "Unexpected failure in the reformat job"};
        }
        events.push(std::move(event));
    });
}

}  // namespace ainiux::editor
