#include "server/server.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#if !defined(_WIN32)
#include <sys/stat.h>
#else
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <vector>
#endif

#include "app/app.hpp"
#include "platform/environment.hpp"
#include "platform/filesystem.hpp"
#if defined(_WIN32)
#include "platform/windows_utf.hpp"
#endif
#include "runtime/interrupt.hpp"
#include "server/listener.hpp"

namespace ainiux::server {
namespace {

bool path_within(const std::filesystem::path& root, const std::filesystem::path& path) {
    auto root_it = root.begin();
    auto path_it = path.begin();
    for (; root_it != root.end(); ++root_it, ++path_it) {
        if (path_it == path.end() || *root_it != *path_it) return false;
    }
    return true;
}

#if defined(_WIN32)
Error verify_windows_private_acl(const std::filesystem::path& path, const char* option) {
    std::wstring native;
    Error conversion = platform::utf8_to_utf16(path.u8string(), native);
    if (!conversion.ok()) return conversion;
    PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
    PSID owner = nullptr;
    PACL acl = nullptr;
    const DWORD security_error = GetNamedSecurityInfoW(
        native.data(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &acl, nullptr, &raw_descriptor);
    struct DescriptorGuard {
        PSECURITY_DESCRIPTOR value;
        ~DescriptorGuard() { if (value != nullptr) LocalFree(value); }
    } descriptor{raw_descriptor};
    if (security_error != ERROR_SUCCESS) {
        return {ErrorCode::Auth,
                std::string("could not inspect ") + option + " ACL: " +
                    platform::windows_error_message(security_error)};
    }
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    if (!GetSecurityDescriptorControl(raw_descriptor, &control, &revision) ||
        (control & SE_DACL_PROTECTED) == 0 || acl == nullptr) {
        return {ErrorCode::Auth,
                std::string(option) + " must have a protected, non-null Windows DACL"};
    }

    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
        return {ErrorCode::Auth, "could not inspect the current-user SID"};
    }
    struct HandleGuard {
        HANDLE value;
        ~HandleGuard() { if (value != nullptr) CloseHandle(value); }
    } token{raw_token};
    DWORD token_bytes = 0;
    (void)GetTokenInformation(raw_token, TokenUser, nullptr, 0, &token_bytes);
    if (token_bytes == 0) return {ErrorCode::Auth, "could not size the current-user SID"};
    std::vector<unsigned char> token_data(token_bytes);
    if (!GetTokenInformation(raw_token, TokenUser, token_data.data(), token_bytes, &token_bytes)) {
        return {ErrorCode::Auth, "could not read the current-user SID"};
    }
    PSID user_sid = reinterpret_cast<TOKEN_USER*>(token_data.data())->User.Sid;
    unsigned char system_storage[SECURITY_MAX_SID_SIZE]{};
    DWORD system_bytes = sizeof(system_storage);
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system_storage, &system_bytes)) {
        return {ErrorCode::Auth, "could not create the Windows SYSTEM SID"};
    }
    PSID system_sid = system_storage;
    if (owner == nullptr || (!EqualSid(owner, user_sid) && !EqualSid(owner, system_sid))) {
        return {ErrorCode::Auth,
                std::string(option) + " must be owned by the current user or SYSTEM"};
    }
    ACL_SIZE_INFORMATION information{};
    if (!GetAclInformation(acl, &information, sizeof(information), AclSizeInformation)) {
        return {ErrorCode::Auth, std::string("could not inspect ") + option + " ACL entries"};
    }
    for (DWORD index = 0; index < information.AceCount; ++index) {
        void* raw_ace = nullptr;
        if (!GetAce(acl, index, &raw_ace)) {
            return {ErrorCode::Auth, std::string("could not inspect ") + option + " ACL entry"};
        }
        const ACE_HEADER* header = static_cast<const ACE_HEADER*>(raw_ace);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            if (header->AceType == ACCESS_ALLOWED_OBJECT_ACE_TYPE ||
                header->AceType == ACCESS_ALLOWED_CALLBACK_ACE_TYPE ||
                header->AceType == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE) {
                return {ErrorCode::Auth,
                        std::string(option) + " contains an unsupported allow ACL entry"};
            }
            continue;
        }
        const ACCESS_ALLOWED_ACE* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw_ace);
        PSID trustee = const_cast<DWORD*>(&ace->SidStart);
        if (!EqualSid(trustee, user_sid) && !EqualSid(trustee, system_sid)) {
            return {ErrorCode::Auth,
                    std::string(option) + " grants access beyond the current user and SYSTEM"};
        }
    }
    return ok_error();
}
#endif

