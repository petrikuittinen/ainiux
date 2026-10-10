#include "agent/read_only_command.hpp"

#include "agent/command_guard.hpp"

#include <cctype>
#include <cstddef>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace ainiux::agent {
namespace {

using StringSet = std::set<std::string>;

ReadOnlyCommandAssessment reject(const std::string& reason) {
    ReadOnlyCommandAssessment result;
    result.reason = reason;
    return result;
}

ReadOnlyCommandAssessment accept(std::vector<std::string> paths = {}) {
    ReadOnlyCommandAssessment result;
    result.vetted = true;
    result.path_operands = std::move(paths);
    return result;
}

bool is_short_cluster(const std::string& arg, const std::string& allowed) {
    if (arg.size() < 2 || arg[0] != '-' || arg[1] == '-') return false;
    for (std::size_t i = 1; i < arg.size(); ++i)
        if (allowed.find(arg[i]) == std::string::npos) return false;
    return true;
}

bool exact_or_assignment(const std::string& arg, const StringSet& options,
                         std::string* value = nullptr) {
    if (options.find(arg) != options.end()) return true;
    const std::size_t equal = arg.find('=');
    if (equal == std::string::npos ||
        options.find(arg.substr(0, equal)) == options.end())
        return false;
    if (value != nullptr) *value = arg.substr(equal + 1);
    return true;
}

bool take_value(const std::vector<std::string>& args, std::size_t& index,
                const StringSet& long_options, const std::string& short_options,
                std::string& value) {
    const std::string& arg = args[index];
    const std::size_t equal = arg.find('=');
    if (equal != std::string::npos &&
        long_options.find(arg.substr(0, equal)) != long_options.end()) {
        value = arg.substr(equal + 1);
        return !value.empty();
    }
    if (long_options.find(arg) != long_options.end() ||
        (arg.size() == 2 && arg[0] == '-' &&
         short_options.find(arg[1]) != std::string::npos)) {
        if (++index >= args.size()) return false;
        value = args[index];
        return !value.empty();
    }
    if (arg.size() > 2 && arg[0] == '-' && arg[1] != '-' &&
        short_options.find(arg[1]) != std::string::npos) {
        value = arg.substr(2);
        return true;
    }
    return false;
}

ReadOnlyCommandAssessment simple_file_command(
    const std::vector<std::string>& args, const std::string& short_flags,
    const StringSet& long_flags, const std::string& short_value_flags = {},
    const StringSet& long_value_flags = {}, const StringSet& rejected = {},
    const StringSet& path_value_options = {}) {
    std::vector<std::string> paths;
    bool operands = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--") {
            operands = true;
            continue;
        }
        if (!operands && rejected.find(arg) != rejected.end())
            return reject("mutating or unsafe option: " + arg);
        std::string value;
        if (!operands &&
            take_value(args, i, long_value_flags, short_value_flags, value)) {
            const std::string option =
                arg.rfind("--", 0) == 0 ? arg.substr(0, arg.find('=')) : arg.substr(0, 2);
            if (path_value_options.find(option) != path_value_options.end())
                paths.push_back(value);
            continue;
        }
        if (!operands && (long_flags.find(arg) != long_flags.end() ||
                          is_short_cluster(arg, short_flags)))
            continue;
        if (!operands && !arg.empty() && arg[0] == '-')
            return reject("unknown option: " + arg);
        paths.push_back(arg);
    }
    return accept(std::move(paths));
}

ReadOnlyCommandAssessment assess_ls(const std::vector<std::string>& args) {
    static const StringSet flags = {
        "--all", "--almost-all", "--author", "--classify", "--directory",
        "--file-type", "--group-directories-first", "--human-readable",
        "--inode", "--literal", "--no-group", "--numeric-uid-gid",
        "--quote-name", "--reverse", "--size", "--time-style=full-iso"};
    static const StringSet values = {
        "--block-size", "--color", "--format", "--hide", "--indicator-style",
        "--quoting-style", "--sort", "--time", "--time-style", "--tabsize",
        "--width"};
    static const StringSet rejected = {
        "-R", "--recursive", "-L", "--dereference",
        "--dereference-command-line", "--dereference-command-line-symlink-to-dir"};
    return simple_file_command(args, "1AaBCDFGHNQSTUXZabcdfghiklmnopqrstuwx",
                               flags, "Tw", values, rejected);
}

ReadOnlyCommandAssessment assess_head_tail(const std::vector<std::string>& args,
                                           bool tail) {
    static const StringSet flags = {"--quiet", "--silent", "--verbose",
                                    "--zero-terminated"};
    static const StringSet values = {"--bytes", "--lines"};
    static const StringSet tail_rejected = {
        "-f", "-F", "--follow", "--retry", "--pid", "--max-unchanged-stats",
        "--sleep-interval"};
    return simple_file_command(args, tail ? "qvz" : "qvz", flags, "cn", values,
                               tail ? tail_rejected : StringSet{});
}

ReadOnlyCommandAssessment assess_grep(const std::vector<std::string>& args) {
    static const StringSet flags = {
        "--basic-regexp", "--extended-regexp", "--fixed-strings", "--perl-regexp",
        "--ignore-case", "--no-ignore-case", "--word-regexp", "--line-regexp",
        "--null-data", "--invert-match", "--version", "--help", "--line-number",
        "--with-filename", "--no-filename", "--label", "--only-matching",
        "--quiet", "--silent", "--binary-files=without-match", "--text",
        "--binary", "--directories=read", "--directories=recurse",
        "--devices=skip", "--color=never", "--colour=never", "--exclude-dir=.git"};
    static const StringSet values = {
        "--regexp", "--file", "--max-count", "--byte-offset", "--label",
        "--binary-files", "--directories", "--devices", "--include", "--exclude",
        "--exclude-from", "--exclude-dir", "--color", "--colour",
        "--before-context", "--after-context", "--context", "--group-separator"};
    static const StringSet path_values = {"--file", "--exclude-from"};
    std::vector<std::string> paths;
    bool operands = false;
    bool pattern_seen = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--") {
            operands = true;
            continue;
        }
        if (!operands && (arg == "-R" || arg == "--dereference-recursive"))
            return reject("recursive symlink following is not read-only-vetted");
        std::string value;
        if (!operands && take_value(args, i, values, "efmABC", value)) {
            const std::string option =
                arg.rfind("--", 0) == 0 ? arg.substr(0, arg.find('=')) : arg.substr(0, 2);
            if (option == "-e" || option == "--regexp") pattern_seen = true;
            if (path_values.find(option) != path_values.end()) paths.push_back(value);
            continue;
        }
        if (!operands && (flags.find(arg) != flags.end() ||
                          is_short_cluster(arg, "EFGPivwxznHhoqsaIbr")))
            continue;
        if (!operands && !arg.empty() && arg[0] == '-')
            return reject("unknown grep option: " + arg);
        if (!pattern_seen) {
            pattern_seen = true;
            continue;
        }
        paths.push_back(arg);
    }
    return pattern_seen ? accept(std::move(paths))
                        : reject("grep requires a pattern");
}

ReadOnlyCommandAssessment assess_rg(const std::vector<std::string>& args) {
    static const StringSet rejected = {
        "--pre", "--pre-glob", "--search-zip", "-z", "--follow", "-L",
        "--files", "--type-add", "--type-clear", "--hostname-bin",
        "--generate", "--pcre2-version"};
    static const StringSet flags = {
        "--fixed-strings", "--ignore-case", "--case-sensitive", "--smart-case",
        "--word-regexp", "--line-regexp", "--invert-match", "--line-number",
        "--no-line-number", "--with-filename", "--no-filename", "--no-heading",
        "--heading", "--only-matching", "--quiet", "--text", "--hidden",
        "--no-ignore", "--no-ignore-vcs", "--no-messages", "--stats",
        "--count", "--count-matches", "--files-with-matches",
        "--files-without-match", "--color=never", "--json", "--crlf",
        "--multiline", "--multiline-dotall", "--one-file-system"};
    static const StringSet values = {
        "--regexp", "--file", "--glob", "--iglob", "--type", "--type-not",
        "--max-count", "--max-depth", "--max-filesize", "--context",
        "--before-context", "--after-context", "--context-separator",
        "--field-context-separator", "--field-match-separator", "--sort",
        "--sortr", "--threads", "--encoding", "--engine", "--color"};
    std::vector<std::string> paths;
    bool pattern_seen = false;
    bool operands = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--") {
            operands = true;
            continue;
        }
        std::string rejected_value;
        if (!operands &&
            (rejected.find(arg) != rejected.end() ||
             exact_or_assignment(arg, rejected, &rejected_value)))
            return reject("unsafe rg option: " + arg);
        std::string value;
        if (!operands && take_value(args, i, values, "efgtrmABC", value)) {
            const std::string option =
                arg.rfind("--", 0) == 0 ? arg.substr(0, arg.find('=')) : arg.substr(0, 2);
            if (option == "-e" || option == "--regexp") pattern_seen = true;
            if (option == "-f" || option == "--file") paths.push_back(value);
            continue;
        }
        if (!operands && (flags.find(arg) != flags.end() ||
                          is_short_cluster(arg, "FivwxnHhoqscIlupU")))
            continue;
        if (!operands && !arg.empty() && arg[0] == '-')
            return reject("unknown rg option: " + arg);
        if (!pattern_seen) {
            pattern_seen = true;
            continue;
        }
        paths.push_back(arg);
    }
    return pattern_seen ? accept(std::move(paths)) : reject("rg requires a pattern");
}

