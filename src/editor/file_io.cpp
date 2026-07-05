#include "editor/editor.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <unistd.h>

namespace pkchat::editor {

Error load_file(const std::string& path, PieceTable& out) {
    return load_file(path, EditorSettings{}, out);
}

Error check_load_file_size(const std::string& path, const EditorSettings& settings, FileLoadCheck& check) {
    check = {};
    std::error_code filesystem_error;
    const std::filesystem::file_status status = std::filesystem::status(path, filesystem_error);
    if (filesystem_error) {
        if (filesystem_error == std::errc::no_such_file_or_directory) {
            return {ErrorCode::FileRead, "file not found: " + path};
        }
        return {ErrorCode::FileRead, "could not access file: " + path};
    }
    if (!std::filesystem::exists(status)) {
        return {ErrorCode::FileRead, "file not found: " + path};
    }
    if (!std::filesystem::is_regular_file(status)) {
        return {ErrorCode::FileRead, "editor path is not a regular file: " + path};
    }
    const std::uintmax_t file_size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error) {
        return {ErrorCode::FileRead, "could not determine editor file size before loading: " + path};
    }
    if (settings.file_size_limit >= 0 &&
        file_size > static_cast<std::uintmax_t>(settings.file_size_limit)) {
        return {ErrorCode::FileRead,
                "editor file exceeds FILE_SIZE_LIMIT of " +
                    std::to_string(settings.file_size_limit) + " bytes: " + path +
                    " (" + std::to_string(file_size) + " bytes)"};
    }
    check.size = file_size;
    check.should_warn = settings.huge_file_size_warning > 0 &&
                        file_size >= static_cast<std::uintmax_t>(settings.huge_file_size_warning);
    return ok_error();
}

Error load_file(const std::string& path, const EditorSettings& settings, PieceTable& out) {
    FileLoadCheck check;
    Error err = check_load_file_size(path, settings, check);
    if (!err.ok()) {
        return err;
    }
    if (check.size > static_cast<std::uintmax_t>(std::numeric_limits<size_t>::max())) {
        return {ErrorCode::FileRead,
                "editor file is too large for this platform address space: " + path +
                    " (" + std::to_string(check.size) + " bytes)"};
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {ErrorCode::FileRead, "could not open editor file for reading: " + path};
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
        return {ErrorCode::Internal, "not enough memory to load editor file: " + path};
    } catch (const std::length_error&) {
        return {ErrorCode::FileRead, "editor file is too large to load into memory: " + path};
    }
    if (!in.good() && !in.eof()) {
        return {ErrorCode::FileRead, "failed while reading editor file: " + path};
    }
    out = PieceTable::from_string(std::move(content));
    return ok_error();
}

Error save_file(const std::string& path, const PieceTable& text) {
    if (path.empty()) {
        return {ErrorCode::BadArgs, "no editor save path was provided"};
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return {ErrorCode::FileWrite, "could not open editor file for writing: " + path};
    }
    Error err = text.write_to(out);
    if (!err.ok()) {
        return {err.code, err.message + ": " + path};
    }
    out.close();
    if (!out) {
        return {ErrorCode::FileWrite, "failed while closing editor file after writing: " + path};
    }
    return ok_error();
}

Error ensure_empty_file(const std::string& path) {
    if (path.empty()) {
        return ok_error();
    }
    if (access(path.c_str(), F_OK) == 0) {
        return ok_error();
    }
    return save_file(path, PieceTable::from_string(""));
}


}  // namespace pkchat::editor
