#include "agent/test_apply_patch.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include "agent/apply_patch.hpp"
#include "agent/index/index.hpp"
#include "agent/tools.hpp"
#include "json/json.hpp"
#include "platform/environment.hpp"
#include "support/test_support.hpp"

namespace ainiux::test::agent_apply_patch {
namespace {
using ainiux::test::check;
namespace fs = std::filesystem;

std::string temp_workspace(const std::string& name) {
    const fs::path root =
        fs::temp_directory_path() / ("ainiux-apply-patch-" + name + "-" +
                                     std::to_string(ainiux::platform::current_process_id()));
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
    tool_options.mutation_policy = allow_mutations ? agent::MutationPolicy::Full : agent::MutationPolicy::Disabled;
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

void test_sequential_identical_sites() {
    // GPT-5 often emits two identical one-line hunks for two call sites.
    const std::string original =
        "void a() {\n"
        "    outgoing.body_len = 23U;\n"
        "}\n"
        "void b() {\n"
        "    outgoing.body_len = 23U;\n"
        "}\n";
    agent::ParsedPatch parsed;
    const std::string patch =
        "*** Begin Patch\n"
        "*** Update File: src/server.c\n"
        "@@\n"
        "-    outgoing.body_len = 23U;\n"
        "+    outgoing.body_len = sizeof(\"internal server error\\n\") - 1U;\n"
        "@@\n"
        "-    outgoing.body_len = 23U;\n"
        "+    outgoing.body_len = sizeof(\"internal server error\\n\") - 1U;\n"
        "*** End Patch\n";
    check(agent::parse_apply_patch(patch, parsed).ok() && parsed.ops.size() == 1 &&
              parsed.ops[0].hunks.size() == 2,
          "dual identical hunks parse");
    std::string result;
    std::vector<std::string> modes;
    const Error error =
        agent::apply_patch_hunks(original, parsed.ops[0].hunks, true, result, modes);
    check(error.ok(), "dual identical sites apply: " + error.message);
    check(result.find("outgoing.body_len = 23U;") == std::string::npos,
          "both sites replaced: " + result);
    check(std::count(result.begin(), result.end(), 's') > 0 &&
              result.find("sizeof") != std::string::npos,
          "sizeof replacements present");
    const std::size_t first = result.find("sizeof");
    const std::size_t second = result.find("sizeof", first + 1);
    check(first != std::string::npos && second != std::string::npos && second > first,
          "two sizeof sites");
}

void test_sequential_after_unique_then_weak_brace() {
    // Unique earlier hunk advances the cursor; trailing pure insert after '}' then
    // takes the next brace after the cursor (session log rounds 59/66 pattern).
    const std::string original =
        "static void handle_connection(int fd, server_handler_fn handler, void *user_data)\n"
        "{\n"
        "    memset(&conn, 0, sizeof(conn));\n"
        "    conn.fd = fd;\n"
        "    conn.handler = handler == NULL ? default_handler : handler;\n"
        "    conn.user_data = user_data;\n"
        "}\n"
        "\n"
        "int h2_server_run(const char *bind_address, uint16_t port,\n"
        "                  server_handler_fn handler, void *user_data)\n"
        "{\n"
        "    handle_connection(client, handler, user_data);\n"
        "}\n"
        "\n"
        "void other(void) {\n"
        "    int x = 0;\n"
        "}\n";
    agent::ParsedPatch parsed;
    const std::string patch =
        "*** Begin Patch\n"
        "*** Update File: src/server.c\n"
        "@@\n"
        "-static void handle_connection(int fd, server_handler_fn handler, void *user_data)\n"
        "+static void handle_connection(int fd, const char *web_root,\n"
        "+                              server_handler_fn handler, void *user_data)\n"
        "@@\n"
        "    memset(&conn, 0, sizeof(conn));\n"
        "    conn.fd = fd;\n"
        "-    conn.handler = handler == NULL ? default_handler : handler;\n"
        "-    conn.user_data = user_data;\n"
        "+    conn.use_default_handler = handler == NULL;\n"
        "+    conn.handler = conn.use_default_handler ? default_handler : handler;\n"
        "+    conn.web_root = web_root;\n"
        "+    conn.user_data = conn.use_default_handler ? &conn : user_data;\n"
        "@@\n"
        "-int h2_server_run(const char *bind_address, uint16_t port,\n"
        "-                  server_handler_fn handler, void *user_data)\n"
        "+int h2_server_run_with_root(const char *bind_address, uint16_t port,\n"
        "+                            const char *web_root, server_handler_fn handler,\n"
        "+                            void *user_data)\n"
        "@@\n"
        "-        handle_connection(client, handler, user_data);\n"
        "+        handle_connection(client, web_root, handler, user_data);\n"
        "@@\n"
        " }\n"
        "+\n"
        "+int h2_server_run(const char *bind_address, uint16_t port,\n"
        "+                  server_handler_fn handler, void *user_data)\n"
        "+{\n"
        "+    return h2_server_run_with_root(bind_address, port, NULL, handler, user_data);\n"
        "+}\n"
        "*** End Patch\n";
    check(agent::parse_apply_patch(patch, parsed).ok(), "weak-brace multi-hunk parses");
    std::string result;
    std::vector<std::string> modes;
    const Error error =
        agent::apply_patch_hunks(original, parsed.ops[0].hunks, true, result, modes);
    check(error.ok(), "sequential weak brace insert: " + error.message);
    check(result.find("h2_server_run_with_root") != std::string::npos, "renamed with_root");
    check(result.find("return h2_server_run_with_root") != std::string::npos,
          "wrapper inserted: " + result);
    // Wrapper should appear after h2_server_run_with_root body, not after other().
    const std::size_t wrapper = result.find("return h2_server_run_with_root");
    const std::size_t other_fn = result.find("void other");
    check(wrapper != std::string::npos && other_fn != std::string::npos && wrapper < other_fn,
          "wrapper before other(): " + result);
}

void test_line_anchor_disambiguates_brace() {
    // Bare first-hunk '}' is still ambiguous without an anchor or prior cursor.
    const std::string original = "void a(void) {\n}\nvoid b(void) {\n}\n";
    agent::ParsedPatch parsed;
    const std::string ambiguous =
        "*** Begin Patch\n"
        "*** Update File: x.c\n"
        "@@\n"
        " }\n"
        "+// after\n"
        "*** End Patch\n";
    check(agent::parse_apply_patch(ambiguous, parsed).ok(), "ambiguous brace parses");
    std::string result;
    std::vector<std::string> modes;
    Error error = agent::apply_patch_hunks(original, parsed.ops[0].hunks, true, result, modes);
    check(!error.ok() && error.message.find("matches ") != std::string::npos,
          "bare brace still ambiguous: " + error.message);
    check(error.message.find("old_text=") != std::string::npos,
          "error includes old_text preview: " + error.message);

    const std::string anchored =
        "*** Begin Patch\n"
        "*** Update File: x.c\n"
        "@@ -2,1 +2,2 @@\n"
        " }\n"
        "+// after a\n"
        "*** End Patch\n";
    check(agent::parse_apply_patch(anchored, parsed).ok(), "anchored brace parses");
    error = agent::apply_patch_hunks(original, parsed.ops[0].hunks, true, result, modes);
    check(error.ok(), "line-anchored brace applies: " + error.message);
    check(result.find("// after a") != std::string::npos &&
              result.find("void b") != std::string::npos &&
              result.find("// after a") < result.find("void b"),
          "insert after first function: " + result);
}

void test_tool_diff_alias_and_return_zero_context() {
    // Session B pattern: many "return 0;\\n}" sites; multi-hunk with unique prior then insert.
    const std::string workspace = temp_workspace("return0");
    std::string body;
    for (int i = 0; i < 5; ++i) {
        body += "int f";
        body += std::to_string(i);
        body += "(void) {\n    do_work();\n    return 0;\n}\n\n";
    }
    write_text(fs::path(workspace) / "src" / "server.c", body);
    agent::ReadToolRegistry tools = make_registry(workspace, true);

    // Unique rename of f2 then insert after its return 0;\n}
    const std::string patch =
        "*** Begin Patch\n"
        "*** Update File: src/server.c\n"
        "@@\n"
        "-int f2(void) {\n"
        "+int f2_http1(void) {\n"
        "@@\n"
        " int f2_http1(void) {\n"
        "     do_work();\n"
        "     return 0;\n"
        " }\n"
        "+static int extra(void) { return 1; }\n"
        "*** End Patch\n";
    std::string escaped;
    for (char ch : patch) {
        if (ch == '\\' || ch == '"') escaped.push_back('\\');
        if (ch == '\n') {
            escaped += "\\n";
            continue;
        }
        escaped.push_back(ch);
    }
    // Prefer the `diff` field like OpenRouter gpt-5.6-luna.
    const std::string result =
        tools.execute("apply_patch", std::string("{\"diff\":\"") + escaped + "\"}");
    check(json_ok(result), "diff alias multi-hunk applies: " + result);
    const std::string content = read_text(fs::path(workspace) / "src" / "server.c");
    check(content.find("int f2_http1(void)") != std::string::npos, "renamed f2");
    check(content.find("static int extra") != std::string::npos, "extra inserted: " + content);
    // Insertion should sit with f2_http1, not only at EOF after f4.
    const std::size_t extra_pos = content.find("static int extra");
    const std::size_t f3 = content.find("int f3(void)");
    check(extra_pos != std::string::npos && f3 != std::string::npos && extra_pos < f3,
          "extra before f3: " + content);
    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void run_all() {
    test_parse_and_hunks();
    test_apply_patch_tool();
    test_malformed_patch();
    test_bare_update_without_envelope();
    test_tool_accepts_bare_patch();
    test_sequential_identical_sites();
    test_sequential_after_unique_then_weak_brace();
    test_line_anchor_disambiguates_brace();
    test_tool_diff_alias_and_return_zero_context();
}

}  // namespace ainiux::test::agent_apply_patch
