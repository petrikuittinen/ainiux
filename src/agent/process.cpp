#include "agent/process.hpp"

#include "agent/command_guard.hpp"
#include "agent/project_scripts.hpp"
#include "agent/read_only_command.hpp"
#include "platform/environment.hpp"
#include "platform/filesystem.hpp"
#include "provider/provider.hpp"
#include "runtime/subprocess.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <set>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace ainiux::agent {
namespace {
namespace fs = std::filesystem;

bool inside_root(const fs::path& root, const fs::path& candidate) {
    bool within = false;
    return platform::path_is_within(root.u8string(), candidate.u8string(), within).ok() && within;
}

Error resolve_cwd(const ProcessOptions& options, fs::path& root, fs::path& cwd) {
    std::error_code ec;
    root = fs::canonical(fs::absolute(fs::u8path(options.workspace), ec), ec);
    if (ec || !fs::is_directory(root, ec))
        return {ErrorCode::FileRead, "could not resolve command workspace: " + options.workspace};
    const fs::path supplied = fs::u8path(options.cwd);
    fs::path requested =
        options.cwd.empty() ? root
                            : (supplied.is_absolute() ? supplied : root / supplied);
    cwd = fs::canonical(requested, ec);
    if (ec || !fs::is_directory(cwd, ec) ||
        (!options.allow_external_cwd && !inside_root(root, cwd)))
        return {ErrorCode::BadArgs,
                options.allow_external_cwd
                    ? "run_command cwd must be an existing canonical directory"
                    : "run_command cwd must be an existing directory inside the workspace"};
    return ok_error();
}

// POSIX deliberately uses a small trusted PATH. Native Windows uses the inherited
// PATH, but its resolver below ignores empty/relative components and never falls
// back to the current directory.
std::string fixed_command_path() {
#if defined(_WIN32)
    const std::string inherited = platform::environment_value("PATH");
    std::string sanitized;
    std::size_t start = 0;
    while (start <= inherited.size()) {
        const std::size_t end = inherited.find(';', start);
        const std::string entry = inherited.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        const fs::path directory = fs::u8path(entry);
        if (!entry.empty() && directory.is_absolute() &&
            platform::validate_windows_path_syntax(entry).ok()) {
            if (!sanitized.empty()) sanitized.push_back(';');
            sanitized += directory.lexically_normal().u8string();
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return sanitized;
#else
    return "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin";
#endif
}

#if defined(_WIN32)
std::string env_name_key(std::string name) {
    for (char& ch : name) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    }
    return name;
}
#else
std::string env_name_key(std::string name) { return name; }
#endif

bool environment_has_name(const std::vector<std::string>& environment, const std::string& name) {
    const std::string key = env_name_key(name);
    for (const std::string& entry : environment) {
        const std::size_t eq = entry.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        if (env_name_key(entry.substr(0, eq)) == key) return true;
    }
    return false;
}

void append_inherited_environment(std::vector<std::string>& environment, const char* name) {
    if (name == nullptr || name[0] == '\0') return;
    if (environment_has_name(environment, name)) return;
    const std::string value = platform::environment_value(name);
    if (value.empty()) return;
    environment.push_back(std::string(name) + "=" + value);
}

bool same_running_executable(const std::string& resolved) {
    const std::string self = platform::executable_path();
    if (self.empty() || resolved.empty()) return false;
    std::error_code ec;
    const fs::path left = fs::weakly_canonical(fs::absolute(fs::u8path(self), ec), ec);
    if (ec || left.empty()) return false;
    const fs::path right = fs::weakly_canonical(fs::absolute(fs::u8path(resolved), ec), ec);
    return !ec && !right.empty() && left == right;
}

#if defined(_WIN32)
std::string ascii_lower(std::string value) {
    for (char& ch : value) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    }
    return value;
}

bool supported_windows_executable_extension(const fs::path& path) {
    const std::string extension = ascii_lower(path.extension().u8string());
    return extension == ".com" || extension == ".exe" || extension == ".bat" ||
           extension == ".cmd";
}

std::vector<std::string> windows_path_extensions() {
    const std::vector<std::string> fallback = {".com", ".exe", ".bat", ".cmd"};
    const std::string inherited = platform::environment_value("PATHEXT");
    if (inherited.empty()) return fallback;
    std::vector<std::string> extensions;
    std::size_t start = 0;
    while (start <= inherited.size()) {
        const std::size_t end = inherited.find(';', start);
        std::string extension = ascii_lower(
            inherited.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (!extension.empty() && extension.front() != '.') extension.insert(extension.begin(), '.');
        if (std::find(fallback.begin(), fallback.end(), extension) != fallback.end() &&
            std::find(extensions.begin(), extensions.end(), extension) == extensions.end())
            extensions.push_back(std::move(extension));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return extensions.empty() ? fallback : extensions;
}
#endif

Error resolve_fixed_path_executable(const std::string& name, std::string& resolved) {
    const std::string path = fixed_command_path();
#if defined(_WIN32)
    const fs::path supplied = fs::u8path(name);
    const bool has_extension = supplied.has_extension();
    if (has_extension && !supported_windows_executable_extension(supplied))
        return {ErrorCode::BadArgs,
                "Windows agent commands only execute .com, .exe, .bat, or .cmd files: " +
                    name};
    const std::vector<std::string> extensions =
        has_extension ? std::vector<std::string>{""} : windows_path_extensions();
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t end = path.find(';', start);
        const std::string directory_text =
            path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const fs::path directory = fs::u8path(directory_text);
        if (!directory_text.empty() && directory.is_absolute()) {
            for (const std::string& extension : extensions) {
                const fs::path candidate = directory / fs::u8path(name + extension);
                std::error_code ec;
                if (!fs::is_regular_file(candidate, ec) || ec) continue;
                const fs::path canonical = fs::canonical(candidate, ec);
                if (!ec && supported_windows_executable_extension(canonical)) {
                    resolved = canonical.u8string();
                    return ok_error();
                }
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return {ErrorCode::FileRead,
            "command not found on inherited absolute PATH entries: " + name};
#else
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t colon = path.find(':', start);
        const std::string directory =
            path.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
        const fs::path candidate = fs::path(directory.empty() ? "." : directory) / name;
        if (::access(candidate.c_str(), X_OK) == 0) {
            std::error_code ec;
            const fs::path canonical = fs::canonical(candidate, ec);
            if (!ec) {
                resolved = canonical.u8string();
                return ok_error();
            }
        }
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return {ErrorCode::FileRead,
            std::string("command not found on fixed PATH (") + path +
                "): " + name};
#endif
}

// Accept an on-disk executable. When unrestricted is false, require it to live
// under the workspace root (after symlink canonicalization).
Error accept_executable_file(const fs::path& candidate,
                             const fs::path& workspace_root,
                             bool unrestricted,
                             std::string& resolved) {
    std::error_code ec;
    if (!fs::exists(candidate, ec) || ec)
        return {ErrorCode::FileRead, "executable not found: " + candidate.u8string()};
#if defined(_WIN32)
    if (!fs::is_regular_file(candidate, ec) || ec ||
        !supported_windows_executable_extension(candidate))
        return {ErrorCode::BadArgs,
                "path is not a supported Windows executable: " + candidate.u8string()};
#else
    if (::access(candidate.c_str(), X_OK) != 0)
        return {ErrorCode::BadArgs,
                "path is not executable (chmod +x?): " + candidate.u8string()};
#endif
    const fs::path canonical = fs::weakly_canonical(candidate, ec);
    if (ec)
        return {ErrorCode::FileRead,
                "could not resolve executable path: " + candidate.u8string() + ": " +
                    ec.message()};
    if (!unrestricted && !inside_root(workspace_root, canonical))
        return {ErrorCode::BadArgs,
                "executable path must stay inside the workspace: " + canonical.u8string()};
    resolved = canonical.u8string();
    return ok_error();
}

Error resolve_executable(const std::string& name,
                         std::string& resolved,
                         const fs::path* workspace_root = nullptr,
                         const fs::path* cwd = nullptr,
                         bool allow_workspace_executables = false,
                         bool unrestricted = false) {
    if (name.empty())
        return {ErrorCode::BadArgs, "run_command command is empty"};

    // Path form: ./script, scripts/run.sh, /abs/path (Yolo/external only).
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
        if (!allow_workspace_executables || workspace_root == nullptr || cwd == nullptr)
            return {ErrorCode::BadArgs,
                    "run_command requires a bare command name (no path); binaries resolve "
                    "from a fixed PATH, or use Agent workspace scripts (./script.sh)"};
        const fs::path supplied = fs::u8path(name);
        const fs::path candidate =
            supplied.is_absolute() ? supplied : (*cwd / supplied);
        return accept_executable_file(candidate, *workspace_root, unrestricted, resolved);
    }

    // Bare name: the platform resolver path first. POSIX uses the fixed trusted
    // path above; native Windows uses sanitized inherited absolute PATH entries.
    Error path_error = resolve_fixed_path_executable(name, resolved);
    if (path_error.ok()) return ok_error();

    // Agent: bare project scripts (server.sh) that live in cwd or workspace root.
    if (allow_workspace_executables && workspace_root != nullptr && cwd != nullptr) {
        Error local = accept_executable_file(*cwd / name, *workspace_root, unrestricted,
                                             resolved);
        if (local.ok()) return ok_error();
        if (*cwd != *workspace_root) {
            local = accept_executable_file(*workspace_root / name, *workspace_root,
                                           unrestricted, resolved);
            if (local.ok()) return ok_error();
        }
    }
    return path_error;
}

// True when an argv element still looks like a shell operator token (used for
// git pathspecs / revisions). After shell-free tokenization, ordinary program
// data may legally contain ';' '|' etc. (e.g. python3 -c "a; b").
bool dangerous_argument(const std::string& argument) {
    return argument.find_first_of("|;&<>\r\n`") != std::string::npos;
}

// Walk the raw command string with shell-ish quote rules. run_command never
// invokes a shell, so unquoted control operators are rejected as "model meant
// shell syntax", while the same characters inside quotes are payload data.
bool scan_unquoted_shell_syntax(const std::string& command,
                                bool& saw_control_operator,
                                bool& saw_substitution) {
    saw_control_operator = false;
    saw_substitution = false;
    char quote = 0;
    bool escaping = false;
    for (std::size_t index = 0; index < command.size(); ++index) {
        const char ch = command[index];
        if (quote == '\'') {
            if (ch == quote) quote = 0;
            continue;
        }
        if (escaping) {
            escaping = false;
            continue;
        }
        if (ch == '\\') {
#if defined(_WIN32)
            // Backslash is a path separator for direct Windows argv. It never
            // quotes a following shell operator because no shell is involved.
            continue;
#else
            escaping = true;
            continue;
#endif
        }
        if (quote != 0) {
            if (ch == quote) quote = 0;
            continue;
        }
        if (ch == '\'' || ch == '"') {
            quote = ch;
            continue;
        }
        if (ch == '$' && index + 1 < command.size() &&
            (command[index + 1] == '(' || command[index + 1] == '{')) {
            saw_substitution = true;
            return true;
        }
        if (ch == '`' || ch == '|' || ch == '&' || ch == ';' || ch == '<' ||
            ch == '>' || ch == '\n' || ch == '\r') {
            saw_control_operator = true;
            return true;
        }
    }
    return false;
}

bool option_or_assignment(const std::string& argument, const std::string& option) {
    return argument == option || argument.rfind(option + "=", 0) == 0;
}

Error enforce_git_policy(const std::vector<std::string>& args) {
    if (args.size() < 2)
        return {ErrorCode::BadArgs, "git requires an allowlisted read-only subcommand"};
    const std::string& subcommand = args[1];
    if (subcommand == "status") {
        bool paths = false;
        for (std::size_t index = 2; index < args.size(); ++index) {
            const std::string& argument = args[index];
            if (argument == "--") { paths = true; continue; }
            if (paths) continue;
            if (argument == "-s" || argument == "--short" ||
                argument == "-b" || argument == "--branch" ||
                argument == "--porcelain" || argument == "--porcelain=v1" ||
                argument == "--porcelain=v2" ||
                argument == "--untracked-files=no" || argument == "-uno" ||
                argument == "--untracked-files=normal" || argument == "-unormal") {
                continue;
            }
            return {ErrorCode::BadArgs,
                    "git status only permits short/porcelain metadata options and paths after --"};
        }
        return ok_error();
    }
    if (subcommand == "diff") {
        // Read-only workspace/index diffs only. Reject options that write files,
        // run external diffs, open pagers, or rewrite the index.
        bool paths = false;
        for (std::size_t index = 2; index < args.size(); ++index) {
            const std::string& argument = args[index];
            if (argument == "--") {
                paths = true;
                continue;
            }
            if (paths) continue;  // pathspecs after --
            if (argument.empty())
                return {ErrorCode::BadArgs, "git diff rejected an empty argument"};
            if (argument.front() != '-') {
                // Revision or pathspec token (no shell metacharacters).
                if (dangerous_argument(argument))
                    return {ErrorCode::BadArgs, "git diff rejected shell metacharacters"};
                continue;
            }
            // Allowed read-only options.
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
            // Explicitly reject known write/exec forms for clearer errors.
            if (argument == "-O" || argument.rfind("-O", 0) == 0 || argument == "--output" ||
                argument.rfind("--output=", 0) == 0 || argument == "--ext-diff" ||
                argument == "--textconv" || argument == "--no-index" ||
                argument == "--binary") {
                return {ErrorCode::BadArgs,
                        "git diff rejected an option that can write files or invoke external "
                        "tooling"};
            }
            return {ErrorCode::BadArgs,
                    "git diff only permits bounded read-only options "
                    "(--stat/--cached/--name-only and pathspecs)"};
        }
        return ok_error();
    }
    if (subcommand == "ls-files") {
        bool paths = false;
        for (std::size_t index = 2; index < args.size(); ++index) {
            const std::string& argument = args[index];
            if (argument == "--") { paths = true; continue; }
            if (paths) continue;
            if (argument == "-c" || argument == "--cached" ||
                argument == "-o" || argument == "--others" ||
                argument == "--exclude-standard") {
                continue;
            }
            return {ErrorCode::BadArgs,
                    "git ls-files only permits cached/other file listing options and paths after --"};
        }
        return ok_error();
    }
    if (subcommand == "rev-parse") {
        static const std::set<std::string> exact_options = {
            "--show-toplevel", "--show-prefix", "--is-inside-work-tree",
            "--is-bare-repository", "--show-superproject-working-tree"};
        if (args.size() == 3 && exact_options.find(args[2]) != exact_options.end())
            return ok_error();
        if (args.size() == 4 && args[2] == "--abbrev-ref" && args[3] == "HEAD")
            return ok_error();
        return {ErrorCode::BadArgs,
                "git rev-parse only permits fixed workspace and HEAD metadata queries"};
    }
    return {ErrorCode::BadArgs,
            "git subcommand is not available in snapshot-only security review mode: " + subcommand};
}

Error enforce_common_safety(const std::vector<std::string>& args,
                            bool allow_absolute_paths = false) {
    if (args.empty()) return {ErrorCode::BadArgs, "run_command command is empty"};
    // Shell-free execve: after tokenization, argv elements are program data, not
    // shell syntax. Do not reject ';' '|' etc. here — that blocked legitimate
    // payloads such as python3 -c "import x; print(1)". Unquoted operators in
    // the raw command string are rejected in parse_command instead.
    for (const std::string& arg : args) {
#if defined(_WIN32)
        if (arg.size() >= 2 &&
            ((arg[0] >= 'A' && arg[0] <= 'Z') ||
             (arg[0] >= 'a' && arg[0] <= 'z')) &&
            arg[1] == ':') {
            const Error syntax = platform::validate_windows_path_syntax(arg);
            if (!syntax.ok())
                return {ErrorCode::BadArgs,
                        "run_command rejected unsafe Windows path syntax: " +
                            syntax.message};
        }
#endif
        const fs::path possible_path(arg);
        if (possible_path.is_absolute() && !allow_absolute_paths)
            return {ErrorCode::BadArgs, "run_command rejects absolute path arguments"};
        for (const fs::path& component : possible_path) {
            std::string value = component.u8string();
#if defined(_WIN32)
            value = ascii_lower(std::move(value));
#endif
            if (value == ".." || value == ".ainiux-pr" || value == ".ainiux" ||
                value == ".git" || value == ".hg" || value == ".svn") {
                if (retired_project_script_path(arg) ||
                    fs::u8path(arg).generic_u8string().find(".ainiux-pr/script") !=
                        std::string::npos)
                    return {ErrorCode::BadArgs, retired_project_script_message()};
                return {ErrorCode::BadArgs,
                        "run_command rejects traversal and protected metadata paths"};
            }
        }
    }
    return ok_error();
}

Error enforce_inspection_policy(std::vector<std::string>& args) {
    Error error = enforce_common_safety(args);
    if (!error.ok()) return error;
    const std::string& command = args.front();
    if (command == "pwd") {
        if (args.size() != 1)
            return {ErrorCode::BadArgs, "pwd does not accept arguments in security review mode"};
        return ok_error();
    }
    if (command == "ls") {
        std::size_t operands = 0;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "-R" || args[i] == "--recursive")
                return {ErrorCode::BadArgs, "recursive ls is not allowed; use ls or grep"};
            if (!args[i].empty() && args[i].front() == '-' && args[i] != "-1" && args[i] != "--")
                return {ErrorCode::BadArgs, "ls only permits the -1 option in security review mode"};
            if (args[i].empty() || args[i].front() != '-') ++operands;
        }
        if (operands > 1) return {ErrorCode::BadArgs, "ls permits at most one workspace path"};
        return ok_error();
    }
    if (command == "rg" || command == "grep") {
        for (const std::string& arg : args) {
            if (arg == "--pre" || arg.rfind("--pre=", 0) == 0 || arg == "--pre-glob" ||
                arg == "--type-add" || arg == "--type-clear" ||
                option_or_assignment(arg, "--hostname-bin") || arg == "-z" ||
                arg == "--search-zip")
                return {ErrorCode::BadArgs,
                        "run_command rejected an option that can invoke or configure external "
                        "processing"};
            if (arg == "--hidden" || arg == "--no-ignore" || arg == "--no-ignore-vcs" ||
                arg == "-u" || arg == "-uu" || arg == "-uuu" ||
                (command == "grep" && (arg == "-r" || arg == "-R" || arg == "--recursive")))
                return {ErrorCode::BadArgs,
                        "run_command cannot bypass workspace ignore rules or recursively grep"};
            if (arg == "--json" || arg == "--files" || arg == "--files-with-matches" ||
                arg == "--files-without-match" || arg == "-l" || arg == "-L" || arg == "--null" ||
                arg == "-0" || arg == "--null-data")
                return {ErrorCode::BadArgs, "run_command requires line-oriented search output"};
        }
        if (command == "rg")
            args.insert(args.begin() + 1,
                        {"--with-filename", "--line-number", "--no-heading", "--color=never"});
        else
            args.insert(args.begin() + 1, {"-H", "-n"});
        return ok_error();
    }
    if (command == "find") {
        for (const std::string& arg : args) {
            if (arg == "-delete" || arg.rfind("-exec", 0) == 0 || arg.rfind("-ok", 0) == 0 ||
                arg.rfind("-fls", 0) == 0 || arg.rfind("-fprint", 0) == 0 ||
                arg.rfind("-fprintf", 0) == 0 || arg == "-printf" || arg == "-print0" ||
                arg == "-ls")
                return {ErrorCode::BadArgs, "find actions other than printing are not allowed"};
        }
        return ok_error();
    }
    if (command == "git") {
        Error git_error = enforce_git_policy(args);
        if (!git_error.ok()) return git_error;
        const std::string subcommand = args[1];
        args.erase(args.begin() + 1);
        args.insert(args.begin() + 1,
                    {"-c", "core.pager=cat", "-c", "pager.show=false", "-c", "pager.diff=false",
                     "-c", "diff.external=", subcommand});
        if (subcommand == "status") args.insert(args.begin() + 10, "--untracked-files=no");
        return ok_error();
    }
    return {ErrorCode::BadArgs,
            "command is not on the security-review inspection allowlist: " + command +
                " (allowed: pwd, ls, rg, grep, find, git status/diff/…)"};
}

Error enforce_plan_read_only_policy(std::vector<std::string>& args,
                                    bool allow_absolute_paths) {
    Error error = enforce_common_safety(args, allow_absolute_paths);
    if (!error.ok()) return error;
    const ReadOnlyCommandAssessment assessment = assess_read_only_command(args);
    if (!assessment.vetted)
        return {ErrorCode::BadArgs,
                "command is not a vetted read-only Plan invocation" +
                    (assessment.reason.empty() ? std::string()
                                               : ": " + assessment.reason)};
    return ok_error();
}

// Inject git -c pager/external-diff hardening and return subcommand name.
Error harden_git_argv(std::vector<std::string>& args, std::string& subcommand) {
    if (args.size() < 2) return {ErrorCode::BadArgs, "git requires a subcommand"};
    subcommand = args[1];
    args.erase(args.begin() + 1);
    args.insert(args.begin() + 1,
                {"-c", "core.pager=cat", "-c", "pager.show=false", "-c", "pager.diff=false",
                 "-c", "diff.external=", subcommand});
    return ok_error();
}

// Agent-mode git: no subcommand allowlist. Harden pagers; Guard covers destructive forms.
// Block only path/config overrides that escape the workspace runner model.
Error enforce_agent_git_policy(std::vector<std::string>& args) {
    if (args.size() < 2) return {ErrorCode::BadArgs, "git requires a subcommand"};
    const std::string& sub = args[1];
    if (sub == "config" || sub == "filter-branch" || sub == "update-ref" || sub == "replace")
        return {ErrorCode::BadArgs, "git " + sub + " is not allowed via run_command"};

    for (std::size_t i = 2; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--exec-path" || arg.rfind("--exec-path=", 0) == 0 ||
            arg == "--git-dir" || arg.rfind("--git-dir=", 0) == 0 ||
            arg == "--work-tree" || arg.rfind("--work-tree=", 0) == 0)
            return {ErrorCode::BadArgs, "git rejected a path-override option"};
    }
    std::string ignored;
    return harden_git_argv(args, ignored);
}

Error apply_guard_decision(const std::vector<std::string>& args,
                           GuardAskHandling ask_handling,
                           const GuardApprovalCallback* on_guard_ask,
                           runtime::CancellationToken cancellation,
                           std::string& guard_rule_id,
                           std::string& guard_decision_out) {
    guard_rule_id.clear();
    guard_decision_out = "allow";
    GuardResult guard = evaluate_command_guard(args);
    if (guard.decision == GuardDecision::Allow) {
        guard_decision_out = "allow";
        return ok_error();
    }
    if (guard.decision == GuardDecision::Deny) {
        guard_rule_id = guard.rule_id;
        guard_decision_out = "deny";
        return {ErrorCode::BadArgs, guard.message.empty()
                                        ? "command blocked by destructive-command guard"
                                        : guard.message};
    }

    // Ask
    guard_rule_id = guard.rule_id;
    if (ask_handling == GuardAskHandling::DeferAsk) {
        // Path-validation pass only; execute will re-check and prompt.
        guard_decision_out = "ask";
        return ok_error();
    }
    if (ask_handling == GuardAskHandling::PromptAsk && on_guard_ask && *on_guard_ask) {
        GuardApprovalRequest request;
        request.tool_name = "run";
        request.command_preview = format_command_preview(args);
        request.rule_id = guard.rule_id;
        request.message = guard.message;
        request.arguments = args;
        const GuardApprovalDecision decision = (*on_guard_ask)(request, cancellation);
        if (decision == GuardApprovalDecision::Allow) {
            guard_decision_out = "allow";
            return ok_error();
        }
        guard_decision_out = decision == GuardApprovalDecision::Cancelled ? "cancelled" : "deny";
        std::string message = guard.message.empty()
                                  ? "command requires approval"
                                  : guard.message;
        if (decision == GuardApprovalDecision::Cancelled)
            message += " (approval cancelled)";
        else
            message += " (user denied approval)";
        return {decision == GuardApprovalDecision::Cancelled ? ErrorCode::Cancelled
                                                             : ErrorCode::BadArgs,
                message};
    }

    // DenyAsk or missing callback: headless Deny.
    GuardResult denied = finalize_guard_for_headless(guard);
    guard_decision_out = "deny";
    return {ErrorCode::BadArgs, denied.message.empty()
                                    ? "command blocked by destructive-command guard"
                                    : denied.message};
}

Error enforce_agent_policy(std::vector<std::string>& args,
                           std::string& guard_rule_id,
                           GuardAskHandling ask_handling,
                           const GuardApprovalCallback* on_guard_ask,
                           runtime::CancellationToken cancellation,
                           std::string& guard_decision_out,
                           bool allow_absolute_paths,
                           bool unrestricted = false) {
    guard_rule_id.clear();
    guard_decision_out = "allow";
    Error error = enforce_common_safety(args, allow_absolute_paths || unrestricted);
    if (!error.ok()) return error;

    // Denylist / Ask first: free-form shells, privilege escalation, disk destroyers,
    // rm -rf, etc. Agent mode intentionally does NOT maintain a command/option
    // allowlist — Linux has thousands of mostly harmless tools; structural safety +
    // Guard scales better. Interactive Yolo skips hard Guard denials at the user's
    // risk (workspace script paths and ordinary tools already work in Smart).
    if (!unrestricted) {
        error = apply_guard_decision(args, ask_handling, on_guard_ask, cancellation,
                                     guard_rule_id, guard_decision_out);
        if (!error.ok()) return error;
    } else {
        guard_decision_out = "allow";
        guard_rule_id.clear();
    }

    const std::string& command = args.front();
    if (command == "command" &&
        (args.size() < 3 || args[1] != "-v"))
        return {ErrorCode::BadArgs,
                "run_command supports the shell builtin only as: command -v NAME"};
    // Soft hardening only where the runner must inject safe defaults (git pagers).
    // Do not option-allowlist ordinary tools (ls, stat, cat, …).
    if (command == "git") return enforce_agent_git_policy(args);

    // Optional rg line-oriented defaults (same inserts as inspection when model uses rg).
    if (command == "rg") {
        for (const std::string& arg : args) {
            if (arg == "--pre" || arg.rfind("--pre=", 0) == 0 || arg == "--pre-glob" ||
                arg == "--type-add" || arg == "--type-clear" ||
                option_or_assignment(arg, "--hostname-bin") || arg == "-z" ||
                arg == "--search-zip")
                return {ErrorCode::BadArgs,
                        "run_command rejected an option that can invoke or configure external "
                        "processing"};
        }
        args.insert(args.begin() + 1,
                    {"--with-filename", "--line-number", "--no-heading", "--color=never"});
    }
    return ok_error();
}

Error tokenize_command(const std::string& command, std::vector<std::string>& arguments) {
    arguments.clear();
    std::string current;
    char quote = 0;
    bool escaping = false;
    bool token_started = false;
    for (char ch : command) {
        if (quote == '\'') {
            if (ch == quote)
                quote = 0;
            else
                current.push_back(ch);
            continue;
        }
        if (escaping) {
            current.push_back(ch);
            escaping = false;
            token_started = true;
            continue;
        }
        if (ch == '\\') {
#if defined(_WIN32)
            current.push_back(ch);
            token_started = true;
#else
            escaping = true;
            token_started = true;
#endif
            continue;
        }
        if (quote != 0) {
            if (ch == quote)
                quote = 0;
            else
                current.push_back(ch);
            continue;
        }
        if (ch == '\'' || ch == '"') {
            quote = ch;
            token_started = true;
            continue;
        }
        if (ch == ' ' || ch == '\t') {
            if (token_started) {
                arguments.push_back(std::move(current));
                current.clear();
                token_started = false;
            }
            continue;
        }
        current.push_back(ch);
        token_started = true;
    }
    if (escaping || quote != 0)
        return {ErrorCode::BadArgs, "run_command contains an incomplete quote or escape"};
    if (token_started) arguments.push_back(std::move(current));
    return ok_error();
}

}  // namespace

