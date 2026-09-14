#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::xlsx {

struct ReadOptions {
    std::size_t max_bytes = 64U * 1024U * 1024U;
    runtime::CancellationToken cancellation;
};

struct WriteOptions {
    std::size_t max_bytes = 64U * 1024U * 1024U;
    runtime::CancellationToken cancellation;
};

struct Diagnostics {
    std::size_t omitted_sheets = 0;
    std::size_t omitted_empty_sheets = 0;
    std::size_t flattened_merged_cells = 0;
    std::vector<std::string> messages;
};

// XLSX conversion has one normalization boundary: SpreadsheetML is read as
// GitHub-flavored Markdown tables and generated packages are newly typeset
// from Markdown tables. This is not lossless workbook editing.
Error to_markdown_file(const std::string& path,
                       const ReadOptions& options,
                       std::string& markdown,
                       Diagnostics* diagnostics = nullptr);
Error to_markdown_bytes(std::string_view bytes,
                        const ReadOptions& options,
                        std::string& markdown,
                        Diagnostics* diagnostics = nullptr);
Error from_markdown(std::string_view markdown,
                    const WriteOptions& options,
                    std::string& bytes,
                    Diagnostics* diagnostics = nullptr);

bool looks_like_xlsx(std::string_view bytes);

inline constexpr const char* kMimeType =
    "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";

}  // namespace ainiux::xlsx