ReadOnlyCommandAssessment assess_find(const std::vector<std::string>& args) {
    static const StringSet no_value = {
        "-print", "-print0", "-ls", "-true", "-false", "-empty", "-readable",
        "-writable", "-executable", "-delete", "-quit", "-mount", "-xdev",
        "-depth", "-ignore_readdir_race", "-noignore_readdir_race"};
    static const StringSet one_value = {
        "-name", "-iname", "-path", "-ipath", "-regex", "-iregex", "-type",
        "-xtype", "-size", "-links", "-inum", "-uid", "-gid", "-user", "-group",
        "-perm", "-mtime", "-mmin", "-atime", "-amin", "-ctime", "-cmin",
        "-newer", "-newermt", "-maxdepth", "-mindepth", "-printf", "-fstype"};
    std::vector<std::string> paths;
    std::size_t i = 1;
    if (i < args.size() && (args[i] == "-H" || args[i] == "-L"))
        return reject("find symlink mode is not vetted");
    if (i < args.size() && args[i] == "-P") ++i;
    while (i < args.size() && (args[i].empty() || args[i][0] != '-' ||
                              args[i] == ".")) {
        paths.push_back(args[i++]);
    }
    for (; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "-delete" || arg.rfind("-exec", 0) == 0 ||
            arg.rfind("-ok", 0) == 0 || arg.rfind("-fprint", 0) == 0 ||
            arg.rfind("-fprintf", 0) == 0 || arg == "-fls")
            return reject("find action can mutate, execute, or write a file");
        if (arg == "!" || arg == "-not" || arg == "-a" || arg == "-and" ||
            arg == "-o" || arg == "-or" || arg == "(" || arg == ")")
            continue;
        if (no_value.find(arg) != no_value.end()) continue;
        if (one_value.find(arg) != one_value.end()) {
            if (++i >= args.size()) return reject("find option is missing its value");
            if (arg == "-newer") paths.push_back(args[i]);
            continue;
        }
        return reject("unknown find expression: " + arg);
    }
    return accept(std::move(paths));
}

ReadOnlyCommandAssessment assess_checksums(const std::vector<std::string>& args) {
    static const StringSet flags = {"--binary", "--text", "--tag", "--zero",
                                    "--help", "--version"};
    static const StringSet rejected = {
        "-c", "--check", "--ignore-missing", "--quiet", "--status", "--strict",
        "-w", "--warn"};
    return simple_file_command(args, "btzl", flags, "l",
                               {"--length", "--algorithm"}, rejected);
}

ReadOnlyCommandAssessment assess_passive(const std::vector<std::string>& args) {
    const std::string& command = args[0];
    static const std::set<std::string> no_args = {
        "whoami", "uptime", "nproc", "arch"};
    if (no_args.find(command) != no_args.end())
        return args.size() == 1 ? accept() : reject(command + " takes no vetted operands");

    if (command == "ps") {
        static const StringSet values = {
            "--format", "--pid", "--ppid", "--quick-pid", "--command", "--tty",
            "--user", "--User", "--group", "--Group", "--sort", "--cols",
            "--columns", "--width", "--rows", "--lines"};
        for (std::size_t i = 1; i < args.size(); ++i) {
            const std::string& arg = args[i];
            std::string value;
            if (take_value(args, i, values, "opqCtUuGg", value)) continue;
            if (arg.size() > 2 && arg[0] == '-' && arg[1] != '-' &&
                arg.back() == 'o') {
                if (++i >= args.size()) return reject("ps -o is missing its format");
                continue;
            }
            if ((!arg.empty() && arg[0] != '-' &&
                 arg.find_first_not_of("aAdefHjlNrsTuvwxZ") == std::string::npos) ||
                is_short_cluster(arg, "aAdefHjlNrsTuvwxZ"))
                continue;
            if (arg == "--all" || arg == "--no-headers" || arg == "--headers" ||
                arg == "--help" || arg == "--version")
                continue;
            return reject("unknown ps option or operand: " + arg);
        }
        return accept();
    }

    if (command == "free") {
        static const StringSet values = {"--count", "--seconds"};
        for (std::size_t i = 1; i < args.size(); ++i) {
            std::string value;
            if (take_value(args, i, values, "cs", value)) continue;
            if (is_short_cluster(args[i], "bkmghtwV") ||
                args[i] == "--bytes" || args[i] == "--kibi" ||
                args[i] == "--mebi" || args[i] == "--gibi" ||
                args[i] == "--giga" || args[i] == "--tera" ||
                args[i] == "--human" || args[i] == "--wide" ||
                args[i] == "--help" || args[i] == "--version")
                continue;
            return reject("unknown free option: " + args[i]);
        }
        return accept();
    }

    static const std::map<std::string, std::string> short_flags = {
        {"id", "GgnruZz"}, {"who", "abdlmprstTuHq"},
        {"uname", "asnrvmpio"}, {"lsb_release", "asdircvh"},
        {"hostname", "fdsiaIy"}};
    const auto found = short_flags.find(command);
    if (found == short_flags.end()) return reject("not a passive snapshot command");
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--") return reject("unexpected passive-command operand");
        if (arg.rfind("--", 0) == 0) {
            static const StringSet longs = {
                "--all", "--no-headers", "--headers", "--human-readable",
                "--si", "--inodes", "--local", "--portability", "--total",
                "--help", "--version", "--all-architectures", "--kernel-name",
                "--nodename", "--kernel-release", "--kernel-version", "--machine",
                "--processor", "--hardware-platform", "--operating-system",
                "--short", "--fqdn", "--domain", "--ip-address"};
            if (longs.find(arg) == longs.end()) return reject("unknown option: " + arg);
        } else if (command == "ps" && !arg.empty() && arg[0] != '-' &&
                   arg.find_first_not_of(found->second) == std::string::npos) {
            continue;
        } else if (!is_short_cluster(arg, found->second)) {
            return reject("unknown option or operand: " + arg);
        }
    }
    return accept();
}

ReadOnlyCommandAssessment assess_date(const std::vector<std::string>& args) {
    std::vector<std::string> paths;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "-s" || arg == "--set" || arg.rfind("--set=", 0) == 0)
            return reject("date clock-setting option is not read-only");
        std::string value;
        static const StringSet values = {"--date", "--file", "--reference",
                                         "--iso-8601", "--rfc-3339"};
        if (take_value(args, i, values, "dfr", value)) {
            const std::string option =
                arg.rfind("--", 0) == 0 ? arg.substr(0, arg.find('=')) : arg.substr(0, 2);
            if (option == "-f" || option == "--file" || option == "-r" ||
                option == "--reference")
                paths.push_back(value);
            continue;
        }
        if (arg == "-u" || arg == "--utc" || arg == "--universal" ||
            arg == "--debug" || arg == "--help" || arg == "--version" ||
            (!arg.empty() && arg[0] == '+'))
            continue;
        return reject("unknown date option or operand: " + arg);
    }
    return accept(std::move(paths));
}

ReadOnlyCommandAssessment assess_ip(const std::vector<std::string>& args) {
    static const StringSet global = {
        "-brief", "-details", "-statistics", "-human", "-iec", "-json",
        "-pretty", "-oneline", "-resolve", "-color=never", "-br", "-d", "-s",
        "-h", "-j", "-p", "-o", "-r", "-4", "-6", "-0"};
    static const StringSet objects = {
        "address", "addr", "link", "route", "rule", "neighbour", "neighbor",
        "neigh", "ntable", "tunnel", "tuntap", "maddress", "mroute", "monitor",
        "netns", "l2tp", "tcp_metrics", "token", "macsec", "xfrm"};
    std::size_t i = 1;
    while (i < args.size() && global.find(args[i]) != global.end()) ++i;
    if (i >= args.size() || objects.find(args[i++]) == objects.end())
        return reject("ip requires a vetted query object");
    if (i >= args.size() || (args[i] != "show" && args[i] != "list"))
        return reject("ip is limited to show/list queries");
    ++i;
    static const StringSet query_words = {
        "dev", "type", "scope", "table", "vrf", "to", "from", "via", "proto",
        "master", "nomaster", "up", "dynamic", "permanent", "nud", "label",
        "root", "match", "exact"};
    for (; i < args.size(); ++i) {
        if (args[i].empty() || args[i][0] == '-' ||
            query_words.find(args[i]) == query_words.end())
            return reject("unknown ip query shape: " + args[i]);
        // Query keywords that take a value consume one opaque non-option value.
        if (args[i] != "nomaster" && args[i] != "up" &&
            args[i] != "dynamic" && args[i] != "permanent") {
            if (++i >= args.size() || args[i].empty() || args[i][0] == '-')
                return reject("ip query keyword is missing a value");
        }
    }
    return accept();
}

