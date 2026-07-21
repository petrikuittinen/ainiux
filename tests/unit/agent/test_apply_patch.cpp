#include "agent/test_apply_patch.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "agent/apply_patch.hpp"
#include "agent/index/index.hpp"
#include "agent/tools.hpp"
#include "json/json.hpp"
#include "support/test_support.hpp"

namespace ainiux::test::agent_apply_patch {
namespace {
using ainiux::test::check;
namespace fs = std::filesystem;

std::string temp_workspace(const std::string& name) {
    const fs::path root =
        fs::temp_directory_path() / ("ainiux-apply-patch-" + name + "-" +
                                     std::to_string(static_cast<long long>(::getpid())));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "src", ec);
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

agent::ReadToolRegistry make_registry(const std::string& workspace, bool allow_mutations) {
    agent::index::Options options;
    options.workspace = workspace;
    options.max_source_code_file_size = 1024 * 1024;
    agent::index::RefreshStats stats;
    check(agent::index::refresh(options, stats).ok(), "index refresh");
    agent::index::Snapshot snapshot;
    check(agent::index::load_snapshot(options, snapshot).ok(), "load snapshot");
    agent::ReadToolRegistry tools;
    agent::ToolRegistryOptions tool_options;
    tool_options.allow_mutations = allow_mutations;
    check(agent::ReadToolRegistry::create(std::move(options), std::move(snapshot), {}, tools,
                                          tool_options)
              .ok(),
          "create registry");
    return tools;
}

void test_parse_and_hunks() {
    agent::ParsedPatch parsed;
    const std::string patch =
        "*** Begin Patch\n"
        "*** Add File: hello.txt\n"
        "+hello\n"
        "+world\n"
        "*** Update File: src/a.cpp\n"
        "@@\n"
        " int main() {\n"
        "-  return 1;\n"
        "+  return 0;\n"
        " }\n"
        "*** Delete File: gone.txt\n"
        "*** End Patch\n";
    const Error error = agent::parse_apply_patch(patch, parsed);
    check(error.ok(), "parse sample patch: " + error.message);
    check(parsed.ops.size() == 3, "three ops");
    check(parsed.ops[0].kind == agent::PatchOpKind::AddFile && parsed.ops[0].path == "hello.txt",
          "add path");
    check(parsed.ops[0].add_content.find("hello") != std::string::npos &&
              parsed.ops[0].add_content.find("world") != std::string::npos,
          "add body");
    check(parsed.ops[1].kind == agent::PatchOpKind::UpdateFile && parsed.ops[1].hunks.size() == 1,
          "update hunk");
    check(parsed.ops[2].kind == agent::PatchOpKind::DeleteFile && parsed.ops[2].path == "gone.txt",
          "delete path");

    std::string old_text;
    std::string new_text;
    check(agent::hunk_old_new_text(parsed.ops[1].hunks[0], old_text, new_text).ok(), "hunk texts");
    check(old_text.find("return 1") != std::string::npos, "old has return 1");
    check(new_text.find("return 0") != std::string::npos, "new has return 0");

    std::string result;
    std::vector<std::string> modes;
    const std::string original = "int main() {\n  return 1;\n}\n";
    check(agent::apply_patch_hunks(original, parsed.ops[1].hunks, true, result, modes).ok(),
          "apply hunks");
    check(result == "int main() {\n  return 0;\n}\n", "hunk result content: " + result);
}

void test_apply_patch_tool() {
    const std::string workspace = temp_workspace("tool");
    write_text(fs::path(workspace) / "src" / "a.cpp", "int main() {\n  return 1;\n}\n");
    write_text(fs::path(workspace) / "gone.txt", "bye\n");
    agent::ReadToolRegistry tools = make_registry(workspace, true);

    bool has_patch = false;
    for (const provider::FunctionDefinition& definition : tools.definitions())
        if (definition.name == "apply_patch") has_patch = true;
    check(has_patch, "mutation registry exposes apply_patch");
    check(!json_ok(make_registry(workspace, false)
                       .execute("apply_patch", R"JSON({"patch":"*** Begin Patch\n*** End Patch"})JSON")),
          "read-only denies apply_patch");

    const std::string patch =
        "*** Begin Patch\n"
        "*** Add File: src/new.md\n"
        "+# title\n"
        "*** Update File: src/a.cpp\n"
        "@@\n"
        " int main() {\n"
        "-  return 1;\n"
        "+  return 0;\n"
        " }\n"
        "*** Delete File: gone.txt\n"
        "*** End Patch\n";

    // Escape for JSON string.
    std::string escaped;
    for (char ch : patch) {
        if (ch == '\\' || ch == '"') escaped.push_back('\\');
        if (ch == '\n') {
            escaped += "\\n";
            continue;
        }
        if (ch == '\r') continue;
        escaped.push_back(ch);
    }
    const std::string args = std::string("{\"patch\":\"") + escaped + "\"}";
    const std::string result = tools.execute("apply_patch", args);
    check(json_ok(result), "apply_patch succeeds: " + result);
    check(read_text(fs::path(workspace) / "src" / "a.cpp") == "int main() {\n  return 0;\n}\n",
          "update applied");
    check(read_text(fs::path(workspace) / "src" / "new.md") == "# title\n", "add applied");
    check(!fs::exists(fs::path(workspace) / "gone.txt"), "delete applied");

    // Same-session read sees updated file.
    const std::string read = tools.execute(
        "read_file", R"JSON({"path":"src/a.cpp","start_line":1,"end_line":10,"max_bytes":4096})JSON");
    check(json_ok(read), "read after apply_patch: " + read);

    // input alias works.
    write_text(fs::path(workspace) / "src" / "b.cpp", "x = 1\n");
    tools = make_registry(workspace, true);
    const std::string patch2 =
        "*** Begin Patch\n"
        "*** Update File: src/b.cpp\n"
        "@@\n"
        "-x = 1\n"
        "+x = 2\n"
        "*** End Patch\n";
    std::string escaped2;
    for (char ch : patch2) {
        if (ch == '\\' || ch == '"') escaped2.push_back('\\');
        if (ch == '\n') {
            escaped2 += "\\n";
            continue;
        }
        escaped2.push_back(ch);
    }
    const std::string via_input =
        tools.execute("apply_patch", std::string("{\"input\":\"") + escaped2 + "\"}");
    check(json_ok(via_input), "input alias: " + via_input);
    check(read_text(fs::path(workspace) / "src" / "b.cpp") == "x = 2\n", "input alias applied");

    // Failed context does not apply (atomic plan fails before writes).
    write_text(fs::path(workspace) / "src" / "c.cpp", "keep\n");
    tools = make_registry(workspace, true);
    const std::string bad =
        "*** Begin Patch\n"
        "*** Update File: src/c.cpp\n"
        "@@\n"
        "-missing line\n"
        "+nope\n"
        "*** End Patch\n";
    std::string escaped_bad;
    for (char ch : bad) {
        if (ch == '\\' || ch == '"') escaped_bad.push_back('\\');
        if (ch == '\n') {
            escaped_bad += "\\n";
            continue;
        }
        escaped_bad.push_back(ch);
    }
    const std::string failed =
        tools.execute("apply_patch", std::string("{\"patch\":\"") + escaped_bad + "\"}");
    check(!json_ok(failed), "bad hunk fails: " + failed);
    check(read_text(fs::path(workspace) / "src" / "c.cpp") == "keep\n", "file unchanged on failure");

    // Fuzzy whitespace update.
    write_text(fs::path(workspace) / "src" / "d.cpp", "foo(  1,  2 )\n");
    tools = make_registry(workspace, true);
    const std::string fuzzy =
        "*** Begin Patch\n"
        "*** Update File: src/d.cpp\n"
        "@@\n"
        "-foo(1, 2)\n"
        "+foo(3, 4)\n"
        "*** End Patch\n";
    std::string escaped_fuzzy;
    for (char ch : fuzzy) {
        if (ch == '\\' || ch == '"') escaped_fuzzy.push_back('\\');
        if (ch == '\n') {
            escaped_fuzzy += "\\n";
            continue;
        }
        escaped_fuzzy.push_back(ch);
    }
    const std::string fuzzy_result =
        tools.execute("apply_patch", std::string("{\"patch\":\"") + escaped_fuzzy + "\"}");
    check(json_ok(fuzzy_result), "fuzzy update succeeds: " + fuzzy_result);
    check(read_text(fs::path(workspace) / "src" / "d.cpp") == "foo(3, 4)\n",
          "fuzzy update content");

    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_malformed_patch() {
    agent::ParsedPatch parsed;
    check(!agent::parse_apply_patch("not a patch", parsed).ok(), "rejects garbage");
    check(!agent::parse_apply_patch("*** Begin Patch\n*** End Patch\n", parsed).ok(),
          "rejects empty ops");
    check(!agent::parse_apply_patch("*** Begin Patch\n*** Add File:\n+x\n*** End Patch\n", parsed)
               .ok(),
          "rejects empty path");
}

void test_bare_update_without_envelope() {
    // Qwen and similar models often omit *** Begin/End Patch on the first try.
    const std::string bare =
        "*** Update File: hello.py\n"
        "@@ -1,3 +1,4 @@\n"
        " # comment\n"
        "-print(1)\n"
        "+def main():\n"
        "+    print(1)\n";
    agent::ParsedPatch parsed;
    const Error error = agent::parse_apply_patch(bare, parsed);
    check(error.ok(), "bare Update File without Begin/End parses: " + error.message);
    check(parsed.ops.size() == 1 && parsed.ops[0].kind == agent::PatchOpKind::UpdateFile &&
              parsed.ops[0].path == "hello.py",
          "one update op for hello.py");

    std::string result;
    std::vector<std::string> modes;
    check(agent::apply_patch_hunks("# comment\nprint(1)\n", parsed.ops[0].hunks, true, result, modes)
              .ok(),
          "bare patch hunks apply");
    check(result.find("def main()") != std::string::npos &&
              result.find("print(1)") != std::string::npos,
          "bare patch result: " + result);

    // Begin without End still errors (incomplete envelope).
    check(!agent::parse_apply_patch("*** Begin Patch\n*** Update File: x\n@@\n-a\n+b\n", parsed).ok(),
          "Begin without End fails");

    // Markdown fence wrapper is stripped.
    const std::string fenced =
        "```\n*** Begin Patch\n*** Add File: a.txt\n+hi\n*** End Patch\n```\n";
    check(agent::parse_apply_patch(fenced, parsed).ok() && parsed.ops.size() == 1 &&
              parsed.ops[0].kind == agent::PatchOpKind::AddFile,
          "fenced patch parses");
}

void test_tool_accepts_bare_patch() {
    const std::string workspace = temp_workspace("bare");
    write_text(fs::path(workspace) / "src" / "hello.py", "print(1)\n");
    agent::ReadToolRegistry tools = make_registry(workspace, true);
    const std::string bare =
        "*** Update File: src/hello.py\n"
        "@@\n"
        "-print(1)\n"
        "+def main():\n"
        "+    print(1)\n"
        "+\n"
        "+if __name__ == \"__main__\":\n"
        "+    main()\n";
    std::string escaped;
    for (char ch : bare) {
        if (ch == '\\' || ch == '"') escaped.push_back('\\');
        if (ch == '\n') {
            escaped += "\\n";
            continue;
        }
        escaped.push_back(ch);
    }
    const std::string result =
        tools.execute("apply_patch", std::string("{\"patch\":\"") + escaped + "\"}");
    check(json_ok(result), "tool accepts bare patch: " + result);
    const std::string content = read_text(fs::path(workspace) / "src" / "hello.py");
    check(content.find("def main()") != std::string::npos &&
              content.find("__main__") != std::string::npos,
          "bare tool patch applied: " + content);
    std::error_code ec;
    fs::remove_all(workspace, ec);
}

}  // namespace

void run_all() {
    test_parse_and_hunks();
    test_apply_patch_tool();
    test_malformed_patch();
    test_bare_update_without_envelope();
    test_tool_accepts_bare_patch();
}

}  // namespace ainiux::test::agent_apply_patch