std::vector<std::string> agent_command_environment(const std::string& resolved_executable) {
    std::vector<std::string> environment = {
        std::string("PATH=") + fixed_command_path(),
#if defined(_WIN32)
        "PAGER=cat",
        "GIT_PAGER=cat",
        "GIT_EXTERNAL_DIFF=",
        "GIT_OPTIONAL_LOCKS=0",
        "RIPGREP_CONFIG_PATH=NUL"};
    const char* inherited_names[] = {"SystemRoot", "WINDIR", "HOME", "USERPROFILE",
                                     "TEMP", "TMP"};
    for (const char* name : inherited_names) append_inherited_environment(environment, name);
    environment.push_back("PATHEXT=.COM;.EXE;.BAT;.CMD");
    const std::string system_root_for_env = platform::environment_value("SystemRoot");
    if (!system_root_for_env.empty() && !environment_has_name(environment, "COMSPEC"))
        environment.push_back(
            "COMSPEC=" +
            (std::filesystem::u8path(system_root_for_env) / "System32" / "cmd.exe").u8string());
#else
        "LC_ALL=C.UTF-8",
        "LANG=C.UTF-8",
        "PAGER=cat",
        "GIT_PAGER=cat",
        "GIT_EXTERNAL_DIFF=",
        "GIT_OPTIONAL_LOCKS=0",
        "RIPGREP_CONFIG_PATH=/dev/null"};
    append_inherited_environment(environment, "HOME");
    append_inherited_environment(environment, "USER");
