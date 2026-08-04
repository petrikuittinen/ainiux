#include "editor/clipboard.hpp"

#include "html/html.hpp"
#include "platform/environment.hpp"
#include "runtime/subprocess.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "platform/windows_utf.hpp"
#else
#include <unistd.h>
#endif

namespace ainiux::editor {
namespace {

std::string getenv_copy(const char* name) {
    return platform::environment_value(name);
}

#if !defined(_WIN32)
bool executable_in_path(const std::string& path, const std::string& name, std::string& resolved) {
    size_t start = 0;
    while (start <= path.size()) {
        const size_t end = path.find(':', start);
        const std::string directory =
            path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!directory.empty() && directory.front() == '/') {
            const std::string candidate = directory + "/" + name;
            if (access(candidate.c_str(), X_OK) == 0) {
                resolved = candidate;
                return true;
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

void append_safe_environment(std::vector<std::string>& environment, const char* name) {
    const std::string value = platform::environment_value(name);
    if (!value.empty()) environment.push_back(std::string(name) + "=" + value);
}

SystemClipboardResult run_helper(const ClipboardCommand& command,
                                 const std::string* input,
                                 runtime::CancellationToken token) {
    SystemClipboardResult result;
    result.backend = command.backend;
    runtime::SubprocessOptions options;
    options.executable = command.executable;
    options.arguments = command.arguments;
    options.provide_stdin = input != nullptr;
    if (input != nullptr) options.stdin_text = *input;
    options.timeout_ms = kClipboardHelperTimeoutMs;
    options.stdout_limit = kExternalClipboardReadLimit + 1U;
    options.stderr_limit = 4096;
    options.cancellation = token;
    options.environment.push_back("PATH=" + platform::environment_value("PATH"));
    const char* safe_names[] = {"HOME", "DISPLAY", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR",
                                "XAUTHORITY", "DBUS_SESSION_BUS_ADDRESS", "TERMUX_VERSION",
                                "PREFIX"};
    for (const char* name : safe_names) append_safe_environment(options.environment, name);
    runtime::SubprocessResult process;
    const Error error = runtime::run_subprocess(options, process);
    result.text = std::move(process.stdout_text);
    if (process.termination == runtime::SubprocessTerminationReason::Cancelled) {
        result.error = SystemClipboardError::Cancelled;
        result.message = "clipboard operation cancelled";
        return result;
    }
    if (process.termination == runtime::SubprocessTerminationReason::TimedOut) {
        result.error = SystemClipboardError::Timeout;
        result.message = command.backend + " clipboard helper timed out after two seconds";
        return result;
    }
    if (!error.ok() || process.exit_code != 0) {
        result.error = SystemClipboardError::Failed;
        result.message = !error.message.empty()
                             ? error.message
                             : command.backend + " clipboard helper exited unsuccessfully";
        return result;
    }
    if (input != nullptr) {
        if (process.stdin_incomplete) {
            result.error = SystemClipboardError::Failed;
            result.message = command.backend +
                             " clipboard helper closed stdin before accepting all text";
        }
        return result;
    }
    if (process.stdout_truncated || result.text.size() > kExternalClipboardReadLimit) {
        result.text.clear();
        result.error = SystemClipboardError::TooLarge;
        result.message = "system clipboard exceeds the 16 MiB text limit";
        return result;
    }
    if (result.text.empty()) {
        result.error = SystemClipboardError::Empty;
        result.message = "system clipboard contains no text";
    } else if (result.text.find('\0') != std::string::npos) {
        result.error = SystemClipboardError::NonText;
        result.message = "system clipboard is not text";
    } else if (process.stdout_repaired_utf8 || !html::is_valid_utf8(result.text)) {
        result.error = SystemClipboardError::Malformed;
        result.message = "system clipboard text is not valid UTF-8";
    }
    return result;
}
#else
bool native_clipboard_open(void*) { return OpenClipboard(nullptr) != FALSE; }
void native_clipboard_close(void*) { (void)CloseClipboard(); }
bool native_clipboard_unicode_available(void*) {
    return IsClipboardFormatAvailable(CF_UNICODETEXT) != FALSE;
}
void* native_clipboard_get_unicode(void*) {
    return GetClipboardData(CF_UNICODETEXT);
}
std::size_t native_clipboard_global_size(void*, void* object) {
    return static_cast<std::size_t>(GlobalSize(static_cast<HGLOBAL>(object)));
}
void* native_clipboard_global_lock(void*, void* object) {
    return GlobalLock(static_cast<HGLOBAL>(object));
}
void native_clipboard_global_unlock(void*, void* object) {
    (void)GlobalUnlock(static_cast<HGLOBAL>(object));
}
bool native_clipboard_empty(void*) { return EmptyClipboard() != FALSE; }
void* native_clipboard_global_alloc(void*, std::size_t bytes) {
    return GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(bytes));
}
bool native_clipboard_set_unicode(void*, void* memory) {
    return SetClipboardData(CF_UNICODETEXT, static_cast<HGLOBAL>(memory)) != nullptr;
}
void native_clipboard_global_free(void*, void* memory) {
    (void)GlobalFree(static_cast<HGLOBAL>(memory));
}
unsigned long native_clipboard_last_error(void*) { return GetLastError(); }
void native_clipboard_wait(void*, unsigned long milliseconds) { Sleep(milliseconds); }

const WindowsClipboardApiForTests kNativeClipboardApi = {
    nullptr,
    native_clipboard_open,
    native_clipboard_close,
    native_clipboard_unicode_available,
    native_clipboard_get_unicode,
    native_clipboard_global_size,
    native_clipboard_global_lock,
    native_clipboard_global_unlock,
    native_clipboard_empty,
    native_clipboard_global_alloc,
    native_clipboard_set_unicode,
    native_clipboard_global_free,
    native_clipboard_last_error,
    native_clipboard_wait,
};
const WindowsClipboardApiForTests* windows_clipboard_api_override = nullptr;

const WindowsClipboardApiForTests& windows_clipboard_api() {
    return windows_clipboard_api_override == nullptr ? kNativeClipboardApi
                                                     : *windows_clipboard_api_override;
}

class ClipboardGuard {
   public:
    ClipboardGuard() : api_(windows_clipboard_api()) {}
    ~ClipboardGuard() { if (open_) api_.close(api_.context); }
    bool open(runtime::CancellationToken token, SystemClipboardResult& result) {
        if (token.cancelled()) {
            result.error = SystemClipboardError::Cancelled;
            result.message = "clipboard operation cancelled";
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(kClipboardHelperTimeoutMs);
        while (!api_.open(api_.context)) {
            if (token.cancelled()) {
                result.error = SystemClipboardError::Cancelled;
                result.message = "clipboard operation cancelled";
                return false;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                result.error = SystemClipboardError::Timeout;
                result.message = "Windows clipboard remained busy for two seconds";
                return false;
            }
            api_.wait_ms(api_.context, 10);
        }
        open_ = true;
        return true;
    }
    const WindowsClipboardApiForTests& api() const { return api_; }
   private:
    const WindowsClipboardApiForTests& api_;
    bool open_ = false;
};

std::string normalize_clipboard_lf(std::string text) {
    std::size_t write = 0;
    for (std::size_t read = 0; read < text.size(); ++read) {
        if (text[read] == '\r') {
            if (read + 1 < text.size() && text[read + 1] == '\n') ++read;
            text[write++] = '\n';
        } else {
            text[write++] = text[read];
        }
    }
    text.resize(write);
    return text;
}

std::string clipboard_crlf(const std::string& text) {
    std::string output;
    output.reserve(text.size() + text.size() / 16U);
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char ch = text[index];
        if (ch == '\n' && (index == 0 || text[index - 1] != '\r')) output.push_back('\r');
        output.push_back(ch);
    }
    return output;
}

SystemClipboardResult read_windows_clipboard(runtime::CancellationToken token) {
    SystemClipboardResult result;
    result.backend = "Windows";
    ClipboardGuard clipboard;
    if (!clipboard.open(token, result)) return result;
    const WindowsClipboardApiForTests& api = clipboard.api();
    void* object = api.get_unicode_text(api.context);
    if (object == nullptr) {
        result.error = api.unicode_text_available(api.context)
                           ? SystemClipboardError::Failed
                           : SystemClipboardError::NonText;
        result.message = result.error == SystemClipboardError::NonText
                             ? "Windows clipboard does not contain Unicode text"
                             : "could not read Windows clipboard data: " +
                                   platform::windows_error_message(api.last_error(api.context));
        return result;
    }
    const std::size_t bytes = api.global_size(api.context, object);
    if (bytes < sizeof(wchar_t)) {
        result.error = SystemClipboardError::Empty;
        result.message = "system clipboard contains no text";
        return result;
    }
    const std::size_t capacity = static_cast<std::size_t>(bytes / sizeof(wchar_t));
    if (capacity > kExternalClipboardReadLimit + 1U) {
        result.error = SystemClipboardError::TooLarge;
        result.message = "system clipboard exceeds the 16 MiB text limit";
        return result;
    }
    const wchar_t* locked =
        static_cast<const wchar_t*>(api.global_lock(api.context, object));
    if (locked == nullptr) {
        result.error = SystemClipboardError::Failed;
        result.message = "could not lock Windows clipboard data: " +
                         platform::windows_error_message(api.last_error(api.context));
        return result;
    }
    std::size_t length = 0;
    while (length < capacity && locked[length] != L'\0') ++length;
    std::wstring wide(locked, length);
    api.global_unlock(api.context, object);
    if (length == capacity) {
        result.error = SystemClipboardError::Malformed;
        result.message = "Windows clipboard text is not NUL terminated";
        return result;
    }
    Error conversion = platform::utf16_to_utf8(wide, result.text);
    if (!conversion.ok()) {
        result.error = SystemClipboardError::Malformed;
        result.message = conversion.message;
        return result;
    }
    result.text = normalize_clipboard_lf(std::move(result.text));
    if (result.text.size() > kExternalClipboardReadLimit) {
        result.text.clear();
        result.error = SystemClipboardError::TooLarge;
        result.message = "system clipboard exceeds the 16 MiB text limit";
    } else if (result.text.empty()) {
        result.error = SystemClipboardError::Empty;
        result.message = "system clipboard contains no text";
    }
    return result;
}

SystemClipboardResult write_windows_clipboard(const std::string& text,
                                              runtime::CancellationToken token) {
    SystemClipboardResult result;
    result.backend = "Windows";
    if (text.size() > kExternalClipboardReadLimit) {
        result.error = SystemClipboardError::TooLarge;
        result.message = "system clipboard exceeds the 16 MiB text limit";
        return result;
    }
    if (!html::is_valid_utf8(text) || text.find('\0') != std::string::npos) {
        result.error = SystemClipboardError::Malformed;
        result.message = "clipboard text is not valid UTF-8";
        return result;
    }
    std::wstring wide;
    Error conversion = platform::utf8_to_utf16(clipboard_crlf(text), wide);
    if (!conversion.ok()) {
        result.error = SystemClipboardError::Malformed;
        result.message = conversion.message;
        return result;
    }
    ClipboardGuard clipboard;
    if (!clipboard.open(token, result)) return result;
    const WindowsClipboardApiForTests& api = clipboard.api();
    if (!api.empty(api.context)) {
        result.error = SystemClipboardError::Failed;
        result.message = "could not empty Windows clipboard: " +
                         platform::windows_error_message(api.last_error(api.context));
        return result;
    }
    const std::size_t bytes = (wide.size() + 1U) * sizeof(wchar_t);
    void* memory = api.global_alloc(api.context, bytes);
    if (memory == nullptr) {
        result.error = SystemClipboardError::Failed;
        result.message = "could not allocate Windows clipboard memory";
        return result;
    }
    wchar_t* destination =
        static_cast<wchar_t*>(api.global_lock(api.context, memory));
    if (destination == nullptr) {
        const unsigned long code = api.last_error(api.context);
        api.global_free(api.context, memory);
        result.error = SystemClipboardError::Failed;
        result.message = "could not lock Windows clipboard memory: " +
                         platform::windows_error_message(code);
        return result;
    }
    std::copy(wide.begin(), wide.end(), destination);
    destination[wide.size()] = L'\0';
    api.global_unlock(api.context, memory);
    if (!api.set_unicode_text(api.context, memory)) {
        const unsigned long code = api.last_error(api.context);
        api.global_free(api.context, memory);
        result.error = SystemClipboardError::Failed;
        result.message = "could not publish Windows clipboard text: " +
                         platform::windows_error_message(code);
    }
    // On success the clipboard owns memory and is responsible for GlobalFree.
    return result;
}
#endif

}  // namespace

void Clipboard::set(std::string text) {
    text_ = std::move(text);
}

void Clipboard::clear() {
    text_.clear();
}

Clipboard& shared_clipboard() {
    static Clipboard clipboard;
    return clipboard;
}

#if defined(_WIN32)
void set_windows_clipboard_api_for_tests(const WindowsClipboardApiForTests* api) {
    windows_clipboard_api_override = api;
}
#endif

ClipboardEnvironment current_clipboard_environment() {
    ClipboardEnvironment environment;
    environment.path = getenv_copy("PATH");
#if defined(_WIN32)
    environment.windows = true;
#endif
#if defined(__APPLE__)
    environment.macos = true;
#endif
    environment.wayland = !getenv_copy("WAYLAND_DISPLAY").empty();
    environment.x11 = !getenv_copy("DISPLAY").empty();
    environment.termux =
        !getenv_copy("TERMUX_VERSION").empty() ||
        (!getenv_copy("PREFIX").empty() &&
         getenv_copy("PREFIX").find("com.termux") != std::string::npos);
    const std::string release = getenv_copy("WSL_DISTRO_NAME") + getenv_copy("WSL_INTEROP");
    environment.wsl = !release.empty();
    environment.ssh = !getenv_copy("SSH_CONNECTION").empty() ||
                      !getenv_copy("SSH_TTY").empty() ||
                      !getenv_copy("SSH_CLIENT").empty();
    return environment;
}

bool prefer_terminal_clipboard_query(const ClipboardEnvironment& environment) {
    return environment.ssh;
}

bool resolve_clipboard_command(const ClipboardEnvironment& environment,
                               bool write,
                               ClipboardCommand& command) {
    if (environment.windows) return false;
    auto choose = [&](const char* backend, const char* name,
                      std::vector<std::string> arguments) {
        std::string executable;
        if (!executable_in_path(environment.path, name, executable)) return false;
        command = {backend, std::move(executable), std::move(arguments)};
        return true;
    };
    if (environment.termux &&
        choose("Termux", write ? "termux-clipboard-set" : "termux-clipboard-get", {}))
        return true;
    if (environment.wsl) {
        if (write && choose("WSL", "clip.exe", {})) return true;
        if (!write &&
            choose("WSL", "powershell.exe",
                   {"-NoProfile", "-NonInteractive", "-Command", "Get-Clipboard -Raw"}))
            return true;
    }
    if (environment.macos && choose("macOS", write ? "pbcopy" : "pbpaste", {})) return true;
    if (environment.wayland &&
        choose("Wayland", write ? "wl-copy" : "wl-paste",
               write ? std::vector<std::string>{"--type", "text/plain;charset=utf-8"}
                     : std::vector<std::string>{}))
        return true;
    if (environment.x11 &&
        choose("X11 xclip", "xclip",
               {"-selection", "clipboard", write ? "-in" : "-out"}))
        return true;
    if (environment.x11 &&
        choose("X11 xsel", "xsel",
               {"--clipboard", write ? "--input" : "--output"}))
        return true;
    return false;
}

SystemClipboardResult read_system_clipboard(const ClipboardEnvironment& environment,
                                            runtime::CancellationToken token) {
#if defined(_WIN32)
    (void)environment;
    return read_windows_clipboard(token);
#else
    ClipboardCommand command;
    if (!resolve_clipboard_command(environment, false, command)) {
        return {SystemClipboardError::Unavailable, "", "",
                "no supported system clipboard reader was found"};
    }
    return run_helper(command, nullptr, token);
#endif
}

SystemClipboardResult write_system_clipboard(const ClipboardEnvironment& environment,
                                             const std::string& text,
                                             runtime::CancellationToken token) {
#if defined(_WIN32)
    (void)environment;
    return write_windows_clipboard(text, token);
#else
    ClipboardCommand command;
    if (!resolve_clipboard_command(environment, true, command)) {
        return {SystemClipboardError::Unavailable, "", "",
                "no supported system clipboard writer was found"};
    }
    return run_helper(command, &text, token);
#endif
}

std::string clipboard_failure_help(const ClipboardEnvironment& environment,
                                   const SystemClipboardResult& result,
                                   bool reading) {
    if (result.error != SystemClipboardError::Unavailable)
        return result.message +
               (reading ? "; use your terminal paste shortcut" : "");
    std::string suggestion;
    if (environment.windows) suggestion = "use Windows Terminal or modern conhost";
    else if (environment.wayland) suggestion = "install wl-clipboard";
    else if (environment.x11) suggestion = "install xclip or xsel";
    else if (environment.termux) suggestion = "install Termux:API";
    else if (environment.wsl) suggestion = "ensure clip.exe and powershell.exe are in PATH";
    else if (environment.macos) suggestion = "ensure /usr/bin is in PATH";
    else suggestion = "install wl-clipboard, xclip, or xsel";
    return result.message + "; " + suggestion +
           (reading ? ", or use your terminal paste shortcut" : "");
}

ClipboardRuntime::~ClipboardRuntime() {
    cancel_all();
}

std::uint64_t ClipboardRuntime::start_read(const ClipboardEnvironment& environment) {
    read_job_.cancel();
    read_job_.join();
    const std::uint64_t generation = ++next_generation_;
    read_job_.start([this, environment, generation](runtime::CancellationToken token) {
        ClipboardRuntimeEvent event;
        event.type = ClipboardRuntimeEventType::ReadFinished;
        event.generation = generation;
        event.result = read_system_clipboard(environment, token);
        events_.push(std::move(event));
    });
    return generation;
}

std::uint64_t ClipboardRuntime::start_write(const ClipboardEnvironment& environment,
                                            std::string text) {
    write_job_.cancel();
    write_job_.join();
    const std::uint64_t generation = ++next_generation_;
    write_job_.start([this, environment, generation, text = std::move(text)](
                         runtime::CancellationToken token) {
        ClipboardRuntimeEvent event;
        event.type = ClipboardRuntimeEventType::WriteFinished;
        event.generation = generation;
        event.result = write_system_clipboard(environment, text, token);
        events_.push(std::move(event));
    });
    return generation;
}

void ClipboardRuntime::cancel_read() {
    read_job_.cancel();
    read_job_.join();
    ++next_generation_;
}

void ClipboardRuntime::cancel_all() {
    read_job_.cancel();
    write_job_.cancel();
    read_job_.join();
    write_job_.join();
    events_.close();
}

bool ClipboardRuntime::try_pop(ClipboardRuntimeEvent& event) {
    return events_.try_pop(event);
}

}  // namespace ainiux::editor
