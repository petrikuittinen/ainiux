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
    check(agent::evaluate_command_guard({"rm", "-rf", "build"}).decision ==
              agent::GuardDecision::Allow,
          "rm -rf is not a hard Guard deny; Smart asks only for non-empty trees");
    check(agent::evaluate_command_guard({"rm", "-fr", "build"}).decision ==
              agent::GuardDecision::Allow,
          "rm -fr is classified later for emptiness");
    check(agent::evaluate_command_guard({"rm", "-r", "-f", "build"}).decision ==
              agent::GuardDecision::Allow,
          "rm -r -f is classified later for emptiness");
    check(agent::finalize_guard_for_headless(agent::evaluate_command_guard(
              {"rm", "app.sqlite"})).decision == agent::GuardDecision::Deny,
          "rm of a database-looking file still Asks/denies headless");
    check(deny({"git", "reset", "--hard"}), "git reset --hard denied");
    check(deny({"git", "-c", "core.pager=cat", "reset", "--hard"}),
          "git -c pager prefix does not hide git reset --hard from Guard");
    check(deny({"git", "clean", "-fdx"}), "git clean -fdx denied");
    check(deny({"git", "push", "--force"}), "git push --force denied");
    check(deny({"find", ".", "-delete"}), "find -delete denied");
    check(deny({"sqlite3", "app.sqlite", "DROP TABLE users;"}), "destructive sql denied");
    check(deny({"bash", "-c", "echo hi"}), "bash -c free-form denied");
    check(deny({"nohup", "python3", "scripts/ainiux/serve_dir.py"}),
          "nohup detach denied");
    check(agent::evaluate_command_guard(
              {"python3", "-c", "import os\nprint(1)"}).decision ==
              agent::GuardDecision::Allow,
          "multi-line python -c is allowed by guard");
    check(deny({"python3", "-c", "subprocess.Popen(['scripts/ainiux/x.py'])"}),
          "python -c wrapping a project script denied");
    check(deny({"python3", "-"}), "python stdin program denied");
    check(deny({"python3", "-m", "pip", "install", "requests"}),
          "python -m pip install denied");
    check(deny({"python3.12", "-m", "ensurepip"}),
          "versioned python -m ensurepip denied");
    check(agent::evaluate_command_guard(
              {"python3", "-c", "print(1)"}).decision == agent::GuardDecision::Allow,
          "short one-line python -c still allowed by guard");
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
    check(deny({"dotnet", "nuget", "push", "pkg.nupkg"}),
          "dotnet nuget push denied");
    check(deny({"dotnet", "nuget", "delete", "pkg"}),
          "dotnet nuget delete denied");
    check(deny({"dotnet", "tool", "install", "-g", "foo"}),
          "dotnet tool install -g denied");
    check(deny({"dotnet", "tool", "install", "--global", "foo"}),
          "dotnet tool install --global denied");
    check(agent::evaluate_command_guard({"dotnet", "build"}).decision ==
              agent::GuardDecision::Allow,
          "dotnet build allowed by guard");
    check(agent::evaluate_command_guard(
              {"dotnet", "tool", "install", "foo"}).decision ==
              agent::GuardDecision::Allow,
          "local dotnet tool install is not a hard Guard deny");

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
    check(error.ok() && rule.empty(),
          "agent parse accepts rm -rf; emptiness is checked at execute: " +
              error.message);

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
          "agent accepts quoted short python -c payload: " + error.message);
    error = agent::parse_command(
        R"CMD(python3 -c "import os
import sys
print(1)")CMD",
        args, agent::CommandPolicy::Agent, rule);
    check(error.ok() && args.size() == 3 && args[2].find("import os") != std::string::npos,
          "agent accepts quoted multi-line python -c: " + error.message);
    error = agent::parse_command("python3 -m pip install requests", args,
                                 agent::CommandPolicy::Agent, rule);
    check(!error.ok() && error.message.find("environment") != std::string::npos,
          "agent denylists python -m pip: " + error.message);

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
                                 agent::CommandPolicy::RestrictedReadOnly, rule);
    check(error.ok(), "restricted policy accepts expanded vetted read-only commands: " + error.message);
    error = agent::parse_command("make test", args, agent::CommandPolicy::RestrictedReadOnly, rule);
    check(!error.ok(), "restricted policy denies non-vetted build commands");
    error = agent::parse_command("git diff --stat", args,
                                 agent::CommandPolicy::RestrictedReadOnly, rule);
    check(error.ok() && args.size() > 9 && args[9] == "diff",
          "restricted policy accepts bounded git diff and injects pager hardening");
    error = agent::parse_command("git -c core.pager=cat status --short", args,
                                 agent::CommandPolicy::RestrictedReadOnly, rule);
    check(error.ok() && agent::git_subcommand_index(args) < args.size() &&
              args[agent::git_subcommand_index(args)] == "status",
          "restricted policy accepts git status after a copied pager -c: " + error.message);
    error = agent::parse_command("git -c core.pager=cat log --oneline -3", args,
                                 agent::CommandPolicy::RestrictedReadOnly, rule);
    check(error.ok(),
          "restricted policy accepts git log inspection: " + error.message);
    error = agent::parse_command("git -c core.pager=cat config --get user.name", args,
                                 agent::CommandPolicy::Agent, rule);
    check(!error.ok() && error.message.find("config") != std::string::npos,
          "agent still blocks git config after a pager -c prefix: " + error.message);
    error = agent::parse_command("git -c alias.status=!id status --short", args,
                                 agent::CommandPolicy::Agent, rule);
    check(!error.ok() && error.message.find("alias") != std::string::npos,
          "agent blocks git -c alias overrides: " + error.message);
    error = agent::parse_command("git -c core.pager=cat status --short", args,
                                 agent::CommandPolicy::Agent, rule);
    check(error.ok() && agent::git_subcommand_index(args) < args.size() &&
              args[agent::git_subcommand_index(args)] == "status",
          "agent git hardening keeps status as the subcommand after -c pager: " +
              error.message);
    error = agent::parse_command("node --test", args, agent::CommandPolicy::RestrictedReadOnly,
                                 rule);
    check(!error.ok(), "restricted policy does not treat node --test as read-only");
    error = agent::parse_command("tail -f tic_tac_toe.py", args,
                                 agent::CommandPolicy::RestrictedReadOnly, rule);
    check(!error.ok(), "restricted policy denies mutating/following display-command forms");
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
    check(vetted({"git", "status"}) &&
              vetted({"git", "status", "--short", "--branch"}) &&
              vetted({"git", "diff"}) &&
              vetted({"git", "diff", "--stat"}) &&
              vetted({"git", "diff", "--cached", "--", "src/main.cpp"}) &&
              vetted({"git", "ls-files"}) &&
              vetted({"git", "rev-parse", "--is-inside-work-tree"}),
          "classifier: bounded git inspection");
    check(vetted({"git", "-c", "core.pager=cat", "-c", "pager.show=false",
                  "-c", "pager.diff=false", "-c", "diff.external=", "diff"}),
          "classifier: git diff after runner pager hardening");
    check(vetted({"git", "-c", "core.pager=cat", "status", "--short"}) &&
              vetted({"git", "-c", "core.pager=cat", "-c", "pager.show=false",
                      "-c", "pager.diff=false", "-c", "diff.external=",
                      "-c", "core.pager=cat", "status", "--short", "--branch"}) &&
              vetted({"git", "-c", "core.pager=cat", "diff", "--cached", "--stat"}),
          "classifier: copied git -c pager prefix still classifies status/diff");
    check(vetted({"git", "log", "--oneline", "-3"}) &&
              vetted({"git", "-c", "core.pager=cat", "log", "--oneline", "-15"}) &&
              vetted({"git", "log", "--oneline", "-3", "--", "src/ainiux/isa.py"}) &&
              vetted({"git", "rev-list", "--left-right", "--count",
                      "origin/main...main"}) &&
              vetted({"git", "remote", "-v"}) &&
              vetted({"git", "show", "HEAD"}) &&
              vetted({"git", "branch", "--show-current"}) &&
              vetted({"git", "stash", "list"}),
          "classifier: common git inspection forms");
    check(!vetted({"git", "diff", "--output=owned"}) &&
              !vetted({"git", "commit", "-am", "x"}) &&
              !vetted({"git", "push"}) &&
              !vetted({"git", "add", "-A"}) &&
              !vetted({"git", "stash"}) &&
              !vetted({"git", "branch", "topic"}) &&
              !vetted({"git", "remote", "add", "origin", "git@example.com:x.git"}) &&
              !vetted({"git", "-c", "alias.status=!id", "status"}),
          "classifier: mutating git stays unvetted");

    auto node_test = [](std::initializer_list<const char*> words) {
        std::vector<std::string> args;
        for (const char* word : words) args.emplace_back(word);
        return agent::assess_node_test_command(args).vetted;
    };
    check(node_test({"node", "--test"}) &&
              node_test({"node", "--test", "tetris/game.test.js"}) &&
              node_test({"node", "--test", "--test-only", "src"}),
          "classifier: node --test");
    check(!node_test({"node", "script.js"}) &&
              !node_test({"node", "-e", "console.log(1)"}) &&
              !node_test({"node", "--test", "-e", "console.log(1)"}) &&
              !node_test({"node", "--test", "--watch"}) &&
              !node_test({"node", "--eval", "1"}),
          "classifier: node eval/script/watch stay outside node --test");

    auto interpreter = [](std::initializer_list<const char*> words) {
        std::vector<std::string> args;
        for (const char* word : words) args.emplace_back(word);
        return agent::assess_workspace_interpreter_command(args).vetted;
    };
    check(interpreter({"python3", "-c", "print(1)"}) &&
              interpreter({"python3", "-c", "import os\nprint(1)"}) &&
              interpreter({"python3", "-m", "pytest"}) &&
              interpreter({"python3", "-m", "py_compile", "src/a.py"}) &&
              interpreter({"python3", "tests/test_foo.py"}) &&
              interpreter({"python3.12", "-u", "-c", "print(1)"}),
          "interpreter classifier: python file, -m, and -c");
    check(!interpreter({"python3", "-c", "print(open('/etc/passwd').read())"}) &&
              !interpreter({"python3", "-c", "open('~/secret')"}) &&
              !interpreter({"python3", "-m", "http.server"}) &&
              !interpreter({"python3", "-m", "pip"}) &&
              !interpreter({"python3", "--foo", "x.py"}) &&
              !interpreter({"python3", "-"}),
          "interpreter classifier: escaping/listener/unknown python stays unvetted");
    check(interpreter({"node", "--test"}) &&
              interpreter({"node", "script.js"}) &&
              interpreter({"node", "-e", "console.log(1)"}) &&
              interpreter({"node", "--no-warnings", "app.mjs"}),
          "interpreter classifier: node file, --test, and -e");
    check(!interpreter({"node", "--inspect", "script.js"}) &&
              !interpreter({"node", "--watch"}) &&
              !interpreter({"node", "-e", "require('/etc/passwd')"}),
          "interpreter classifier: node inspect/watch/absolute stay unvetted");
    check(interpreter({"pytest"}) && interpreter({"pytest", "tests/"}) &&
              !interpreter({"pytest", "--pdb"}),
          "interpreter classifier: pytest without debugger flags");
    check(interpreter({"g++", "-c", "src/hello.cpp", "-o", "hello.o"}) &&
              interpreter({"gcc-13", "-O2", "main.c"}) &&
              interpreter({"clang++", "src/a.cpp"}) &&
              interpreter({"/usr/bin/clang-18", "-Wall", "a.c"}) &&
              interpreter({"aarch64-linux-gnu-gcc", "-c", "a.c"}) &&
              interpreter({"clang-format", "src/a.cpp"}) &&
              interpreter({"make", "test"}) && interpreter({"make", "-j8"}) &&
              interpreter({"cmake", "-S", ".", "-B", "build"}) &&
              interpreter({"cmake", "--build", "build"}) &&
              interpreter({"ctest", "--test-dir", "build"}) &&
              interpreter({"ninja", "all"}),
          "interpreter classifier: C/C++ compilers and build tools");
    check(!interpreter({"g++", "-fplugin=evil.so", "a.cpp"}) &&
              !interpreter({"clang", "-load", "plugin.so", "a.c"}) &&
              !interpreter({"make", "--eval", "evil"}) &&
              !interpreter({"make", "-f", "-"}) &&
              !interpreter({"make", "install"}) &&
              !interpreter({"cmake", "--install", "build"}) &&
              !interpreter({"ninja", "install"}),
          "interpreter classifier: compiler plugins and install stay unvetted");
    check(interpreter({"javac", "Main.java"}) &&
              interpreter({"java", "-cp", "out", "Main"}) &&
              interpreter({"jar", "cf", "app.jar", "Main.class"}) &&
              interpreter({"mvn", "-q", "test"}) &&
              interpreter({"mvn", "package"}) &&
              interpreter({"./mvnw", "verify"}) &&
              interpreter({"gradle", "build"}) &&
              interpreter({"./gradlew", "test"}) &&
              interpreter({"gradle", "publishToMavenLocal"}),
          "interpreter classifier: Java compilers and build tools");
    check(!interpreter({"jshell"}) &&
              !interpreter({"java", "-javaagent:agent.jar", "Main"}) &&
              !interpreter({"mvn", "deploy"}) &&
              !interpreter({"mvn", "package", "deploy"}) &&
              !interpreter({"mvn", "release:perform"}) &&
              !interpreter({"gradle", "publish"}) &&
              !interpreter({"./gradlew", ":app:publish"}),
          "interpreter classifier: Java publish/repl stay unvetted");
    check(interpreter({"dotnet", "build"}) &&
              interpreter({"dotnet", "test"}) &&
              interpreter({"dotnet", "run"}) &&
              interpreter({"dotnet", "publish"}) &&
              interpreter({"dotnet", "App.csproj"}) &&
              interpreter({"csc", "Program.cs"}) &&
              interpreter({"msbuild", "App.sln"}),
          "interpreter classifier: C# compilers and dotnet build/test");
    check(!interpreter({"dotnet", "nuget", "push", "pkg.nupkg"}) &&
              !interpreter({"dotnet", "tool", "install", "foo"}) &&
              !interpreter({"dotnet", "ef", "database", "update"}),
          "interpreter classifier: dotnet nuget/tool/ef stay unvetted");

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
              !vetted({"make", "test"}) && !vetted({"node", "--test"}),
          "classifier: intentionally non-vetted command families");

    auto fs_ok = [](std::initializer_list<const char*> words) {
        std::vector<std::string> args;
        for (const char* word : words) args.emplace_back(word);
        return agent::assess_workspace_fs_command(args).classified;
    };
    check(fs_ok({"mkdir", "-p", "src/out"}), "fs classifier: mkdir -p");
    check(fs_ok({"rmdir", "src/empty"}), "fs classifier: rmdir");
    check(fs_ok({"rm", "src/a.cpp"}), "fs classifier: rm file");
    check(fs_ok({"rm", "-rf", "build"}), "fs classifier: rm -rf");
    check(fs_ok({"mv", "src/a.cpp", "src/b.cpp"}), "fs classifier: mv");
    check(!fs_ok({"rm", "--one-file-system", "src"}),
          "fs classifier rejects unknown rm flags");
    check(!fs_ok({"make", "test"}), "fs classifier ignores builds");
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
        agent_tools.execute("run", R"JSON({"command":"python3 hello.py"})JSON");
    check(json_ok(py), "agent run_command python3 hello.py: " + py);
    const std::string inline_py = agent_tools.execute(
        "run",
        R"JSON({"command":"python3 -c \"import os\nprint('ok')\""})JSON");
    check(json_ok(inline_py),
          "headless agent run accepts multi-line python -c: " + inline_py);
    check(py.find("ok") != std::string::npos || py.find("\"exit_status\":0") != std::string::npos,
          "python output/status: " + py);

    const std::string lookup =
        agent_tools.execute("run", R"JSON({"command":"command -v ls"})JSON");
    check(json_ok(lookup) &&
#if defined(_WIN32)
              lookup.find("ls.exe") != std::string::npos,
#else
              lookup.find("/ls") != std::string::npos,
#endif
          "agent run_command emulates command -v without a shell: " + lookup);
    const std::string missing = agent_tools.execute(
        "run",
        R"JSON({"command":"command -v ainiux-definitely-missing-command"})JSON");
    check(json_ok(missing) && missing.find("\"exit_status\":1") != std::string::npos,
          "command -v reports a missing executable as process status, not a tool error: " +
              missing);

    agent::ReadToolRegistry review = make_registry(workspace, false);
    const std::string denied =
        review.execute("run", R"JSON({"command":"python3 hello.py"})JSON");
    check(!json_ok(denied), "security-review still denies python3: " + denied);
    check(denied.find("security-review") != std::string::npos ||
              denied.find("inspection allowlist") != std::string::npos,
          "error mentions inspection allowlist: " + denied);

    fs::create_directories(fs::path(workspace) / "build" / "obj");
    {
        std::ofstream out(fs::path(workspace) / "build" / "obj" / "a.o");
        out << "x\n";
    }
    const std::string rm =
        agent_tools.execute("run", R"JSON({"command":"rm -rf build"})JSON");
    check(!json_ok(rm) && rm.find("policy_denied") != std::string::npos,
          "agent denies nonempty rm -rf headless: " + rm);
    check(rm.find("headless") != std::string::npos || rm.find("Ask") != std::string::npos ||
              rm.find("refusing") != std::string::npos ||
              rm.find("non-empty") != std::string::npos ||
              rm.find("approval") != std::string::npos,
          "rm -rf error mentions guard/headless: " + rm);
    check(fs::exists(fs::path(workspace) / "build" / "obj" / "a.o"),
          "nonempty tree remains after denied rm -rf");

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
        out << "#!/bin/sh\necho \"arg=$1\"\necho \"secret=${OPENAI_API_KEY-}\"\n";
    }
    ::chmod((fs::path(workspace) / script_name).c_str(), 0755);
    const std::optional<std::string> previous_api_key =
        ainiux::test::test_environment("OPENAI_API_KEY");
    constexpr const char* inherited_secret = "ainiux-agent-secret-must-not-leak";
    ainiux::test::set_test_environment("OPENAI_API_KEY", inherited_secret);
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
    if (previous_api_key.has_value())
        ainiux::test::set_test_environment("OPENAI_API_KEY", *previous_api_key);
    else
        ainiux::test::unset_test_environment("OPENAI_API_KEY");
    check(error.ok() && result.exit_status == 0 &&
              result.stdout_text.find("arg=start") != std::string::npos,
          "agent runs ./server.sh with args: " + error.message + " out=" + result.stdout_text);
    check(result.stdout_text.find(inherited_secret) == std::string::npos,
          "agent subprocess environment excludes inherited API keys");
