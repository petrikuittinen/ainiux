#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "platform/windows_utf.hpp"

#include <algorithm>
#include <limits>
#include <vector>

namespace ainiux::platform {
namespace {

Error conversion_size_error(const char* direction) {
    return {ErrorCode::BadArgs,
            std::string("could not convert ") + direction + ": input is too large"};
}

}  // namespace

Error utf8_to_utf16(const std::string& input, std::wstring& output) {
    output.clear();
    if (input.empty()) return ok_error();
    if (input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return conversion_size_error("UTF-8 to UTF-16");
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                                           static_cast<int>(input.size()), nullptr, 0);
    if (length <= 0)
        return {ErrorCode::BadArgs,
                "invalid UTF-8 passed to a Windows API: " +
                    windows_error_message(GetLastError())};
    output.resize(static_cast<std::size_t>(length));
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                            static_cast<int>(input.size()), output.data(), length) != length) {
        output.clear();
        return {ErrorCode::Internal,
                "could not convert UTF-8 for a Windows API: " +
                    windows_error_message(GetLastError())};
    }
    return ok_error();
}

Error utf16_to_utf8(const std::wstring& input, std::string& output) {
    output.clear();
    if (input.empty()) return ok_error();
    if (input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return conversion_size_error("UTF-16 to UTF-8");
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
                                           static_cast<int>(input.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (length <= 0)
        return {ErrorCode::BadArgs,
                "invalid UTF-16 returned by a Windows API: " +
                    windows_error_message(GetLastError())};
    output.resize(static_cast<std::size_t>(length));
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
                            static_cast<int>(input.size()), output.data(), length,
                            nullptr, nullptr) != length) {
        output.clear();
        return {ErrorCode::Internal,
                "could not convert UTF-16 returned by a Windows API: " +
                    windows_error_message(GetLastError())};
    }
    return ok_error();
}

std::string windows_error_message(unsigned long code) {
    wchar_t* allocated = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&allocated), 0, nullptr);
    if (length == 0 || allocated == nullptr) return "Windows error " + std::to_string(code);
    std::wstring wide(allocated, length);
    LocalFree(allocated);
    while (!wide.empty() && (wide.back() == L'\r' || wide.back() == L'\n' ||
                             wide.back() == L' ' || wide.back() == L'\t'))
        wide.pop_back();
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                          static_cast<int>(wide.size()), nullptr, 0,
                                          nullptr, nullptr);
    if (bytes <= 0) return "Windows error " + std::to_string(code);
    std::string utf8(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                            utf8.data(), bytes, nullptr, nullptr) != bytes)
        return "Windows error " + std::to_string(code);
    return utf8 + " (Windows error " + std::to_string(code) + ")";
}

}  // namespace ainiux::platform

#endif
