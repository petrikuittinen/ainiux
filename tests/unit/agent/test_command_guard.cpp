#include "agent/test_command_guard.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <vector>

#include "agent/approval.hpp"
#include "agent/command_guard.hpp"
#include "agent/index/index.hpp"
#include "agent/process.hpp"
#include "agent/read_only_command.hpp"
#include "agent/tools.hpp"
#include "json/json.hpp"
#include "platform/environment.hpp"
#include "support/test_support.hpp"

#include <atomic>
#include <chrono>
#include <thread>

namespace ainiux::test::agent_command_guard {
namespace {
using ainiux::test::check;
namespace fs = std::filesystem;

void test_guard_patterns() {
    auto deny = [](const std::vector<std::string>& args) {
        const agent::GuardResult result =
            agent::finalize_guard_for_headless(agent::evaluate_command_guard(args));
        return result.decision == agent::GuardDecision::Deny;
    };
    check(deny({"rm", "-rf", "build"}), "rm -rf denied");
    check(deny({"rm", "-fr", "build"}), "rm -fr denied");
    check(deny({"rm", "-r", "-f", "build"}), "rm -r -f denied");
    check(deny({"git", "reset", "--hard"}), "git reset --hard denied");
    check(deny({"git", "clean", "-fdx"}), "git clean -fdx denied");
    check(deny({"git", "push", "--force"}), "git push --force denied");
    check(deny({"find", ".", "-delete"}), "find -delete denied");
    check(deny({"sqlite3", "app.sqlite", "DROP TABLE users;"}), "destructive sql denied");
    check(deny({"bash", "-c", "echo hi"}), "bash -c free-form denied");
    check(agent::evaluate_command_guard({"bash", "server.sh", "start"}).decision ==
              agent::GuardDecision::Allow,
          "bash script-file form allowed by guard");
    check(agent::evaluate_command_guard({"sh", "./scripts/setup.sh"}).decision ==
              agent::GuardDecision::Allow,
          "sh ./script form allowed by guard");
    check(deny({"sudo", "make"}), "sudo denied");
    check(deny({"shutdown.exe", "/s"}), "Windows shutdown.exe denied");
    check(deny({"diskpart.exe"}), "Windows diskpart.exe denied");
    check(deny({"reg.exe", "delete", "HKCU\\Software\\Example"}),
          "Windows registry deletion denied");
    check(deny({"del.cmd", "/q", "data.db"}), "Windows del.cmd guarded");
    check(deny({"rmdir", "/s", "/q", "build"}), "Windows recursive rmdir guarded");
    check(deny({"powershell.exe", "-EncodedCommand", "AAAA"}),
          "PowerShell encoded command denied");
    check(deny({"Remove-Item", "-Recurse", "build"}),
          "destructive PowerShell cmdlet guarded");
    check(deny({"apt-get", "install", "curl"}), "system package manager denied");
    check(deny({"ssh", "host"}), "remote shell denied");
    check(deny({"reboot"}), "host control denied");

    const agent::GuardResult allow =
        agent::evaluate_command_guard({"python3", "hello.py"});
    check(allow.decision == agent::GuardDecision::Allow, "python3 hello.py allowed by guard");
    check(agent::evaluate_command_guard({"make", "test"}).decision == agent::GuardDecision::Allow,
          "make test allowed by guard");
    check(agent::evaluate_command_guard({"stat", "file.py"}).decision == agent::GuardDecision::Allow,
          "stat allowed by guard");
    check(agent::evaluate_command_guard({"touch", "x"}).decision == agent::GuardDecision::Allow,
          "touch allowed by guard (not an allowlist)");
}

#if defined(_WIN32)
void test_windows_direct_argv_paths() {
    std::vector<std::string> arguments;
    std::string rule;
    Error error = agent::parse_command(
        R"CMD(python.exe -c "print('C:\work\file.txt')")CMD", arguments,
        agent::CommandPolicy::Agent, rule, agent::GuardAskHandling::DenyAsk,
        nullptr, {}, true, true);
    check(error.ok() && arguments.size() == 3 &&
              arguments[2].find(R"(C:\work\file.txt)") != std::string::npos,
          "Windows direct argv tokenizer preserves backslash path separators");
    error = agent::parse_command(
        R"(type.exe C:relative\file.txt)", arguments, agent::CommandPolicy::Agent,
        rule, agent::GuardAskHandling::DenyAsk, nullptr, {}, true, true);
    check(!error.ok() && error.message.find("drive-relative") != std::string::npos,
          "Windows run_command rejects ambiguous drive-relative path arguments");
}
#endif

void test_parse_policies() {
    std::vector<std::string> args;
    std::string rule;
    Error error = agent::parse_command("python3 hello.py", args, agent::CommandPolicy::InspectionOnly, rule);
    check(!error.ok() && error.message.find("security-review") != std::string::npos,
          "inspection policy rejects python3: " + error.message);

    args.clear();
    error = agent::parse_command("python3 hello.py", args, agent::CommandPolicy::Agent, rule);
    check(error.ok(), "agent policy accepts python3: " + error.message);
    check(args.size() == 2 && args[0] == "python3" && args[1] == "hello.py", "python3 argv");

    error = agent::parse_command("rm -rf build", args, agent::CommandPolicy::Agent, rule);
    check(!error.ok() && !rule.empty(), "agent policy guards rm -rf: " + error.message);

    // Agent default-allow: ordinary tools/options are not option-allowlisted.
    error = agent::parse_command("ls -laFg tic_tac_toe.py", args, agent::CommandPolicy::Agent, rule);
    check(error.ok(), "agent default-allow accepts ordinary ls options: " + error.message);

    error = agent::parse_command("stat -c %y tic_tac_toe.py", args, agent::CommandPolicy::Agent,
                                 rule);
    check(error.ok(), "agent default-allow accepts stat: " + error.message);

    error = agent::parse_command("touch notes.txt", args, agent::CommandPolicy::Agent, rule);
    check(error.ok(), "agent default-allow accepts touch (not an allowlist game): " + error.message);

    error = agent::parse_command(
        R"(dpkg-query -W -f='${Status} ${Version}\n' apache2)", args,
        agent::CommandPolicy::Agent, rule);
    check(error.ok() && args.size() == 4 && args[2] == "-f=${Status} ${Version}\\n",
          "agent preserves quoted literal package-query formats: " + error.message);
    error = agent::parse_command("python3 ${UNQUOTED}", args,
                                 agent::CommandPolicy::Agent, rule);
    check(!error.ok() && error.message.find("unquoted shell substitutions") != std::string::npos,
          "agent still rejects unquoted shell substitutions");

    error = agent::parse_command(
        R"CMD(python3 -c "import readline; print('readline available')")CMD",
        args, agent::CommandPolicy::Agent, rule);
    check(error.ok() && args.size() == 3 && args[2].find(';') != std::string::npos,
          "agent accepts quoted python -c multi-statement payload: " + error.message);

    error = agent::parse_command("echo hi | wc -l", args, agent::CommandPolicy::Agent, rule);
    check(!error.ok() && error.message.find("shell-free") != std::string::npos,
          "agent rejects unquoted pipe as shell syntax: " + error.message);

    error = agent::parse_command("bash -c echo", args, agent::CommandPolicy::Agent, rule);
    check(!error.ok(), "agent still denylists free-form bash -c: " + error.message);

    error = agent::parse_command("bash server.sh start", args, agent::CommandPolicy::Agent,
                                 rule);
    check(error.ok() && args.size() == 3 && args[0] == "bash" && args[1] == "server.sh",
          "agent accepts bash script-file invocations: " + error.message);

    error = agent::parse_command("./server.sh start", args, agent::CommandPolicy::Agent, rule);
    check(error.ok() && args.size() == 2 && args[0] == "./server.sh",
          "agent accepts relative workspace script paths: " + error.message);

    error = agent::parse_command("bash -c echo", args, agent::CommandPolicy::Agent, rule,
                                 agent::GuardAskHandling::DenyAsk, nullptr, {}, false,
                                 true);
    check(error.ok(),
          "Yolo unrestricted accepts free-form bash -c: " + error.message);

    error = agent::parse_command("sudo make", args, agent::CommandPolicy::Agent, rule);
    check(!error.ok(), "agent still denylists sudo: " + error.message);
    error = agent::parse_command("sudo make", args, agent::CommandPolicy::Agent, rule,
                                 agent::GuardAskHandling::DenyAsk, nullptr, {}, false,
                                 true);
    check(error.ok(), "Yolo unrestricted accepts sudo at user risk: " + error.message);

    error = agent::parse_command("cat /etc/passwd", args, agent::CommandPolicy::Agent, rule);
    check(!error.ok() && error.message.find("absolute path") != std::string::npos,
          "direct agent command parsing rejects absolute operands by default");
    error = agent::parse_command("cat /etc/passwd", args, agent::CommandPolicy::Agent, rule,
                                 agent::GuardAskHandling::DeferAsk, nullptr, {}, true);
    check(error.ok(),
          "tool-layer preview may defer absolute operands to canonical authorization");

    error = agent::parse_command("stat tic_tac_toe.py", args, agent::CommandPolicy::InspectionOnly,
                                 rule);
    check(!error.ok() && error.message.find("inspection allowlist") != std::string::npos,
          "security-review remains a strict allowlist: " + error.message);

    error = agent::parse_command("stat -c %y tic_tac_toe.py", args,
                                 agent::CommandPolicy::PlanReadOnly, rule);
    check(error.ok(), "Plan accepts expanded vetted read-only commands: " + error.message);
    error = agent::parse_command("make test", args, agent::CommandPolicy::PlanReadOnly, rule);
    check(!error.ok(), "Plan denies non-vetted build commands");
    error = agent::parse_command("tail -f tic_tac_toe.py", args,
                                 agent::CommandPolicy::PlanReadOnly, rule);
    check(!error.ok(), "Plan denies mutating/following display-command forms");
}

void test_read_only_command_classifier() {
    auto vetted = [](std::initializer_list<const char*> words) {
        std::vector<std::string> args;
        for (const char* word : words) args.emplace_back(word);
        return agent::assess_read_only_command(args).vetted;
    };
    check(vetted({"pwd"}), "classifier: pwd");
    check(vetted({"ls", "-laFg", "src"}), "classifier: ordinary combined ls flags");
    check(vetted({"cat", "-n", "README.md"}), "classifier: cat");
    check(vetted({"head", "-n", "5", "README.md"}), "classifier: head");
    check(vetted({"tail", "-n", "5", "README.md"}), "classifier: non-following tail");
    check(vetted({"stat", "-c", "%y", "README.md"}), "classifier: stat");
    check(vetted({"file", "--mime-type", "README.md"}), "classifier: file");
    check(vetted({"wc", "-l", "README.md"}), "classifier: wc");
    check(vetted({"du", "-sh", "src"}), "classifier: du");
    check(vetted({"grep", "-n", "-C", "2", "needle", "src/main.cpp"}),
          "classifier: grep context flags");
    check(vetted({"rg", "-n", "-g", "*.cpp", "needle", "src"}),
          "classifier: rg matching and glob flags");
    check(vetted({"find", "src", "-type", "f", "-print"}),
          "classifier: print-only find");
    check(vetted({"diff", "-u", "input1.txt", "input2.txt"}), "classifier: diff");
    check(vetted({"cmp", "-s", "input1.txt", "input2.txt"}), "classifier: cmp");
    check(vetted({"readlink", "-f", "src"}), "classifier: readlink");
    check(vetted({"md5sum", "README.md"}) &&
              vetted({"sha256sum", "README.md"}) &&
              vetted({"b2sum", "README.md"}) &&
              vetted({"cksum", "README.md"}),
          "classifier: checksum families");
    check(vetted({"ps", "aux"}) && vetted({"ps", "-eo", "pid,cmd"}) &&
              vetted({"df", "-h"}) &&
              vetted({"whoami"}) && vetted({"id", "-u"}) &&
              vetted({"groups"}) && vetted({"who", "-H"}) &&
              vetted({"uname", "-a"}) && vetted({"lsb_release", "-a"}) &&
              vetted({"uptime"}) && vetted({"free", "-h"}) &&
              vetted({"nproc"}) && vetted({"arch"}),
          "classifier: passive host snapshots");
    check(vetted({"hostname", "-f"}) && vetted({"date", "-u", "+%FT%TZ"}) &&
              vetted({"ifconfig", "-a"}) && vetted({"ip", "addr", "show"}),
          "classifier: strict display-only forms");
    check(vetted({"command", "-v", "apache2"}),
          "classifier: command -v executable lookup");
    check(!vetted({"command", "-p", "apache2"}) &&
              !vetted({"command", "-v", "/usr/bin/apache2"}),
          "classifier: command builtin remains narrowly vetted");

    check(!vetted({"ls", "--definitely-unknown"}), "classifier: unknown option fallback");
    check(!vetted({"date", "--set", "tomorrow"}), "classifier: date --set trap");
    check(!vetted({"hostname", "new-name"}), "classifier: hostname mutation trap");
    check(!vetted({"ifconfig", "eth0", "up"}), "classifier: ifconfig mutation trap");
    check(!vetted({"find", ".", "-fprint", "owned"}), "classifier: find output trap");
    check(!vetted({"find", ".", "-exec", "touch", "owned", ";"}),
          "classifier: find exec trap");
    check(!vetted({"rg", "--pre", "cat", "needle"}), "classifier: rg preprocessor trap");
    check(!vetted({"sha256sum", "--check", "sums"}), "classifier: checksum check trap");
    check(!vetted({"tail", "--follow", "README.md"}), "classifier: tail follow trap");
    check(!vetted({"diff", "--output=owned", "input1.txt", "input2.txt"}),
          "classifier: output-file option trap");
    check(!vetted({"file", "--compile"}), "classifier: file compile trap");
    check(!vetted({"ping", "localhost"}) && !vetted({"top"}) &&
              !vetted({"git", "status"}) && !vetted({"make", "test"}),
          "classifier: intentionally non-vetted command families");
}

std::string temp_workspace(const std::string& name) {
    const fs::path root =
        fs::temp_directory_path() / ("ainiux-cmd-guard-" + name + "-" +
                                     std::to_string(ainiux::platform::current_process_id()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    {
        std::ofstream out(root / "hello.py");
        out << "print('ok')\n";
    }
    return root.string();
}

bool json_ok(const std::string& result) {
    const json::ParseResult parsed = json::parse(result);
    if (!parsed.error.ok() || !parsed.value.is_object()) return false;
    const json::Value* ok = parsed.value.get("ok");
    return ok != nullptr && ok->type == json::Value::Type::Bool && ok->boolean;
}

agent::ReadToolRegistry make_registry(const std::string& workspace, bool mutations) {
    agent::index::Options options;
    options.workspace = workspace;
    agent::index::RefreshStats stats;
    check(agent::index::refresh(options, stats).ok(), "refresh");
    agent::index::Snapshot snapshot;
    check(agent::index::load_snapshot(options, snapshot).ok(), "snapshot");
    agent::ReadToolRegistry tools;
    agent::ToolRegistryOptions tool_options;
    tool_options.mutation_policy = mutations ? agent::MutationPolicy::Full : agent::MutationPolicy::Disabled;
    check(agent::ReadToolRegistry::create(std::move(options), std::move(snapshot), {}, tools,
                                          tool_options)
              .ok(),
          "create tools");
    return tools;
}

void test_tool_agent_python_and_security_deny() {
    const std::string workspace = temp_workspace("tool");
    agent::ReadToolRegistry agent_tools = make_registry(workspace, true);
    const std::string py =
        agent_tools.execute("run_command", R"JSON({"command":"python3 hello.py"})JSON");
    check(json_ok(py), "agent run_command python3 hello.py: " + py);
    check(py.find("ok") != std::string::npos || py.find("\"exit_status\":0") != std::string::npos,
          "python output/status: " + py);

    const std::string lookup =
        agent_tools.execute("run_command", R"JSON({"command":"command -v ls"})JSON");
    check(json_ok(lookup) &&
#if defined(_WIN32)
              lookup.find("ls.exe") != std::string::npos,
#else
              lookup.find("/ls") != std::string::npos,
#endif
          "agent run_command emulates command -v without a shell: " + lookup);
    const std::string missing = agent_tools.execute(
        "run_command",
        R"JSON({"command":"command -v ainiux-definitely-missing-command"})JSON");
    check(json_ok(missing) && missing.find("\"exit_status\":1") != std::string::npos,
          "command -v reports a missing executable as process status, not a tool error: " +
              missing);

    agent::ReadToolRegistry review = make_registry(workspace, false);
    const std::string denied =
        review.execute("run_command", R"JSON({"command":"python3 hello.py"})JSON");
    check(!json_ok(denied), "security-review still denies python3: " + denied);
    check(denied.find("security-review") != std::string::npos ||
              denied.find("inspection allowlist") != std::string::npos,
          "error mentions inspection allowlist: " + denied);

    const std::string rm =
        agent_tools.execute("run_command", R"JSON({"command":"rm -rf build"})JSON");
    check(!json_ok(rm) && rm.find("policy_denied") != std::string::npos,
          "agent denies rm -rf headless: " + rm);
    check(rm.find("headless") != std::string::npos || rm.find("Ask") != std::string::npos ||
              rm.find("refusing") != std::string::npos,
          "rm -rf error mentions guard/headless: " + rm);

    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_workspace_script_execution() {
    const std::string workspace = temp_workspace("workspace-script");
#if defined(_WIN32)
    const std::string script_name = "server.cmd";
    {
        std::ofstream out(fs::path(workspace) / script_name);
        out << "@echo off\r\necho arg=%1\r\necho secret=%OPENAI_API_KEY%\r\n";
    }
    const std::optional<std::string> previous_api_key =
        ainiux::test::test_environment("OPENAI_API_KEY");
    constexpr const char* inherited_secret = "ainiux-agent-secret-must-not-leak";
    ainiux::test::set_test_environment("OPENAI_API_KEY", inherited_secret);
#else
    const std::string script_name = "server.sh";
    {
        std::ofstream out(fs::path(workspace) / script_name);
        out << "#!/bin/sh\necho \"arg=$1\"\n";
    }
    ::chmod((fs::path(workspace) / script_name).c_str(), 0755);
#endif

    agent::ProcessOptions options;
    options.workspace = workspace;
    options.cwd = workspace;
    options.allow_workspace_executables = true;
    options.timeout_ms = 5000;
    agent::ProcessResult result;

    const std::string relative_command = "./" + script_name + " start";
    const std::string bare_command = script_name + " start";
    Error error = agent::run_command(relative_command, options, result,
                                     agent::CommandPolicy::Agent);
#if defined(_WIN32)
    if (previous_api_key.has_value())
        ainiux::test::set_test_environment("OPENAI_API_KEY", *previous_api_key);
    else
        ainiux::test::unset_test_environment("OPENAI_API_KEY");
#endif
    check(error.ok() && result.exit_status == 0 &&
              result.stdout_text.find("arg=start") != std::string::npos,
          "agent runs ./server.sh with args: " + error.message + " out=" + result.stdout_text);
#if defined(_WIN32)
    check(result.stdout_text.find(inherited_secret) == std::string::npos,
          "Windows agent subprocess environment excludes inherited API keys");
    result = {};
    error = agent::run_command("./server.cmd \"unsafe&argument\"", options, result,
                               agent::CommandPolicy::Agent);
    check(!error.ok() && error.message.find("metacharacters") != std::string::npos,
          "Windows batch shim rejects cmd.exe expansion metacharacters");
#endif

    result = {};
    error = agent::run_command(bare_command, options, result, agent::CommandPolicy::Agent);
    check(error.ok() && result.exit_status == 0 &&
              result.stdout_text.find("arg=start") != std::string::npos,
          "agent runs bare workspace script server.sh: " + error.message +
              " out=" + result.stdout_text);

#if !defined(_WIN32)
    result = {};
    error =
        agent::run_command("bash server.sh start", options, result, agent::CommandPolicy::Agent);
    check(error.ok() && result.exit_status == 0 &&
              result.stdout_text.find("arg=start") != std::string::npos,
          "agent runs bash server.sh form: " + error.message + " out=" + result.stdout_text);
#endif

    // Without workspace executables, path form still fails closed (inspection-style).
    options.allow_workspace_executables = false;
    result = {};
    error =
        agent::run_command(relative_command, options, result, agent::CommandPolicy::Agent);
    check(!error.ok() && error.message.find("bare command") != std::string::npos,
          "path scripts require allow_workspace_executables: " + error.message);

    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_interactive_approval_allows_then_denies() {
    const std::string workspace = temp_workspace("approve");
    agent::index::Options options;
    options.workspace = workspace;
    agent::index::RefreshStats stats;
    check(agent::index::refresh(options, stats).ok(), "refresh approve workspace");
    agent::index::Snapshot snapshot;
    check(agent::index::load_snapshot(options, snapshot).ok(), "snapshot approve");

    std::atomic<int> ask_count{0};
    agent::ToolRegistryOptions tool_options;
    tool_options.mutation_policy = agent::MutationPolicy::Full;
    tool_options.on_guard_ask =
        [&](const agent::GuardApprovalRequest& request,
            runtime::CancellationToken) -> agent::GuardApprovalDecision {
        const int n = ++ask_count;
        check(request.tool_name == "run_command", "ask tool is run_command");
        check(!request.rule_id.empty(), "ask has rule_id");
        if (n == 1) {
            check(request.command_preview.find("rm") != std::string::npos,
                  "first ask preview mentions rm");
            return agent::GuardApprovalDecision::Allow;
        }
        check(request.command_preview.find("git") != std::string::npos ||
                  request.command_preview.find("reset") != std::string::npos,
              "second ask is git reset");
        return agent::GuardApprovalDecision::Deny;
    };
    agent::ReadToolRegistry tools;
    check(agent::ReadToolRegistry::create(options, std::move(snapshot), {}, tools, tool_options)
              .ok(),
          "create tools with ask callback");

    // Allow: rm -rf should proceed (exit may be non-zero if path missing — still not policy_denied).
    const std::string allowed =
        tools.execute("run_command", R"JSON({"command":"rm -rf missing_build_dir"})JSON");
    check(ask_count.load() >= 1, "approval callback invoked");
    check(allowed.find("policy_denied") == std::string::npos ||
              allowed.find("\"ok\":true") != std::string::npos ||
              allowed.find("exit_status") != std::string::npos,
          "approved rm runs (not hard policy deny): " + allowed);

    // Deny path: second destructive Ask.
    const std::string denied =
        tools.execute("run_command", R"JSON({"command":"git reset --hard"})JSON");
    check(!json_ok(denied), "denied git reset --hard after user deny: " + denied);
    check(denied.find("denied") != std::string::npos ||
              denied.find("policy_denied") != std::string::npos ||
              denied.find("refusing") != std::string::npos,
          "deny message present: " + denied);

    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_approval_gate_resolve_and_cancel() {
    agent::ApprovalGate gate;
    std::atomic<bool> notified{false};
    gate.set_notify([&](const agent::GuardApprovalRequest& req) {
        check(req.rule_id == "ask_on_destructive_git", "notify rule_id");
        notified = true;
    });

    agent::GuardApprovalRequest req;
    req.tool_name = "run_command";
    req.command_preview = "git reset --hard";
    req.rule_id = "ask_on_destructive_git";
    req.message = "test";

    agent::GuardApprovalDecision got = agent::GuardApprovalDecision::Deny;
    std::thread worker([&] {
        got = gate.request(req, runtime::CancellationToken());
    });
    // Wait until pending.
    for (int i = 0; i < 100 && !gate.has_pending(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(gate.has_pending(), "gate has pending request");
    check(notified.load(), "notify fired");
    gate.resolve(agent::GuardApprovalDecision::Allow);
    worker.join();
    check(got == agent::GuardApprovalDecision::Allow, "resolve Allow");

    notified = false;
    std::thread worker2([&] {
        got = gate.request(req, runtime::CancellationToken());
    });
    for (int i = 0; i < 100 && !gate.has_pending(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    gate.cancel_pending();
    worker2.join();
    check(got == agent::GuardApprovalDecision::Cancelled, "cancel_pending → Cancelled");
}

void test_ask_raw_decision_not_finalized() {
    const agent::GuardResult ask =
        agent::evaluate_command_guard({"git", "push", "--force"});
    check(ask.decision == agent::GuardDecision::Ask, "force push is Ask before finalize");
    const agent::GuardResult headless = agent::finalize_guard_for_headless(ask);
    check(headless.decision == agent::GuardDecision::Deny, "headless maps Ask→Deny");
    check(headless.message.find("headless") != std::string::npos,
          "headless message mentions headless: " + headless.message);
}

void test_run_command_cancellation_remains_effective() {
    const std::string workspace = temp_workspace("cancel");
    runtime::CancellationSource source;
    source.cancel();
    agent::ProcessOptions options;
    options.workspace = workspace;
    options.timeout_ms = 5000;
    options.cancellation = source.token();
    agent::ProcessResult result;
    const Error error =
        agent::run_command("sleep 5", options, result,
                           agent::CommandPolicy::Agent);
    check(error.code == ErrorCode::Cancelled && result.cancelled,
          "direct argv command remains cancellation-aware");
    std::error_code ec;
    fs::remove_all(workspace, ec);
}

}  // namespace

void run_all() {
    test_guard_patterns();
#if defined(_WIN32)
    test_windows_direct_argv_paths();
#endif
    test_parse_policies();
    test_read_only_command_classifier();
    test_tool_agent_python_and_security_deny();
    test_workspace_script_execution();
    test_interactive_approval_allows_then_denies();
    test_approval_gate_resolve_and_cancel();
    test_ask_raw_decision_not_finalized();
    test_run_command_cancellation_remains_effective();
}

}  // namespace ainiux::test::agent_command_guard
