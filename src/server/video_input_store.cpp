#include "server/video_input_store.hpp"

#include <atomic>
#include <unordered_set>

#include "platform/filesystem.hpp"
#include "server/limits.hpp"

namespace ainiux::server {
namespace {

bool is_supported_image(const std::string& mime) {
    return mime == "image/png" || mime == "image/jpeg" || mime == "image/webp" ||
           mime == "image/gif" || mime == "image/bmp" || mime == "image/tiff" ||
           mime == "image/heic" || mime == "image/heif";
}

bool is_supported_video(const std::string& mime) {
    return mime == "video/mp4" || mime == "video/quicktime";
}

bool is_supported_audio(const std::string& mime) {
    return mime == "audio/mpeg" || mime == "audio/wav" || mime == "audio/x-wav";
}

Error validate_signature(const std::string& mime, const std::string& bytes) {
    if (is_supported_video(mime) &&
        (bytes.size() < 12 || bytes.compare(4, 4, "ftyp") != 0)) {
        return {ErrorCode::BadArgs, "uploaded video signature does not match MP4/MOV"};
    }
    if (mime == "image/png" &&
        (bytes.size() < 8 || bytes.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) != 0)) {
        return {ErrorCode::BadArgs, "uploaded image signature does not match PNG"};
    }
    if (mime == "image/jpeg" &&
        (bytes.size() < 3 || static_cast<unsigned char>(bytes[0]) != 0xff ||
         static_cast<unsigned char>(bytes[1]) != 0xd8)) {
        return {ErrorCode::BadArgs, "uploaded image signature does not match JPEG"};
    }
    if (mime == "image/webp" &&
        (bytes.size() < 12 || bytes.compare(0, 4, "RIFF") != 0 ||
         bytes.compare(8, 4, "WEBP") != 0)) {
        return {ErrorCode::BadArgs, "uploaded image signature does not match WebP"};
    }
    if (mime == "image/gif" &&
        (bytes.size() < 6 || (bytes.compare(0, 6, "GIF87a") != 0 &&
                              bytes.compare(0, 6, "GIF89a") != 0))) {
        return {ErrorCode::BadArgs, "uploaded image signature does not match GIF"};
    }
    if (mime == "image/bmp" && (bytes.size() < 2 || bytes.compare(0, 2, "BM") != 0)) {
        return {ErrorCode::BadArgs, "uploaded image signature does not match BMP"};
    }
    if (mime == "image/tiff" &&
        (bytes.size() < 4 || (bytes.compare(0, 4, "II*\0", 4) != 0 &&
                              bytes.compare(0, 4, "MM\0*", 4) != 0))) {
        return {ErrorCode::BadArgs, "uploaded image signature does not match TIFF"};
    }
    if ((mime == "image/heic" || mime == "image/heif") &&
        (bytes.size() < 12 || bytes.compare(4, 4, "ftyp") != 0)) {
        return {ErrorCode::BadArgs, "uploaded image signature does not match HEIC/HEIF"};
    }
    if ((mime == "audio/wav" || mime == "audio/x-wav") &&
        (bytes.size() < 12 || bytes.compare(0, 4, "RIFF") != 0 ||
         bytes.compare(8, 4, "WAVE") != 0)) {
        return {ErrorCode::BadArgs, "uploaded audio signature does not match WAV"};
    }
    if (mime == "audio/mpeg" &&
        !(bytes.size() >= 3 && bytes.compare(0, 3, "ID3") == 0) &&
        !(bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xff &&
          (static_cast<unsigned char>(bytes[1]) & 0xe0) == 0xe0)) {
        return {ErrorCode::BadArgs, "uploaded audio signature does not match MP3"};
    }
    return ok_error();
}

}  // namespace

struct VideoInputStore::Budget {
    explicit Budget(std::size_t configured_capacity) : capacity(configured_capacity) {}

