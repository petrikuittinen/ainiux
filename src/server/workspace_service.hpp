#pragma once

#include <cstddef>
#include <string>

#include "common.hpp"

namespace ainiux::server {

// Read-only access to the one canonical workspace owned by a control listener.
// All paths accepted here are wire-format relative paths; callers never receive
// the native workspace root or any other absolute filesystem path.
class WorkspaceService {
   public:
    explicit WorkspaceService(std::string workspace);

    WorkspaceService(const WorkspaceService&) = delete;
    WorkspaceService& operator=(const WorkspaceService&) = delete;

    Error list(const std::string& relative_path, std::string& body) const;
    Error read(const std::string& relative_path, std::string& body) const;
    Error review(std::string& body) const;

   private:
    std::string workspace_;
};

}  // namespace ainiux::server
