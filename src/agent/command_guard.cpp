#include "agent/command_guard.hpp"

#include "agent/read_only_command.hpp"

#include <algorithm>
#include <cctype>

namespace ainiux::agent {
namespace {

std::string lowercase(std::string text) {
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return text;
}

bool versioned_interpreter(const std::string& command, const char* prefix) {
    const std::size_t n = std::char_traits<char>::length(prefix);
    if (command.size() <= n || command.compare(0, n, prefix) != 0 || command[n] != '.')
        return false;
    for (std::size_t i = n + 1; i < command.size(); ++i) {
        if (command[i] != '.' && (command[i] < '0' || command[i] > '9')) return false;
    }
    return true;
}

bool is_python_interpreter(const std::string& command) {
    return command == "python" || command == "python3" || command == "py" ||
           command == "pypy" || command == "pypy3" ||
           versioned_interpreter(command, "python") ||
           versioned_interpreter(command, "python3") ||
           versioned_interpreter(command, "pypy") ||
           versioned_interpreter(command, "pypy3");
}

bool is_python_env_mutation_module(const std::string& module) {
    const std::string lower = lowercase(module);
    return lower == "pip" || lower == "pip3" || lower == "ensurepip" ||
           lower == "easy_install" || lower.rfind("pip.", 0) == 0 ||
           lower.rfind("ensurepip.", 0) == 0;
}

bool has_flag(const std::vector<std::string>& args, const char* flag) {
    for (const std::string& arg : args) {
        if (arg == flag) return true;
    }
    return false;
}

bool is_database_name(const std::string& path) {
    const std::string lower = lowercase(path);
    static const char* kSuffixes[] = {".sqlite", ".sqlite3", ".db", ".db3", ".duckdb"};
    for (const char* suffix : kSuffixes) {
        const std::size_t n = std::char_traits<char>::length(suffix);
        if (lower.size() >= n && lower.compare(lower.size() - n, n, suffix) == 0) return true;
    }
    return false;
}

bool looks_like_sql_destructive(const std::string& text) {
    const std::string lower = lowercase(text);
    return lower.find("drop table") != std::string::npos ||
           lower.find("drop database") != std::string::npos ||
           lower.find("delete from") != std::string::npos ||
           lower.find("truncate ") != std::string::npos ||
           lower.find("alter table") != std::string::npos;
}

GuardResult deny(const char* rule_id, const std::string& message) {
    GuardResult result;
    result.decision = GuardDecision::Deny;
    result.rule_id = rule_id;
    result.message = message;
    return result;
}

GuardResult ask(const char* rule_id, const std::string& message) {
    GuardResult result;
    result.decision = GuardDecision::Ask;
    result.rule_id = rule_id;
    result.message = message;
    return result;
}

}  // namespace

std::string normalized_command_basename(const std::string& text) {
    std::string command = text;
    const std::size_t slash = command.find_last_of("/\\");
    if (slash != std::string::npos) command.erase(0, slash + 1);
    command = lowercase(std::move(command));
    static const char* extensions[] = {".exe", ".com", ".cmd", ".bat"};
    for (const char* extension : extensions) {
        const std::size_t length = std::char_traits<char>::length(extension);
        if (command.size() > length &&
            command.compare(command.size() - length, length, extension) == 0) {
            command.resize(command.size() - length);
            break;
        }
    }
    return command;
}

GuardResult evaluate_command_guard(const std::vector<std::string>& arguments) {
    if (arguments.empty()) return {};
    const std::string command = normalized_command_basename(arguments.front());

    if (command == "del" || command == "erase") {
        return ask("ask_on_windows_delete",
                   "refusing Windows file deletion via del/erase; use the remove tool for "
                   "workspace paths");
    }
    if (command == "rd" ||
        (command == "rmdir" &&
         (has_flag(arguments, "/s") || has_flag(arguments, "/S")))) {
        const bool recursive = has_flag(arguments, "/s") || has_flag(arguments, "/S");
        return ask(recursive ? "ask_on_recursive_delete" : "ask_on_windows_delete",
                   recursive ? "refusing recursive Windows rmdir /s"
                             : "refusing Windows directory deletion via rmdir/rd");
    }
    if (command == "format" || command == "diskpart" || command == "bcdedit" ||
        command == "cipher")
        return deny("forbid_disk_destroy",
                    "Windows disk/device destructive commands are not allowed");
    if (command == "reg" && arguments.size() >= 2 &&
        lowercase(arguments[1]) == "delete")
        return deny("forbid_registry_delete", "Windows registry deletion is not allowed");
    if (command == "runas" || command == "elevate" || command == "gsudo")
        return deny("forbid_privilege_escalation",
                    "Windows privilege elevation commands are not allowed");
    if (command == "cmd") {
        for (const std::string& argument : arguments) {
            const std::string lower = lowercase(argument);
            if (lower == "/c" || lower == "/k")
                return deny("forbid_shell_wrapper",
                            "cmd.exe free-form /c and /k execution is not allowed");
        }
    }
    if (command == "powershell" || command == "pwsh") {
        for (const std::string& argument : arguments) {
            const std::string lower = lowercase(argument);
            if (lower == "-command" || lower == "-c" || lower == "-encodedcommand" ||
                lower == "-ec")
                return deny("forbid_shell_wrapper",
                            "PowerShell free-form/encoded command execution is not allowed");
        }
    }
    static const char* destructive_powershell[] = {
        "remove-item", "clear-content", "remove-itemproperty", "remove-partition",
        "format-volume", "clear-disk", "initialize-disk", "stop-computer",
        "restart-computer", "remove-computer"};
    for (const char* name : destructive_powershell) {
        if (command == name)
            return ask("ask_on_destructive_powershell",
                       "refusing destructive PowerShell command: " + command);
    }

    // File rm and empty-dir rmdir/rm -r are classified later. Ask here only for
    // database-looking operands. Non-empty tree deletes are prompted in the run
    // tool after the path is resolved (Smart/Confirm; headless Deny).
    if (command == "rm") {
        for (std::size_t i = 1; i < arguments.size(); ++i) {
            if (!arguments[i].empty() && arguments[i].front() == '-') continue;
            if (is_database_name(arguments[i]))
                return ask("ask_on_database_delete",
                           "refusing to delete database file via shell: " + arguments[i]);
        }
    }

    if (command == "find") {
        for (const std::string& arg : arguments) {
            if (arg == "-delete" || arg.rfind("-exec", 0) == 0 || arg.rfind("-ok", 0) == 0)
                return deny("forbid_find_destructive",
                            "find -delete/-exec/-ok is not allowed");
        }
    }

    if (command == "git") {
        if (arguments.size() < 2) return {};
        const std::size_t sub_index = git_subcommand_index(arguments);
        if (sub_index >= arguments.size()) return {};
        const std::string sub = lowercase(arguments[sub_index]);
        if (sub == "reset") {
            for (const std::string& arg : arguments)
                if (arg == "--hard" || arg == "--merge")
                    return ask("ask_on_destructive_git",
                               "refusing destructive git reset (" + arg + ")");
        }
        if (sub == "clean") {
            for (const std::string& arg : arguments) {
                if (arg == "-f" || arg == "-fd" || arg == "-fdx" || arg == "-fx" ||
                    arg == "-xffd" || arg.find('f') != std::string::npos)
                    return ask("ask_on_destructive_git",
                               "refusing destructive git clean");
            }
        }
        if (sub == "push") {
            for (const std::string& arg : arguments)
                if (arg == "--force" || arg == "-f" || arg == "--force-with-lease")
                    return ask("ask_on_destructive_git",
                               "refusing force git push");
        }
        if (sub == "branch" || sub == "tag") {
            for (const std::string& arg : arguments)
                if (arg == "-D" || arg == "--delete" || arg == "-d")
                    return ask("ask_on_destructive_git",
                               "refusing git branch/tag delete via run_command");
        }
        if (sub == "checkout" || sub == "restore" || sub == "switch") {
            for (const std::string& arg : arguments)
                if (arg == "--force" || arg == "-f" || arg == "--ours" || arg == "--theirs")
                    return ask("ask_on_destructive_git",
                               "refusing forced git " + sub);
        }
    }

    if (command == "sqlite3" || command == "sqlite" || command == "psql" || command == "mysql") {
        for (std::size_t i = 1; i < arguments.size(); ++i) {
            if (looks_like_sql_destructive(arguments[i]))
                return ask("ask_on_destructive_sql",
                           "refusing destructive SQL via " + command);
            if (is_database_name(arguments[i]) && i + 1 < arguments.size() &&
                looks_like_sql_destructive(arguments[i + 1]))
                return ask("ask_on_destructive_sql",
                           "refusing destructive SQL against " + arguments[i]);
        }
    }

    // Shell *interpreters* used as free-form code runners (sh -c / bash -c) are
    // high risk. Running a concrete script file is normal project work and is
    // allowed: e.g. `bash server.sh start`, `sh ./scripts/setup.sh`.
    if (command == "sh" || command == "bash" || command == "zsh" || command == "dash" ||
        command == "csh" || command == "tcsh" || command == "fish" || command == "ksh" ||
        command == "busybox") {
        bool freeform = false;
        bool has_script = false;
        for (std::size_t i = 1; i < arguments.size(); ++i) {
            const std::string& arg = arguments[i];
            if (arg == "-c" || arg == "--command" || arg == "-s" ||
                (arg.rfind("-c", 0) == 0 && arg.size() > 2)) {
                // bash -ce '…' / -c'…' and plain -c are free-form code.
                freeform = true;
                break;
            }
            if (arg == "--") {
                if (i + 1 < arguments.size()) has_script = true;
                break;
            }
            // Skip shell flags; first non-option is the script path/name.
            if (!arg.empty() && arg.front() == '-') continue;
            has_script = true;
            break;
        }
        if (freeform || !has_script)
            return deny("forbid_shell_wrapper",
                        "shell free-form code is not allowed (no sh -c / bash -c); "
                        "run a workspace script path instead (example: ./server.sh start "
                        "or bash server.sh start)");
        // Script-file form: allow; permission mode / Guard Ask still apply at the
        // tool layer for Smart/Confirm.
        return {};
    }
    if (command == "sudo" || command == "doas" || command == "su" || command == "pkexec" ||
        command == "runuser")
        return deny("forbid_privilege_escalation", "privilege escalation commands are not allowed");

    // dd / mkfs / shred
    if (command == "dd" || command == "mkfs" || command == "mkfs.ext4" || command == "mkfs.xfs" ||
        command == "mkfs.btrfs" || command == "shred" || command == "wipefs" ||
        command == "fdisk" || command == "parted" || command == "sfdisk")
        return deny("forbid_disk_destroy", "disk/device destructive commands are not allowed");

    // Host power / service control outside the workspace coding model.
    if (command == "reboot" || command == "poweroff" || command == "halt" ||
        command == "shutdown" || command == "systemctl" || command == "service" ||
        command == "init")
        return deny("forbid_host_control", "host power/service control is not allowed via run_command");

    // System package managers and global environment mutation.
    if (command == "apt" || command == "apt-get" || command == "aptitude" || command == "dpkg" ||
        command == "yum" || command == "dnf" || command == "pacman" || command == "zypper" ||
        command == "apk" || command == "snap" || command == "flatpak")
        return deny("forbid_system_package_manager",
                    "system package managers are not allowed via run_command");

    // Network listeners / remote shells (workspace coding should use fetch_url/web_search).
    if (command == "nc" || command == "ncat" || command == "netcat" || command == "socat" ||
        command == "ssh" || command == "scp" || command == "sftp" || command == "telnet" ||
        command == "rsh" || command == "ftp")
        return deny("forbid_remote_shell",
                    "remote shell / listener tools are not allowed via run_command");

    // Language package install/publish (environment mutation). Prefer project-local tooling.
    if (command == "npm" || command == "npx" || command == "yarn" || command == "pnpm") {
        if (arguments.size() >= 2) {
            const std::string sub = lowercase(arguments[1]);
            if (sub == "publish" || sub == "add" || sub == "install" || sub == "i" ||
                sub == "uninstall" || sub == "update" || sub == "upgrade" || sub == "ci")
                return deny("forbid_package_env_mutation",
                            command + " " + sub +
                                " is not allowed via run_command (install/publish changes "
                                "environment)");
        }
    }
    if (command == "cargo" && arguments.size() >= 2) {
        const std::string sub = lowercase(arguments[1]);
        if (sub == "publish" || sub == "install" || sub == "login")
            return deny("forbid_package_env_mutation",
                        "cargo " + sub + " is not allowed via run_command");
    }
    if (command == "go" && arguments.size() >= 2) {
        const std::string sub = lowercase(arguments[1]);
        if (sub == "get" || sub == "install")
            return deny("forbid_package_env_mutation",
                        "go " + sub + " is not allowed via run_command");
    }
    if (command == "pip" || command == "pip3" || command == "pipx") {
        if (arguments.size() >= 2) {
            const std::string sub = lowercase(arguments[1]);
            if (sub == "install" || sub == "uninstall" || sub == "download")
                return deny("forbid_package_env_mutation",
                            command + " " + sub + " is not allowed via run_command");
        }
    }
    if (command == "dotnet" && arguments.size() >= 3) {
        const std::string group = lowercase(arguments[1]);
        const std::string action = lowercase(arguments[2]);
        if (group == "nuget" && (action == "push" || action == "delete"))
            return deny("forbid_package_env_mutation",
                        "dotnet nuget " + action +
                            " is not allowed via run_command (publish changes "
                            "environment)");
        if (group == "tool" &&
            (action == "install" || action == "update" || action == "uninstall")) {
            for (std::size_t i = 3; i < arguments.size(); ++i) {
                if (arguments[i] == "-g" || arguments[i] == "--global")
                    return deny("forbid_package_env_mutation",
                                "dotnet tool " + action +
                                    " --global is not allowed via run_command");
            }
        }
    }

    if (command == "nohup" || command == "setsid" || command == "disown")
        return deny("forbid_detach",
                    "do not detach with nohup/setsid; use run background=true for "
                    "long-running project scripts");

    if (is_python_interpreter(command)) {
        const char* inline_fix =
            "write scripts/ainiux/NAME and run python3 scripts/ainiux/NAME [args]";
        bool seen_program = false;
        for (std::size_t i = 1; i < arguments.size(); ++i) {
            const std::string& arg = arguments[i];
            if (seen_program) break;
            std::string payload;
            std::string module;
            if (arg == "-c" || arg == "--command" || arg.rfind("--command=", 0) == 0) {
                if (arg.rfind("--command=", 0) == 0) {
                    payload = arg.substr(10);
                } else if (i + 1 >= arguments.size()) {
                    return deny("forbid_inline_python",
                                std::string("python -c requires a program; ") + inline_fix);
                } else {
                    payload = arguments[++i];
                }
                seen_program = true;
            } else if (arg.size() > 2 && arg.compare(0, 2, "-c") == 0 && arg[1] == 'c') {
                payload = arg.substr(2);
                seen_program = true;
            } else if (arg == "-m" || arg == "--module" || arg.rfind("--module=", 0) == 0) {
                if (arg.rfind("--module=", 0) == 0) {
                    module = arg.substr(9);
                } else if (i + 1 >= arguments.size()) {
                    return deny("forbid_package_env_mutation",
                                "python -m requires a module name");
                } else {
                    module = arguments[++i];
                }
                seen_program = true;
            } else if (arg.size() > 2 && arg.compare(0, 2, "-m") == 0 && arg[1] == 'm' &&
                       arg[2] != '-') {
                module = arg.substr(2);
                seen_program = true;
            } else if (arg == "-") {
                return deny("forbid_inline_python",
                            std::string("python stdin programs are not allowed; ") +
                                inline_fix);
            } else if (!arg.empty() && arg.front() != '-') {
                seen_program = true;
                continue;
            } else {
                continue;
            }
            if (!module.empty() && is_python_env_mutation_module(module))
                return deny("forbid_package_env_mutation",
                            "python -m " + module +
                                " is not allowed via run_command (install/publish changes "
                                "environment)");
            if (!payload.empty() &&
                (payload.find("scripts/ainiux") != std::string::npos ||
                 payload.find(".ainiux-pr/scripts") != std::string::npos))
                return deny("forbid_inline_python",
                            std::string("do not wrap a project script in python -c; ") +
                                inline_fix);
        }
    }

    return {};
}

GuardResult finalize_guard_for_headless(GuardResult result) {
    if (result.decision == GuardDecision::Ask) {
        result.decision = GuardDecision::Deny;
        if (!result.message.empty())
            result.message +=
                " (headless agent denies Ask decisions; use interactive agent for approval)";
        else
            result.message =
                "headless agent denies Ask decisions; use interactive agent for approval";
    }
    return result;
}

}  // namespace ainiux::agent
