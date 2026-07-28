#pragma once

#include <chrono>
#include <mutex>

#include "agent/index/index.hpp"

namespace ainiux::app {

// Compact stderr renderer for foreground indexing. TTYs reuse one line;
// redirected output is limited to phase changes and coarse percentage steps.
class IndexProgressPrinter {
   public:
    explicit IndexProgressPrinter(bool enabled);
    ~IndexProgressPrinter();
    IndexProgressPrinter(const IndexProgressPrinter&) = delete;
    IndexProgressPrinter& operator=(const IndexProgressPrinter&) = delete;

    void update(const agent::index::Progress& progress);
    void finish();

   private:
    bool enabled_ = true;
    bool tty_ = false;
    bool wrote_ = false;
    bool finished_ = false;
    agent::index::ProgressPhase phase_ =
        agent::index::ProgressPhase::Discovery;
    int percent_bucket_ = -1;
    std::chrono::steady_clock::time_point last_;
    std::mutex mutex_;
};

const char* index_progress_phase_name(agent::index::ProgressPhase phase);

}  // namespace ainiux::app