    std::atomic<std::size_t> resident{0};
    const std::size_t capacity;
};

VideoInputStore::VideoInputStore(std::size_t capacity, std::chrono::seconds lifetime)
    : budget_(std::make_shared<Budget>(capacity)), lifetime_(lifetime) {}

void VideoInputStore::expire(std::chrono::system_clock::time_point now) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.expires <= now) it = entries_.erase(it);
        else ++it;
    }
}

Error VideoInputStore::add(std::string mime, std::string bytes, StoredVideoInput& output) {
    if (bytes.empty() || bytes.size() > Limits::video_upload_body_bytes) {
        return {ErrorCode::BadArgs,
                "video reference upload is empty or exceeds the 200 MiB limit"};
    }
    const bool image = is_supported_image(mime);
    const bool video = is_supported_video(mime);
    const bool audio = is_supported_audio(mime);
    if (!image && !video && !audio) {
        return {ErrorCode::UnsupportedFeature, "unsupported video reference Content-Type"};
    }
    if (image && bytes.size() > 30U * 1024U * 1024U) {
        return {ErrorCode::BadArgs, "video reference images are limited to 30 MiB"};
    }
    if (audio && bytes.size() > 15U * 1024U * 1024U) {
        return {ErrorCode::BadArgs, "video reference audio is limited to 15 MiB"};
    }
    Error error = validate_signature(mime, bytes);
    if (!error.ok()) return error;

    std::lock_guard<std::mutex> lock(mutex_);
    expire(std::chrono::system_clock::now());
    std::size_t resident = budget_->resident.load();
    do {
        if (resident > budget_->capacity || bytes.size() > budget_->capacity - resident) {
            return {ErrorCode::RateLimit, "temporary video input storage is full"};
        }
    } while (!budget_->resident.compare_exchange_weak(resident, resident + bytes.size()));

    auto budget = budget_;
    const std::size_t byte_count = bytes.size();
    std::shared_ptr<const std::string> payload(
        new std::string(std::move(bytes)), [budget, byte_count](const std::string* value) {
            delete value;
            budget->resident.fetch_sub(byte_count);
        });
    const auto expires = std::chrono::system_clock::now() + lifetime_;
    for (int attempt = 0; attempt < 4; ++attempt) {
        std::string random;
        error = platform::secure_random_hex(16, random);
        if (!error.ok()) return error;
        const std::string id = "video_input_" + random;
        const auto inserted = entries_.emplace(id, Entry{mime, payload, expires});
        if (inserted.second) {
            output = {id, std::move(mime), std::move(payload), expires};
            return ok_error();
        }
    }
    return {ErrorCode::Internal,
            "could not allocate a unique temporary video input identifier"};
}

Error VideoInputStore::resolve(const std::vector<std::string>& ids,
                               std::vector<StoredVideoInput>& output) {
    output.clear();
    if (ids.size() > Limits::video_input_count) {
        return {ErrorCode::BadArgs, "input_media_ids accepts at most 50 files"};
    }
    std::unordered_set<std::string> unique;
    std::size_t total = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    expire(std::chrono::system_clock::now());
    for (const std::string& id : ids) {
        if (!unique.insert(id).second) {
            return {ErrorCode::BadArgs, "input_media_ids contains duplicates"};
        }
        const auto found = entries_.find(id);
        if (found == entries_.end()) {
            return {ErrorCode::FileRead, "uploaded video input is missing or expired"};
        }
        if (found->second.bytes->size() > Limits::video_job_upload_bytes - total) {
            return {ErrorCode::BadArgs, "video references exceed the combined job limit"};
        }
        total += found->second.bytes->size();
        output.push_back({id, found->second.mime, found->second.bytes, found->second.expires});
    }
    return ok_error();
}

bool VideoInputStore::erase(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    expire(std::chrono::system_clock::now());
    return entries_.erase(id) != 0;
}

}  // namespace ainiux::server