std::string ascii_lower_copy(std::string text) {
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return text;
}

void split_git_config_assignment(const std::string& assignment, std::string& key,
                                 std::string& value) {
    const std::size_t equal = assignment.find('=');
    if (equal == std::string::npos) {
        key = ascii_lower_copy(assignment);
        value.clear();
        return;
    }
    key = ascii_lower_copy(assignment.substr(0, equal));
    value = assignment.substr(equal + 1);
}

bool git_closed_value(const std::string& value, std::initializer_list<const char*> allowed) {
    for (const char* item : allowed)
        if (value == item) return true;
    return false;
}

bool config_assignment_is_dangerous(const std::string& assignment) {
    std::string key;
    std::string value;
    split_git_config_assignment(assignment, key, value);
    if (key.rfind("alias.", 0) == 0) return true;
    if (key == "core.hookspath" || key == "core.sshcommand" ||
        key == "core.gitproxy" || key == "core.editor" || key == "core.askpass" ||
        key == "sequence.editor" || key == "gpg.program" ||
        key == "credential.helper")
        return true;
    if (key.rfind("filter.", 0) == 0 || key.rfind("difftool.", 0) == 0 ||
        key.rfind("mergetool.", 0) == 0)
        return true;
    if (key == "diff.external" && !value.empty()) return true;
    if ((key == "core.pager" || key.rfind("pager.", 0) == 0) &&
        value.find_first_of("/\\") != std::string::npos)
        return true;
    return false;
}

bool config_assignment_is_auto_allowed(const std::string& assignment) {
    std::string key;
    std::string value;
    split_git_config_assignment(assignment, key, value);
    if (config_assignment_is_dangerous(assignment)) return false;
    if (key == "core.pager" || key.rfind("pager.", 0) == 0)
        return git_closed_value(value, {"", "cat", "true", "false", "never"});
    if (key == "diff.external") return value.empty();
    if (key == "color.ui" || key == "color.diff" || key == "color.status")
        return git_closed_value(value, {"", "never", "false", "true", "auto"});
    if (key.rfind("advice.", 0) == 0)
        return git_closed_value(value, {"", "true", "false"});
    if (key == "status.short" || key == "status.branch")
        return git_closed_value(value, {"", "true", "false"});
    return false;
}

bool take_git_config(const std::vector<std::string>& args, std::size_t i,
                     std::string& assignment, std::size_t& next) {
    if (i >= args.size()) return false;
    const std::string& argument = args[i];
    if (argument == "-c") {
        if (i + 1 >= args.size() || args[i + 1].empty()) return false;
        assignment = args[i + 1];
        next = i + 2;
        return true;
    }
    if (argument.size() > 2 && argument[0] == '-' && argument[1] == 'c' &&
        argument[2] != '-') {
        assignment = argument.substr(2);
        next = i + 1;
        return true;
    }
    return false;
}

bool git_pager_global_flag(const std::string& argument) {
    return argument == "-p" || argument == "--paginate" || argument == "-P" ||
           argument == "--no-pager" || argument == "--no-replace-objects" ||
           argument == "--no-lazy-fetch" || argument == "--no-advice" ||
           argument == "--bare" || argument == "--literal-pathspecs" ||
           argument == "--glob-pathspecs" || argument == "--noglob-pathspecs" ||
           argument == "--icase-pathspecs" || argument == "--no-optional-locks";
}

bool git_path_override_equals(const std::string& argument) {
    return argument.rfind("--git-dir=", 0) == 0 ||
           argument.rfind("--work-tree=", 0) == 0 ||
           argument.rfind("--namespace=", 0) == 0 ||
           argument.rfind("--super-prefix=", 0) == 0 ||
           argument.rfind("--attr-source=", 0) == 0 ||
           argument.rfind("--config-env=", 0) == 0 ||
           argument.rfind("--exec-path=", 0) == 0 ||
           argument.rfind("--list-cmds=", 0) == 0;
}

bool git_path_override_separate(const std::string& argument) {
    return argument == "--git-dir" || argument == "--work-tree" ||
           argument == "--namespace" || argument == "--super-prefix" ||
           argument == "--attr-source" || argument == "--config-env";
}

bool git_info_command(const std::string& argument) {
    return argument == "--version" || argument == "--help" || argument == "-h" ||
           argument == "--html-path" || argument == "--man-path" ||
           argument == "--info-path";
}

std::size_t find_git_subcommand(const std::vector<std::string>& args) {
    if (args.size() < 2) return args.size();
    std::size_t i = 1;
    while (i < args.size()) {
        const std::string& argument = args[i];
        if (argument == "--")
            return i + 1 < args.size() ? i + 1 : args.size();
        std::string ignored;
        std::size_t next = i;
        if (take_git_config(args, i, ignored, next)) {
            i = next;
            continue;
        }
        if (argument == "-C") {
            i += (i + 1 < args.size()) ? 2 : 1;
            continue;
        }
        if (argument.size() > 2 && argument[0] == '-' && argument[1] == 'C' &&
            argument[2] != '-') {
            ++i;
            continue;
        }
        if (git_path_override_equals(argument)) {
            ++i;
            continue;
        }
        if (argument == "--exec-path") {
            if (i + 1 >= args.size() || args[i + 1].empty() || args[i + 1][0] == '-')
                return i;
            i += 2;
            continue;
        }
        if (git_path_override_separate(argument)) {
            i += (i + 1 < args.size()) ? 2 : 1;
            continue;
        }
        if (git_pager_global_flag(argument)) {
            ++i;
            continue;
        }
        if (git_info_command(argument)) return i;
        if (!argument.empty() && argument[0] == '-') {
            ++i;
            continue;
        }
        return i;
    }
    return args.size();
}

bool git_write_or_exec_option(const std::string& argument) {
    if (argument == "--output" || argument.rfind("--output=", 0) == 0) return true;
    if (argument == "-O" ||
        (argument.size() > 2 && argument.rfind("-O", 0) == 0 && argument[2] != '-'))
        return true;
    if (argument == "--ext-diff" || argument == "--textconv" ||
        argument == "--binary" || argument == "--no-index")
        return true;
    if (argument == "--exec" || argument.rfind("--exec=", 0) == 0) return true;
    if (argument == "--show-signature") return true;
    return false;
}

ReadOnlyCommandAssessment assess_git_globals(const std::vector<std::string>& args,
                                             std::size_t sub_index,
                                             std::vector<std::string>& paths) {
    for (std::size_t i = 1; i < sub_index;) {
        const std::string& argument = args[i];
        if (argument == "--") {
            ++i;
            continue;
        }
        std::string assignment;
        std::size_t next = i;
        if (take_git_config(args, i, assignment, next)) {
            if (!config_assignment_is_auto_allowed(assignment))
                return reject("git -c is not a vetted read-only config override: " +
                              assignment);
            i = next;
            continue;
        }
        if (argument == "-C") {
            if (i + 1 >= sub_index || args[i + 1].empty())
                return reject("git -C requires a path");
            paths.push_back(args[i + 1]);
            i += 2;
            continue;
        }
        if (argument.size() > 2 && argument[0] == '-' && argument[1] == 'C' &&
            argument[2] != '-') {
            paths.push_back(argument.substr(2));
            ++i;
            continue;
        }
        if (git_pager_global_flag(argument)) {
            ++i;
            continue;
        }
        if (git_path_override_equals(argument) || git_path_override_separate(argument) ||
            argument == "--exec-path" || argument.rfind("--exec-path=", 0) == 0)
            return reject("git rejected a path-override option");
        if (!argument.empty() && argument[0] == '-')
            return reject("git global option is not a vetted read-only form: " + argument);
        ++i;
    }
    return accept();
}

ReadOnlyCommandAssessment assess_git_revision_walk(const std::vector<std::string>& args,
                                                   std::size_t sub_index,
                                                   std::vector<std::string> paths) {
    bool after_double_dash = false;
    for (std::size_t i = sub_index + 1; i < args.size(); ++i) {
        const std::string& argument = args[i];
        if (argument == "--") {
            after_double_dash = true;
            continue;
        }
        if (after_double_dash) {
            paths.push_back(argument);
            continue;
        }
        if (argument.empty()) return reject("git rejected an empty argument");
        if (git_write_or_exec_option(argument))
            return reject("git option can write files or invoke external tooling: " +
                          argument);
        if (argument.front() != '-') continue;
    }
    return accept(std::move(paths));
}

