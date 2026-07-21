#include "agent/test_project_root.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "agent/compact.hpp"
#include "agent/project_root.hpp"
#include "agent/tool_display.hpp"
#include "support/test_support.hpp"

namespace ainiux::test::agent_project_root {
namespace {
using ainiux::test::check;
namespace fs = std::filesystem;

std::string temp_dir(const std::string& name) {
    const fs::path root =
        fs::temp_directory_path() / ("ainiux-proj-" + name + "-" +
                                     std::to_string(static_cast<long long>(::getpid())));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    return root.string();
}

void test_resolve_clean_workspace() {
    const std::string root = temp_dir("clean");
    std::string abs;
    Error error = agent::resolve_agent_project_root(root, abs);
    check(error.ok(), "clean workspace resolves: " + error.message);
    check(!abs.empty(), "absolute root set");
    std::error_code ec;
    fs::remove_all(root, ec);
}

void test_reject_parent_ainiux() {
    const std::string parent = temp_dir("parent");
    fs::create_directories(fs::path(parent) / ".ainiux-pr");
    const std::string child = (fs::path(parent) / "sub").string();
    fs::create_directories(child);
    std::string abs;
    Error error = agent::resolve_agent_project_root(child, abs);
    check(!error.ok(), "child under parent .ainiux-pr rejected");
    std::error_code ec;
    fs::remove_all(parent, ec);
}

void test_home_ainiux_profile_is_not_a_project() {
    // ~/.ainiux is the user chat/media profile, not a project marker.
    const std::string home = temp_dir("home");
    fs::create_directories(fs::path(home) / ".ainiux");
    const std::string code = (fs::path(home) / "code").string();
    fs::create_directories(code);
    std::string abs;
    Error error = agent::resolve_agent_project_root(code, abs);
    check(error.ok(), "parent ~/.ainiux profile must not block agent: " + error.message);
    std::error_code ec;
    fs::remove_all(home, ec);
}

void test_reject_nested_child_ainiux() {
    const std::string root = temp_dir("nest");
    fs::create_directories(fs::path(root) / "pkg" / ".ainiux-pr");
    std::string abs;
    Error error = agent::resolve_agent_project_root(root, abs);
    check(!error.ok(), "nested child .ainiux-pr rejected");
    std::error_code ec;
    fs::remove_all(root, ec);
}

void test_compact_threshold_defaults() {
    check(agent::effective_compact_limit_percent(0, 128000) == 75, "large window default 75");
    check(agent::effective_compact_limit_percent(0, 65536) == 100, "64k window default 100");
    check(agent::effective_compact_limit_percent(0, 8000) == 100, "small window default 100");
    check(agent::effective_compact_limit_percent(60, 128000) == 60, "explicit override");
    check(!agent::should_auto_compact(false, 75, 100000, 90000), "auto off never compact");
    check(agent::should_auto_compact(true, 75, 100000, 80000), "at 80% of 100k with 75% limit");
    check(!agent::should_auto_compact(true, 75, 100000, 70000), "below 75% threshold");
}

void test_tool_display_format() {
    const std::string line = agent::format_compact_tool_line(
        1, "read_file", R"({"path":"example.txt"})", R"({"ok":true})");
    check(line.find("1: read_file(") != std::string::npos, "index and name");
    check(line.find("example.txt") != std::string::npos, "path preview");
    check(line.find("→ ok") != std::string::npos || line.find("ok") != std::string::npos, "ok status");
}

}  // namespace

void run_all() {
    test_resolve_clean_workspace();
    test_reject_parent_ainiux();
    test_home_ainiux_profile_is_not_a_project();
    test_reject_nested_child_ainiux();
    test_compact_threshold_defaults();
    test_tool_display_format();
}

}  // namespace ainiux::test::agent_project_root
