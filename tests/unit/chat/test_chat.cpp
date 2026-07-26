#include "chat/test_chat.hpp"
#include "support/test_support.hpp"
#include "app/app.hpp"
#include "chat/session.hpp"
#include "chat/generation_settings.hpp"
#include "chat/media_store.hpp"
#include "chat/settings.hpp"
#include "chat/sqlite_store.hpp"
#include "ainiux/model_setting.hpp"
#include "json/json.hpp"
#include "provider/model_selection.hpp"
#include <filesystem>
#include <fstream>
#include <sqlite3.h>
#include <sstream>
#include <string>
#include <vector>

namespace ainiux::test::chat {

namespace {

using ainiux::test::check;
using ainiux::test::read_fixture;

void test_chat_session_json_round_trip() {
    ainiux::provider::RequestContext context;
    context.profile.name = "custom_openai_chat";
    context.base_url = "http://localhost:8000/v1";
    context.options.model = "mock-model";
    context.options.stream = false;
    context.options.context_tokens = 65536;
    ainiux::chat::Session session = ainiux::chat::new_session(context);
    check(session.settings_json.find("\"context_tokens\":null") != std::string::npos,
          "configured context window is not stored as a thread override");
    context.options.has_context_tokens = true;
    context.options.reasoning = ainiux::ReasoningSelection::named("high");
    context.options.reasoning_explicit = true;
    session = ainiux::chat::new_session(context);
    check(session.settings_json.find("\"context_tokens\":65536") != std::string::npos,
          "explicit context window override is persisted in settings_json");
    session.created_at = "2026-06-14T00:00:00Z";
    session.updated_at = session.created_at;
    session.messages.push_back({"user", "hello"});
    session.messages.push_back({"assistant", "Hello"});
    session.read_only = true;
    session.read_only_reason = "managed attachment expired";
    session.compaction_events.push_back({"2026-06-14T00:01:00Z", "truncate-oldest", 2, 1000, 500,
                                         "Context compacted for test"});

    const std::string encoded = ainiux::chat::session_to_json(session);
    ainiux::json::ParseResult parsed = ainiux::json::parse(encoded);
    check(parsed.error.ok(), "chat session JSON parses");
    const ainiux::json::Value* messages = parsed.value.get("messages");
    check(messages != nullptr && messages->is_array() && messages->array.size() == 2, "chat messages persisted");

    const std::string path = "build/unit-chat.json";
    ainiux::Error err = ainiux::chat::save_session_atomic(path, session);
    check(err.ok(), "chat session saves atomically");
    ainiux::chat::Session loaded;
    err = ainiux::chat::load_session(path, loaded);
    check(err.ok(), "chat session loads");
    check(loaded.messages.size() == 2, "loaded chat has messages");
    check(!loaded.messages.empty() && loaded.messages[0].content == "hello", "loaded user message preserved");
    check(loaded.compaction_events.size() == 1 && loaded.compaction_events[0].messages_compacted == 2,
          "loaded chat preserves compaction events");
    check(loaded.read_only && loaded.read_only_reason == "managed attachment expired",
          "JSON export preserves a thread's read-only attachment lock");
    ainiux::cli::Options loaded_settings;
    err = ainiux::chat::apply_settings_json(loaded_settings, loaded.settings_json);
    check(err.ok() &&
              loaded_settings.reasoning == ainiux::ReasoningSelection::named("high"),
          "JSON export restores the thread's complete reasoning selection");

    ainiux::provider::ChatResult blocked_result;
    std::ostringstream blocked_output;
    const size_t original_message_count = loaded.messages.size();
    err = ainiux::app::send_session_turn(context, loaded, "continue", blocked_output,
                                         blocked_result);
    check(!err.ok() && err.code == ainiux::ErrorCode::FileLock &&
              loaded.messages.size() == original_message_count && blocked_output.str().empty(),
          "core chat requests reject read-only exported threads before mutation or transport");
}

void test_chat_session_rejects_corrupt_json() {
    const std::string path = "build/corrupt-chat.json";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "{bad json";
    out.close();
    ainiux::chat::Session session;
    ainiux::Error err = ainiux::chat::load_session(path, session);
    check(!err.ok(), "corrupt chat file rejected");
    check(err.code == ainiux::ErrorCode::JsonParse, "corrupt chat file reports JSON parse error");
}

void test_chat_sqlite_store_round_trip_and_listing() {
    const std::string path = "build/unit-ainiux.db";
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");

    ainiux::chat::SqliteStore store;
    ainiux::Error err = store.open(path);
    check(err.ok(), "SQLite chat store opens");
    check(store.path() == path, "SQLite chat store records database path");

    ainiux::provider::RequestContext context;
    context.profile.name = "lm_studio";
    context.base_url = "http://localhost:1234/v1";
    context.options.model = "local-model";
    context.options.reasoning = ainiux::ReasoningSelection::named("high");
    context.options.reasoning_explicit = true;
    ainiux::chat::Session session = ainiux::chat::new_session(context);
    session.messages.push_back({"system", "Be concise"});
    session.messages.push_back({"user", "first prompt", {{"image/png", "aW1hZ2UtYnl0ZXM="}}});
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

    ainiux::chat::Session loaded;
    err = store.load_session(session.thread_id, loaded);
    check(err.ok(), "SQLite chat session loads");
    check(loaded.thread_id == session.thread_id && loaded.name == session.name,
          "SQLite load preserves thread identity and name");
    check(loaded.provider == "lm_studio" && loaded.base_url == "http://localhost:1234/v1" &&
              loaded.model == "local-model",
          "SQLite load preserves provider, base URL, and model metadata");
    check(loaded.messages.size() == 3 && loaded.messages[1].content == "first prompt" &&
              loaded.messages[1].images.size() == 1 &&
              loaded.messages[1].images[0].base64_data.empty() &&
              loaded.messages[1].images[0].storage_ref.size() == 64,
          "SQLite load preserves messages and image attachments");
    check(loaded.usage_json.find("prompt_tokens") != std::string::npos,
          "SQLite load preserves thread usage JSON");
    check(loaded.compaction_events.size() == 1 &&
              loaded.compaction_events[0].messages_compacted == 2,
          "SQLite load preserves compaction events");
    ainiux::cli::Options loaded_settings;
    err = ainiux::chat::apply_settings_json(loaded_settings, loaded.settings_json);
    check(err.ok() &&
              loaded_settings.reasoning == ainiux::ReasoningSelection::named("high"),
          "SQLite load restores a named per-thread reasoning selection");

    session.messages.pop_back();
    session.usage_json = "{\"prompt_tokens\":2,\"completion_tokens\":3,\"total_tokens\":5}";
    err = store.save_session(session);
    check(err.ok(), "SQLite chat session saves after assistant response is popped");
    loaded = {};
    err = store.load_session(session.thread_id, loaded);
    check(err.ok() && loaded.messages.size() == 2 && loaded.messages.back().role == "user",
          "SQLite save after assistant pop preserves remaining messages");

    context.options.reasoning = ainiux::ReasoningSelection::token_budget(3072);
    ainiux::chat::Session second = ainiux::chat::new_session(context);
    second.messages.push_back({"user", "newest prompt"});
    second.messages.push_back({"assistant", "newest answer"});
    err = store.save_session(second);
    check(err.ok(), "SQLite second chat session saves");
    ainiux::chat::Session loaded_second;
    err = store.load_session(second.thread_id, loaded_second);
    loaded_settings = {};
    if (err.ok()) {
        err = ainiux::chat::apply_settings_json(
            loaded_settings, loaded_second.settings_json);
    }
    check(err.ok() &&
              loaded_settings.reasoning ==
                  ainiux::ReasoningSelection::token_budget(3072),
          "SQLite keeps reasoning selections independent across threads");

    std::vector<ainiux::chat::ThreadSummary> threads;
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

void test_chat_managed_media_cleanup_and_read_only_threads() {
    const std::string directory = "build/unit-managed-media";
    const std::string path = directory + "/ainiux.db";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);

    ainiux::chat::SqliteStore store;
    ainiux::Error err = store.open(path);
    check(err.ok(), "managed-media SQLite store opens");

    ainiux::provider::ImageInput image;
    err = store.import_media("abc", "image/png", "photo.png", "/tmp/photo.png", image);
    check(err.ok(), "managed media imports raw bytes");
    check(image.storage_ref ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" &&
              image.base64_data.empty() && image.byte_size == 3,
          "managed media uses the standard SHA-256 digest without retaining base64");
    const std::string object_path = ainiux::chat::media_root_for_database(path) +
                                    "/sha256/ba/" + image.storage_ref;
    check(std::filesystem::is_regular_file(object_path),
          "managed media is stored as a private external object");

    ainiux::provider::RequestContext context;
    context.profile.name = "lm_studio";
    context.base_url = "http://localhost:1234/v1";
    context.options.model = "local-model";
    ainiux::chat::Session session = ainiux::chat::new_session(context);
    session.messages.push_back({"user", "describe", {image}});
    session.messages.push_back({"assistant", "description"});
    err = store.save_session(session);
    check(err.ok() && session.thread_id > 0, "thread with managed media saves");

    sqlite3* raw_db = nullptr;
    int rc = sqlite3_open(path.c_str(), &raw_db);
    check(rc == SQLITE_OK && raw_db != nullptr, "managed-media database opens for assertions");
    if (raw_db != nullptr) {
        sqlite3_stmt* statement = nullptr;
        rc = sqlite3_prepare_v2(
            raw_db,
            "SELECT storage_ref, object_sha256 FROM attachments WHERE thread_id = ?1;",
            -1, &statement, nullptr);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int64(statement, 1, session.thread_id);
            rc = sqlite3_step(statement);
            check(rc == SQLITE_ROW && sqlite3_column_bytes(statement, 0) == 0 &&
                      std::string(reinterpret_cast<const char*>(sqlite3_column_text(statement, 1))) ==
                          image.storage_ref,
                  "SQLite stores only a media-object reference, not base64 payload data");
        } else {
            check(false, "managed-media attachment assertion query prepares");
        }
        sqlite3_finalize(statement);
        sqlite3_close(raw_db);
    }

