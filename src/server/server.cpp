#include "server/server.hpp"

#include <algorithm>
#include <filesystem>
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
#include "server/web_ui_launch.hpp"

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
                       bool require_outside_workspace,
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
    if (require_outside_workspace && path_within(workspace, canonical)) {
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
    Error read_error = platform::read_file_bounded(canonical.u8string(), 4096U, secret);
    if (!read_error.ok()) {
        secret.clear();
        return {read_error.code, std::string("could not read ") + option + ": " + read_error.message};
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

bool ipv4_address(const std::string& text, bool& loopback) {
    loopback = false;
    if (text == "localhost") {
        loopback = true;
        return true;
    }
    int components = 0;
    unsigned int first = 0;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t dot = text.find('.', start);
        const std::string part = text.substr(
            start, dot == std::string::npos ? std::string::npos : dot - start);
        if (part.empty() || part.size() > 3U || (part.size() > 1U && part.front() == '0')) return false;
        unsigned int number = 0;
        for (unsigned char c : part) {
            if (c < '0' || c > '9') return false;
            number = number * 10U + static_cast<unsigned int>(c - '0');
        }
        if (number > 255U) return false;
        if (components == 0) first = number;
        ++components;
        if (dot == std::string::npos) break;
        start = dot + 1U;
    }
    loopback = components == 4 && first == 127U;
    return components == 4;
}

std::string effective_bind_address(const cli::Options& options) {
    return options.webui && !options.server_bind_explicit ? "0.0.0.0"
                                                         : options.bind_address;
}

Error validate_tls_file(const std::string& path,
                        const std::filesystem::path& workspace,
                        const char* option,
                        bool private_file,
                        bool outside_workspace) {
    if (path.empty()) return ok_error();
    bool crosses_link = false;
    Error error = platform::path_contains_link_or_reparse(path, crosses_link);
    if (!error.ok()) return error;
    if (crosses_link) {
        return {ErrorCode::Tls,
                std::string(option) + " must not cross a symlink or reparse point"};
    }
    std::error_code filesystem_error;
    const std::filesystem::path canonical = std::filesystem::canonical(
        std::filesystem::u8path(path), filesystem_error);
    if (filesystem_error || !std::filesystem::is_regular_file(canonical, filesystem_error)) {
        return {ErrorCode::FileRead, std::string(option) + " must name a regular file"};
    }
    if (outside_workspace && path_within(workspace, canonical)) {
        return {ErrorCode::BadArgs,
                std::string(option) + " must be outside the served workspace"};
    }
    if (!private_file) return ok_error();
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
    return verify_windows_private_acl(canonical, option);
#endif
    return ok_error();
}

}  // namespace

Error managed_server_secret_path(std::string& path) {
    const std::string home = platform::home_directory();
    if (home.empty()) {
        path.clear();
        return {ErrorCode::Config,
                "cannot locate the user home directory for ~/.ainiux/server-secret"};
    }
    path = (std::filesystem::u8path(home) / ".ainiux" / "server-secret").u8string();
    return ok_error();
}

Error load_or_create_managed_server_secret(const std::string& path,
                                           std::string& secret,
                                           bool& created) {
    secret.clear();
    created = false;
    const std::filesystem::path secret_path = std::filesystem::u8path(path);
    const std::filesystem::path parent = secret_path.parent_path();
    if (path.empty() || parent.empty()) {
        return {ErrorCode::BadArgs, "managed server secret path must include a parent directory"};
    }
    Error error = platform::ensure_private_directory(parent.u8string(), true, true);
    if (!error.ok()) return error;

    std::error_code exists_error;
    const bool already_exists = std::filesystem::exists(secret_path, exists_error);
    if (exists_error) {
        return {ErrorCode::FileRead,
                "could not inspect managed server secret: " + exists_error.message()};
    }
    std::string generated;
    if (!already_exists) {
        error = platform::secure_random_hex(32U, generated);
        if (!error.ok()) return error;
        const Error create_error = platform::atomic_write_private_create(path, generated, true);
        if (create_error.ok()) {
            created = true;
        } else {
            exists_error.clear();
            if (!std::filesystem::exists(secret_path, exists_error) || exists_error) {
                return create_error;
            }
        }
    }

    const std::filesystem::path unused_workspace;
    error = load_secret_file(path, unused_workspace, "managed server secret", false, secret);
    std::fill(generated.begin(), generated.end(), '\0');
    generated.clear();
    if (!error.ok()) return error;
    if (secret.size() != 64U) {
        secret.clear();
        return {ErrorCode::Auth,
                "managed server secret must contain exactly 64 lowercase hexadecimal characters"};
    }
    for (const unsigned char c : secret) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            secret.clear();
            return {ErrorCode::Auth,
                    "managed server secret must contain exactly 64 lowercase hexadecimal characters"};
        }
    }
    return ok_error();
}

