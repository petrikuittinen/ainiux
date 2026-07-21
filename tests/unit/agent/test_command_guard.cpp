#include "agent/test_command_guard.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "agent/command_guard.hpp"
#include "agent/index/index.hpp"
#include "agent/process.hpp"
#include "agent/tools.hpp"
#include "json/json.hpp"
#include "support/test_support.hpp"

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

    const agent::GuardResult allow =
        agent::evaluate_command_guard({"python3", "hello.py"});
    check(allow.decision == agent::GuardDecision::Allow, "python3 hello.py allowed by guard");
    check(agent::evaluate_command_guard({"make", "test"}).decision == agent::GuardDecision::Allow,
          "make test allowed by guard");
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

    error = agent::parse_command("ls -1", args, agent::CommandPolicy::Agent, rule);
    check(error.ok(), "agent policy still allows ls");
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
    tool_options.allow_mutations = mutations;
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
          "agent denies rm -rf: " + rm);

    std::error_code ec;
    fs::remove_all(workspace, ec);
}

}  // namespace

void run_all() {
    test_guard_patterns();
    test_parse_policies();
    test_tool_agent_python_and_security_deny();
}

}  // namespace ainiux::test::agent_command_guard
