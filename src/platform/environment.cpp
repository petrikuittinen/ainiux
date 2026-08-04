#include "platform/environment.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include "platform/windows_utf.hpp"
#else
#include <limits.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

namespace ainiux::platform {

std::string environment_value(const char* ascii_name) {
#if defined(_WIN32)
    std::wstring name;
    while (*ascii_name != '\0') name.push_back(static_cast<unsigned char>(*ascii_name++));
    const DWORD required = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
    if (required == 0) return {};
    std::wstring value(static_cast<std::size_t>(required), L'\0');
    const DWORD written = GetEnvironmentVariableW(name.c_str(), value.data(), required);
    if (written == 0 || written >= required) return {};
    value.resize(written);
    std::string utf8;
    return utf16_to_utf8(value, utf8).ok() ? utf8 : std::string();
#else
    const char* value = std::getenv(ascii_name);
    return value == nullptr ? std::string() : std::string(value);
#endif
}

std::string home_directory() {
    std::string home = environment_value("HOME");
#if defined(_WIN32)
    if (home.empty()) home = environment_value("USERPROFILE");
#endif
    return home;
}

std::string executable_directory() {
#if defined(_WIN32)
    std::wstring path(512, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                               static_cast<DWORD>(path.size()));
        if (length == 0) return {};
        if (length < path.size() - 1) {
            path.resize(length);
            break;
        }
        if (path.size() >= 32768) return {};
        path.resize(path.size() * 2);
    }
    std::string utf8;
    if (!utf16_to_utf8(std::filesystem::path(path).parent_path().wstring(), utf8).ok()) return {};
    return utf8;
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    if (size == 0) return {};
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) return {};
    path.resize(std::char_traits<char>::length(path.c_str()));
    std::error_code ec;
    return std::filesystem::canonical(path, ec).parent_path().string();
#else
    std::string path(512, '\0');
    for (;;) {
        const ssize_t length = ::readlink("/proc/self/exe", path.data(), path.size());
        if (length < 0) return {};
        if (static_cast<std::size_t>(length) < path.size()) {
            path.resize(static_cast<std::size_t>(length));
            return std::filesystem::path(path).parent_path().string();
        }
        if (path.size() >= static_cast<std::size_t>(PATH_MAX) * 4U) return {};
        path.resize(path.size() * 2U);
    }
#endif
}

void initialize_process_environment() {
#if defined(_WIN32)
    const std::string home = environment_value("HOME");
    if (home.empty()) {
        const std::wstring profile_name = L"USERPROFILE";
        const DWORD required = GetEnvironmentVariableW(profile_name.c_str(), nullptr, 0);
        if (required > 1) {
            std::wstring profile(required, L'\0');
            const DWORD written = GetEnvironmentVariableW(profile_name.c_str(), profile.data(), required);
            if (written > 0 && written < required) {
                profile.resize(written);
                (void)SetEnvironmentVariableW(L"HOME", profile.c_str());
            }
        }
    }
#endif
}

ProcessEnvironmentGuard::ProcessEnvironmentGuard() {
    initialize_process_environment();
#if defined(_WIN32)
    input_code_page_ = GetConsoleCP();
    output_code_page_ = GetConsoleOutputCP();
    (void)SetConsoleCP(CP_UTF8);
    (void)SetConsoleOutputCP(CP_UTF8);
    stdin_mode_ = _setmode(_fileno(stdin), _O_BINARY);
    stdout_mode_ = _setmode(_fileno(stdout), _O_BINARY);
    stderr_mode_ = _setmode(_fileno(stderr), _O_BINARY);
#endif
}

ProcessEnvironmentGuard::~ProcessEnvironmentGuard() {
#if defined(_WIN32)
    (void)std::fflush(stdout);
    (void)std::fflush(stderr);
    if (stdin_mode_ >= 0) (void)_setmode(_fileno(stdin), stdin_mode_);
    if (stdout_mode_ >= 0) (void)_setmode(_fileno(stdout), stdout_mode_);
    if (stderr_mode_ >= 0) (void)_setmode(_fileno(stderr), stderr_mode_);
    if (input_code_page_ != 0) (void)SetConsoleCP(input_code_page_);
    if (output_code_page_ != 0) (void)SetConsoleOutputCP(output_code_page_);
#endif
}

unsigned long long current_process_id() {
#if defined(_WIN32)
    return static_cast<unsigned long long>(GetCurrentProcessId());
#else
    return static_cast<unsigned long long>(::getpid());
#endif
}

}  // namespace ainiux::platform
