#include "editor/editor.hpp"

#include "common.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <unistd.h>

namespace ainiux::editor {

namespace {

constexpr size_t kIndentDetectionLines = 20;

}  // namespace

const char* tab_style_name(TabStyle style) {
    return style == TabStyle::Tab ? "tab" : "spaces";
}

const char* linebreak_name(LineBreak linebreak) {
    switch (linebreak) {
        case LineBreak::Cr:
            return "cr";
        case LineBreak::Crlf:
            return "crlf";
        case LineBreak::Lf:
            return "lf";
    }
    return "lf";
}

bool parse_tab_style(const std::string& value, TabStyle& out) {
    if (value == "spaces") {
        out = TabStyle::Spaces;
        return true;
    }
    if (value == "tab") {
        out = TabStyle::Tab;
        return true;
    }
    return false;
}

bool parse_linebreak(const std::string& value, LineBreak& out) {
    if (value == "lf") {
        out = LineBreak::Lf;
        return true;
    }
    if (value == "cr") {
        out = LineBreak::Cr;
        return true;
    }
    if (value == "crlf") {
        out = LineBreak::Crlf;
        return true;
    }
    return false;
}

IndentationDetection detect_indentation(const std::string& text,
                                        size_t fallback_width,
                                        TabStyle fallback_style) {
    IndentationDetection result;
    result.tab_width = std::max<size_t>(1, std::min(fallback_width, kMaxTabWidth));
    result.tab_style = fallback_style;

    std::array<size_t, kMaxTabWidth + 1> differences{};
    size_t difference_count = 0;
    size_t space_indented_lines = 0;
    size_t tab_indented_lines = 0;
    size_t mixed_indented_lines = 0;
    size_t previous_space_indent = 0;
    bool have_previous_space_line = false;
    size_t start = 0;
    for (size_t line_number = 0;
         line_number < kIndentDetectionLines && start <= text.size();
         ++line_number) {
        const size_t newline = text.find('\n', start);
        const size_t end = newline == std::string::npos ? text.size() : newline;
        size_t position = start;
        size_t spaces = 0;
        size_t tabs = 0;
        while (position < end && (text[position] == ' ' || text[position] == '\t')) {
            if (text[position] == ' ') {
                ++spaces;
            } else {
                ++tabs;
            }
            ++position;
        }
        if (position < end) {
            if (spaces > 0 && tabs == 0) {
                ++space_indented_lines;
            } else if (tabs > 0 && spaces == 0) {
                ++tab_indented_lines;
            } else if (spaces > 0 && tabs > 0) {
                ++mixed_indented_lines;
            }

            if (tabs == 0) {
                if (have_previous_space_line) {
                    const size_t difference = spaces >= previous_space_indent
                                                  ? spaces - previous_space_indent
                                                  : previous_space_indent - spaces;
                    if (difference > 0 && difference <= kMaxTabWidth) {
                        ++differences[difference];
                        ++difference_count;
                    }
                }
                previous_space_indent = spaces;
                have_previous_space_line = true;
            } else {
                have_previous_space_line = false;
            }
        }
        if (newline == std::string::npos) {
            break;
        }
        start = newline + 1;
    }

    if (space_indented_lines > 0 && tab_indented_lines == 0 && mixed_indented_lines == 0) {
        result.tab_style = TabStyle::Spaces;
        result.tab_style_detected = true;
    } else if (tab_indented_lines > 0 && space_indented_lines == 0 &&
               mixed_indented_lines == 0) {
        result.tab_style = TabStyle::Tab;
        result.tab_style_detected = true;
    }

    if (!result.tab_style_detected || result.tab_style != TabStyle::Spaces ||
        difference_count == 0) {
        return result;
    }
    if (differences[1] == difference_count) {
        result.tab_width = 1;
        result.tab_width_detected = true;
        return result;
    }

    size_t best_width = 0;
    size_t best_score = 0;
    size_t best_exact = 0;
    bool ambiguous = false;
    for (size_t candidate = 2; candidate <= kMaxTabWidth; ++candidate) {
        size_t score = 0;
        for (size_t difference = candidate; difference <= kMaxTabWidth;
             difference += candidate) {
            score += differences[difference];
        }
        const size_t exact = differences[candidate];
        if (score > best_score || (score == best_score && exact > best_exact)) {
            best_width = candidate;
            best_score = score;
            best_exact = exact;
            ambiguous = false;
        } else if (score > 0 && score == best_score && exact == best_exact) {
            ambiguous = true;
        }
    }
    if (best_width > 0 && !ambiguous && best_score * 3 >= difference_count * 2) {
        result.tab_width = best_width;
        result.tab_width_detected = true;
    }
    return result;
}

Error load_file(const std::string& path, PieceTable& out) {
    return load_file(path, EditorSettings{}, out);
}

Error check_load_file_size(const std::string& path, const EditorSettings& settings, FileLoadCheck& check) {
    const std::string resolved = expand_user_path(path);
    check = {};
    std::error_code filesystem_error;
    const std::filesystem::file_status status = std::filesystem::status(resolved, filesystem_error);
    if (filesystem_error) {
        if (filesystem_error == std::errc::no_such_file_or_directory) {
            return {ErrorCode::FileRead, "file not found: " + resolved};
        }
        return {ErrorCode::FileRead, "could not access file: " + resolved};
    }
    if (!std::filesystem::exists(status)) {
        return {ErrorCode::FileRead, "file not found: " + resolved};
    }
    if (!std::filesystem::is_regular_file(status)) {
        return {ErrorCode::FileRead, "editor path is not a regular file: " + resolved};
    }
    const std::uintmax_t file_size = std::filesystem::file_size(resolved, filesystem_error);
    if (filesystem_error) {
        return {ErrorCode::FileRead, "could not determine editor file size before loading: " + resolved};
    }
    if (settings.file_size_limit >= 0 &&
        file_size > static_cast<std::uintmax_t>(settings.file_size_limit)) {
        return {ErrorCode::FileRead,
                "editor file exceeds FILE_SIZE_LIMIT of " +
                    std::to_string(settings.file_size_limit) + " bytes: " + resolved +
                    " (" + std::to_string(file_size) + " bytes)"};
    }
    check.size = file_size;
    check.should_warn = settings.huge_file_size_warning > 0 &&
                        file_size >= static_cast<std::uintmax_t>(settings.huge_file_size_warning);
    return ok_error();
}

Error load_file(const std::string& path, const EditorSettings& settings, PieceTable& out) {
    LoadedFile loaded;
    Error err = load_file(path, settings, loaded);
    if (err.ok()) {
        out = std::move(loaded.text);
    }
    return err;
}

Error load_file(const std::string& path, const EditorSettings& settings, LoadedFile& out) {
    out = LoadedFile{};
    out.tab_width = settings.tab_width;
    out.tab_style = settings.tab_style;
    const std::string resolved = expand_user_path(path);
    FileLoadCheck check;
    Error err = check_load_file_size(resolved, settings, check);
    if (!err.ok()) {
        return err;
    }
    if (check.size > static_cast<std::uintmax_t>(std::numeric_limits<size_t>::max())) {
        return {ErrorCode::FileRead,
                "editor file is too large for this platform address space: " + resolved +
                    " (" + std::to_string(check.size) + " bytes)"};
    }

    std::ifstream in(resolved, std::ios::binary);
    if (!in) {
        return {ErrorCode::FileRead, "could not open editor file for reading: " + resolved};
    }

    std::string content;
    try {
        content.reserve(static_cast<size_t>(check.size));
        std::array<char, 65536> buffer{};
        while (in) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = in.gcount();
            if (count <= 0) {
                break;
            }
            content.append(buffer.data(), static_cast<size_t>(count));
        }
    } catch (const std::bad_alloc&) {
        return {ErrorCode::Internal, "not enough memory to load editor file: " + resolved};
    } catch (const std::length_error&) {
        return {ErrorCode::FileRead, "editor file is too large to load into memory: " + resolved};
    }
    if (!in.good() && !in.eof()) {
        return {ErrorCode::FileRead, "failed while reading editor file: " + resolved};
    }
    size_t lf_count = 0;
    size_t cr_count = 0;
    size_t crlf_count = 0;
    for (size_t i = 0; i < content.size(); ++i) {
        if (content[i] == '\r') {
            if (i + 1 < content.size() && content[i + 1] == '\n') {
                ++crlf_count;
                ++i;
            } else {
                ++cr_count;
            }
        } else if (content[i] == '\n') {
            ++lf_count;
        }
    }
    const size_t styles = (lf_count > 0 ? 1U : 0U) + (cr_count > 0 ? 1U : 0U) +
                          (crlf_count > 0 ? 1U : 0U);
    out.mixed_linebreaks = styles > 1;
    if (styles == 1) {
        out.linebreak = crlf_count > 0 ? LineBreak::Crlf
                          : cr_count > 0 ? LineBreak::Cr
                                         : LineBreak::Lf;
    } else {
        out.linebreak = settings.linebreak;
    }