#if defined(_WIN32)
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
        check(request.tool_name == "run", "ask tool is run_command");
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

    fs::create_directories(fs::path(workspace) / "missing_build_dir" / "obj");
    {
        std::ofstream out(fs::path(workspace) / "missing_build_dir" / "obj" / "a.o");
        out << "x\n";
    }
    // Allow: nonempty rm -rf asks once, then proceeds.
    const std::string allowed =
        tools.execute("run", R"JSON({"command":"rm -rf missing_build_dir"})JSON");
    check(ask_count.load() >= 1, "approval callback invoked");
    check(allowed.find("policy_denied") == std::string::npos ||
              allowed.find("\"ok\":true") != std::string::npos ||
              allowed.find("exit_status") != std::string::npos,
          "approved rm runs (not hard policy deny): " + allowed);

    // Deny path: second destructive Ask.
    const std::string denied =
        tools.execute("run", R"JSON({"command":"git reset --hard"})JSON");
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
    req.tool_name = "run";
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

void test_nested_ainiux_environment_forwards_keys() {
    const std::optional<std::string> previous_api_key =
        ainiux::test::test_environment("OPENAI_API_KEY");
    constexpr const char* inherited_secret = "ainiux-nested-image-key";
    ainiux::test::set_test_environment("OPENAI_API_KEY", inherited_secret);
    const std::vector<std::string> foreign =
        agent::agent_command_environment("/usr/bin/true");
    bool foreign_has_key = false;
    bool self_has_key = false;
    for (const std::string& entry : foreign) {
        if (entry.rfind("OPENAI_API_KEY=", 0) == 0) foreign_has_key = true;
    }
    const std::string self = platform::executable_path();
    const std::vector<std::string> nested = agent::agent_command_environment(self);
    for (const std::string& entry : nested) {
        if (entry == std::string("OPENAI_API_KEY=") + inherited_secret) self_has_key = true;
    }
    if (previous_api_key.has_value())
        ainiux::test::set_test_environment("OPENAI_API_KEY", *previous_api_key);
    else
        ainiux::test::unset_test_environment("OPENAI_API_KEY");
    check(!foreign_has_key,
          "non-ainiux agent commands do not inherit OPENAI_API_KEY");
    check(!self.empty() && self_has_key,
          "nested ainiux (this process) inherits OPENAI_API_KEY for image/CLI");
}

void run_all() {
    test_guard_patterns();
#if defined(_WIN32)
    test_windows_direct_argv_paths();
#endif
    test_parse_policies();
    test_read_only_command_classifier();
    test_tool_agent_python_and_security_deny();
    test_workspace_script_execution();
    test_nested_ainiux_environment_forwards_keys();
    test_interactive_approval_allows_then_denies();
    test_approval_gate_resolve_and_cancel();
    test_ask_raw_decision_not_finalized();
    test_run_command_cancellation_remains_effective();
}

}  // namespace ainiux::test::agent_command_guard
