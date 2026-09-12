#include "docx/zip.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>
#include <utility>

#include "html/html.hpp"

namespace ainiux::docx::detail {
namespace {

constexpr std::uint32_t kLocalSignature = 0x04034b50U;
constexpr std::uint32_t kCentralSignature = 0x02014b50U;
constexpr std::uint32_t kEndSignature = 0x06054b50U;
constexpr std::uint32_t kZip64EndSignature = 0x06064b50U;
constexpr std::uint32_t kZip64LocatorSignature = 0x07064b50U;
constexpr std::uint32_t kDescriptorSignature = 0x08074b50U;

bool range_ok(std::size_t offset, std::size_t count, std::size_t size) {
    return offset <= size && count <= size - offset;
}

bool u16_at(std::string_view bytes, std::size_t offset, std::uint16_t& value) {
    if (!range_ok(offset, 2, bytes.size())) return false;
    value = static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[offset])) |
            static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8U;
    return true;
}

bool u32_at(std::string_view bytes, std::size_t offset, std::uint32_t& value) {
    if (!range_ok(offset, 4, bytes.size())) return false;
    value = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset])) |
            static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8U |
            static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 2])) << 16U |
            static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 3])) << 24U;
    return true;
}

bool u64_at(std::string_view bytes, std::size_t offset, std::uint64_t& value) {
    std::uint32_t low = 0;
    std::uint32_t high = 0;
    if (!u32_at(bytes, offset, low) || !u32_at(bytes, offset + 4, high)) return false;
    value = static_cast<std::uint64_t>(low) | static_cast<std::uint64_t>(high) << 32U;
    return true;
}

void append_u16(std::string& out, std::uint16_t value) {
    out.push_back(static_cast<char>(value & 0xffU));
    out.push_back(static_cast<char>((value >> 8U) & 0xffU));
}

void append_u32(std::string& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xffU));
    }
}

bool fits_size(std::uint64_t value) {
    return value <= static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
}

Error zip_error(std::string message) {
    return {ErrorCode::FileRead, "invalid DOCX ZIP: " + std::move(message)};
}

struct InflateGuard {
    z_stream stream{};
    bool initialized = false;
    ~InflateGuard() {
        if (initialized) inflateEnd(&stream);
    }
};

struct DeflateGuard {
    z_stream stream{};
    bool initialized = false;
    ~DeflateGuard() {
        if (initialized) deflateEnd(&stream);
    }
};

Error parse_zip64_extra(std::string_view extra,
                        bool need_uncompressed,
                        bool need_compressed,
                        bool need_offset,
                        std::uint64_t& uncompressed,
                        std::uint64_t& compressed,
                        std::uint64_t& offset) {
    std::size_t pos = 0;
    while (pos < extra.size()) {
        std::uint16_t id = 0;
        std::uint16_t length = 0;
        if (!u16_at(extra, pos, id) || !u16_at(extra, pos + 2, length) ||
            !range_ok(pos + 4, length, extra.size())) {
            return zip_error("malformed central-directory extra data");
        }
        if (id == 0x0001U) {
            std::size_t field = pos + 4;
            const std::size_t end = field + length;
            auto take = [&](std::uint64_t& value) {
                if (!range_ok(field, 8, end)) return false;
                const bool ok = u64_at(extra, field, value);
                field += 8;
                return ok;
            };
            if (need_uncompressed && !take(uncompressed)) return zip_error("missing Zip64 uncompressed size");
            if (need_compressed && !take(compressed)) return zip_error("missing Zip64 compressed size");
            if (need_offset && !take(offset)) return zip_error("missing Zip64 local-header offset");
            return ok_error();
        }
        pos += 4 + length;
    }
    if (need_uncompressed || need_compressed || need_offset) {
        return zip_error("required Zip64 extra data is absent");
    }
    return ok_error();
}

bool unsupported_flags(std::uint16_t flags) {
    constexpr std::uint16_t allowed = 0x080eU;  // deflate tuning, descriptor, UTF-8
    return (flags & static_cast<std::uint16_t>(~allowed)) != 0;
}

Error check_limit(std::size_t current, std::size_t extra, std::size_t maximum) {
    if (current > maximum || extra > maximum - current) {
        return {ErrorCode::UnsupportedFeature, "DOCX output exceeds size limit"};
    }
    return ok_error();
}

}  // namespace

