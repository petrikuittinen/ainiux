#include "io/test_io_faults.hpp"
#include "support/test_support.hpp"

#include "chat/session.hpp"
#include "editor/editor.hpp"
#include "provider/provider.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace pkchat::test::io {

namespace {

using pkchat::test::check;

void chmod_path(const std::string& path, std::filesystem::perms perms) {
    std::error_code error;
    std::filesystem::permissions(path, perms, error);
    check(!error, "test fixture chmod succeeded: " + path);
}

class PermissionGuard {
public:
    PermissionGuard(std::string path, std::filesystem::perms perms) : path_(std::move(path)), perms_(perms) {}

    ~PermissionGuard() {
        std::error_code error;
        std::filesystem::permissions(path_, perms_, error);
    }

private:
    std::string path_;
    std::filesystem::perms perms_;
};

pkchat::chat::Session make_session() {
    pkchat::provider::RequestContext context;
    context.profile.name = "custom_openai_chat";
    context.base_url = "http://localhost:8000/v1";
    context.options.model = "mock-model";
    pkchat::chat::Session session = pkchat::chat::new_session(context);
    session.created_at = "2026-06-28T00:00:00Z";
    session.updated_at = session.created_at;
    session.messages.push_back({"user", u8"مرحبا"});
    return session;
}

void test_readonly_chat_load_and_save() {
    const std::string dir = "build/mock-readonly-chat";
    const std::string path = dir + "/chat.json";
    const auto restore_dir_perms = std::filesystem::perms::owner_all | std::filesystem::perms::group_all |
                                   std::filesystem::perms::others_all;
    const auto restore_file_perms = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
    std::filesystem::create_directories(dir);
    PermissionGuard dir_guard(dir, restore_dir_perms);
    PermissionGuard file_guard(path, restore_file_perms);
    pkchat::chat::Session session = make_session();
    pkchat::Error err = pkchat::chat::save_session_atomic(path, session);
    check(err.ok(), "chat session saves before read-only fixture is applied");

    chmod_path(path, std::filesystem::perms::none);
    pkchat::chat::Session loaded;
    err = pkchat::chat::load_session(path, loaded);
    check(!err.ok() && err.code == pkchat::ErrorCode::FileRead &&
              err.message.find("could not open chat file for reading") != std::string::npos,
          "permission-denied chat file load reports a file-read error");

    chmod_path(path, restore_file_perms);
    chmod_path(dir, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec |
                          std::filesystem::perms::group_read | std::filesystem::perms::group_exec |
                          std::filesystem::perms::others_read | std::filesystem::perms::others_exec);
    err = pkchat::chat::save_session_atomic(path, session);
    check(!err.ok() && err.code == pkchat::ErrorCode::FileWrite,
          "read-only directory blocks chat session save");
}

void test_readonly_editor_load_and_save() {
    const std::string dir = "build/mock-readonly-editor";
    const std::string path = dir + "/notes.txt";
    const auto restore_dir_perms = std::filesystem::perms::owner_all | std::filesystem::perms::group_all |
                                   std::filesystem::perms::others_all;
    const auto restore_file_perms = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
    std::filesystem::create_directories(dir);
    PermissionGuard dir_guard(dir, restore_dir_perms);
    PermissionGuard file_guard(path, restore_file_perms);
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << u8"你好";
    }

    chmod_path(path, std::filesystem::perms::none);
    pkchat::editor::PieceTable table;
    pkchat::Error err = pkchat::editor::load_file(path, table);
    check(!err.ok() && err.code == pkchat::ErrorCode::FileRead &&
              err.message.find("could not open editor file for reading") != std::string::npos,
          "permission-denied editor file load reports a file-read error");

    chmod_path(path, restore_file_perms);
    table = pkchat::editor::PieceTable::from_string("save me");
    chmod_path(dir, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec |
                          std::filesystem::perms::group_read | std::filesystem::perms::group_exec |
                          std::filesystem::perms::others_read | std::filesystem::perms::others_exec);
    err = pkchat::editor::save_file(dir + "/new-save.txt", table);
    check(!err.ok() && err.code == pkchat::ErrorCode::FileWrite,
          "read-only directory blocks editor save of a new file");
}

void test_enospc_chat_save() {
    const char* mock_flag = std::getenv("PKCHAT_MOCK_ENOSPC");
    if (mock_flag == nullptr || std::string(mock_flag) != "1") {
        check(false, "ENOSPC mock tests require PKCHAT_MOCK_ENOSPC=1 and LD_PRELOAD posix_io_mock");
        return;
    }

    const std::string path = "build/mock-enospc/chat.json";
    std::filesystem::create_directories("build/mock-enospc");
    pkchat::chat::Session session = make_session();
    pkchat::Error err = pkchat::chat::save_session_atomic(path, session);
    check(!err.ok() && err.code == pkchat::ErrorCode::FileWrite &&
              (err.message.find("No space left on device") != std::string::npos ||
               err.message.find("could not open temporary chat file") != std::string::npos),
          "ENOSPC mock blocks chat session save");
}

void test_enospc_editor_save() {
    const char* mock_flag = std::getenv("PKCHAT_MOCK_ENOSPC");
    if (mock_flag == nullptr || std::string(mock_flag) != "1") {
        check(false, "ENOSPC mock tests require PKCHAT_MOCK_ENOSPC=1 and LD_PRELOAD posix_io_mock");
        return;
    }

    const std::string path = "build/mock-enospc/editor.txt";
    std::filesystem::create_directories("build/mock-enospc");
    pkchat::editor::PieceTable table = pkchat::editor::PieceTable::from_string("disk full");
    pkchat::Error err = pkchat::editor::save_file(path, table);
    const bool write_failed = !err.ok() && err.code == pkchat::ErrorCode::FileWrite;
    const bool mentions_open = err.message.find("could not open editor file for writing") != std::string::npos;
    const bool mentions_write = err.message.find("failed while writing editor buffer") != std::string::npos;
    const bool mentions_close = err.message.find("failed while closing editor file after writing") != std::string::npos;
    check(write_failed && (mentions_open || mentions_write || mentions_close),
          "ENOSPC mock blocks editor save");
}

}  // namespace

void run_readonly_all() {
    test_readonly_chat_load_and_save();
    test_readonly_editor_load_and_save();
}

void run_enospc_all() {
    test_enospc_chat_save();
    test_enospc_editor_save();
}

}  // namespace pkchat::test::io