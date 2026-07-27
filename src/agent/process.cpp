#include "agent/process.hpp"

#include "agent/command_guard.hpp"
#include "agent/read_only_command.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <set>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace ainiux::agent {
namespace {
namespace fs = std::filesystem;

class Pipe {
   public:
    ~Pipe() { close_all(); }
    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;
    Pipe() = default;
    Error open() {
        if (::pipe(fds_) != 0) return {ErrorCode::Internal, "could not create process pipe: " + std::string(std::strerror(errno))};
        return ok_error();
    }
    int read_fd() const { return fds_[0]; }
    int write_fd() const { return fds_[1]; }
    int release_read() { const int fd = fds_[0]; fds_[0] = -1; return fd; }
    void close_read() { close_one(0); }
    void close_write() { close_one(1); }
   private:
    int fds_[2] = {-1, -1};
    void close_one(int index) { if (fds_[index] >= 0) { ::close(fds_[index]); fds_[index] = -1; } }
    void close_all() { close_one(0); close_one(1); }
};

bool inside_root(const fs::path& root, const fs::path& candidate) {
    auto root_it = root.begin();
    auto path_it = candidate.begin();
    for (; root_it != root.end(); ++root_it, ++path_it) {
        if (path_it == candidate.end() || *root_it != *path_it) return false;
    }
    return true;
}

Error resolve_cwd(const ProcessOptions& options, fs::path& root, fs::path& cwd) {
    std::error_code ec;
    root = fs::canonical(fs::absolute(options.workspace, ec), ec);
    if (ec || !fs::is_directory(root, ec))
        return {ErrorCode::FileRead, "could not resolve command workspace: " + options.workspace};
    const fs::path supplied(options.cwd);
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

Error resolve_executable(const std::string& name, std::string& resolved) {
    if (name.find('/') != std::string::npos)
        return {ErrorCode::BadArgs,
                "run_command requires a bare command name (no path); binaries resolve from a fixed PATH"};
    // Never resolve a workspace-controlled executable through the caller's PATH.
    const std::string path = "/usr/local/bin:/usr/bin:/bin";
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t colon = path.find(':', start);
        const std::string directory = path.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
        const fs::path candidate = fs::path(directory.empty() ? "." : directory) / name;
        if (::access(candidate.c_str(), X_OK) == 0) {
            std::error_code ec;
            const fs::path canonical = fs::canonical(candidate, ec);
            if (!ec) { resolved = canonical.string(); return ok_error(); }
        }
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return {ErrorCode::FileRead, "command not found on fixed PATH (/usr/local/bin:/usr/bin:/bin): " + name};
}

bool dangerous_argument(const std::string& argument) {
    return argument.find_first_of("|;&<>\r\n`") != std::string::npos ||
           argument.find("$(") != std::string::npos || argument.find("${") != std::string::npos;
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
    for (const std::string& arg : args) {
        if (dangerous_argument(arg))
            return {ErrorCode::BadArgs, "run_command rejected shell metacharacters or substitutions"};
        const fs::path possible_path(arg);
        if (possible_path.is_absolute() && !allow_absolute_paths)
            return {ErrorCode::BadArgs, "run_command rejects absolute path arguments"};
        for (const fs::path& component : possible_path) {
            const std::string value = component.string();
            if (value == ".." || value == ".ainiux-pr" || value == ".ainiux" || value == ".git" ||
                value == ".hg" || value == ".svn")
                return {ErrorCode::BadArgs,
                        "run_command rejects traversal and protected metadata paths"};
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
                return {ErrorCode::BadArgs, "recursive ls is not allowed; use list_directory or find"};
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
        request.tool_name = "run_command";
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
                           bool allow_absolute_paths) {
    guard_rule_id.clear();
    guard_decision_out = "allow";
    Error error = enforce_common_safety(args, allow_absolute_paths);
    if (!error.ok()) return error;

    // Denylist / Ask first: shells, privilege escalation, disk destroyers, rm -rf, etc.
    // Agent mode intentionally does NOT maintain a command/option allowlist — Linux has
    // thousands of mostly harmless tools; structural safety + Guard scales better.
    error = apply_guard_decision(args, ask_handling, on_guard_ask, cancellation, guard_rule_id,
                                 guard_decision_out);
    if (!error.ok()) return error;

    const std::string& command = args.front();
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
        if (escaping) {
            current.push_back(ch);
            escaping = false;
            token_started = true;
            continue;
        }
        if (ch == '\\') {
            escaping = true;
            token_started = true;
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

void append_bounded(std::string& output, const char* data, std::size_t count,
                    std::size_t limit, bool& truncated) {
    const std::size_t remaining = output.size() < limit ? limit - output.size() : 0;
    const std::size_t accepted = std::min(remaining, count);
    output.append(data, accepted);
    if (accepted != count) truncated = true;
}

void drain_fd(int fd, std::string& output, std::size_t limit, bool& truncated, bool& open) {
    char buffer[8192];
    while (true) {
        const ssize_t count = ::read(fd, buffer, sizeof(buffer));
        if (count > 0) { append_bounded(output, buffer, static_cast<std::size_t>(count), limit, truncated); continue; }
        if (count == 0) { ::close(fd); open = false; return; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        if (errno == EINTR) continue;
        ::close(fd); open = false; return;
    }
}

}  // namespace

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
                    bool allow_absolute_paths) {
    guard_rule_id.clear();
    Error error = tokenize_command(command, arguments);
    if (!error.ok()) return error;
    if (policy == CommandPolicy::Agent) {
        std::string unused_decision;
        return enforce_agent_policy(arguments, guard_rule_id, ask_handling, on_guard_ask,
                                    cancellation, unused_decision, allow_absolute_paths);
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

Error run_command(const std::string& command,
                  const ProcessOptions& options,
                  ProcessResult& result,
                  CommandPolicy policy) {
    ProcessResult output;
    const GuardAskHandling ask_handling =
        policy == CommandPolicy::Agent
            ? (options.on_guard_ask ? GuardAskHandling::PromptAsk : GuardAskHandling::DenyAsk)
            : GuardAskHandling::DenyAsk;
    const GuardApprovalCallback* ask_ptr =
        options.on_guard_ask ? &options.on_guard_ask : nullptr;
    Error error = parse_command(command, output.arguments, policy, output.guard_rule_id,
                                ask_handling, ask_ptr, options.cancellation,
                                options.allow_external_paths);
    // Re-run with decision capture for agent policy (parse_command dropped it).
    if (policy == CommandPolicy::Agent && error.ok()) {
        // parse already applied guard; leave decision as allow when ok.
        output.guard_decision = "allow";
    } else if (policy == CommandPolicy::Agent && !error.ok()) {
        output.guard_decision =
            error.code == ErrorCode::Cancelled ? "cancelled" : "deny";
    }
    if (!error.ok()) {
        result = std::move(output);
        return error;
    }
    fs::path root;
    fs::path cwd;
    if (!(error = resolve_cwd(options, root, cwd)).ok()) { result = std::move(output); return error; }
    output.cwd = cwd.string();
    std::string executable;
    if (!(error = resolve_executable(output.arguments.front(), executable)).ok()) { result = std::move(output); return error; }

    // Prepare every allocation before fork. Security review may have several
    // worker threads; the child must call only async-signal-safe functions
    // until execve replaces the process image.
    std::vector<char*> argv;
    argv.reserve(output.arguments.size() + 1);
    for (std::string& item : output.arguments) argv.push_back(item.data());
    argv.push_back(nullptr);
    std::vector<std::string> environment_storage = {
        "PATH=/usr/local/bin:/usr/bin:/bin", "LC_ALL=C.UTF-8", "LANG=C.UTF-8",
        "PAGER=cat", "GIT_PAGER=cat", "GIT_EXTERNAL_DIFF=", "GIT_OPTIONAL_LOCKS=0",
        "RIPGREP_CONFIG_PATH=/dev/null"};
    std::vector<char*> environment;
    environment.reserve(environment_storage.size() + 1);
    for (std::string& item : environment_storage) environment.push_back(item.data());
    environment.push_back(nullptr);

    Pipe stdout_pipe;
    Pipe stderr_pipe;
    if (!(error = stdout_pipe.open()).ok() || !(error = stderr_pipe.open()).ok()) return error;
    const auto started = std::chrono::steady_clock::now();
    const pid_t pid = ::fork();
    if (pid < 0) return {ErrorCode::Internal, "could not fork inspection command: " + std::string(std::strerror(errno))};
    if (pid == 0) {
        ::setpgid(0, 0);
        stdout_pipe.close_read();
        stderr_pipe.close_read();
        const int null_fd = ::open("/dev/null", O_RDONLY);
        if (null_fd >= 0) { ::dup2(null_fd, STDIN_FILENO); ::close(null_fd); }
        ::dup2(stdout_pipe.write_fd(), STDOUT_FILENO);
        ::dup2(stderr_pipe.write_fd(), STDERR_FILENO);
        stdout_pipe.close_write();
        stderr_pipe.close_write();
        if (::chdir(cwd.c_str()) != 0) _exit(126);
        ::execve(executable.c_str(), argv.data(), environment.data());
        _exit(errno == ENOENT ? 127 : 126);
    }

    ::setpgid(pid, pid);
    stdout_pipe.close_write();
    stderr_pipe.close_write();
    ::fcntl(stdout_pipe.read_fd(), F_SETFL, ::fcntl(stdout_pipe.read_fd(), F_GETFL) | O_NONBLOCK);
    ::fcntl(stderr_pipe.read_fd(), F_SETFL, ::fcntl(stderr_pipe.read_fd(), F_GETFL) | O_NONBLOCK);
    int stdout_fd = stdout_pipe.release_read();
    int stderr_fd = stderr_pipe.release_read();
    bool stdout_open = true;
    bool stderr_open = true;
    int wait_status = 0;
    bool reaped = false;
    bool terminated = false;
    long long terminated_at = 0;
    while (!reaped || stdout_open || stderr_open) {
        const auto now = std::chrono::steady_clock::now();
        const long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count();
        if (!terminated && (options.cancellation.cancelled() || elapsed > options.timeout_ms)) {
            output.cancelled = options.cancellation.cancelled();
            output.timed_out = !output.cancelled;
            ::kill(-pid, SIGTERM);
            terminated = true;
            terminated_at = elapsed;
        }
        if (terminated && elapsed > terminated_at + 250) ::kill(-pid, SIGKILL);
        pollfd fds[2] = {{stdout_fd, static_cast<short>(stdout_open ? POLLIN | POLLHUP : 0), 0},
                         {stderr_fd, static_cast<short>(stderr_open ? POLLIN | POLLHUP : 0), 0}};
        ::poll(fds, 2, 25);
        if (stdout_open) drain_fd(stdout_fd, output.stdout_text, options.stdout_limit, output.stdout_truncated, stdout_open);
        if (stderr_open) drain_fd(stderr_fd, output.stderr_text, options.stderr_limit, output.stderr_truncated, stderr_open);
        if (!reaped) {
            const pid_t waited = ::waitpid(pid, &wait_status, WNOHANG);
            if (waited == pid) reaped = true;
            else if (waited < 0 && errno != EINTR) reaped = true;
        }
    }
    output.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started).count();
    if (WIFEXITED(wait_status)) output.exit_status = WEXITSTATUS(wait_status);
    if (WIFSIGNALED(wait_status)) output.signal = WTERMSIG(wait_status);
    output.policy =
        policy == CommandPolicy::Agent ? "allowed-agent" : "allowed-read-only";
    if (output.guard_decision.empty()) output.guard_decision = "allow";
    result = std::move(output);
    if (result.cancelled) return {ErrorCode::Cancelled, "run_command cancelled"};
    if (result.timed_out) return {ErrorCode::Timeout, "run_command exceeded its timeout"};
    return ok_error();
}

}  // namespace ainiux::agent
