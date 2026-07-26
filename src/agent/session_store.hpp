#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common.hpp"

struct sqlite3;

namespace ainiux::agent {

// Project-local agent UI/session database at .ainiux-pr/agent.sqlite (never
// ~/.ainiux/ainiux.db). One durable transcript thread per project.

inline constexpr int kAgentSessionSchemaVersion = 2;

struct AgentProjectRecord {
    long long id = 1;
    long long created_at = 0;
    long long updated_at = 0;
    std::string status;  // idle | running
    std::string provider;
    std::string model;
    std::string api;
    std::string protocol;  // native | xml
    std::string base_url;
    std::string settings_json = "{}";
    std::string workspace;
    std::string summary_text;
    long long turns = 0;
    long long tool_calls = 0;
};

struct AgentMessageRecord {
    long long id = 0;
    long long seq = 0;
    long long created_at = 0;
    std::string role;  // user | assistant | tool | notice | summary
    std::string content;
    std::string tool_name;
    bool tool_ok = true;
    std::string args_preview;
};

struct AgentApprovalRecord {
    long long id = 0;
    long long created_at = 0;
    std::string tool_name;
    std::string command_preview;
    std::string rule_id;
    std::string decision;  // allow | deny | cancelled
    std::string source;    // interactive | headless
    std::string message;
};

// Backward-compatible aliases used by older call sites during transition.
using AgentSessionRecord = AgentProjectRecord;

class AgentSessionStore {
   public:
    AgentSessionStore() = default;
    ~AgentSessionStore();
    AgentSessionStore(const AgentSessionStore&) = delete;
    AgentSessionStore& operator=(const AgentSessionStore&) = delete;
    AgentSessionStore(AgentSessionStore&& other) noexcept;
    AgentSessionStore& operator=(AgentSessionStore&& other) noexcept;

    Error open(const std::string& workspace);
    void close();
    bool is_open() const { return db_ != nullptr; }
    const std::string& path() const { return path_; }
    const std::string& workspace() const { return workspace_; }

    static std::string database_path(const std::string& workspace);

    // Open or create the singleton project row (id=1). Fills record.
    Error open_project(AgentProjectRecord& record);

    // Legacy name: same as open_project (single thread).
    Error create_session(AgentSessionRecord& record) { return open_project(record); }

    Error update_project_meta(const AgentProjectRecord& record);

    Error append_message(const std::string& role,
                         const std::string& content,
                         const std::string& tool_name = {},
                         bool tool_ok = true,
                         const std::string& args_preview = {});

    // Legacy overload used by older runtime (session_id ignored; singleton).
    Error append_message(long long /*session_id*/,
                         const std::string& role,
                         const std::string& content,
                         const std::string& tool_call_id = {});

    Error append_tool_event(long long /*session_id*/,
                            long long turn,
                            const std::string& call_id,
                            const std::string& tool_name,
                            const std::string& arguments,
                            const std::string& result,
                            bool ok);

    Error load_messages(std::vector<AgentMessageRecord>& messages, int limit = 0) const;

    // Atomically persist a compaction summary event and project summary metadata.
    // Original transcript rows are retained. keep_recent describes the request
    // projection rebuilt by AgentSessionRuntime and is kept for API compatibility.
    Error compact_with_summary(const std::string& summary_text, int keep_recent = 12);

    Error set_status(const std::string& status);
    Error bump_counters(long long turns_delta, long long tool_calls_delta);

    // Legacy finish_session: mark idle and store final note in summary if provided.
    Error finish_session(long long /*session_id*/,
                         const std::string& status,
                         const std::string& final_text,
                         const std::string& error_code,
                         const std::string& error_message,
                         long long turns,
                         long long tool_calls);

    Error list_sessions(std::vector<AgentSessionRecord>& sessions, int limit = 50) const;
    Error load_session(long long session_id,
                       AgentSessionRecord& session,
                       std::vector<AgentMessageRecord>& messages,
                       std::vector<struct AgentToolEventRecord>& tool_events) const;

    // Persist one Guard Ask resolution (interactive y/n or headless Deny).
    Error record_approval(const AgentApprovalRecord& record);
    Error load_approvals(std::vector<AgentApprovalRecord>& approvals, int limit = 100) const;

   private:
    Error ensure_schema();
    Error ensure_approvals_table();
    Error ensure_project_settings_columns();
    Error next_seq(long long& seq);
    Error touch();

    sqlite3* db_ = nullptr;
    std::string path_;
    std::string workspace_;
};

// Kept for unit tests that still reference tool_events table shape.
struct AgentToolEventRecord {
    long long id = 0;
    long long session_id = 0;
    long long seq = 0;
    long long created_at = 0;
    long long turn = 0;
    std::string call_id;
    std::string tool_name;
    std::string arguments;
    std::string result;
    bool ok = true;
};

}  // namespace ainiux::agent
