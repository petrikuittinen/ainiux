#include "agent/test_file_tools.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "agent/index/index.hpp"
#include "agent/tools.hpp"
#include "json/json.hpp"
#include "provider/provider.hpp"
#include "support/test_support.hpp"

namespace ainiux::test::agent_file_tools {
namespace {
using ainiux::test::check;
namespace fs = std::filesystem;

std::string write_temp_workspace(const std::string& name) {
    const fs::path root =
        fs::temp_directory_path() / ("ainiux-agent-file-tools-" + name + "-" +
                                     std::to_string(static_cast<long long>(::getpid())));
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
                                      agent::GuardApprovalCallback on_guard_ask = {}) {
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

    const std::string missing = tools.execute(
        "edit_file",
        R"JSON({"path":"src/sym.cpp","ops":[{"type":"replace_symbol","symbol_id":999999,"replacement":"x"}]})JSON");
    check(!json_ok(missing), "unknown symbol_id fails: " + missing);

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

void test_inspect_and_find_tests() {
    const std::string workspace = write_temp_workspace("inspect");
    write_text(fs::path(workspace) / "src" / "agent_loop.cpp", "void run_agent_loop() {}\n");
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
          "Plan run_command uses inspection-only command policy");

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
    check(saw_edit && !saw_remove && !saw_rebuild,
          "Plan definitions expose planning edits but hide remove and index_rebuild");
    fs::remove_all(workspace, ec);
}

}  // namespace

void run_all() {
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
    test_replace_symbol();
    test_index_status_and_update();
    test_inspect_and_find_tests();
    test_git_and_network_tools_policy();
    test_plan_document_mutation_policy();
}

}  // namespace ainiux::test::agent_file_tools
