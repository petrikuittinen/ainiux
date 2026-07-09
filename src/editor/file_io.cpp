#include "editor/editor.hpp"

#include "common.hpp"

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
    out = PieceTable::from_string(std::move(content));
    return ok_error();
}

Error save_file(const std::string& path, const PieceTable& text) {
    const std::string resolved = expand_user_path(path);
    if (resolved.empty()) {
        return {ErrorCode::BadArgs, "no editor save path was provided"};
    }
    std::ofstream out(resolved, std::ios::binary | std::ios::trunc);
    if (!out) {
        return {ErrorCode::FileWrite, "could not open editor file for writing: " + resolved};
    }
    Error err = text.write_to(out);
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
