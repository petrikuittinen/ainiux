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
        InterlockedExchange(&g_interrupted, 1);
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
void interrupt_handler(int) { g_interrupted = 1; }
}  // namespace
struct InterruptGuard::Impl {
    struct sigaction previous{};
    bool installed = false;
};

InterruptGuard::InterruptGuard() : impl_(std::make_unique<Impl>()) {
    g_interrupted = 0;
    struct sigaction action{};
    action.sa_handler = interrupt_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    impl_->installed = sigaction(SIGINT, &action, &impl_->previous) == 0;
}
InterruptGuard::~InterruptGuard() {
    if (impl_->installed) (void)sigaction(SIGINT, &impl_->previous, nullptr);
}
bool InterruptGuard::interrupted() const { return g_interrupted != 0; }
bool InterruptGuard::installed() const { return impl_->installed; }
#endif

}  // namespace ainiux::runtime