    ainiux::chat::Session loaded;
    err = store.load_session(session.thread_id, loaded);
    check(err.ok() && !loaded.read_only && loaded.messages.size() == 2 &&
              loaded.messages[0].images.size() == 1 &&
              loaded.messages[0].images[0].base64_data.empty() &&
              loaded.messages[0].images[0].storage_ref == image.storage_ref,
          "SQLite load restores lightweight managed-media references");
    err = ainiux::chat::hydrate_message_images(path, loaded.messages, 1024);
    check(err.ok() && loaded.messages[0].images[0].base64_data == "YWJj",
          "request hydration reads, verifies, and base64-encodes managed media");

    raw_db = nullptr;
    rc = sqlite3_open(path.c_str(), &raw_db);
    check(rc == SQLITE_OK && raw_db != nullptr, "managed-media database reopens for aging");
    if (raw_db != nullptr) {
        char* message = nullptr;
        rc = sqlite3_exec(raw_db,
                          "UPDATE media_objects SET last_used_at = '2020-01-01T00:00:00Z';",
                          nullptr, nullptr, &message);
        check(rc == SQLITE_OK,
              message == nullptr ? "managed media can be aged for cleanup testing"
                                 : message);
        sqlite3_free(message);
        sqlite3_close(raw_db);
    }

