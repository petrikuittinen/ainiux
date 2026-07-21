#include "agent/command_guard.hpp"

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

bool has_flag(const std::vector<std::string>& args, const char* flag) {
    for (const std::string& arg : args) {
        if (arg == flag) return true;
    }
    return false;
}

// Combined short options like -rf, -fr, -rR.
bool has_combined_recursive_force(const std::vector<std::string>& args) {
    for (const std::string& arg : args) {
        if (arg.size() < 2 || arg[0] != '-' || arg[1] == '-') continue;
        bool has_r = false;
        bool has_f = false;
        for (std::size_t i = 1; i < arg.size(); ++i) {
            if (arg[i] == 'r' || arg[i] == 'R') has_r = true;
            if (arg[i] == 'f') has_f = true;
        }
        if (has_r && has_f) return true;
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

GuardResult evaluate_command_guard(const std::vector<std::string>& arguments) {
    if (arguments.empty()) return {};
    const std::string command = lowercase(arguments.front());

    // Recursive force delete variants.
    if (command == "rm") {
        const bool recursive =
            has_flag(arguments, "-r") || has_flag(arguments, "-R") || has_flag(arguments, "--recursive") ||
            has_combined_recursive_force(arguments);
        const bool force =
            has_flag(arguments, "-f") || has_flag(arguments, "--force") ||
            has_combined_recursive_force(arguments);
        if (recursive && force)
            return ask("ask_on_recursive_force_delete",
                       "refusing recursive force delete (rm -rf/-fr/…); use remove tool for "
                       "workspace paths or confirm interactively later");
        if (recursive)
            return ask("ask_on_recursive_delete",
                       "refusing recursive rm; use the remove tool with recursive=true for "
                       "workspace directories");
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
        const std::string sub = lowercase(arguments[1]);
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

    // Shell wrappers are high risk (arbitrary composition).
    if (command == "sh" || command == "bash" || command == "zsh" || command == "dash" ||
        command == "csh" || command == "tcsh" || command == "fish" || command == "ksh") {
        return deny("forbid_shell_wrapper",
                    "interactive/shell wrappers are not allowed; pass a direct allowlisted command");
    }
    if (command == "sudo" || command == "doas" || command == "su")
        return deny("forbid_privilege_escalation", "privilege escalation commands are not allowed");

    // dd / mkfs / shred
    if (command == "dd" || command == "mkfs" || command == "mkfs.ext4" || command == "shred" ||
        command == "wipefs")
        return deny("forbid_disk_destroy", "disk/device destructive commands are not allowed");

    return {};
}

GuardResult evaluate_command_guard_line(const std::string& command_line) {
    std::vector<std::string> args;
    std::string current;
    bool in_token = false;
    for (char ch : command_line) {
        if (ch == ' ' || ch == '\t') {
            if (in_token) {
                args.push_back(std::move(current));
                current.clear();
                in_token = false;
            }
            continue;
        }
        current.push_back(ch);
        in_token = true;
    }
    if (in_token) args.push_back(std::move(current));
    return evaluate_command_guard(args);
}

GuardResult finalize_guard_for_headless(GuardResult result) {
    if (result.decision == GuardDecision::Ask) {
        result.decision = GuardDecision::Deny;
        if (!result.message.empty())
            result.message += " (headless agent denies Ask decisions; no approval UI yet)";
    }
    return result;
}

}  // namespace ainiux::agent
