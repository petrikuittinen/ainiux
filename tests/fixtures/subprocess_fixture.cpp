#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

#if defined(_WIN32)
bool wide_to_utf8(const wchar_t* input, std::string& output);
std::string fixture_environment_value();
#endif

int fixture_main(const std::string& self, const std::vector<std::string>& arguments) {
    if (arguments.empty()) return 2;
    const std::string& mode = arguments[0];
    if (mode == "--inspect") {
        std::cout << "cwd:" << std::filesystem::current_path().u8string() << '\n';
        for (std::size_t index = 1; index < arguments.size(); ++index)
            std::cout << "arg:" << arguments[index] << '\n';
#if defined(_WIN32)
        std::cout << "env:" << fixture_environment_value() << '\n';
#else
        const char* environment = std::getenv("AINIUX_FIXTURE");
        std::cout << "env:" << (environment == nullptr ? "" : environment) << '\n';
#endif
        std::string input((std::istreambuf_iterator<char>(std::cin)),
                          std::istreambuf_iterator<char>());
        std::cout << "stdin:" << input;
        std::cerr << "fixture-stderr\r\n";
        return 0;
    }
    if (mode == "--exit" && arguments.size() == 2)
        return std::atoi(arguments[1].c_str());
    if (mode == "--sleep" && arguments.size() == 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(
            std::strtoll(arguments[1].c_str(), nullptr, 10)));
        return 0;
    }
    if (mode == "--close-stdin") {
#if defined(_WIN32)
        const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
        if (input == nullptr || input == INVALID_HANDLE_VALUE || !CloseHandle(input))
            return 3;
        (void)SetStdHandle(STD_INPUT_HANDLE, INVALID_HANDLE_VALUE);
#else
        if (::close(STDIN_FILENO) != 0) return 3;
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        return 0;
    }
    if (mode == "--flood" && arguments.size() == 2) {
        const std::size_t bytes = static_cast<std::size_t>(
            std::strtoull(arguments[1].c_str(), nullptr, 10));
        for (std::size_t index = 0; index < bytes; ++index) std::cout.put('x');
        return 0;
    }
    if (mode == "--invalid") {
        const char bytes[] = {'o', 'k', ':', static_cast<char>(0xF0),
                              static_cast<char>(0x28), static_cast<char>(0x8C),
                              static_cast<char>(0x28), '\n'};
        std::cout.write(bytes, static_cast<std::streamsize>(sizeof(bytes)));
        return 0;
    }
    if (mode == "--delayed-marker" && arguments.size() == 3) {
        std::this_thread::sleep_for(std::chrono::milliseconds(
            std::strtoll(arguments[2].c_str(), nullptr, 10)));
        std::ofstream marker(std::filesystem::u8path(arguments[1]), std::ios::binary);
        marker << "descendant survived\n";
        return marker ? 0 : 3;
    }
    if (mode == "--descendant" && arguments.size() == 3) {
#if defined(_WIN32)
        const std::wstring executable = std::filesystem::absolute(
            std::filesystem::u8path(self)).wstring();
        const std::wstring marker = std::filesystem::u8path(arguments[1]).wstring();
        const std::wstring delay(arguments[2].begin(), arguments[2].end());
        std::wstring command = L"\"" + executable + L"\" --delayed-marker \"" +
                               marker + L"\" " + delay;
        std::vector<wchar_t> mutable_command(command.begin(), command.end());
        mutable_command.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable.c_str(), mutable_command.data(), nullptr, nullptr,
                            FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
            return 4;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
#else
        const pid_t child = fork();
        if (child < 0) return 4;
        if (child == 0) {
            execl(self.c_str(), self.c_str(), "--delayed-marker", arguments[1].c_str(),
                  arguments[2].c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
#endif
    }
#if defined(_WIN32)
    if (mode == "--exception") {
        RaiseException(0xE1234567U, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        return 5;
    }
#endif
    return 2;
}

#if defined(_WIN32)
bool wide_to_utf8(const wchar_t* input, std::string& output) {
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input, -1,
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return false;
    output.resize(static_cast<std::size_t>(bytes));
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input, -1,
                            output.data(), bytes, nullptr, nullptr) != bytes)
        return false;
    output.pop_back();
    return true;
}

std::string fixture_environment_value() {
    const DWORD required = GetEnvironmentVariableW(L"AINIUX_FIXTURE", nullptr, 0);
    if (required == 0) return {};
    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    const DWORD written = GetEnvironmentVariableW(L"AINIUX_FIXTURE", wide.data(), required);
    if (written == 0 || written >= required) return {};
    wide.resize(written);
    std::string utf8;
    return wide_to_utf8(wide.c_str(), utf8) ? utf8 : std::string();
}
#endif

}  // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
    std::string self;
    if (!wide_to_utf8(argv[0], self)) return 2;
    std::vector<std::string> arguments;
    for (int index = 1; index < argc; ++index) {
        std::string value;
        if (!wide_to_utf8(argv[index], value)) return 2;
        arguments.push_back(std::move(value));
    }
    return fixture_main(self, arguments);
}
#else
int main(int argc, char** argv) {
    return fixture_main(argv[0], std::vector<std::string>(argv + 1, argv + argc));
}
#endif