    ainiux::chat::MediaCleanupResult cleanup;
    err = store.cleanup_media(7, session.thread_id, "test expiration", cleanup);
    check(err.ok() && cleanup.objects_expired == 0 && std::filesystem::exists(object_path),
          "manual cleanup protects the currently open thread");

    err = store.cleanup_media(7, 0, "test expiration", cleanup);
    check(err.ok() && cleanup.objects_expired == 1 && cleanup.files_removed == 1 &&
              cleanup.bytes_reclaimed == 3 && cleanup.threads_locked == 1 &&
              !std::filesystem::exists(object_path),
          "cleanup expires inactive media, reclaims bytes, and locks affected threads");

    loaded = {};
    err = store.load_session(session.thread_id, loaded);
    check(err.ok() && loaded.read_only && loaded.read_only_reason == "test expiration" &&
              loaded.messages.size() == 2,
          "expired-media threads remain readable with an explicit read-only reason");
    err = store.save_session(loaded);
    check(!err.ok() && err.code == ainiux::ErrorCode::FileWrite &&
              err.message.find("read-only") != std::string::npos,
          "SQLite rejects mutations to a read-only thread");
    std::vector<ainiux::chat::ThreadSummary> summaries;
    err = store.list_threads(summaries, 20);
    check(err.ok() && summaries.size() == 1 && summaries[0].read_only,
          "thread listings expose the read-only marker");

    ainiux::provider::ImageInput missing_image;
    err = store.import_media("different", "image/png", "missing.png", "/tmp/missing.png",
                             missing_image);
    check(err.ok(), "second managed object imports for missing-file test");
    ainiux::chat::Session missing = ainiux::chat::new_session(context);
    missing.messages.push_back({"user", "inspect", {missing_image}});
    missing.messages.push_back({"assistant", "seen"});
    err = store.save_session(missing);
    check(err.ok(), "thread for missing-file detection saves");
    const std::string missing_path = ainiux::chat::media_root_for_database(path) +
                                     "/sha256/" + missing_image.storage_ref.substr(0, 2) +
                                     "/" + missing_image.storage_ref;
    std::filesystem::remove(missing_path);
    ainiux::chat::Session missing_loaded;
    err = store.load_session(missing.thread_id, missing_loaded);
    check(err.ok() && missing_loaded.read_only &&
              missing_loaded.read_only_reason.find("missing") != std::string::npos,
          "a manually missing media file locks its thread without hiding the transcript");
}

void test_chat_markdown_attachment_storage_tiers() {
    const std::string directory = "build/unit-markdown-attachments";
    const std::string path = directory + "/ainiux.db";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);

    ainiux::chat::SqliteStore store;
    ainiux::Error err = store.open(path);
    check(err.ok(), "Markdown attachment SQLite store opens");

    ainiux::provider::RequestContext context;
    context.profile.name = "lm_studio";
    context.base_url = "http://localhost:1234/v1";
    context.options.model = "local-model";

    ainiux::provider::TextAttachment small;
    err = store.import_text_attachment("# Small\n\ninline", 64, "small.md",
                                       "/tmp/small.md", small);
    check(err.ok() && small.storage_ref.empty() &&
              small.markdown_content == "# Small\n\ninline",
          "small canonical Markdown remains inline");
    ainiux::chat::Session inline_session = ainiux::chat::new_session(context);
    inline_session.messages.push_back({"user", "Use the small file", {}, {small}});
    inline_session.messages.push_back({"assistant", "ok"});
    err = store.save_session(inline_session);
    check(err.ok(), "thread with inline Markdown saves");

    const std::string large_markdown = "# Large\n\n" + std::string(96, 'x');
    ainiux::provider::TextAttachment large;
    err = store.import_text_attachment(large_markdown, 32, "large.html",
                                       "/tmp/large.html", large);
    check(err.ok() && large.markdown_content.empty() && large.storage_ref.size() == 64,
          "large canonical Markdown uses a managed reference");
    const std::string large_path = ainiux::chat::media_root_for_database(path) +
                                   "/sha256/" + large.storage_ref.substr(0, 2) + "/" +
                                   large.storage_ref + ".md";
    check(std::filesystem::is_regular_file(large_path),
          "large canonical Markdown is stored as a .md media object");
    ainiux::chat::Session managed_session = ainiux::chat::new_session(context);
    managed_session.messages.push_back({"user", "Use the large file", {}, {large}});
    managed_session.messages.push_back({"assistant", "ok"});
    err = store.save_session(managed_session);
    check(err.ok(), "thread with managed Markdown saves");