ReadOnlyCommandAssessment assess_git(const std::vector<std::string>& args) {
    const std::size_t sub_index = find_git_subcommand(args);
    if (sub_index >= args.size())
        return reject("git requires an allowlisted read-only subcommand");
    const std::string& subcommand = args[sub_index];
    std::vector<std::string> paths;
    const ReadOnlyCommandAssessment globals = assess_git_globals(args, sub_index, paths);
    if (!globals.vetted) return globals;

    if (subcommand == "version" || subcommand == "--version") {
        for (std::size_t i = sub_index + 1; i < args.size(); ++i)
            if (args[i] != "--build-options")
                return reject("git version option is not a vetted read-only form: " +
                              args[i]);
        return accept(std::move(paths));
    }

    if (subcommand == "status") {
        bool after_double_dash = false;
        for (std::size_t i = sub_index + 1; i < args.size(); ++i) {
            const std::string& argument = args[i];
            if (argument == "--") {
                after_double_dash = true;
                continue;
            }
            if (after_double_dash) {
                paths.push_back(argument);
                continue;
            }
            if (argument.empty()) return reject("git status rejected an empty argument");
            if (argument.front() != '-') continue;
            if (argument == "-s" || argument == "--short" ||
                argument == "-b" || argument == "--branch" || argument == "-sb" ||
                argument == "-bs" || argument == "-v" || argument == "-vv" ||
                argument == "--verbose" || argument == "--ignored" ||
                argument == "--porcelain" || argument == "--porcelain=v1" ||
                argument == "--porcelain=v2" ||
                argument == "--untracked-files" || argument == "--untracked-files=no" ||
                argument == "--untracked-files=normal" ||
                argument == "--untracked-files=all" || argument == "-u" ||
                argument == "-uno" || argument == "-unormal" || argument == "-uall" ||
                argument == "--no-ahead-behind" || argument == "--ahead-behind" ||
                argument == "--no-color" || argument == "--color=never") {
                continue;
            }
            return reject("git status option is not a vetted read-only form: " +
                          argument);
        }
        return accept(std::move(paths));
    }

    if (subcommand == "diff") {
        bool after_double_dash = false;
        for (std::size_t i = sub_index + 1; i < args.size(); ++i) {
            const std::string& argument = args[i];
            if (argument == "--") {
                after_double_dash = true;
                continue;
            }
            if (after_double_dash) {
                paths.push_back(argument);
                continue;
            }
            if (argument.empty()) return reject("git diff rejected an empty argument");
            if (argument.front() != '-') continue;
            if (argument == "--cached" || argument == "--staged" || argument == "--stat" ||
                argument == "--numstat" || argument == "--shortstat" ||
                argument == "--name-only" || argument == "--name-status" ||
                argument == "--raw" || argument == "--no-color" ||
                argument == "--color=never" || argument == "--no-ext-diff" ||
                argument == "--no-prefix" || argument == "--quiet" ||
                argument == "-U" || argument.rfind("-U", 0) == 0 ||
                argument == "--unified" || argument.rfind("--unified=", 0) == 0 ||
                argument == "-w" || argument == "--ignore-all-space" ||
                argument == "-b" || argument == "--ignore-space-change") {
                continue;
            }
            if (git_write_or_exec_option(argument)) {
                return reject("git diff option can write files or invoke external tooling: " +
                              argument);
            }
            return reject("git diff option is not a vetted read-only form: " + argument);
        }
        return accept(std::move(paths));
    }

    if (subcommand == "ls-files") {
        bool after_double_dash = false;
        for (std::size_t i = sub_index + 1; i < args.size(); ++i) {
            const std::string& argument = args[i];
            if (argument == "--") {
                after_double_dash = true;
                continue;
            }
            if (after_double_dash) {
                paths.push_back(argument);
                continue;
            }
            if (argument == "-c" || argument == "--cached" ||
                argument == "-o" || argument == "--others" ||
                argument == "-m" || argument == "--modified" ||
                argument == "-d" || argument == "--deleted" ||
                argument == "--full-name" || argument == "--exclude-standard") {
                continue;
            }
            return reject("git ls-files option is not a vetted read-only form: " +
                          argument);
        }
        return accept(std::move(paths));
    }

    if (subcommand == "log" || subcommand == "show" || subcommand == "rev-list" ||
        subcommand == "shortlog" || subcommand == "blame" || subcommand == "ls-tree")
        return assess_git_revision_walk(args, sub_index, std::move(paths));

    if (subcommand == "rev-parse")
        return assess_git_revision_walk(args, sub_index, std::move(paths));

    if (subcommand == "check-ignore") {
        bool after_double_dash = false;
        for (std::size_t i = sub_index + 1; i < args.size(); ++i) {
            const std::string& argument = args[i];
            if (argument == "--") {
                after_double_dash = true;
                continue;
            }
            if (after_double_dash) {
                paths.push_back(argument);
                continue;
            }
            if (argument == "-v" || argument == "--verbose" || argument == "-n" ||
                argument == "--non-matching" || argument == "--no-index" ||
                argument == "-q" || argument == "--quiet" || argument == "--stdin")
                continue;
            if (!argument.empty() && argument[0] == '-')
                return reject("git check-ignore option is not a vetted read-only form: " +
                              argument);
            paths.push_back(argument);
        }
        return accept(std::move(paths));
    }

    if (subcommand == "remote") {
        std::vector<std::string> rest;
        for (std::size_t i = sub_index + 1; i < args.size(); ++i) {
            if (args[i] == "-v" || args[i] == "--verbose") continue;
            rest.push_back(args[i]);
        }
        if (rest.empty()) return accept(std::move(paths));
        if (rest[0] == "get-url") {
            for (std::size_t i = 1; i < rest.size(); ++i) {
                if (rest[i] == "--push" || rest[i] == "--all") continue;
                if (!rest[i].empty() && rest[i][0] == '-')
                    return reject("git remote get-url option is not a vetted read-only form: " +
                                  rest[i]);
            }
            return accept(std::move(paths));
        }
        if (rest[0] == "show") {
            bool no_fetch = false;
            for (std::size_t i = 1; i < rest.size(); ++i) {
                if (rest[i] == "-n") {
                    no_fetch = true;
                    continue;
                }
                if (!rest[i].empty() && rest[i][0] == '-')
                    return reject("git remote show option is not a vetted read-only form: " +
                                  rest[i]);
            }
            if (!no_fetch)
                return reject("git remote show contacts the remote unless -n is given");
            return accept(std::move(paths));
        }
        return reject("git remote subcommand is not a vetted read-only form: " + rest[0]);
    }

    if (subcommand == "branch") {
        bool allow_pattern = false;
        for (std::size_t i = sub_index + 1; i < args.size(); ++i) {
            const std::string& argument = args[i];
            if (argument == "-d" || argument == "-D" || argument == "--delete" ||
                argument == "-m" || argument == "-M" || argument == "--move" ||
                argument == "-c" || argument == "-C" || argument == "--copy" ||
                argument == "-f" || argument == "--force" || argument == "--track" ||
                argument == "--set-upstream-to" ||
                argument.rfind("--set-upstream-to=", 0) == 0 ||
                argument == "--unset-upstream" || argument == "--edit-description" ||
                argument == "--create-reflog")
                return reject("git branch mutation is not a vetted read-only form: " +
                              argument);
            std::string value;
            if (take_value(args, i,
                           {"--list", "--contains", "--no-contains", "--merged",
                            "--no-merged", "--points-at", "--format", "--sort",
                            "--column", "--color", "--abbrev"},
                           "l", value)) {
                if (args[i].rfind("--list", 0) == 0 || args[i] == "-l" ||
                    (args[i].size() >= 2 && args[i][0] == '-' && args[i][1] == 'l'))
                    allow_pattern = true;
                continue;
            }
            if (argument == "-a" || argument == "--all" || argument == "-r" ||
                argument == "--remotes" || argument == "-v" || argument == "-vv" ||
                argument == "--verbose" || argument == "--list" || argument == "-l" ||
                argument == "--show-current" || argument == "--no-color" ||
                argument == "--color=never" || argument == "--ignore-case" ||
                argument == "-i" || argument == "--column" || argument == "--no-column" ||
                argument == "--no-abbrev") {
                if (argument == "--list" || argument == "-l") allow_pattern = true;
                continue;
            }
            if (!argument.empty() && argument[0] == '-')
                return reject("git branch option is not a vetted list form: " + argument);
            if (!allow_pattern)
                return reject("git branch create/rename is not a vetted read-only form");
        }
        return accept(std::move(paths));
    }

    if (subcommand == "tag") {
        bool allow_pattern = false;
        for (std::size_t i = sub_index + 1; i < args.size(); ++i) {
            const std::string& argument = args[i];
            if (argument == "-d" || argument == "--delete" || argument == "-a" ||
                argument == "--annotate" || argument == "-m" || argument == "--message" ||
                argument == "-F" || argument == "--file" || argument == "-f" ||
                argument == "--force" || argument == "-s" || argument == "--sign" ||
                argument == "-u" || argument == "--local-user" || argument == "--edit")
                return reject("git tag mutation is not a vetted read-only form: " +
                              argument);
            std::string value;
            if (take_value(args, i,
                           {"--list", "--contains", "--no-contains", "--merged",
                            "--no-merged", "--points-at", "--format", "--sort",
                            "--column", "--color", "--abbrev"},
                           "ln", value)) {
                if (args[i].rfind("--list", 0) == 0 || args[i] == "-l")
                    allow_pattern = true;
                continue;
            }
            if (argument == "-l" || argument == "--list" || argument == "-n" ||
                argument == "--ignore-case" || argument == "-i" ||
                argument == "--column" || argument == "--no-column" ||
                argument == "--no-color" || argument == "--color=never") {
                if (argument == "-l" || argument == "--list") allow_pattern = true;
                continue;
            }
            if (!argument.empty() && argument[0] == '-')
                return reject("git tag option is not a vetted list form: " + argument);
            if (!allow_pattern)
                return reject("git tag create is not a vetted read-only form");
        }
        return accept(std::move(paths));
    }

    if (subcommand == "stash") {
        if (sub_index + 1 >= args.size())
            return reject("git stash without list/show is a mutation");
        const std::string& action = args[sub_index + 1];
        if (action != "list" && action != "show")
            return reject("git stash " + action + " is not a vetted read-only form");
        return assess_git_revision_walk(args, sub_index + 1, std::move(paths));
    }

    if (subcommand == "merge-base" || subcommand == "describe" ||
        subcommand == "name-rev" || subcommand == "show-ref" ||
        subcommand == "for-each-ref" || subcommand == "cat-file")
        return assess_git_revision_walk(args, sub_index, std::move(paths));

    return reject("git subcommand is not a vetted read-only form: " + subcommand);
}

