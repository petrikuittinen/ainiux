#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "runtime/subprocess.hpp"

#include "platform/windows_utf.hpp"
#include "runtime/subprocess_detail.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ainiux::runtime {
namespace {

class Handle {
   public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.release()) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    HANDLE get() const { return value_; }
    HANDLE release() { HANDLE value = value_; value_ = nullptr; return value; }
    void reset(HANDLE value = nullptr) {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_ = value;
    }
    explicit operator bool() const {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }
   private:
    HANDLE value_ = nullptr;
};

std::string windows_error(const char* action, DWORD code = GetLastError());

class AttributeList {
   public:
    AttributeList() = default;
    ~AttributeList() {
        if (list_ != nullptr) {
            if (initialized_) DeleteProcThreadAttributeList(list_);
            HeapFree(GetProcessHeap(), 0, list_);
        }
    }
    AttributeList(const AttributeList&) = delete;
    AttributeList& operator=(const AttributeList&) = delete;

    Error initialize(const std::vector<HANDLE>& handles) {
        SIZE_T bytes = 0;
        (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        if (bytes == 0)
            return {ErrorCode::Internal,
                    windows_error("could not size subprocess handle list")};
        list_ = static_cast<PPROC_THREAD_ATTRIBUTE_LIST>(
            HeapAlloc(GetProcessHeap(), 0, bytes));
        if (list_ == nullptr)
            return {ErrorCode::Internal,
                    "could not allocate subprocess handle list"};
        if (!InitializeProcThreadAttributeList(list_, 1, 0, &bytes))
            return {ErrorCode::Internal,
                    windows_error("could not initialize subprocess handle list")};
        initialized_ = true;
        if (!UpdateProcThreadAttribute(list_, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       const_cast<HANDLE*>(handles.data()),
                                       handles.size() * sizeof(HANDLE), nullptr, nullptr))
            return {ErrorCode::Internal,
                    windows_error("could not restrict subprocess inherited handles")};
        return ok_error();
    }
    PPROC_THREAD_ATTRIBUTE_LIST get() const { return list_; }

   private:
    PPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
    bool initialized_ = false;
};

std::string windows_error(const char* action, DWORD code) {
    return std::string(action) + ": " + platform::windows_error_message(code);
}

Error make_pipe(Handle& read_end, Handle& write_end, bool inherit_read, bool inherit_write) {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE read_value = nullptr;
    HANDLE write_value = nullptr;
    if (!CreatePipe(&read_value, &write_value, &attributes, 0))
        return {ErrorCode::Internal, windows_error("could not create subprocess pipe")};
    read_end.reset(read_value);
    write_end.reset(write_value);
    if (!inherit_read && !SetHandleInformation(read_end.get(), HANDLE_FLAG_INHERIT, 0))
        return {ErrorCode::Internal, windows_error("could not protect subprocess pipe")};
    if (!inherit_write && !SetHandleInformation(write_end.get(), HANDLE_FLAG_INHERIT, 0))
        return {ErrorCode::Internal, windows_error("could not protect subprocess pipe")};
    return ok_error();
}

std::wstring quote_argument(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return argument;
    std::wstring out = L"\"";
    std::size_t slashes = 0;
    for (wchar_t ch : argument) {
        if (ch == L'\\') {
            ++slashes;
        } else if (ch == L'\"') {
            out.append(slashes * 2 + 1, L'\\');
            out.push_back(ch);
            slashes = 0;
        } else {
            out.append(slashes, L'\\');
            slashes = 0;
            out.push_back(ch);
        }
    }
    out.append(slashes * 2, L'\\');
    out.push_back(L'\"');
    return out;
}

Error make_command_line(const SubprocessOptions& options, std::wstring& line) {
    if (options.windows_batch) {
        if (options.arguments.empty())
            return {ErrorCode::BadArgs, "Windows batch subprocess is missing its script"};
        std::wstring batch_command = L"\"";
        for (std::size_t index = 0; index < options.arguments.size(); ++index) {
            std::wstring wide;
            Error error = platform::utf8_to_utf16(options.arguments[index], wide);
            if (!error.ok()) return error;
            if (wide.find_first_of(L"\"&|<>^%!()\r\n") != std::wstring::npos)
                return {ErrorCode::BadArgs,
                        "Windows batch argument contains cmd.exe expansion metacharacters"};
            if (index != 0) batch_command.push_back(L' ');
            batch_command += quote_argument(wide);
        }
        batch_command.push_back(L'\"');
        std::wstring executable;
        Error error = platform::utf8_to_utf16(options.executable, executable);
        if (!error.ok()) return error;
        line = quote_argument(executable) + L" /d /s /c " + batch_command;
        return ok_error();
    }
    std::vector<std::string> values;
    values.reserve(options.arguments.size() + 1);
    values.push_back(options.executable);
    values.insert(values.end(), options.arguments.begin(), options.arguments.end());
    line.clear();
    for (const std::string& value : values) {
        std::wstring wide;
        Error error = platform::utf8_to_utf16(value, wide);
        if (!error.ok()) return error;
        if (!line.empty()) line.push_back(L' ');
        line += quote_argument(wide);
    }
    return ok_error();
}

Error make_environment(const std::vector<std::string>& values, std::vector<wchar_t>& block) {
    std::vector<std::wstring> entries;
    entries.reserve(values.size());
    for (const std::string& value : values) {
        const std::size_t equals = value.find('=');
        if (equals == std::string::npos || equals == 0)
            return {ErrorCode::BadArgs,
                    "subprocess environment entries must use non-empty NAME=VALUE form"};
        std::wstring wide;
        Error error = platform::utf8_to_utf16(value, wide);
        if (!error.ok()) return error;
        entries.push_back(std::move(wide));
    }
    std::stable_sort(entries.begin(), entries.end(), [](const std::wstring& left,
                                                        const std::wstring& right) {
        return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()),
                                    right.c_str(), static_cast<int>(right.size()), TRUE) ==
               CSTR_LESS_THAN;
    });
    for (std::size_t index = 1; index < entries.size(); ++index) {
        const std::size_t previous_equals = entries[index - 1].find(L'=');
        const std::size_t current_equals = entries[index].find(L'=');
        if (previous_equals == current_equals &&
            CompareStringOrdinal(entries[index - 1].c_str(),
                                 static_cast<int>(previous_equals),
                                 entries[index].c_str(),
                                 static_cast<int>(current_equals), TRUE) == CSTR_EQUAL) {
            return {ErrorCode::BadArgs,
                    "subprocess environment contains a duplicate variable name"};
        }
    }
    block.clear();
    std::size_t size = 1;
    for (const std::wstring& entry : entries) size += entry.size() + 1;
    block.reserve(size);
    for (const std::wstring& entry : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    if (entries.empty()) block.push_back(L'\0');
    return ok_error();
}

