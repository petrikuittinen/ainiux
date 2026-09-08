#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common.hpp"

namespace ainiux::server {

struct StoredVideoInput {
    std::string id;
    std::string mime_type;
    std::shared_ptr<const std::string> bytes;
    std::chrono::system_clock::time_point expires_at;
};

class VideoInputStore {
  public:
    explicit VideoInputStore(std::size_t capacity,
                             std::chrono::seconds lifetime = std::chrono::hours(1));
    Error add(std::string mime, std::string bytes, StoredVideoInput& output);
    Error resolve(const std::vector<std::string>& ids, std::vector<StoredVideoInput>& output);
    bool erase(const std::string& id);

  private:
    struct Budget;
    struct Entry {
        std::string mime;
        std::shared_ptr<const std::string> bytes;
        std::chrono::system_clock::time_point expires;
    };

    void expire(std::chrono::system_clock::time_point now);

    std::shared_ptr<Budget> budget_;
    std::chrono::seconds lifetime_;
    std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
};

}  // namespace ainiux::server
