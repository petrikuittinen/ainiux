#include "platform/filesystem.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include "platform/windows_utf.hpp"
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace ainiux::platform {
namespace {
namespace fs = std::filesystem;

std::string lower_ascii(std::string value) {
    for (char& ch : value) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    }
    return value;
}

bool reserved_windows_component(std::string component) {
    while (!component.empty() && (component.back() == ' ' || component.back() == '.'))
        component.pop_back();
    const std::size_t dot = component.find('.');
    if (dot != std::string::npos) component.resize(dot);
    component = lower_ascii(std::move(component));
    static const std::set<std::string> fixed = {"con", "prn", "aux", "nul", "clock$"};
    if (fixed.find(component) != fixed.end()) return true;
    if (component.size() == 4 &&
        ((component.rfind("com", 0) == 0) || (component.rfind("lpt", 0) == 0)) &&
        component[3] >= '1' && component[3] <= '9')
        return true;
    return false;
}

std::string path_error(const char* action, const std::string& path, const std::string& detail) {
    return std::string(action) + ": " + path + (detail.empty() ? "" : ": " + detail);
}

#if defined(_WIN32)
std::atomic<int> g_next_atomic_write_fault{0};

class Handle {
   public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
    HANDLE release() { const HANDLE value = value_; value_ = INVALID_HANDLE_VALUE; return value; }
    void reset(HANDLE value = INVALID_HANDLE_VALUE) {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_ = value;
    }
    explicit operator bool() const { return value_ != nullptr && value_ != INVALID_HANDLE_VALUE; }
   private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};

Error native_path(const std::string& utf8, std::wstring& output) {
    Error error = validate_windows_path_syntax(utf8);
    if (!error.ok()) return error;
    if (!(error = utf8_to_utf16(utf8, output)).ok()) return error;
    std::replace(output.begin(), output.end(), L'/', L'\\');
    if (output.rfind(L"\\\\?\\", 0) == 0) return ok_error();
    if (!(output.rfind(L"\\\\", 0) == 0 ||
          (output.size() >= 3 && output[1] == L':' && output[2] == L'\\'))) {
        const DWORD required = GetFullPathNameW(output.c_str(), 0, nullptr, nullptr);
        if (required == 0)
            return {ErrorCode::BadArgs,
                    path_error("could not make Windows path absolute", utf8,
                               windows_error_message(GetLastError()))};
        std::wstring absolute(static_cast<std::size_t>(required), L'\0');
        const DWORD written = GetFullPathNameW(output.c_str(), required, absolute.data(), nullptr);
        if (written == 0 || written >= required)
            return {ErrorCode::BadArgs,
                    path_error("could not make Windows path absolute", utf8,
                               windows_error_message(GetLastError()))};
        absolute.resize(written);
        output = std::move(absolute);
    }
    if (output.rfind(L"\\\\", 0) == 0) {
        output = L"\\\\?\\UNC\\" + output.substr(2);
    } else if (output.size() >= 3 && output[1] == L':' && output[2] == L'\\') {
        output = L"\\\\?\\" + output;
    }
    return ok_error();
}

struct SecurityAttributes {
    SECURITY_ATTRIBUTES attributes{};
    SECURITY_DESCRIPTOR descriptor{};
    PACL acl = nullptr;
    std::vector<unsigned char> token_user;
    std::array<unsigned char, SECURITY_MAX_SID_SIZE> system_sid{};

    ~SecurityAttributes() { if (acl != nullptr) LocalFree(acl); }
    Error initialize() {
        Handle token;
        HANDLE raw_token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token))
            return {ErrorCode::FileWrite,
                    "could not query current-user token: " +
                        windows_error_message(GetLastError())};
        token.reset(raw_token);
        DWORD required = 0;
        (void)GetTokenInformation(token.get(), TokenUser, nullptr, 0, &required);
        if (required == 0)
            return {ErrorCode::FileWrite, "could not size current-user SID"};
        token_user.resize(required);
        if (!GetTokenInformation(token.get(), TokenUser, token_user.data(), required,
                                 &required))
            return {ErrorCode::FileWrite,
                    "could not read current-user SID: " +
                        windows_error_message(GetLastError())};
        DWORD system_sid_size = static_cast<DWORD>(system_sid.size());
        if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system_sid.data(),
                                &system_sid_size))
            return {ErrorCode::FileWrite,
                    "could not create SYSTEM SID: " +
                        windows_error_message(GetLastError())};
        EXPLICIT_ACCESSW access[2]{};
        access[0].grfAccessPermissions = GENERIC_ALL;
        access[0].grfAccessMode = SET_ACCESS;
        access[0].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        access[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access[0].Trustee.TrusteeType = TRUSTEE_IS_USER;
        access[0].Trustee.ptstrName = reinterpret_cast<LPWSTR>(
            reinterpret_cast<TOKEN_USER*>(token_user.data())->User.Sid);
        access[1] = access[0];
        access[1].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        access[1].Trustee.ptstrName = reinterpret_cast<LPWSTR>(system_sid.data());
        const DWORD acl_error = SetEntriesInAclW(2, access, nullptr, &acl);
        if (acl_error != ERROR_SUCCESS)
            return {ErrorCode::FileWrite,
                    "could not create private Windows ACL: " +
                        windows_error_message(acl_error)};
        if (!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
            !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) ||
            !SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED,
                                          SE_DACL_PROTECTED))
            return {ErrorCode::FileWrite,
                    "could not create private Windows security descriptor: " +
                        windows_error_message(GetLastError())};
        attributes.nLength = sizeof(attributes);
        attributes.lpSecurityDescriptor = &descriptor;
        attributes.bInheritHandle = FALSE;
        return ok_error();
    }
};