void read_pipe(HANDLE pipe,
               std::string& output,
               std::size_t limit,
               bool& truncated,
               std::mutex& mutex) {
    char buffer[8192];
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(pipe, buffer, sizeof(buffer), &count, nullptr) || count == 0) break;
        std::lock_guard<std::mutex> lock(mutex);
        detail::append_bounded(output, buffer, static_cast<std::size_t>(count), limit,
                               truncated);
    }
}

void write_pipe(HANDLE pipe, const std::string& input, bool& incomplete) {
    std::size_t offset = 0;
    while (offset < input.size()) {
        const DWORD wanted = static_cast<DWORD>(
            std::min<std::size_t>(input.size() - offset, 64U * 1024U));
        DWORD written = 0;
        if (!WriteFile(pipe, input.data() + offset, wanted, &written, nullptr) || written == 0)
            break;
        offset += written;
    }
    incomplete = offset < input.size();
    CloseHandle(pipe);
}

Error validate_options(const SubprocessOptions& options) {
    if (options.executable.empty())
        return {ErrorCode::BadArgs, "subprocess executable is empty"};
    if (options.executable.find('\0') != std::string::npos ||
        options.cwd.find('\0') != std::string::npos)
        return {ErrorCode::BadArgs, "subprocess path contains a NUL byte"};
    for (const std::string& argument : options.arguments) {
        if (argument.find('\0') != std::string::npos)
            return {ErrorCode::BadArgs, "subprocess argument contains a NUL byte"};
    }
    for (const std::string& entry : options.environment) {
        const std::size_t equals = entry.find('=');
        if (equals == std::string::npos || equals == 0 ||
            entry.find('\0') != std::string::npos)
            return {ErrorCode::BadArgs,
                    "subprocess environment entries must use non-empty NAME=VALUE form"};
    }
    return ok_error();
}

