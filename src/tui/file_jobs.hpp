#pragma once

#include <functional>
#include <string>

#include "chat/session.hpp"
#include "chat/sqlite_store.hpp"
#include "provider/provider.hpp"
#include "runtime/runtime.hpp"
#include "tui/events.hpp"

namespace ainiux::tui {

struct TuiFileJobs {
    runtime::JobHandle& file_job;
    runtime::EventQueue<TuiEvent>& events;
    provider::RequestContext& context;
    chat::Session& session;
    chat::SqliteStore& sqlite_store;
    std::string& sqlite_path;
    bool& sqlite_available;
    std::function<std::string()> sqlite_unavailable_message;
    std::string& status;

    bool busy(bool quiet = false) const;
    void start_save(const std::string& path, chat::Session snapshot, bool quiet_success = false);
    void start_load(const std::string& path);
    void start_store_load(long long thread_id);
    void start_store_save(chat::Session snapshot);
    void start_media_cleanup(int expiration_days,
                             long long protected_thread_id,
                             bool automatic);
    void start_insert(const std::string& source);
    void start_attach(const std::string& path);
    void start_fetch(const std::string& url);
    void start_search(const std::string& query);
};

}  // namespace ainiux::tui
