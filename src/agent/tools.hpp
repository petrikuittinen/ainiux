#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "agent/index/index.hpp"
#include "common.hpp"
#include "provider/provider.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::agent {

struct SourceRange {
    std::string path;
    std::string content;
    std::string file_hash;
    std::string range_hash;
    std::size_t start_line = 1;
    std::size_t end_line = 0;
    std::size_t bytes = 0;
    bool truncated = false;
    bool redacted = false;
};

class ReadToolRegistry {
   public:
    ReadToolRegistry() = default;

    static Error create(index::Options index_options,
                        index::Snapshot snapshot,
                        std::vector<std::string> secrets,
                        ReadToolRegistry& registry);

    const index::Snapshot& snapshot() const { return snapshot_; }
    std::vector<provider::FunctionDefinition> definitions() const;
    std::string execute(const std::string& name,
                        const std::string& arguments_json,
                        runtime::CancellationToken cancellation = runtime::CancellationToken()) const;
    Error read_source(const std::string& path,
                      std::size_t start_line,
                      std::size_t end_line,
                      std::size_t max_bytes,
                      SourceRange& range) const;

   private:
    index::Options index_options_;
    index::Snapshot snapshot_;
    std::vector<std::string> secrets_;
    std::map<std::string, const index::IndexedFile*> files_;
};

std::string tool_error_result(const std::string& code, const std::string& message);

}  // namespace ainiux::agent