Error apply_private_acl(HANDLE handle, const std::string& path, PACL acl) {
    const DWORD code = SetSecurityInfo(
        handle, SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, acl, nullptr);
    if (code != ERROR_SUCCESS)
        return {ErrorCode::FileWrite,
                path_error("could not protect Windows ACL", path,
                           windows_error_message(code))};
    return ok_error();
}

Error reject_reparse_ancestors(const std::string& utf8_path) {
    std::error_code error;
    const fs::path absolute = fs::absolute(fs::u8path(utf8_path), error).lexically_normal();
    if (error)
        return {ErrorCode::BadArgs,
                path_error("could not resolve path", utf8_path, error.message())};
    fs::path current = absolute.root_path();
    for (const fs::path& component : absolute.relative_path()) {
        current /= component;
        std::wstring native;
        Error conversion = native_path(current.u8string(), native);
        if (!conversion.ok()) return conversion;
        const DWORD attributes = GetFileAttributesW(native.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND)
                continue;
            return {ErrorCode::FileRead,
                    path_error("could not inspect path", current.u8string(),
                               windows_error_message(GetLastError()))};
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            return {ErrorCode::BadArgs,
                    "path crosses a Windows reparse point: " + current.u8string()};
    }
    return ok_error();
}

#else
class Fd {
   public:
    explicit Fd(int value = -1) : value_(value) {}
    ~Fd() { reset(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return value_; }
    int release() { const int value = value_; value_ = -1; return value; }
    void reset(int value = -1) {
        if (value_ >= 0) {
            while (::close(value_) != 0 && errno == EINTR) {}
        }
        value_ = value;
    }
   private:
    int value_ = -1;
};

#endif

}  // namespace

