#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "agent/approval.hpp"
#include "agent/index/index.hpp"
#include "common.hpp"
#include "fetch/fetch.hpp"
#include "json/json.hpp"
#include "provider/provider.hpp"
#include "runtime/runtime.hpp"
#include "search/search.hpp"

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
// write_file, edit_file, str_replace (with fuzzy fallback), remove, apply_patch,
// and network tools (fetch_url / search_web) when allow_network is also true.
struct HistoryBackupPolicy {
    bool enabled = true;
    std::size_t max_bytes = 1024U * 1024U;  // 1 MiB
    int ttl_days = 7;
};

struct ToolRegistryOptions {
    bool allow_mutations = false;
    // Network tools reuse src/fetch and src/search. Disabled for security-review.
    bool allow_network = false;
    HistoryBackupPolicy history_backup;
    fetch::Options fetch_options;
    search::Options search_options = search::default_options();
    // Interactive Guard Ask. Empty ⇒ headless Deny for Ask decisions.
    GuardApprovalCallback on_guard_ask;
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
                                     bool allow_fuzzy,
                                     std::size_t hint_start_line,
                                     std::size_t hint_end_line,
                                     const std::string& expected_file_hash,
                                     std::string& history_path,
                                     std::size_t& matches_found,
                                     std::size_t& replacements_made,
                                     std::string& match_mode,
                                     std::string& old_hash,
                                     std::string& new_hash,
                                     std::vector<std::string>& candidate_lines) const;
    Error edit_workspace_file(const std::string& relative_path,
                              const std::string& expected_file_hash,
                              const json::Value& ops,
                              bool create_dirs,
                              std::string& history_path,
                              std::string& old_hash,
                              std::string& new_hash,
                              std::size_t& operations_applied,
                              std::vector<std::string>& summary,
                              std::vector<std::string>& warnings) const;
    Error remove_workspace_path(const std::string& relative_path,
                                bool recursive,
                                bool confirm,
                                const std::string& expected_file_hash,
                                std::string& history_path,
                                bool& was_directory,
                                std::string& guard_decision,
                                std::string& guard_rule_id,
                                std::string& old_hash,
                                std::vector<std::string>& suggestions,
                                std::vector<std::string>& warnings) const;
    Error apply_workspace_patch(const std::string& patch_text,
                                bool atomic,
                                bool allow_fuzzy,
                                std::vector<std::string>& files_changed,
                                std::size_t& operations_applied,
                                std::map<std::string, std::string>& new_hashes,
                                std::string& reverse_patch_path,
                                std::vector<std::string>& summary,
                                std::vector<std::string>& warnings) const;
    void note_written_file(const std::string& relative_path, const std::string& content) const;
    void note_removed_path(const std::string& relative_path) const;
    void rebuild_file_map() const;
    Error resolve_writable_path(const std::string& relative_path, std::filesystem::path& absolute) const;
    Error save_history_copy(const std::string& relative_path,
                            const std::string& previous_content,
                            std::string& history_path) const;
    Error purge_expired_history_backups() const;

    // Shared by run_command / remove when Guard returns Ask.
    GuardApprovalDecision request_guard_approval(const GuardApprovalRequest& request,
                                                 runtime::CancellationToken cancellation) const;

    index::Options index_options_;
    // Mutable so const execute() can refresh hashes after agent writes without
    // forcing security-review call sites off const references.
    mutable index::Snapshot snapshot_;
    std::vector<std::string> secrets_;
    mutable std::map<std::string, const index::IndexedFile*> files_;
    bool allow_mutations_ = false;
    bool allow_network_ = false;
    HistoryBackupPolicy history_backup_{};
    fetch::Options fetch_options_{};
    search::Options search_options_{};
    GuardApprovalCallback on_guard_ask_;
};

std::string tool_error_result(const std::string& code, const std::string& message);

}  // namespace ainiux::agent
