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

void test_edit_file_ops() {
    const std::string workspace = write_temp_workspace("edit");
    write_text(fs::path(workspace) / "src" / "hello.cpp",
               "line1\nline2\nline3\nline4\n");
    agent::ReadToolRegistry tools = make_registry(workspace, true);

    bool has_edit = false;
    for (const provider::FunctionDefinition& definition : tools.definitions())
        if (definition.name == "edit_file") has_edit = true;
    check(has_edit, "mutation registry exposes edit_file");

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

    const std::string deleted = tools.execute(
        "edit_file",
        R"JSON({"path":"src/hello.cpp","ops":[{"type":"delete_range","start_line":3,"end_line":4}]})JSON");
    check(json_ok(deleted), "delete_range succeeds: " + deleted);
    check(read_text(fs::path(workspace) / "src" / "hello.cpp") == "line1\ninserted\nline4\n",
          "delete_range removed lines");

    const std::string multi = tools.execute(
        "edit_file",
        R"JSON({"path":"src/hello.cpp","ops":[
          {"type":"replace_range","start_line":1,"end_line":1,"replacement":"A\n"},
          {"type":"replace_range","start_line":3,"end_line":3,"replacement":"C\n"}
        ]})JSON");
    check(json_ok(multi), "multi line ops bottom-to-top succeed: " + multi);
    check(read_text(fs::path(workspace) / "src" / "hello.cpp") == "A\ninserted\nC\n",
          "multi replace_range applied correctly");

    const std::string text_op = tools.execute(
        "edit_file",
        R"JSON({"path":"src/hello.cpp","ops":[{"type":"replace_text","old_text":"inserted","new_text":"B"}]})JSON");
    check(json_ok(text_op), "replace_text succeeds: " + text_op);
    check(read_text(fs::path(workspace) / "src" / "hello.cpp") == "A\nB\nC\n",
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

    std::error_code ec;
    fs::remove_all(workspace, ec);
}

}  // namespace

void run_all() {
    test_glob_recursive_root_and_nested();
    test_read_only_registry_hides_writes();
    test_write_file_create_and_readback();
    test_str_replace_exact();
    test_edit_file_ops();
}

}  // namespace ainiux::test::agent_file_tools