bool safe_part_name(std::string_view name) {
    if (name.empty() || name.front() == '/' || name.front() == '\\' ||
        name.find('\\') != std::string_view::npos || name.find('\0') != std::string_view::npos) {
        return false;
    }
    if (name.size() >= 2 && ((name[0] >= 'A' && name[0] <= 'Z') ||
                             (name[0] >= 'a' && name[0] <= 'z')) && name[1] == ':') {
        return false;
    }
    std::size_t start = 0;
    while (start <= name.size()) {
        const std::size_t slash = name.find('/', start);
        const std::size_t end = slash == std::string_view::npos ? name.size() : slash;
        const std::string_view component = name.substr(start, end - start);
        if (component == "." || component == ".." ||
            (component.empty() && end != name.size())) return false;
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return true;
}

Error resolve_part_name(std::string_view source_part,
                        std::string_view target,
                        std::string& resolved) {
    resolved.clear();
    if (target.empty() || target.find('\\') != std::string_view::npos ||
        target.find('\0') != std::string_view::npos) {
        return zip_error("relationship has an unsafe empty or backslash target");
    }
    std::string joined;
    if (target.front() == '/') {
        joined.assign(target.substr(1));
    } else {
        const std::size_t slash = source_part.find_last_of('/');
        if (slash != std::string_view::npos) joined.assign(source_part.substr(0, slash + 1));
        joined.append(target.data(), target.size());
    }
    std::vector<std::string> components;
    std::size_t start = 0;
    while (start <= joined.size()) {
        const std::size_t slash = joined.find('/', start);
        const std::size_t end = slash == std::string::npos ? joined.size() : slash;
        const std::string component = joined.substr(start, end - start);
        if (component.empty() || component == ".") {
            // A leading slash was already removed; repeated separators normalize away.
        } else if (component == "..") {
            if (components.empty()) return zip_error("relationship target escapes the package root");
            components.pop_back();
        } else {
            components.push_back(component);
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    for (std::size_t i = 0; i < components.size(); ++i) {
        if (i != 0) resolved.push_back('/');
        resolved += components[i];
    }
    if (!safe_part_name(resolved)) return zip_error("relationship target is not a safe package part");
    return ok_error();
}

Error ZipArchive::open(std::string_view bytes,
                       const ZipLimits& limits,
                       runtime::CancellationToken cancellation) {
    archive_.clear();
    entries_.clear();
    cache_.clear();
    names_.clear();
    selected_bytes_ = 0;
    limits_ = limits;
    cancellation_ = cancellation;
    if (cancellation.cancelled()) return {ErrorCode::Cancelled, "DOCX ZIP read cancelled"};
    if (bytes.size() < 22) return zip_error("end-of-central-directory record is missing");
    try {
        archive_.assign(bytes.data(), bytes.size());
    } catch (const std::bad_alloc&) {
        return {ErrorCode::Internal, "not enough memory to read DOCX ZIP"};
    }

    const std::size_t search_start = archive_.size() > 65557U ? archive_.size() - 65557U : 0U;
    std::size_t eocd = std::string::npos;
    for (std::size_t pos = archive_.size() - 22U;; --pos) {
        std::uint32_t signature = 0;
        if (u32_at(archive_, pos, signature) && signature == kEndSignature) {
            std::uint16_t comment = 0;
            if (u16_at(archive_, pos + 20, comment) && pos + 22U + comment == archive_.size()) {
                eocd = pos;
                break;
            }
        }
        if (pos == search_start) break;
    }
    if (eocd == std::string::npos) return zip_error("end-of-central-directory record is missing");

    std::uint16_t disk = 0, central_disk = 0, disk_entries = 0, total16 = 0;
    std::uint32_t central_size32 = 0, central_offset32 = 0;
    u16_at(archive_, eocd + 4, disk);
    u16_at(archive_, eocd + 6, central_disk);
    u16_at(archive_, eocd + 8, disk_entries);
    u16_at(archive_, eocd + 10, total16);
    u32_at(archive_, eocd + 12, central_size32);
    u32_at(archive_, eocd + 16, central_offset32);
    if (disk != 0 || central_disk != 0) return zip_error("multi-disk archives are unsupported");

    std::uint64_t total = total16;
    std::uint64_t central_size = central_size32;
    std::uint64_t central_offset = central_offset32;
    const bool needs_zip64 = total16 == 0xffffU || disk_entries == 0xffffU ||
                             central_size32 == 0xffffffffU || central_offset32 == 0xffffffffU;
    if (needs_zip64) {
        if (eocd < 20) return zip_error("Zip64 locator is truncated");
        std::uint32_t locator_signature = 0, locator_disk = 0, locator_disks = 0;
        std::uint64_t zip64_offset = 0;
        if (!u32_at(archive_, eocd - 20, locator_signature) || locator_signature != kZip64LocatorSignature ||
            !u32_at(archive_, eocd - 16, locator_disk) || !u64_at(archive_, eocd - 12, zip64_offset) ||
            !u32_at(archive_, eocd - 4, locator_disks)) {
            return zip_error("Zip64 locator is malformed");
        }
        if (locator_disk != 0 || locator_disks != 1 || !fits_size(zip64_offset)) {
            return zip_error("multi-disk or out-of-range Zip64 metadata is unsupported");
        }
        const std::size_t z = static_cast<std::size_t>(zip64_offset);
        std::uint32_t signature = 0, zip_disk = 0, zip_central_disk = 0;
        std::uint64_t record_size = 0, disk_total = 0;
        if (!u32_at(archive_, z, signature) || signature != kZip64EndSignature ||
            !u64_at(archive_, z + 4, record_size) || record_size < 44 ||
            record_size > archive_.size() || !range_ok(z, static_cast<std::size_t>(record_size + 12), archive_.size()) ||
            !u32_at(archive_, z + 16, zip_disk) || !u32_at(archive_, z + 20, zip_central_disk) ||
            !u64_at(archive_, z + 24, disk_total) || !u64_at(archive_, z + 32, total) ||
            !u64_at(archive_, z + 40, central_size) || !u64_at(archive_, z + 48, central_offset)) {
            return zip_error("Zip64 end record is malformed");
        }
        if (zip_disk != 0 || zip_central_disk != 0 || disk_total != total) {
            return zip_error("multi-disk Zip64 archive is unsupported");
        }
    } else if (disk_entries != total16) {
        return zip_error("central-directory entry counts disagree");
    }
    if (total > limits_.max_members) {
        return {ErrorCode::UnsupportedFeature, "DOCX ZIP contains more than " +
                                                   std::to_string(limits_.max_members) + " members"};
    }
    if (!fits_size(central_offset) || !fits_size(central_size) || central_offset > eocd ||
        central_size > eocd - central_offset) {
        return zip_error("central directory is outside the archive");
    }
    const std::size_t cd_start = static_cast<std::size_t>(central_offset);
    const std::size_t cd_end = cd_start + static_cast<std::size_t>(central_size);
    if (cd_end > eocd) return zip_error("central directory overlaps trailing records");

    std::size_t pos = cd_start;
    try {
        entries_.reserve(static_cast<std::size_t>(total));
        names_.reserve(static_cast<std::size_t>(total));
    } catch (const std::bad_alloc&) {
        return {ErrorCode::Internal, "not enough memory to index DOCX ZIP"};
    }
    for (std::uint64_t index = 0; index < total; ++index) {
        if ((index & 0xffU) == 0 && cancellation.cancelled())
            return {ErrorCode::Cancelled, "DOCX ZIP indexing cancelled"};
        std::uint32_t signature = 0, crc = 0, comp32 = 0, uncomp32 = 0, offset32 = 0;
        std::uint16_t flags = 0, method = 0, name_length = 0, extra_length = 0, comment_length = 0;
        std::uint16_t entry_disk = 0;
        if (!range_ok(pos, 46, cd_end) || !u32_at(archive_, pos, signature) ||
            signature != kCentralSignature || !u16_at(archive_, pos + 8, flags) ||
            !u16_at(archive_, pos + 10, method) || !u32_at(archive_, pos + 16, crc) ||
            !u32_at(archive_, pos + 20, comp32) || !u32_at(archive_, pos + 24, uncomp32) ||
            !u16_at(archive_, pos + 28, name_length) || !u16_at(archive_, pos + 30, extra_length) ||
            !u16_at(archive_, pos + 32, comment_length) || !u16_at(archive_, pos + 34, entry_disk) ||
            !u32_at(archive_, pos + 42, offset32)) {
            return zip_error("central-directory entry is truncated or malformed");
        }
        const std::size_t variable = static_cast<std::size_t>(name_length) + extra_length + comment_length;
        if (!range_ok(pos + 46, variable, cd_end)) return zip_error("central-directory entry data is truncated");
        if (entry_disk != 0) return zip_error("multi-disk member is unsupported");
        if ((flags & 1U) != 0 || (flags & 0x40U) != 0 || (flags & 0x2000U) != 0)
            return zip_error("encrypted members are unsupported");
        if (unsupported_flags(flags)) return zip_error("member uses unsupported general-purpose flags");
        if (method != 0 && method != 8) return zip_error("member uses unsupported compression method " + std::to_string(method));
        const std::string name = archive_.substr(pos + 46, name_length);
        std::size_t invalid = 0;
        if (!html::is_valid_utf8(name, &invalid)) return zip_error("member name is not valid UTF-8");
        if (!safe_part_name(name)) return zip_error("member has an absolute or traversing name: " + name);
        if (entries_.find(name) != entries_.end()) return zip_error("duplicate member name: " + name);

        Entry entry;
        entry.flags = flags;
        entry.method = method;
        entry.crc = crc;
        entry.compressed_size = comp32;
        entry.uncompressed_size = uncomp32;
        entry.local_offset = offset32;
        entry.zip64_sizes = comp32 == 0xffffffffU || uncomp32 == 0xffffffffU;
        const std::string_view extra(archive_.data() + pos + 46 + name_length, extra_length);
        Error error = parse_zip64_extra(extra, uncomp32 == 0xffffffffU, comp32 == 0xffffffffU,
                                        offset32 == 0xffffffffU, entry.uncompressed_size,
                                        entry.compressed_size, entry.local_offset);
        if (!error.ok()) return error;
        if (!fits_size(entry.local_offset) || !fits_size(entry.compressed_size) ||
            !fits_size(entry.uncompressed_size)) return zip_error("member size or offset exceeds this platform");
        if (entry.uncompressed_size > limits_.max_member_bytes)
            return {ErrorCode::UnsupportedFeature, "DOCX ZIP member exceeds the 64 MiB inflation limit: " + name};
        if (entry.uncompressed_size > 0 && entry.compressed_size == 0)
            return zip_error("non-empty member has no compressed data: " + name);
        if (entry.compressed_size > 0 &&
            entry.compressed_size <= std::numeric_limits<std::uint64_t>::max() /
                                         limits_.max_expansion_ratio &&
            entry.uncompressed_size > entry.compressed_size * limits_.max_expansion_ratio)
            return {ErrorCode::UnsupportedFeature, "DOCX ZIP member exceeds the 1000:1 expansion limit: " + name};

        const std::size_t local = static_cast<std::size_t>(entry.local_offset);
        std::uint32_t local_signature = 0, local_crc = 0, local_comp = 0, local_uncomp = 0;
        std::uint16_t local_flags = 0, local_method = 0, local_name_length = 0, local_extra_length = 0;
        if (!range_ok(local, 30, cd_start) || !u32_at(archive_, local, local_signature) ||
            local_signature != kLocalSignature || !u16_at(archive_, local + 6, local_flags) ||
            !u16_at(archive_, local + 8, local_method) || !u32_at(archive_, local + 14, local_crc) ||
            !u32_at(archive_, local + 18, local_comp) || !u32_at(archive_, local + 22, local_uncomp) ||
            !u16_at(archive_, local + 26, local_name_length) ||
            !u16_at(archive_, local + 28, local_extra_length)) {
            return zip_error("local header is missing or malformed for " + name);
        }
        if (local_flags != flags || local_method != method || local_name_length != name_length ||
            !range_ok(local + 30, static_cast<std::size_t>(local_name_length) + local_extra_length, cd_start) ||
            archive_.compare(local + 30, local_name_length, name) != 0) {
            return zip_error("local and central headers disagree for " + name);
        }
        if (method == 0 && (flags & 6U) != 0)
            return zip_error("stored member uses DEFLATE-only option flags: " + name);
        if ((flags & 8U) == 0) {
            if (local_crc != crc ||
                (local_comp != 0xffffffffU && local_comp != entry.compressed_size) ||
                (local_uncomp != 0xffffffffU && local_uncomp != entry.uncompressed_size)) {
                return zip_error("local sizes or CRC disagree for " + name);
            }
        }
        entry.data_offset = local + 30U + local_name_length + local_extra_length;
        if (entry.data_offset > cd_start || entry.compressed_size > cd_start - entry.data_offset)
            return zip_error("compressed member data is outside the archive: " + name);
        entry.data_end = entry.data_offset + entry.compressed_size;
        entry.record_end = entry.data_end;
        if ((flags & 8U) != 0) {
            const std::size_t begin = static_cast<std::size_t>(entry.data_end);
            auto descriptor_end = [&](bool with_signature, std::size_t& end) {
                std::size_t descriptor = begin;
                std::uint32_t value = 0;
                if (with_signature) {
                    if (!u32_at(archive_, descriptor, value) || value != kDescriptorSignature) return false;
                    descriptor += 4;
                }
                if (!u32_at(archive_, descriptor, value) || value != crc) return false;
                descriptor += 4;
                if (entry.zip64_sizes) {
                    std::uint64_t c = 0, u = 0;
                    if (!u64_at(archive_, descriptor, c) || !u64_at(archive_, descriptor + 8, u) ||
                        c != entry.compressed_size || u != entry.uncompressed_size) return false;
                    descriptor += 16;
                } else {
                    std::uint32_t c = 0, u = 0;
                    if (!u32_at(archive_, descriptor, c) || !u32_at(archive_, descriptor + 4, u) ||
                        c != entry.compressed_size || u != entry.uncompressed_size) return false;
                    descriptor += 8;
                }
                if (descriptor > cd_start) return false;
                end = descriptor;
                return true;
            };
            std::size_t descriptor = 0;
            // A signature is optional, and an unsigned descriptor's CRC can
            // itself equal 0x08074b50. Try the signed shape first, then the
            // unsigned shape instead of treating that CRC as unambiguously a
            // signature.
            if (!descriptor_end(true, descriptor) && !descriptor_end(false, descriptor))
                return zip_error("data descriptor is missing or inconsistent for " + name);
            entry.record_end = descriptor;
        }
        entries_.emplace(name, entry);
        names_.push_back(name);
        pos += 46U + variable;
    }
    if (pos != cd_end) return zip_error("central-directory size does not match its entries");

    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    ranges.reserve(entries_.size());
    for (const auto& item : entries_) ranges.emplace_back(item.second.local_offset, item.second.record_end);
    std::sort(ranges.begin(), ranges.end());
    for (std::size_t i = 1; i < ranges.size(); ++i) {
        if (ranges[i].first < ranges[i - 1].second) return zip_error("member records overlap");
    }
    return ok_error();
}

bool ZipArchive::contains(const std::string& name) const {
    return entries_.find(name) != entries_.end();
}

Error ZipArchive::read(const std::string& name, std::string& bytes) {
    bytes.clear();
    const auto cached = cache_.find(name);
    if (cached != cache_.end()) {
        bytes = cached->second;
        return ok_error();
    }
    const auto found = entries_.find(name);
    if (found == entries_.end()) return zip_error("required member is missing: " + name);
    const Entry& entry = found->second;
    if (cancellation_.cancelled()) return {ErrorCode::Cancelled, "DOCX member inflation cancelled: " + name};
    const std::size_t expected = static_cast<std::size_t>(entry.uncompressed_size);
    if (selected_bytes_ > limits_.max_total_selected_bytes ||
        expected > limits_.max_total_selected_bytes - selected_bytes_) {
        return {ErrorCode::UnsupportedFeature, "DOCX selected XML parts exceed the 128 MiB inflation limit"};
    }
    std::string output;
    try {
        output.reserve(expected);
        if (entry.method == 0) {
            output.assign(archive_.data() + static_cast<std::size_t>(entry.data_offset),
                          static_cast<std::size_t>(entry.compressed_size));
        } else {
            InflateGuard guard;
            if (inflateInit2(&guard.stream, -MAX_WBITS) != Z_OK)
                return {ErrorCode::Internal, "could not initialize zlib for DOCX inflation"};
            guard.initialized = true;
            std::size_t input_pos = static_cast<std::size_t>(entry.data_offset);
            const std::size_t input_end = static_cast<std::size_t>(entry.data_end);
            std::array<char, 32768> chunk{};
            int status = Z_OK;
            while (status != Z_STREAM_END) {
                if (cancellation_.cancelled()) return {ErrorCode::Cancelled, "DOCX member inflation cancelled: " + name};
                if (guard.stream.avail_in == 0 && input_pos < input_end) {
                    const std::size_t available = std::min<std::size_t>(input_end - input_pos,
                                                                       std::numeric_limits<uInt>::max());
                    guard.stream.next_in = reinterpret_cast<Bytef*>(archive_.data() + input_pos);
                    guard.stream.avail_in = static_cast<uInt>(available);
                    input_pos += available;
                }
                guard.stream.next_out = reinterpret_cast<Bytef*>(chunk.data());
                guard.stream.avail_out = static_cast<uInt>(chunk.size());
                status = inflate(&guard.stream, Z_NO_FLUSH);
                if (status != Z_OK && status != Z_STREAM_END)
                    return zip_error("raw DEFLATE stream is malformed for " + name);
                const std::size_t produced = chunk.size() - guard.stream.avail_out;
                if (output.size() > expected || produced > expected - output.size())
                    return zip_error("member inflated beyond its declared size: " + name);
                output.append(chunk.data(), produced);
                if (status != Z_STREAM_END && guard.stream.avail_in == 0 && input_pos == input_end && produced == 0)
                    return zip_error("raw DEFLATE stream is truncated for " + name);
            }
            if (guard.stream.avail_in != 0 || input_pos != input_end)
                return zip_error("raw DEFLATE stream has trailing compressed bytes: " + name);
        }
    } catch (const std::bad_alloc&) {
        return {ErrorCode::Internal, "not enough memory to inflate DOCX member: " + name};
    }
    if (output.size() != expected) return zip_error("member inflated size does not match metadata: " + name);
    const std::uint32_t actual_crc = static_cast<std::uint32_t>(
        crc32(0L, reinterpret_cast<const Bytef*>(output.data()), static_cast<uInt>(output.size())));
    if (actual_crc != entry.crc) return zip_error("CRC-32 check failed for " + name);
    selected_bytes_ += output.size();
    try {
        cache_.emplace(name, output);
        bytes = std::move(output);
    } catch (const std::bad_alloc&) {
        return {ErrorCode::Internal, "not enough memory to retain DOCX member: " + name};
    }
    return ok_error();
}

Error write_zip(const std::vector<ZipWriteEntry>& entries,
                std::size_t max_bytes,
                runtime::CancellationToken cancellation,
                std::string& bytes) {
    bytes.clear();
    if (entries.size() > 65535U) return {ErrorCode::UnsupportedFeature, "DOCX output has too many ZIP members"};
    struct Written {
        std::string name;
        std::uint32_t crc = 0;
        std::uint32_t compressed = 0;
        std::uint32_t uncompressed = 0;
        std::uint32_t offset = 0;
        std::uint16_t method = 0;
    };
    std::vector<Written> written;
    std::set<std::string> names;
    try {
        written.reserve(entries.size());
        for (const ZipWriteEntry& item : entries) {
            if (cancellation.cancelled()) return {ErrorCode::Cancelled, "DOCX serialization cancelled"};
            if (!safe_part_name(item.name) || !names.insert(item.name).second)
                return {ErrorCode::Internal, "DOCX writer produced an unsafe or duplicate ZIP member"};
            if (item.bytes.size() > std::numeric_limits<std::uint32_t>::max() ||
                bytes.size() > std::numeric_limits<std::uint32_t>::max())
                return {ErrorCode::UnsupportedFeature, "DOCX output requires unsupported Zip64 writing"};
            Written meta;
            meta.name = item.name;
            meta.crc = static_cast<std::uint32_t>(crc32(
                0L, reinterpret_cast<const Bytef*>(item.bytes.data()), static_cast<uInt>(item.bytes.size())));
            meta.uncompressed = static_cast<std::uint32_t>(item.bytes.size());
            meta.offset = static_cast<std::uint32_t>(bytes.size());
            meta.method = item.compress ? 8U : 0U;
            std::string compressed;
            if (item.compress) {
                DeflateGuard guard;
                if (deflateInit2(&guard.stream, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK)
                    return {ErrorCode::Internal, "could not initialize zlib for DOCX serialization"};
                guard.initialized = true;
                std::size_t pos = 0;
                std::array<char, 32768> chunk{};
                int status = Z_OK;
                while (status != Z_STREAM_END) {
                    if (cancellation.cancelled()) return {ErrorCode::Cancelled, "DOCX serialization cancelled"};
                    if (guard.stream.avail_in == 0 && pos < item.bytes.size()) {
                        const std::size_t count = std::min<std::size_t>(item.bytes.size() - pos,
                                                                      std::numeric_limits<uInt>::max());
                        guard.stream.next_in = reinterpret_cast<Bytef*>(
                            const_cast<char*>(item.bytes.data() + pos));
                        guard.stream.avail_in = static_cast<uInt>(count);
                        pos += count;
                    }
                    guard.stream.next_out = reinterpret_cast<Bytef*>(chunk.data());
                    guard.stream.avail_out = static_cast<uInt>(chunk.size());
                    status = deflate(&guard.stream,
                                     pos == item.bytes.size() && guard.stream.avail_in == 0 ? Z_FINISH : Z_NO_FLUSH);
                    if (status != Z_OK && status != Z_STREAM_END)
                        return {ErrorCode::Internal, "zlib failed while serializing DOCX"};
                    compressed.append(chunk.data(), chunk.size() - guard.stream.avail_out);
                }
            } else {
                compressed = item.bytes;
            }
            if (compressed.size() > std::numeric_limits<std::uint32_t>::max())
                return {ErrorCode::UnsupportedFeature, "DOCX compressed member is too large"};
            meta.compressed = static_cast<std::uint32_t>(compressed.size());
            const std::size_t record_size = 30U + item.name.size() + compressed.size();
            Error limit = check_limit(bytes.size(), record_size, max_bytes);
            if (!limit.ok()) return limit;
            append_u32(bytes, kLocalSignature);
            append_u16(bytes, 20);  // version needed
            append_u16(bytes, 0x0800U);  // UTF-8 names, no descriptor
            append_u16(bytes, meta.method);
            append_u16(bytes, 0);        // fixed time
            append_u16(bytes, 0x0021U);  // 1980-01-01
            append_u32(bytes, meta.crc);
            append_u32(bytes, meta.compressed);
            append_u32(bytes, meta.uncompressed);
            append_u16(bytes, static_cast<std::uint16_t>(item.name.size()));
            append_u16(bytes, 0);
            bytes += item.name;
            bytes += compressed;
            written.push_back(std::move(meta));
        }
        const std::size_t central_offset = bytes.size();
        for (const Written& meta : written) {
            Error limit = check_limit(bytes.size(), 46U + meta.name.size(), max_bytes);
            if (!limit.ok()) return limit;
            append_u32(bytes, kCentralSignature);
            append_u16(bytes, 20);  // made by, portable DOS subset
            append_u16(bytes, 20);
            append_u16(bytes, 0x0800U);
            append_u16(bytes, meta.method);
            append_u16(bytes, 0);
            append_u16(bytes, 0x0021U);
            append_u32(bytes, meta.crc);
            append_u32(bytes, meta.compressed);
            append_u32(bytes, meta.uncompressed);
            append_u16(bytes, static_cast<std::uint16_t>(meta.name.size()));
            append_u16(bytes, 0);
            append_u16(bytes, 0);
            append_u16(bytes, 0);
            append_u16(bytes, 0);
            append_u32(bytes, 0);
            append_u32(bytes, meta.offset);
            bytes += meta.name;
        }
        const std::size_t central_size = bytes.size() - central_offset;
        if (central_offset > std::numeric_limits<std::uint32_t>::max() ||
            central_size > std::numeric_limits<std::uint32_t>::max())
            return {ErrorCode::UnsupportedFeature, "DOCX output requires unsupported Zip64 writing"};
        Error limit = check_limit(bytes.size(), 22, max_bytes);
        if (!limit.ok()) return limit;
        append_u32(bytes, kEndSignature);
        append_u16(bytes, 0);
        append_u16(bytes, 0);
        append_u16(bytes, static_cast<std::uint16_t>(written.size()));
        append_u16(bytes, static_cast<std::uint16_t>(written.size()));
        append_u32(bytes, static_cast<std::uint32_t>(central_size));
        append_u32(bytes, static_cast<std::uint32_t>(central_offset));
        append_u16(bytes, 0);
    } catch (const std::bad_alloc&) {
        bytes.clear();
        return {ErrorCode::Internal, "not enough memory to serialize DOCX ZIP"};
    } catch (const std::length_error&) {
        bytes.clear();
        return {ErrorCode::UnsupportedFeature, "DOCX output is too large to serialize"};
    }
    return ok_error();
}

}  // namespace ainiux::docx::detail
