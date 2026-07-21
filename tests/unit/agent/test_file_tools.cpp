#include "agent/test_file_tools.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "agent/index/index.hpp"
#include "agent/tools.hpp"
#include "json/json.hpp"
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

agent::ReadToolRegistry make_registry(const std::string& workspace, bool allow_mutations) {
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
    tool_options.allow_mutations = allow_mutations;
    const Error create_error =
        agent::ReadToolRegistry::create(std::move(options), std::move(snapshot), {}, tools,
                                        tool_options);
    check(create_error.ok(), "create tool registry");
    return tools;
}

void test_read_only_registry_hides_writes() {
    const std::string workspace = write_temp_workspace("readonly");
    agent::ReadToolRegistry tools = make_registry(workspace, false);
    bool has_write = false;
    bool has_replace = false;
    for (const provider::FunctionDefinition& definition : tools.definitions()) {
        if (definition.name == "write_file") has_write = true;
        if (definition.name == "str_replace") has_replace = true;
    }
    check(!has_write && !has_replace, "read-only registry omits mutation tools");
    const std::string denied =
        tools.execute("write_file", R"({"path":"src/x.cpp","content":"x\n"})");
    check(!json_ok(denied) && json_error_code(denied) == "policy_denied",
          "write_file denied without mutations");
    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_write_file_create_and_readback() {
    const std::string workspace = write_temp_workspace("write");
    agent::ReadToolRegistry tools = make_registry(workspace, true);
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

    const std::string nested = tools.execute(
        "write_file",
        R"({"path":"src/deep/nested/file.txt","content":"nested\n","create_dirs":true,"mode":"create_new"})");
    check(json_ok(nested), "create_dirs creates parents: " + nested);
    check(read_text(fs::path(workspace) / "src" / "deep" / "nested" / "file.txt") == "nested\n",
          "nested create wrote content");

    const std::string escape =
        tools.execute("write_file", R"({"path":"../outside.txt","content":"x\n"})");
    check(!json_ok(escape), "path escape is denied");
    const std::string protected_path =
        tools.execute("write_file", R"({"path":".ainiux/evil.txt","content":"x\n"})");
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

}  // namespace

void run_all() {
    test_read_only_registry_hides_writes();
    test_write_file_create_and_readback();
    test_str_replace_exact();
}

}  // namespace ainiux::test::agent_file_tools
