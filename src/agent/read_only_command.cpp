#include "agent/read_only_command.hpp"

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

}  // namespace

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
    return assess_passive(args);
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

}  // namespace ainiux::agent
