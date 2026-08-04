#pragma once

#include <memory>

namespace ainiux::runtime {

// Process-wide headless Ctrl+C guard. Only one is expected to be active; all
// current callers are mutually exclusive top-level modes.
class InterruptGuard {
   public:
    InterruptGuard();
    ~InterruptGuard();
    InterruptGuard(const InterruptGuard&) = delete;
    InterruptGuard& operator=(const InterruptGuard&) = delete;

    bool interrupted() const;
    bool installed() const;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ainiux::runtime
