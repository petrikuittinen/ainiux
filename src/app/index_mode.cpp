#include "app/app.hpp"

#include <fstream>
#include <iostream>

#include "agent/index/index.hpp"
#include "app/index_progress.hpp"
#include "runtime/interrupt.hpp"

namespace ainiux::app {
int run_index_mode(const cli::Options& options) {
    runtime::InterruptGuard interrupt_guard;
    agent::index::Options index_options;
    index_options.workspace = ".";
    index_options.max_source_code_file_size = options.max_source_code_file_size;
    index_options.interrupted = [&interrupt_guard] { return interrupt_guard.interrupted(); };

    if (options.clear_index) {
        agent::index::ClearStats stats;
        const Error error = agent::index::clear_database(index_options, stats);
        if (!error.ok()) {
            print_error(error);
            return exit_code_for(error.code);
        }
        if (stats.removed_files == 0) {
            std::cerr << "Code index is already clear; no database exists at "
                      << agent::index::database_path(index_options.workspace) << ".\n";
        } else {
            std::cerr << "Code index cleared: removed " << stats.removed_files
                      << " database file(s) from "
                      << agent::index::database_path(index_options.workspace) << ".\n";
        }
        return 0;
    }

    if (options.index_code) {
        IndexProgressPrinter progress(true);
        index_options.on_progress =
            [&progress](const agent::index::Progress& update) {
                progress.update(update);
            };
        agent::index::RefreshStats stats;
        const Error error = agent::index::refresh(index_options, stats);
        progress.finish();
        // Drop the stack-capturing callback before leaving this scope so a
        // following --print-index freshness discover cannot use a dangling
        // IndexProgressPrinter (stack-use-after-scope under ASan).
        index_options.on_progress = {};
        if (!error.ok()) {
            print_error(error);
            return exit_code_for(error.code);
        }
        for (const std::string& diagnostic : stats.diagnostics) {
            std::cerr << "Index warning: " << diagnostic << "\n";
        }
        std::cerr << "Code index updated in " << stats.elapsed_ms << " ms: "
                  << stats.discovered << " eligible, " << stats.indexed << " indexed, "
                  << stats.unchanged << " unchanged, " << stats.skipped << " skipped, "
                  << stats.removed << " removed, " << stats.symbols
                  << " symbols; "
                  << stats.worker_count << " worker(s).\n";
    }

    if (options.print_index) {
        agent::index::Freshness freshness;
        Error error = agent::index::check_freshness(index_options, freshness);
        if (!error.ok()) {
            print_error(error);
            return exit_code_for(error.code);
        }
        if (!freshness.fresh) {
            std::cerr << "Warning: code index snapshot is stale; printing the stored snapshot without updating it.\n";
        }
        std::ofstream file;
        std::ostream* output = output_stream(options, file, error);
        if (!error.ok()) {
            print_error(error);
            return exit_code_for(error.code);
        }
        error = agent::index::print_markdown(index_options, freshness, *output);
        if (!error.ok()) {
            print_error(error);
            return exit_code_for(error.code);
        }
    }
    return 0;
}

}  // namespace ainiux::app