ReadOnlyCommandAssessment assess_node_test(const std::vector<std::string>& args) {
    static const StringSet flags = {
        "--test", "--test-only", "--test-force-exit", "--no-warnings"};
    static const StringSet values = {
        "--test-name-pattern", "--test-skip-pattern", "--test-timeout",
        "--test-concurrency", "--test-shard"};
    static const StringSet rejected = {
        "-e", "--eval", "-p", "--print", "-c", "--check", "-i", "--interactive",
        "-r", "--require", "--import", "--loader", "--experimental-loader",
        "--inspect", "--inspect-brk", "--inspect-port", "--watch", "--watch-path",
        "--run", "--test-update-snapshots", "--test-reporter-destination"};
    std::vector<std::string> paths;
    bool seen_test = false;
    bool operands = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--") {
            operands = true;
            continue;
        }
        if (operands) {
            paths.push_back(arg);
            continue;
        }
        std::string rejected_value;
        if (rejected.find(arg) != rejected.end() ||
            exact_or_assignment(arg, rejected, &rejected_value))
            return reject("node option is not a vetted test-runner form: " + arg);
        std::string value;
        if (take_value(args, i, values, {}, value)) continue;
        if (flags.find(arg) != flags.end()) {
            if (arg == "--test") seen_test = true;
            continue;
        }
        if (!arg.empty() && arg[0] == '-')
            return reject("unknown node option: " + arg);
        if (!seen_test)
            return reject("node requires --test before path operands");
        paths.push_back(arg);
    }
    if (!seen_test) return reject("node is limited to --test");
    return accept(std::move(paths));
}

}  // namespace

std::size_t git_subcommand_index(const std::vector<std::string>& arguments) {
    return find_git_subcommand(arguments);
}

bool git_config_assignment_is_dangerous(const std::string& assignment) {
    return config_assignment_is_dangerous(assignment);
}

ReadOnlyCommandAssessment assess_read_only_command(
    const std::vector<std::string>& args) {
    if (args.empty()) return reject("command is empty");
    const std::string& command = args[0];
    if (command == "command") {
        if (args.size() < 3 || args[1] != "-v")
            return reject("only command -v NAME is a vetted shell-builtin form");
        for (std::size_t index = 2; index < args.size(); ++index)
            if (args[index].empty() || args[index][0] == '-' ||
                args[index].find('/') != std::string::npos)
                return reject("command -v requires bare command names");
        return accept();
    }
    if (command == "pwd") {
        for (std::size_t i = 1; i < args.size(); ++i)
            if (args[i] != "-L" && args[i] != "-P" &&
                args[i] != "--logical" && args[i] != "--physical")
                return reject("pwd does not accept operands");
        return accept();
    }
    if (command == "ls") return assess_ls(args);
    if (command == "cat")
        return simple_file_command(args, "AbenstuvET", {
            "--show-all", "--number-nonblank", "--number", "--squeeze-blank",
            "--show-ends", "--show-tabs", "--show-nonprinting"});
    if (command == "head") return assess_head_tail(args, false);
    if (command == "tail") return assess_head_tail(args, true);
    if (command == "stat")
        return simple_file_command(args, "ft", {"--file-system", "--terse"},
                                   "c", {"--format", "--printf"});
    if (command == "file")
        return simple_file_command(
            args, "bhikNnprsSvz0", {"--brief", "--no-dereference", "--mime", "--mime-type",
                                   "--mime-encoding", "--keep-going", "--raw",
                                   "--no-pad", "--preserve-date", "--special-files",
                                   "--uncompress", "--print0"},
            "emf", {"--exclude", "--magic-file", "--files-from"},
            {"-C", "--compile", "-L", "--dereference"},
            {"-m", "--magic-file", "-f", "--files-from"});
    if (command == "wc")
        return simple_file_command(args, "clmwL", {"--bytes", "--chars", "--lines",
                                                   "--max-line-length", "--words"});
    if (command == "du")
        return simple_file_command(
            args, "abchkmSsx", {"--all", "--apparent-size", "--bytes", "--total",
                                "--human-readable", "--si", "--summarize",
                                "--one-file-system", "--separate-dirs"},
            "d", {"--max-depth", "--block-size", "--exclude", "--exclude-from",
                  "--time", "--time-style"},
            {"-H", "-L", "--dereference", "--dereference-args"},
            {"--exclude-from"});
    if (command == "df")
        return simple_file_command(
            args, "aBghHiklmPTt", {"--all", "--human-readable", "--si",
                                    "--inodes", "--local", "--portability",
                                    "--print-type", "--total"},
            "B", {"--block-size", "--type", "--exclude-type"});
    if (command == "grep") return assess_grep(args);
    if (command == "rg") return assess_rg(args);
    if (command == "find") return assess_find(args);
    if (command == "diff")
        return simple_file_command(
            args, "abBdiNqrstTuwy", {"--brief", "--context", "--ed", "--forward-ed",
                                     "--ignore-all-space", "--ignore-blank-lines",
                                     "--ignore-case", "--ignore-space-change",
                                     "--minimal", "--new-file", "--normal",
                                     "--recursive", "--report-identical-files",
                                     "--side-by-side", "--speed-large-files",
                                     "--strip-trailing-cr", "--text", "--unified"},
            "CUIF", {"--context", "--unified", "--ignore-matching-lines",
                     "--label", "--starting-file", "--horizon-lines",
                     "--width", "--tabsize", "--from-file", "--to-file"},
            {"--output", "-o"}, {"--from-file", "--to-file"});
    if (command == "cmp")
        return simple_file_command(args, "blsn", {"--print-bytes", "--verbose",
                                                  "--silent", "--quiet"},
                                   "i", {"--ignore-initial", "--bytes"});
    if (command == "readlink")
        return simple_file_command(args, "efmnqsvz", {"--canonicalize",
                                                      "--canonicalize-existing",
                                                      "--canonicalize-missing",
                                                      "--no-newline", "--quiet",
                                                      "--silent", "--verbose",
                                                      "--zero"});
    if (command == "md5sum" || command == "sha1sum" || command == "sha224sum" ||
        command == "sha256sum" || command == "sha384sum" ||
        command == "sha512sum" || command == "b2sum" || command == "cksum")
        return assess_checksums(args);
    if (command == "date") return assess_date(args);
    if (command == "ip") return assess_ip(args);
    if (command == "ifconfig") {
        if (args.size() == 1) return accept();
        if (args.size() == 2 &&
            (args[1] == "-a" || args[1] == "-s" || args[1] == "-v" ||
             (!args[1].empty() && args[1][0] != '-')))
            return accept();
        return reject("ifconfig configuration operands are not vetted");
    }
    if (command == "groups") {
        for (std::size_t i = 1; i < args.size(); ++i)
            if (args[i].empty() || args[i][0] == '-')
                return reject("unknown groups option");
        return accept();
    }
    if (command == "git") return assess_git(args);
    return assess_passive(args);
}

ReadOnlyCommandAssessment assess_node_test_command(
    const std::vector<std::string>& args) {
    if (args.empty()) return reject("not a node test-runner command");
    const std::string command = normalized_command_basename(args[0]);
    if (command != "node" && command != "nodejs")
        return reject("not a node test-runner command");
    return assess_node_test(args);
}

