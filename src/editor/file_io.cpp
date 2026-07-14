#include "editor/editor.hpp"

#include "common.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <unistd.h>

namespace pkchat::editor {

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


}  // namespace pkchat::editor
