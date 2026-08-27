#pragma once

#include <string>

namespace ainiux::platform {

// UTF-8 environment lookup. On Windows this bypasses the active ANSI code page.
std::string environment_value(const char* ascii_name);

// HOME with the product's compatibility fallback: USERPROFILE on Windows only.
std::string home_directory();

// Absolute path of the running executable, or empty on failure.
std::string executable_path();

// Absolute directory containing the running executable, or empty on failure.
std::string executable_directory();

// Publishes HOME from USERPROFILE on Windows. It is harmless on POSIX.
void initialize_process_environment();

// Process-lifetime console/CRT encoding guard. Windows uses raw UTF-8 bytes for
// redirected streams and restores the caller's console code pages and CRT modes
// before returning to the shell. POSIX only performs environment initialization.
class ProcessEnvironmentGuard {
   public:
    ProcessEnvironmentGuard();
    ~ProcessEnvironmentGuard();
    ProcessEnvironmentGuard(const ProcessEnvironmentGuard&) = delete;
    ProcessEnvironmentGuard& operator=(const ProcessEnvironmentGuard&) = delete;

   private:
#if defined(_WIN32)
    unsigned int input_code_page_ = 0;
    unsigned int output_code_page_ = 0;
    int stdin_mode_ = -1;
    int stdout_mode_ = -1;
    int stderr_mode_ = -1;
#endif
};

unsigned long long current_process_id();

}  // namespace ainiux::platform
