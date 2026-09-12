#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::docx {

struct ReadOptions {
    std::size_t max_bytes = 64U * 1024U * 1024U;
    runtime::CancellationToken cancellation;
};

struct WriteOptions {
    std::size_t max_bytes = 64U * 1024U * 1024U;
    runtime::CancellationToken cancellation;
};

struct Diagnostics {
    std::size_t omitted_images = 0;
    std::size_t flattened_merged_tables = 0;
    std::size_t unsupported_story_parts = 0;
    std::vector<std::string> messages;
};

// DOCX conversion has one normalization boundary: WordprocessingML is read as
// Markdown and generated packages are always newly typeset from Markdown.
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

inline constexpr const char* kMimeType =
    "application/vnd.openxmlformats-officedocument.wordprocessingml.document";

}  // namespace ainiux::docx
