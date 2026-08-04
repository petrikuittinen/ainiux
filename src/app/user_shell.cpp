#include "app/user_shell.hpp"

#include "platform/environment.hpp"
#if defined(_WIN32)
#include "platform/windows_utf.hpp"
#endif
#include "runtime/subprocess.hpp"
#include "security/redact.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ainiux::app {
namespace {

std::string resolve_shell_path() {
#if defined(_WIN32)
    const std::string system_root = platform::environment_value("SystemRoot");
    if (system_root.empty()) return {};
    const std::filesystem::path shell =
        std::filesystem::u8path(system_root) / "System32" / "WindowsPowerShell" /
        "v1.0" / "powershell.exe";
    std::error_code error;
    if (std::filesystem::is_regular_file(shell, error) && !error)
        return shell.u8string();
    return {};
#else
    static const char* kCandidates[] = {"/bin/sh", "/usr/bin/sh"};
    for (const char* path : kCandidates) {
        std::error_code error;
        if (std::filesystem::is_regular_file(path, error) && !error) return path;
    }
    return {};
#endif
}

std::string process_cwd() {
    std::error_code error;
    const std::filesystem::path cwd = std::filesystem::current_path(error);
    if (!error) return cwd.u8string();
    return ".";
}

#if defined(_WIN32)
std::string base64_encode(const std::string& bytes) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((bytes.size() + 2U) / 3U) * 4U);
    for (std::size_t offset = 0; offset < bytes.size(); offset += 3) {
        const std::size_t remaining = bytes.size() - offset;
        const unsigned int first = static_cast<unsigned char>(bytes[offset]);
        const unsigned int second =
            remaining > 1 ? static_cast<unsigned char>(bytes[offset + 1]) : 0U;
        const unsigned int third =
            remaining > 2 ? static_cast<unsigned char>(bytes[offset + 2]) : 0U;
        const unsigned int value = (first << 16U) | (second << 8U) | third;
        output.push_back(alphabet[(value >> 18U) & 0x3FU]);
        output.push_back(alphabet[(value >> 12U) & 0x3FU]);
        output.push_back(remaining > 1 ? alphabet[(value >> 6U) & 0x3FU] : '=');
        output.push_back(remaining > 2 ? alphabet[value & 0x3FU] : '=');
    }
    return output;
}

Error powershell_encoded_command(const std::string& command, std::string& encoded) {
    const std::string script =
        "$ProgressPreference='SilentlyContinue';"
        "$ainiux_utf8=New-Object System.Text.UTF8Encoding($false);"
        "$OutputEncoding=$ainiux_utf8;"
        "[Console]::OutputEncoding=$ainiux_utf8;"
        "[Console]::InputEncoding=$ainiux_utf8;"
        "& { " + command +
        " };$ainiux_ok=$?;$ainiux_code=$LASTEXITCODE;"
        "if($ainiux_ok){exit 0}"
        "elseif($null -ne $ainiux_code){exit [int]$ainiux_code}else{exit 1}";
    std::wstring wide;
    Error error = platform::utf8_to_utf16(script, wide);
    if (!error.ok()) return error;
    std::string bytes;
    bytes.reserve(wide.size() * 2U);
    for (wchar_t value : wide) {
        const unsigned int code = static_cast<unsigned int>(value);
        bytes.push_back(static_cast<char>(code & 0xFFU));
        bytes.push_back(static_cast<char>((code >> 8U) & 0xFFU));
    }
    encoded = base64_encode(bytes);
    return ok_error();
}
#endif

void append_environment(std::vector<std::string>& environment, const char* name) {
    const std::string value = platform::environment_value(name);
    if (!value.empty()) environment.push_back(std::string(name) + "=" + value);
}

std::string ascii_trim_copy(std::string text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' ||
                             text.front() == '\n')) {
        text.erase(text.begin());
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' ||
                             text.back() == '\n')) {
        text.pop_back();
    }
    return text;
}

}  // namespace

