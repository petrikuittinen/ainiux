#include "agent/test_project_root.hpp"

#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <string>
#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "agent/compact.hpp"
#include "agent/history_backup.hpp"
#include "agent/project_root.hpp"
#include "agent/reasoning_preview.hpp"
#include "agent/tool_display.hpp"
#include "platform/environment.hpp"
#include "support/test_support.hpp"

namespace ainiux::test::agent_project_root {
namespace {
using ainiux::test::check;
namespace fs = std::filesystem;

std::string temp_dir(const std::string& name) {
    const fs::path root =
        fs::temp_directory_path() / ("ainiux-proj-" + name + "-" +
                                     std::to_string(ainiux::platform::current_process_id()));
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

#if defined(_WIN32)
void test_windows_protected_names_are_case_insensitive() {
    const std::string root = temp_dir("windows-protected-case");
    fs::create_directories(fs::u8path(root) / "pkg" / ".AINIUX-PR");
    std::string absolute;
    Error error = agent::resolve_agent_project_root(root, absolute);
    check(!error.ok(),
          "Windows nested project markers compare case-insensitively");

    const fs::path target = fs::u8path(root) / "target";
    fs::create_directories(target / ".AINIUX-PR");
    agent::NewProjectTarget resolved;
    error = agent::resolve_new_project_target(root, target.u8string(), resolved);
    check(error.ok() && resolved.state_dir_exists,
          "Windows /new recognizes case-variant project state");
    std::error_code ignored;
    fs::remove_all(fs::u8path(root), ignored);
}
#endif

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

    const std::optional<std::string> old_home =
        ainiux::test::test_environment("HOME");
    const fs::path home_project = fs::path(root) / "home project";
    fs::create_directories(home_project);
    ainiux::test::set_test_environment("HOME", root);
    error = agent::resolve_new_project_target(root, "~/home project", target);
    check(error.ok() && target.root == fs::canonical(home_project).generic_string(),
          "/new expands ~/ from HOME");
    error = agent::resolve_new_project_target(root, "~someone/project", target);
    check(!error.ok(), "/new rejects ~user expansion");
    if (old_home.has_value())
        ainiux::test::set_test_environment("HOME", *old_home);
    else
        ainiux::test::unset_test_environment("HOME");

    const fs::path locked = fs::path(root) / "locked";
    fs::create_directories(locked);
    std::error_code permission_ec;
    fs::permissions(locked, fs::perms::owner_read, fs::perm_options::replace,
                    permission_ec);
#if !defined(_WIN32)
    if (!permission_ec && ::geteuid() != 0) {
        error = agent::resolve_new_project_target(root, locked.string(), target);
        check(!error.ok() && error.code == ErrorCode::FileWrite,
              "/new rejects inaccessible target directories");
    }
#endif
    fs::permissions(locked, fs::perms::owner_all, fs::perm_options::replace,
                    permission_ec);

    std::error_code ec;
    fs::remove_all(root, ec);
}

void test_compact_threshold_defaults() {
    check(agent::effective_compact_limit_percent(0, 128000) == 75, "large window default 75");
    check(agent::effective_compact_limit_percent(0, 65536) == 75, "64k window default 75");
    check(agent::effective_compact_limit_percent(0, 8000) == 75, "small window default 75");
    check(agent::effective_compact_limit_percent(60, 128000) == 60, "explicit override");
    check(!agent::should_auto_compact(false, 75, 100000, 90000), "auto off never compact");
    check(agent::should_auto_compact(true, 75, 100000, 80000), "at 80% of 100k with 75% limit");
    check(!agent::should_auto_compact(true, 75, 100000, 70000), "below 75% threshold");
}

void test_compaction_strategies_timeline_and_partition() {
    CompactionStrategy strategy = CompactionStrategy::Fast;
    check(agent::parse_compaction_strategy("SMART", strategy) &&
              strategy == CompactionStrategy::Smart &&
              std::string(agent::compaction_strategy_name(strategy)) == "smart",
          "compaction strategy parsing is strict and case-insensitive");
    check(!agent::parse_compaction_strategy("automatic", strategy),
          "unknown compaction strategy is rejected");

    std::vector<agent::AgentMessageRecord> messages;
    auto message = [&](long long seq, const std::string& role,
                       const std::string& content,
                       const std::string& tool = std::string()) {
        agent::AgentMessageRecord row;
        row.seq = seq;
        row.role = role;
        row.content = content;
        row.tool_name = tool;
        messages.push_back(std::move(row));
    };
    message(0, "index", "| Language | Files |\n| --- | ---: |");
    message(1, "user", "goal");
    message(2, "notice", "display only");
    message(3, "assistant", "inspect");
    message(4, "thinking", "hidden thought");
    message(5, "tool", "compact duplicate", "read_file");
    for (long long seq = 7; seq <= 14; ++seq)
        message(seq, seq % 2 ? "user" : "assistant",
                "message " + std::to_string(seq));
    agent::AgentToolEventRecord tool;
    tool.seq = 6;
    tool.tool_name = "read_file";
    tool.arguments = R"({"path":"src/main.cpp"})";
    tool.result = R"({"ok":true,"content":"main"})";

    const auto timeline =
        agent::build_compaction_timeline(messages, {tool});
    bool saw_display = false;
    int tool_items = 0;
    std::string read_file_body;
    for (const auto& item : timeline) {
        saw_display = saw_display || item.role == "notice" ||
                      item.role == "thinking" || item.role == "index";
        if (item.role == "tool") {
            ++tool_items;
            if (item.tool_name == "read_file") read_file_body = item.content;
        }
    }
    check(!saw_display && tool_items == 1,
          "timeline excludes display roles and keeps one full logical tool unit");
    check(agent::is_reloadable_file_read_tool("read_file") &&
              agent::is_reloadable_file_read_tool("read_many") &&
              agent::is_reloadable_file_read_tool("grep") &&
              !agent::is_reloadable_file_read_tool("run_command") &&
              !agent::is_reloadable_file_read_tool("edit_file"),
          "reloadable stub-tier tools are classified correctly");
    check(agent::tool_compaction_tier("list_directory") ==
              agent::ToolCompactionTier::Prune &&
              agent::tool_compaction_tier("search_text") ==
                  agent::ToolCompactionTier::Stub &&
              agent::tool_compaction_tier("edit_file") ==
                  agent::ToolCompactionTier::Digest &&
              agent::tool_compaction_tier("run_command") ==
                  agent::ToolCompactionTier::Semantic &&
              agent::tool_compaction_tier("unknown_tool") ==
                  agent::ToolCompactionTier::Full,
          "tool compaction tiers match the Fable policy table");
    check(read_file_body.find("src/main.cpp") != std::string::npos &&
              read_file_body.find("reloadable") != std::string::npos &&
              read_file_body.find("\"content\":\"main\"") == std::string::npos &&
              read_file_body.find("\nmain\n") == std::string::npos,
          "successful read_file timeline items keep the path and omit the body");

    const std::string large_body(8000, 'x');
    agent::AgentToolEventRecord large_read;
    large_read.seq = 100;
    large_read.tool_name = "read_file";
    large_read.arguments = R"({"path":"src/big.cpp","start_line":1,"end_line":400})";
    large_read.result =
        std::string(R"({"ok":true,"content":")") + large_body + "\"}";
    large_read.ok = true;
    agent::AgentToolEventRecord many_read;
    many_read.seq = 101;
    many_read.tool_name = "read_many";
    many_read.arguments =
        R"({"items":[{"path":"a.cpp"},{"path":"b.cpp"}]})";
    many_read.result =
        R"({"ok":true,"results":[{"path":"a.cpp","content":"aaa"},{"path":"b.cpp","content":"bbb"}]})";
    many_read.ok = true;
    agent::AgentToolEventRecord failed_read;
    failed_read.seq = 102;
    failed_read.tool_name = "read_file";
    failed_read.arguments = R"({"path":"missing.cpp"})";
    failed_read.result =
        R"({"ok":false,"error":{"code":"not_found","message":"file not found: missing.cpp"}})";
    failed_read.ok = false;
    agent::AgentToolEventRecord command;
    command.seq = 103;
    command.tool_name = "run_command";
    command.arguments = R"({"command":"echo hi"})";
    command.result =
        R"({"ok":true,"data":{"stdout":"hi\n","stderr":"","exit_status":0}})";
    command.ok = true;
    const std::string search_blob(4000, 'm');
    agent::AgentToolEventRecord search;
    search.seq = 104;
    search.tool_name = "search_text";
    search.arguments = R"({"query":"TODO"})";
    search.result = std::string(R"({"ok":true,"data":[)") +
                    R"({"path":"src/a.cpp","line":10,"text":")" + search_blob +
                    R"("},{"path":"src/b.cpp","line":20,"text":"x"}]})";
    search.ok = true;
    agent::AgentToolEventRecord edit;
    edit.seq = 105;
    edit.tool_name = "edit_file";
    edit.arguments =
        R"({"path":"src/a.cpp","ops":[{"type":"replace_text","old_text":"a","new_text":"b"}]})";
    edit.result =
        R"({"ok":true,"data":{"path":"src/a.cpp","new_file_hash":"abc123def456"}})";
    edit.ok = true;
    agent::AgentToolEventRecord list_dir;
    list_dir.seq = 106;
    list_dir.tool_name = "list_directory";
    list_dir.arguments = R"({"path":"src"})";
    list_dir.result =
        R"({"ok":true,"data":{"entries":["a.cpp","b.cpp","c.cpp","d.cpp"]}})";
    list_dir.ok = true;
    const std::string huge_unknown(3000, 'z');
    agent::AgentToolEventRecord unknown;
    unknown.seq = 107;
    unknown.tool_name = "mystery_tool";
    unknown.arguments = R"({"q":"x"})";
    unknown.result = std::string(R"({"ok":true,"blob":")") + huge_unknown + "\"}";
    unknown.ok = true;

    const auto stubbed = agent::build_compaction_timeline(
        {}, {large_read, many_read, failed_read, command, search, edit, list_dir,
             unknown});
    check(stubbed.size() == 8, "eight tool events become eight timeline items");
    for (const auto& item : stubbed) {
        if (item.tool_name == "read_file" && item.tool_ok) {
            check(item.content.find(large_body) == std::string::npos &&
                      item.content.find("src/big.cpp") != std::string::npos &&
                      item.content.find("Status: ok") != std::string::npos,
                  "large successful read_file is stubbed without body");
        } else if (item.tool_name == "read_many") {
            check(item.content.find("\"content\":\"aaa\"") == std::string::npos &&
                      item.content.find("a.cpp") != std::string::npos &&
                      item.content.find("reloadable") != std::string::npos,
                  "read_many keeps paths and drops result bodies");
        } else if (item.tool_name == "read_file" && !item.tool_ok) {
            check(item.content.find("file not found: missing.cpp") !=
                      std::string::npos &&
                      item.content.find("Status: failed") != std::string::npos &&
                      item.content.find(large_body) == std::string::npos,
                  "failed read_file keeps a bounded error and omits file body");
        } else if (item.tool_name == "run_command") {
            check(item.content.find("hi") != std::string::npos &&
                      item.content.find("Exit: 0") != std::string::npos,
                  "semantic run_command keeps a short exit-aware digest");
        } else if (item.tool_name == "search_text") {
            check(item.content.find(search_blob) == std::string::npos &&
                      item.content.find("src/a.cpp") != std::string::npos &&
                      item.content.find("reloadable") != std::string::npos,
                  "search_text keeps hit paths and drops match bodies");
        } else if (item.tool_name == "edit_file") {
            check(item.content.find("src/a.cpp") != std::string::npos &&
                      item.content.find("new_text") == std::string::npos &&
                      item.content.find("Status: ok") != std::string::npos &&
                      item.primary_path == "src/a.cpp",
                  "edit_file digest keeps path and drops op bodies");
        } else if (item.tool_name == "list_directory") {
            check(item.content.find("list_directory") != std::string::npos &&
                      item.content.find("-> ok") != std::string::npos &&
                      item.content.find("c.cpp") == std::string::npos,
                  "list_directory is pruned to one status line");
        } else if (item.tool_name == "mystery_tool") {
            check(item.content.find(huge_unknown) == std::string::npos &&
                      item.content.find("re-run to reload") != std::string::npos,
                  "oversized unknown tools use the size-based stub");
        }
    }
    // render_compaction_source only emits middle history; feed stubbed tools
    // there directly so a large window cannot absorb them into the tail.
    agent::CompactionPartition render_partition;
    for (const auto& item : stubbed) {
        if (agent::is_reloadable_file_read_tool(item.tool_name) ||
            item.tool_name == "run_command" || item.tool_name == "edit_file" ||
            item.tool_name == "list_directory" || item.tool_name == "mystery_tool")
            render_partition.middle.push_back(item);
    }
    const std::string source = agent::render_compaction_source(render_partition);
    check(!source.empty() && source.find(large_body) == std::string::npos &&
              source.find("src/big.cpp") != std::string::npos &&
              source.find("reloadable") != std::string::npos &&
              source.find("\"content\":\"aaa\"") == std::string::npos &&
              source.find(search_blob) == std::string::npos &&
              source.find(huge_unknown) == std::string::npos,
          "summarizer source omits reloadable and oversized tool bodies");
    const std::string direct_stub = agent::stub_reloadable_tool_item_content(
        "read_file", R"({"path":"src/big.cpp"})", large_read.result, true);
    check(direct_stub.find(large_body) == std::string::npos &&
              direct_stub.find("src/big.cpp") != std::string::npos,
          "stub helper itself never copies successful file content");

    // Middle pre-shrink: consecutive reads merge; lone read-then-edit drops read.
    agent::AgentToolEventRecord read_a;
    read_a.seq = 200;
    read_a.tool_name = "read_file";
    read_a.arguments = R"({"path":"src/a.cpp"})";
    read_a.result = R"({"ok":true,"data":{"path":"src/a.cpp","content":"A"}})";
    read_a.ok = true;
    agent::AgentToolEventRecord read_b;
    read_b.seq = 201;
    read_b.tool_name = "read_file";
    read_b.arguments = R"({"path":"src/b.cpp"})";
    read_b.result = R"({"ok":true,"data":{"path":"src/b.cpp","content":"B"}})";
    read_b.ok = true;
    agent::AgentToolEventRecord read_c;
    read_c.seq = 202;
    read_c.tool_name = "read_file";
    read_c.arguments = R"({"path":"src/c.cpp"})";
    read_c.result = R"({"ok":true,"data":{"path":"src/c.cpp","content":"C"}})";
    read_c.ok = true;
    agent::AgentToolEventRecord edit_c;
    edit_c.seq = 203;
    edit_c.tool_name = "edit_file";
    edit_c.arguments = R"({"path":"src/c.cpp","ops":[{"type":"insert_at","line":1,"new_text":"x"}]})";
    edit_c.result =
        R"({"ok":true,"data":{"path":"src/c.cpp","new_file_hash":"deadbeef"}})";
    edit_c.ok = true;
    agent::AgentToolEventRecord fail_cmd;
    fail_cmd.seq = 204;
    fail_cmd.tool_name = "run_command";
    fail_cmd.arguments = R"({"command":"make test"})";
    fail_cmd.result =
        R"({"ok":false,"data":{"stdout":"","stderr":"error: undefined reference to foo\n","exit_status":2}})";
    fail_cmd.ok = false;
    // Group A: two consecutive reads (merge). Gap via edit_c after lone read_c.
    auto merge_timeline = agent::build_compaction_timeline(
        {}, {read_a, read_b, read_c, edit_c, fail_cmd});
    // After build, insert an assistant spacer between read_b and read_c so
    // only a/b merge and read_c remains for the read-then-edit rule.
    {
        agent::CompactionLogicalItem spacer;
        spacer.seq = 2015;
        spacer.role = "assistant";
        spacer.content = "looking at c next";
        spacer.estimated_tokens = 4;
        merge_timeline.insert(merge_timeline.begin() + 2, std::move(spacer));
    }
    agent::pre_shrink_compaction_middle(merge_timeline);
    bool saw_merged_reads = false;
    bool saw_lone_read_c = false;
    bool saw_edit_c = false;
    for (const auto& item : merge_timeline) {
        if (item.tool_name == "read_many" &&
            item.content.find("merged") != std::string::npos)
            saw_merged_reads = true;
        if (item.tool_name == "read_file" &&
            item.primary_path == "src/c.cpp")
            saw_lone_read_c = true;
        if (item.tool_name == "edit_file" && item.primary_path == "src/c.cpp")
            saw_edit_c = true;
    }
    check(saw_merged_reads && saw_edit_c && !saw_lone_read_c,
          "pre-shrink merges consecutive reads and drops read-then-edit pairs");
    const auto keep = agent::harvest_compaction_keep_list(merge_timeline);
    bool keep_has_edit = false;
    bool keep_has_fail = false;
    for (const auto& line : keep.lines) {
        keep_has_edit = keep_has_edit || line.find("edit_file") != std::string::npos;
        keep_has_fail =
            keep_has_fail || line.find("run_command") != std::string::npos;
    }
    check(keep_has_edit && keep_has_fail,
          "keep-list harvests mutation digests and failed commands");
    const std::string guidance = agent::compaction_summary_user_guidance(keep);
    check(guidance.find("Verified facts") != std::string::npos &&
              guidance.find("## Active Task") != std::string::npos &&
              guidance.find("edit_file") != std::string::npos,
          "summary user guidance seeds verified facts and heading skeleton");

    const auto partition =
        agent::partition_compaction_timeline(timeline, 200);
    check(partition.head.size() == 3 && partition.tail.size() >= 3 &&
              partition.tail.size() <= 20 && !partition.middle.empty(),
          "first compaction retains three head items and a bounded whole-item tail");
    check(partition.tail_budget_tokens == 16,
          "tail budget uses 8% of the context window");
    check(agent::partition_compaction_timeline(timeline, 100000)
                  .tail_budget_tokens == 8000,
          "tail budget scales to 8% of a large window");

    message(15, "summary", "prior structured checkpoint");
    message(16, "user", "new follow-up");
    message(17, "assistant", "new work");
    message(18, "user", "more work");
    message(19, "assistant", "latest");
    const auto repeated = agent::partition_compaction_timeline(
        agent::build_compaction_timeline(messages, {}), 100);
    check(repeated.head.empty() &&
              repeated.prior_summary == "prior structured checkpoint",
          "repeated compaction carries the previous summary instead of duplicating head");

    // Use a larger window so the partition's tail budget is not tiny; the
    // "does not escalate" check should not fire just because the toy window
    // was 200 tokens.
    const auto roomy_partition =
        agent::partition_compaction_timeline(timeline, 100000);
    agent::CompactionKeepList seed_keep;
    seed_keep.lines.push_back("edit_file src/a.cpp ok");
    const auto fast =
        agent::build_fast_compaction_candidate(roomy_partition, 1000, seed_keep);
    std::string reason;
    check(!fast.checkpoint.empty() &&
              fast.checkpoint.find("edit_file src/a.cpp ok") != std::string::npos &&
              !agent::smart_compaction_should_escalate(
                  fast, 10000, 7500, roomy_partition.tail_budget_tokens, reason),
          "fast checkpoint embeds keep-list facts and does not escalate when small");
    agent::FastCompactionCandidate lossy = fast;
    lossy.omitted_substantive_tokens = 2000;
    check(agent::smart_compaction_should_escalate(
              lossy, 10000, 7500, 1500, reason) &&
              !reason.empty(),
          "smart strategy escalates when substantive history is omitted");
    check(agent::compaction_summary_input_budget(10000) == 6000 &&
              agent::compaction_summary_input_budget(0) == 8000 &&
              agent::compaction_summary_output_budget(8000, 10000) == 512 &&
              agent::compaction_summary_output_budget(80000, 128000) == 2000 &&
              agent::compaction_summary_model_timeout_ms() == 30000,
          "summary input/output budgets and model timeout follow context clamps");
    check(agent::compaction_summary_reasoning(
              {ReasoningSelection::named("low"),
               ReasoningSelection::named("disabled")}) ==
              ReasoningSelection::named("disabled") &&
              agent::compaction_summary_reasoning(
                  {ReasoningSelection::named("high"),
                   ReasoningSelection::named("minimal")}) ==
                  ReasoningSelection::named("minimal"),
          "summary reasoning prefers disabling then minimal catalogue values");
    const std::string schema =
        agent::compaction_summary_schema_prompt("user goal");
    check(schema.find("read_file") != std::string::npos &&
              schema.find("read_many") != std::string::npos &&
              schema.find("grep") != std::string::npos &&
              schema.find("Never paste source") != std::string::npos &&
              schema.find("verified-facts") != std::string::npos &&
              schema.find("Active Task") != std::string::npos,
          "summary schema covers reduced tools and verified-facts retention");
    check(agent::compaction_checkpoint_wrapper("checkpoint text")
                  .find("re-read workspace files") != std::string::npos,
          "checkpoint wrapper tells the model to re-read files");
    check(agent::format_compaction_progress(CompactionStrategy::Summary, 0) ==
              "Compacting context using summary." &&
              agent::format_compaction_progress(CompactionStrategy::Summary, 1) ==
                  "Compacting context using summary.." &&
              agent::format_compaction_progress(CompactionStrategy::Summary, 2) ==
                  "Compacting context using summary..." &&
              agent::format_compaction_success_notice(116, 12000, 4500) ==
                  "Compacting context succeeded in 1 min 56 seconds. "
                  "~7500 tokens saved. 4500 tokens in remaining context." &&
              agent::format_compaction_success_notice(1, 100, 40) ==
                  "Compacting context succeeded in 1 second. "
                  "~60 tokens saved. 40 tokens in remaining context." &&
              agent::format_compaction_success_notice(0, 0, 0) ==
                  "Compacting context succeeded in 0 seconds. "
                  "~0 tokens saved. 0 tokens in remaining context.",
          "compaction success reports elapsed time, tokens saved, and remaining");
    check(agent::format_compaction_no_op_notice(8900).find(
              "Compacting context skipped.") == 0 &&
              agent::format_compaction_no_op_notice(8900).find(
                  "~8900 tokens remain.") != std::string::npos &&
              agent::format_compaction_failure_notice(
                  "provider\nreturned an error")
                      .find('\n') == std::string::npos,
          "no-op and failure outcomes are explicit one-line notices");
}

