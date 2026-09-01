#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "agent/agent_controller.hpp"
#include "cli/args.hpp"
#include "common.hpp"
#include "runtime/runtime.hpp"
#include "server/event_broker.hpp"

namespace ainiux::server {

class InteractiveSession {
   public:
    ~InteractiveSession();
    InteractiveSession(const InteractiveSession&) = delete;
    InteractiveSession& operator=(const InteractiveSession&) = delete;

    const std::string& id() const { return id_; }
    EventBroker& events() { return events_; }
    const EventBroker& events() const { return events_; }
    std::string snapshot_json() const;

    Error start_turn(const std::string& body, std::string& turn_id);
    Error cancel_turn(const std::string& turn_id);
    Error resolve_approval(const std::string& approval_id,
                           const std::string& decision);
    Error review_file(const std::string& approval_id, std::string& body) const;
    void close();

   private:
    friend class SessionHub;
    InteractiveSession(std::string id,
                       std::string workspace,
                       provider::RequestContext context,
                       agent::PermissionMode permission_mode,
                       agent::AgentTaskMode task_mode,
                       std::size_t max_events);

    void start_preparation();
    void consume_event(const agent::AgentSurfaceEvent& event);
    void publish(const std::string& type,
                 const std::string& data_json = "{}",
                 const std::string& turn_id = {});
    std::string safe_error(const Error& error) const;

    const std::string id_;
    const std::string workspace_;
    const std::string created_at_;
    mutable std::mutex mutex_;
    provider::RequestContext context_;
    agent::PermissionMode permission_mode_;
    agent::AgentTaskMode task_mode_;
    std::string status_ = "preparing";
    std::string updated_at_;
    std::string active_turn_id_;
    std::string pending_approval_id_;
    std::string pending_review_path_;
    std::string pending_tool_name_;
    std::string pending_command_preview_;
    std::string pending_rule_id_;
    std::string pending_message_;
    std::size_t next_turn_ = 1;
    std::size_t next_approval_ = 1;
    bool closed_ = false;
    std::shared_ptr<agent::AgentController> controller_;
    runtime::JobHandle preparation_job_;
    runtime::JobHandle event_pump_;
    EventBroker events_;
};

struct SessionCreateResult {
    std::shared_ptr<InteractiveSession> session;
    Error error;
};

class SessionHub {
   public:
    SessionHub(cli::Options base_options,
               std::string workspace,
               std::size_t max_sessions);
    ~SessionHub();
    SessionHub(const SessionHub&) = delete;
    SessionHub& operator=(const SessionHub&) = delete;

    SessionCreateResult create(const std::string& body);
    std::shared_ptr<InteractiveSession> find(const std::string& id) const;
    std::string list_json() const;
    bool erase(const std::string& id);
    void shutdown();
    std::size_t size() const;

   private:
    cli::Options base_options_;
    std::string workspace_;
    std::size_t max_sessions_;
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<InteractiveSession>> sessions_;
    std::size_t next_session_id_ = 1;
    bool stopping_ = false;
};

}  // namespace ainiux::server