bool parse_user_shell_invocation(const std::string& trimmed,
                                 std::string& command_out,
                                 std::string& error_out,
                                 UserShellDestination& destination_out) {
    command_out.clear();
    error_out.clear();
    destination_out = UserShellDestination::Notice;
    if (trimmed.empty()) return false;

    // Draft forms first so !! is not treated as notice !.
    if (trimmed.rfind("!!", 0) == 0) {
        destination_out = UserShellDestination::Draft;
        command_out = ascii_trim_copy(trimmed.substr(2));
        if (command_out.empty()) {
            error_out = "Usage: !!COMMAND  or  /shell-stdout COMMAND";
        }
        return true;
    }
    if (trimmed == "/shell-stdout") {
        destination_out = UserShellDestination::Draft;
        error_out = "Usage: /shell-stdout COMMAND  or  !!COMMAND";
        return true;
    }
    if (trimmed.rfind("/shell-stdout ", 0) == 0) {
        destination_out = UserShellDestination::Draft;
        command_out = ascii_trim_copy(trimmed.substr(14));
        if (command_out.empty()) {
            error_out = "Usage: /shell-stdout COMMAND  or  !!COMMAND";
        }
        return true;
    }

    if (trimmed.front() == '!') {
        destination_out = UserShellDestination::Notice;
        command_out = ascii_trim_copy(trimmed.substr(1));
        if (command_out.empty()) {
            error_out = "Usage: !COMMAND  or  /shell COMMAND";
        }
        return true;
    }

    if (trimmed == "/shell") {
        destination_out = UserShellDestination::Notice;
        error_out = "Usage: /shell COMMAND  or  !COMMAND";
        return true;
    }
    if (trimmed.rfind("/shell ", 0) == 0) {
        destination_out = UserShellDestination::Notice;
        command_out = ascii_trim_copy(trimmed.substr(7));
        if (command_out.empty()) {
            error_out = "Usage: /shell COMMAND  or  !COMMAND";
        }
        return true;
    }
    return false;
}

Error run_user_shell(const std::string& command,
                     const UserShellOptions& options,
                     UserShellResult& result) {
    result = UserShellResult{};
    result.command = command;
    if (ascii_trim_copy(command).empty()) {
        return {ErrorCode::BadArgs, "Usage: /shell COMMAND  or  !COMMAND"};
    }

    const std::string shell = resolve_shell_path();
    if (shell.empty()) {
#if defined(_WIN32)
        return {ErrorCode::FileRead,
                "could not find Windows PowerShell 5.1 below %SystemRoot%\\System32"};
#else
        return {ErrorCode::FileRead, "could not find executable /bin/sh or /usr/bin/sh"};
#endif
    }

    std::string cwd = options.cwd.empty() ? process_cwd() : options.cwd;
    result.cwd = cwd;

    runtime::SubprocessOptions subprocess;
    subprocess.executable = shell;
    subprocess.cwd = cwd;
    subprocess.timeout_ms = options.timeout_ms > 0 ? options.timeout_ms : 60000;
    subprocess.stdout_limit = options.stdout_limit > 0 ? options.stdout_limit : 256 * 1024;
    subprocess.stderr_limit = options.stderr_limit > 0 ? options.stderr_limit : 256 * 1024;
    subprocess.cancellation = options.cancellation;
#if defined(_WIN32)
    std::string encoded;
    Error encode_error = powershell_encoded_command(command, encoded);
    if (!encode_error.ok()) return encode_error;
    subprocess.arguments = {"-NoLogo", "-NoProfile", "-NonInteractive",
                            "-ExecutionPolicy", "Bypass", "-EncodedCommand", encoded};
    append_environment(subprocess.environment, "SystemRoot");
    append_environment(subprocess.environment, "WINDIR");
    append_environment(subprocess.environment, "COMSPEC");
    append_environment(subprocess.environment, "PATH");
    append_environment(subprocess.environment, "PATHEXT");
    append_environment(subprocess.environment, "HOME");
    append_environment(subprocess.environment, "USERPROFILE");
    append_environment(subprocess.environment, "TEMP");
    append_environment(subprocess.environment, "TMP");
    append_environment(subprocess.environment, "PSModulePath");
#else
    subprocess.arguments = {"-c", command};
    subprocess.environment = {
        "PATH=/usr/local/bin:/usr/bin:/bin",
        "LC_ALL=C.UTF-8",
        "LANG=C.UTF-8",
        "PAGER=cat",
    };
    append_environment(subprocess.environment, "HOME");
    append_environment(subprocess.environment, "TERM");
    append_environment(subprocess.environment, "USER");
#endif
    runtime::SubprocessResult process_result;
    Error error = runtime::run_subprocess(subprocess, process_result);
    result.stdout_text = std::move(process_result.stdout_text);
    result.stderr_text = std::move(process_result.stderr_text);
    result.exit_status = process_result.exit_code;
    result.signal = process_result.signal;
    result.duration_ms = process_result.duration_ms;
    result.stdout_truncated = process_result.stdout_truncated;
    result.stderr_truncated = process_result.stderr_truncated;
    result.cancelled = process_result.termination ==
                       runtime::SubprocessTerminationReason::Cancelled;
    result.timed_out = process_result.termination ==
                       runtime::SubprocessTerminationReason::TimedOut;
    return error;
}