struct WinBackgroundJob {
    Handle process;
    Handle job;
    Handle stdout_read;
    Handle stderr_read;
    std::shared_ptr<std::string> stdout_text;
    std::shared_ptr<std::string> stderr_text;
    std::shared_ptr<bool> stdout_truncated;
    std::shared_ptr<bool> stderr_truncated;
    std::shared_ptr<std::mutex> output_mutex;
    std::thread stdout_thread;
    std::thread stderr_thread;
    std::thread lifetime;
    long timeout_ms = 0;
    std::chrono::steady_clock::time_point started;
};

std::mutex g_background_mutex;
std::map<std::int64_t, std::shared_ptr<WinBackgroundJob>> g_background_jobs;

void windows_lifetime_watch(std::shared_ptr<WinBackgroundJob> job) {
    for (;;) {
        const DWORD waited = WaitForSingleObject(job->process.get(), 50);
        if (waited == WAIT_OBJECT_0) break;
        const auto now = std::chrono::steady_clock::now();
        const long long elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - job->started)
                .count();
        if (job->timeout_ms > 0 && elapsed >= job->timeout_ms) {
            (void)TerminateJobObject(job->job.get(), WAIT_TIMEOUT);
            (void)WaitForSingleObject(job->process.get(), INFINITE);
            break;
        }
        if (waited == WAIT_FAILED) {
            (void)TerminateJobObject(job->job.get(), 126);
            break;
        }
    }
    if (job->stdout_thread.joinable()) job->stdout_thread.join();
    if (job->stderr_thread.joinable()) job->stderr_thread.join();
    job->job.reset();
    std::lock_guard<std::mutex> lock(g_background_mutex);
    for (auto it = g_background_jobs.begin(); it != g_background_jobs.end();) {
        if (it->second == job)
            it = g_background_jobs.erase(it);
        else
            ++it;
    }
}

}  // namespace

Error kill_background_process(std::int64_t pid) {
    if (pid <= 0) return ok_error();
    std::shared_ptr<WinBackgroundJob> job;
    {
        std::lock_guard<std::mutex> lock(g_background_mutex);
        const auto found = g_background_jobs.find(pid);
        if (found != g_background_jobs.end()) job = found->second;
    }
    if (job && job->job) (void)TerminateJobObject(job->job.get(), ERROR_CANCELLED);
    return ok_error();
}

void kill_all_background_processes() {
    std::vector<std::int64_t> pids;
    {
        std::lock_guard<std::mutex> lock(g_background_mutex);
        for (const auto& entry : g_background_jobs) pids.push_back(entry.first);
    }
    for (const std::int64_t pid : pids) (void)kill_background_process(pid);
}

