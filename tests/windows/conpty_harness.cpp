#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

class Handle {
   public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
    HANDLE release() { const HANDLE value = value_; value_ = nullptr; return value; }
    void reset(HANDLE value = nullptr) {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_ = value;
    }
   private:
    HANDLE value_ = nullptr;
};

class PseudoConsole {
   public:
    ~PseudoConsole() { reset(); }
    PseudoConsole(const PseudoConsole&) = delete;
    PseudoConsole& operator=(const PseudoConsole&) = delete;
    PseudoConsole() = default;
    HPCON get() const { return value_; }
    HPCON* put() {
        reset();
        return &value_;
    }
    void reset() {
        if (value_ != nullptr) ClosePseudoConsole(value_);
        value_ = nullptr;
    }

   private:
    HPCON value_ = nullptr;
};

class AttributeList {
   public:
    ~AttributeList() {
        if (value_ != nullptr) {
            if (initialized_) DeleteProcThreadAttributeList(value_);
            HeapFree(GetProcessHeap(), 0, value_);
        }
    }
    AttributeList(const AttributeList&) = delete;
    AttributeList& operator=(const AttributeList&) = delete;
    AttributeList() = default;

    bool initialize(HPCON pseudo_console) {
        SIZE_T bytes = 0;
        (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        if (bytes == 0) return false;
        value_ = static_cast<PPROC_THREAD_ATTRIBUTE_LIST>(
            HeapAlloc(GetProcessHeap(), 0, bytes));
        if (value_ == nullptr ||
            !InitializeProcThreadAttributeList(value_, 1, 0, &bytes))
            return false;
        initialized_ = true;
        return UpdateProcThreadAttribute(value_, 0,
                                         PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                         pseudo_console, sizeof(HPCON), nullptr,
                                         nullptr) != FALSE;
    }
    PPROC_THREAD_ATTRIBUTE_LIST get() const { return value_; }

   private:
    PPROC_THREAD_ATTRIBUTE_LIST value_ = nullptr;
    bool initialized_ = false;
};

bool write_input(HANDLE pipe, const std::string& text) {
    DWORD written = 0;
    return WriteFile(pipe, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
           written == text.size();
}

std::size_t count_occurrences(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = text.find(needle, offset)) != std::string::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

class ScopedHome {
   public:
    ~ScopedHome() {
        if (!changed_) return;
        (void)SetEnvironmentVariableW(L"HOME", had_value_ ? previous_.c_str() : nullptr);
    }
    ScopedHome(const ScopedHome&) = delete;
    ScopedHome& operator=(const ScopedHome&) = delete;
    ScopedHome() = default;

    bool set(const std::wstring& value) {
        SetLastError(ERROR_SUCCESS);
        const DWORD count = GetEnvironmentVariableW(L"HOME", nullptr, 0);
        if (count > 0) {
            previous_.resize(count);
            const DWORD copied = GetEnvironmentVariableW(L"HOME", previous_.data(), count);
            if (copied == 0 || copied >= count) return false;
            previous_.resize(copied);
            had_value_ = true;
        } else if (GetLastError() != ERROR_ENVVAR_NOT_FOUND) {
            had_value_ = true;
            previous_.clear();
        }
        if (!SetEnvironmentVariableW(L"HOME", value.c_str())) return false;
        changed_ = true;
        return true;
    }

   private:
    std::wstring previous_;
    bool had_value_ = false;
    bool changed_ = false;
};

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2 && argc != 3) {
        std::cerr << "usage: conpty_harness PATH_TO_AINIUX_EXE [HOME]\n";
        return 2;
    }
    ScopedHome scoped_home;
    std::wstring child_directory;
    if (argc == 3) {
        child_directory = argv[2];
        if (child_directory.empty() || !scoped_home.set(child_directory)) {
            std::cerr << "could not configure isolated HOME for ConPTY child\n";
            return 2;
        }
    }
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    Handle pseudo_input;
    Handle host_input;
    Handle host_output;
    Handle pseudo_output;
    HANDLE first = nullptr;
    HANDLE second = nullptr;
    if (!CreatePipe(&first, &second, &attributes, 0)) return 3;
    pseudo_input.reset(first);
    host_input.reset(second);
    first = second = nullptr;
    if (!CreatePipe(&first, &second, &attributes, 0)) return 3;
    host_output.reset(first);
    pseudo_output.reset(second);
    (void)SetHandleInformation(host_input.get(), HANDLE_FLAG_INHERIT, 0);
    (void)SetHandleInformation(host_output.get(), HANDLE_FLAG_INHERIT, 0);