namespace {

WorkspaceFsCommandAssessment reject_fs(const std::string& reason) {
    WorkspaceFsCommandAssessment result;
    result.reason = reason;
    return result;
}

WorkspaceFsCommandAssessment accept_fs(std::vector<std::string> paths,
                                       bool recursive_rm) {
    WorkspaceFsCommandAssessment result;
    result.classified = true;
    result.recursive_rm = recursive_rm;
    result.path_operands = std::move(paths);
    return result;
}

bool short_flags_only(const std::string& arg, const char* allowed) {
    if (arg.size() < 2 || arg[0] != '-' || arg[1] == '-') return false;
    for (std::size_t i = 1; i < arg.size(); ++i) {
        bool ok = false;
        for (const char* p = allowed; *p != '\0'; ++p) {
            if (arg[i] == *p) {
                ok = true;
                break;
            }
        }
        if (!ok) return false;
    }
    return true;
}

bool short_flags_contain(const std::string& arg, char flag) {
    if (arg.size() < 2 || arg[0] != '-' || arg[1] == '-') return false;
    return arg.find(flag) != std::string::npos;
}

}  // namespace

WorkspaceFsCommandAssessment assess_workspace_fs_command(
    const std::vector<std::string>& args) {
    if (args.empty()) return reject_fs("command is empty");
    const std::string& command = args[0];
    std::vector<std::string> paths;
    bool seen_double_dash = false;
    auto take_operand = [&](const std::string& arg) {
        if (arg.empty() || arg == "-") return false;
        paths.push_back(arg);
        return true;
    };

    if (command == "mkdir") {
        for (std::size_t i = 1; i < args.size(); ++i) {
            const std::string& arg = args[i];
            if (!seen_double_dash && arg == "--") {
                seen_double_dash = true;
                continue;
            }
            if (!seen_double_dash && (arg == "-p" || arg == "--parents" ||
                                      arg == "-v" || arg == "--verbose"))
                continue;
            if (!seen_double_dash && !arg.empty() && arg[0] == '-')
                return reject_fs("mkdir flag is not classified");
            if (!take_operand(arg)) return reject_fs("mkdir operand is invalid");
        }
        if (paths.empty()) return reject_fs("mkdir requires a path");
        return accept_fs(std::move(paths), false);
    }

    if (command == "rmdir") {
        for (std::size_t i = 1; i < args.size(); ++i) {
            const std::string& arg = args[i];
            if (!seen_double_dash && arg == "--") {
                seen_double_dash = true;
                continue;
            }
            if (!seen_double_dash &&
                (arg == "-p" || arg == "--parents" || arg == "-v" ||
                 arg == "--verbose" || arg == "--ignore-fail-on-non-empty"))
                continue;
            if (!seen_double_dash && !arg.empty() && arg[0] == '-')
                return reject_fs("rmdir flag is not classified");
            if (!take_operand(arg)) return reject_fs("rmdir operand is invalid");
        }
        if (paths.empty()) return reject_fs("rmdir requires a path");
        return accept_fs(std::move(paths), false);
    }

    if (command == "rm") {
        bool recursive = false;
        for (std::size_t i = 1; i < args.size(); ++i) {
            const std::string& arg = args[i];
            if (!seen_double_dash && arg == "--") {
                seen_double_dash = true;
                continue;
            }
            if (!seen_double_dash && (arg == "-r" || arg == "-R" ||
                                      arg == "--recursive")) {
                recursive = true;
                continue;
            }
            if (!seen_double_dash && (arg == "-f" || arg == "--force" ||
                                      arg == "-v" || arg == "--verbose"))
                continue;
            if (!seen_double_dash && short_flags_only(arg, "rRfv")) {
                if (short_flags_contain(arg, 'r') || short_flags_contain(arg, 'R'))
                    recursive = true;
                continue;
            }
            if (!seen_double_dash && !arg.empty() && arg[0] == '-')
                return reject_fs("rm flag is not classified");
            if (!take_operand(arg)) return reject_fs("rm operand is invalid");
        }
        if (paths.empty()) return reject_fs("rm requires a path");
        return accept_fs(std::move(paths), recursive);
    }

    if (command == "mv") {
        for (std::size_t i = 1; i < args.size(); ++i) {
            const std::string& arg = args[i];
            if (!seen_double_dash && arg == "--") {
                seen_double_dash = true;
                continue;
            }
            if (!seen_double_dash &&
                (arg == "-f" || arg == "--force" || arg == "-n" ||
                 arg == "--no-clobber" || arg == "-v" || arg == "--verbose"))
                continue;
            if (!seen_double_dash && short_flags_only(arg, "fnv")) continue;
            if (!seen_double_dash && !arg.empty() && arg[0] == '-')
                return reject_fs("mv flag is not classified");
            if (!take_operand(arg)) return reject_fs("mv operand is invalid");
        }
        if (paths.size() < 2) return reject_fs("mv requires source and destination");
        return accept_fs(std::move(paths), false);
    }

    return reject_fs("not a classified workspace filesystem command");
}

namespace {

constexpr std::size_t kMaxInlinePayload = 32u * 1024u;

bool versioned_name(const std::string& command, const char* prefix) {
    const std::size_t n = std::char_traits<char>::length(prefix);
    if (command.size() <= n || command.compare(0, n, prefix) != 0 || command[n] != '.')
        return false;
    for (std::size_t i = n + 1; i < command.size(); ++i) {
        if (command[i] != '.' && (command[i] < '0' || command[i] > '9')) return false;
    }
    return true;
}

bool is_python_command(const std::string& command) {
    return command == "python" || command == "python3" || command == "py" ||
           command == "pypy" || command == "pypy3" ||
           versioned_name(command, "python") || versioned_name(command, "python3") ||
           versioned_name(command, "pypy") || versioned_name(command, "pypy3");
}

bool is_node_command(const std::string& command) {
    return command == "node" || command == "nodejs";
}

bool is_pytest_command(const std::string& command) {
    return command == "pytest" || command == "py.test";
}

bool is_listener_module(const std::string& module) {
    return module == "http.server" || module == "xmlrpc.server" ||
           module == "smtpd" || module == "wsgiref.simple_server";
}

bool is_env_mutation_module(const std::string& module) {
    return module == "pip" || module == "pip3" || module == "ensurepip" ||
           module == "easy_install" || module.rfind("pip.", 0) == 0 ||
           module.rfind("ensurepip.", 0) == 0;
}

bool is_protected_path_component(const std::string& part) {
    return part == ".ainiux-pr" || part == ".ainiux" || part == ".git" ||
           part == ".hg" || part == ".svn" || part == "..";
}

bool quoted_string_escapes_workspace(const std::string& value) {
    if (value.empty()) return false;
    if (value[0] == '/' || value[0] == '~') return true;
    if (value.size() >= 2 &&
        std::isalpha(static_cast<unsigned char>(value[0])) != 0 && value[1] == ':')
        return true;
    if (value.find("../") != std::string::npos ||
        value.find("..\\") != std::string::npos)
        return true;
    if (value.find(".ainiux-pr") != std::string::npos) return true;
    std::string token;
    for (char ch : value) {
        if (ch == '/' || ch == '\\') {
            if (is_protected_path_component(token)) return true;
            token.clear();
        } else {
            token.push_back(ch);
        }
    }
    return is_protected_path_component(token);
}

void extract_quoted(const std::string& text, std::size_t& index, char quote,
                    std::string& out) {
    const bool triple = index + 2 < text.size() && text[index + 1] == quote &&
                        text[index + 2] == quote;
    index += triple ? 3 : 1;
    out.clear();
    while (index < text.size()) {
        if (!triple && text[index] == '\\' && index + 1 < text.size()) {
            out.push_back(text[index + 1]);
            index += 2;
            continue;
        }
        if (triple) {
            if (index + 2 < text.size() && text[index] == quote &&
                text[index + 1] == quote && text[index + 2] == quote) {
                index += 3;
                return;
            }
        } else if (text[index] == quote) {
            ++index;
            return;
        }
        out.push_back(text[index]);
        ++index;
    }
}

bool inline_payload_targets_workspace(const std::string& payload,
                                      std::string& reason) {
    if (payload.size() > kMaxInlinePayload) {
        reason = "inline interpreter payload exceeds 32 KiB";
        return false;
    }
    if (payload.find("$HOME") != std::string::npos ||
        payload.find("${HOME}") != std::string::npos ||
        payload.find("%USERPROFILE%") != std::string::npos ||
        payload.find("%HOMEPATH%") != std::string::npos ||
        payload.find("%HOME%") != std::string::npos ||
        payload.find("~/") != std::string::npos ||
        payload.find("~\\") != std::string::npos) {
        reason = "inline payload references a home directory";
        return false;
    }
    static const char* kAbsolute[] = {
        "/etc/", "/usr/", "/home/", "/root/", "/proc/", "/sys/", "/dev/",
        "/var/", "/tmp/", "/opt/", "/Users/", "/private/"};
    for (const char* prefix : kAbsolute) {
        if (payload.find(prefix) != std::string::npos) {
            reason = "inline payload contains an absolute path";
            return false;
        }
    }
    std::string extracted;
    for (std::size_t i = 0; i < payload.size();) {
        const char ch = payload[i];
        if (ch == '\'' || ch == '"' || ch == '`') {
            extract_quoted(payload, i, ch, extracted);
            if (quoted_string_escapes_workspace(extracted)) {
                reason = "inline payload contains an out-of-workspace path literal";
                return false;
            }
            continue;
        }
        ++i;
    }
    return true;
}

bool consume_value(const std::vector<std::string>& args, std::size_t& index,
                   const std::string& option, std::string& value) {
    const std::string& arg = args[index];
    const std::string prefix = option + "=";
    if (arg.rfind(prefix, 0) == 0) {
        value = arg.substr(prefix.size());
        return !value.empty();
    }
    if (arg != option) return false;
    if (index + 1 >= args.size()) return false;
    value = args[++index];
    return !value.empty();
}

ReadOnlyCommandAssessment assess_inline_payload(const std::string& payload) {
    std::string reason;
    if (payload.empty()) return reject("inline interpreter program is empty");
    if (!inline_payload_targets_workspace(payload, reason)) return reject(reason);
    return accept();
}

ReadOnlyCommandAssessment assess_python_interpreter(
    const std::vector<std::string>& args) {
    static const StringSet long_flags = {"--help", "--version", "--copyright",
                                         "--license"};
    static const StringSet value_flags = {"--check-hash-based-pycs"};
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--") {
            if (i + 1 >= args.size() || args[i + 1] == "-")
                return reject("python requires a script, -m, or -c");
            return accept({args[i + 1]});
        }
        std::string payload;
        if (arg == "-c" || arg == "--command" ||
            consume_value(args, i, "--command", payload)) {
            if (payload.empty()) {
                if (i + 1 >= args.size())
                    return reject("python -c requires a program");
                payload = args[++i];
            }
            return assess_inline_payload(payload);
        }
        if (arg.size() > 2 && arg.compare(0, 2, "-c") == 0)
            return assess_inline_payload(arg.substr(2));
        std::string module;
        if (arg == "-m" || arg == "--module" ||
            consume_value(args, i, "--module", module)) {
            if (module.empty()) {
                if (i + 1 >= args.size())
                    return reject("python -m requires a module name");
                module = args[++i];
            }
            if (is_env_mutation_module(module))
                return reject("python -m " + module + " mutates the environment");
            if (is_listener_module(module))
                return reject("python -m " + module + " starts a network listener");
            return accept();
        }
        if (arg.size() > 2 && arg.compare(0, 2, "-m") == 0 && arg[2] != '-') {
            module = arg.substr(2);
            if (is_env_mutation_module(module))
                return reject("python -m " + module + " mutates the environment");
            if (is_listener_module(module))
                return reject("python -m " + module + " starts a network listener");
            return accept();
        }
        if (arg == "-") return reject("python stdin programs are not classified");
        std::string value;
        if (arg == "-W" || arg == "-X" || arg.compare(0, 2, "-W") == 0 ||
            arg.compare(0, 2, "-X") == 0) {
            if (arg == "-W" || arg == "-X") {
                if (i + 1 >= args.size())
                    return reject("python " + arg + " requires a value");
                ++i;
            }
            continue;
        }
        if (take_value(args, i, value_flags, {}, value)) continue;
        if (long_flags.find(arg) != long_flags.end() ||
            is_short_cluster(arg, "bBdEhOQqsSuvVx"))
            continue;
        if (!arg.empty() && arg[0] == '-')
            return reject("unknown python option: " + arg);
        if (arg.empty()) return reject("python script path is empty");
        return accept({arg});
    }
    return reject("python requires a script, -m, or -c");
}

