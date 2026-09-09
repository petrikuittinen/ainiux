#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "common.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::pdf {

struct Options {
    std::size_t max_bytes = 64 * 1024 * 1024;
    std::size_t max_pages = 0;
    std::string password;
    runtime::CancellationToken cancellation;
};

// Canonical conversion is PDF → Markdown. Callers that want plaintext or HTML
// run markdown::render on the result (so html-to-pdf is html→md→pdf).
Error to_markdown_file(const std::string& path, const Options& options, std::string& markdown);
Error to_markdown_bytes(std::string_view pdf, const Options& options, std::string& markdown);

}  // namespace ainiux::pdf
