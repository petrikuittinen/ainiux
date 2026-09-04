#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common.hpp"

namespace ainiux::server {

struct StoredImageInput {
    std::string id;
    std::string mime_type;
    std::shared_ptr<const std::string> bytes;
    std::chrono::system_clock::time_point expires_at;
};

class ImageInputStore {
   public:
    explicit ImageInputStore(
        std::size_t byte_capacity,
        std::chrono::seconds lifetime = std::chrono::hours(1));
    ImageInputStore(const ImageInputStore&) = delete;
    ImageInputStore& operator=(const ImageInputStore&) = delete;

    Error add(std::string mime_type, std::string bytes, StoredImageInput& output);
    Error resolve(const std::vector<std::string>& ids,
                  std::vector<StoredImageInput>& output);
    bool erase(const std::string& id);
    std::size_t resident_bytes() const;

   private:
    struct Budget;
    struct Entry {
        std::string mime_type;
        std::shared_ptr<const std::string> bytes;
        std::chrono::system_clock::time_point expires_at;
    };

    void erase_expired_locked(std::chrono::system_clock::time_point now);

    std::shared_ptr<Budget> budget_;
    std::chrono::seconds lifetime_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
};

std::string image_input_expiry_timestamp(std::chrono::system_clock::time_point value);

}  // namespace ainiux::server