std::string format_user_shell_notice(const UserShellResult& result,
                                     const std::vector<std::string>& secrets) {
    std::string text;
    text.reserve(result.stdout_text.size() + result.stderr_text.size() + 128);
    text += "$ ";
    text += result.command;
    text += "\n";
    text += "exit=";
    text += std::to_string(result.exit_status);
    if (result.signal > 0) {
        text += " signal=";
        text += std::to_string(result.signal);
    }
    text += "  ";
    text += std::to_string(result.duration_ms);
    text += "ms  cwd=";
    text += result.cwd;
    if (result.cancelled) text += "  [cancelled]";
    if (result.timed_out) text += "  [timeout]";
    text += "\n";
    if (!result.stdout_text.empty()) {
        text += result.stdout_text;
        if (!result.stdout_text.empty() && result.stdout_text.back() != '\n') text += "\n";
    }
    if (!result.stderr_text.empty()) {
        text += "--- stderr ---\n";
        text += result.stderr_text;
        if (!result.stderr_text.empty() && result.stderr_text.back() != '\n') text += "\n";
    }
    if (result.stdout_truncated) text += "[stdout truncated]\n";
    if (result.stderr_truncated) text += "[stderr truncated]\n";
    if (result.stdout_text.empty() && result.stderr_text.empty() && !result.cancelled &&
        !result.timed_out) {
        text += "(no output)\n";
    }
    return redact_secrets(std::move(text), secrets);
}

std::string format_user_shell_draft_stdout(const UserShellResult& result,
                                           const std::vector<std::string>& secrets) {
    return redact_secrets(result.stdout_text, secrets);
}

bool user_shell_failed(const Error& error, const UserShellResult& result) {
    if (!error.ok()) return true;
    if (result.cancelled || result.timed_out) return true;
    if (result.signal > 0) return true;
    if (result.exit_status != 0) return true;
    return false;
}

namespace {

std::string first_line_snippet(const std::string& text, std::size_t max_bytes) {
    std::string line;
    for (char ch : text) {
        if (ch == '\n' || ch == '\r') break;
        line.push_back(ch);
        if (line.size() >= max_bytes) break;
    }
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
    std::size_t start = 0;
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) ++start;
    if (start > 0) line = line.substr(start);
    if (line.size() > max_bytes) line.resize(max_bytes);
    return line;
}

}  // namespace

