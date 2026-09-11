#include "runtime/interrupt.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <cstring>
#endif

namespace ainiux::runtime {

#if defined(_WIN32)
namespace {
volatile LONG g_interrupted = 0;
BOOL WINAPI console_control_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        if (InterlockedCompareExchange(&g_interrupted, 1, 0) != 0) return FALSE;
        return TRUE;
    }
    return FALSE;
}
}  // namespace
struct InterruptGuard::Impl { bool installed = false; };

InterruptGuard::InterruptGuard() : impl_(std::make_unique<Impl>()) {
    InterlockedExchange(&g_interrupted, 0);
    impl_->installed = SetConsoleCtrlHandler(console_control_handler, TRUE) != FALSE;
}
InterruptGuard::~InterruptGuard() {
    if (impl_->installed) (void)SetConsoleCtrlHandler(console_control_handler, FALSE);
}
bool InterruptGuard::interrupted() const {
    return InterlockedCompareExchange(&g_interrupted, 0, 0) != 0;
}
bool InterruptGuard::installed() const { return impl_->installed; }
#else
namespace {
volatile std::sig_atomic_t g_interrupted = 0;
void interrupt_handler(int signo) {
    if (g_interrupted) {
        struct sigaction action{};
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        (void)sigaction(signo, &action, nullptr);
        (void)raise(signo);
        return;
    }
    g_interrupted = 1;
}
}  // namespace
struct InterruptGuard::Impl {
    struct sigaction previous_int{};
    struct sigaction previous_term{};
    bool installed_int = false;
    bool installed_term = false;
};

InterruptGuard::InterruptGuard() : impl_(std::make_unique<Impl>()) {
    g_interrupted = 0;
    struct sigaction action{};
    action.sa_handler = interrupt_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    impl_->installed_int = sigaction(SIGINT, &action, &impl_->previous_int) == 0;
    impl_->installed_term = sigaction(SIGTERM, &action, &impl_->previous_term) == 0;
}
InterruptGuard::~InterruptGuard() {
    if (impl_->installed_int) (void)sigaction(SIGINT, &impl_->previous_int, nullptr);
    if (impl_->installed_term) (void)sigaction(SIGTERM, &impl_->previous_term, nullptr);
}
bool InterruptGuard::interrupted() const { return g_interrupted != 0; }
bool InterruptGuard::installed() const { return impl_->installed_int; }
#endif

}  // namespace ainiux::runtime