#endif
    if (same_running_executable(resolved_executable)) {
        append_inherited_environment(environment, "XDG_CONFIG_HOME");
        append_inherited_environment(environment, "XDG_CONFIG_DIRS");
        std::set<std::string> key_names = {"AINIUX_API_KEY"};
        for (const provider::Profile& profile : provider::built_in_profiles()) {
            for (const std::string& name : profile.key_envs) key_names.insert(name);
        }
        for (const std::string& name : key_names)
            append_inherited_environment(environment, name.c_str());
    }
    return environment;
}

Error parse_inspection_command(const std::string& command, std::vector<std::string>& arguments) {
    std::string unused_rule;
    return parse_command(command, arguments, CommandPolicy::InspectionOnly, unused_rule);
}

Error parse_command(const std::string& command,
                    std::vector<std::string>& arguments,
                    CommandPolicy policy,
                    std::string& guard_rule_id,
                    GuardAskHandling ask_handling,
                    const GuardApprovalCallback* on_guard_ask,
                    runtime::CancellationToken cancellation,
                    bool allow_absolute_paths,
                    bool unrestricted) {
    guard_rule_id.clear();
    arguments.clear();
    {
        bool control = false;
        bool substitution = false;
        if (scan_unquoted_shell_syntax(command, control, substitution)) {
            if (substitution)
                return {ErrorCode::BadArgs,
                        "run_command rejected unquoted shell substitutions "
                        "($... / `...`); it never runs a shell"};
            return {ErrorCode::BadArgs,
                    "run_command is shell-free and rejects unquoted shell control "
                    "operators (| & ; < > ` and newlines). Quote them when they are "
                    "program data, or write scripts/ainiux/NAME and run that file "
                    "instead of chaining commands"};
        }
    }
    Error error = tokenize_command(command, arguments);
    if (!error.ok()) return error;
    if (policy == CommandPolicy::Agent) {
        std::string unused_decision;
        return enforce_agent_policy(arguments, guard_rule_id, ask_handling, on_guard_ask,
                                    cancellation, unused_decision, allow_absolute_paths,
                                    unrestricted);
    }
    if (policy == CommandPolicy::PlanReadOnly)
        return enforce_plan_read_only_policy(arguments, allow_absolute_paths);
    return enforce_inspection_policy(arguments);
}