std::string format_user_shell_draft_status(const Error& error,
                                           const UserShellResult& result,
                                           const std::vector<std::string>& secrets) {
    std::string line;
    if (error.code == ErrorCode::Cancelled || result.cancelled) {
        line = "Shell-stdout cancelled";
        if (!result.stdout_text.empty()) {
            line += " · partial draft " + std::to_string(result.stdout_text.size()) + " bytes";
        }
    } else if (error.code == ErrorCode::Timeout || result.timed_out) {
        line = "Shell-stdout timed out";
        if (result.duration_ms > 0) {
            line += " after " + std::to_string(result.duration_ms) + "ms";
        }
        if (!result.stdout_text.empty()) {
            line += " · partial draft " + std::to_string(result.stdout_text.size()) + " bytes";
        }
    } else if (!error.ok()) {
        line = "Shell-stdout error · ";
        line += error.message.empty() ? error_code_name(error.code) : error.message;
    } else if (result.signal > 0) {
        line = "Shell-stdout failed · signal " + std::to_string(result.signal);
    } else if (result.exit_status != 0) {
        line = "Shell-stdout failed · exit " + std::to_string(result.exit_status);
    } else {
        line = "Shell → draft · exit 0 · " + std::to_string(result.stdout_text.size()) + " bytes";
        if (result.stdout_truncated) line += " · truncated";
        if (result.stdout_text.empty()) line += " · empty stdout";
        return redact_secrets(std::move(line), secrets);
    }

    const std::string snippet = first_line_snippet(result.stderr_text, 160);
    if (!snippet.empty()) {
        line += " · ";
        line += snippet;
    } else if (result.exit_status == 127 && error.ok()) {
        line += " · command not found (exit 127)";
    } else if (result.stdout_text.empty() && result.stderr_text.empty() && error.ok() &&
               result.exit_status != 0) {
        line += " · no output";
    }
    if (result.stdout_truncated) line += " · stdout truncated";
    if (result.stderr_truncated) line += " · stderr truncated";
    return redact_secrets(std::move(line), secrets);
}

std::string format_user_shell_failure_notice(const Error& error,
                                             const UserShellResult& result,
                                             const std::vector<std::string>& secrets) {
    std::string text;
    text.reserve(result.stderr_text.size() + result.command.size() + 160);
    text += "shell-stdout failed\n";
    text += "command: ";
    text += result.command.empty() ? "(empty)" : result.command;
    text += "\n";
    if (!error.ok()) {
        text += "error: ";
        text += error_code_name(error.code);
        if (!error.message.empty()) {
            text += ": ";
            text += error.message;
        }
        text += "\n";
    }
    if (result.cancelled) text += "reason: cancelled\n";
    if (result.timed_out) text += "reason: timeout\n";
    if (result.exit_status >= 0) {
        text += "exit: ";
        text += std::to_string(result.exit_status);
        text += "\n";
    }
    if (result.signal > 0) {
        text += "signal: ";
        text += std::to_string(result.signal);
        text += "\n";
    }
    if (result.duration_ms > 0) {
        text += "duration_ms: ";
        text += std::to_string(result.duration_ms);
        text += "\n";
    }
    if (!result.cwd.empty()) {
        text += "cwd: ";
        text += result.cwd;
        text += "\n";
    }
    if (!result.stderr_text.empty()) {
        text += "stderr:\n";
        text += result.stderr_text;
        if (result.stderr_text.back() != '\n') text += "\n";
    } else if (error.ok() && result.exit_status != 0 && !result.cancelled && !result.timed_out) {
        text += "stderr: (empty)\n";
        if (result.exit_status == 127) {
            text += "hint: exit 127 usually means the command was not found on PATH\n";
        }
    }
    if (result.stdout_truncated) text += "[stdout truncated]\n";
    if (result.stderr_truncated) text += "[stderr truncated]\n";
    if (!result.stdout_text.empty()) {
        text += "note: pure stdout was placed in the input draft (edit/clear before send)\n";
    }
    return redact_secrets(std::move(text), secrets);
}

bool is_provider_chat_role(const std::string& role) {
    return role == "system" || role == "user" || role == "assistant";
}

std::vector<provider::Message> provider_chat_messages(
    const std::vector<provider::Message>& messages) {
    std::vector<provider::Message> out;
    out.reserve(messages.size());
    for (const provider::Message& message : messages) {
        if (is_provider_chat_role(message.role)) out.push_back(message);
    }
    return out;
}

}  // namespace ainiux::app