    ainiux::chat::Session loaded_inline;
    err = store.load_session(inline_session.thread_id, loaded_inline);
    check(err.ok() && loaded_inline.messages[0].text_attachments.size() == 1 &&
              loaded_inline.messages[0].text_attachments[0].markdown_content ==
                  small.markdown_content,
          "SQLite restores inline canonical Markdown");
    err = ainiux::chat::hydrate_message_text_attachments(path, loaded_inline.messages, 1024);
    check(err.ok() && loaded_inline.messages[0].content.find("# Small") != std::string::npos &&
              loaded_inline.messages[0].text_attachments.empty(),
          "request hydration expands inline Markdown once");

    ainiux::chat::Session loaded_managed;
    err = store.load_session(managed_session.thread_id, loaded_managed);
    check(err.ok() && loaded_managed.messages[0].text_attachments.size() == 1 &&
              loaded_managed.messages[0].text_attachments[0].storage_ref == large.storage_ref,
          "SQLite restores managed Markdown references");
    err = ainiux::chat::hydrate_message_text_attachments(path, loaded_managed.messages, 1024);
    check(err.ok() && loaded_managed.messages[0].content.find(large_markdown) != std::string::npos,
          "request hydration verifies and expands managed Markdown");

    sqlite3* raw_db = nullptr;
    int rc = sqlite3_open(path.c_str(), &raw_db);
    check(rc == SQLITE_OK && raw_db != nullptr, "Markdown database opens for aging");
    if (raw_db != nullptr) {
        char* message = nullptr;
        rc = sqlite3_exec(raw_db,
                          "UPDATE media_objects SET last_used_at = '2020-01-01T00:00:00Z';",
                          nullptr, nullptr, &message);
        check(rc == SQLITE_OK, message == nullptr ? "managed Markdown can be aged" : message);
        sqlite3_free(message);
        sqlite3_close(raw_db);
    }
    ainiux::chat::MediaCleanupResult cleanup;
    err = store.cleanup_media(7, 0, "Markdown expired", cleanup);
    check(err.ok() && cleanup.objects_expired == 1 && cleanup.threads_locked == 1 &&
              !std::filesystem::exists(large_path),
          "cleanup expires only file-backed Markdown and locks its thread");
    loaded_inline = {};
    err = store.load_session(inline_session.thread_id, loaded_inline);
    check(err.ok() && !loaded_inline.read_only,
          "inline Markdown never expires with managed media cleanup");
}

void test_chat_sqlite_v4_markdown_migration() {
    const std::string path = "build/unit-markdown-migration.db";
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
    {
        ainiux::chat::SqliteStore store;
        ainiux::Error err = store.open(path);
        check(err.ok(), "v4 migration fixture database opens");
    }
    sqlite3* raw_db = nullptr;
    int rc = sqlite3_open(path.c_str(), &raw_db);
    check(rc == SQLITE_OK && raw_db != nullptr, "v4 migration fixture reopens raw");
    if (raw_db != nullptr) {
        char* message = nullptr;
        rc = sqlite3_exec(raw_db, R"SQL(
ALTER TABLE attachments RENAME TO attachments_v4;
CREATE TABLE attachments (
    id INTEGER PRIMARY KEY,
    thread_id INTEGER NOT NULL REFERENCES threads(id) ON DELETE CASCADE,
    message_id INTEGER REFERENCES messages(id) ON DELETE CASCADE,
    ordinal INTEGER NOT NULL DEFAULT 0,
    kind TEXT NOT NULL,
    mime_type TEXT NOT NULL DEFAULT '',
    display_name TEXT NOT NULL DEFAULT '',
    metadata_json TEXT NOT NULL DEFAULT '{}',
    storage_ref TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL,
    object_sha256 TEXT REFERENCES media_objects(sha256),
    source_ref TEXT NOT NULL DEFAULT '',
    byte_size INTEGER NOT NULL DEFAULT 0
);
DROP TABLE attachments_v4;
DELETE FROM schema_migrations WHERE version = 4;
)SQL", nullptr, nullptr, &message);
        check(rc == SQLITE_OK,
              message == nullptr ? "v3 attachment schema fixture is created" : message);
        sqlite3_free(message);
        sqlite3_close(raw_db);
    }
    ainiux::chat::SqliteStore migrated;
    ainiux::Error err = migrated.open(path);
    check(err.ok(), "v3 database migrates to v4");
    raw_db = nullptr;
    rc = sqlite3_open(path.c_str(), &raw_db);
    check(rc == SQLITE_OK && raw_db != nullptr, "migrated database opens for assertion");
    if (raw_db != nullptr) {
        sqlite3_stmt* statement = nullptr;
        rc = sqlite3_prepare_v2(
            raw_db,
            "SELECT COUNT(*) FROM pragma_table_info('attachments') WHERE name = 'inline_content';",
            -1, &statement, nullptr);
        check(rc == SQLITE_OK && sqlite3_step(statement) == SQLITE_ROW &&
                  sqlite3_column_int(statement, 0) == 1,
              "v4 migration adds inline Markdown content exactly once");
        sqlite3_finalize(statement);
        sqlite3_close(raw_db);
    }
}

