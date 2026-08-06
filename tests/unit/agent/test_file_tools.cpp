#include "agent/test_file_tools.hpp"

#include <cstdlib>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "agent/index/index.hpp"
#include "agent/process.hpp"
#include "agent/tools.hpp"
#include "json/json.hpp"
#include "provider/provider.hpp"
#include "platform/environment.hpp"
#include "support/test_support.hpp"

namespace ainiux::test::agent_file_tools {
namespace {
using ainiux::test::check;
namespace fs = std::filesystem;

std::string write_temp_workspace(const std::string& name) {
    const fs::path root =
        fs::temp_directory_path() / ("ainiux-agent-file-tools-" + name + "-" +
                                     std::to_string(ainiux::platform::current_process_id()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "src", ec);
    {
        std::ofstream out(root / "src" / "hello.cpp");
        out << "int main() { return 0; }\n";
    }
    return root.string();
}

void write_text(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool json_ok(const std::string& result) {
    const json::ParseResult parsed = json::parse(result);
    if (!parsed.error.ok() || !parsed.value.is_object()) return false;
    const json::Value* ok = parsed.value.get("ok");
    return ok != nullptr && ok->type == json::Value::Type::Bool && ok->boolean;
}

std::string json_error_code(const std::string& result) {
    const json::ParseResult parsed = json::parse(result);
    if (!parsed.error.ok() || !parsed.value.is_object()) return {};
    const json::Value* error = parsed.value.get("error");
    if (error == nullptr || !error->is_object()) return {};
    const json::Value* code = error->get("code");
    return code != nullptr && code->is_string() ? code->string : std::string{};
}

std::string json_data_string(const std::string& result, const std::string& key) {
    const json::ParseResult parsed = json::parse(result);
    if (!parsed.error.ok() || !parsed.value.is_object()) return {};
    const json::Value* data = parsed.value.get("data");
    if (data == nullptr || !data->is_object()) return {};
    const json::Value* value = data->get(key);
    return value != nullptr && value->is_string() ? value->string : std::string{};
}

std::string json_string(const std::string& text) {
    json::Value value;
    value.type = json::Value::Type::String;
    value.string = text;
    return json::stringify(value);
}

agent::ReadToolRegistry make_registry(const std::string& workspace,
                                      agent::MutationPolicy mutation_policy,
                                      bool auto_approve_create_dirs = false,
                                      agent::GuardApprovalCallback on_guard_ask = {},
                                      agent::PermissionMode permission_mode =
                                          agent::PermissionMode::Smart,
                                      bool permission_controls = false) {
    agent::index::Options options;
    options.workspace = workspace;
    options.max_source_code_file_size = 1024 * 1024;
    agent::index::RefreshStats stats;
    const Error refresh_error = agent::index::refresh(options, stats);
    check(refresh_error.ok(), "index refresh for file-tool fixture");
    agent::index::Snapshot snapshot;
    const Error load_error = agent::index::load_snapshot(options, snapshot);
    check(load_error.ok(), "load snapshot for file-tool fixture");
    agent::ReadToolRegistry tools;
    agent::ToolRegistryOptions tool_options;
    tool_options.mutation_policy = mutation_policy;
    tool_options.permission_mode = permission_mode;
    tool_options.permission_controls = permission_controls;
    if (on_guard_ask) {
        tool_options.on_guard_ask = std::move(on_guard_ask);
    } else if (auto_approve_create_dirs) {
        tool_options.on_guard_ask =
            [](const agent::GuardApprovalRequest& request,
               runtime::CancellationToken) -> agent::GuardApprovalDecision {
            // Tests only auto-approve directory creation; other Ask still denies.
            if (request.rule_id == "ask_on_create_dirs")
                return agent::GuardApprovalDecision::Allow;
            return agent::GuardApprovalDecision::Deny;
        };
    }
    const Error create_error =
        agent::ReadToolRegistry::create(std::move(options), std::move(snapshot), {}, tools,
                                        tool_options);
    check(create_error.ok(), "create tool registry");
    return tools;
}

agent::ReadToolRegistry make_registry(const std::string& workspace,
                                      bool allow_mutations,
                                      bool auto_approve_create_dirs);

agent::ReadToolRegistry make_lazy_registry(const std::string& workspace) {
    agent::index::Options options;
    options.workspace = workspace;
    options.max_source_code_file_size = 1024 * 1024;
    agent::index::RefreshStats stats;
    check(agent::index::refresh(options, stats).ok(),
          "index refresh for lazy registry fixture");
    agent::ReadToolRegistry tools;
    agent::ToolRegistryOptions tool_options;
    tool_options.mutation_policy = agent::MutationPolicy::Full;
    const Error error = agent::ReadToolRegistry::create_lazy(
        std::move(options), {}, tools, tool_options);
    check(error.ok() && tools.snapshot().files.empty() &&
              tools.snapshot().symbols.empty(),
          "lazy Agent registry attaches without an eager snapshot");
    return tools;
}

void test_external_file_access_requires_one_shot_approval() {
    const std::string workspace = write_temp_workspace("external-project");
    const fs::path outside_dir =
        fs::path(workspace).parent_path() /
        (fs::path(workspace).filename().string() + "-outside");
    std::error_code ec;
    fs::remove_all(outside_dir, ec);
    fs::create_directories(outside_dir, ec);
    check(!ec, "create external file fixture");
    const fs::path outside_read = outside_dir / "read.txt";
    const fs::path outside_write = outside_dir / "written.txt";
    write_text(outside_read, "outside line one\noutside line two\n");

    agent::ReadToolRegistry headless = make_registry(workspace, true, false);
    const std::string read_args =
        std::string("{\"path\":") + json_string(outside_read.generic_string()) +
        ",\"max_bytes\":4096}";
    const std::string denied_read = headless.execute("read_file", read_args);
    check(!json_ok(denied_read) && denied_read.find("approval") != std::string::npos,
          "headless external read is denied without approval: " + denied_read);
    const std::string write_args =
        std::string("{\"path\":") + json_string(outside_write.generic_string()) +
        ",\"content\":\"approved write\\n\",\"mode\":\"create_new\"}";
    const std::string denied_write = headless.execute("write_file", write_args);
    check(!json_ok(denied_write) && !fs::exists(outside_write),
          "headless external write is denied without touching the file");

    std::vector<std::string> asked_rules;
    agent::ReadToolRegistry approved = make_registry(
        workspace, agent::MutationPolicy::Full, false,
        [&](const agent::GuardApprovalRequest& request,
            runtime::CancellationToken) -> agent::GuardApprovalDecision {
            asked_rules.push_back(request.rule_id);
            check(request.command_preview.find(outside_dir.generic_string()) != std::string::npos,
                  "external approval shows exact resolved target");
            check(request.message.find("only to this tool call") != std::string::npos,
                  "external approval is explicitly one-shot");
            return agent::GuardApprovalDecision::Allow;
        });
    const std::string allowed_read = approved.execute("read_file", read_args);
    check(json_ok(allowed_read) &&
              allowed_read.find("outside line one") != std::string::npos &&
              !asked_rules.empty() && asked_rules.back() == "ask_on_external_file_read",
          "approved external read returns bounded file content: " + allowed_read);
    const std::string allowed_write = approved.execute("write_file", write_args);
    check(json_ok(allowed_write) && read_text(outside_write) == "approved write\n" &&
              asked_rules.size() == 2 &&
              asked_rules.back() == "ask_on_external_file_write",
          "approved external write changes only the exact requested file: " + allowed_write);
    check(json_data_string(allowed_write, "history_path").empty(),
          "external write does not claim a project history backup");

    int plan_asks = 0;
    agent::ReadToolRegistry plan = make_registry(
        workspace, agent::MutationPolicy::PlanningDocuments, false,
        [&](const agent::GuardApprovalRequest&,
            runtime::CancellationToken) -> agent::GuardApprovalDecision {
            ++plan_asks;
            return agent::GuardApprovalDecision::Allow;
        });
    const fs::path plan_target = outside_dir / "plan-write.txt";
    const std::string plan_args =
        std::string("{\"path\":") + json_string(plan_target.generic_string()) +
        ",\"content\":\"no\\n\"}";
    check(!json_ok(plan.execute("write_file", plan_args)) && plan_asks == 0 &&
              !fs::exists(plan_target),
          "Plan mode cannot approve or perform outside-project writes");

    agent::ReadToolRegistry user_denied = make_registry(
        workspace, agent::MutationPolicy::Full, false,
        [](const agent::GuardApprovalRequest&,
           runtime::CancellationToken) -> agent::GuardApprovalDecision {
            return agent::GuardApprovalDecision::Deny;
        });
    const fs::path denied_target = outside_dir / "denied.txt";
    const std::string user_denied_args =
        std::string("{\"path\":") + json_string(denied_target.generic_string()) +
        ",\"content\":\"no\\n\"}";
    const std::string user_denied_result =
        user_denied.execute("write_file", user_denied_args);
    check(!json_ok(user_denied_result) && !fs::exists(denied_target),
          "selecting No leaves the outside file untouched");

    fs::remove_all(outside_dir, ec);
    fs::remove_all(workspace, ec);
}

void test_permission_modes_and_native_path_tools() {
    const std::string workspace = write_temp_workspace("permission-modes");
    std::atomic<int> asks{0};
    auto allow = [&](const agent::GuardApprovalRequest&,
                     runtime::CancellationToken) {
        ++asks;
        return agent::GuardApprovalDecision::Allow;
    };
    agent::ReadToolRegistry confirm =
        make_registry(workspace, agent::MutationPolicy::Full, false, allow,
                      agent::PermissionMode::Confirm, true);
    const std::string confirmed = confirm.execute(
        "write_file", R"({"path":"confirmed.txt","content":"ok\n"})");
    check(json_ok(confirmed) && asks.load() == 1,
          "confirm mode asks once for project-native write");
    const std::string confirmed_command =
        confirm.execute("run_command", R"({"command":"pwd"})");
    check(json_ok(confirmed_command) && asks.load() == 2,
          "confirm mode asks once for every model-issued command");

    agent::ReadToolRegistry smart =
        make_registry(workspace, agent::MutationPolicy::Full, false, {},
                      agent::PermissionMode::Smart, true);
    const fs::path temp_parent =
        fs::path(workspace).parent_path() /
        (fs::path(workspace).filename().string() + "-native-tools");
    std::error_code ec;
    fs::remove_all(temp_parent, ec);
    const std::string created = smart.execute(
        "create_directory",
        "{\"path\":" + json_string((temp_parent / "nested").string()) +
            ",\"parents\":true}");
    check(json_ok(created) && fs::is_directory(temp_parent / "nested"),
          "smart mode permits native directory creation under system temp");
    write_text(temp_parent / "nested" / "from.txt", "hello\n");
    const std::string renamed = smart.execute(
        "rename_path",
        "{\"source\":" +
            json_string((temp_parent / "nested" / "from.txt").string()) +
            ",\"destination\":" +
            json_string((temp_parent / "nested" / "to.txt").string()) + "}");
    check(json_ok(renamed) && fs::exists(temp_parent / "nested" / "to.txt"),
          "rename_path renames an external temp file without copy fallback");
    const std::string listed = smart.execute(
        "list_directory",
        "{\"path\":" + json_string((temp_parent / "nested").string()) + "}");
    check(json_ok(listed) && listed.find("to.txt") != std::string::npos,
          "list_directory supports an exact external directory under temp");
    const std::string smart_command =
        smart.execute("run_command", R"({"command":"pwd"})");
    check(json_ok(smart_command),
          "smart mode auto-approves a vetted read-only project command");
    const std::string smart_build =
        smart.execute("run_command", R"({"command":"make test"})");
    check(!json_ok(smart_build) &&
              json_error_code(smart_build) == "policy_denied",
          "smart mode still requires approval for builds and tests");
    const std::string smart_external = smart.execute(
        "run_command",
        "{\"command\":\"pwd\",\"cwd\":" +
            json_string((temp_parent / "nested").string()) + "}");
    check(!json_ok(smart_external) &&
              smart_external.find("approval") != std::string::npos,
          "smart mode still requires approval for an external command cwd");
    std::atomic<int> smart_asks{0};
    agent::ReadToolRegistry smart_prompting = make_registry(
        workspace, agent::MutationPolicy::Full, false,
        [&](const agent::GuardApprovalRequest&,
            runtime::CancellationToken) -> agent::GuardApprovalDecision {
            ++smart_asks;
            return agent::GuardApprovalDecision::Deny;
        },
        agent::PermissionMode::Smart, true);
    check(json_ok(smart_prompting.execute(
              "run_command", R"({"command":"ls -laFg src"})")) &&
              smart_asks.load() == 0,
          "smart mode does not prompt for a vetted project read");
    check(!json_ok(smart_prompting.execute(
              "run_command", R"({"command":"make test"})")) &&
              smart_asks.load() == 1,
          "smart mode prompts for a non-vetted command");
    check(!json_ok(smart_prompting.execute(
              "run_command",
              "{\"command\":\"pwd\",\"cwd\":" +
                  json_string((temp_parent / "nested").string()) + "}")) &&
              smart_asks.load() == 2,
          "smart mode prompts for an external-path read-only command");
    const std::string absolute_project_file = smart.execute(
        "run_command",
        "{\"command\":" +
            json_string("cat " + (fs::path(workspace) / "src" / "hello.cpp").string()) +
            "}");
    check(json_ok(absolute_project_file) &&
              absolute_project_file.find("int main") != std::string::npos,
          "smart mode accepts a canonical absolute in-project read operand");
    check(!json_ok(smart.execute(
              "run_command", R"({"command":"cat ../outside.txt"})")),
          "smart mode rejects traversal before approval");
    check(!json_ok(smart.execute(
              "run_command", R"({"command":"ls .ainiux-pr"})")),
          "smart mode rejects protected metadata before approval");
    fs::create_symlink(temp_parent / "nested" / "to.txt",
                       fs::path(workspace) / "escape-link", ec);
    check(!json_ok(smart.execute(
              "run_command", R"({"command":"cat escape-link"})")),
          "smart mode rejects symlink operands before approval");
    check(!json_ok(smart.execute(
              "run_command", R"({"command":"pwd > owned"})")) &&
              !json_ok(smart.execute(
                  "run_command", "{\"command\":\"pwd $(whoami)\"}")),
          "redirects and substitutions remain structurally denied");

    agent::ReadToolRegistry yolo =
        make_registry(workspace, agent::MutationPolicy::Full, false, {},
                      agent::PermissionMode::Yolo, true);
    // Yolo skips hard Guard denials (sudo/shells/…) at user risk. Structural
    // shell-free rules (unquoted | & ;) still apply because run_command never
    // spawns a real shell.
    const std::string yolo_sudo =
        yolo.execute("run_command", R"({"command":"sudo true"})");
    check(json_ok(yolo_sudo) ||
              yolo_sudo.find("not found") != std::string::npos ||
              yolo_sudo.find("\"exit_status\"") != std::string::npos,
          "yolo elevates hard Guard denials (sudo runs or fails as a process): " +
              yolo_sudo);
    check(!json_ok(yolo.execute(
              "run_command", R"({"command":"echo hi | wc -l"})")),
          "yolo still rejects unquoted shell control operators");
    check(json_ok(yolo.execute(
              "run_command", R"({"command":"echo yolo-non-vetted"})")),
          "yolo remains prompt-free for other validated commands");
    const std::string external_cwd = yolo.execute(
        "run_command",
        "{\"command\":\"pwd\",\"cwd\":" +
            json_string((temp_parent / "nested").string()) + "}");
    check(json_ok(external_cwd) &&
              external_cwd.find((temp_parent / "nested").string()) !=
                  std::string::npos,
          "yolo run_command supports a validated canonical external cwd");
    const std::string external_operand = yolo.execute(
        "run_command",
        "{\"command\":" +
            json_string("stat " + (temp_parent / "nested" / "to.txt").string()) + "}");
    check(json_ok(external_operand),
          "yolo run_command supports an authorized standalone external path operand");
    const std::string removed = yolo.execute(
        "remove",
        "{\"path\":" + json_string(temp_parent.string()) +
            ",\"recursive\":true}");
    check(json_ok(removed) && !fs::exists(temp_parent),
          "yolo permits validated recursive external removal");
    fs::remove_all(workspace, ec);
}

agent::ReadToolRegistry make_registry(const std::string& workspace,
                                      bool allow_mutations,
                                      bool auto_approve_create_dirs = false) {
    return make_registry(workspace,
                         allow_mutations ? agent::MutationPolicy::Full
                                         : agent::MutationPolicy::Disabled,
                         auto_approve_create_dirs);
}

bool json_array_contains_string(const std::string& result, const std::string& needle) {
    const json::ParseResult parsed = json::parse(result);
    if (!parsed.error.ok() || !parsed.value.is_object()) return false;
    const json::Value* data = parsed.value.get("data");
    if (data == nullptr || !data->is_array()) return false;
    for (const json::Value& item : data->array) {
        if (item.is_string() && item.string == needle) return true;
    }
    return false;
}

bool json_array_contains_string_field(const std::string& result,
                                      const std::string& field,
                                      const std::string& needle) {
    const json::ParseResult parsed = json::parse(result);
    if (!parsed.error.ok() || !parsed.value.is_object()) return false;
    const json::Value* data = parsed.value.get("data");
    if (data == nullptr || !data->is_array()) return false;
    for (const json::Value& item : data->array) {
        if (!item.is_object()) continue;
        const json::Value* value = item.get(field);
        if (value != nullptr && value->is_string() && value->string == needle) return true;
    }
    return false;
}

void assert_schema_arrays_have_items(const json::Value& node, const std::string& path,
                                     std::vector<std::string>& failures) {
    if (node.is_object()) {
        const json::Value* type = node.get("type");
        if (type != nullptr && type->is_string() && type->string == "array") {
            const json::Value* items = node.get("items");
            if (items == nullptr)
                failures.push_back(path + ": array missing items (Gemini/Google require this)");
            else
                assert_schema_arrays_have_items(*items, path + ".items", failures);
        }
        const json::Value* properties = node.get("properties");
        if (properties != nullptr && properties->is_object()) {
            for (const auto& entry : properties->object)
                assert_schema_arrays_have_items(entry.second, path + "." + entry.first, failures);
        }
        for (const char* key : {"items", "additionalProperties"}) {
            const json::Value* child = node.get(key);
            if (child != nullptr && child->is_object())
                assert_schema_arrays_have_items(*child, path + "." + key, failures);
        }
    } else if (node.is_array()) {
        for (std::size_t i = 0; i < node.array.size(); ++i)
            assert_schema_arrays_have_items(node.array[i], path + "[" + std::to_string(i) + "]",
                                            failures);
    }
}

void test_tool_schemas_gemini_compatible() {
    const std::string workspace = write_temp_workspace("schema");
    agent::ReadToolRegistry tools = make_registry(workspace, true);
    std::vector<std::string> failures;
    bool saw_edit = false;
    for (const provider::FunctionDefinition& definition : tools.definitions()) {
        const json::ParseResult parsed = json::parse(definition.parameters_json);
        check(parsed.error.ok() && parsed.value.is_object(),
              "tool schema parses for " + definition.name + ": " + definition.parameters_json);
        assert_schema_arrays_have_items(parsed.value, definition.name, failures);
        if (definition.name == "edit_file") {
            saw_edit = true;
            const json::Value* properties = parsed.value.get("properties");
            check(properties != nullptr && properties->is_object(), "edit_file has properties");
            const json::Value* ops = properties->get("ops");
            check(ops != nullptr && ops->is_object(), "edit_file has ops");
            const json::Value* items = ops->get("items");
            check(items != nullptr && items->is_object(),
                  "edit_file.ops.items present for Gemini");
        }
    }
    check(saw_edit, "mutation registry includes edit_file");
    check(failures.empty(),
          failures.empty() ? "schemas ok"
                           : ("schema array items missing: " + failures.front()));
    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_glob_recursive_root_and_nested() {
    const std::string workspace = write_temp_workspace("glob");
    write_text(fs::path(workspace) / "hello.py", "print(1)\n");
    write_text(fs::path(workspace) / "src" / "hello.py", "print(2)\n");
    fs::create_directories(fs::path(workspace) / "a" / "b");
    write_text(fs::path(workspace) / "a" / "b" / "hello.py", "print(3)\n");
    write_text(fs::path(workspace) / "src" / "other.cpp", "int x;\n");
    agent::ReadToolRegistry tools = make_registry(workspace, false);

    const std::string starstar = tools.execute("glob", R"JSON({"pattern":"**/hello.py"})JSON");
    check(json_ok(starstar), "glob **/hello.py succeeds: " + starstar);
    check(json_array_contains_string(starstar, "hello.py"),
          "**/hello.py matches workspace-root hello.py");
    check(json_array_contains_string(starstar, "src/hello.py"),
          "**/hello.py matches nested src/hello.py");
    check(json_array_contains_string(starstar, "a/b/hello.py"),
          "**/hello.py matches deeply nested hello.py");

    const std::string basename = tools.execute("glob", R"JSON({"pattern":"hello.py"})JSON");
    check(json_ok(basename) && json_array_contains_string(basename, "hello.py") &&
              json_array_contains_string(basename, "src/hello.py"),
          "basename-only hello.py matches root and nested paths");

    const std::string src_cpp =
        tools.execute("glob", R"JSON({"pattern":"src/**/*.cpp"})JSON");
    check(json_ok(src_cpp) && json_array_contains_string(src_cpp, "src/other.cpp"),
          "src/**/*.cpp matches src/other.cpp: " + src_cpp);

    const std::string braces =
        tools.execute("glob", R"JSON({"pattern":"src/*.{cpp,py}"})JSON");
    check(json_ok(braces) && json_array_contains_string(braces, "src/hello.py") &&
              json_array_contains_string(braces, "src/other.cpp"),
          "brace alternatives still work: " + braces);

    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_read_only_registry_hides_writes() {
    const std::string workspace = write_temp_workspace("readonly");
    agent::ReadToolRegistry tools = make_registry(workspace, false);
    bool has_write = false;
    bool has_replace = false;
    bool has_remove = false;
    bool has_edit = false;
    for (const provider::FunctionDefinition& definition : tools.definitions()) {
        if (definition.name == "write_file") has_write = true;
        if (definition.name == "str_replace") has_replace = true;
        if (definition.name == "remove") has_remove = true;
        if (definition.name == "edit_file") has_edit = true;
    }
    check(!has_write && !has_replace && !has_remove && !has_edit,
          "read-only registry omits mutation tools");
    const std::string denied =
        tools.execute("write_file", R"({"path":"src/x.cpp","content":"x\n"})");
    check(!json_ok(denied) && json_error_code(denied) == "policy_denied",
          "write_file denied without mutations");
    write_text(fs::path(workspace) / "after-snapshot.txt", "not review eligible\n");
    const std::string unindexed_read = tools.execute(
        "read_file", R"({"path":"after-snapshot.txt","max_bytes":4096})");
    check(!json_ok(unindexed_read),
          "security-review registry retains index-only read scope");
    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_write_file_create_and_readback() {
    const std::string workspace = write_temp_workspace("write");
    // Headless registry: create_dirs Ask is denied without interactive approval.
    agent::ReadToolRegistry tools = make_registry(workspace, true, false);
    bool has_write = false;
    for (const provider::FunctionDefinition& definition : tools.definitions())
        if (definition.name == "write_file") has_write = true;
    check(has_write, "mutation registry exposes write_file");

    const std::string created = tools.execute(
        "write_file",
        R"({"path":"src/new_note.md","content":"# hello\n","mode":"create_new","create_dirs":false})");
    check(json_ok(created), "write_file create_new succeeds: " + created);
    check(read_text(fs::path(workspace) / "src" / "new_note.md") == "# hello\n",
          "write_file wrote expected content");
    const std::string new_hash = json_data_string(created, "new_file_hash");
    check(!new_hash.empty(), "write_file returns new_file_hash");

    // Same-session read must see the new snapshot hash.
    const std::string read = tools.execute(
        "read_file", R"({"path":"src/new_note.md","start_line":1,"end_line":10,"max_bytes":4096})");
    check(json_ok(read), "read_file after write uses updated snapshot: " + read);

    const std::string exists = tools.execute(
        "write_file",
        R"({"path":"src/new_note.md","content":"nope\n","mode":"create_new"})");
    check(!json_ok(exists), "create_new fails when file exists");

    const std::string stale = tools.execute(
        "write_file",
        R"({"path":"src/new_note.md","content":"stale\n","expected_file_hash":"not-the-hash"})");
    check(!json_ok(stale) && json_error_code(stale) == "stale_file",
          "stale expected_file_hash is rejected");

    const std::string overwrite = tools.execute(
        "write_file",
        ("{\"path\":\"src/new_note.md\",\"content\":\"# next\\n\",\"expected_file_hash\":\"" +
         new_hash + "\"}")
            .c_str());
    check(json_ok(overwrite), "overwrite with matching hash succeeds: " + overwrite);
    check(read_text(fs::path(workspace) / "src" / "new_note.md") == "# next\n",
          "overwrite wrote expected content");
    check(!json_data_string(overwrite, "history_path").empty(),
          "overwrite records history path");
    check(fs::exists(fs::path(workspace) / json_data_string(overwrite, "history_path")),
          "history backup file exists");

    // Headless create_dirs without approval must fail (no silent mkdir -p).
    const std::string nested_denied = tools.execute(
        "write_file",
        R"({"path":"src/deep/nested/file.txt","content":"nested\n","create_dirs":true,"mode":"create_new"})");
    check(!json_ok(nested_denied), "headless create_dirs denied without Ask: " + nested_denied);
    check(nested_denied.find("create directories") != std::string::npos ||
              nested_denied.find("approval") != std::string::npos ||
              nested_denied.find("headless") != std::string::npos,
          "create_dirs deny message: " + nested_denied);
    check(!fs::exists(fs::path(workspace) / "src" / "deep" / "nested" / "file.txt"),
          "denied create_dirs did not write nested file");

    // With interactive-style approval of ask_on_create_dirs only.
    agent::ReadToolRegistry approved = make_registry(workspace, true, true);
    const std::string nested = approved.execute(
        "write_file",
        R"({"path":"src/deep/nested/file.txt","content":"nested\n","create_dirs":true,"mode":"create_new"})");
    check(json_ok(nested), "create_dirs after approval creates parents: " + nested);
    check(read_text(fs::path(workspace) / "src" / "deep" / "nested" / "file.txt") == "nested\n",
          "nested create wrote content");

    const std::string escape =
        tools.execute("write_file", R"({"path":"../outside.txt","content":"x\n"})");
    check(!json_ok(escape), "path escape is denied");
    check(escape.find("outside the project directory") != std::string::npos,
          "escape error is user-facing: " + escape);
    const std::string absolute =
        tools.execute("write_file", R"({"path":"/tmp/evil.txt","content":"x\n"})");
    check(!json_ok(absolute), "absolute path is denied");
    check(absolute.find("outside the project directory") != std::string::npos,
          "absolute path error is user-facing: " + absolute);
    // Critical regression: "~/…" must not create "$workspace/~/…"
    const std::string tilde =
        tools.execute("write_file",
                      R"({"path":"~/code/empty.txt","content":"","create_dirs":true})");
    check(!json_ok(tilde), "tilde home path is denied: " + tilde);
    check(tilde.find("outside the project") != std::string::npos ||
              tilde.find("~") != std::string::npos,
          "tilde path error mentions home/outside: " + tilde);
    check(!fs::exists(fs::path(workspace) / "~" / "code" / "empty.txt"),
          "tilde path must not create literal ~/ under workspace");
    const std::string tilde_mid =
        tools.execute("write_file", R"({"path":"src/~/evil.txt","content":"x"})");
    check(!json_ok(tilde_mid), "tilde as path component is denied");
    const std::string protected_path =
        tools.execute("write_file", R"({"path":".ainiux-pr/evil.txt","content":"x\n"})");
    check(!json_ok(protected_path), "protected metadata path is denied");

    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_str_replace_exact() {
    const std::string workspace = write_temp_workspace("replace");
    write_text(fs::path(workspace) / "src" / "hello.cpp",
               "int main() {\n  return 1;\n  return 1;\n}\n");
    agent::ReadToolRegistry tools = make_registry(workspace, true);

    const std::string ambiguous = tools.execute(
        "str_replace",
        R"({"path":"src/hello.cpp","old_text":"return 1;","new_text":"return 0;"})");
    check(!json_ok(ambiguous) && json_error_code(ambiguous) == "ambiguous_match",
          "ambiguous str_replace without replace_all fails");

    const std::string replaced = tools.execute(
        "str_replace",
        R"({"path":"src/hello.cpp","old_text":"return 1;","new_text":"return 0;","replace_all":true})");
    check(json_ok(replaced), "replace_all succeeds: " + replaced);
    check(read_text(fs::path(workspace) / "src" / "hello.cpp") ==
              "int main() {\n  return 0;\n  return 0;\n}\n",
          "replace_all updated both matches");

    const std::string missing = tools.execute(
        "str_replace",
        R"({"path":"src/hello.cpp","old_text":"does-not-exist","new_text":"x"})");
    check(!json_ok(missing) && json_error_code(missing) == "not_found",
          "missing old_text fails");

    const std::string single = tools.execute(
        "str_replace",
        R"JSON({"path":"src/hello.cpp","old_text":"int main()","new_text":"int entry()"})JSON");
    check(json_ok(single), "single exact replace succeeds: " + single);
    check(read_text(fs::path(workspace) / "src" / "hello.cpp").find("int entry()") !=
              std::string::npos,
          "single replace updated text");

    // Read after replace must not claim the snapshot is stale.
    const std::string read = tools.execute(
        "read_file", R"({"path":"src/hello.cpp","start_line":1,"end_line":20,"max_bytes":4096})");
    check(json_ok(read), "read_file after str_replace uses updated hash: " + read);

    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_edit_file_ops() {
    const std::string workspace = write_temp_workspace("edit");
    write_text(fs::path(workspace) / "src" / "hello.cpp",
               "line1\nline2\nline3\nline4\n");
    agent::ReadToolRegistry tools = make_registry(workspace, true);

    bool has_edit = false;
    bool edit_documents_line = false;
    for (const provider::FunctionDefinition& definition : tools.definitions()) {
        if (definition.name == "edit_file") {
            has_edit = true;
            edit_documents_line =
                definition.description.find("\"line\":2") != std::string::npos;
        }
    }
    check(has_edit, "mutation registry exposes edit_file");
    check(edit_documents_line, "edit_file description shows the canonical insert_at line field");

    const std::string replaced = tools.execute(
        "edit_file",
        R"JSON({"path":"src/hello.cpp","ops":[{"type":"replace_range","start_line":2,"end_line":3,"replacement":"LINE2\nLINE3\n"}]})JSON");
    check(json_ok(replaced), "replace_range succeeds: " + replaced);
    check(read_text(fs::path(workspace) / "src" / "hello.cpp") == "line1\nLINE2\nLINE3\nline4\n",
          "replace_range rewrote middle lines");

    const std::string inserted = tools.execute(
        "edit_file",
        R"JSON({"path":"src/hello.cpp","ops":[{"type":"insert_at","line":2,"new_text":"inserted\n"}]})JSON");
    check(json_ok(inserted), "insert_at succeeds: " + inserted);
    check(read_text(fs::path(workspace) / "src" / "hello.cpp") ==
              "line1\ninserted\nLINE2\nLINE3\nline4\n",
          "insert_at inserted before line 2");

    const std::string inserted_with_start_line = tools.execute(
        "edit_file",
        R"JSON({"path":"src/hello.cpp","ops":[{"op":"insert_at","start_line":3,"new_text":"aliased\n"}]})JSON");
    check(json_ok(inserted_with_start_line),
          "insert_at accepts local-model start_line alias: " + inserted_with_start_line);
    check(read_text(fs::path(workspace) / "src" / "hello.cpp") ==
              "line1\ninserted\naliased\nLINE2\nLINE3\nline4\n",
          "insert_at start_line alias inserts before the requested line");

    const std::string deleted = tools.execute(
        "edit_file",
        R"JSON({"path":"src/hello.cpp","ops":[{"type":"delete_range","start_line":4,"end_line":5}]})JSON");
    check(json_ok(deleted), "delete_range succeeds: " + deleted);
    check(read_text(fs::path(workspace) / "src" / "hello.cpp") ==
              "line1\ninserted\naliased\nline4\n",
          "delete_range removed lines");

    const std::string multi = tools.execute(
        "edit_file",
        R"JSON({"path":"src/hello.cpp","ops":[
          {"type":"replace_range","start_line":1,"end_line":1,"replacement":"A\n"},
          {"type":"replace_range","start_line":4,"end_line":4,"replacement":"C\n"}
        ]})JSON");
    check(json_ok(multi), "multi line ops bottom-to-top succeed: " + multi);
    check(read_text(fs::path(workspace) / "src" / "hello.cpp") == "A\ninserted\naliased\nC\n",
          "multi replace_range applied correctly");

    const std::string text_op = tools.execute(
        "edit_file",
        R"JSON({"path":"src/hello.cpp","ops":[{"type":"replace_text","old_text":"inserted","new_text":"B"}]})JSON");
    check(json_ok(text_op), "replace_text succeeds: " + text_op);
    check(read_text(fs::path(workspace) / "src" / "hello.cpp") == "A\nB\naliased\nC\n",
          "replace_text updated content");

    const std::string created = tools.execute(
        "edit_file",
        R"JSON({"path":"src/brand_new.txt","create_dirs":true,"ops":[{"type":"create_file","new_text":"hello\n"}]})JSON");
    check(json_ok(created), "create_file op succeeds: " + created);
    check(read_text(fs::path(workspace) / "src" / "brand_new.txt") == "hello\n",
          "create_file wrote new file");

    const std::string denied =
        make_registry(workspace, false)
            .execute("edit_file",
                     R"JSON({"path":"src/hello.cpp","ops":[{"type":"delete_range","start_line":1,"end_line":1}]})JSON");
    check(!json_ok(denied), "read-only registry denies edit_file");

    // Weak models sometimes emit "op" instead of "type".
    write_text(fs::path(workspace) / "src" / "alias.cpp", "alpha\nbeta\n");
    // Re-create registry after write outside tools so snapshot sees the file.
    tools = make_registry(workspace, true);
    const std::string alias = tools.execute(
        "edit_file",
        R"JSON({"path":"src/alias.cpp","ops":[{"op":"replace_range","start_line":1,"end_line":1,"replacement":"ALPHA\n"}]})JSON");
    check(json_ok(alias), "edit_file accepts op alias for type: " + alias);
    check(read_text(fs::path(workspace) / "src" / "alias.cpp") == "ALPHA\nbeta\n",
          "op alias replace_range applied");

    // Infer replace_range when type/op is omitted but start/end/new_text are present.
    write_text(fs::path(workspace) / "src" / "infer.cpp", "one\ntwo\n");
    tools = make_registry(workspace, true);
    const std::string inferred = tools.execute(
        "edit_file",
        R"JSON({"path":"src/infer.cpp","ops":[{"start_line":1,"end_line":1,"new_text":"ONE"}]})JSON");
    check(json_ok(inferred), "edit_file infers replace_range without type: " + inferred);
    // Missing trailing newline in new_text must not glue to the next line.
    check(read_text(fs::path(workspace) / "src" / "infer.cpp") == "ONE\ntwo\n",
          "replace_range preserves line terminator when model omits it");

    // DeepSeek-style nested op object + outer "text" field.
    write_text(fs::path(workspace) / "src" / "nested.cpp", "old\nkeep\n");
    tools = make_registry(workspace, true);
    const std::string nested = tools.execute(
        "edit_file",
        R"JSON({"path":"src/nested.cpp","ops":[{"replace_range":{"start_line":1,"end_line":1},"text":"NEW"}]})JSON");
    check(json_ok(nested), "edit_file accepts nested replace_range object: " + nested);
    check(read_text(fs::path(workspace) / "src" / "nested.cpp") == "NEW\nkeep\n",
          "nested replace_range applied with text alias");

    // Nested object with new_text inside (DeepSeek second attempt shape).
    write_text(fs::path(workspace) / "src" / "nested2.cpp", "aaa\nbbb\n");
    tools = make_registry(workspace, true);
    const std::string nested2 = tools.execute(
        "edit_file",
        R"JSON({"path":"src/nested2.cpp","ops":[{"replace_range":{"start_line":1,"end_line":1,"new_text":"AAA"}}]})JSON");
    check(json_ok(nested2), "edit_file accepts nested replace_range with inner new_text: " + nested2);
    check(read_text(fs::path(workspace) / "src" / "nested2.cpp") == "AAA\nbbb\n",
          "nested replace_range with inner new_text applied");

    // Wrong per-op expected_hash is ignored when expected_file_hash matches (DeepSeek).
    write_text(fs::path(workspace) / "src" / "hashy.cpp", "line1\nline2\n");
    tools = make_registry(workspace, true);
    const std::string read_hash = tools.execute(
        "read_file", R"JSON({"path":"src/hashy.cpp","start_line":1,"end_line":2,"max_bytes":4096})JSON");
    check(json_ok(read_hash), "read hashy for file hash");
    const json::ParseResult read_parsed = json::parse(read_hash);
    const std::string file_hash =
        read_parsed.value.get("data")->get("file_hash")->string;
    const std::string wrong_range =
        tools.execute("edit_file",
                      ("{\"path\":\"src/hashy.cpp\",\"expected_file_hash\":\"" + file_hash +
                       "\",\"ops\":[{\"type\":\"replace_range\",\"start_line\":1,\"end_line\":1,"
                       "\"expected_hash\":\"deadbeefcafebabe\",\"new_text\":\"LINE1\"}]}")
                          .c_str());
    check(json_ok(wrong_range),
          "mismatched expected_hash ignored when file hash matches: " + wrong_range);
    check(read_text(fs::path(workspace) / "src" / "hashy.cpp") == "LINE1\nline2\n",
          "edit applied despite wrong range expected_hash");
    const json::ParseResult wrong_parsed = json::parse(wrong_range);
    const json::Value* warns = wrong_parsed.value.get("warnings");
    check(warns != nullptr && warns->is_array() && !warns->array.empty(),
          "mismatch produces a warning");

    // Without a matching file fingerprint, a wrong expected_hash still fails hard.
    write_text(fs::path(workspace) / "src" / "strict.cpp", "s1\ns2\n");
    tools = make_registry(workspace, true);
    const std::string strict = tools.execute(
        "edit_file",
        R"JSON({"path":"src/strict.cpp","ops":[{"type":"replace_range","start_line":1,"end_line":1,"expected_hash":"deadbeefcafebabe","new_text":"S1"}]})JSON");
    check(!json_ok(strict) && json_error_code(strict) == "stale_range",
          "wrong expected_hash without file hash still fails: " + strict);
    check(strict.find("current_range_hash=") != std::string::npos,
          "stale_range error includes current_range_hash");

    // Natural-language agents (Qwen etc.) often nest path inside the op object.
    write_text(fs::path(workspace) / "src" / "nested_path.py", "# first\nprint(1)\n");
    tools = make_registry(workspace, true);
    const std::string nested_path = tools.execute(
        "edit_file",
        R"JSON({"ops":[{"op":"replace_range","path":"src/nested_path.py","start_line":1,"end_line":1,"new_text":"# first\n# second line\n"}]})JSON");
    check(json_ok(nested_path),
          "edit_file promotes path from ops when top-level path is missing: " + nested_path);
    check(read_text(fs::path(workspace) / "src" / "nested_path.py") == "# first\n# second line\nprint(1)\n",
          "nested-path edit inserted second comment line");

    // GPT-5.x / OpenRouter fills every nested op type with empty defaults while also
    // setting top-level type + old_text/new_text (session B edit_file failure shape).
    write_text(fs::path(workspace) / "src" / "gpt5.cpp",
               "#define MAX_BODY 1\n#define OUTPUT_CHUNK 2\n");
    tools = make_registry(workspace, true);
    const std::string gpt5_polluted = tools.execute(
        "edit_file",
        R"JSON({
          "path":"src/gpt5.cpp",
          "ops":[{
            "type":"replace_text",
            "op":"replace_range",
            "old_text":"#define MAX_BODY 1\n#define OUTPUT_CHUNK 2",
            "new_text":"#define MAX_BODY 1\n#define MAX_HTTP1 3\n#define OUTPUT_CHUNK 2",
            "create_file":{"end_line":1,"expected_hash":"","fuzzy":false,"line":1,"new_text":"","old_text":"","replace_all":false,"replacement":"","start_line":1,"symbol_id":1,"text":""},
            "delete_range":{"end_line":1,"expected_hash":"","fuzzy":false,"line":1,"new_text":"","old_text":"","replace_all":false,"replacement":"","start_line":1,"symbol_id":1,"text":""},
            "insert_at":{"end_line":1,"expected_hash":"","fuzzy":false,"line":1,"new_text":"","old_text":"","replace_all":false,"replacement":"","start_line":1,"symbol_id":1,"text":""},
            "replace_range":{"end_line":1,"expected_hash":"","fuzzy":false,"line":1,"new_text":"","old_text":"","replace_all":false,"replacement":"","start_line":1,"symbol_id":1,"text":""},
            "replace_text":{"end_line":1,"expected_hash":"","fuzzy":false,"line":1,"new_text":"","old_text":"","replace_all":false,"replacement":"","start_line":1,"symbol_id":1,"text":""},
            "replace_symbol":{"end_line":1,"expected_hash":"","fuzzy":false,"line":1,"new_text":"","old_text":"","replace_all":false,"replacement":"","start_line":1,"symbol_id":1,"text":""},
            "start_line":1,"end_line":2,"line":1,"symbol_id":1,"replacement":"","text":""
          }]
        })JSON");
    check(json_ok(gpt5_polluted),
          "edit_file tolerates GPT-5 nested shell pollution: " + gpt5_polluted);
    check(read_text(fs::path(workspace) / "src" / "gpt5.cpp").find("MAX_HTTP1") !=
              std::string::npos,
          "polluted replace_text applied: " +
              read_text(fs::path(workspace) / "src" / "gpt5.cpp"));

    // Schema must not advertise nested op objects (they trigger GPT-5 filling).
    bool schema_is_flat = false;
    for (const provider::FunctionDefinition& definition : tools.definitions()) {
        if (definition.name == "edit_file") {
            schema_is_flat =
                definition.parameters_json.find("\"replace_range\":{\"type\":\"object\"") ==
                    std::string::npos &&
                definition.parameters_json.find("\"create_file\":{\"type\":\"object\"") ==
                    std::string::npos;
        }
    }
    check(schema_is_flat, "edit_file schema stays flat (no nested op object properties)");

    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_str_replace_fuzzy_whitespace_and_indent() {
    const std::string workspace = write_temp_workspace("fuzzy");
    write_text(fs::path(workspace) / "src" / "ws.cpp",
               "int add(  int a,\n  int b ) {\n  return a + b;\n}\n");
    agent::ReadToolRegistry tools = make_registry(workspace, true);

    // Needle has different internal whitespace than the file.
    const std::string ws = tools.execute(
        "str_replace",
        R"JSON({"path":"src/ws.cpp","old_text":"int add(int a, int b)","new_text":"int add(int x, int y)"})JSON");
    check(json_ok(ws), "normalized whitespace str_replace succeeds: " + ws);
    check(json_data_string(ws, "match_mode") == "normalized_whitespace",
          "match_mode reports normalized_whitespace: " + ws);
    check(read_text(fs::path(workspace) / "src" / "ws.cpp").find("int add(int x, int y)") !=
              std::string::npos,
          "whitespace-fuzzy replace updated signature");

    // Multi-line block with wrong leading indent.
    write_text(fs::path(workspace) / "src" / "indent.cpp",
               "void f() {\n    if (true) {\n        do_work();\n    }\n}\n");
    tools = make_registry(workspace, true);
    const std::string indent = tools.execute(
        "str_replace",
        R"JSON({"path":"src/indent.cpp","old_text":"if (true) {\n    do_work();\n}","new_text":"if (ready) {\n    do_work();\n}"})JSON");
    check(json_ok(indent), "indent-stripped str_replace succeeds: " + indent);
    // May resolve via normalized_whitespace or indent_stripped depending on snippet shape.
    check(json_data_string(indent, "match_mode") == "indent_stripped" ||
              json_data_string(indent, "match_mode") == "normalized_whitespace",
          "match_mode reports a fuzzy mode: " + indent);
    check(read_text(fs::path(workspace) / "src" / "indent.cpp").find("if (ready)") !=
              std::string::npos,
          "indent-fuzzy replace updated condition");

    // fuzzy=false must not fall back.
    write_text(fs::path(workspace) / "src" / "exact_only.cpp", "foo(  1,  2 )\n");
    tools = make_registry(workspace, true);
    const std::string no_fuzzy = tools.execute(
        "str_replace",
        R"JSON({"path":"src/exact_only.cpp","old_text":"foo(1, 2)","new_text":"foo(3, 4)","fuzzy":false})JSON");
    check(!json_ok(no_fuzzy) && json_error_code(no_fuzzy) == "not_found",
          "fuzzy=false rejects whitespace-only mismatch: " + no_fuzzy);

    // edit_file.replace_text also uses fuzzy fallback.
    write_text(fs::path(workspace) / "src" / "edit_ws.cpp", "print(  'hi'  )\n");
    tools = make_registry(workspace, true);
    const std::string edit_ws = tools.execute(
        "edit_file",
        R"JSON({"path":"src/edit_ws.cpp","ops":[{"type":"replace_text","old_text":"print('hi')","new_text":"print('ok')"}]})JSON");
    check(json_ok(edit_ws), "edit_file replace_text fuzzy succeeds: " + edit_ws);
    check(read_text(fs::path(workspace) / "src" / "edit_ws.cpp") == "print('ok')\n",
          "edit_file fuzzy replace_text content");

    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_remove_tool() {
    const std::string workspace = write_temp_workspace("remove");
    write_text(fs::path(workspace) / "src" / "gone.cpp", "int x;\n");
    fs::create_directories(fs::path(workspace) / "src" / "empty_dir");
    fs::create_directories(fs::path(workspace) / "src" / "full_dir");
    write_text(fs::path(workspace) / "src" / "full_dir" / "keep.cpp", "int y;\n");
    write_text(fs::path(workspace) / "data.sqlite", "not-a-real-db");
    agent::ReadToolRegistry tools = make_registry(workspace, true);

    bool has_remove = false;
    for (const provider::FunctionDefinition& definition : tools.definitions())
        if (definition.name == "remove") has_remove = true;
    check(has_remove, "mutation registry exposes remove");

    const std::string removed =
        tools.execute("remove", R"JSON({"path":"src/gone.cpp"})JSON");
    check(json_ok(removed), "remove file succeeds: " + removed);
    check(!fs::exists(fs::path(workspace) / "src" / "gone.cpp"), "file was removed");
    check(!json_data_string(removed, "history_path").empty(), "remove records history for text");

    const std::string empty =
        tools.execute("remove", R"JSON({"path":"src/empty_dir"})JSON");
    check(json_ok(empty), "remove empty directory succeeds: " + empty);

    const std::string nonempty =
        tools.execute("remove", R"JSON({"path":"src/full_dir"})JSON");
    check(!json_ok(nonempty), "non-empty directory without recursive fails: " + nonempty);

    const std::string recursive =
        tools.execute("remove", R"JSON({"path":"src/full_dir","recursive":true})JSON");
    check(json_ok(recursive), "recursive remove succeeds: " + recursive);
    check(!fs::exists(fs::path(workspace) / "src" / "full_dir"), "directory tree removed");

    const std::string db =
        tools.execute("remove", R"JSON({"path":"data.sqlite"})JSON");
    check(!json_ok(db) && json_error_code(db) == "policy_denied",
          "database file remove denied in headless mode: " + db);
    check(fs::exists(fs::path(workspace) / "data.sqlite"), "database file still present");

    // Missing path suggests #wrapped# siblings.
    write_text(fs::path(workspace) / "plain.txt", "a\n");
    write_text(fs::path(workspace) / "#plain.txt#", "backup\n");
    tools = make_registry(workspace, true);
    const std::string missing =
        tools.execute("remove", R"JSON({"path":"nope.txt"})JSON");
    check(!json_ok(missing) && json_error_code(missing) == "not_found",
          "missing path fails: " + missing);

    // Plain name blocked when #plain.txt# also exists unless confirm=true.
    const std::string ambiguous =
        tools.execute("remove", R"JSON({"path":"plain.txt"})JSON");
    check(!json_ok(ambiguous) && json_error_code(ambiguous) == "ambiguous_match",
          "plain remove blocked when #sibling# exists: " + ambiguous);
    check(fs::exists(fs::path(workspace) / "plain.txt"), "plain file kept without confirm");

    const std::string hash_name =
        tools.execute("remove", R"JSON({"path":"#plain.txt#"})JSON");
    check(json_ok(hash_name), "exact #wrapped# remove succeeds: " + hash_name);
    check(!fs::exists(fs::path(workspace) / "#plain.txt#"), "hash-wrapped file removed");
    check(fs::exists(fs::path(workspace) / "plain.txt"), "plain sibling still present");

    // Recreate wrapper; confirm=true allows deleting the plain name intentionally.
    write_text(fs::path(workspace) / "#plain.txt#", "backup2\n");
    tools = make_registry(workspace, true);
    const std::string confirmed =
        tools.execute("remove", R"JSON({"path":"plain.txt","confirm":true})JSON");
    check(json_ok(confirmed), "confirm=true removes plain when sibling exists: " + confirmed);
    check(!fs::exists(fs::path(workspace) / "plain.txt"), "plain removed with confirm");

    const std::string denied =
        make_registry(workspace, false).execute("remove", R"JSON({"path":"src/hello.cpp"})JSON");
    check(!json_ok(denied) && json_error_code(denied) == "policy_denied",
          "read-only registry denies remove");

    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_list_directory_filesystem() {
    const std::string workspace = write_temp_workspace("listdir");
    // Empty nested dirs (parent is non-empty only because of the child dir).
    fs::create_directories(fs::path(workspace) / "you_can_remove_me" / "del_me");
    // Non-source / unusual names are invisible to the code index.
    write_text(fs::path(workspace) / "#hello_world.py#", "backup content\n");
    write_text(fs::path(workspace) / "notes.txt", "not indexed as code if unknown? may be skipped\n");
    agent::ReadToolRegistry tools = make_registry(workspace, false);

    const std::string root = tools.execute("list_directory", R"JSON({"path":"."})JSON");
    check(json_ok(root), "list_directory root succeeds: " + root);
    check(json_array_contains_string_field(root, "name", "you_can_remove_me") ||
              root.find("you_can_remove_me") != std::string::npos,
          "lists non-indexed empty-parent directory: " + root);
    check(root.find("#hello_world.py#") != std::string::npos,
          "lists #wrapped# non-indexed file: " + root);
    check(root.find("hello.cpp") != std::string::npos || root.find("\"src\"") != std::string::npos,
          "still lists indexed tree: " + root);

    // Parent dir is not empty (has del_me); del_me itself is empty.
    const std::string nested =
        tools.execute("list_directory", R"JSON({"path":"you_can_remove_me"})JSON");
    check(json_ok(nested) && nested.find("del_me") != std::string::npos,
          "lists nested directory entry: " + nested);
    check(nested.find("\"empty\":true") != std::string::npos ||
              nested.find("\"empty\": true") != std::string::npos,
          "marks empty child directory: " + nested);

    const std::string empty_leaf =
        tools.execute("list_directory", R"JSON({"path":"you_can_remove_me/del_me"})JSON");
    check(json_ok(empty_leaf), "list empty directory succeeds: " + empty_leaf);

    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_smart_act_native_tools_accept_unindexed_project_paths() {
    const std::string workspace = write_temp_workspace("smart-unindexed");
    std::atomic<int> asks{0};
    agent::ReadToolRegistry tools = make_registry(
        workspace, agent::MutationPolicy::Full, false,
        [&](const agent::GuardApprovalRequest&,
            runtime::CancellationToken) -> agent::GuardApprovalDecision {
            ++asks;
            return agent::GuardApprovalDecision::Allow;
        },
        agent::PermissionMode::Smart, true);

    // Create these after the snapshot so they are certainly absent from it.
    write_text(fs::path(workspace) / "names.txt", "Ada\nLinus\n");
    write_text(fs::path(workspace) / "languages.txt", "C++\nPython\n");
    write_text(fs::path(workspace) / "remove-me.txt", "temporary\n");
    fs::create_directory(fs::path(workspace) / "unindexed-dir");

    const std::string read = tools.execute(
        "read_file",
        R"JSON({"path":"names.txt","start_line":1,"end_line":20,"max_bytes":4096})JSON");
    check(json_ok(read) && read.find("Ada") != std::string::npos,
          "Smart Act read_file reads an unindexed project file: " + read);
    const std::string many = tools.execute(
        "read_many",
        R"JSON({"items":[{"path":"names.txt","start_line":1,"end_line":20},{"path":"languages.txt","start_line":1,"end_line":20}],"max_bytes":4096})JSON");
    const json::ParseResult many_parsed = json::parse(many);
    const json::Value* many_data =
        many_parsed.error.ok() ? many_parsed.value.get("data") : nullptr;
    const json::Value* many_metadata =
        many_parsed.error.ok() ? many_parsed.value.get("metadata") : nullptr;
    check(json_ok(many) && many.find("Linus") != std::string::npos &&
              many.find("Python") != std::string::npos &&
              many_data != nullptr && many_data->is_array() &&
              many_data->array.size() == 2 &&
              many_metadata != nullptr && many_metadata->is_object() &&
              many_metadata->get("requested_items") != nullptr &&
              many_metadata->get("requested_items")->number == 2 &&
              many_metadata->get("returned_items") != nullptr &&
              many_metadata->get("returned_items")->number == 2,
          "Smart Act read_many returns two unindexed project files in one tool call: " +
              many);

    const std::string edited = tools.execute(
        "edit_file",
        R"JSON({"path":"names.txt","ops":[{"type":"replace_text","old_text":"Linus","new_text":"Grace"}]})JSON");
    check(json_ok(edited) &&
              read_text(fs::path(workspace) / "names.txt") == "Ada\nGrace\n",
          "Smart Act edit_file modifies an unindexed project file: " + edited);
    const std::string created = tools.execute(
        "write_file",
        R"JSON({"path":"unindexed-dir/created.txt","content":"created\n","mode":"create_new"})JSON");
    check(json_ok(created), "Smart Act write_file creates under an unindexed directory: " + created);
    const std::string renamed = tools.execute(
        "rename_path",
        R"JSON({"source":"unindexed-dir","destination":"renamed-dir"})JSON");
    check(json_ok(renamed) &&
              fs::exists(fs::path(workspace) / "renamed-dir" / "created.txt"),
          "Smart Act rename_path renames an unindexed project directory: " + renamed);
    const std::string removed = tools.execute(
        "remove", R"JSON({"path":"remove-me.txt"})JSON");
    check(json_ok(removed) && !fs::exists(fs::path(workspace) / "remove-me.txt"),
          "Smart Act remove deletes an unindexed project file: " + removed);
    check(asks.load() == 0,
          "Smart Act native project operations need no ordinary write approval");

    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_agent_command_output_keeps_unindexed_project_paths() {
    const std::string workspace = write_temp_workspace("command-unindexed");
    std::atomic<int> confirm_asks{0};
    agent::ReadToolRegistry confirm = make_registry(
        workspace, agent::MutationPolicy::Full, false,
        [&](const agent::GuardApprovalRequest&,
            runtime::CancellationToken) -> agent::GuardApprovalDecision {
            ++confirm_asks;
            return agent::GuardApprovalDecision::Allow;
        },
        agent::PermissionMode::Confirm, true);
    agent::ReadToolRegistry smart = make_registry(
        workspace, agent::MutationPolicy::Full, false, {},
        agent::PermissionMode::Smart, true);
    agent::ReadToolRegistry yolo = make_registry(
        workspace, agent::MutationPolicy::Full, false, {},
        agent::PermissionMode::Yolo, true);

    // Create after every registry snapshot so this path is guaranteed to be
    // absent from all three indexes.
    const std::string late_name = "late-unindexed-output.txt";
    write_text(fs::path(workspace) / late_name, "visible\n");
    const std::string command = R"JSON({"command":"ls -laFg"})JSON";
    const std::string confirmed = confirm.execute("run_command", command);
    const std::string smart_result = smart.execute("run_command", command);
    const std::string yolo_result = yolo.execute("run_command", command);
    check(json_ok(confirmed) && confirmed.find(late_name) != std::string::npos &&
              confirm_asks.load() == 1,
          "Confirm preserves unindexed long-format ls output after approval: " +
              confirmed);
    check(json_ok(smart_result) &&
              smart_result.find(late_name) != std::string::npos,
          "Smart preserves unindexed long-format ls output: " + smart_result);
    check(json_ok(yolo_result) &&
              yolo_result.find(late_name) != std::string::npos,
          "Yolo preserves unindexed long-format ls output: " + yolo_result);
    check(confirmed.find("output referring to non-indexed paths was omitted") ==
                  std::string::npos &&
              smart_result.find("output referring to non-indexed paths was omitted") ==
                  std::string::npos &&
              yolo_result.find("output referring to non-indexed paths was omitted") ==
                  std::string::npos,
          "Agent permission modes do not emit the security-review index filter warning");

    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_replace_symbol() {
    const std::string workspace = write_temp_workspace("symbol");
    write_text(fs::path(workspace) / "src" / "sym.cpp",
               "int alpha() {\n  return 1;\n}\n\nint beta() {\n  return 2;\n}\n");
    agent::ReadToolRegistry tools = make_registry(workspace, true);

    const std::string search =
        tools.execute("search_symbol", R"JSON({"query":"alpha","max_results":10})JSON");
    check(json_ok(search), "search_symbol finds alpha: " + search);
    const json::ParseResult parsed = json::parse(search);
    check(parsed.error.ok() && parsed.value.get("data") != nullptr &&
              parsed.value.get("data")->is_array() && !parsed.value.get("data")->array.empty(),
          "search_symbol returns hits");
    const json::Value& hit = parsed.value.get("data")->array.front();
    const json::Value* id_value = hit.get("id");
    if (id_value == nullptr) id_value = hit.get("symbol_id");
    check(id_value != nullptr && id_value->type == json::Value::Type::Number,
          "hit has id: " + search);
    const long long symbol_id = static_cast<long long>(id_value->number);

    const std::string replaced = tools.execute(
        "edit_file",
        ("{\"path\":\"src/sym.cpp\",\"ops\":[{\"type\":\"replace_symbol\",\"symbol_id\":" +
         std::to_string(symbol_id) +
         ",\"replacement\":\"int alpha() {\\n  return 42;\\n}\\n\"}]}")
            .c_str());
    check(json_ok(replaced), "replace_symbol succeeds: " + replaced);
    const std::string content = read_text(fs::path(workspace) / "src" / "sym.cpp");
    check(content.find("return 42;") != std::string::npos, "symbol body updated");
    check(content.find("int beta()") != std::string::npos, "other symbol preserved");
    check(tools.refresh_persistent_index(false).ok(),
          "native mutation flushes its coalesced touched-file index update");
    ainiux::agent::index::Options persisted_options;
    persisted_options.workspace = workspace;
    ainiux::agent::index::Snapshot persisted;
    check(ainiux::agent::index::load_snapshot(persisted_options, persisted).ok(),
          "mutation-aware persistent snapshot reloads");
    bool alpha_persisted = false;
    for (const auto& symbol : persisted.symbols) {
        if (symbol.path == "src/sym.cpp" && symbol.symbol.name == "alpha" &&
            symbol.symbol.line_end >= 3)
            alpha_persisted = true;
    }
    check(alpha_persisted,
          "touched-file persistence keeps the edited symbol definition");

    const std::string missing = tools.execute(
        "edit_file",
        R"JSON({"path":"src/sym.cpp","ops":[{"type":"replace_symbol","symbol_id":999999,"replacement":"x"}]})JSON");
    check(!json_ok(missing), "unknown symbol_id fails: " + missing);

    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_index_refresh_drops_completed_prepare_cancellation() {
    const std::string workspace = write_temp_workspace("stale-index-cancel");
    runtime::CancellationSource prepare_source;
    const runtime::CancellationToken prepare_token = prepare_source.token();
    agent::index::Options options;
    options.workspace = workspace;
    options.max_source_code_file_size = 1024 * 1024;
    options.cancellation = prepare_token;
    options.interrupted = [prepare_token] { return prepare_token.cancelled(); };
    agent::index::RefreshStats stats;
    check(agent::index::refresh(options, stats).ok(),
          "prepare-time index refresh succeeds before cancellation");
    agent::index::Snapshot snapshot;
    check(agent::index::load_snapshot(options, snapshot).ok(),
          "prepare-time index snapshot loads before cancellation");

    agent::ToolRegistryOptions tool_options;
    tool_options.mutation_policy = agent::MutationPolicy::Full;
    agent::ReadToolRegistry tools;
    check(agent::ReadToolRegistry::create(
              std::move(options), std::move(snapshot), {}, tools, tool_options)
              .ok(),
          "create long-lived registry from prepare-time index options");

    // Mirrors the TUI runtime job finishing after session preparation.
    prepare_source.cancel();
    const Error refresh_error = tools.refresh_persistent_index(true);
    check(refresh_error.ok(),
          "later index generation ignores the completed prepare-job token: " +
              refresh_error.message);
    const std::string search = tools.execute(
        "search_text",
        R"JSON({"query":"main","glob":"src/*.cpp","max_results":10})JSON");
    check(json_ok(search),
          "snapshot search remains usable after prepare-job cancellation: " +
              search);

    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_indexing_disabled_registry_is_strict_and_live() {
    const std::string workspace = write_temp_workspace("indexing-disabled");
    write_text(fs::path(workspace) / ".gitignore", "ignored.py\n");
    write_text(fs::path(workspace) / "ignored.py",
               "def ignored_marker():\n    pass\n");
    write_text(fs::path(workspace) / "src" / "live.py",
               "def LiveMarker():\n    return 'Needle'\n");
    write_text(fs::path(workspace) / "src" / "other.py",
               "def OtherMarker():\n    return 'Needle'\n");
    agent::ToolRegistryOptions options;
    options.mutation_policy = agent::MutationPolicy::Full;
    options.indexing_enabled = false;
    agent::index::Options index_options;
    index_options.workspace = fs::canonical(workspace).string();
    index_options.max_source_code_file_size = 1024 * 1024;
    agent::ReadToolRegistry tools;
    Error error = agent::ReadToolRegistry::create_without_index(
        index_options, {}, tools, options);
    check(error.ok(), "create live registry without an index: " + error.message);

    bool has_list = false;
    bool has_read = false;
    bool has_edit = false;
    bool has_glob = false;
    bool has_search = false;
    bool has_grep = false;
    bool has_find = false;
    bool has_index_tool = false;
    bool advertises_replace_symbol = false;
    for (const provider::FunctionDefinition& definition :
         tools.definitions()) {
        has_list = has_list || definition.name == "list_directory";
        has_read = has_read || definition.name == "read_file";
        has_edit = has_edit || definition.name == "edit_file";
        has_glob = has_glob || definition.name == "glob";
        has_search = has_search || definition.name == "search_text";
        has_grep = has_grep || definition.name == "grep";
        has_find = has_find || definition.name == "find";
        has_index_tool =
            has_index_tool || definition.name == "project_overview" ||
            definition.name == "search_symbol" ||
            definition.name == "index_status" ||
            definition.name == "inspect_code_task";
        if (definition.name == "edit_file")
            advertises_replace_symbol =
                definition.description.find("replace_symbol") !=
                    std::string::npos ||
                definition.parameters_json.find("replace_symbol") !=
                    std::string::npos;
    }
    check(has_list && has_read && has_edit && has_glob && has_search &&
              has_grep && has_find && !has_index_tool &&
              !advertises_replace_symbol,
          "indexing-off definitions retain live search/edit tools and hide index tools");
    check(json_ok(tools.execute(
              "list_directory", R"JSON({"path":"src"})JSON")) &&
              json_ok(tools.execute(
                  "read_file", R"JSON({"path":"src/hello.cpp"})JSON")),
          "indexing-off registry retains live directory and exact-path reads");
    const std::string live_glob =
        tools.execute("glob", R"JSON({"pattern":"**/*.{cpp,py}"})JSON");
    check(json_ok(live_glob) &&
              json_array_contains_string(live_glob, "src/hello.cpp") &&
              json_array_contains_string(live_glob, "src/live.py") &&
              !json_array_contains_string(live_glob, "ignored.py"),
          "indexing-off glob discovers eligible live files and honors ignores: " +
              live_glob);
    const std::string live_search = tools.execute(
        "search_text",
        R"JSON({"query":"needle","case_sensitive":false,"glob":"src/*.py","context":1})JSON");
    check(json_ok(live_search) &&
              live_search.find("src/live.py") != std::string::npos &&
              live_search.find("Needle") != std::string::npos,
          "indexing-off literal search reads live UTF-8 sources: " +
              live_search);
    const std::string regex_search = tools.execute(
        "grep",
        R"JSON({"query":"Live(Marker|Missing)","regex":true,"glob":"src/*.py"})JSON");
    check(json_ok(regex_search) &&
              regex_search.find("LiveMarker") != std::string::npos,
          "indexing-off grep alias retains regex behavior: " +
              regex_search);
    const std::string inferred_regex_search = tools.execute(
        "search_text",
        R"JSON({"query":"LiveMarker|MissingMarker","path":"src/live.py"})JSON");
    check(json_ok(inferred_regex_search) &&
              inferred_regex_search.find("LiveMarker") != std::string::npos &&
              inferred_regex_search.find("regex=true inferred") !=
                  std::string::npos,
          "search_text infers alternation only when regex is omitted and honors exact path: " +
              inferred_regex_search);
    const std::string explicit_literal_search = tools.execute(
        "search_text",
        R"JSON({"query":"LiveMarker|MissingMarker","regex":false,"path":"src/live.py"})JSON");
    check(json_ok(explicit_literal_search) &&
              explicit_literal_search.find("\"data\":[]") != std::string::npos &&
              explicit_literal_search.find("searched literally") !=
                  std::string::npos,
          "explicit regex=false preserves literal pipe behavior: " +
              explicit_literal_search);
    const std::string pattern_alias_search = tools.execute(
        "grep",
        R"JSON({"pattern":"Needle","path":"src/live.py","max_results":5})JSON");
    check(json_ok(pattern_alias_search) &&
              pattern_alias_search.find("Needle") != std::string::npos &&
              pattern_alias_search.find("src/other.py") == std::string::npos,
          "grep accepts pattern as an unambiguous query alias: " +
              pattern_alias_search);
    const std::string logical_or_search = tools.execute(
        "search_text",
        R"JSON({"query":"||","path":"src/live.py"})JSON");
    check(json_ok(logical_or_search) &&
              logical_or_search.find("regex=true inferred") ==
                  std::string::npos,
          "search_text does not infer alternation for a logical-or token: " +
              logical_or_search);
    check(!json_ok(tools.execute(
              "search_text",
              R"JSON({"query":"Needle","path":"src/live.py","glob":"*.py"})JSON")),
          "search_text rejects ambiguous exact-path and glob filters");
    const std::string repaired_glob_search = tools.execute(
        "search_text",
        R"JSON({"query":"Needle","glob": *.py,"max_results":5})JSON");
    check(json_ok(repaired_glob_search) &&
              repaired_glob_search.find("Needle") != std::string::npos,
          "search_text repairs an unquoted path-like glob value: " +
              repaired_glob_search);
    check(!json_ok(tools.execute(
              "search_text", R"JSON({"query":"(","regex":true})JSON")),
          "indexing-off search returns structured invalid-regex errors");
    check(json_ok(tools.execute(
              "edit_file",
              R"JSON({"path":"src/hello.cpp","ops":[{"type":"replace_text","old_text":"return 0","new_text":"return 7"}]})JSON")),
          "indexing-off registry retains ordinary edits");
    check(json_ok(tools.execute("search_text",
                                R"JSON({"query":"return 7","glob":"src/hello.cpp"})JSON")) &&
              !json_ok(tools.execute("project_overview", "{}")) &&
              !json_ok(tools.execute(
                  "edit_file",
                  R"JSON({"path":"src/hello.cpp","ops":[{"type":"replace_symbol","symbol_id":1,"replacement":"x"}]})JSON")),
          "indexing-off search sees writes while index tools and replace_symbol stay denied");
    check(tools.refresh_persistent_index(true).ok() &&
              !fs::exists(fs::path(workspace) / ".ainiux-pr" /
                          "index.sqlite"),
          "indexing-off registry does not create a refresh database");

    agent::index::RefreshStats enable_stats;
    agent::index::Snapshot enabled_snapshot;
    check(agent::index::refresh(index_options, enable_stats).ok() &&
              agent::index::load_snapshot(index_options, enabled_snapshot).ok() &&
              tools.enable_persistent_index(index_options,
                                            std::move(enabled_snapshot)).ok(),
          "live registry can enable a completed persistent index in place");
    bool has_index_status = false;
    bool has_replace_symbol = false;
    for (const provider::FunctionDefinition& definition : tools.definitions()) {
        has_index_status =
            has_index_status || definition.name == "index_status";
        if (definition.name != "edit_file") continue;
        has_replace_symbol =
            definition.parameters_json.find("replace_symbol") !=
            std::string::npos;
    }
    check(tools.indexing_enabled() && has_index_status &&
              has_replace_symbol,
          "in-place index enablement immediately publishes index-aware tool schemas");

    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_read_many_preference_limits_and_serialization() {
    const std::string workspace = write_temp_workspace("read-many");
    write_text(fs::path(workspace) / "src" / "large.txt",
               std::string(70000, 'x') + "\n");
    write_text(fs::path(workspace) / "src" / "lines.txt",
               "alpha\nbeta\ngamma\n");
    agent::ReadToolRegistry tools =
        make_registry(workspace, agent::MutationPolicy::Full);

    const std::vector<provider::FunctionDefinition> definitions =
        tools.definitions();
    std::size_t read_many_index = definitions.size();
    std::size_t read_file_index = definitions.size();
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        if (definitions[index].name == "read_many") read_many_index = index;
        if (definitions[index].name == "read_file") read_file_index = index;
    }
    check(read_many_index < read_file_index &&
              definitions[read_many_index].description.find(
                  "Preferred file reader") != std::string::npos &&
              definitions[read_many_index].description.find(
                  "native parallel tool calls") != std::string::npos &&
              definitions[read_file_index].description.find(
                  "Single-target fallback") != std::string::npos &&
              definitions[read_file_index].description.find(
                  "Do not issue multiple parallel read_file") !=
                  std::string::npos,
          "indexed Agent definitions advertise read_many before the read_file fallback");

    const std::string limited = tools.execute(
        "read_many",
        R"JSON({"items":[{"path":"src/lines.txt","start_line":2,"end_line":3,"max_bytes":5},{"path":"src/large.txt"}]})JSON");
    const json::ParseResult limited_json = json::parse(limited);
    const json::Value* limited_data =
        limited_json.error.ok() ? limited_json.value.get("data") : nullptr;
    const json::Value* first =
        limited_data != nullptr && limited_data->is_array()
            ? limited_data->at(0)
            : nullptr;
    const json::Value* second =
        limited_data != nullptr && limited_data->is_array()
            ? limited_data->at(1)
            : nullptr;
    check(json_ok(limited) && limited_data != nullptr &&
              limited_data->is_array() && limited_data->array.size() == 2 &&
              first != nullptr && first->get("content") != nullptr &&
              first->get("content")->is_string() &&
              first->get("content")->string.rfind("2: ", 0) == 0 &&
              first->get("bytes") != nullptr &&
              first->get("bytes")->number <= 5 &&
              first->get("file_hash") != nullptr &&
              first->get("range_hash") != nullptr &&
              first->get("line_start") != nullptr &&
              first->get("line_end") != nullptr &&
              second != nullptr && second->get("bytes") != nullptr &&
              second->get("bytes")->number == 65536 &&
              second->get("truncated") != nullptr &&
              second->get("truncated")->boolean,
          "read_many honors item max_bytes, defaults each item to 64 KiB, and returns numbered hash/range metadata: " +
              limited);

    const std::string aggregate = tools.execute(
        "read_many",
        R"JSON({"items":[{"path":"src/lines.txt","max_bytes":10},{"path":"src/large.txt","max_bytes":10}],"max_bytes":12})JSON");
    const json::ParseResult aggregate_json = json::parse(aggregate);
    const json::Value* aggregate_data =
        aggregate_json.error.ok() ? aggregate_json.value.get("data") : nullptr;
    double aggregate_bytes = 0;
    if (aggregate_data != nullptr && aggregate_data->is_array()) {
        for (const json::Value& item : aggregate_data->array) {
            const json::Value* bytes = item.get("bytes");
            if (bytes != nullptr &&
                bytes->type == json::Value::Type::Number)
                aggregate_bytes += bytes->number;
        }
    }
    check(json_ok(aggregate) && aggregate_bytes <= 12 &&
              aggregate.find("\"truncated\":true") != std::string::npos,
          "read_many retains the aggregate cap in addition to item limits: " +
              aggregate);

    const std::string partial = tools.execute(
        "read_many",
        R"JSON({"items":[{"path":"src/lines.txt"},{"path":"src/missing.txt"}]})JSON");
    const json::ParseResult partial_json = json::parse(partial);
    const json::Value* partial_data =
        partial_json.error.ok() ? partial_json.value.get("data") : nullptr;
    check(json_ok(partial) && partial_data != nullptr &&
              partial_data->is_array() && partial_data->array.size() == 1 &&
              partial.find("omitted src/missing.txt") != std::string::npos &&
              partial.find("\"truncated\":true") != std::string::npos,
          "read_many preserves successful items and warnings after a partial failure: " +
              partial);
    check(!json_ok(tools.execute("read_many", R"JSON({"items":[]})JSON")),
          "read_many rejects an empty batch");
    std::string oversized_batch = R"JSON({"items":[)JSON";
    for (std::size_t index = 0; index < 101; ++index) {
        if (index != 0) oversized_batch += ',';
        oversized_batch += R"JSON({"path":"src/lines.txt"})JSON";
    }
    oversized_batch += "]}";
    check(!json_ok(tools.execute("read_many", oversized_batch)),
          "read_many rejects more than 100 items");

    provider::RequestContext context;
    context.options.model = "test-model";
    provider::ToolConversation conversation;
    conversation.messages.push_back({"user", "inspect"});
    for (provider::ApiKind api : {provider::ApiKind::ChatCompletions,
                                  provider::ApiKind::Responses}) {
        context.api_kind = api;
        const json::ParseResult request = json::parse(
            provider::serialize_tool_request(context, conversation, definitions));
        const json::Value* serialized_tools =
            request.error.ok() ? request.value.get("tools") : nullptr;
        const json::Value* serialized_many =
            serialized_tools != nullptr ? serialized_tools->at(read_many_index)
                                        : nullptr;
        const json::Value* serialized_file =
            serialized_tools != nullptr ? serialized_tools->at(read_file_index)
                                        : nullptr;
        const json::Value* many_name =
            serialized_many == nullptr
                ? nullptr
                : (api == provider::ApiKind::Responses
                       ? serialized_many->get("name")
                       : (serialized_many->get("function") == nullptr
                              ? nullptr
                              : serialized_many->get("function")->get("name")));
        const json::Value* file_name =
            serialized_file == nullptr
                ? nullptr
                : (api == provider::ApiKind::Responses
                       ? serialized_file->get("name")
                       : (serialized_file->get("function") == nullptr
                              ? nullptr
                              : serialized_file->get("function")->get("name")));
        check(request.error.ok() && serialized_tools != nullptr &&
                  serialized_tools->is_array() &&
                  many_name != nullptr && many_name->is_string() &&
                  many_name->string == "read_many" &&
                  file_name != nullptr && file_name->is_string() &&
                  file_name->string == "read_file",
              std::string(api == provider::ApiKind::Responses ? "Responses"
                                                              : "Chat Completions") +
                  " serializes read_many before read_file");
    }

    agent::ToolRegistryOptions live_options;
    live_options.mutation_policy = agent::MutationPolicy::Full;
    live_options.indexing_enabled = false;
    agent::index::Options index_options;
    index_options.workspace = fs::canonical(workspace).string();
    index_options.max_source_code_file_size = 1024 * 1024;
    agent::ReadToolRegistry live_tools;
    check(agent::ReadToolRegistry::create_without_index(
              index_options, {}, live_tools, live_options)
              .ok(),
          "create indexing-disabled registry for read tool ordering");
    const std::vector<provider::FunctionDefinition> live_definitions =
        live_tools.definitions();
    std::size_t live_many = live_definitions.size();
    std::size_t live_file = live_definitions.size();
    for (std::size_t index = 0; index < live_definitions.size(); ++index) {
        if (live_definitions[index].name == "read_many") live_many = index;
        if (live_definitions[index].name == "read_file") live_file = index;
    }
    check(live_many < live_file &&
              live_definitions[live_many].description.find(
                  "Preferred file reader") != std::string::npos &&
              live_definitions[live_file].description.find(
                  "Single-target fallback") != std::string::npos,
          "indexing-disabled Agent definitions retain read_many preference and order");

    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_index_status_and_update() {
    const std::string workspace = write_temp_workspace("index-tools");
    agent::ReadToolRegistry tools = make_registry(workspace, true);

    const std::string status = tools.execute("index_status", R"JSON({"check_filesystem":true})JSON");
    check(json_ok(status), "index_status succeeds: " + status);
    check(json_data_string(status, "path").find("index.sqlite") != std::string::npos,
          "index_status reports index path");

    write_text(fs::path(workspace) / "src" / "hello.cpp", "int main() { return 1; }\n");
    const std::string update =
        tools.execute("index_update", R"JSON({"paths":["src/hello.cpp"],"force":true})JSON");
    check(json_ok(update), "index_update force path succeeds: " + update);

    const std::string rebuild_denied =
        tools.execute("index_rebuild", R"JSON({"confirm":false})JSON");
    check(!json_ok(rebuild_denied), "index_rebuild requires confirm");

    const std::string rebuild = tools.execute("index_rebuild", R"JSON({"confirm":true})JSON");
    check(json_ok(rebuild), "index_rebuild confirm succeeds: " + rebuild);

    agent::ReadToolRegistry review = make_registry(workspace, false);
    const std::string review_rebuild =
        review.execute("index_rebuild", R"JSON({"confirm":true})JSON");
    check(!json_ok(review_rebuild), "read-only registry hides index_rebuild");
    check(json_error_code(review_rebuild) == "unknown_tool" ||
              json_error_code(review_rebuild) == "policy_denied",
          "index_rebuild denied in review: " + review_rebuild);

    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_lazy_index_tools_and_touched_overlay() {
    const std::string workspace = write_temp_workspace("lazy-tools");
    write_text(fs::path(workspace) / "src" / "symbols.cpp",
               "int alpha() { return 1; }\n");
    agent::ReadToolRegistry tools = make_lazy_registry(workspace);

    check(json_ok(tools.execute("project_overview", "{}")) &&
              json_ok(tools.execute(
                  "glob", R"JSON({"pattern":"src/*.cpp"})JSON")) &&
              json_ok(tools.execute(
                  "search_text", R"JSON({"query":"alpha"})JSON")) &&
              json_ok(tools.execute(
                  "search_symbol", R"JSON({"query":"alpha"})JSON")) &&
              json_ok(tools.execute(
                  "get_skeleton",
                  R"JSON({"path":"src/symbols.cpp"})JSON")) &&
              json_ok(tools.execute(
                  "inspect_code_task",
                  R"JSON({"query":"alpha symbol"})JSON")) &&
              json_ok(tools.execute(
                  "find_tests",
                  R"JSON({"path":"src/symbols.cpp"})JSON")) &&
              json_ok(tools.execute("index_status", "{}")),
          "lazy Agent queries cover overview, files, symbols, skeletons, tests, and totals");

    const std::string write = tools.execute(
        "write_file",
        R"JSON({"path":"src/touched.cpp","content":"int touched_symbol() { return 2; }\n"})JSON");
    const std::string touched = tools.execute(
        "search_symbol", R"JSON({"query":"touched_symbol"})JSON");
    check(json_ok(write) && json_ok(touched) &&
              touched.find("touched_symbol") != std::string::npos,
          "touched-path overlay exposes a native write before/through persistence");
    check(tools.refresh_persistent_index(false).ok(),
          "lazy touched-path revision flushes to SQLite");

    const std::string removed = tools.execute(
        "remove",
        R"JSON({"path":"src/touched.cpp","confirm":true})JSON");
    const std::string after_remove = tools.execute(
        "glob", R"JSON({"pattern":"src/touched.cpp"})JSON");
    check(json_ok(removed) && json_ok(after_remove) &&
              after_remove.find("src/touched.cpp") == std::string::npos,
          "touched-path removal overlay hides persisted rows immediately");
    check(tools.refresh_persistent_index(false).ok(),
          "lazy removal revision flushes and clears its overlay");

    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_inspect_and_find_tests() {
    const std::string workspace = write_temp_workspace("inspect");
    write_text(fs::path(workspace) / "src" / "agent_loop.cpp",
               "void helper() {}\n"
               "void run_agent_loop() { helper(); }\n");
    std::error_code mkdir_ec;
    fs::create_directories(fs::path(workspace) / "tests", mkdir_ec);
    write_text(fs::path(workspace) / "tests" / "test_agent_loop.cpp",
               "void test_agent_loop_runs() {}\n");
    agent::ReadToolRegistry tools = make_registry(workspace, true);

    const std::string inspect =
        tools.execute("inspect_code_task", R"JSON({"query":"agent loop","max_files":5})JSON");
    check(json_ok(inspect), "inspect_code_task succeeds: " + inspect);

    const std::string tests =
        tools.execute("find_tests", R"JSON({"path":"src/agent_loop.cpp"})JSON");
    check(json_ok(tests), "find_tests succeeds: " + tests);

    const std::string helper_search =
        tools.execute("search_symbol", R"JSON({"query":"helper"})JSON");
    check(helper_search.find("\"importance\"") != std::string::npos,
          "search_symbol returns static importance");
    const json::ParseResult helper_parsed = json::parse(helper_search);
    long long helper_id = 0;
    long long run_id = 0;
    if (helper_parsed.error.ok() && helper_parsed.value.get("data") != nullptr &&
        helper_parsed.value.get("data")->is_array()) {
        for (const json::Value& hit :
             helper_parsed.value.get("data")->array) {
            const json::Value* id = hit.get("id");
            const json::Value* name = hit.get("name");
            if (id != nullptr && id->type == json::Value::Type::Number &&
                name != nullptr && name->is_string() &&
                name->string == "helper")
                helper_id = static_cast<long long>(id->number);
        }
    }
    const std::string run_search =
        tools.execute("search_symbol", R"JSON({"query":"run_agent_loop"})JSON");
    const json::ParseResult run_parsed = json::parse(run_search);
    if (run_parsed.error.ok() && run_parsed.value.get("data") != nullptr &&
        run_parsed.value.get("data")->is_array() &&
        !run_parsed.value.get("data")->array.empty()) {
        const json::Value* id =
            run_parsed.value.get("data")->array.front().get("id");
        if (id != nullptr && id->type == json::Value::Type::Number)
            run_id = static_cast<long long>(id->number);
    }
    check(helper_id > 0 && run_id > 0,
          "inspect fixture symbols have ids");
    const std::string helper_read = tools.execute(
        "read_symbol",
        "{\"symbol_id\":" + std::to_string(helper_id) + "}");
    check(json_ok(helper_read) &&
              helper_read.find("\"importance\"") != std::string::npos &&
              helper_read.find("caller_count") == std::string::npos,
          "read_symbol returns importance without graph fields");
    check(!json_ok(tools.execute(
              "find_callers",
              "{\"symbol_id\":" + std::to_string(helper_id) + "}")) &&
              !json_ok(tools.execute(
                  "find_callees",
                  "{\"symbol_id\":" + std::to_string(run_id) + "}")),
          "removed graph tools are unavailable");

    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_git_and_network_tools_policy() {
    const std::string workspace = write_temp_workspace("git-net");
    // Initialize a tiny git repo so git_status has a chance to succeed when git exists.
    {
        const std::string cmd =
            "cd " + workspace +
            " && git init -q && git config user.email t@example.com && "
            "git config user.name test && git add src/hello.cpp && "
            "git commit -q -m init";
        const int git_setup_status = std::system(cmd.c_str());
        (void)git_setup_status;
    }
    agent::ReadToolRegistry tools = make_registry(workspace, true);
    agent::ToolRegistryOptions net_options;
    net_options.mutation_policy = agent::MutationPolicy::Full;
    net_options.allow_network = true;
    {
        agent::index::Options options;
        options.workspace = workspace;
        options.max_source_code_file_size = 1024 * 1024;
        agent::index::RefreshStats stats;
        check(agent::index::refresh(options, stats).ok(), "refresh for network tools");
        agent::index::Snapshot snapshot;
        check(agent::index::load_snapshot(options, snapshot).ok(), "snapshot for network tools");
        agent::ReadToolRegistry net_tools;
        check(agent::ReadToolRegistry::create(std::move(options), std::move(snapshot), {},
                                              net_tools, net_options)
                  .ok(),
              "create network-enabled registry");

        const std::string status = net_tools.execute("git_status", R"JSON({})JSON");
        // git may be missing in some environments; accept ok or unavailable.
        check(json_ok(status) || json_error_code(status) == "unavailable" ||
                  json_error_code(status) == "FileRead" ||
                  json_error_code(status) == "file_read",
              "git_status runs or reports missing git: " + status);

        const std::string diff =
            net_tools.execute("git_diff", R"JSON({"stat":true})JSON");
        check(json_ok(diff) || json_error_code(diff) == "unavailable" ||
                  json_error_code(diff) == "FileRead" ||
                  json_error_code(diff) == "file_read",
              "git_diff runs or reports missing git: " + diff);

        // Private URL should be blocked by default fetch policy.
        const std::string fetch =
            net_tools.execute("fetch_url", R"JSON({"url":"http://127.0.0.1/"})JSON");
        check(!json_ok(fetch), "fetch_url blocks loopback by default: " + fetch);

        // Schema must not invite raw HTML; extract_text is not a declared parameter.
        bool fetch_schema_ok = false;
        for (const provider::FunctionDefinition& def : net_tools.definitions()) {
            if (def.name != "fetch_url") continue;
            check(def.parameters_json.find("extract_text") == std::string::npos,
                  "fetch_url schema omits extract_text (Markdown-only tool)");
            check(def.description.find("Markdown") != std::string::npos ||
                      def.description.find("markdown") != std::string::npos,
                  "fetch_url description mentions Markdown");
            fetch_schema_ok = true;
        }
        check(fetch_schema_ok, "fetch_url is in network-enabled registry");

        // Legacy extract_text=false must not open a raw-HTML path (still blocks loopback).
        const std::string fetch_legacy = net_tools.execute(
            "fetch_url", R"JSON({"url":"http://127.0.0.1/","extract_text":false})JSON");
        check(!json_ok(fetch_legacy), "fetch_url with extract_text=false still policy-safe");

        const std::string search =
            net_tools.execute("search_web", R"JSON({"term":"ainiux agent"})JSON");
        // May succeed via keyless providers or fail unavailable — must not crash.
        check(json_ok(search) || !json_error_code(search).empty(),
              "search_web returns structured result: " + search);
    }

    agent::ReadToolRegistry review = make_registry(workspace, false);
    const std::string no_fetch =
        review.execute("fetch_url", R"JSON({"url":"https://example.com/"})JSON");
    check(!json_ok(no_fetch), "review registry does not expose fetch_url");
    const std::string no_search =
        review.execute("search_web", R"JSON({"term":"x"})JSON");
    check(!json_ok(no_search), "review registry does not expose search_web");

    // Schema list includes new tools in agent mode.
    bool saw_git = false;
    bool saw_index = false;
    bool saw_inspect = false;
    for (const provider::FunctionDefinition& def : tools.definitions()) {
        if (def.name == "git_status" || def.name == "git_diff") saw_git = true;
        if (def.name == "index_status" || def.name == "index_update") saw_index = true;
        if (def.name == "inspect_code_task" || def.name == "find_tests") saw_inspect = true;
    }
    check(saw_git && saw_index && saw_inspect, "agent definitions include new tools");

    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_search_text_uses_rg_when_available_else_builtin() {
    const std::string workspace = write_temp_workspace("search-backend");
    write_text(fs::path(workspace) / "src" / "needle.cpp",
               "int alpha_marker = 1;\n"
               "int beta_marker = 2;\n"
               "int gamma_unique_xyz = 3;\n");
    write_text(fs::path(workspace) / "src" / "other.cpp",
               "int delta_marker = 4;\n");
    agent::index::Options index_options;
    index_options.workspace = fs::canonical(workspace).string();
    agent::index::RefreshStats stats;
    check(agent::index::refresh(index_options, stats).ok(),
          "search backend fixture indexes");
    agent::index::Snapshot snapshot;
    check(agent::index::load_snapshot(index_options, snapshot).ok(),
          "search backend fixture snapshot loads");
    agent::ToolRegistryOptions tool_options;
    tool_options.mutation_policy = agent::MutationPolicy::Full;
    agent::ReadToolRegistry tools;
    check(agent::ReadToolRegistry::create(index_options, std::move(snapshot), {},
                                          tools, tool_options)
              .ok(),
          "search backend registry creates");

    const std::string result = tools.execute(
        "search_text",
        R"JSON({"query":"gamma_unique_xyz","max_results":10,"context":1})JSON");
    check(json_ok(result) &&
              result.find("src/needle.cpp") != std::string::npos &&
              result.find("gamma_unique_xyz") != std::string::npos,
          "search_text finds the rare needle: " + result);
    check(result.find("\"search_backend\"") != std::string::npos,
          "search_text reports search_backend metadata: " + result);
    if (agent::ripgrep_available()) {
        check(result.find("\"search_backend\":\"rg\"") != std::string::npos ||
                  result.find("\"search_backend\": \"rg\"") != std::string::npos,
              "search_text uses rg backend when ripgrep is on the fixed PATH: " +
                  result);
        // Context lines should be present when context>0.
        check(result.find("\"context\"") != std::string::npos,
              "rg search_text includes context when requested: " + result);
    } else {
        check(result.find("builtin") != std::string::npos,
              "search_text falls back to builtin when rg is absent: " + result);
    }

    // Live / no-index path must still work (rg or builtin_live).
    agent::ToolRegistryOptions live_options;
    live_options.mutation_policy = agent::MutationPolicy::Full;
    live_options.indexing_enabled = false;
    agent::ReadToolRegistry live;
    check(agent::ReadToolRegistry::create_without_index(index_options, {}, live,
                                                        live_options)
              .ok(),
          "live registry for search backend");
    const std::string live_result = live.execute(
        "search_text",
        R"JSON({"query":"delta_marker","glob":"src/*.cpp","max_results":5})JSON");
    check(json_ok(live_result) &&
              live_result.find("src/other.cpp") != std::string::npos &&
              live_result.find("delta_marker") != std::string::npos,
          "indexing-off search_text still works with rg/builtin: " + live_result);

    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_plan_document_mutation_policy() {
    const std::string workspace = write_temp_workspace("plan-policy");
    std::error_code ec;
    fs::create_directories(fs::path(workspace) / "docs" / "plans" / "existing", ec);
    write_text(fs::path(workspace) / "PLANS.md", "old plan\n");
    write_text(fs::path(workspace) / "README.md", "readme\n");
    agent::ReadToolRegistry tools =
        make_registry(workspace, agent::MutationPolicy::PlanningDocuments);

    const std::string absolute_plans_args =
        std::string("{\"path\":") +
        json::quote((fs::path(workspace) / "PLANS.md").string()) +
        ",\"content\":\"absolute plan\\n\"}";
    const std::string absolute_plans_result =
        tools.execute("write_file", absolute_plans_args);
    check(json_ok(absolute_plans_result) &&
              json_data_string(absolute_plans_result, "path") == "PLANS.md" &&
              read_text(fs::path(workspace) / "PLANS.md") == "absolute plan\n",
          "Plan policy normalizes an absolute path contained by the project");

    const std::string outside_args =
        std::string("{\"path\":") +
        json::quote((fs::temp_directory_path() / "ainiux-outside-PLANS.md").string()) +
        ",\"content\":\"outside\\n\"}";
    check(!json_ok(tools.execute("write_file", outside_args)),
          "Plan policy still denies absolute paths outside the project");

    check(json_ok(tools.execute(
              "write_file",
              R"JSON({"path":"PLAN.md","content":"new plan\n","mode":"create_new"})JSON")),
          "Plan policy allows approved root plan creation");
    check(json_ok(tools.execute(
              "write_file",
              R"JSON({"path":"docs/plans/existing/design.md","content":"design\n","mode":"create_new"})JSON")),
          "Plan policy allows lowercase .md below existing docs/plans tree");
    check(!json_ok(tools.execute(
              "write_file",
              R"JSON({"path":"README.md","content":"changed\n"})JSON")) &&
              read_text(fs::path(workspace) / "README.md") == "readme\n",
          "Plan policy denies arbitrary root Markdown");
    check(!json_ok(tools.execute(
              "write_file",
              R"JSON({"path":"src/plan.md","content":"changed\n"})JSON")),
          "Plan policy denies Markdown outside docs/plans");
    check(!json_ok(tools.execute(
              "write_file",
              R"JSON({"path":"docs/plans/existing/WRONG.MD","content":"changed\n"})JSON")),
          "Plan policy enforces case-sensitive .md extension");
    check(!json_ok(tools.execute(
              "write_file",
              R"JSON({"path":"docs/plans/missing/design.md","content":"changed\n","create_dirs":true})JSON")),
          "Plan policy cannot create destination directories");
    check(!json_ok(tools.execute(
              "write_file",
              R"JSON({"path":"../PLAN.md","content":"changed\n"})JSON")),
          "Plan policy denies traversal targets");
    fs::create_directory_symlink(fs::path(workspace) / "docs" / "plans" / "existing",
                                 fs::path(workspace) / "docs" / "plans" / "linked", ec);
    check(!json_ok(tools.execute(
              "write_file",
              R"JSON({"path":"docs/plans/linked/design.md","content":"changed\n"})JSON")),
          "Plan policy denies symlink path components");
    check(!json_ok(tools.execute(
              "remove", R"JSON({"path":"PLANS.md"})JSON")),
          "Plan policy hides and defensively denies remove");
    check(!json_ok(tools.execute(
              "apply_patch",
              R"JSON({"patch":"*** Begin Patch\n*** Delete File: PLANS.md\n*** End Patch\n"})JSON")),
          "Plan policy denies patch delete operations");
    check(!json_ok(tools.execute(
              "run_command", R"JSON({"command":"touch PLAN.md"})JSON")),
          "Plan run_command denies non-vetted commands");
    check(json_ok(tools.execute(
              "run_command", R"JSON({"command":"stat -c %y PLANS.md"})JSON")),
          "Plan run_command accepts the expanded vetted read-only set");
    check(json_ok(tools.execute(
              "run_command",
              std::string("{\"command\":") +
                  json::quote("cat " +
                              (fs::path(workspace) / "PLANS.md").string()) +
                  "}")),
          "Plan run_command accepts a canonical absolute in-project read path");
    check(!json_ok(tools.execute(
              "run_command", R"JSON({"command":"tail -f PLANS.md"})JSON")) &&
              !json_ok(tools.execute(
                  "run_command", R"JSON({"command":"find . -exec touch owned ;"})JSON")) &&
              !json_ok(tools.execute(
                  "run_command", R"JSON({"command":"rg --pre cat plan ."})JSON")),
          "Plan denies following, execution, and external-preprocessor forms");

    const std::string mixed =
        "*** Begin Patch\n"
        "*** Update File: PLANS.md\n"
        "@@\n"
        "-absolute plan\n"
        "+updated plan\n"
        "*** Update File: README.md\n"
        "@@\n"
        "-readme\n"
        "+changed\n"
        "*** End Patch\n";
    const std::string mixed_args =
        std::string("{\"patch\":") + json::quote(mixed) + "}";
    check(!json_ok(tools.execute("apply_patch", mixed_args)) &&
              read_text(fs::path(workspace) / "PLANS.md") == "absolute plan\n" &&
              read_text(fs::path(workspace) / "README.md") == "readme\n",
          "Plan mixed patch is rejected before any allowed file changes");

    bool saw_edit = false;
    bool saw_remove = false;
    bool saw_rebuild = false;
    for (const provider::FunctionDefinition& definition : tools.definitions()) {
        if (definition.name == "edit_file" || definition.name == "write_file" ||
            definition.name == "str_replace" || definition.name == "apply_patch")
            saw_edit = true;
        if (definition.name == "remove") saw_remove = true;
        if (definition.name == "index_rebuild") saw_rebuild = true;
    }
    check(saw_edit && saw_remove && saw_rebuild,
          "Plan definitions expose the stable agent superset while policy denies Act-only tools");
    fs::remove_all(workspace, ec);
}

void test_goal_met_tool_hooks() {
    const std::string workspace = write_temp_workspace("goal-met");
    agent::index::Options options;
    options.workspace = workspace;
    options.max_source_code_file_size = 1024 * 1024;
    agent::index::RefreshStats stats;
    check(agent::index::refresh(options, stats).ok(), "index refresh for goal_met");
    agent::index::Snapshot snapshot;
    check(agent::index::load_snapshot(options, snapshot).ok(), "snapshot for goal_met");

    bool active = false;
    std::string completed_evidence;
    agent::ToolRegistryOptions tool_options;
    tool_options.mutation_policy = agent::MutationPolicy::Full;
    tool_options.goal_hooks.has_active_goal = [&]() { return active; };
    tool_options.goal_hooks.mark_complete =
        [&](const std::string& evidence) -> Error {
        if (!active) return {ErrorCode::UnsupportedFeature, "no active session goal"};
        if (ascii_trim(evidence).empty())
            return {ErrorCode::BadArgs, "empty evidence"};
        completed_evidence = evidence;
        active = false;
        return ok_error();
    };

    agent::ReadToolRegistry tools;
    check(agent::ReadToolRegistry::create(std::move(options), std::move(snapshot), {},
                                          tools, tool_options)
              .ok(),
          "create registry with goal hooks");

    bool has_goal_met = false;
    for (const provider::FunctionDefinition& definition : tools.definitions()) {
        if (definition.name == "goal_met") {
            has_goal_met = true;
            check(definition.parameters_json.find("evidence") != std::string::npos,
                  "goal_met schema requires evidence");
        }
    }
    check(has_goal_met, "agent registry always advertises goal_met");

    std::string result =
        tools.execute("goal_met", R"JSON({"evidence":"file exists"})JSON");
    check(!json_ok(result) && json_error_code(result) == "no_active_goal",
          "goal_met rejects when no active goal");

    active = true;
    result = tools.execute("goal_met", R"JSON({"evidence":"   "})JSON");
    check(!json_ok(result) && json_error_code(result) == "invalid_arguments",
          "goal_met rejects empty evidence");

    active = true;
    result = tools.execute("goal_met", R"JSON({"evidence":"tests passed"})JSON");
    check(json_ok(result) && completed_evidence == "tests passed" && !active,
          "goal_met completes active goal with evidence");

    // Security-review / read-only registries hide goal_met.
    agent::ReadToolRegistry readonly =
        make_registry(workspace, agent::MutationPolicy::Disabled);
    has_goal_met = false;
    for (const provider::FunctionDefinition& definition : readonly.definitions()) {
        if (definition.name == "goal_met") has_goal_met = true;
    }
    check(!has_goal_met, "read-only registry does not advertise goal_met");

    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_attach_image_tool_hooks() {
    const std::string workspace = write_temp_workspace("attach-image");
    // Minimal PNG signature + a few bytes (loader checks signature).
    {
        std::ofstream out(fs::path(workspace) / "shot.png", std::ios::binary);
        out.write("\x89PNG\r\n\x1a\n", 8);
        out.write("abc", 3);
    }
    agent::index::Options options;
    options.workspace = workspace;
    options.max_source_code_file_size = 1024 * 1024;
    agent::index::RefreshStats stats;
    check(agent::index::refresh(options, stats).ok(), "index refresh for attach_image");
    agent::index::Snapshot snapshot;
    check(agent::index::load_snapshot(options, snapshot).ok(), "snapshot for attach_image");

    std::vector<provider::ImageInput> queued;
    agent::ToolRegistryOptions tool_options;
    tool_options.mutation_policy = agent::MutationPolicy::Full;
    tool_options.vision_hooks.max_image_bytes = 1024;
    tool_options.vision_hooks.max_images_per_turn = 2;
    tool_options.vision_hooks.validate_capability = []() -> Error { return ok_error(); };
    tool_options.vision_hooks.queue_image = [&](provider::ImageInput image) -> Error {
        if (queued.size() >= 2)
            return {ErrorCode::UnsupportedFeature, "attach_image limit reached for this turn"};
        queued.push_back(std::move(image));
        return ok_error();
    };

    agent::ReadToolRegistry tools;
    check(agent::ReadToolRegistry::create(std::move(options), std::move(snapshot), {},
                                          tools, tool_options)
              .ok(),
          "create registry with vision hooks");

    bool has_attach = false;
    for (const provider::FunctionDefinition& definition : tools.definitions()) {
        if (definition.name == "attach_image") {
            has_attach = true;
            check(definition.parameters_json.find("path") != std::string::npos,
                  "attach_image schema requires path");
        }
    }
    check(has_attach, "agent registry advertises attach_image");

    std::string result =
        tools.execute("attach_image", R"JSON({"path":"shot.png"})JSON");
    check(json_ok(result) && queued.size() == 1 &&
              queued[0].mime_type == "image/png" && !queued[0].base64_data.empty(),
          "attach_image queues a request-local PNG: " + result);

    result = tools.execute("attach_image", R"JSON({"path":"missing.jpg"})JSON");
    check(!json_ok(result), "attach_image rejects missing files");

    result = tools.execute("attach_image", R"JSON({"path":"shot.png"})JSON");
    check(json_ok(result) && queued.size() == 2, "second attach_image succeeds under limit");
    result = tools.execute("attach_image", R"JSON({"path":"shot.png"})JSON");
    check(!json_ok(result) && json_error_code(result) == "limit_exceeded",
          "third attach_image hits per-turn limit");

    // Image path via read_file should not invite Python/PIL.
    result = tools.execute("read_file", R"JSON({"path":"shot.png"})JSON");
    check(!json_ok(result) && result.find("attach_image") != std::string::npos,
          "read_file on image points to attach_image");

    agent::ReadToolRegistry readonly =
        make_registry(workspace, agent::MutationPolicy::Disabled);
    has_attach = false;
    for (const provider::FunctionDefinition& definition : readonly.definitions()) {
        if (definition.name == "attach_image") has_attach = true;
    }
    check(!has_attach, "read-only registry does not advertise attach_image");

    std::error_code ec;
    fs::remove_all(workspace, ec);
}

}  // namespace

void run_all() {
    test_permission_modes_and_native_path_tools();
    test_tool_schemas_gemini_compatible();
    test_external_file_access_requires_one_shot_approval();
    test_glob_recursive_root_and_nested();
    test_read_only_registry_hides_writes();
    test_write_file_create_and_readback();
    test_str_replace_exact();
    test_edit_file_ops();
    test_str_replace_fuzzy_whitespace_and_indent();
    test_remove_tool();
    test_list_directory_filesystem();
    test_smart_act_native_tools_accept_unindexed_project_paths();
    test_agent_command_output_keeps_unindexed_project_paths();
    test_replace_symbol();
    test_index_refresh_drops_completed_prepare_cancellation();
    test_indexing_disabled_registry_is_strict_and_live();
    test_search_text_uses_rg_when_available_else_builtin();
    test_read_many_preference_limits_and_serialization();
    test_index_status_and_update();
    test_lazy_index_tools_and_touched_overlay();
    test_inspect_and_find_tests();
    test_git_and_network_tools_policy();
    test_plan_document_mutation_policy();
    test_goal_met_tool_hooks();
    test_attach_image_tool_hooks();
}

}  // namespace ainiux::test::agent_file_tools
