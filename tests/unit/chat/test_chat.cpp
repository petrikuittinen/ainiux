#include "chat/test_chat.hpp"
#include "support/test_support.hpp"
#include "chat/session.hpp"
#include "chat/sqlite_store.hpp"
#include "json/json.hpp"
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace pkchat::test::chat {

namespace {

using pkchat::test::check;
using pkchat::test::read_fixture;

void test_chat_session_json_round_trip() {
    pkchat::provider::RequestContext context;
    context.profile.name = "custom_openai_chat";
    context.base_url = "http://localhost:8000/v1";
    context.options.model = "mock-model";
    context.options.stream = false;
    context.options.context_tokens = 65536;
    pkchat::chat::Session session = pkchat::chat::new_session(context);
    check(session.settings_json.find("\"context_tokens\":65536") != std::string::npos,
          "new chat settings preserve the configured context-window size");
    session.created_at = "2026-06-14T00:00:00Z";
    session.updated_at = session.created_at;
    session.messages.push_back({"user", "hello"});
    session.messages.push_back({"assistant", "Hello"});
    session.compaction_events.push_back({"2026-06-14T00:01:00Z", "truncate-oldest", 2, 1000, 500,
                                         "Context compacted for test"});

    const std::string encoded = pkchat::chat::session_to_json(session);
    pkchat::json::ParseResult parsed = pkchat::json::parse(encoded);
    check(parsed.error.ok(), "chat session JSON parses");
    const pkchat::json::Value* messages = parsed.value.get("messages");
    check(messages != nullptr && messages->is_array() && messages->array.size() == 2, "chat messages persisted");

    const std::string path = "build/unit-chat.json";
    pkchat::Error err = pkchat::chat::save_session_atomic(path, session);
    check(err.ok(), "chat session saves atomically");
    pkchat::chat::Session loaded;
    err = pkchat::chat::load_session(path, loaded);
    check(err.ok(), "chat session loads");
    check(loaded.messages.size() == 2, "loaded chat has messages");
    check(!loaded.messages.empty() && loaded.messages[0].content == "hello", "loaded user message preserved");
    check(loaded.compaction_events.size() == 1 && loaded.compaction_events[0].messages_compacted == 2,
          "loaded chat preserves compaction events");
}

void test_chat_session_rejects_corrupt_json() {
    const std::string path = "build/corrupt-chat.json";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "{bad json";
    out.close();
    pkchat::chat::Session session;
    pkchat::Error err = pkchat::chat::load_session(path, session);
    check(!err.ok(), "corrupt chat file rejected");
    check(err.code == pkchat::ErrorCode::JsonParse, "corrupt chat file reports JSON parse error");
}

void test_chat_sqlite_store_round_trip_and_listing() {
    const std::string path = "build/unit-pkchat.db";
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");

    pkchat::chat::SqliteStore store;
    pkchat::Error err = store.open(path);
    check(err.ok(), "SQLite chat store opens");
    check(store.path() == path, "SQLite chat store records database path");

    pkchat::provider::RequestContext context;
    context.profile.name = "lm_studio";
    context.base_url = "http://localhost:1234/v1";
    context.options.model = "local-model";
    pkchat::chat::Session session = pkchat::chat::new_session(context);
    session.messages.push_back({"system", "Be concise"});
    session.messages.push_back({"user", "first prompt", {{"image/png", "base64-image"}}});
    session.messages.push_back({"assistant", "first answer"});
    session.usage_json = "{\"prompt_tokens\":2,\"completion_tokens\":3,\"total_tokens\":5}";
    session.compaction_events.push_back({"2026-06-28T00:00:00Z", "truncate-oldest", 2, 1000, 500,
                                         "Context compacted for test"});

    err = store.save_session(session);
    check(err.ok(), "SQLite chat session saves");
    check(session.thread_id > 0, "SQLite save assigns a thread id");
    check(session.name == "first prompt", "SQLite save derives a thread name from the first user message");

    long long last_id = 0;
    bool found = false;
    err = store.last_thread_id(last_id, found);
    check(err.ok() && found && last_id == session.thread_id, "SQLite store records last active thread");

    pkchat::chat::Session loaded;
    err = store.load_session(session.thread_id, loaded);
    check(err.ok(), "SQLite chat session loads");
    check(loaded.thread_id == session.thread_id && loaded.name == session.name,
          "SQLite load preserves thread identity and name");
    check(loaded.provider == "lm_studio" && loaded.base_url == "http://localhost:1234/v1" &&
              loaded.model == "local-model",
          "SQLite load preserves provider, base URL, and model metadata");
    check(loaded.messages.size() == 3 && loaded.messages[1].content == "first prompt" &&
              loaded.messages[1].images.size() == 1 &&
              loaded.messages[1].images[0].base64_data == "base64-image",
          "SQLite load preserves messages and image attachments");
    check(loaded.usage_json.find("prompt_tokens") != std::string::npos,
          "SQLite load preserves thread usage JSON");
    check(loaded.compaction_events.size() == 1 &&
              loaded.compaction_events[0].messages_compacted == 2,
          "SQLite load preserves compaction events");

    pkchat::chat::Session second = pkchat::chat::new_session(context);
    second.messages.push_back({"user", "newest prompt"});
    second.messages.push_back({"assistant", "newest answer"});
    err = store.save_session(second);
    check(err.ok(), "SQLite second chat session saves");

    std::vector<pkchat::chat::ThreadSummary> threads;
    err = store.list_threads(threads, 20);
    check(err.ok(), "SQLite thread list query succeeds");
    check(threads.size() == 2, "SQLite thread list returns saved threads");
    check(!threads.empty() && threads[0].id == second.thread_id,
          "SQLite thread list is newest thread first");
    check(!threads.empty() && threads[0].last_provider == "lm_studio" &&
              threads[0].last_model == "local-model" && threads[0].message_count == 2,
          "SQLite thread summary includes provider, model, and message count");

    err = store.soft_delete_thread(second.thread_id);
    check(err.ok(), "SQLite soft delete succeeds");
    threads.clear();
    err = store.list_threads(threads, 20);
    check(err.ok() && threads.size() == 1 && threads[0].id == session.thread_id,
          "SQLite thread list hides soft-deleted threads");
}

void test_chat_sqlite_missing_thread_and_corrupt_database() {
    const std::string path = "build/unit-corrupt-pkchat.db";
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "not-a-sqlite-database";
    }
    pkchat::chat::SqliteStore corrupt_store;
    pkchat::Error err = corrupt_store.open(path);
    check(!err.ok(), "corrupt SQLite database open fails");

    std::filesystem::remove(path);
    pkchat::chat::SqliteStore store;
    err = store.open(path);
    check(err.ok(), "fresh SQLite database opens for missing-thread test");

    pkchat::chat::Session missing;
    err = store.load_session(424242, missing);
    check(!err.ok() && err.code == pkchat::ErrorCode::FileRead &&
              err.message.find("SQLite chat thread not found: 424242") != std::string::npos,
          "missing SQLite thread reports a file-read error with thread id");

    long long last_id = 0;
    bool found = false;
    err = store.set_last_thread_id(424242);
    check(err.ok(), "SQLite last_thread_id can be set for recovery testing");
    err = store.last_thread_id(last_id, found);
    check(err.ok() && found && last_id == 424242, "SQLite last_thread_id round-trips");
    err = store.load_session(last_id, missing);
    check(!err.ok() && err.message.find("SQLite chat thread not found") != std::string::npos,
          "stale last_thread_id target remains unloadable until caller recovers");
}

void test_chat_session_file_failures_and_unicode() {
    pkchat::chat::Session session;
    pkchat::Error err = pkchat::chat::load_session("build/missing-chat-session.json", session);
    check(!err.ok() && err.code == pkchat::ErrorCode::FileRead,
          "missing chat file reports a file-read error");

    pkchat::provider::RequestContext context;
    context.profile.name = "custom_openai_chat";
    context.base_url = "http://localhost:8000/v1";
    context.options.model = "mock-model";
    session = pkchat::chat::new_session(context);
    session.created_at = "2026-06-28T00:00:00Z";
    session.updated_at = session.created_at;
    session.messages.push_back({"user", ""});
    session.messages.push_back({"assistant", u8"مرحبا 你好 👨‍👩‍👧‍👦"});

    const std::string path = "build/unit-empty-and-unicode-chat.json";
    err = pkchat::chat::save_session_atomic(path, session);
    check(err.ok(), "chat session with empty and Unicode messages saves");
    pkchat::chat::Session loaded;
    err = pkchat::chat::load_session(path, loaded);
    check(err.ok() && loaded.messages.size() == 2 &&
              loaded.messages[0].content.empty() &&
              loaded.messages[1].content == u8"مرحبا 你好 👨‍👩‍👧‍👦",
          "chat session round-trip preserves empty and Unicode message content");

    err = pkchat::chat::save_session_atomic("build/no-such-dir/chat.json", session);
    check(!err.ok() && err.code == pkchat::ErrorCode::FileWrite,
          "chat save to a missing parent directory reports a file-write error");
}

}  // namespace

void run_all() {
    test_chat_session_json_round_trip();
    test_chat_session_rejects_corrupt_json();
    test_chat_session_file_failures_and_unicode();
    test_chat_sqlite_store_round_trip_and_listing();
    test_chat_sqlite_missing_thread_and_corrupt_database();
}

}  // namespace pkchat::test::chat