void test_chat_sqlite_missing_thread_and_corrupt_database() {
    const std::string path = "build/unit-corrupt-ainiux.db";
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "not-a-sqlite-database";
    }
    ainiux::chat::SqliteStore corrupt_store;
    ainiux::Error err = corrupt_store.open(path);
    check(!err.ok(), "corrupt SQLite database open fails");

    std::filesystem::remove(path);
    ainiux::chat::SqliteStore store;
    err = store.open(path);
    check(err.ok(), "fresh SQLite database opens for missing-thread test");

    ainiux::chat::Session missing;
    err = store.load_session(424242, missing);
    check(!err.ok() && err.code == ainiux::ErrorCode::FileRead &&
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
    ainiux::chat::Session session;
    ainiux::Error err = ainiux::chat::load_session("build/missing-chat-session.json", session);
    check(!err.ok() && err.code == ainiux::ErrorCode::FileRead,
          "missing chat file reports a file-read error");

    ainiux::provider::RequestContext context;
    context.profile.name = "custom_openai_chat";
    context.base_url = "http://localhost:8000/v1";
    context.options.model = "mock-model";
    session = ainiux::chat::new_session(context);
    session.created_at = "2026-06-28T00:00:00Z";
    session.updated_at = session.created_at;
    session.messages.push_back({"user", ""});
    session.messages.push_back({"assistant", u8"مرحبا 你好 👨‍👩‍👧‍👦"});

    const std::string path = "build/unit-empty-and-unicode-chat.json";
    err = ainiux::chat::save_session_atomic(path, session);
    check(err.ok(), "chat session with empty and Unicode messages saves");
    ainiux::chat::Session loaded;
    err = ainiux::chat::load_session(path, loaded);
    check(err.ok() && loaded.messages.size() == 2 &&
              loaded.messages[0].content.empty() &&
              loaded.messages[1].content == u8"مرحبا 你好 👨‍👩‍👧‍👦",
          "chat session round-trip preserves empty and Unicode message content");

    err = ainiux::chat::save_session_atomic("build/no-such-dir/chat.json", session);
    check(!err.ok() && err.code == ainiux::ErrorCode::FileWrite,
          "chat save to a missing parent directory reports a file-write error");
}