ReadOnlyCommandAssessment assess_node_interpreter(
    const std::vector<std::string>& args) {
    const ReadOnlyCommandAssessment test = assess_node_test(args);
    if (test.vetted) return test;
    static const StringSet flags = {
        "--no-warnings", "--experimental-strip-types",
        "--experimental-transform-types", "--trace-uncaught",
        "--abort-on-uncaught-exception", "--no-deprecation", "--trace-warnings",
        "--check", "-c"};
    static const StringSet values = {"--input-type", "--title"};
    static const StringSet rejected = {
        "--inspect", "--inspect-brk", "--inspect-port", "--watch", "--watch-path",
        "--loader", "--experimental-loader", "--run", "-i", "--interactive",
        "--test-update-snapshots", "--test-reporter-destination"};
    bool seen_eval = false;
    bool seen_test = false;
    std::string payload;
    std::vector<std::string> paths;
    bool operands = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--") {
            operands = true;
            continue;
        }
        if (operands) {
            if (seen_eval) continue;
            if (paths.empty()) paths.push_back(arg);
            continue;
        }
        std::string rejected_value;
        if (rejected.find(arg) != rejected.end() ||
            exact_or_assignment(arg, rejected, &rejected_value) ||
            arg.rfind("--inspect", 0) == 0 || arg.rfind("--watch", 0) == 0)
            return reject("node option is not a workspace interpreter form: " + arg);
        std::string value;
        if (arg == "-e" || arg == "--eval" || arg == "-p" || arg == "--print" ||
            consume_value(args, i, "--eval", value) ||
            consume_value(args, i, "--print", value)) {
            if (seen_test) return reject("node --test does not combine with -e");
            if (value.empty()) {
                if (arg == "-e" || arg == "--eval" || arg == "-p" || arg == "--print") {
                    if (i + 1 >= args.size())
                        return reject("node -e requires a program");
                    payload = args[++i];
                } else {
                    payload = value;
                }
            } else {
                payload = value;
            }
            seen_eval = true;
            continue;
        }
        if (arg == "-r" || arg == "--require" || arg == "--import" ||
            consume_value(args, i, "--require", value) ||
            consume_value(args, i, "--import", value)) {
            if (value.empty()) {
                if (i + 1 >= args.size())
                    return reject("node --require/--import requires a path");
                value = args[++i];
            }
            if (value.find('/') == std::string::npos &&
                value.find('\\') == std::string::npos &&
                value.find('.') == std::string::npos)
                return reject("node --require/--import is limited to workspace files");
            paths.push_back(value);
            continue;
        }
        if (take_value(args, i, values, {}, value)) continue;
        if (arg == "--test" || arg.rfind("--test-", 0) == 0) {
            seen_test = true;
            continue;
        }
        if (flags.find(arg) != flags.end()) continue;
        if (!arg.empty() && arg[0] == '-')
            return reject("unknown node option: " + arg);
        if (seen_eval) continue;
        if (arg == "-") return reject("node stdin programs are not classified");
        if (paths.empty()) paths.push_back(arg);
    }
    if (seen_eval) return assess_inline_payload(payload);
    if (seen_test) return test;
    if (!paths.empty()) return accept(std::move(paths));
    return reject("node requires a script, --test, or -e");
}

ReadOnlyCommandAssessment assess_pytest_interpreter(
    const std::vector<std::string>& args) {
    static const StringSet rejected = {"--pdb", "--trace", "--pdbcls"};
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--") continue;
        std::string rejected_value;
        if (rejected.find(arg) != rejected.end() ||
            exact_or_assignment(arg, rejected, &rejected_value) ||
            arg.rfind("--pdb", 0) == 0)
            return reject("pytest debugger options are not auto-allowed");
    }
    return accept();
}

bool hyphen_versioned(const std::string& command, const char* prefix) {
    const std::size_t n = std::char_traits<char>::length(prefix);
    if (command.size() <= n + 1 || command.compare(0, n, prefix) != 0 ||
        command[n] != '-')
        return false;
    for (std::size_t i = n + 1; i < command.size(); ++i) {
        if (command[i] != '.' && (command[i] < '0' || command[i] > '9'))
            return false;
    }
    return true;
}

bool exact_or_hyphen_versioned(const std::string& command, const char* name) {
    return command == name || hyphen_versioned(command, name);
}

bool triple_suffixed(const std::string& command, const char* suffix) {
    const std::size_t n = std::char_traits<char>::length(suffix);
    if (command.size() <= n + 1) return false;
    if (command.compare(command.size() - n, n, suffix) != 0) return false;
    return command[command.size() - n - 1] == '-';
}

bool is_c_compiler_command(const std::string& command) {
    return exact_or_hyphen_versioned(command, "gcc") ||
           exact_or_hyphen_versioned(command, "g++") ||
           exact_or_hyphen_versioned(command, "cc") ||
           exact_or_hyphen_versioned(command, "c++") ||
           exact_or_hyphen_versioned(command, "clang") ||
           exact_or_hyphen_versioned(command, "clang++") ||
           command == "clang-cl" || command == "cl" ||
           triple_suffixed(command, "gcc") || triple_suffixed(command, "g++") ||
           triple_suffixed(command, "c++") || triple_suffixed(command, "clang") ||
           triple_suffixed(command, "clang++");
}

bool is_c_support_command(const std::string& command) {
    return exact_or_hyphen_versioned(command, "clang-format") ||
           exact_or_hyphen_versioned(command, "clang-tidy") || command == "ar" ||
           command == "ranlib" || command == "ld" || command == "lld";
}

bool is_make_command(const std::string& command) {
    return command == "make" || command == "gmake" ||
           command == "mingw32-make" || command == "nmake";
}

bool is_cmake_command(const std::string& command) {
    return command == "cmake" || command == "ctest";
}

bool is_ninja_command(const std::string& command) {
    return command == "ninja";
}