Error run_inspection_command(const std::string& command,
                             const ProcessOptions& options,
                             ProcessResult& result) {
    return run_command(command, options, result, CommandPolicy::InspectionOnly);
}

Error execute_resolved_command(ProcessResult& output,
                               const ProcessOptions& options,
                               CommandPolicy policy) {
    Error error = ok_error();
    fs::path root;
    fs::path cwd;
    if (!(error = resolve_cwd(options, root, cwd)).ok()) return error;
    output.cwd = cwd.u8string();
    if (output.arguments.empty())
        return {ErrorCode::BadArgs, "run_command command is empty"};
    const bool workspace_exec =
        policy == CommandPolicy::Agent && options.allow_workspace_executables;
    if (output.arguments.front() == "command") {
        const auto started = std::chrono::steady_clock::now();
        bool found_all = true;
        for (std::size_t index = 2; index < output.arguments.size(); ++index) {
            std::string found;
            if (resolve_executable(output.arguments[index], found, &root, &cwd,
                                   workspace_exec, options.unrestricted)
                    .ok())
                output.stdout_text += found + "\n";
            else
                found_all = false;
        }
        output.exit_status = found_all ? 0 : 1;
        output.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
        output.policy =
            policy == CommandPolicy::Agent ? "allowed-agent" : "allowed-read-only";
        if (output.guard_decision.empty()) output.guard_decision = "allow";
        return ok_error();
    }
    std::string executable;
    if (!(error = resolve_executable(output.arguments.front(), executable, &root, &cwd,
                                     workspace_exec, options.unrestricted))
             .ok())
        return error;
    output.resolved_executable = executable;

    runtime::SubprocessOptions subprocess;
    subprocess.executable = executable;
    subprocess.arguments.assign(output.arguments.begin() + 1, output.arguments.end());
    subprocess.cwd = cwd.u8string();
    subprocess.timeout_ms = options.timeout_ms;
    subprocess.stdout_limit = options.stdout_limit;
    subprocess.stderr_limit = options.stderr_limit;
    subprocess.background = options.background;
    subprocess.startup_ms = options.startup_ms;
    subprocess.cancellation = options.cancellation;
    subprocess.environment = agent_command_environment(executable);
#if defined(_WIN32)
    const std::string extension = ascii_lower(fs::u8path(executable).extension().u8string());
    if (extension == ".bat" || extension == ".cmd") {
        const std::string system_root = platform::environment_value("SystemRoot");
        if (system_root.empty())
            return {ErrorCode::Config,
                    "SystemRoot is not set; cannot resolve cmd.exe for batch command"};
        const fs::path cmd = fs::u8path(system_root) / "System32" / "cmd.exe";
        std::error_code cmd_error;
        const fs::path resolved_cmd = fs::canonical(cmd, cmd_error);
        if (cmd_error || !fs::is_regular_file(resolved_cmd, cmd_error))
            return {ErrorCode::FileRead,
                    "could not resolve cmd.exe for batch command: " + cmd.u8string()};
        subprocess.executable = resolved_cmd.u8string();
        subprocess.arguments.insert(subprocess.arguments.begin(), executable);
        subprocess.windows_batch = true;
    }
#endif
    runtime::SubprocessResult process_result;
    error = runtime::run_subprocess(subprocess, process_result);
    output.stdout_text = std::move(process_result.stdout_text);
    output.stderr_text = std::move(process_result.stderr_text);
    output.exit_status = process_result.exit_code;
    output.signal = process_result.signal;
    output.duration_ms = process_result.duration_ms;
    output.stdout_truncated = process_result.stdout_truncated;
    output.stderr_truncated = process_result.stderr_truncated;
    output.cancelled = process_result.termination ==
                       runtime::SubprocessTerminationReason::Cancelled;
    output.timed_out = process_result.termination ==
                       runtime::SubprocessTerminationReason::TimedOut;
    output.background = process_result.background;
    output.pid = process_result.pid;
    output.policy =
        policy == CommandPolicy::Agent ? "allowed-agent" : "allowed-read-only";
    if (output.guard_decision.empty()) output.guard_decision = "allow";
    return error;
}