Error load_secret_file(const std::string& path,
                       const std::filesystem::path& workspace,
                       const char* option,
                       std::string& secret) {
    if (path.empty()) return ok_error();
    bool crosses_link = false;
    Error link_error = platform::path_contains_link_or_reparse(path, crosses_link);
    if (!link_error.ok()) return link_error;
    if (crosses_link) {
        return {ErrorCode::Auth,
                std::string(option) + " must not cross a symlink or reparse point"};
    }
    std::error_code filesystem_error;
    const std::filesystem::path canonical = std::filesystem::canonical(
        std::filesystem::u8path(path), filesystem_error);
    if (filesystem_error) {
        return {ErrorCode::FileRead, std::string("could not resolve ") + option + " " + path};
    }
    if (path_within(workspace, canonical)) {
        return {ErrorCode::BadArgs,
                std::string(option) + " must be outside the served workspace"};
    }
#if !defined(_WIN32)
    struct stat info{};
    if (::lstat(canonical.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
        return {ErrorCode::FileRead, std::string(option) + " must name a regular file"};
    }
    if ((info.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        return {ErrorCode::Auth,
                std::string(option) + " must not grant permissions to group or other users"};
    }
#else
    Error acl_error = verify_windows_private_acl(canonical, option);
    if (!acl_error.ok()) return acl_error;
#endif
    std::ifstream input(canonical, std::ios::binary);
    if (!input) return {ErrorCode::FileRead, std::string("could not open ") + option};
    secret.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (!input.eof() || secret.size() > 4096U) {
        secret.clear();
        return {ErrorCode::FileRead, std::string(option) + " must be a file of at most 4096 bytes"};
    }
    while (!secret.empty() && (secret.back() == '\n' || secret.back() == '\r')) secret.pop_back();
    if (secret.empty() || secret.find('\n') != std::string::npos || secret.find('\r') != std::string::npos) {
        secret.clear();
        return {ErrorCode::Auth, std::string(option) + " must contain one non-empty secret"};
    }
    return ok_error();
}

Error validate_environment_secret(const char* name, std::string& secret) {
    if (secret.size() > 4096U || secret.find('\n') != std::string::npos ||
        secret.find('\r') != std::string::npos) {
        secret.clear();
        return {ErrorCode::Auth,
                std::string(name) + " must contain one secret of at most 4096 bytes"};
    }
    return ok_error();
}

}  // namespace

Error validate_server_options(const cli::Options& options) {
    if (!options.server) {
        return options.server_options_seen
                   ? Error{ErrorCode::BadArgs,
                           "--workspace, --port, --server-secret-file, --mcp-secret-file, "
                           "--max-connections, and --max-jobs require server mode"}
                   : ok_error();
    }
    if (options.port < 1 || options.port > 65535) {
        return {ErrorCode::BadArgs, "--port must be between 1 and 65535"};
    }
    if (options.max_connections < 1 || options.max_connections > 4096) {
        return {ErrorCode::BadArgs, "--max-connections must be between 1 and 4096"};
    }
    if (options.max_jobs < 1 || options.max_jobs > 4096) {
        return {ErrorCode::BadArgs, "--max-jobs must be between 1 and 4096"};
    }
    if (options.key_stdin) {
        return {ErrorCode::BadArgs,
                "--key-stdin cannot be used in server mode; use a provider environment variable or --key-file"};
    }
    if (options.agent || options.agent_run || options.image || options.benchmark || options.grade ||
        options.editor || options.tui || options.repl || options.security_review || options.index_code ||
        options.print_index || options.clear_index || options.list_models || !options.prompt.empty() ||
        !options.prompt_file.empty() || !options.input_path.empty() || !options.fetch_url.empty() ||
        !options.search_query.empty() || !options.positional_url.empty()) {
        return {ErrorCode::BadArgs, "server mode cannot be combined with another command or prompt"};
    }
    return ok_error();
}

int run_server(const cli::Options& options) {
    Error error = validate_server_options(options);
    if (!error.ok()) {
        app::print_error(error);
        return app::exit_code_for(error.code);
    }
    std::error_code filesystem_error;
    const std::filesystem::path workspace = std::filesystem::canonical(
        std::filesystem::u8path(options.workspace.empty() ? "." : options.workspace),
        filesystem_error);
    if (filesystem_error || !std::filesystem::is_directory(workspace, filesystem_error)) {
        error = {ErrorCode::FileRead, "--workspace must name an existing directory"};
        app::print_error(error);
        return app::exit_code_for(error.code);
    }

    AuthConfig auth;
    if (!options.server_secret_file.empty()) {
        error = load_secret_file(options.server_secret_file, workspace,
                                 "--server-secret-file", auth.full_control_secret);
    } else {
        auth.full_control_secret = platform::environment_value("AINIUX_SERVER_SECRET");
        error = validate_environment_secret("AINIUX_SERVER_SECRET", auth.full_control_secret);
    }
    if (!error.ok()) {
        app::print_error(error);
        return app::exit_code_for(error.code);
    }
    if (!options.mcp_secret_file.empty()) {
        error = load_secret_file(options.mcp_secret_file, workspace,
                                 "--mcp-secret-file", auth.mcp_secret);
    } else {
        auth.mcp_secret = platform::environment_value("AINIUX_MCP_SECRET");
        error = validate_environment_secret("AINIUX_MCP_SECRET", auth.mcp_secret);
    }
    if (!error.ok()) {
        app::print_error(error);
        return app::exit_code_for(error.code);
    }
    if (auth.full_control_secret.empty()) {
        error = {ErrorCode::Auth,
                 "server mode requires AINIUX_SERVER_SECRET or --server-secret-file"};
        app::print_error(error);
        return app::exit_code_for(error.code);
    }
    if (!auth.mcp_secret.empty() && constant_time_equal(auth.full_control_secret, auth.mcp_secret)) {
        error = {ErrorCode::Auth, "full-control and MCP-only secrets must be different"};
        app::print_error(error);
        return app::exit_code_for(error.code);
    }

    runtime::InterruptGuard interrupt;
    if (!interrupt.installed()) {
        error = {ErrorCode::Internal, "could not install the server Ctrl+C handler"};
        app::print_error(error);
        return app::exit_code_for(error.code);
    }
    Listener listener;
    ListenerConfig config;
    config.port = static_cast<unsigned short>(options.port);
    config.max_connections = static_cast<std::size_t>(options.max_connections);
    config.max_jobs = static_cast<std::size_t>(options.max_jobs);
    config.auth = std::move(auth);
    config.base_options = options;
    config.workspace = workspace.u8string();
    error = listener.start(std::move(config));
    if (!error.ok()) {
        app::print_error(error);
        return app::exit_code_for(error.code);
    }
    if (!options.quiet) {
        std::cerr << "Ainiux control API listening on http://127.0.0.1:" << listener.port()
                  << "/ainiux/v1/ (workspace fixed; bearer authentication required)\n";
    }
    error = listener.serve_until([&] { return interrupt.interrupted(); });
    if (!error.ok()) {
        app::print_error(error);
        return app::exit_code_for(error.code);
    }
    if (!options.quiet) std::cerr << "Ainiux control API stopped\n";
    return 0;
}

}  // namespace ainiux::server
