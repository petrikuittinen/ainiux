#include "app/app.hpp"

#include <iostream>

namespace pkchat::app {

std::ostream* output_stream(const cli::Options& options, std::ofstream& file, Error& error) {
    if (options.output_path.empty() || options.output_path == "stdout") {
        return &std::cout;
    }
    file.open(options.output_path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = {ErrorCode::FileWrite, "could not open output file for writing: " + options.output_path};
        return nullptr;
    }
    return &file;
}

}  // namespace pkchat::app