void test_tool_display_format() {
    const std::string line = agent::format_compact_tool_line(
        1, "read_file", R"({"path":"example.txt"})", R"({"ok":true})", 150, 80);
    check(line.find("1: read_file(") != std::string::npos, "index and name");
    check(line.find("example.txt") != std::string::npos, "path preview");
    check(line.find("→ ok") != std::string::npos || line.find("ok") != std::string::npos, "ok status");
    check(line.find("in 150 ms") != std::string::npos, "independent tool duration");

    const std::string many = agent::format_compact_tool_line(
        2, "read_many",
        R"({"items":[{"path":"README.md"},{"path":"AGENTS.md"}],"max_bytes":131072})",
        R"({"ok":true})", 240, 120);
    check(many.find("read_many(2 reads:") != std::string::npos &&
              many.find("README.md") != std::string::npos &&
              many.find("AGENTS.md") != std::string::npos,
          "read_many compact activity shows one batch with its paths: " + many);
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
    check(agent::format_task_complete(60000) == "Task complete in 1 minute.",
          "task complete exact minute has no zero-second tail");
    check(agent::format_task_complete(61000) == "Task complete in 1 minute and 1 second.",
          "task complete one minute and one second");
    check(agent::format_task_complete(1084130) ==
              "Task complete in 18 minutes and 4 seconds.",
          "task complete long runs use minutes and seconds");
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
    agent::AgentMessageRecord index_report;
    index_report.role = "index";
    index_report.content = "| Language | Files |\n| --- | ---: |";
    messages.push_back(index_report);
    const std::string context = agent::build_prior_session_context(messages, 4000);
    check(!context.empty(), "prior context non-empty");
    check(context.find("write a game") != std::string::npos, "includes prior user goal");
    check(context.find("write_file") != std::string::npos, "includes tool activity");
    check(context.find("left agent mode") == std::string::npos, "skips leave notices");
    check(context.find("secret analysis") == std::string::npos,
          "skips thinking previews");
    check(context.find("display only") == std::string::npos,
          "skips every display notice");
    check(context.find("Language") == std::string::npos,
          "skips display-only index reports");
    check(agent::estimate_transcript_tokens(messages) <
              agent::estimate_transcript_tokens(
                  {messages[0], messages[1], messages[2]}) + 8,
          "display-only roles do not materially affect transcript token estimates");
}

