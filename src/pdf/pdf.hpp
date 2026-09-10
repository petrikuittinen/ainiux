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

struct WriteOptions {
    std::size_t max_bytes = 64 * 1024 * 1024;
    std::size_t max_pages = 0;
    runtime::CancellationToken cancellation;
    std::size_t substituted_glyphs = 0;
    std::string font_path;
    bool cjk_font_missing = false;
    bool rtl_font_missing = false;
};

// Canonical conversion is PDF ↔ Markdown. Callers that want plaintext or HTML
// run markdown::render on the Markdown result (so html-to-pdf is html→md→pdf).
Error to_markdown_file(const std::string& path, const Options& options, std::string& markdown);
Error to_markdown_bytes(std::string_view pdf, const Options& options, std::string& markdown);
Error from_markdown(std::string_view markdown, WriteOptions& options, std::string& pdf);

}  // namespace ainiux::pdf