void test_chat_settings_helpers() {
    ainiux::cli::Options options;
    ainiux::Error err = ainiux::chat::apply_chat_setting(options, "temperature", "0.9");
    check(err.ok() && options.has_temperature && options.temperature == 0.9,
          "chat setting parser applies temperature");
    err = ainiux::chat::apply_chat_setting(options, "reasoning", "8192");
    check(err.ok() &&
              options.reasoning == ainiux::ReasoningSelection::token_budget(8192),
          "chat setting parser applies an exact reasoning token budget");
    err = ainiux::chat::apply_chat_setting(options, "reasoning", "ultra");
    check(err.ok() &&
              options.reasoning == ainiux::ReasoningSelection::named("ultra"),
          "chat setting parser accepts an uncatalogued ASCII reasoning value");
    err = ainiux::chat::apply_chat_setting(options, "reasoning", "auto");
    check(err.ok() && options.reasoning.is_auto(),
          "chat setting reasoning=auto clears the override");
    err = ainiux::chat::apply_chat_setting(options, "reasoning", "not valid");
    check(!err.ok() && err.code == ainiux::ErrorCode::BadArgs,
          "chat setting rejects an invalid reasoning token");
    err = ainiux::chat::apply_chat_setting(options, "auto-convert-html-to-md", "no");
    check(err.ok() && !options.auto_convert_html_to_markdown,
          "chat setting parser can disable HTML-to-Markdown insertion conversion");
    err = ainiux::chat::apply_chat_setting(
        options, "thinking_preview_max_chars", "321");
    check(err.ok() && options.has_agent_thinking_preview_max_chars &&
              options.agent_thinking_preview_max_chars == 321,
          "agent thinking preview setting accepts a project override");
    err = ainiux::chat::apply_chat_setting(
        options, "thinking_preview_max_chars", "1001");
    check(!err.ok(), "agent thinking preview setting rejects values above 1000");

    ainiux::cli::Options source;
    source.has_top_k = true;
    source.top_k = 40;
    source.has_chat_purpose = true;
    source.chat_purpose = "general";
    source.reasoning = ainiux::ReasoningSelection::named("high");
    source.reasoning_explicit = true;
    source.auto_convert_html_to_markdown = false;
    source.has_agent_thinking_preview_max_chars = true;
    source.agent_thinking_preview_max_chars = 0;
    const std::string encoded = ainiux::chat::settings_json_from_options(source);
    ainiux::cli::Options loaded;
    err = ainiux::chat::apply_settings_json(loaded, encoded);
    check(err.ok() && loaded.has_top_k && loaded.top_k == 40 && loaded.has_chat_purpose &&
              loaded.chat_purpose == "general" &&
              loaded.reasoning == ainiux::ReasoningSelection::named("high") &&
              !loaded.auto_convert_html_to_markdown &&
              loaded.has_agent_thinking_preview_max_chars &&
              loaded.agent_thinking_preview_max_chars == 0,
          "chat settings JSON round-trips a named reasoning selection");

    source = ainiux::cli::Options{};
    source.reasoning = ainiux::ReasoningSelection::token_budget(4096);
    source.reasoning_explicit = true;
    err = ainiux::chat::apply_settings_json(
        loaded, ainiux::chat::settings_json_from_options(source));
    check(err.ok() &&
              loaded.reasoning == ainiux::ReasoningSelection::token_budget(4096),
          "chat settings JSON round-trips an exact reasoning token budget");

    source = ainiux::cli::Options{};
    err = ainiux::chat::apply_settings_json(
        loaded, ainiux::chat::settings_json_from_options(source));
    check(err.ok() && loaded.reasoning.is_auto(),
          "chat settings JSON round-trips Auto as null");

    err = ainiux::chat::apply_settings_json(
        loaded, "{\"thinking_budget\":\"legacy-high\"}");
    check(err.ok() &&
              loaded.reasoning == ainiux::ReasoningSelection::named("legacy-high"),
          "legacy chat settings migrate into the canonical reasoning selection");

    err = ainiux::chat::apply_chat_setting(options, "temperature", "NULL");
    check(err.ok() && !options.has_temperature, "chat setting NULL clears temperature override");
    options.has_temperature = true;
    options.temperature = 0.9;
    options.reasoning = ainiux::ReasoningSelection::named("ultra");
    const std::string with_nulls = ainiux::chat::settings_json_from_options(options);
    check(with_nulls.find("\"top_k\":null") != std::string::npos,
          "chat settings JSON emits null for unset overrides");
    ainiux::cli::Options cleared;
    err = ainiux::chat::apply_settings_json(cleared, with_nulls);
    check(err.ok() && cleared.has_temperature && cleared.temperature == 0.9 &&
              !cleared.has_top_k &&
              cleared.reasoning == ainiux::ReasoningSelection::named("ultra"),
          "chat settings JSON restores explicit values and clears null overrides");

    const std::string panel = ainiux::chat::format_settings_panel(options);
    check(panel.find("temperature=0.9") != std::string::npos &&
              panel.find("reasoning=ultra") != std::string::npos &&
              panel.find("auto-convert-html-to-md=no") != std::string::npos &&
              panel.find("top_k=") != std::string::npos &&
              panel.find("top_k=40") == std::string::npos,
          "chat settings panel shows canonical reasoning and set values");
    const std::string warning_panel = ainiux::chat::format_settings_panel(
        options, "temperature may be rejected for this model");
    check(warning_panel.find("warning=temperature may be rejected") !=
              std::string::npos,
          "chat settings panel surfaces explicit temperature advisories");

    ainiux::cli::Options empty_panel_options;
    const std::string empty_panel = ainiux::chat::format_settings_panel(empty_panel_options);
    check(empty_panel.find("temperature=\n") != std::string::npos &&
              empty_panel.find("reasoning=auto") != std::string::npos &&
              empty_panel.find("purpose=\n") != std::string::npos &&
              empty_panel.find("default") == std::string::npos,
          "chat settings panel shows Auto while leaving other unset fields empty");

    ainiux::ModelCapability gpt54;
    gpt54.temperature = ainiux::TemperatureSupport::ReasoningNoneOnly;
    gpt54.reasoning_default = ainiux::ReasoningSelection::named("none");
    ainiux::ModelSetting preset;
    preset.purpose = "coding";
    preset.temperature = 0.6;
    preset.reasoning = ainiux::ReasoningSelection::named("high");
    options = ainiux::cli::Options{};
    err = ainiux::chat::apply_model_setting_preset(options, preset, &gpt54);
    check(err.ok() && !options.has_temperature &&
              options.reasoning == ainiux::ReasoningSelection::named("high"),
          "automatic presets omit temperature when the reasoning combination is unsupported");

    preset.reasoning = ainiux::ReasoningSelection::named("none");
    err = ainiux::chat::apply_model_setting_preset(options, preset, &gpt54);
    check(err.ok() && options.has_temperature && options.temperature == 0.6,
          "conditional GPT-5 temperature metadata permits presets at reasoning=none");

    gpt54.id = "dynamic-gpt";
    gpt54.provider = "openai";
    gpt54.api = "chat";
    gpt54.model_regex = "^dynamic-gpt$";
    preset.model_id = gpt54.id;
    options.provider = "openai";
    options.api = "chat";
    options.model = "dynamic-gpt";
    options.model_catalog.models.push_back(gpt54);
    options.model_catalog.presets.push_back(preset);
    err = ainiux::chat::apply_chat_setting(options, "reasoning", "high");
    check(err.ok() && !options.has_temperature &&
              options.temperature_preset_applied,
          "changing reasoning suppresses an automatic conditionally unsupported temperature");
    err = ainiux::chat::apply_chat_setting(options, "reasoning", "none");
    check(err.ok() && options.has_temperature && options.temperature == 0.6,
          "reasoning=none restores the catalog preset temperature exactly");

    options = ainiux::cli::Options{};
    options.has_temperature = true;
    options.temperature = 0.8;
    options.reasoning = ainiux::ReasoningSelection::named("high");
    options.reasoning_explicit = true;
    err = ainiux::chat::apply_model_setting_preset(options, preset, &gpt54);
    check(err.ok() && options.has_temperature && options.temperature == 0.8 &&
              options.reasoning == ainiux::ReasoningSelection::named("high"),
          "explicit temperature and reasoning overrides survive advisory presets");
}