void test_reasoning_preview_unicode_redaction_and_limits() {
    const std::string preview = agent::format_reasoning_preview(
        u8"  e\u0301 \n 👩‍💻 你好 SECRET trailing", 22, {"SECRET"});
    check(preview.rfind("Thinking: ", 0) == 0, "reasoning preview has stable prefix");
    check(preview.find('\n') == std::string::npos && preview.find('\r') == std::string::npos,
          "reasoning preview maps vertical whitespace for a single history row");
    check(preview.find("SECRET") == std::string::npos,
          "reasoning preview redacts configured secrets");
    check(agent::format_reasoning_preview("anything", 0, {}).empty(),
          "zero disables reasoning previews");
    const std::string clipped =
        agent::format_reasoning_preview(u8"😀😀😀😀", 13, {});
    check(clipped == u8"Thinking: 😀😀…",
          "reasoning preview clips by grapheme with ellipsis inside limit");

    // Provider spacing is preserved (Kimi whitespace gluer is intentionally not
    // applied — it corrupted previews on other models, e.g. "I need" → "Ineed").
    const std::string spaced = agent::format_reasoning_preview(
        "So the current state of server .c is inconsistent. The user is asking "
        "about blocking vs non-blocking. Let me look at parse _http and "
        "End -to -end HTTP / 1 . 1. I need main.c fully.",
        400, {});
    check(spaced.find("server .c") != std::string::npos, "keeps spaced dots: " + spaced);
    check(spaced.find("user is") != std::string::npos &&
              spaced.find("Let me") != std::string::npos &&
              spaced.find("I need") != std::string::npos &&
              spaced.find("main.c fully") != std::string::npos,
          "keeps normal English word spaces: " + spaced);
    check(spaced.find("Ineed") == std::string::npos &&
              spaced.find("main.cfully") == std::string::npos &&
              spaced.find("Letme") == std::string::npos,
          "does not glue English words: " + spaced);

    const std::string newline_row = agent::format_reasoning_preview("I\nneed make clean", 80, {});
    check(newline_row.find("I need") != std::string::npos &&
              newline_row.find("Ineed") == std::string::npos,
          "newline becomes a space without gluing single-letter words: " + newline_row);
}