    if (cr_count > 0 || crlf_count > 0) {
        try {
            std::string normalized;
            normalized.reserve(content.size());
            for (size_t i = 0; i < content.size(); ++i) {
                if (content[i] == '\r') {
                    if (i + 1 < content.size() && content[i + 1] == '\n') {
                        ++i;
                    }
                    normalized.push_back('\n');
                } else {
                    normalized.push_back(content[i]);
                }
            }
            content = std::move(normalized);
        } catch (const std::bad_alloc&) {
            return {ErrorCode::Internal,
                    "not enough memory to normalize editor line endings: " + resolved};
        } catch (const std::length_error&) {
            return {ErrorCode::FileRead,
                    "editor file is too large to normalize line endings: " + resolved};
        }
    }
    const IndentationDetection indentation =
        detect_indentation(content, settings.tab_width, settings.tab_style);
    out.tab_width = indentation.tab_width;
    out.tab_style = indentation.tab_style;
    out.tab_width_detected = indentation.tab_width_detected;
    out.tab_style_detected = indentation.tab_style_detected;
    try {
        out.text = PieceTable::from_string(std::move(content));
    } catch (const std::bad_alloc&) {
        return {ErrorCode::Internal, "not enough memory to initialize editor file: " + resolved};
    } catch (const std::length_error&) {
        return {ErrorCode::FileRead, "editor file is too large to initialize: " + resolved};
    }
    return ok_error();
}

Error save_file(const std::string& path, const PieceTable& text) {
    return save_file(path, text, LineBreak::Lf);
}

Error save_file(const std::string& path, const PieceTable& text, LineBreak linebreak) {
    const std::string resolved = expand_user_path(path);
    if (resolved.empty()) {
        return {ErrorCode::BadArgs, "no editor save path was provided"};
    }
    std::ofstream out(resolved, std::ios::binary | std::ios::trunc);
    if (!out) {
        return {ErrorCode::FileWrite, "could not open editor file for writing: " + resolved};
    }
    Error err = text.write_to(out, linebreak);
    if (!err.ok()) {
        return {err.code, err.message + ": " + resolved};
    }
    out.close();
    if (!out) {
        return {ErrorCode::FileWrite, "failed while closing editor file after writing: " + resolved};
    }
    return ok_error();
}

Error ensure_empty_file(const std::string& path) {
    const std::string resolved = expand_user_path(path);
    if (resolved.empty()) {
        return ok_error();
    }
    if (access(resolved.c_str(), F_OK) == 0) {
        return ok_error();
    }
    return save_file(resolved, PieceTable::from_string(""));
}


}  // namespace ainiux::editor
