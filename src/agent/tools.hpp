#pragma once

#include <cstddef>
#include <filesystem>
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

// Snapshot-backed workspace tools. Security-review keeps allow_mutations=false
// (read/search/inspect only). Agent mode sets allow_mutations=true to expose
// write_file and exact str_replace for ordinary in-workspace edits.
struct ToolRegistryOptions {
    bool allow_mutations = false;
};

class ReadToolRegistry {
   public:
    ReadToolRegistry() = default;

    static Error create(index::Options index_options,
                        index::Snapshot snapshot,
                        std::vector<std::string> secrets,
                        ReadToolRegistry& registry,
                        ToolRegistryOptions options = {});

    const index::Snapshot& snapshot() const { return snapshot_; }
    bool allow_mutations() const { return allow_mutations_; }
    std::vector<provider::FunctionDefinition> definitions() const;
    // Mutating tools update the in-memory snapshot so later reads in the same
    // run see the new file hashes. Security-review never enables mutations.
    std::string execute(const std::string& name,
                        const std::string& arguments_json,
                        runtime::CancellationToken cancellation = runtime::CancellationToken()) const;
    Error read_source(const std::string& path,
                      std::size_t start_line,
                      std::size_t end_line,
                      std::size_t max_bytes,
                      SourceRange& range) const;

   private:
    Error write_workspace_file(const std::string& relative_path,
                               const std::string& content,
                               bool create_dirs,
                               const std::string& mode,
                               const std::string& expected_file_hash,
                               std::string& history_path,
                               bool& created,
                               std::string& old_hash,
                               std::string& new_hash) const;
    Error str_replace_workspace_file(const std::string& relative_path,
                                     const std::string& old_text,
                                     const std::string& new_text,
                                     bool replace_all,
                                     const std::string& expected_file_hash,
                                     std::string& history_path,
                                     std::size_t& matches_found,
                                     std::size_t& replacements_made,
                                     std::string& old_hash,
                                     std::string& new_hash) const;
    void note_written_file(const std::string& relative_path, const std::string& content) const;
    void rebuild_file_map() const;
    Error resolve_writable_path(const std::string& relative_path, std::filesystem::path& absolute) const;
    Error save_history_copy(const std::string& relative_path,
                            const std::string& previous_content,
                            std::string& history_path) const;

    index::Options index_options_;
    // Mutable so const execute() can refresh hashes after agent writes without
    // forcing security-review call sites off const references.
    mutable index::Snapshot snapshot_;
    std::vector<std::string> secrets_;
    mutable std::map<std::string, const index::IndexedFile*> files_;
    bool allow_mutations_ = false;
    mutable std::size_t history_sequence_ = 0;
};

std::string tool_error_result(const std::string& code, const std::string& message);

}  // namespace ainiux::agent
