#include "runtime/test_runtime.hpp"
#include "support/test_support.hpp"
#include "platform/environment.hpp"
#include "platform/filesystem.hpp"
#include "runtime/runtime.hpp"
#include "runtime/subprocess.hpp"
#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#else
#include <signal.h>
#endif

namespace ainiux::test::runtime {

namespace {

using ainiux::test::check;
using ainiux::test::read_fixture;
namespace fs = std::filesystem;

class TemporaryDirectory {
   public:
    TemporaryDirectory()
        : path_(fs::temp_directory_path() /
                fs::u8path("ainiux-subprocess-" +
                           std::to_string(ainiux::platform::current_process_id()) + u8"-ä")) {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
        fs::create_directories(path_, ignored);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    const fs::path& path() const { return path_; }

   private:
    fs::path path_;
};

std::string subprocess_fixture_path() {
#if defined(_WIN32)
    const char* name = "build/subprocess_fixture.exe";
#else
    const char* name = "build/subprocess_fixture";
#endif
    std::error_code error;
    return fs::absolute(fs::u8path(name), error).u8string();
}

#if defined(_WIN32)
class NativeHandle {
   public:
    explicit NativeHandle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~NativeHandle() {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
    }
    NativeHandle(const NativeHandle&) = delete;
    NativeHandle& operator=(const NativeHandle&) = delete;
    HANDLE get() const { return value_; }
    explicit operator bool() const {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }

   private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};

class LocalSecurityDescriptor {
   public:
    ~LocalSecurityDescriptor() {
        if (value_ != nullptr) LocalFree(value_);
    }
    LocalSecurityDescriptor(const LocalSecurityDescriptor&) = delete;
    LocalSecurityDescriptor& operator=(const LocalSecurityDescriptor&) = delete;
    LocalSecurityDescriptor() = default;
    PSECURITY_DESCRIPTOR* put() { return &value_; }
    PSECURITY_DESCRIPTOR get() const { return value_; }

   private:
    PSECURITY_DESCRIPTOR value_ = nullptr;
};

bool has_private_windows_dacl(const fs::path& path, bool directory) {
    NativeHandle file(CreateFileW(
        path.wstring().c_str(), READ_CONTROL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) return false;

    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) return false;
    NativeHandle token(raw_token);
    DWORD user_bytes = 0;
    (void)GetTokenInformation(token.get(), TokenUser, nullptr, 0, &user_bytes);
    if (user_bytes == 0) return false;
    std::vector<unsigned char> user_storage(user_bytes);
    if (!GetTokenInformation(token.get(), TokenUser, user_storage.data(), user_bytes,
                             &user_bytes))
        return false;
    PSID user_sid = reinterpret_cast<TOKEN_USER*>(user_storage.data())->User.Sid;
    std::array<unsigned char, SECURITY_MAX_SID_SIZE> system_storage{};
    DWORD system_bytes = static_cast<DWORD>(system_storage.size());
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system_storage.data(),
                            &system_bytes))
        return false;

