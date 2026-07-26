#include "agent/test_project_root.hpp"

#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <string>
#include <unistd.h>

#include "agent/compact.hpp"
#include "agent/project_root.hpp"
#include "agent/reasoning_preview.hpp"
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

void test_new_project_target_resolution_and_validation() {
    const std::string root = temp_dir("new-target");
    const fs::path spaced = fs::path(root) / "project with spaces";
    fs::create_directories(spaced);

    agent::NewProjectTarget target;
    Error error =
        agent::resolve_new_project_target(root, "project with spaces", target);
    check(error.ok() && target.root == fs::canonical(spaced).generic_string() &&
              target.root_exists,
          "relative /new path with spaces resolves from active root: " + error.message);

    error = agent::resolve_new_project_target(root, "fresh", target);
    check(error.ok() && !target.root_exists &&
              target.root == (fs::canonical(root) / "fresh").generic_string(),
          "/new permits exactly final-component creation");

    error = agent::resolve_new_project_target(root, "missing/child", target);
    check(!error.ok() && error.message.find("parent directory") != std::string::npos,
          "/new rejects missing intermediate directories");

    {
        std::ofstream file(fs::path(root) / "regular-file");
        file << "not a directory";
    }
    error = agent::resolve_new_project_target(root, "regular-file", target);
    check(!error.ok() && error.message.find("not a directory") != std::string::npos,
          "/new rejects file targets");

    fs::create_directories(spaced / ".ainiux-pr-real");
    std::error_code link_ec;
    fs::create_directory_symlink(spaced / ".ainiux-pr-real", spaced / ".ainiux-pr", link_ec);
    if (!link_ec) {
        error = agent::resolve_new_project_target(root, spaced.string(), target);
        check(!error.ok() && error.message.find("symlink") != std::string::npos,
              "/new rejects .ainiux-pr symlinks");
    }

    const char* old_home_value = std::getenv("HOME");
    const std::string old_home = old_home_value == nullptr ? "" : old_home_value;
    const fs::path home_project = fs::path(root) / "home project";
    fs::create_directories(home_project);
    ::setenv("HOME", root.c_str(), 1);
    error = agent::resolve_new_project_target(root, "~/home project", target);
    check(error.ok() && target.root == fs::canonical(home_project).generic_string(),
          "/new expands ~/ from HOME");
    error = agent::resolve_new_project_target(root, "~someone/project", target);
    check(!error.ok(), "/new rejects ~user expansion");
    if (old_home_value == nullptr)
        ::unsetenv("HOME");
    else
        ::setenv("HOME", old_home.c_str(), 1);

    const fs::path locked = fs::path(root) / "locked";
    fs::create_directories(locked);
    std::error_code permission_ec;
    fs::permissions(locked, fs::perms::owner_read, fs::perm_options::replace,
                    permission_ec);
    if (!permission_ec && ::geteuid() != 0) {
        error = agent::resolve_new_project_target(root, locked.string(), target);
        check(!error.ok() && error.code == ErrorCode::FileWrite,
              "/new rejects inaccessible target directories");
    }
    fs::permissions(locked, fs::perms::owner_all, fs::perm_options::replace,
                    permission_ec);

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
        1, "read_file", R"({"path":"example.txt"})", R"({"ok":true})", 150, 80);
    check(line.find("1: read_file(") != std::string::npos, "index and name");
    check(line.find("example.txt") != std::string::npos, "path preview");
    check(line.find("→ ok") != std::string::npos || line.find("ok") != std::string::npos, "ok status");
    check(line.find("in 150 ms") != std::string::npos, "independent tool duration");
}