Error validate_server_options(const cli::Options& options) {
    if (!options.server) {
        return options.server_options_seen
                   ? Error{ErrorCode::BadArgs,
                           "--webui, --workspace, --bind, --port, server credentials, "
                           "TLS/bind policy, and server limits require server mode"}
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
    if (options.max_sessions < 1 || options.max_sessions > 1024) {
        return {ErrorCode::BadArgs, "--max-sessions must be between 1 and 1024"};
    }
    bool loopback = false;
    const std::string bind_address = effective_bind_address(options);
    if (!ipv4_address(bind_address, loopback)) {
        return {ErrorCode::BadArgs, "--bind must be an IPv4 address or localhost"};
    }
    if (options.tls_cert_file.empty() != options.tls_key_file.empty()) {
        return {ErrorCode::BadArgs, "--tls-cert and --tls-key must be supplied together"};
    }
    if (!loopback && options.tls_cert_file.empty() && !options.insecure_plain_bind &&
        !options.webui) {
        return {ErrorCode::BadArgs,
                "non-loopback --bind requires TLS or explicit --insecure-plain-bind"};
    }
    if (loopback && options.insecure_plain_bind) {
        return {ErrorCode::BadArgs,
                "--insecure-plain-bind is only meaningful with a non-loopback --bind"};
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
    error = validate_tls_file(options.tls_cert_file, workspace, "--tls-cert", false, false);
    if (error.ok()) {
        error = validate_tls_file(options.tls_key_file, workspace, "--tls-key", true, true);
    }
    if (!error.ok()) {
        app::print_error(error);
        return app::exit_code_for(error.code);
    }

    const std::string bind_address = effective_bind_address(options);
    AuthConfig auth;
    bool managed_secret = false;
    bool managed_secret_created = false;
    std::string managed_secret_file;
    if (!options.server_secret_file.empty()) {
        error = load_secret_file(options.server_secret_file, workspace,
                                 "--server-secret-file", true, auth.full_control_secret);
    } else {
        auth.full_control_secret = platform::environment_value("AINIUX_SERVER_SECRET");
        error = validate_environment_secret("AINIUX_SERVER_SECRET", auth.full_control_secret);
        if (error.ok() && auth.full_control_secret.empty()) {
            error = managed_server_secret_path(managed_secret_file);
            if (error.ok()) {
                error = load_or_create_managed_server_secret(
                    managed_secret_file, auth.full_control_secret, managed_secret_created);
                managed_secret = error.ok();
            }
        }
    }
    if (!error.ok()) {
        app::print_error(error);
        return app::exit_code_for(error.code);
    }
    if (!options.mcp_secret_file.empty()) {
        error = load_secret_file(options.mcp_secret_file, workspace,
                                 "--mcp-secret-file", true, auth.mcp_secret);
    } else {
        auth.mcp_secret = platform::environment_value("AINIUX_MCP_SECRET");
        error = validate_environment_secret("AINIUX_MCP_SECRET", auth.mcp_secret);
    }
    if (!error.ok()) {
        app::print_error(error);
        return app::exit_code_for(error.code);
    }
    bool loopback = false;
    (void)ipv4_address(bind_address, loopback);
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
    config.bind_address = bind_address;
    config.port = static_cast<unsigned short>(options.port);
    config.max_connections = static_cast<std::size_t>(options.max_connections);
    config.max_jobs = static_cast<std::size_t>(options.max_jobs);
    config.max_sessions = static_cast<std::size_t>(options.max_sessions);
    std::string managed_secret_for_display =
        managed_secret && !options.quiet ? auth.full_control_secret : std::string{};
    config.auth = std::move(auth);
    config.base_options = options;
    config.base_options.bind_address = bind_address;
    config.workspace = workspace.u8string();
    config.tls_cert_file = options.tls_cert_file;
    config.tls_key_file = options.tls_key_file;
    config.allow_remote_yolo = loopback || options.allow_remote_yolo;
    error = listener.start(std::move(config));
    if (!error.ok()) {
        std::fill(managed_secret_for_display.begin(), managed_secret_for_display.end(), '\0');
        managed_secret_for_display.clear();
        app::print_error(error);
        return app::exit_code_for(error.code);
    }
    if (options.webui) {
        const std::vector<std::string> urls = web_ui_urls(
            listener.bind_address(), listener.port(), listener.tls_enabled(),
            local_ipv4_addresses());
        if (!loopback) {
            std::cerr << "WARNING: the Ainiux browser controller is reachable from other "
                         "machines on this network using "
                      << (listener.tls_enabled() ? "TLS" : "PLAINTEXT HTTP") << ".\n";
            if (!listener.tls_enabled()) {
                std::cerr << "WARNING: bearer tokens and workspace data can be observed on "
                             "an untrusted network; use --tls-cert/--tls-key when possible.\n";
            }
        }
        std::cerr << "Ainiux web UI:\n";
        for (const std::string& url : urls) std::cerr << "  " << url << '\n';
        if (listener.bind_address() == "0.0.0.0") {
            std::cerr << "Interface addresses are local hints; router NAT, firewalls, VPNs, "
                         "and DNS may require a different reachable address.\n";
        }
        if (managed_secret && !options.quiet) {
            std::cerr << "Controller token: " << managed_secret_for_display << '\n'
                      << "Managed token file: " << managed_secret_file
                      << (managed_secret_created ? " (created)" : "") << '\n';
            std::fill(managed_secret_for_display.begin(), managed_secret_for_display.end(), '\0');
            managed_secret_for_display.clear();
        } else if (!managed_secret && !options.quiet) {
            std::cerr << "Enter the controller token configured by --server-secret-file or "
                         "AINIUX_SERVER_SECRET.\n";
        }
        if (!urls.empty()) {
            const BrowserLaunchResult launch = launch_browser_best_effort(urls.front());
            if (!launch.started && !options.quiet && !launch.detail.empty())
                std::cerr << launch.detail << "; open one of the URLs above manually.\n";
        }
    } else if (!options.quiet) {
        const char* scheme = listener.tls_enabled() ? "https" : "http";
        std::cerr << "Ainiux control API listening on " << scheme << "://"
                  << listener.bind_address() << ':' << listener.port()
                  << "/ainiux/v1/ (workspace fixed; bearer authentication required)\n";
        if (!loopback) {
            std::cerr << "WARNING: direct remote control is enabled for workspace "
                      << workspace.u8string() << " using "
                      << (listener.tls_enabled() ? "TLS" : "explicit insecure plain HTTP")
                      << "; remote Yolo is "
                      << (options.allow_remote_yolo ? "enabled" : "denied") << "\n";
        }
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