Error run_subprocess(const SubprocessOptions& options, SubprocessResult& result) {
    result = SubprocessResult{};
    Error error = validate_options(options);
    if (!error.ok()) return error;

    std::wstring executable;
    std::wstring cwd;
    std::wstring command_line;
    std::vector<wchar_t> environment;
    error = platform::utf8_to_utf16(options.executable, executable);
    if (!error.ok()) return error;
    if (!options.cwd.empty()) {
        error = platform::utf8_to_utf16(options.cwd, cwd);
        if (!error.ok()) return error;
    }
    if (!(error = make_command_line(options, command_line)).ok() ||
        !(error = make_environment(options.environment, environment)).ok())
        return error;

    Handle stdout_read;
    Handle stdout_write;
    Handle stderr_read;
    Handle stderr_write;
    Handle stdin_read;
    Handle stdin_write;
    if (!(error = make_pipe(stdout_read, stdout_write, false, true)).ok() ||
        !(error = make_pipe(stderr_read, stderr_write, false, true)).ok() ||
        (options.provide_stdin &&
         !(error = make_pipe(stdin_read, stdin_write, true, false)).ok()))
        return error;
    Handle null_input;
    if (!options.provide_stdin) {
        null_input.reset(CreateFileW(L"NUL", GENERIC_READ,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!null_input)
            return {ErrorCode::Internal, windows_error("could not open NUL")};
        if (!SetHandleInformation(null_input.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
            return {ErrorCode::Internal, windows_error("could not inherit NUL handle")};
    }

    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) return {ErrorCode::Internal, windows_error("could not create subprocess job")};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits)))
        return {ErrorCode::Internal, windows_error("could not configure subprocess job")};

    const HANDLE child_stdin = options.provide_stdin ? stdin_read.get() : null_input.get();
    AttributeList inherited_handles;
    error = inherited_handles.initialize({child_stdin, stdout_write.get(), stderr_write.get()});
    if (!error.ok()) return error;
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = child_stdin;
    startup.StartupInfo.hStdOutput = stdout_write.get();
    startup.StartupInfo.hStdError = stderr_write.get();
    startup.lpAttributeList = inherited_handles.get();
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutable_line(command_line.begin(), command_line.end());
    mutable_line.push_back(L'\0');
    const auto started = std::chrono::steady_clock::now();
    if (!CreateProcessW(executable.c_str(), mutable_line.data(), nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW |
                            EXTENDED_STARTUPINFO_PRESENT,
                        environment.data(), cwd.empty() ? nullptr : cwd.c_str(),
                        &startup.StartupInfo, &process)) {
        result.termination = SubprocessTerminationReason::SpawnFailed;
        return {ErrorCode::FileRead,
                windows_error(("could not start subprocess " + options.executable).c_str())};
    }
    Handle process_handle(process.hProcess);
    Handle thread_handle(process.hThread);
    if (!AssignProcessToJobObject(job.get(), process_handle.get())) {
        (void)TerminateProcess(process_handle.get(), 126);
        return {ErrorCode::Internal,
                windows_error("could not assign subprocess to its job")};
    }
    if (ResumeThread(thread_handle.get()) == static_cast<DWORD>(-1)) {
        (void)TerminateJobObject(job.get(), 126);
        return {ErrorCode::Internal, windows_error("could not resume subprocess")};
    }
    thread_handle.reset();
    stdout_write.reset();
    stderr_write.reset();
    stdin_read.reset();
    null_input.reset();

    auto live_stdout = std::make_shared<std::string>();
    auto live_stderr = std::make_shared<std::string>();
    auto live_out_trunc = std::make_shared<bool>(false);
    auto live_err_trunc = std::make_shared<bool>(false);
    auto output_mutex = std::make_shared<std::mutex>();
    std::thread stdout_thread;
    std::thread stderr_thread;
    std::thread stdin_thread;
    try {
        stdout_thread =
            std::thread(read_pipe, stdout_read.get(), std::ref(*live_stdout),
                        options.stdout_limit, std::ref(*live_out_trunc),
                        std::ref(*output_mutex));
        stderr_thread =
            std::thread(read_pipe, stderr_read.get(), std::ref(*live_stderr),
                        options.stderr_limit, std::ref(*live_err_trunc),
                        std::ref(*output_mutex));
        if (options.provide_stdin) {
            HANDLE input_writer = stdin_write.release();
            try {
                stdin_thread = std::thread(write_pipe, input_writer,
                                           std::cref(options.stdin_text),
                                           std::ref(result.stdin_incomplete));
            } catch (...) {
                CloseHandle(input_writer);
                throw;
            }
        }
    } catch (...) {
        result.termination = SubprocessTerminationReason::IoFailed;
        stdin_write.reset();
        (void)TerminateJobObject(job.get(), 126);
        (void)WaitForSingleObject(process_handle.get(), INFINITE);
        job.reset();
        if (stdin_thread.joinable()) stdin_thread.join();
        if (stdout_thread.joinable()) stdout_thread.join();
        if (stderr_thread.joinable()) stderr_thread.join();
        return {ErrorCode::Internal,
                "could not start subprocess pipe reader/writer threads"};
    }

    bool detached = false;
    const long startup_ms = options.startup_ms > 0 ? options.startup_ms : 400;
    for (;;) {
        const DWORD waited = WaitForSingleObject(process_handle.get(), 20);
        if (waited == WAIT_OBJECT_0) break;
        const auto now = std::chrono::steady_clock::now();
        const long long elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count();
        if (options.cancellation.cancelled()) {
            result.termination = SubprocessTerminationReason::Cancelled;
            (void)TerminateJobObject(job.get(), ERROR_CANCELLED);
            (void)WaitForSingleObject(process_handle.get(), INFINITE);
            break;
        }
        if (!options.background && options.timeout_ms > 0 && elapsed >= options.timeout_ms) {
            result.termination = SubprocessTerminationReason::TimedOut;
            (void)TerminateJobObject(job.get(), WAIT_TIMEOUT);
            (void)WaitForSingleObject(process_handle.get(), INFINITE);
            break;
        }
        if (options.background && elapsed >= startup_ms) {
            detached = true;
            break;
        }
        if (waited == WAIT_FAILED) {
            result.termination = SubprocessTerminationReason::IoFailed;
            (void)TerminateJobObject(job.get(), 126);
            (void)WaitForSingleObject(process_handle.get(), INFINITE);
            break;
        }
    }
    result.pid = static_cast<std::int64_t>(GetProcessId(process_handle.get()));
    result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    {
        std::lock_guard<std::mutex> lock(*output_mutex);
        result.stdout_text = *live_stdout;
        result.stderr_text = *live_stderr;
        result.stdout_truncated = *live_out_trunc;
        result.stderr_truncated = *live_err_trunc;
    }
    if (detached) {
        auto tracked = std::make_shared<WinBackgroundJob>();
        tracked->process = std::move(process_handle);
        tracked->job = std::move(job);
        tracked->stdout_read = std::move(stdout_read);
        tracked->stderr_read = std::move(stderr_read);
        tracked->stdout_text = std::move(live_stdout);
        tracked->stderr_text = std::move(live_stderr);
        tracked->stdout_truncated = std::move(live_out_trunc);
        tracked->stderr_truncated = std::move(live_err_trunc);
        tracked->output_mutex = std::move(output_mutex);
        tracked->stdout_thread = std::move(stdout_thread);
        tracked->stderr_thread = std::move(stderr_thread);
        tracked->timeout_ms = options.timeout_ms;
        tracked->started = started;
        if (stdin_thread.joinable()) stdin_thread.join();
        {
            std::lock_guard<std::mutex> lock(g_background_mutex);
            g_background_jobs[result.pid] = tracked;
        }
        tracked->lifetime = std::thread(windows_lifetime_watch, tracked);
        tracked->lifetime.detach();
        result.background = true;
        result.termination = SubprocessTerminationReason::Running;
        detail::normalize_output(result.stdout_text, options.stdout_limit,
                                 result.stdout_truncated, result.stdout_repaired_utf8);
        detail::normalize_output(result.stderr_text, options.stderr_limit,
                                 result.stderr_truncated, result.stderr_repaired_utf8);
        return ok_error();
    }
    // A command owns its descendants. Closing the kill-on-close job now prevents
    // a detached descendant from retaining a pipe and hanging the reader joins.
    job.reset();
    if (stdin_thread.joinable()) stdin_thread.join();
    if (stdout_thread.joinable()) stdout_thread.join();
    if (stderr_thread.joinable()) stderr_thread.join();
    stdout_read.reset();
    stderr_read.reset();

    DWORD exit_code = 0;
    if (GetExitCodeProcess(process_handle.get(), &exit_code))
        result.exit_code = static_cast<std::int64_t>(exit_code);
    if (result.termination != SubprocessTerminationReason::Cancelled &&
        result.termination != SubprocessTerminationReason::TimedOut &&
        result.termination != SubprocessTerminationReason::IoFailed)
        result.termination = SubprocessTerminationReason::Exited;
    detail::normalize_output(result.stdout_text, options.stdout_limit,
                             result.stdout_truncated, result.stdout_repaired_utf8);
    detail::normalize_output(result.stderr_text, options.stderr_limit,
                             result.stderr_truncated, result.stderr_repaired_utf8);

    if (result.termination == SubprocessTerminationReason::Cancelled)
        return {ErrorCode::Cancelled, "subprocess cancelled: " + options.executable};
    if (result.termination == SubprocessTerminationReason::TimedOut)
        return {ErrorCode::Timeout, "subprocess exceeded its timeout: " + options.executable};
    if (result.termination == SubprocessTerminationReason::IoFailed)
        return {ErrorCode::Internal, windows_error("could not wait for subprocess")};
    return ok_error();
}

}  // namespace ainiux::runtime

#endif
