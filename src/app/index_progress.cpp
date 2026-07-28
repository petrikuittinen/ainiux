#include "app/index_progress.hpp"

#include <algorithm>
#include <iostream>

#include <unistd.h>

namespace ainiux::app {

const char* index_progress_phase_name(agent::index::ProgressPhase phase) {
    switch (phase) {
        case agent::index::ProgressPhase::Discovery:
            return "discovering";
        case agent::index::ProgressPhase::Scanning:
            return "scanning";
        case agent::index::ProgressPhase::SnapshotCommit:
            return "committing snapshot";
        case agent::index::ProgressPhase::Compaction:
            return "compacting index";
    }
    return "indexing";
}

IndexProgressPrinter::IndexProgressPrinter(bool enabled)
    : enabled_(enabled), tty_(::isatty(STDERR_FILENO) != 0) {}

IndexProgressPrinter::~IndexProgressPrinter() { finish(); }

void IndexProgressPrinter::update(const agent::index::Progress& progress) {
    if (!enabled_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (finished_) return;
    const auto now = std::chrono::steady_clock::now();
    const bool phase_changed = !wrote_ || progress.phase != phase_;
    int bucket = -1;
    if (progress.total > 0) {
        const std::size_t bounded = std::min(progress.completed, progress.total);
        bucket = static_cast<int>((bounded * 10) / progress.total);
    }
    const bool completed =
        progress.total > 0 && progress.completed >= progress.total;
    if (tty_) {
        if (!phase_changed && !completed && wrote_ &&
            now - last_ < std::chrono::milliseconds(80))
            return;
    } else if (!phase_changed && !completed && bucket == percent_bucket_) {
        return;
    }

    std::cerr << (tty_ ? "\r" : "") << "Index "
              << index_progress_phase_name(progress.phase) << ": ";
    if (progress.total > 0)
        std::cerr << std::min(progress.completed, progress.total) << "/"
                  << progress.total;
    else
        std::cerr << progress.completed;
    std::cerr << " work item(s), " << progress.discovered
              << " discovered, " << progress.changed << " changed, "
              << progress.elapsed_ms << " ms";
    if (!tty_) std::cerr << "\n";
    std::cerr.flush();

    wrote_ = true;
    phase_ = progress.phase;
    percent_bucket_ = bucket;
    last_ = now;
}

void IndexProgressPrinter::finish() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (finished_) return;
    if (enabled_ && tty_ && wrote_) std::cerr << "\n";
    finished_ = true;
}

}  // namespace ainiux::app
