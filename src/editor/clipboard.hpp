#pragma once

#include "common.hpp"
#include "runtime/runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ainiux::editor {

class Clipboard {
   public:
    void set(std::string text);
    const std::string& text() const { return text_; }
    bool empty() const { return text_.empty(); }
    void clear();

   private:
    std::string text_;
};

Clipboard& shared_clipboard();

constexpr size_t kExternalClipboardReadLimit = 16U * 1024U * 1024U;
constexpr int kClipboardHelperTimeoutMs = 2000;

enum class SystemClipboardError {
    None,
    Unavailable,
    Failed,
    Timeout,
    Cancelled,
    Empty,
    TooLarge,
    NonText,
    Malformed
};

struct ClipboardCommand {
    std::string backend;
    std::string executable;
    std::vector<std::string> arguments;
};

struct SystemClipboardResult {
    SystemClipboardError error = SystemClipboardError::None;
    std::string backend;
    std::string text;
    std::string message;

    bool ok() const { return error == SystemClipboardError::None; }
};

struct ClipboardEnvironment {
    std::string path;
    bool windows = false;
    bool macos = false;
    bool wayland = false;
    bool x11 = false;
    bool termux = false;
    bool wsl = false;
    bool ssh = false;
};

#if defined(_WIN32)
// Narrow test seam around the Win32 clipboard calls. Production uses the
// native implementation when this override is null; unit tests install a
// process-local fake to cover busy/cancel, conversion, and ownership paths
// without changing the user's clipboard.
struct WindowsClipboardApiForTests {
    void* context = nullptr;
    bool (*open)(void*) = nullptr;
    void (*close)(void*) = nullptr;
    bool (*unicode_text_available)(void*) = nullptr;
    void* (*get_unicode_text)(void*) = nullptr;
    std::size_t (*global_size)(void*, void*) = nullptr;
    void* (*global_lock)(void*, void*) = nullptr;
    void (*global_unlock)(void*, void*) = nullptr;
    bool (*empty)(void*) = nullptr;
    void* (*global_alloc)(void*, std::size_t) = nullptr;
    bool (*set_unicode_text)(void*, void*) = nullptr;
    void (*global_free)(void*, void*) = nullptr;
    unsigned long (*last_error)(void*) = nullptr;
    void (*wait_ms)(void*, unsigned long) = nullptr;
};

void set_windows_clipboard_api_for_tests(const WindowsClipboardApiForTests* api);
#endif

ClipboardEnvironment current_clipboard_environment();
bool prefer_terminal_clipboard_query(const ClipboardEnvironment& environment);
bool resolve_clipboard_command(const ClipboardEnvironment& environment,
                               bool write,
                               ClipboardCommand& command);
SystemClipboardResult read_system_clipboard(const ClipboardEnvironment& environment,
                                            runtime::CancellationToken token);
SystemClipboardResult write_system_clipboard(const ClipboardEnvironment& environment,
                                             const std::string& text,
                                             runtime::CancellationToken token);
std::string clipboard_failure_help(const ClipboardEnvironment& environment,
                                   const SystemClipboardResult& result,
                                   bool reading);

enum class ClipboardRuntimeEventType { ReadFinished, WriteFinished };

struct ClipboardRuntimeEvent {
    ClipboardRuntimeEventType type = ClipboardRuntimeEventType::ReadFinished;
    std::uint64_t generation = 0;
    SystemClipboardResult result;
};

class ClipboardRuntime {
   public:
    ClipboardRuntime() = default;
    ~ClipboardRuntime();
    ClipboardRuntime(const ClipboardRuntime&) = delete;
    ClipboardRuntime& operator=(const ClipboardRuntime&) = delete;

    std::uint64_t start_read(const ClipboardEnvironment& environment);
    std::uint64_t start_write(const ClipboardEnvironment& environment, std::string text);
    void cancel_read();
    void cancel_all();
    bool try_pop(ClipboardRuntimeEvent& event);
    bool read_running() const { return read_job_.running(); }

   private:
    std::uint64_t next_generation_ = 0;
    runtime::EventQueue<ClipboardRuntimeEvent> events_;
    runtime::JobHandle read_job_;
    runtime::JobHandle write_job_;
};

}  // namespace ainiux::editor