void test_tool_display_clips_to_width() {
    const std::string long_path(200, 'x');
    const std::string args = std::string(R"({"path":")") + long_path + R"("})";
    const std::string line =
        agent::format_compact_tool_line(12, "read_file", args, R"({"ok":true})", 7, 40);
    check(line.size() <= 40, "tool line clipped to 40 cells");
    check(line.find("...") != std::string::npos, "ellipsis when clipped");
    check(agent::clip_to_cells("hello world", 5) == "he...", "clip_to_cells short");
    check(agent::clip_to_cells("hi", 10) == "hi", "clip_to_cells no-op");
    check(agent::terminal_column_count(80) >= 20, "terminal columns at least 20");
}

void test_elapsed_seconds_format() {
    check(agent::format_elapsed_ms(2270) == "2270 ms", "tool elapsed ms");
    check(agent::format_elapsed_ms(0) == "0 ms", "zero ms");
    check(agent::format_elapsed_ms(-5) == "0 ms", "negative ms clamps");
    check(agent::execution_only_elapsed_ms(175, 20, 145) == 50,
          "tool timing subtracts Guard approval wait");
    check(agent::execution_only_elapsed_ms(10, 100, 130) == 0,
          "approval subtraction clamps execution time at zero");
    check(agent::format_task_complete(21340) == "Task complete in 21.34 seconds.",
          "task complete 21.34s");
    check(agent::format_task_complete(0) == "Task complete in 0.00 seconds.", "task complete zero");
    check(agent::format_elapsed_seconds(6540) == "6.54 seconds elapsed", "legacy seconds format");
    check(agent::normalize_timestamp_ms(1700000000) == 1700000000000LL, "seconds promote to ms");
    check(agent::normalize_timestamp_ms(1700000000123LL) == 1700000000123LL, "ms unchanged");
    check(agent::now_unix_ms() > 1000000000000LL, "now is millisecond scale");

    const std::string outside_err =
        R"JSON({"ok":false,"error":{"code":"invalid_arguments","message":"Forbidden to create or modify files outside the project directory. Use a path relative to the project root only (not absolute, ~/..., or $ENV paths). Refused: ~/code/empty.txt"}})JSON";
    check(agent::compact_tool_error_brief(outside_err).find("outside project") != std::string::npos,
          "outside-project brief: " + agent::compact_tool_error_brief(outside_err));
    const std::string line = agent::format_compact_tool_line(
        2, "write_file", R"JSON({"path":"~/code/empty.txt","content":""})JSON",
        outside_err, 23, 120);
    check(line.find("error:") != std::string::npos && line.find("outside project") != std::string::npos,
          "compact line includes outside-project reason: " + line);
    check(line.find("in 23 ms") != std::string::npos,
          "failed compact line includes its own duration: " + line);
    const std::string deny_err =
        R"JSON({"ok":false,"error":{"code":"policy_denied","message":"shell wrappers are not allowed; pass a direct command (no sh -c)"}})JSON";
    check(agent::compact_tool_error_brief(deny_err).find("shell") != std::string::npos,
          "shell-deny brief: " + agent::compact_tool_error_brief(deny_err));
}

void test_prior_session_context_includes_recent_work() {
    std::vector<agent::AgentMessageRecord> messages(4);
    messages[0].role = "user";
    messages[0].content = "write a game";
    messages[1].role = "tool";
    messages[1].content = "1: write_file(\"game.py\") → ok";
    messages[1].tool_name = "write_file";
    messages[2].role = "assistant";
    messages[2].content = "created game.py";
    messages[3].role = "notice";
    messages[3].content = "left agent mode";
    agent::AgentMessageRecord thinking;
    thinking.role = "thinking";
    thinking.content = "Thinking: secret analysis";
    messages.push_back(thinking);
    agent::AgentMessageRecord notice;
    notice.role = "notice";
    notice.content = "display only";
    messages.push_back(notice);
    const std::string context = agent::build_prior_session_context(messages, 4000);
    check(!context.empty(), "prior context non-empty");
    check(context.find("write a game") != std::string::npos, "includes prior user goal");
    check(context.find("write_file") != std::string::npos, "includes tool activity");
    check(context.find("left agent mode") == std::string::npos, "skips leave notices");
    check(context.find("secret analysis") == std::string::npos,
          "skips thinking previews");
    check(context.find("display only") == std::string::npos,
          "skips every display notice");
    check(agent::estimate_transcript_tokens(messages) <
              agent::estimate_transcript_tokens(
                  {messages[0], messages[1], messages[2]}) + 8,
          "display-only roles do not materially affect transcript token estimates");
}

void test_reasoning_preview_unicode_redaction_and_limits() {
    const std::string preview = agent::format_reasoning_preview(
        u8"  e\u0301 \n 👩‍💻 你好 SECRET trailing", 22, {"SECRET"});
    check(preview.rfind("Thinking: ", 0) == 0, "reasoning preview has stable prefix");
    check(preview.find('\n') == std::string::npos &&
              preview.find("  ") == std::string::npos,
          "reasoning preview normalizes whitespace");
    check(preview.find("SECRET") == std::string::npos,
          "reasoning preview redacts configured secrets");
    check(agent::format_reasoning_preview("anything", 0, {}).empty(),
          "zero disables reasoning previews");
    const std::string clipped =
        agent::format_reasoning_preview(u8"😀😀😀😀", 13, {});
    check(clipped == u8"Thinking: 😀😀…",
          "reasoning preview clips by grapheme with ellipsis inside limit");
}

}  // namespace

void run_all() {
    test_resolve_clean_workspace();
    test_reject_parent_ainiux();
    test_home_ainiux_profile_is_not_a_project();
    test_reject_nested_child_ainiux();
    test_new_project_target_resolution_and_validation();
    test_compact_threshold_defaults();
    test_tool_display_format();
    test_tool_display_clips_to_width();
    test_elapsed_seconds_format();
    test_prior_session_context_includes_recent_work();
    test_reasoning_preview_unicode_redaction_and_limits();
}

}  // namespace ainiux::test::agent_project_root
