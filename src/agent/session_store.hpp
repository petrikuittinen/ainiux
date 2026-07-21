#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common.hpp"

struct sqlite3;

namespace ainiux::agent {

// Project-local agent UI/session database at .ainiux/agent.sqlite (never
// ~/.ainiux/ainiux.db). Portable with the workspace tree.

inline constexpr int kAgentSessionSchemaVersion = 1;

struct AgentSessionRecord {
    long long id = 0;
    long long created_at = 0;
    long long updated_at = 0;
    long long finished_at = 0;
    std::string status;  // running | success | error | cancelled | aborted
    std::string goal;
    std::string provider;
    std::string model;
    std::string api;
    std::string protocol;  // native | xml
    std::string workspace;
    std::string final_text;
    std::string error_code;
    std::string error_message;
    long long turns = 0;
    long long tool_calls = 0;
    std::string run_id;  // optional diagnostic log run id
};

struct AgentMessageRecord {
    long long id = 0;
    long long session_id = 0;
    long long seq = 0;
    long long created_at = 0;
    std::string role;  // system | user | assistant | tool | notice
    std::string content;
    std::string tool_call_id;
};

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

class AgentSessionStore {
   public:
    AgentSessionStore() = default;
    ~AgentSessionStore();
    AgentSessionStore(const AgentSessionStore&) = delete;
    AgentSessionStore& operator=(const AgentSessionStore&) = delete;
    AgentSessionStore(AgentSessionStore&& other) noexcept;
    AgentSessionStore& operator=(AgentSessionStore&& other) noexcept;

    // Creates .ainiux/ if needed and opens/creates agent.sqlite under workspace.
    Error open(const std::string& workspace);
    void close();
    bool is_open() const { return db_ != nullptr; }
    const std::string& path() const { return path_; }
    const std::string& workspace() const { return workspace_; }

    static std::string database_path(const std::string& workspace);

    // Creates a running session; fills record.id / timestamps on success.
    Error create_session(AgentSessionRecord& record);

    Error append_message(long long session_id,
                         const std::string& role,
                         const std::string& content,
                         const std::string& tool_call_id = {});

    Error append_tool_event(long long session_id,
                            long long turn,
                            const std::string& call_id,
                            const std::string& tool_name,
                            const std::string& arguments,
                            const std::string& result,
                            bool ok);

    // status: success | error | cancelled | aborted
    Error finish_session(long long session_id,
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
                       std::vector<AgentToolEventRecord>& tool_events) const;

   private:
    Error ensure_schema();
    Error next_seq(long long session_id, long long& seq);
    Error touch_session(long long session_id);

    sqlite3* db_ = nullptr;
    std::string path_;
    std::string workspace_;
};

}  // namespace ainiux::agent
