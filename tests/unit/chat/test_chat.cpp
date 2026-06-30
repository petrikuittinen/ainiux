#include "chat/test_chat.hpp"
#include "support/test_support.hpp"
#include "chat/session.hpp"
#include "chat/settings.hpp"
#include "chat/sqlite_store.hpp"
#include "pkchat/model_setting.hpp"
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
    check(session.settings_json.find("\"context_tokens\":null") != std::string::npos,
          "configured context window is not stored as a thread override");
    context.options.has_context_tokens = true;
    session = pkchat::chat::new_session(context);
    check(session.settings_json.find("\"context_tokens\":65536") != std::string::npos,
          "explicit context window override is persisted in settings_json");
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

    session.messages.pop_back();
    session.usage_json = "{\"prompt_tokens\":2,\"completion_tokens\":3,\"total_tokens\":5}";
    err = store.save_session(session);
    check(err.ok(), "SQLite chat session saves after assistant response is popped");
    loaded = {};
    err = store.load_session(session.thread_id, loaded);
    check(err.ok() && loaded.messages.size() == 2 && loaded.messages.back().role == "user",
          "SQLite save after assistant pop preserves remaining messages");

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

void test_chat_settings_helpers() {
    check(pkchat::chat::model_pattern_matches("Qwen3.6-*", "Qwen3.6-35B-A3B"),
          "model pattern wildcard matches a concrete model");
    check(!pkchat::chat::model_pattern_matches("Qwen3.6-*", "Qwen3.5-4B"),
          "model pattern wildcard rejects a different family");

    const std::vector<pkchat::ModelSetting> presets = {
        {"Qwen3.6-*", "coding", "", 0.6, 20, 0.95, 0.0, 1.0, 0.0},
        {"Qwen3.6-35B-A3B", "coding", "", 0.4, 10, 0.8, 0.0, 1.0, 0.0},
    };
    const pkchat::ModelSetting* preset =
        pkchat::chat::find_model_setting("Qwen3.6-35B-A3B", "coding", presets);
    check(preset != nullptr && preset->temperature == 0.4,
          "model preset lookup prefers the longest matching pattern");

    pkchat::cli::Options options;
    pkchat::Error err = pkchat::chat::apply_chat_setting(options, "temperature", "0.9");
    check(err.ok() && options.has_temperature && options.temperature == 0.9,
          "chat setting parser applies temperature");
    err = pkchat::chat::apply_chat_setting(options, "thinking", "on");
    check(err.ok() && options.has_enable_thinking && options.enable_thinking,
          "chat setting parser applies thinking");
    err = pkchat::chat::apply_chat_setting(options, "thinking_budget", "8192");
    check(err.ok() && options.has_thinking_budget && options.thinking_budget == "8192" &&
              pkchat::chat::thinking_budget_is_token_count(options.thinking_budget),
          "chat setting parser applies numeric thinking_budget");
    err = pkchat::chat::apply_chat_setting(options, "thinking_budget", "high");
    check(err.ok() && options.thinking_budget == "high" &&
              !pkchat::chat::thinking_budget_is_token_count(options.thinking_budget),
          "chat setting parser applies verbal thinking_budget");

    pkchat::cli::Options source;
    source.has_top_k = true;
    source.top_k = 40;
    source.has_chat_purpose = true;
    source.chat_purpose = "general";
    source.has_thinking_budget = true;
    source.thinking_budget = "medium";
    const std::string encoded = pkchat::chat::settings_json_from_options(source);
    pkchat::cli::Options loaded;
    err = pkchat::chat::apply_settings_json(loaded, encoded);
    check(err.ok() && loaded.has_top_k && loaded.top_k == 40 && loaded.has_chat_purpose &&
              loaded.chat_purpose == "general" && loaded.has_thinking_budget &&
              loaded.thinking_budget == "medium",
          "chat settings JSON round-trips verbal thinking_budget");

    source = pkchat::cli::Options{};
    source.has_thinking_budget = true;
    source.thinking_budget = "4096";
    err = pkchat::chat::apply_settings_json(loaded, pkchat::chat::settings_json_from_options(source));
    check(err.ok() && loaded.thinking_budget == "4096" &&
              pkchat::chat::thinking_budget_is_token_count(loaded.thinking_budget),
          "chat settings JSON round-trips numeric thinking_budget");

    err = pkchat::chat::apply_chat_setting(options, "temperature", "NULL");
    check(err.ok() && !options.has_temperature, "chat setting NULL clears temperature override");
    options.has_temperature = true;
    options.temperature = 0.9;
    const std::string with_nulls = pkchat::chat::settings_json_from_options(options);
    check(with_nulls.find("\"top_k\":null") != std::string::npos,
          "chat settings JSON emits null for unset overrides");
    pkchat::cli::Options cleared;
    err = pkchat::chat::apply_settings_json(cleared, with_nulls);
    check(err.ok() && cleared.has_temperature && cleared.temperature == 0.9 && !cleared.has_top_k,
          "chat settings JSON null values clear overrides on load");
}

}  // namespace

void run_all() {
    test_chat_session_json_round_trip();
    test_chat_session_rejects_corrupt_json();
    test_chat_session_file_failures_and_unicode();
    test_chat_sqlite_store_round_trip_and_listing();
    test_chat_sqlite_missing_thread_and_corrupt_database();
    test_chat_settings_helpers();
}

}  // namespace pkchat::test::chat