    PACL dacl = nullptr;
    LocalSecurityDescriptor descriptor;
    if (GetSecurityInfo(file.get(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                        nullptr, nullptr, &dacl, nullptr, descriptor.put()) != ERROR_SUCCESS ||
        dacl == nullptr)
        return false;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    if (!GetSecurityDescriptorControl(descriptor.get(), &control, &revision) ||
        (control & SE_DACL_PROTECTED) == 0)
        return false;

    bool current_user_allowed = false;
    bool system_allowed = false;
    bool unexpected_ace = false;
    for (DWORD index = 0; index < dacl->AceCount; ++index) {
        void* raw_ace = nullptr;
        if (!GetAce(dacl, index, &raw_ace) || raw_ace == nullptr) return false;
        const ACE_HEADER* header = static_cast<const ACE_HEADER*>(raw_ace);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            unexpected_ace = true;
            continue;
        }
        const ACCESS_ALLOWED_ACE* ace =
            static_cast<const ACCESS_ALLOWED_ACE*>(raw_ace);
        PSID sid = const_cast<DWORD*>(&ace->SidStart);
        if (EqualSid(sid, user_sid))
            current_user_allowed = true;
        else if (EqualSid(sid, system_storage.data()))
            system_allowed = true;
        else
            unexpected_ace = true;
    }
    const bool same_principal = EqualSid(user_sid, system_storage.data()) != FALSE;
    const DWORD expected_aces = same_principal ? 1U : 2U;
    return current_user_allowed && (system_allowed || same_principal) &&
           !unexpected_ace && dacl->AceCount == expected_aces;
}
#endif

void test_runtime_event_queue_and_job_cancel() {
    ainiux::runtime::EventQueue<int> queue;
    int value = 0;
    check(!queue.try_pop(value), "empty runtime queue has no event");
    queue.push(7);
    check(queue.try_pop(value) && value == 7, "runtime queue preserves event value");

    ainiux::runtime::JobHandle job;
    std::atomic<bool> entered{false};
    job.start([&](ainiux::runtime::CancellationToken token) {
        entered.store(true, std::memory_order_release);
        while (!token.cancelled()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        queue.push(42);
    });
    for (int i = 0; i < 100 && !entered.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(job.running() && job.joinable(), "runtime job reports running and joinable");
    job.cancel();
    check(queue.wait_pop_for(value, std::chrono::milliseconds(1000)) && value == 42, "runtime job observes cancellation");
    job.join();
    check(!job.running() && !job.joinable(), "runtime job reports stopped after join");

    job.start([](ainiux::runtime::CancellationToken) {});
    for (int i = 0; i < 100 && job.running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(!job.running() && job.joinable(),
          "completed runtime job stays joinable until its event owner joins it");
    job.join();
}

void test_runtime_queue_timeout() {
    ainiux::runtime::EventQueue<int> queue;
    int value = 0;
    check(!queue.wait_pop_for(value, std::chrono::milliseconds(10)),
          "runtime queue wait_pop_for times out on an empty queue");
}

void test_background_jobs_supersede_without_blocking() {
    ainiux::runtime::BackgroundJobs jobs;
    std::atomic<bool> stale_started{false};
    std::atomic<bool> release_stale{false};
    std::atomic<bool> newest_finished{false};
    check(jobs.start([&](ainiux::runtime::CancellationToken) {
              stale_started.store(true, std::memory_order_release);
              while (!release_stale.load(std::memory_order_acquire))
                  std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }) == 1,
          "first background job receives generation one");
    for (int i = 0; i < 100 && !stale_started.load(std::memory_order_acquire); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    const auto before = std::chrono::steady_clock::now();
    const std::uint64_t generation =
        jobs.start([&](ainiux::runtime::CancellationToken) {
            newest_finished.store(true, std::memory_order_release);
        });
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - before);
    check(generation == 2 && elapsed.count() < 100,
          "superseding background work never joins a stalled worker on the caller");
    for (int i = 0; i < 100 && !newest_finished.load(std::memory_order_acquire); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    jobs.reap_finished();
    check(newest_finished.load(std::memory_order_acquire) && jobs.job_count() == 1,
          "latest background job can finish while stale work remains retired");
    release_stale.store(true, std::memory_order_release);
    jobs.shutdown();
    check(jobs.job_count() == 0, "background shutdown joins every generation");
}

void test_subprocess_utf8_io_and_status() {
    TemporaryDirectory temporary;
    ainiux::runtime::SubprocessOptions options;
    options.executable = subprocess_fixture_path();
    options.arguments = {"--inspect", u8"snowman-☃", "space argument"};
    options.cwd = temporary.path().u8string();
    options.environment = {u8"AINIUX_FIXTURE=välue"};
    options.provide_stdin = true;
    options.stdin_text = "alpha\r\nbeta\r";
    options.timeout_ms = 5000;
    ainiux::runtime::SubprocessResult result;
    const Error error = ainiux::runtime::run_subprocess(options, result);
    check(error.ok() && result.termination ==
                            ainiux::runtime::SubprocessTerminationReason::Exited &&
              result.exit_code == 0,
          "subprocess fixture exits normally");
    check(result.stdout_text.find("cwd:" + temporary.path().u8string()) != std::string::npos,
          "subprocess receives a Unicode cwd");
    check(result.stdout_text.find(u8"arg:snowman-☃\n") != std::string::npos &&
              result.stdout_text.find("arg:space argument\n") != std::string::npos,
          "subprocess preserves Unicode and spaced argv");
    check(result.stdout_text.find(u8"env:välue\n") != std::string::npos,
          "subprocess receives only the explicit UTF-8 environment");
    check(result.stdout_text.find("stdin:alpha\nbeta\n") != std::string::npos,
          "subprocess stdin and stdout normalize CRLF/CR to LF");
    check(result.stderr_text == "fixture-stderr\n",
          "subprocess stderr is captured and LF-normalized");

    options = ainiux::runtime::SubprocessOptions{};
    options.executable = subprocess_fixture_path();
    options.arguments = {"--exit", "37"};
    options.environment = {};
    check(ainiux::runtime::run_subprocess(options, result).ok() && result.exit_code == 37,
          "subprocess preserves a nonzero exit status");

    options.arguments = {"--invalid"};
    check(ainiux::runtime::run_subprocess(options, result).ok() &&
              result.stdout_text == u8"ok:�(�(\n" && result.stdout_repaired_utf8,
          "subprocess replaces malformed native output with valid UTF-8");

    options.arguments = {"--flood", "4096"};
    options.stdout_limit = 128;
    check(ainiux::runtime::run_subprocess(options, result).ok() &&
              result.stdout_text.size() == 128 && result.stdout_truncated,
          "subprocess output limits retain a bounded prefix and report truncation");

    options = ainiux::runtime::SubprocessOptions{};
    options.executable = subprocess_fixture_path();
    options.arguments = {std::string("bad\0argument", 12)};
    check(ainiux::runtime::run_subprocess(options, result).code == ErrorCode::BadArgs,
          "subprocess rejects embedded NUL bytes before native process creation");
#if defined(_WIN32)
    options.arguments = {"--exit", "0"};
    options.environment = {"Path=one", "PATH=two"};
    check(ainiux::runtime::run_subprocess(options, result).code == ErrorCode::BadArgs,
          "Windows subprocess rejects case-insensitive duplicate environment names");
    options.environment.clear();
    options.arguments = {"--exception"};
    options.stdout_limit = 1024;
    check(ainiux::runtime::run_subprocess(options, result).ok() &&
              result.exit_code == static_cast<std::int64_t>(0xE1234567U),
          "Windows subprocess preserves the unsigned exception exit code");
#endif
}

void test_subprocess_timeout_cancellation_and_descendants() {
    ainiux::runtime::SubprocessOptions options;
    options.executable = subprocess_fixture_path();
    options.arguments = {"--sleep", "5000"};
    options.timeout_ms = 100;
    ainiux::runtime::SubprocessResult result;
    Error error = ainiux::runtime::run_subprocess(options, result);
    check(error.code == ErrorCode::Timeout &&
              result.termination == ainiux::runtime::SubprocessTerminationReason::TimedOut,
          "subprocess timeout terminates a stalled child");

    ainiux::runtime::CancellationSource cancellation;
    options.timeout_ms = 5000;
    options.cancellation = cancellation.token();
    std::thread cancel_thread([cancellation]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        cancellation.cancel();
    });
    error = ainiux::runtime::run_subprocess(options, result);
    cancel_thread.join();
    check(error.code == ErrorCode::Cancelled &&
              result.termination == ainiux::runtime::SubprocessTerminationReason::Cancelled,
          "subprocess cancellation terminates a stalled child");

    TemporaryDirectory temporary;
    const fs::path marker = temporary.path() / "descendant-marker.txt";
    options = ainiux::runtime::SubprocessOptions{};
    options.executable = subprocess_fixture_path();
    options.arguments = {"--descendant", marker.u8string(), "400"};
    options.timeout_ms = 100;
    error = ainiux::runtime::run_subprocess(options, result);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    check(error.code == ErrorCode::Timeout && !fs::exists(marker),
          "subprocess timeout terminates the complete descendant process tree");
}

void test_subprocess_closed_stdin_is_incomplete() {
    ainiux::runtime::SubprocessOptions options;
    options.executable = subprocess_fixture_path();
    options.arguments = {"--close-stdin"};
    options.provide_stdin = true;
    options.stdin_text.assign(1024 * 1024, 'x');
    options.timeout_ms = 5000;
    ainiux::runtime::SubprocessResult result;
    const Error error = ainiux::runtime::run_subprocess(options, result);
    check(error.ok() &&
              result.termination ==
                  ainiux::runtime::SubprocessTerminationReason::Exited &&
              result.exit_code == 0,
          "subprocess survives a child closing stdin before a large payload is written");
    check(result.stdin_incomplete,
          "subprocess reports stdin incomplete when the child closes without reading it");

#if !defined(_WIN32)
    sigset_t sigpipe_set{};
    sigemptyset(&sigpipe_set);
    sigaddset(&sigpipe_set, SIGPIPE);
    sigset_t previous_mask{};
    const int mask_error =
        pthread_sigmask(SIG_BLOCK, &sigpipe_set, &previous_mask);
    check(mask_error == 0, "subprocess SIGPIPE isolation test blocks SIGPIPE");
    if (mask_error == 0) {
        sigset_t pending_before{};
        const bool already_pending =
            sigpending(&pending_before) == 0 &&
            sigismember(&pending_before, SIGPIPE) == 1;
        check(!already_pending,
              "subprocess SIGPIPE isolation test starts without a pending SIGPIPE");
        if (!already_pending && raise(SIGPIPE) == 0) {
            const Error pending_error =
                ainiux::runtime::run_subprocess(options, result);
            sigset_t pending_after{};
            const bool remains_pending =
                sigpending(&pending_after) == 0 &&
                sigismember(&pending_after, SIGPIPE) == 1;
            check(pending_error.ok() && result.stdin_incomplete && remains_pending,
                  "subprocess preserves a SIGPIPE that was already pending");
        } else if (!already_pending) {
            check(false, "subprocess SIGPIPE isolation test queues SIGPIPE");
        }
        sigset_t pending_cleanup{};
        if (!already_pending && sigpending(&pending_cleanup) == 0 &&
            sigismember(&pending_cleanup, SIGPIPE) == 1) {
            int signal_number = 0;
            (void)sigwait(&sigpipe_set, &signal_number);
        }
        (void)pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr);
    }
#endif
}

#if defined(_WIN32)
void test_subprocess_handle_stability() {
    DWORD before = 0;
    check(GetProcessHandleCount(GetCurrentProcess(), &before) != FALSE,
          "Windows subprocess handle test reads its initial handle count");
    for (int iteration = 0; iteration < 64; ++iteration) {
        ainiux::runtime::SubprocessOptions options;
        options.executable = subprocess_fixture_path();
        options.arguments = {"--exit", "0"};
        options.timeout_ms = 5000;
        ainiux::runtime::SubprocessResult result;
        check(ainiux::runtime::run_subprocess(options, result).ok() &&
                  result.exit_code == 0,
              "repeated Windows subprocess exits cleanly");
    }
    DWORD after = 0;
    check(GetProcessHandleCount(GetCurrentProcess(), &after) != FALSE &&
              after <= before + 2U,
          "repeated Windows subprocesses do not leak process, pipe, thread, or job handles");
}
#endif

void test_portable_path_safety_and_atomic_files() {
    using ainiux::platform::validate_windows_path_syntax;
    check(validate_windows_path_syntax(R"(C:\work\space\unicode-ä.txt)").ok(),
          "Windows path validation accepts drive-rooted Unicode paths");
    check(validate_windows_path_syntax(R"(\\server\share\folder/file.txt)").ok(),
          "Windows path validation accepts UNC paths and mixed separators");
    check(!validate_windows_path_syntax(R"(C:relative\file.txt)").ok(),
          "Windows path validation rejects drive-relative paths");
    check(!validate_windows_path_syntax(R"(C:\work\file.txt:secret)").ok(),
          "Windows path validation rejects alternate data streams");
    check(!validate_windows_path_syntax(R"(C:\work\CON.txt)").ok() &&
              !validate_windows_path_syntax(R"(C:\work\com1)").ok(),
          "Windows path validation rejects reserved device names case-insensitively");
    check(!validate_windows_path_syntax(R"(\\?\C:\work\file.txt)").ok() &&
              !validate_windows_path_syntax(R"(C:\work\trailing.)").ok(),
          "Windows path validation rejects caller NT namespaces and ambiguous trailing dots");
    check(!validate_windows_path_syntax(std::string("C:\\work\\bad") + '\x1f' + ".txt").ok(),
          "Windows path validation rejects control characters");

    TemporaryDirectory temporary;
    const fs::path private_directory = temporary.path() / "private";
    bool created = false;
    Error error = ainiux::platform::create_private_directory_exclusive(
        private_directory.u8string(), created);
    check(error.ok() && created && fs::is_directory(private_directory),
          "private directory creation is exclusive and succeeds once");
    bool created_again = true;
    error = ainiux::platform::create_private_directory_exclusive(
        private_directory.u8string(), created_again);
    check(error.ok() && !created_again,
          "private directory creation reports an existing target without replacing it");

    const fs::path target = private_directory / u8"atomic-ä.txt";
    error = ainiux::platform::atomic_write_private(target.u8string(), "first\n");
    check(error.ok(), "private atomic write creates a Unicode target");
    error = ainiux::platform::atomic_write_private(target.u8string(), "second\n");
    std::string contents;
    const Error read_error =
        ainiux::platform::read_file_bounded(target.u8string(), 1024, contents);
    check(error.ok() && read_error.ok() && contents == "second\n",
          "private atomic write replaces complete contents");
#if defined(_WIN32)
    check(has_private_windows_dacl(private_directory, true) &&
              has_private_windows_dacl(target, false),
          "Windows private state has a protected current-user/SYSTEM-only DACL");
#endif

    bool within = false;
    error = ainiux::platform::path_is_within(temporary.path().u8string(),
                                              target.u8string(), within);
    check(error.ok() && within,
          "path containment uses existing filesystem identities for a descendant");
    const fs::path future_target = private_directory / "future" / "child.txt";
    error = ainiux::platform::path_is_within(temporary.path().u8string(),
                                              future_target.u8string(), within);
    check(error.ok() && within,
          "path containment anchors a new target to its canonical existing parent");
    fs::path sibling = temporary.path().parent_path();
    error = ainiux::platform::path_is_within(private_directory.u8string(),
                                              sibling.u8string(), within);
    check(error.ok() && !within,
          "path containment rejects a canonical ancestor outside the requested root");

    const fs::path outside_file = temporary.path().parent_path() /
                                  ("ainiux-runtime-outside-" +
                                   std::to_string(ainiux::platform::current_process_id()));
    {
        std::ofstream output(outside_file, std::ios::binary);
        output << "outside\n";
    }
    const fs::path link = private_directory / "outside-link";
    std::error_code link_error;
    fs::create_symlink(outside_file, link, link_error);
    if (!link_error) {
        bool contains_link = false;
        const Error link_check = ainiux::platform::path_contains_link_or_reparse(
            link.u8string(), contains_link);
        check(link_check.ok() && contains_link,
              "path inspection detects a symlink or Windows reparse point");
        error = ainiux::platform::path_is_within(private_directory.u8string(),
                                                  link.u8string(), within);
        check(error.ok() && !within,
              "identity containment follows a link and rejects an outside target");
    }
    fs::remove(outside_file, link_error);

#if defined(_WIN32)
    auto no_atomic_temporary = [&] {
        const std::string prefix = target.filename().u8string() + ".ainiux-tmp-";
        std::error_code enumeration_error;
        for (fs::directory_iterator iterator(private_directory, enumeration_error), end;
             !enumeration_error && iterator != end; iterator.increment(enumeration_error)) {
            if (iterator->path().filename().u8string().rfind(prefix, 0) == 0)
                return false;
        }
        return !enumeration_error;
    };
    ainiux::platform::testing::set_next_atomic_write_fault(
        ainiux::platform::testing::AtomicWriteFault::DiskFull);
    error = ainiux::platform::atomic_write_private(target.u8string(), "disk full", true);
    contents.clear();
    const Error after_disk_full = ainiux::platform::read_file_bounded(
        target.u8string(), 1024, contents);
    check(error.code == ErrorCode::FileWrite && after_disk_full.ok() &&
              contents == "second\n" && no_atomic_temporary(),
          "Windows disk-full injection preserves the target and removes its temporary file");

    ainiux::platform::testing::set_next_atomic_write_fault(
        ainiux::platform::testing::AtomicWriteFault::ShortWrite);
    error = ainiux::platform::atomic_write_private(target.u8string(), "short write", true);
    contents.clear();
    const Error after_short_write = ainiux::platform::read_file_bounded(
        target.u8string(), 1024, contents);
    check(error.code == ErrorCode::FileWrite && after_short_write.ok() &&
              contents == "second\n" && no_atomic_temporary(),
          "Windows short-write injection preserves the target and removes its temporary file");

    fs::path long_directory = private_directory;
    while (long_directory.u8string().size() < 300U)
        long_directory /= "long-path-component";
    error = ainiux::platform::ensure_private_directory(long_directory.u8string(), true);
    const fs::path long_file = long_directory / u8"unicode-ä.txt";
    if (error.ok()) error = ainiux::platform::atomic_write_private(
        long_file.u8string(), "long path\n", true);
    contents.clear();
    if (error.ok()) error = ainiux::platform::read_file_bounded(
        long_file.u8string(), 1024, contents);
    check(error.ok() && contents == "long path\n",
          "Windows secure file helpers support Unicode long paths");
#endif
}

}  // namespace

void run_all() {
    test_runtime_event_queue_and_job_cancel();
    test_runtime_queue_timeout();
    test_background_jobs_supersede_without_blocking();
    test_subprocess_utf8_io_and_status();
    test_subprocess_closed_stdin_is_incomplete();
    test_subprocess_timeout_cancellation_and_descendants();
#if defined(_WIN32)
    test_subprocess_handle_stability();
#endif
    test_portable_path_safety_and_atomic_files();
}

}  // namespace ainiux::test::runtime
