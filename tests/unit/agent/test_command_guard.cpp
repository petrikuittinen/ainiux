#include "agent/test_command_guard.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "agent/approval.hpp"
#include "agent/command_guard.hpp"
#include "agent/index/index.hpp"
#include "agent/process.hpp"
#include "agent/tools.hpp"
#include "json/json.hpp"
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
    check(deny({"bash", "-c", "echo hi"}), "bash wrapper denied");
    check(deny({"sudo", "make"}), "sudo denied");
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

    error = agent::parse_command("bash -c echo", args, agent::CommandPolicy::Agent, rule);
    check(!error.ok(), "agent still denylists shell wrappers: " + error.message);

    error = agent::parse_command("sudo make", args, agent::CommandPolicy::Agent, rule);
    check(!error.ok(), "agent still denylists sudo: " + error.message);

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
}

std::string temp_workspace(const std::string& name) {
    const fs::path root =
        fs::temp_directory_path() / ("ainiux-cmd-guard-" + name + "-" +
                                     std::to_string(static_cast<long long>(::getpid())));
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

}  // namespace

void run_all() {
    test_guard_patterns();
    test_parse_policies();
    test_tool_agent_python_and_security_deny();
    test_interactive_approval_allows_then_denies();
    test_approval_gate_resolve_and_cancel();
    test_ask_raw_decision_not_finalized();
}

}  // namespace ainiux::test::agent_command_guard
