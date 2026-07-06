#pragma once

#include <string>
#include <vector>

#include "common.hpp"
#include "context/context.hpp"
#include "provider/provider.hpp"

namespace pkchat::chat {

struct Session {
    long long thread_id = 0;
    int schema_version = 1;
    std::string name;
    std::string created_at;
    std::string updated_at;
    std::string provider;
    std::string base_url;
    std::string model;
    std::string settings_json = "{}";
    std::string usage_json = "{}";
    std::vector<provider::Message> messages;
    std::vector<context::CompactionEvent> compaction_events;
};

std::string current_timestamp_utc();
bool session_has_chat_messages(const Session& session);
Session new_session(const provider::RequestContext& context);
Error load_session(const std::string& path, Session& session);
Error save_session_atomic(const std::string& path, Session session);
std::string session_to_json(const Session& session);

}  // namespace pkchat::chat
