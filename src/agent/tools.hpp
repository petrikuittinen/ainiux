#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
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

struct IndexRefreshState;
struct IndexOverlayEntry {
    bool removed = false;
    index::IndexedFile file;
    std::vector<index::IndexedSymbol> symbols;
    std::size_t revision = 0;
};

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

enum class MutationPolicy { Disabled, PlanningDocuments, Full };
enum class IndexAccessMode {
    Disabled,
    LazyHints,
    SnapshotAuthorization,
};

// Snapshot-backed workspace tools. Security review disables mutation, Plan
// limits writes to approved planning Markdown, and Act enables full mutation.
struct HistoryBackupPolicy {
    bool enabled = true;
    std::size_t max_bytes = 1024U * 1024U;  // 1 MiB
    int ttl_days = 7;
};

struct ToolRegistryOptions {
    MutationPolicy mutation_policy = MutationPolicy::Disabled;
    // Network tools reuse src/fetch and src/search. Disabled for security-review.
    bool allow_network = false;
    HistoryBackupPolicy history_backup;
    fetch::Options fetch_options;
    search::Options search_options = search::default_options();
    // Interactive Guard Ask. Empty ⇒ headless Deny for Ask decisions.
    GuardApprovalCallback on_guard_ask;
    PermissionMode permission_mode = PermissionMode::Smart;
    bool permission_controls = false;
    bool indexing_enabled = true;
    IndexAccessMode index_access_mode =
        IndexAccessMode::SnapshotAuthorization;
};

class ReadToolRegistry {
   public:
    ReadToolRegistry();
    ~ReadToolRegistry();
    ReadToolRegistry(const ReadToolRegistry&) = delete;
    ReadToolRegistry& operator=(const ReadToolRegistry&) = delete;
    ReadToolRegistry(ReadToolRegistry&&) noexcept;
    ReadToolRegistry& operator=(ReadToolRegistry&&) noexcept;

    static Error create(index::Options index_options,
                        index::Snapshot snapshot,
                        std::vector<std::string> secrets,
                        ReadToolRegistry& registry,
                        ToolRegistryOptions options = {});
    static Error create_without_index(std::string workspace,
                                      std::vector<std::string> secrets,
                                      ReadToolRegistry& registry,
                                      ToolRegistryOptions options = {});
    static Error create_without_index(index::Options index_options,
                                      std::vector<std::string> secrets,
                                      ReadToolRegistry& registry,
                                      ToolRegistryOptions options = {});
    static Error create_lazy(index::Options index_options,
                             std::vector<std::string> secrets,
                             ReadToolRegistry& registry,
                             ToolRegistryOptions options = {});

    const index::Snapshot& snapshot() const { return snapshot_; }
    bool indexing_enabled() const { return indexing_enabled_; }
    // Promote a live-filesystem Agent registry into an indexed registry without
    // resetting its permission, mutation, network, or approval state.
    Error enable_persistent_index(index::Options index_options,
                                  index::Snapshot snapshot);
    Error enable_lazy_index(index::Options index_options);
    void enqueue_background_freshness() const;
    bool allow_mutations() const { return mutation_policy_ != MutationPolicy::Disabled; }
    MutationPolicy mutation_policy() const { return mutation_policy_; }
    void set_mutation_policy(MutationPolicy policy) { mutation_policy_ = policy; }
    PermissionMode permission_mode() const { return permission_mode_; }
    void set_permission_mode(PermissionMode mode) { permission_mode_ = mode; }
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
    // Wait for queued mutation-aware persistence, optionally enqueue a full-tree
    // incremental freshness pass, then publish the newest complete snapshot.
    Error refresh_persistent_index(
        bool full_tree,
        runtime::CancellationToken cancellation = runtime::CancellationToken()) const;
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
    std::size_t queue_index_paths(const std::vector<std::string>& paths,
                                  bool full_tree = false) const;
    void merge_index_overlay() const;
    void rebuild_file_map() const;
    Error resolve_writable_path(const std::string& relative_path, std::filesystem::path& absolute) const;
    Error normalize_mutation_path(const std::string& input, std::string& relative) const;
    Error validate_mutation_path(const std::string& relative_path,
                                 bool create_dirs,
                                 bool deleting) const;
    Error save_history_copy(const std::string& relative_path,
                            const std::string& previous_content,
                            std::string& history_path) const;
    Error purge_expired_history_backups() const;
    Error read_external_source(const std::filesystem::path& absolute_path,
                               std::size_t start_line,
                               std::size_t end_line,
                               std::size_t max_bytes,
                               SourceRange& range,
                               bool approved_external = true) const;
    Error read_workspace_source(const std::string& relative_path,
                                std::size_t start_line,
                                std::size_t end_line,
                                std::size_t max_bytes,
                                SourceRange& range) const;
    Error write_external_file(const std::filesystem::path& absolute_path,
                              const std::string& content,
                              bool create_dirs,
                              const std::string& mode,
                              const std::string& expected_file_hash,
                              bool& created,
                              std::string& old_hash,
                              std::string& new_hash) const;

    // Shared by run_command / remove when Guard returns Ask.
    GuardApprovalDecision request_guard_approval(const GuardApprovalRequest& request,
                                                 runtime::CancellationToken cancellation) const;
    GuardApprovalDecision request_permission(const std::string& tool_name,
                                              const std::string& preview,
                                              const std::vector<std::string>& arguments,
                                              bool outside_project,
                                              bool under_system_temp,
                                              bool write,
                                              bool destructive,
                                              const std::string& specific_rule,
                                              const std::string& specific_message,
                                              runtime::CancellationToken cancellation) const;

    index::Options index_options_;
    // Mutable so const execute() can refresh hashes after agent writes without
    // forcing security-review call sites off const references.
    mutable index::Snapshot snapshot_;
    std::vector<std::string> secrets_;
    mutable std::map<std::string, const index::IndexedFile*> files_;
    std::unique_ptr<IndexRefreshState> index_refresh_;
    mutable std::size_t loaded_index_generation_ = 0;
    mutable std::map<std::string, IndexOverlayEntry> index_overlay_;
    MutationPolicy mutation_policy_ = MutationPolicy::Disabled;
    bool allow_network_ = false;
    HistoryBackupPolicy history_backup_{};
    fetch::Options fetch_options_{};
    search::Options search_options_{};
    GuardApprovalCallback on_guard_ask_;
    PermissionMode permission_mode_ = PermissionMode::Smart;
    bool permission_controls_ = false;
    bool indexing_enabled_ = true;
    IndexAccessMode index_access_mode_ =
        IndexAccessMode::SnapshotAuthorization;
};

std::string tool_error_result(const std::string& code, const std::string& message);

}  // namespace ainiux::agent
