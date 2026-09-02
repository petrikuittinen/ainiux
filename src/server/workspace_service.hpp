#pragma once

#include <cstddef>
#include <mutex>
#include <string>

#include "common.hpp"

namespace ainiux::server {

struct WorkspaceFileSnapshot {
    std::string path;
    std::string content;
    std::string revision;
};

// Root-aware access to the one canonical workspace owned by a control listener.
// All paths and mutation targets are wire-format relative paths. Callers never
// receive the native workspace root or any other absolute filesystem path.
class WorkspaceService {
   public:
    explicit WorkspaceService(std::string workspace);

    WorkspaceService(const WorkspaceService&) = delete;
    WorkspaceService& operator=(const WorkspaceService&) = delete;

    Error list(const std::string& relative_path, std::string& body) const;
    Error read(const std::string& relative_path, std::string& body) const;
    Error review(std::string& body) const;
    Error load_file(const std::string& relative_path,
                    const std::string& expected_revision,
                    WorkspaceFileSnapshot& snapshot,
                    std::string* current_revision = nullptr) const;
    Error save(const std::string& relative_path,
               const std::string& request_body,
               std::string& body,
               std::string& current_revision);
    Error create_file(const std::string& request_body,
                      std::string& body,
                      std::string& current_revision);
    Error mutate(const std::string& request_body, std::string& body);

   private:
    std::string workspace_;
    mutable std::mutex mutex_;
};

}  // namespace ainiux::server