bool is_java_launcher_command(const std::string& command) {
    return exact_or_hyphen_versioned(command, "java");
}

bool is_javac_command(const std::string& command) {
    return exact_or_hyphen_versioned(command, "javac");
}

bool is_jar_command(const std::string& command) {
    return command == "jar";
}

bool is_maven_command(const std::string& command) {
    return command == "mvn" || command == "mvnw";
}

bool is_gradle_command(const std::string& command) {
    return command == "gradle" || command == "gradlew";
}

bool is_dotnet_command(const std::string& command) {
    return command == "dotnet";
}

bool is_csc_command(const std::string& command) {
    return command == "csc";
}

bool is_msbuild_command(const std::string& command) {
    return command == "msbuild";
}

bool option_matches_prefix(const std::string& arg, const char* prefix) {
    const std::size_t n = std::char_traits<char>::length(prefix);
    return arg.size() >= n && arg.compare(0, n, prefix) == 0;
}

ReadOnlyCommandAssessment collect_response_files(
    const std::vector<std::string>& args) {
    std::vector<std::string> paths;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg.size() > 1 && arg[0] == '@' && arg[1] != '@')
            paths.push_back(arg.substr(1));
    }
    return accept(std::move(paths));
}

bool compiler_plugin_option(const std::string& arg) {
    return arg == "-wrapper" || option_matches_prefix(arg, "-wrapper=") ||
           arg == "-fplugin" || option_matches_prefix(arg, "-fplugin") ||
           arg == "-plugin" || option_matches_prefix(arg, "-plugin=") ||
           arg == "-load" || option_matches_prefix(arg, "-load=") ||
           arg == "-fpass-plugin" ||
           option_matches_prefix(arg, "-fpass-plugin=") ||
           arg == "-load-pass-plugin" ||
           option_matches_prefix(arg, "-load-pass-plugin=");
}

ReadOnlyCommandAssessment assess_c_compiler(const std::vector<std::string>& args) {
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (compiler_plugin_option(args[i]))
            return reject("compiler plugin/wrapper options are not auto-allowed");
    }
    return collect_response_files(args);
}

bool is_install_like_target(const std::string& target) {
    return target == "install" || target == "install-strip" ||
           target == "install/strip" || target == "uninstall";
}

ReadOnlyCommandAssessment assess_make_command(
    const std::vector<std::string>& args) {
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--eval" || option_matches_prefix(arg, "--eval=") || arg == "-E")
            return reject("make --eval is not auto-allowed");
        std::string makefile;
        if (arg == "-f" || arg == "--file" || arg == "--makefile") {
            if (i + 1 >= args.size())
                return reject("make -f requires a makefile");
            makefile = args[++i];
            if (makefile == "-")
                return reject("make makefile from stdin is not auto-allowed");
            continue;
        }
        if (option_matches_prefix(arg, "--file="))
            makefile = arg.substr(7);
        else if (option_matches_prefix(arg, "--makefile="))
            makefile = arg.substr(11);
        else if (arg.size() > 2 && arg.compare(0, 2, "-f") == 0 && arg[1] == 'f')
            makefile = arg.substr(2);
        if (!makefile.empty() && makefile == "-")
            return reject("make makefile from stdin is not auto-allowed");
        if (!arg.empty() && arg[0] == '-') continue;
        if (arg.find('=') != std::string::npos) continue;
        if (is_install_like_target(arg))
            return reject("make install/uninstall is not auto-allowed");
    }
    return accept();
}

ReadOnlyCommandAssessment assess_cmake_command(
    const std::vector<std::string>& args) {
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--install" || option_matches_prefix(arg, "--install="))
            return reject("cmake --install is not auto-allowed");
        std::string target;
        if (arg == "--target" || arg == "-t") {
            if (i + 1 >= args.size())
                return reject("cmake --target requires a name");
            target = args[++i];
        } else if (option_matches_prefix(arg, "--target=")) {
            target = arg.substr(9);
        }
        if (!target.empty() && is_install_like_target(target))
            return reject("cmake install target is not auto-allowed");
    }
    return accept();
}

ReadOnlyCommandAssessment assess_ninja_command(
    const std::vector<std::string>& args) {
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (!arg.empty() && arg[0] == '-') continue;
        if (is_install_like_target(arg))
            return reject("ninja install/uninstall is not auto-allowed");
    }
    return accept();
}

bool java_agent_option(const std::string& arg) {
    return arg == "-agentpath" || option_matches_prefix(arg, "-agentpath:") ||
           arg == "-agentlib" || option_matches_prefix(arg, "-agentlib:") ||
           arg == "-javaagent" || option_matches_prefix(arg, "-javaagent:");
}

ReadOnlyCommandAssessment assess_java_command(
    const std::vector<std::string>& args) {
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (java_agent_option(args[i]))
            return reject("java agent options are not auto-allowed");
    }
    return accept();
}

bool maven_publish_goal(const std::string& arg) {
    if (arg.empty() || arg[0] == '-') return false;
    if (arg == "deploy" || arg == "release:perform") return true;
    if (arg.size() >= 7 && arg.compare(arg.size() - 7, 7, ":deploy") == 0)
        return true;
    return arg.size() > 16 &&
           arg.compare(arg.size() - 16, 16, ":release:perform") == 0;
}

ReadOnlyCommandAssessment assess_maven_command(
    const std::vector<std::string>& args) {
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (maven_publish_goal(args[i]))
            return reject("maven deploy/release is not auto-allowed");
    }
    return accept();
}

bool gradle_remote_publish_task(const std::string& arg) {
    if (arg.empty() || arg[0] == '-') return false;
    std::string task = arg;
    const std::size_t colon = task.rfind(':');
    if (colon != std::string::npos) task = task.substr(colon + 1);
    if (task.rfind("publish", 0) != 0) return false;
    return task.find("ToMavenLocal") == std::string::npos;
}

ReadOnlyCommandAssessment assess_gradle_command(
    const std::vector<std::string>& args) {
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (gradle_remote_publish_task(args[i]))
            return reject("gradle publish is not auto-allowed");
    }
    return accept();
}

bool dotnet_allowed_verb(const std::string& verb) {
    return verb == "build" || verb == "test" || verb == "run" ||
           verb == "restore" || verb == "clean" || verb == "publish" ||
           verb == "pack" || verb == "new" || verb == "sln" || verb == "add" ||
           verb == "format" || verb == "msbuild" || verb == "watch" ||
           verb == "help";
}

bool ends_with_ci(const std::string& text, const char* suffix) {
    const std::size_t n = std::char_traits<char>::length(suffix);
    if (text.size() < n) return false;
    for (std::size_t i = 0; i < n; ++i) {
        char ch = text[text.size() - n + i];
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        if (ch != suffix[i]) return false;
    }
    return true;
}

bool looks_like_dotnet_project(const std::string& arg) {
    return ends_with_ci(arg, ".csproj") || ends_with_ci(arg, ".sln") ||
           ends_with_ci(arg, ".dll");
}

ReadOnlyCommandAssessment assess_dotnet_command(
    const std::vector<std::string>& args) {
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--info" || arg == "--list-sdks" ||
            arg == "--list-runtimes" || arg == "--version" || arg == "--help" ||
            arg == "-h" || arg == "-v" || arg == "--verbosity")
            continue;
        if (!arg.empty() && arg[0] == '-') continue;
        if (looks_like_dotnet_project(arg)) continue;
        std::string verb = arg;
        for (char& ch : verb) {
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        }
        if (verb == "nuget" || verb == "tool")
            return reject("dotnet nuget/tool mutation is not auto-allowed");
        if (!dotnet_allowed_verb(verb))
            return reject("dotnet verb is not a workspace build/test form");
        break;
    }
    return accept();
}

}  // namespace

ReadOnlyCommandAssessment assess_workspace_interpreter_command(
    const std::vector<std::string>& arguments) {
    if (arguments.empty()) return reject("command is empty");
    const std::string command = normalized_command_basename(arguments[0]);
    if (is_python_command(command)) return assess_python_interpreter(arguments);
    if (is_node_command(command)) return assess_node_interpreter(arguments);
    if (is_pytest_command(command)) return assess_pytest_interpreter(arguments);
    if (is_c_compiler_command(command) || is_c_support_command(command) ||
        is_csc_command(command) || is_msbuild_command(command))
        return assess_c_compiler(arguments);
    if (is_make_command(command)) return assess_make_command(arguments);
    if (is_cmake_command(command)) return assess_cmake_command(arguments);
    if (is_ninja_command(command)) return assess_ninja_command(arguments);
    if (command == "jshell") return reject("jshell is not a workspace build form");
    if (is_java_launcher_command(command) || is_javac_command(command) ||
        is_jar_command(command))
        return assess_java_command(arguments);
    if (is_maven_command(command)) return assess_maven_command(arguments);
    if (is_gradle_command(command)) return assess_gradle_command(arguments);
    if (is_dotnet_command(command)) return assess_dotnet_command(arguments);
    return reject("not a workspace interpreter command");
}

}  // namespace ainiux::agent