    PseudoConsole pseudo_console;
    COORD size{80, 24};
    if (FAILED(CreatePseudoConsole(size, pseudo_input.get(), pseudo_output.get(), 0,
                                   pseudo_console.put())))
        return 4;
    pseudo_input.reset();
    pseudo_output.reset();

    AttributeList attribute_list;
    if (!attribute_list.initialize(pseudo_console.get())) return 5;

    const std::wstring executable = argv[1];
    if (executable.empty()) return 6;
    std::wstring command = L"\"" + executable +
                           L"\" --provider none --chat --nocolors";
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attribute_list.get();
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        executable.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, nullptr,
        child_directory.empty() ? nullptr : child_directory.c_str(),
        &startup.StartupInfo, &process);
    if (!created) return 7;
    Handle process_handle(process.hProcess);
    Handle thread_handle(process.hThread);
    std::string output;
    std::mutex output_mutex;
    std::condition_variable output_changed;
    std::thread reader([&] {
        char buffer[8192];
        for (;;) {
            DWORD count = 0;
            if (!ReadFile(host_output.get(), buffer, sizeof(buffer), &count, nullptr) || count == 0)
                break;
            {
                std::lock_guard<std::mutex> lock(output_mutex);
                if (output.size() >= 4U * 1024U * 1024U) continue;
                const std::size_t keep = std::min<std::size_t>(
                    count, 4U * 1024U * 1024U - output.size());
                output.append(buffer, keep);
            }
            output_changed.notify_all();
        }
    });

    auto wait_for_output = [&](const auto& predicate,
                               std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(output_mutex);
        return output_changed.wait_for(lock, timeout, [&] { return predicate(output); });
    };

    const bool startup_ready = wait_for_output(
        [](const std::string& captured) {
            return captured.find("Threads") != std::string::npos ||
                   captured.find("No saved threads") != std::string::npos;
        },
        std::chrono::seconds(5));
    // A fresh chat profile opens the SQLite thread selector. Leave it before
    // sending command-line input.
    const bool startup_picker_closed = write_input(host_input.get(), "\x1b");
    const bool chat_ready = wait_for_output(
        [](const std::string& captured) {
            return captured.find("Thread list cancelled") != std::string::npos;
        },
        std::chrono::seconds(3));
    size = {100, 32};
    const bool resized = SUCCEEDED(ResizePseudoConsole(pseudo_console.get(), size));
    const bool mouse_sent = write_input(host_input.get(), "\x1b[<64;10;10M");
    const bool shell_sent =
        write_input(host_input.get(), "/shell Start-Sleep -Seconds 30\r");
    const bool shell_started = wait_for_output(
        [](const std::string& captured) {
            return captured.find("Running shell: Start-Sleep") != std::string::npos;
        },
        std::chrono::seconds(3));
    const bool cancel_sent = write_input(host_input.get(), "\x1b");
    const bool cancellation_seen_live = wait_for_output(
        [](const std::string& captured) {
            return captured.find("Cancelling file job") != std::string::npos ||
                   captured.find("[cancelled]") != std::string::npos;
        },
        std::chrono::seconds(4));
    const bool agent_sent = write_input(host_input.get(), "/agent\r");
    const std::wstring agent_database =
        child_directory.empty()
            ? std::wstring()
            : child_directory + L"\\.ainiux-pr\\agent.sqlite";
    const bool agent_screen_seen = wait_for_output(
        [&](const std::string& captured) {
            if (count_occurrences(captured, "\x1b[?1049h") < 2) return false;
            if (agent_database.empty())
                return captured.find(" Agent") != std::string::npos;
            const DWORD attributes = GetFileAttributesW(agent_database.c_str());
            return attributes != INVALID_FILE_ATTRIBUTES &&
                   (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
        },
        std::chrono::seconds(5));
    const bool editor_sent = write_input(host_input.get(), "/editor\r");
    const bool editor_screen_seen = wait_for_output(
        [](const std::string& captured) {
            return captured.find("[scratch]") != std::string::npos;
        },
        std::chrono::seconds(5));
    const bool chat_sent = write_input(host_input.get(), "/chat\r");
    const bool chat_return_seen = wait_for_output(
        [](const std::string& captured) {
            return count_occurrences(captured, "\x1b[?1049h") >= 4;
        },
        std::chrono::seconds(5));
    const bool quit_sent = write_input(host_input.get(), std::string(1, static_cast<char>(17)));

    DWORD wait = WaitForSingleObject(process_handle.get(), 10000);
    if (wait != WAIT_OBJECT_0) {
        (void)TerminateProcess(process_handle.get(), 124);
        (void)WaitForSingleObject(process_handle.get(), 2000);
    }
    host_input.reset();
    pseudo_console.reset();
    if (reader.joinable()) reader.join();
    host_output.reset();
    DWORD exit_code = 1;
    (void)GetExitCodeProcess(process_handle.get(), &exit_code);
    const std::size_t alt_entries = count_occurrences(output, "\x1b[?1049h");
    const std::size_t alt_exits = count_occurrences(output, "\x1b[?1049l");
    const bool cancellation_seen =
        output.find("Cancelling file job") != std::string::npos ||
        output.find("[cancelled]") != std::string::npos;
    bool agent_seen = output.find(" Agent") != std::string::npos;
    if (!agent_database.empty()) {
        const DWORD attributes = GetFileAttributesW(agent_database.c_str());
        agent_seen = attributes != INVALID_FILE_ATTRIBUTES &&
                     (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }
    const bool editor_seen = output.find("[scratch]") != std::string::npos;
    if (!startup_ready || !startup_picker_closed || !chat_ready || !resized ||
        !mouse_sent || !shell_sent || !shell_started || !cancel_sent ||
        !cancellation_seen_live || !agent_sent || !agent_screen_seen ||
        !editor_screen_seen || !chat_return_seen ||
        !editor_sent || !chat_sent || !quit_sent || wait != WAIT_OBJECT_0 ||
        exit_code != 0 || alt_entries < 4 || alt_exits < 4 || !cancellation_seen ||
        !agent_seen || !editor_seen) {
        std::cerr << "ConPTY parity smoke failed: startup=" << startup_ready
                  << " picker=" << startup_picker_closed << " chat_ready=" << chat_ready
                  << " resize=" << resized
                  << " mouse=" << mouse_sent << " editor=" << editor_sent
                  << " shell=" << shell_sent << " shell_started=" << shell_started
                  << " cancel=" << cancel_sent
                  << " cancel_live=" << cancellation_seen_live
                  << " agent=" << agent_sent << " agent_screen=" << agent_screen_seen
                  << " editor_screen=" << editor_screen_seen
                  << " chat=" << chat_sent << " chat_return=" << chat_return_seen
                  << " quit=" << quit_sent
                  << " wait=" << wait << " exit=" << exit_code
                  << " alt_entries=" << alt_entries << " alt_exits=" << alt_exits
                  << " cancellation_seen=" << cancellation_seen
                  << " agent_seen=" << agent_seen << " editor_seen=" << editor_seen
                  << '\n';
        return 1;
    }
    std::cout << "Windows ConPTY smoke passed\n";
    return 0;
}
#else
int main() { return 77; }
#endif