Error validate_windows_path_syntax(const std::string& path,
                                   const PathSafetyOptions& options) {
    if (path.empty()) return {ErrorCode::BadArgs, "path is empty"};
    if (path.find('\0') != std::string::npos)
        return {ErrorCode::BadArgs, "path contains a NUL byte"};
    if (path.rfind("\\\\.\\", 0) == 0 || path.rfind("//./", 0) == 0)
        return {ErrorCode::BadArgs, "Windows device namespace paths are not allowed"};
    if (path.rfind("\\\\?\\", 0) == 0 || path.rfind("//?/", 0) == 0 ||
        path.rfind("\\??\\", 0) == 0)
        return {ErrorCode::BadArgs,
                "caller-supplied Windows NT namespace paths are not allowed"};
    const bool drive_prefix = path.size() >= 2 &&
                              ((path[0] >= 'A' && path[0] <= 'Z') ||
                               (path[0] >= 'a' && path[0] <= 'z')) &&
                              path[1] == ':';
    if (drive_prefix && options.reject_drive_relative &&
        (path.size() == 2 || (path[2] != '/' && path[2] != '\\')))
        return {ErrorCode::BadArgs,
                "ambiguous Windows drive-relative path is not allowed: " + path};
    if (options.reject_alternate_streams) {
        const std::size_t colon = path.find(':', drive_prefix ? 2U : 0U);
        if (colon != std::string::npos)
            return {ErrorCode::BadArgs,
                    "NTFS alternate data stream syntax is not allowed: " + path};
    }
    if (options.reject_reserved_names) {
        std::size_t start = drive_prefix ? 2U : 0U;
        while (start <= path.size()) {
            while (start < path.size() && (path[start] == '/' || path[start] == '\\')) ++start;
            const std::size_t end = path.find_first_of("/\\", start);
            const std::string component =
                path.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!component.empty() && component != "." && component != ".." &&
                (component.back() == ' ' || component.back() == '.'))
                return {ErrorCode::BadArgs,
                        "Windows path components may not end in a space or dot: " + component};
            const bool has_control_character =
                std::any_of(component.begin(), component.end(), [](char value) {
                    return static_cast<unsigned char>(value) < 0x20U;
                });
            if (has_control_character)
                return {ErrorCode::BadArgs,
                        "Windows path contains a control character"};
            if (component.find_first_of("<>\"|?*") != std::string::npos)
                return {ErrorCode::BadArgs,
                        "Windows path contains an invalid filename character: " + component};
            if (!component.empty() && component != "." && component != ".." &&
                reserved_windows_component(component))
                return {ErrorCode::BadArgs,
                        "reserved Windows device name is not allowed in path: " + component};
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    return ok_error();
}

std::string generic_path(const std::string& utf8_path) {
    return fs::u8path(utf8_path).lexically_normal().generic_u8string();
}

bool same_file_identity(const FileIdentity& left, const FileIdentity& right) {
    return left.valid && right.valid && left.volume == right.volume &&
           left.file_low == right.file_low && left.file_high == right.file_high;
}

Error file_identity(const std::string& path, FileIdentity& identity, bool follow_links) {
    identity = FileIdentity{};
#if defined(_WIN32)
    std::wstring native;
    Error error = native_path(path, native);
    if (!error.ok()) return error;
    const DWORD flags = FILE_FLAG_BACKUP_SEMANTICS |
                        (follow_links ? 0U : FILE_FLAG_OPEN_REPARSE_POINT);
    Handle handle(CreateFileW(native.c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, flags, nullptr));
    if (!handle)
        return {ErrorCode::FileRead,
                path_error("could not open path identity", path,
                           windows_error_message(GetLastError()))};
    FILE_ID_INFO info{};
    if (!GetFileInformationByHandleEx(handle.get(), FileIdInfo, &info, sizeof(info)))
        return {ErrorCode::FileRead,
                path_error("could not read path identity", path,
                           windows_error_message(GetLastError()))};
    identity.volume = info.VolumeSerialNumber;
    std::memcpy(&identity.file_low, info.FileId.Identifier, sizeof(identity.file_low));
    std::memcpy(&identity.file_high, info.FileId.Identifier + sizeof(identity.file_low),
                sizeof(identity.file_high));
    identity.valid = true;
#else
    struct stat info{};
    const int code = follow_links ? ::stat(path.c_str(), &info) : ::lstat(path.c_str(), &info);
    if (code != 0)
        return {ErrorCode::FileRead,
                path_error("could not read path identity", path, std::strerror(errno))};
    identity.volume = static_cast<std::uint64_t>(info.st_dev);
    identity.file_low = static_cast<std::uint64_t>(info.st_ino);
    identity.valid = true;
#endif
    return ok_error();
}

Error path_is_within(const std::string& root, const std::string& candidate, bool& within) {
    within = false;
#if defined(_WIN32)
    Error error = validate_windows_path_syntax(root);
    if (!error.ok()) return error;
    error = validate_windows_path_syntax(candidate);
    if (!error.ok()) return error;
#else
    Error error;
#endif
    std::error_code filesystem_error;
    const fs::path canonical_root = fs::canonical(fs::u8path(root), filesystem_error);
    if (filesystem_error)
        return {ErrorCode::FileRead,
                path_error("could not resolve containment root", root,
                           filesystem_error.message())};
    FileIdentity root_identity;
    error = file_identity(canonical_root.u8string(), root_identity);
    if (!error.ok()) return error;

    fs::path current = fs::absolute(fs::u8path(candidate), filesystem_error).lexically_normal();
    if (filesystem_error || current.empty())
        return {ErrorCode::FileRead,
                path_error("could not resolve candidate path", candidate,
                           filesystem_error.message())};
    // A target may not exist yet. Anchor it to the deepest existing parent;
    // canonicalization then follows any link/junction before identity walking.
    for (;;) {
        filesystem_error.clear();
        const fs::file_status status = fs::symlink_status(current, filesystem_error);
        if (!filesystem_error && status.type() != fs::file_type::not_found) {
            current = fs::canonical(current, filesystem_error);
            if (filesystem_error)
                return {ErrorCode::FileRead,
                        path_error("could not canonicalize candidate path", candidate,
                                   filesystem_error.message())};
            break;
        }
        if (filesystem_error &&
            filesystem_error != std::errc::no_such_file_or_directory)
            return {ErrorCode::FileRead,
                    path_error("could not inspect candidate path", candidate,
                               filesystem_error.message())};
        const fs::path parent = current.parent_path();
        if (parent == current || parent.empty())
            return {ErrorCode::FileRead,
                    "could not find an existing parent for candidate path: " + candidate};
        current = parent;
    }
    for (;;) {
        FileIdentity current_identity;
        error = file_identity(current.u8string(), current_identity);
        if (!error.ok()) return error;
        if (same_file_identity(root_identity, current_identity)) {
            within = true;
            return ok_error();
        }
        const fs::path parent = current.parent_path();
        if (parent == current || parent.empty()) return ok_error();
        current = parent;
    }
}

Error path_contains_link_or_reparse(const std::string& path, bool& contains) {
    contains = false;
#if defined(_WIN32)
    Error error = validate_windows_path_syntax(path);
    if (!error.ok()) return error;
#endif
    std::error_code filesystem_error;
    const fs::path absolute =
        fs::absolute(fs::u8path(path), filesystem_error).lexically_normal();
    if (filesystem_error)
        return {ErrorCode::FileRead,
                path_error("could not resolve path", path, filesystem_error.message())};
    fs::path current = absolute.root_path();
    for (const fs::path& component : absolute.relative_path()) {
        current /= component;
#if defined(_WIN32)
        std::wstring native;
        Error error = native_path(current.u8string(), native);
        if (!error.ok()) return error;
        const DWORD attributes = GetFileAttributesW(native.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD code = GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return ok_error();
            return {ErrorCode::FileRead,
                    path_error("could not inspect path", current.u8string(),
                               windows_error_message(code))};
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            contains = true;
            return ok_error();
        }
#else
        const fs::file_status status = fs::symlink_status(current, filesystem_error);
        if (filesystem_error == std::errc::no_such_file_or_directory ||
            status.type() == fs::file_type::not_found)
            return ok_error();
        if (filesystem_error)
            return {ErrorCode::FileRead,
                    path_error("could not inspect path", current.u8string(),
                               filesystem_error.message())};
        if (fs::is_symlink(status)) {
            contains = true;
            return ok_error();
        }
#endif
    }
    return ok_error();
}

Error path_is_link_or_reparse(const std::string& path, bool& is_link) {
    is_link = false;
#if defined(_WIN32)
    std::wstring native;
    Error error = native_path(path, native);
    if (!error.ok()) return error;
    const DWORD attributes = GetFileAttributesW(native.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return {ErrorCode::FileRead,
                path_error("could not inspect path", path,
                           windows_error_message(GetLastError()))};
    is_link = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    std::error_code filesystem_error;
    const fs::file_status status = fs::symlink_status(fs::u8path(path), filesystem_error);
    if (filesystem_error)
        return {ErrorCode::FileRead,
                path_error("could not inspect path", path, filesystem_error.message())};
    is_link = fs::is_symlink(status);
#endif
    return ok_error();
}

Error require_directory_access(const std::string& path,
                               bool require_read,
                               bool require_write) {
#if defined(_WIN32)
    std::wstring native;
    Error error = native_path(path, native);
    if (!error.ok()) return error;
    DWORD access = FILE_READ_ATTRIBUTES;
    if (require_read) access |= FILE_LIST_DIRECTORY;
    if (require_write) access |= FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | DELETE_CHILD;
    Handle directory(CreateFileW(native.c_str(), access,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!directory)
        return {require_write ? ErrorCode::FileWrite : ErrorCode::FileRead,
                path_error("cannot access directory", path,
                           windows_error_message(GetLastError()))};
#else
    int mode = X_OK;
    if (require_read) mode |= R_OK;
    if (require_write) mode |= W_OK;
    if (::access(path.c_str(), mode) != 0)
        return {require_write ? ErrorCode::FileWrite : ErrorCode::FileRead,
                path_error("cannot access directory", path, std::strerror(errno))};
#endif
    return ok_error();
}

Error ensure_private_directory(const std::string& path,
                               bool reject_reparse_points,
                               bool protect_existing) {
#if defined(_WIN32)
    if (reject_reparse_points) {
        Error error = reject_reparse_ancestors(path);
        if (!error.ok()) return error;
    }
    std::error_code filesystem_error;
    const fs::path absolute = fs::absolute(fs::u8path(path), filesystem_error).lexically_normal();
    if (filesystem_error)
        return {ErrorCode::FileWrite,
                path_error("could not resolve directory", path, filesystem_error.message())};
    SecurityAttributes security;
    Error error = security.initialize();
    if (!error.ok()) return error;
    fs::path current = absolute.root_path();
    bool final_created = false;
    for (const fs::path& component : absolute.relative_path()) {
        current /= component;
        std::wstring native;
        if (!(error = native_path(current.u8string(), native)).ok()) return error;
        const DWORD attributes = GetFileAttributesW(native.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                return {ErrorCode::FileWrite,
                        "path exists but is not a directory: " + current.u8string()};
            if (reject_reparse_points && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                return {ErrorCode::BadArgs,
                        "directory path crosses a reparse point: " + current.u8string()};
            continue;
        }
        if (CreateDirectoryW(native.c_str(), &security.attributes)) {
            if (current == absolute) final_created = true;
        } else if (GetLastError() == ERROR_ALREADY_EXISTS) {
            const DWORD raced_attributes = GetFileAttributesW(native.c_str());
            if (raced_attributes == INVALID_FILE_ATTRIBUTES ||
                (raced_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
                (reject_reparse_points &&
                 (raced_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0))
                return {ErrorCode::BadArgs,
                        "private directory path raced with a non-directory or reparse point: " +
                            current.u8string()};
        } else {
            return {ErrorCode::FileWrite,
                    path_error("could not create private directory", current.u8string(),
                               windows_error_message(GetLastError()))};
        }
    }
    if (!final_created && !protect_existing) return ok_error();
    std::wstring final_native;
    if (!(error = native_path(absolute.u8string(), final_native)).ok()) return error;
    Handle final_directory(CreateFileW(
        final_native.c_str(), READ_CONTROL | WRITE_DAC,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!final_directory)
        return {ErrorCode::FileWrite,
                path_error("could not open private directory ACL", path,
                           windows_error_message(GetLastError()))};
    BY_HANDLE_FILE_INFORMATION final_information{};
    if (!GetFileInformationByHandle(final_directory.get(), &final_information))
        return {ErrorCode::FileWrite,
                path_error("could not verify private directory", path,
                           windows_error_message(GetLastError()))};
    if ((final_information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (reject_reparse_points &&
         (final_information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0))
        return {ErrorCode::BadArgs,
                "private directory path is not a plain directory: " + path};
    if (!(error = apply_private_acl(final_directory.get(), path, security.acl)).ok())
        return error;
    return ok_error();
#else
    std::error_code filesystem_error;
    fs::path current;
    bool final_created = false;
    const fs::path absolute = fs::absolute(fs::u8path(path), filesystem_error).lexically_normal();
    if (filesystem_error)
        return {ErrorCode::FileWrite,
                path_error("could not resolve directory", path, filesystem_error.message())};
    for (const fs::path& component : absolute) {
        current /= component;
        struct stat info{};
        const int status_code = reject_reparse_points
                                    ? ::lstat(current.c_str(), &info)
                                    : ::stat(current.c_str(), &info);
        if (status_code == 0) {
            if (!S_ISDIR(info.st_mode))
                return {ErrorCode::FileWrite,
                        "path exists but is not a directory: " + current.string()};
            continue;
        }
        if (errno != ENOENT)
            return {ErrorCode::FileWrite,
                    path_error("could not create private directory", current.string(),
                               std::strerror(errno))};
        if (::mkdir(current.c_str(), 0700) == 0) {
            if (current == absolute) final_created = true;
            continue;
        }
        if (errno != EEXIST)
            return {ErrorCode::FileWrite,
                    path_error("could not create private directory", current.string(),
                               std::strerror(errno))};
        struct stat raced{};
        const int raced_status = reject_reparse_points
                                      ? ::lstat(current.c_str(), &raced)
                                      : ::stat(current.c_str(), &raced);
        if (raced_status != 0 || !S_ISDIR(raced.st_mode) ||
            (reject_reparse_points && S_ISLNK(raced.st_mode)))
            return {ErrorCode::BadArgs,
                    "private directory path raced with a non-directory or symlink: " +
                        current.string()};
    }
    if (!final_created && !protect_existing) return ok_error();
    if (::chmod(path.c_str(), 0700) != 0)
        return {ErrorCode::FileWrite,
                path_error("could not protect private directory", path,
                           std::strerror(errno))};
    return ok_error();
#endif
}

Error create_private_directory_exclusive(const std::string& path, bool& created) {
    created = false;
    const fs::path parent = fs::u8path(path).parent_path();
    if (!parent.empty()) {
        Error error = ensure_private_directory(parent.u8string());
        if (!error.ok()) return error;
    }
#if defined(_WIN32)
    SecurityAttributes security;
    Error error = security.initialize();
    if (!error.ok()) return error;
    std::wstring native;
    if (!(error = native_path(path, native)).ok()) return error;
    if (CreateDirectoryW(native.c_str(), &security.attributes)) {
        created = true;
        return ok_error();
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        const DWORD attributes = GetFileAttributesW(native.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
            return {ErrorCode::FileWrite,
                    path_error("could not inspect existing private directory", path,
                               windows_error_message(GetLastError()))};
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            return {ErrorCode::BadArgs,
                    "private directory path is not a plain directory: " + path};
        return ok_error();
    }
    return {ErrorCode::FileWrite,
            path_error("could not create private directory", path,
                       windows_error_message(GetLastError()))};
#else
    if (::mkdir(path.c_str(), 0700) == 0) {
        created = true;
        return ok_error();
    }
    if (errno == EEXIST) {
        struct stat info{};
        if (::lstat(path.c_str(), &info) != 0)
            return {ErrorCode::FileWrite,
                    path_error("could not inspect existing private directory", path,
                               std::strerror(errno))};
        if (!S_ISDIR(info.st_mode) || S_ISLNK(info.st_mode))
            return {ErrorCode::BadArgs,
                    "private directory path is not a plain directory: " + path};
        return ok_error();
    }
    return {ErrorCode::FileWrite,
            path_error("could not create private directory", path, std::strerror(errno))};
#endif
}

Error secure_random_hex(std::size_t byte_count, std::string& output) {
    if (byte_count > 1024U * 1024U)
        return {ErrorCode::BadArgs, "secure random request is too large"};
    std::vector<unsigned char> bytes(byte_count);
#if defined(_WIN32)
    if (byte_count > 0 &&
        BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        return {ErrorCode::Internal, "BCryptGenRandom failed"};
#else
    Fd random(::open("/dev/urandom", O_RDONLY | O_CLOEXEC));
    if (random.get() < 0)
        return {ErrorCode::Internal,
                path_error("could not open secure random source", "/dev/urandom",
                           std::strerror(errno))};
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = ::read(random.get(), bytes.data() + offset, bytes.size() - offset);
        if (count > 0) offset += static_cast<std::size_t>(count);
        else if (count < 0 && errno == EINTR) continue;
        else return {ErrorCode::Internal, "could not read secure random bytes"};
    }
#endif
    static constexpr char digits[] = "0123456789abcdef";
    output.clear();
    output.reserve(bytes.size() * 2U);
    for (unsigned char value : bytes) {
        output.push_back(digits[value >> 4U]);
        output.push_back(digits[value & 0x0FU]);
    }
    return ok_error();
}

Error create_private_file_if_missing(const std::string& path) {
    const fs::path parent = fs::u8path(path).parent_path();
    if (!parent.empty()) {
        Error error = ensure_private_directory(parent.u8string());
        if (!error.ok()) return error;
    }
#if defined(_WIN32)
    SecurityAttributes security;
    Error error = security.initialize();
    if (!error.ok()) return error;
    std::wstring native;
    if (!(error = native_path(path, native)).ok()) return error;
    Handle file(CreateFileW(native.c_str(), GENERIC_READ | GENERIC_WRITE | WRITE_DAC,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, &security.attributes,
                            OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                            nullptr));
    if (!file)
        return {ErrorCode::FileWrite,
                path_error("could not create private file", path,
                           windows_error_message(GetLastError()))};
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(file.get(), &information))
        return {ErrorCode::FileWrite,
                path_error("could not inspect private file", path,
                           windows_error_message(GetLastError()))};
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return {ErrorCode::BadArgs,
                "private file path is a reparse point or directory: " + path};
    if (!(error = apply_private_acl(file.get(), path, security.acl)).ok()) return error;
    return ok_error();
#else
    Fd file(::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (file.get() < 0)
        return {ErrorCode::FileWrite,
                path_error("could not create private file", path, std::strerror(errno))};
    if (::fchmod(file.get(), 0600) != 0)
        return {ErrorCode::FileWrite,
                path_error("could not protect private file", path,
                           std::strerror(errno))};
    return ok_error();
#endif
}

Error create_private_file_exclusive(const std::string& path, bool& created) {
    created = false;
    const fs::path parent = fs::u8path(path).parent_path();
    if (!parent.empty()) {
        Error error = ensure_private_directory(parent.u8string());
        if (!error.ok()) return error;
    }
#if defined(_WIN32)
    SecurityAttributes security;
    Error error = security.initialize();
    if (!error.ok()) return error;
    std::wstring native;
    if (!(error = native_path(path, native)).ok()) return error;
    Handle file(CreateFileW(native.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                            &security.attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file) {
        created = true;
        return ok_error();
    }
    if (GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS)
        return ok_error();
    return {ErrorCode::FileWrite,
            path_error("could not create private file", path,
                       windows_error_message(GetLastError()))};
#else
    Fd file(::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (file.get() >= 0) {
        created = true;
        return ok_error();
    }
    if (errno == EEXIST) return ok_error();
    return {ErrorCode::FileWrite,
            path_error("could not create private file", path, std::strerror(errno))};
#endif
}

Error read_file_bounded(const std::string& path, std::size_t limit, std::string& output) {
    output.clear();
#if defined(_WIN32)
    std::wstring native;
    Error error = native_path(path, native);
    if (!error.ok()) return error;
    Handle file(CreateFileW(native.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file)
        return {ErrorCode::FileRead,
                path_error("could not open file", path,
                           windows_error_message(GetLastError()))};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) > limit)
        return {ErrorCode::FileRead, "file exceeds size limit: " + path};
    output.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    while (offset < output.size()) {
        DWORD read = 0;
        const DWORD wanted = static_cast<DWORD>(
            std::min<std::size_t>(output.size() - offset, 1024U * 1024U));
        if (!ReadFile(file.get(), output.data() + offset, wanted, &read, nullptr))
            return {ErrorCode::FileRead,
                    path_error("could not read file", path,
                               windows_error_message(GetLastError()))};
        if (read == 0) break;
        offset += read;
    }
    output.resize(offset);
#else
    Fd file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (file.get() < 0)
        return {ErrorCode::FileRead,
                path_error("could not open file", path, std::strerror(errno))};
    std::array<char, 8192> buffer{};
    for (;;) {
        const ssize_t count = ::read(file.get(), buffer.data(), buffer.size());
        if (count > 0) {
            if (output.size() + static_cast<std::size_t>(count) > limit)
                return {ErrorCode::FileRead, "file exceeds size limit: " + path};
            output.append(buffer.data(), static_cast<std::size_t>(count));
        } else if (count == 0) {
            break;
        } else if (errno != EINTR) {
            return {ErrorCode::FileRead,
                    path_error("could not read file", path, std::strerror(errno))};
        }
    }
#endif
    return ok_error();
}

namespace {

enum class AtomicWritePrivacy {
    Private,  // 0600 / private DACL (secrets, chat, history)
    Shared,   // umask for new files; preserve mode when overwriting
};

Error atomic_write_impl(const std::string& path,
                        const std::string& data,
                        bool reject_reparse_points,
                        AtomicWritePrivacy privacy,
                        bool replace_existing = true) {
    const fs::path parent = fs::u8path(path).parent_path();
    if (!parent.empty()) {
        Error error = require_directory_access(parent.u8string(), true, true);
        if (!error.ok()) return error;
    }
    if (reject_reparse_points) {
#if defined(_WIN32)
        Error error = reject_reparse_ancestors(parent.empty() ? "." : parent.u8string());
        if (!error.ok()) return error;
        std::wstring existing_native;
        if (!(error = native_path(path, existing_native)).ok()) return error;
        const DWORD attributes = GetFileAttributesW(existing_native.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            return {ErrorCode::BadArgs,
                    "refusing to replace a Windows reparse point: " + path};
        if (attributes == INVALID_FILE_ATTRIBUTES &&
            GetLastError() != ERROR_FILE_NOT_FOUND &&
            GetLastError() != ERROR_PATH_NOT_FOUND)
            return {ErrorCode::FileWrite,
                    path_error("could not inspect atomic-write target", path,
                               windows_error_message(GetLastError()))};
#endif
    }
    std::string suffix;
    Error random_error = secure_random_hex(16, suffix);
    if (!random_error.ok()) return random_error;
    const std::string temporary = path + ".ainiux-tmp-" + suffix;
#if defined(_WIN32)
    SecurityAttributes security;
    LPSECURITY_ATTRIBUTES security_ptr = nullptr;
    if (privacy == AtomicWritePrivacy::Private) {
        Error error = security.initialize();
        if (!error.ok()) return error;
        security_ptr = &security.attributes;
    }
    std::wstring native_temp;
    std::wstring native_path_value;
    Error error;
    if (!(error = native_path(temporary, native_temp)).ok() ||
        !(error = native_path(path, native_path_value)).ok())
        return error;
    Handle file(CreateFileW(native_temp.c_str(), GENERIC_WRITE, 0, security_ptr,
                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                            nullptr));
    if (!file)
        return {ErrorCode::FileWrite,
                path_error("could not create temporary file", temporary,
                           windows_error_message(GetLastError()))};
    std::size_t offset = 0;
    while (offset < data.size()) {
        DWORD written = 0;
        const DWORD wanted = static_cast<DWORD>(
            std::min<std::size_t>(data.size() - offset, 1024U * 1024U));
        const int injected_fault = g_next_atomic_write_fault.exchange(0);
        BOOL write_succeeded = TRUE;
        if (injected_fault ==
            static_cast<int>(testing::AtomicWriteFault::DiskFull)) {
            SetLastError(ERROR_DISK_FULL);
            write_succeeded = FALSE;
        } else if (injected_fault ==
                   static_cast<int>(testing::AtomicWriteFault::ShortWrite)) {
            written = 0;
        } else {
            write_succeeded =
                WriteFile(file.get(), data.data() + offset, wanted, &written, nullptr);
        }
        if (!write_succeeded || written == 0) {
            const DWORD code = write_succeeded ? ERROR_WRITE_FAULT : GetLastError();
            file.reset();
            (void)DeleteFileW(native_temp.c_str());
            return {ErrorCode::FileWrite,
                    path_error("could not write temporary file", temporary,
                               windows_error_message(code))};
        }
        offset += written;
    }
    if (!FlushFileBuffers(file.get())) {
        const DWORD code = GetLastError();
        file.reset();
        (void)DeleteFileW(native_temp.c_str());
        return {ErrorCode::FileWrite,
                path_error("could not flush temporary file", temporary,
                           windows_error_message(code))};
    }
    file.reset();
    const DWORD destination_attributes = GetFileAttributesW(native_path_value.c_str());
    bool replaced = false;
    if (!replace_existing) {
        replaced = MoveFileExW(native_temp.c_str(), native_path_value.c_str(),
                               MOVEFILE_WRITE_THROUGH) != FALSE;
    } else if (destination_attributes != INVALID_FILE_ATTRIBUTES) {
        replaced = ReplaceFileW(native_path_value.c_str(), native_temp.c_str(), nullptr,
                                REPLACEFILE_WRITE_THROUGH, nullptr, nullptr) != FALSE;
    } else {
        replaced = MoveFileExW(native_temp.c_str(), native_path_value.c_str(),
                               MOVEFILE_WRITE_THROUGH) != FALSE;
    }
    if (!replaced) {
        const DWORD code = GetLastError();
        (void)DeleteFileW(native_temp.c_str());
        return {ErrorCode::FileWrite,
                path_error("could not atomically replace file", path,
                           windows_error_message(code))};
    }
    if (privacy == AtomicWritePrivacy::Private) {
        Handle destination(CreateFileW(
            native_path_value.c_str(), READ_CONTROL | WRITE_DAC,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        if (!destination)
            return {ErrorCode::FileWrite,
                    path_error("could not open replaced file ACL", path,
                               windows_error_message(GetLastError()))};
        BY_HANDLE_FILE_INFORMATION destination_information{};
        if (!GetFileInformationByHandle(destination.get(), &destination_information))
            return {ErrorCode::FileWrite,
                    path_error("could not verify replaced file", path,
                               windows_error_message(GetLastError()))};
        if (reject_reparse_points &&
            (destination_information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            return {ErrorCode::BadArgs,
                    "atomic-write target became a Windows reparse point: " + path};
        if (!(error = apply_private_acl(destination.get(), path, security.acl)).ok())
            return error;
    } else if (reject_reparse_points) {
        Handle destination(CreateFileW(
            native_path_value.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        if (destination) {
            BY_HANDLE_FILE_INFORMATION destination_information{};
            if (GetFileInformationByHandle(destination.get(), &destination_information) &&
                (destination_information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                return {ErrorCode::BadArgs,
                        "atomic-write target became a Windows reparse point: " + path};
        }
    }
#else
    mode_t create_mode = 0600;
    mode_t preserve_mode = 0;
    bool have_preserve = false;
    if (privacy == AtomicWritePrivacy::Shared) {
        create_mode = 0666;  // umask applied by the kernel on create
        struct stat existing {};
        if (replace_existing && ::lstat(path.c_str(), &existing) == 0 && S_ISREG(existing.st_mode)) {
            preserve_mode = existing.st_mode & 0777;
            have_preserve = true;
        }
    }
    Fd file(::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                   create_mode));
    if (file.get() < 0)
        return {ErrorCode::FileWrite,
                path_error("could not create temporary file", temporary,
                           std::strerror(errno))};
    if (have_preserve && ::fchmod(file.get(), preserve_mode) != 0) {
        const int code = errno;
        file.reset();
        (void)::unlink(temporary.c_str());
        return {ErrorCode::FileWrite,
                path_error("could not set temporary file mode", temporary, std::strerror(code))};
    }
    if (privacy == AtomicWritePrivacy::Private && ::fchmod(file.get(), 0600) != 0) {
        const int code = errno;
        file.reset();
        (void)::unlink(temporary.c_str());
        return {ErrorCode::FileWrite,
                path_error("could not protect temporary file", temporary, std::strerror(code))};
    }
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t written = ::write(file.get(), data.data() + offset, data.size() - offset);
        if (written > 0) offset += static_cast<std::size_t>(written);
        else if (written < 0 && errno == EINTR) continue;
        else {
            file.reset();
            (void)::unlink(temporary.c_str());
            return {ErrorCode::FileWrite,
                    path_error("could not write temporary file", temporary,
                               written == 0 ? "short write" : std::strerror(errno))};
        }
    }
    if (::fsync(file.get()) != 0) {
        const int code = errno;
        file.reset();
        (void)::unlink(temporary.c_str());
        return {ErrorCode::FileWrite,
                path_error("could not flush temporary file", temporary,
                           std::strerror(code))};
    }
    file.reset();
    if (replace_existing) {
        if (::rename(temporary.c_str(), path.c_str()) != 0) {
            const int code = errno;
            (void)::unlink(temporary.c_str());
            return {ErrorCode::FileWrite,
                    path_error("could not atomically replace file", path, std::strerror(code))};
        }
    } else {
        if (::link(temporary.c_str(), path.c_str()) != 0) {
            const int code = errno;
            (void)::unlink(temporary.c_str());
            return {ErrorCode::FileWrite,
                    path_error("could not atomically create file", path, std::strerror(code))};
        }
        int unlink_result = 0;
        do {
            unlink_result = ::unlink(temporary.c_str());
        } while (unlink_result != 0 && errno == EINTR);
        if (unlink_result != 0) {
            return {ErrorCode::FileWrite,
                    path_error("could not remove atomic-create temporary link", temporary,
                               std::strerror(errno))};
        }
    }
    if (!parent.empty()) {
        Fd directory(::open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY));
        if (directory.get() >= 0) (void)::fsync(directory.get());
    }
#endif
    return ok_error();
}

}  // namespace

Error atomic_write_private(const std::string& path,
                           const std::string& data,
                           bool reject_reparse_points) {
    return atomic_write_impl(path, data, reject_reparse_points, AtomicWritePrivacy::Private);
}

Error atomic_write_private_create(const std::string& path,
                                  const std::string& data,
                                  bool reject_reparse_points) {
    return atomic_write_impl(path, data, reject_reparse_points,
                             AtomicWritePrivacy::Private, false);
}

Error atomic_write_shared(const std::string& path,
                          const std::string& data,
                          bool reject_reparse_points) {
    return atomic_write_impl(path, data, reject_reparse_points, AtomicWritePrivacy::Shared);
}

Error atomic_write_shared_create(const std::string& path,
                                 const std::string& data,
                                 bool reject_reparse_points) {
    return atomic_write_impl(path, data, reject_reparse_points,
                             AtomicWritePrivacy::Shared, false);
}

Error atomic_move(const std::string& from, const std::string& to, bool replace_existing) {
    const fs::path from_parent = fs::u8path(from).parent_path();
    const fs::path to_parent = fs::u8path(to).parent_path();
#if defined(_WIN32)
    std::wstring native_from;
    std::wstring native_to;
    Error error = native_path(from, native_from);
    if (!error.ok()) return error;
    error = native_path(to, native_to);
    if (!error.ok()) return error;
    DWORD flags = MOVEFILE_WRITE_THROUGH;
    if (replace_existing) flags |= MOVEFILE_REPLACE_EXISTING;
    if (!MoveFileExW(native_from.c_str(), native_to.c_str(), flags))
        return {ErrorCode::FileWrite,
                path_error("could not atomically move file", from + " -> " + to,
                           windows_error_message(GetLastError()))};
#else
    if (!replace_existing) {
        struct stat existing{};
        if (::lstat(to.c_str(), &existing) == 0)
            return {ErrorCode::FileWrite, "atomic move destination already exists: " + to};
        if (errno != ENOENT)
            return {ErrorCode::FileWrite,
                    path_error("could not inspect atomic move destination", to,
                               std::strerror(errno))};
    }
    if (::rename(from.c_str(), to.c_str()) != 0)
        return {ErrorCode::FileWrite,
                path_error("could not atomically move file", from + " -> " + to,
                           std::strerror(errno))};
    auto flush_parent = [](const fs::path& parent) -> Error {
        if (parent.empty()) return ok_error();
        Fd directory(::open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY));
        if (directory.get() >= 0 && ::fsync(directory.get()) != 0)
            return {ErrorCode::FileWrite,
                    path_error("could not flush atomic move directory", parent.string(),
                               std::strerror(errno))};
        return ok_error();
    };
    Error flush_error = flush_parent(to_parent);
    if (!flush_error.ok()) return flush_error;
    if (from_parent.lexically_normal() != to_parent.lexically_normal()) {
        flush_error = flush_parent(from_parent);
        if (!flush_error.ok()) return flush_error;
    }
#endif
    return ok_error();
}

Error flush_file(const std::string& path) {
#if defined(_WIN32)
    std::wstring native;
    Error error = native_path(path, native);
    if (!error.ok()) return error;
    Handle file(CreateFileW(native.c_str(), GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file || !FlushFileBuffers(file.get()))
        return {ErrorCode::FileWrite,
                path_error("could not flush file", path,
                           windows_error_message(GetLastError()))};
#else
    Fd file(::open(path.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW));
    if (file.get() < 0 || ::fsync(file.get()) != 0)
        return {ErrorCode::FileWrite,
                path_error("could not flush file", path, std::strerror(errno))};
#endif
    return ok_error();
}

#if defined(_WIN32)
namespace testing {
void set_next_atomic_write_fault(AtomicWriteFault fault) {
    g_next_atomic_write_fault.store(static_cast<int>(fault));
}
}  // namespace testing
#endif

}  // namespace ainiux::platform