void test_chat_session_has_chat_messages() {
    ainiux::chat::Session session;
    check(!ainiux::chat::session_has_chat_messages(session),
          "chat session helper treats an empty session as non-chat");
    session.messages.push_back({"system", "Be concise"});
    check(!ainiux::chat::session_has_chat_messages(session),
          "chat session helper ignores system-only sessions");
    session.messages.push_back({"user", "hello"});
    check(ainiux::chat::session_has_chat_messages(session),
          "chat session helper detects user messages");
}

void test_chat_sqlite_remove_empty_threads() {
    const std::string path = "build/unit-ainiux-empty-threads.db";
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");

    ainiux::chat::SqliteStore store;
    ainiux::Error err = store.open(path);
    check(err.ok(), "SQLite empty-thread cleanup store opens");

    ainiux::provider::RequestContext context;
    context.profile.name = "lm_studio";
    context.base_url = "http://localhost:1234/v1";
    context.options.model = "local-model";

    ainiux::chat::Session system_only = ainiux::chat::new_session(context);
    system_only.messages.push_back({"system", "Be concise"});
    err = store.save_session(system_only);
    check(err.ok(), "SQLite system-only thread saves");

    ainiux::chat::Session no_messages = ainiux::chat::new_session(context);
    err = store.save_session(no_messages);
    check(err.ok(), "SQLite messageless thread saves");

    ainiux::chat::Session with_user = ainiux::chat::new_session(context);
    with_user.messages.push_back({"user", "hello"});
    with_user.messages.push_back({"assistant", "hi"});
    err = store.save_session(with_user);
    check(err.ok(), "SQLite user thread saves");

    long long deleted_count = 0;
    bool current_removed = false;
    err = store.soft_delete_empty_threads(deleted_count, with_user.thread_id, current_removed);
    check(err.ok() && deleted_count == 2 && !current_removed,
          "SQLite empty-thread cleanup removes system-only and messageless threads");

    std::vector<ainiux::chat::ThreadSummary> threads;
    err = store.list_threads(threads, 20);
    check(err.ok() && threads.size() == 1 && threads[0].id == with_user.thread_id,
          "SQLite empty-thread cleanup keeps threads with user or assistant messages");

    current_removed = false;
    deleted_count = 0;
    err = store.soft_delete_empty_threads(deleted_count, system_only.thread_id, current_removed);
    check(err.ok() && deleted_count == 0 && !current_removed,
          "SQLite empty-thread cleanup is idempotent when nothing remains");

    ainiux::chat::Session another_empty = ainiux::chat::new_session(context);
    another_empty.messages.push_back({"system", "Another system prompt"});
    err = store.save_session(another_empty);
    check(err.ok(), "SQLite second system-only thread saves");

    deleted_count = 0;
    current_removed = false;
    err = store.soft_delete_empty_threads(deleted_count, another_empty.thread_id, current_removed);
    check(err.ok() && deleted_count == 1 && current_removed,
          "SQLite empty-thread cleanup reports when the watched thread is removed");
}