void test_reasoning_idle_slice_advances_sentences_and_partials() {
    const std::string normalized = agent::normalize_reasoning_preview_text(
        "First idea here. Second thought continues. Third without end yet", {});
    check(normalized.find("First idea here.") != std::string::npos,
          "normalize keeps sentence punctuation");

    std::size_t offset = 0;
    agent::ReasoningIdleSlice first =
        agent::take_reasoning_idle_slice(normalized, offset, 40, false);
    check(first.text == "First idea here." && first.next_offset > offset,
          "idle slice takes the first complete sentence: " + first.text);
    offset = first.next_offset;

    agent::ReasoningIdleSlice second =
        agent::take_reasoning_idle_slice(normalized, offset, 40, false);
    check(second.text == "Second thought continues." &&
              second.next_offset > offset,
          "idle slice advances to the next sentence: " + second.text);
    offset = second.next_offset;

    agent::ReasoningIdleSlice incomplete =
        agent::take_reasoning_idle_slice(normalized, offset, 40, false);
    check(incomplete.text.empty(),
          "without force_partial, incomplete tail waits for a boundary");

    agent::ReasoningIdleSlice forced =
        agent::take_reasoning_idle_slice(normalized, offset, 12, true);
    check(!forced.text.empty() && forced.next_offset > offset,
          "force_partial emits a clipped monologue chunk: " + forced.text);
    check(agent::format_reasoning_preview(forced.text, 30, {}).rfind("Thinking: ", 0) ==
              0,
          "idle chunks use the same Thinking preview formatter");

    // Avoid treating version/file dots as sentence ends.
    const std::string technical = agent::normalize_reasoning_preview_text(
        "Use HTTP/1.1 and server.c carefully. Then continue.", {});
    agent::ReasoningIdleSlice tech =
        agent::take_reasoning_idle_slice(technical, 0, 80, false);
    check(tech.text.find("server.c") != std::string::npos &&
              tech.text.find("Then") == std::string::npos,
          "sentence split skips technical dots: " + tech.text);

    check(agent::reasoning_active_slice(normalized, first.next_offset)
                  .rfind("Third without", 0) == 0,
          "active slice prefers the newest incomplete fragment");
    check(agent::reasoning_active_slice("Only one complete sentence.", 0) ==
              "Only one complete sentence.",
          "active slice falls back to the latest complete sentence");
}

