#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::docx::detail {

struct ZipLimits {
    std::size_t max_members = 10000;
    std::size_t max_member_bytes = 64U * 1024U * 1024U;
    std::size_t max_total_selected_bytes = 128U * 1024U * 1024U;
    std::size_t max_expansion_ratio = 1000;
};

class ZipArchive {
   public:
    Error open(std::string_view bytes, const ZipLimits& limits,
               runtime::CancellationToken cancellation);
    bool contains(const std::string& name) const;
    Error read(const std::string& name, std::string& bytes);
    const std::vector<std::string>& names() const { return names_; }

   private:
    struct Entry {
        std::uint16_t flags = 0;
        std::uint16_t method = 0;
        std::uint32_t crc = 0;
        std::uint64_t compressed_size = 0;
        std::uint64_t uncompressed_size = 0;
        std::uint64_t local_offset = 0;
        std::uint64_t data_offset = 0;
        std::uint64_t data_end = 0;
        std::uint64_t record_end = 0;
        bool zip64_sizes = false;
    };

    std::string archive_;
    ZipLimits limits_;
    runtime::CancellationToken cancellation_;
    std::unordered_map<std::string, Entry> entries_;
    std::unordered_map<std::string, std::string> cache_;
    std::vector<std::string> names_;
    std::size_t selected_bytes_ = 0;
};

struct ZipWriteEntry {
    std::string name;
    std::string bytes;
    bool compress = true;
};

Error write_zip(const std::vector<ZipWriteEntry>& entries,
                std::size_t max_bytes,
                runtime::CancellationToken cancellation,
                std::string& bytes);

bool safe_part_name(std::string_view name);
Error resolve_part_name(std::string_view source_part,
                        std::string_view target,
                        std::string& resolved);

}  // namespace ainiux::docx::detail