void test_chat_sqlite_thread_name_from_first_user_prompt() {
    const std::string path = "build/unit-thread-name-ainiux.db";
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");

    ainiux::provider::RequestContext context;
    context.profile.name = "lm_studio";
    context.base_url = "http://localhost:1234/v1";
    context.options.model = "local-model";

    ainiux::chat::SqliteStore store;
    ainiux::Error err = store.open(path);
    check(err.ok(), "SQLite thread-name test database opens");

    ainiux::chat::Session session = ainiux::chat::new_session(context);
    err = store.save_session(session);
    check(err.ok() && session.name == "New chat", "SQLite empty thread keeps the default name");
    const long long thread_id = session.thread_id;

    session.messages.push_back({"user", "What is the capital of Norway?"});
    err = store.save_session(session);
    check(err.ok() && session.name == "What is the capital of Norway?",
          "SQLite thread name updates from the first user prompt after an early autosave");

    const std::string long_prompt(48, 'x');
    ainiux::chat::Session long_session = ainiux::chat::new_session(context);
    long_session.name = "New chat";
    long_session.messages.push_back({"user", long_prompt});
    err = store.save_session(long_session);
    check(err.ok() && long_session.name.size() == 40 && long_session.name == long_prompt.substr(0, 40),
          "SQLite thread name truncates the first user prompt to 40 characters");

    ainiux::chat::Session named_session = ainiux::chat::new_session(context);
    named_session.name = "Custom title";
    named_session.messages.push_back({"user", "ignored for explicit names"});
    err = store.save_session(named_session);
    check(err.ok() && named_session.name == "Custom title",
          "SQLite thread name keeps an explicit user-provided title");

    {
        ainiux::chat::SqliteStore legacy_store;
        const std::string legacy_path = "build/unit-thread-name-legacy-ainiux.db";
        std::filesystem::remove(legacy_path);
        std::filesystem::remove(legacy_path + "-wal");
        std::filesystem::remove(legacy_path + "-shm");

        err = legacy_store.open(legacy_path);
        check(err.ok(), "SQLite legacy thread-name database opens");

        ainiux::chat::Session legacy = ainiux::chat::new_session(context);
        err = legacy_store.save_session(legacy);
        check(err.ok() && legacy.thread_id > 0, "SQLite legacy empty thread saves");
        const long long legacy_thread_id = legacy.thread_id;
        legacy_store.close();

        sqlite3* raw_db = nullptr;
        const int open_rc = sqlite3_open(legacy_path.c_str(), &raw_db);
        check(open_rc == SQLITE_OK && raw_db != nullptr, "SQLite legacy raw database opens for seeding");
        if (raw_db != nullptr) {
            char* err_msg = nullptr;
            const std::string insert_sql =
                "INSERT INTO messages(thread_id, ordinal, created_at, role, content, metadata_json) "
                "VALUES(" +
                std::to_string(legacy_thread_id) +
                ", 0, '2026-07-09T00:00:00Z', 'user', 'legacy prompt', '{}');"
                "UPDATE threads SET name = 'New chat', message_count = 1 WHERE id = " +
                std::to_string(legacy_thread_id) + ";"
                "DELETE FROM schema_migrations WHERE version >= 2;";
            const int exec_rc = sqlite3_exec(raw_db, insert_sql.c_str(), nullptr, nullptr, &err_msg);
            check(exec_rc == SQLITE_OK,
                  err_msg == nullptr ? "SQLite legacy seed statements run"
                                     : ("SQLite legacy seed statements run: " + std::string(err_msg)).c_str());
            sqlite3_free(err_msg);
            sqlite3_close(raw_db);
        }

        err = legacy_store.open(legacy_path);
        check(err.ok(), "SQLite legacy thread-name database reopens for migration");

        std::vector<ainiux::chat::ThreadSummary> threads;
        err = legacy_store.list_threads(threads, 20);
        check(err.ok() && threads.size() == 1 && threads[0].name == "legacy prompt",
              "SQLite migration backfills thread names from the first user prompt");
    }

    (void)thread_id;
}

void test_generation_settings_metadata() {
    check(ainiux::chat::generation::is_chat_setting_name(ainiux::chat::generation::kTopP),
          "generation metadata recognizes top_p");
    check(!ainiux::chat::generation::is_chat_setting_name("bogus_setting"),
          "generation metadata rejects unknown chat settings");
    check(ainiux::chat::generation::is_chat_purpose(ainiux::chat::generation::kPurposeCoding),
          "generation metadata recognizes coding purpose");
}

void test_editor_model_selection_app_state() {
    const std::string path = "build/unit-editor-selection-ainiux.db";
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");

    ainiux::chat::SqliteStore store;
    ainiux::Error err = store.open(path);
    check(err.ok(), "editor selection app-state database opens");

    ainiux::provider::ModelSelection selection{
        "openai",
        "gpt-5.4",
        "responses",
        ainiux::ReasoningSelection::token_budget(4096),
    };
    err = store.set_app_state(
        "editor_model_selection",
        ainiux::provider::serialize_model_selection(selection));
    check(err.ok(), "editor model selection is stored in generic app_state");

    std::string encoded;
    bool found = false;
    err = store.app_state("editor_model_selection", encoded, found);
    ainiux::provider::ModelSelection loaded;
    if (err.ok() && found) {
        err = ainiux::provider::parse_model_selection(encoded, loaded);
    }
    check(err.ok() && found && loaded.provider == "openai" &&
              loaded.model == "gpt-5.4" && loaded.api == "responses" &&
              loaded.reasoning == ainiux::ReasoningSelection::token_budget(4096),
          "editor provider, model, API, and reasoning round-trip through app_state");

    selection.reasoning = ainiux::ReasoningSelection::named("ultra");
    err = ainiux::provider::parse_model_selection(
        ainiux::provider::serialize_model_selection(selection), loaded);
    check(err.ok() &&
              loaded.reasoning == ainiux::ReasoningSelection::named("ultra"),
          "shared model-selection serialization preserves forward-compatible values");
}

}  // namespace

void run_all() {
    test_chat_session_json_round_trip();
    test_chat_session_rejects_corrupt_json();
    test_chat_session_file_failures_and_unicode();
    test_chat_session_has_chat_messages();
    test_chat_sqlite_store_round_trip_and_listing();
    test_chat_managed_media_cleanup_and_read_only_threads();
    test_chat_markdown_attachment_storage_tiers();
    test_chat_sqlite_v4_markdown_migration();
    test_chat_sqlite_thread_name_from_first_user_prompt();
    test_chat_sqlite_remove_empty_threads();
    test_chat_sqlite_missing_thread_and_corrupt_database();
    test_chat_settings_helpers();
    test_editor_model_selection_app_state();
    test_generation_settings_metadata();
}

}  // namespace ainiux::test::chat
