#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::csv {

struct ReadOptions {
    std::size_t max_bytes = 10U * 1024U * 1024U;
    runtime::CancellationToken cancellation;
};

struct Diagnostics {
    std::size_t ragged_rows = 0;
    std::vector<std::string> messages;
};

// RFC 4180 CSV → GitHub-flavored Markdown table. First row is the header.
// This is conversion for fetched documents, not a lossless spreadsheet editor.
Error to_markdown_bytes(std::string_view bytes,
                        const ReadOptions& options,
                        std::string& markdown,
                        Diagnostics* diagnostics = nullptr);

inline constexpr const char* kMimeType = "text/csv";

}  // namespace ainiux::csv