Error run_command(const std::string& command,
                  const ProcessOptions& options,
                  ProcessResult& result,
                  CommandPolicy policy) {
    ProcessResult output;
    const GuardAskHandling ask_handling =
        policy == CommandPolicy::Agent
            ? (options.on_guard_ask ? GuardAskHandling::PromptAsk
                                    : GuardAskHandling::DenyAsk)
            : GuardAskHandling::DenyAsk;
    const GuardApprovalCallback* ask_ptr =
        options.on_guard_ask ? &options.on_guard_ask : nullptr;
    Error error = parse_command(command, output.arguments, policy, output.guard_rule_id,
                                ask_handling, ask_ptr, options.cancellation,
                                options.allow_external_paths, options.unrestricted);
    if (policy == CommandPolicy::Agent && error.ok()) {
        output.guard_decision = "allow";
    } else if (policy == CommandPolicy::Agent && !error.ok()) {
        output.guard_decision =
            error.code == ErrorCode::Cancelled ? "cancelled" : "deny";
    }
    if (!error.ok()) {
        result = std::move(output);
        return error;
    }
    error = execute_resolved_command(output, options, policy);
    result = std::move(output);
    return error;
}

Error run_argv(std::vector<std::string> arguments,
               const ProcessOptions& options,
               ProcessResult& result,
               CommandPolicy policy) {
    ProcessResult output;
    output.arguments = std::move(arguments);
    const GuardAskHandling ask_handling =
        policy == CommandPolicy::Agent
            ? (options.on_guard_ask ? GuardAskHandling::PromptAsk
                                    : GuardAskHandling::DenyAsk)
            : GuardAskHandling::DenyAsk;
    const GuardApprovalCallback* ask_ptr =
        options.on_guard_ask ? &options.on_guard_ask : nullptr;
    std::string unused_decision;
    Error error = ok_error();
    if (policy == CommandPolicy::Agent) {
        error = enforce_agent_policy(output.arguments, output.guard_rule_id,
                                     ask_handling, ask_ptr, options.cancellation,
                                     unused_decision, options.allow_external_paths,
                                     options.unrestricted);
        if (error.ok())
            output.guard_decision = "allow";
        else
            output.guard_decision =
                error.code == ErrorCode::Cancelled ? "cancelled" : "deny";
    } else if (policy == CommandPolicy::PlanReadOnly) {
        error = enforce_plan_read_only_policy(output.arguments,
                                              options.allow_external_paths);
    } else {
        error = enforce_inspection_policy(output.arguments);
    }
    if (!error.ok()) {
        result = std::move(output);
        return error;
    }
    error = execute_resolved_command(output, options, policy);
    result = std::move(output);
    return error;
}

bool ripgrep_available() {
    std::string resolved;
    return resolve_fixed_path_executable("rg", resolved).ok();
}

}  // namespace ainiux::agent
