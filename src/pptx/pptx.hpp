#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::pptx {

struct MediaAsset {
    std::string source_part;
    std::string markdown_target;
    std::string content_type;
    std::string extension;
    std::string sha256;
    std::string bytes;
};

struct ReadOptions {
    std::size_t max_bytes = 256U * 1024U * 1024U;
    std::size_t max_members = 10000;
    std::size_t max_slides = 2048;
    std::size_t max_xml_part_bytes = 64U * 1024U * 1024U;
    std::size_t max_selected_xml_bytes = 128U * 1024U * 1024U;
    std::size_t max_shapes_and_cells = 1000000;
    bool collect_media = false;
    // Used verbatim as the relative prefix of generated Markdown links.
    // Empty selects the private "ainiux-media:" round-trip namespace.
    std::string media_link_prefix;
    runtime::CancellationToken cancellation;
};

struct WriteOptions {
    std::size_t max_bytes = 256U * 1024U * 1024U;
    std::string source_directory;
    // When non-empty, local image paths must remain below this directory.
    std::string workspace_root;
    std::vector<MediaAsset> collected_media;
    runtime::CancellationToken cancellation;
};

struct Diagnostics {
    std::size_t omitted_images = 0;
    std::size_t collected_images = 0;
    std::size_t flattened_merged_tables = 0;
    std::size_t unsupported_objects = 0;
    std::size_t missing_relationships = 0;
    std::vector<std::string> messages;
};

Error to_markdown_file(const std::string& path,
                       const ReadOptions& options,
                       std::string& markdown,
                       Diagnostics* diagnostics = nullptr,
                       std::vector<MediaAsset>* media = nullptr);
Error to_markdown_bytes(std::string_view bytes,
                        const ReadOptions& options,
                        std::string& markdown,
                        Diagnostics* diagnostics = nullptr,
                        std::vector<MediaAsset>* media = nullptr);
Error from_markdown(std::string_view markdown,
                    const WriteOptions& options,
                    std::string& bytes,
                    Diagnostics* diagnostics = nullptr);

bool looks_like_pptx(std::string_view bytes);

inline constexpr const char* kMimeType =
    "application/vnd.openxmlformats-officedocument.presentationml.presentation";

}  // namespace ainiux::pptx