void test_reasoning_sticky_slice_prefers_first_thought_and_backtracks() {
    const std::string monologue = agent::normalize_reasoning_preview_text(
        "User wants me to refactor the game engine. I should first look at "
        "src/game/engine.js and the related tests. Then I will plan the "
        "changes carefully. Good.",
        {});

    agent::ReasoningStickySlice head =
        agent::reasoning_sticky_slice(monologue, 0, 110);
    check(head.text.find("User wants me to refactor") != std::string::npos,
          "sticky prefers the first thought: " + head.text);
    check(head.text.find("Good.") == std::string::npos ||
              head.text.find("User wants") != std::string::npos,
          "sticky does not freeze only the stream tail: " + head.text);
    check(head.next_offset > 0 && head.next_offset <= monologue.size(),
          "sticky advances a forward next_offset");

    // Budget packing: first thought fills content graphemes without taking the
    // irrelevant closer when there is ample early text.
    check(head.text.rfind("Good.") == std::string::npos ||
              head.text.find("User wants me to refactor the game engine.") !=
                  std::string::npos,
          "head fill keeps the opening plan: " + head.text);

    // Short closer remainder: backtrack so "Good." carries preceding context.
    const std::string with_closer = agent::normalize_reasoning_preview_text(
        "The unit tests and smoke test passed. Good.", {});
    const std::size_t good_offset = with_closer.rfind("Good.");
    check(good_offset != std::string::npos, "fixture has Good. closer");
    agent::ReasoningStickySlice short_tail =
        agent::reasoning_sticky_slice(with_closer, good_offset, 80);
    check(short_tail.text.find("Good") != std::string::npos,
          "short sticky still ends on the closer: " + short_tail.text);
    check(short_tail.text.find("unit tests") != std::string::npos ||
              short_tail.text.find("smoke test") != std::string::npos,
          "short sticky backtracks for preceding context: " + short_tail.text);
    check(short_tail.next_offset > good_offset,
          "short sticky still advances past the closer unit");

    agent::ReasoningStickySlice only_good =
        agent::reasoning_sticky_slice("Good.", 0, 80);
    check(only_good.text.find("Good") != std::string::npos,
          "lone short closer stays usable: " + only_good.text);

    agent::ReasoningStickySlice empty =
        agent::reasoning_sticky_slice("anything", 0, 0);
    check(empty.text.empty(), "zero content budget disables sticky text");

    // Technical dots must not split the first thought early.
    const std::string technical = agent::normalize_reasoning_preview_text(
        "Use HTTP/1.1 and server.c carefully. Then continue with the rest.",
        {});
    agent::ReasoningStickySlice tech =
        agent::reasoning_sticky_slice(technical, 0, 80);
    check(tech.text.find("server.c") != std::string::npos &&
              tech.text.find("HTTP/1.1") != std::string::npos,
          "sticky sentence split skips technical dots: " + tech.text);
}

}  // namespace

