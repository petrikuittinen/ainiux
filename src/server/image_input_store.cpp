#include "server/image_input_store.hpp"

#include <atomic>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <unordered_set>
#include <utility>

#include "input/input.hpp"
#include "platform/filesystem.hpp"
#include "server/limits.hpp"

namespace ainiux::server {

struct ImageInputStore::Budget {
    explicit Budget(std::size_t maximum) : capacity(maximum) {}
    std::atomic<std::size_t> resident{0};
    const std::size_t capacity;
};

ImageInputStore::ImageInputStore(std::size_t byte_capacity,
                                 std::chrono::seconds lifetime)
    : budget_(std::make_shared<Budget>(byte_capacity)), lifetime_(lifetime) {}

void ImageInputStore::erase_expired_locked(std::chrono::system_clock::time_point now) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.expires_at <= now) it = entries_.erase(it);
        else ++it;
    }
}

Error ImageInputStore::add(std::string mime_type,
                           std::string bytes,
                           StoredImageInput& output) {
    if (bytes.size() > Limits::upload_body_bytes) {
        return {ErrorCode::BadArgs, "image upload exceeds the 20 MiB per-file limit"};
    }
    Error error = input::validate_image_bytes(bytes, mime_type);
    if (!error.ok()) return error;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        erase_expired_locked(std::chrono::system_clock::now());
    }

    std::size_t observed = budget_->resident.load(std::memory_order_acquire);
    do {
        if (observed > budget_->capacity || bytes.size() > budget_->capacity - observed) {
            return {ErrorCode::RateLimit,
                    "temporary image upload storage is full; remove inputs or retry later"};
        }
    } while (!budget_->resident.compare_exchange_weak(
        observed, observed + bytes.size(), std::memory_order_acq_rel));

    const std::shared_ptr<Budget> budget = budget_;
    const std::size_t byte_count = bytes.size();
    std::shared_ptr<const std::string> payload(
        new std::string(std::move(bytes)),
        [budget, byte_count](const std::string* value) {
            delete value;
            budget->resident.fetch_sub(byte_count, std::memory_order_acq_rel);
        });
    const auto expires = std::chrono::system_clock::now() + lifetime_;
    std::string id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (int attempt = 0; attempt < 8; ++attempt) {
            std::string random;
            error = platform::secure_random_hex(16U, random);
            if (!error.ok()) return error;
            id = "input_" + random;
            if (entries_.find(id) == entries_.end()) break;
            id.clear();
        }
        if (id.empty()) {
            return {ErrorCode::Internal,
                    "could not allocate a unique temporary image identifier"};
        }
        entries_.emplace(id, Entry{mime_type, payload, expires});
    }
    output = {id, std::move(mime_type), std::move(payload), expires};
    return ok_error();
}

Error ImageInputStore::resolve(const std::vector<std::string>& ids,
                               std::vector<StoredImageInput>& output) {
    output.clear();
    if (ids.size() > Limits::image_input_count) {
        return {ErrorCode::BadArgs, "input_image_ids accepts at most 16 images"};
    }
    std::unordered_set<std::string> unique;
    std::size_t total = 0;
    const auto now = std::chrono::system_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    erase_expired_locked(now);
    for (const std::string& id : ids) {
        if (!unique.insert(id).second) {
            return {ErrorCode::BadArgs, "input_image_ids must not contain duplicates"};
        }
        const auto found = entries_.find(id);
        if (found == entries_.end()) {
            return {ErrorCode::FileRead,
                    "uploaded image input is missing or expired: " + id};
        }
        if (found->second.bytes->size() > Limits::image_job_upload_bytes - total) {
            return {ErrorCode::BadArgs,
                    "reference images exceed the 40 MiB combined job limit"};
        }
        total += found->second.bytes->size();
        output.push_back({id, found->second.mime_type, found->second.bytes,
                          found->second.expires_at});
    }
    return ok_error();
}

bool ImageInputStore::erase(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    erase_expired_locked(std::chrono::system_clock::now());
    return entries_.erase(id) != 0;
}

std::size_t ImageInputStore::resident_bytes() const {
    return budget_->resident.load(std::memory_order_acquire);
}

std::string image_input_expiry_timestamp(std::chrono::system_clock::time_point value) {
    const std::time_t raw = std::chrono::system_clock::to_time_t(value);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &raw);
#else
    gmtime_r(&raw, &utc);
#endif
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

}  // namespace ainiux::server