void test_history_backup_path_and_enclosing_project() {
    const std::string root = temp_dir("history-bak");
    fs::create_directories(fs::path(root) / ".ainiux-pr" / "history");
    fs::create_directories(fs::path(root) / "src");
    {
        std::ofstream f(fs::path(root) / "src" / "a.cpp");
        f << "int main() {}\n";
    }

    const std::string name1 = agent::history_backup_filename("src/a.cpp");
    const std::string name2 = agent::history_backup_filename("src/a.cpp");
    check(name1 == name2 && !name1.empty(), "history filename is stable for same relative path");
    check(name1 != agent::history_backup_filename("src/b.cpp"),
          "different relative paths get different history names");
    check(name1.size() > 4 && name1.compare(name1.size() - 4, 4, ".bak") == 0,
          "history filename ends with .bak");
    check(name1.find('-') != std::string::npos, "history filename has hash-tail form");
    // generic_u8string keeps a leading "./" distinction; tools pass cleaned relative paths.
    check(agent::history_backup_filename("src/nested/file.hpp").find(".bak") != std::string::npos,
          "nested relative path produces a .bak name");

    const std::string full = agent::history_backup_path(root, "src/a.cpp");
    check(full.find(".ainiux-pr") != std::string::npos && full.find("history") != std::string::npos,
          "history path nests under .ainiux-pr/history");
    check(full.find(name1) != std::string::npos, "history path uses stable filename");

    std::string found_root;
    check(agent::find_enclosing_project_root((fs::path(root) / "src" / "a.cpp").string(),
                                             found_root),
          "finds enclosing project from nested file");
    std::error_code ec;
    check(fs::equivalent(fs::u8path(found_root), fs::u8path(root), ec) ||
              fs::weakly_canonical(found_root) == fs::weakly_canonical(root),
          "enclosing root matches project");

    std::string relative;
    check(agent::project_relative_path(root, (fs::path(root) / "src" / "a.cpp").string(), relative),
          "project_relative_path succeeds");
    check(relative == "src/a.cpp", "relative path is generic src/a.cpp got: " + relative);

    std::string outside_root;
    check(!agent::find_enclosing_project_root(temp_dir("no-project"), outside_root),
          "no project outside .ainiux-pr tree");

    std::error_code rm;
    fs::remove_all(root, rm);
}

void run_all() {
    test_resolve_clean_workspace();
    test_reject_parent_ainiux();
    test_home_ainiux_profile_is_not_a_project();
    test_reject_nested_child_ainiux();
#if defined(_WIN32)
    test_windows_protected_names_are_case_insensitive();
#endif
    test_new_project_target_resolution_and_validation();
    test_history_backup_path_and_enclosing_project();
    test_compact_threshold_defaults();
    test_compaction_strategies_timeline_and_partition();
    test_tool_display_format();
    test_tool_display_clips_to_width();
    test_elapsed_seconds_format();
    test_prior_session_context_includes_recent_work();
    test_reasoning_preview_unicode_redaction_and_limits();
    test_reasoning_idle_slice_advances_sentences_and_partials();
    test_reasoning_sticky_slice_prefers_first_thought_and_backtracks();
}

}  // namespace ainiux::test::agent_project_root
