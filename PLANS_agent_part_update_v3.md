# PLANS.md - agent-mode focused update for pkchat

This is an isolated agent-mode section intended for manual merging into the current `PLANS.md`. It deliberately avoids reworking the whole project plan. It focuses on the agent runtime, built-in tools, prompt/rules loading, hashing, parallel calls, destructive-command guard, memory, and verification.

## 1. Scope

Implement a fast, dependency-light coding agent in pkchat using the existing C++17 codebase, libcurl, and SQLite3.

This section covers:

- agent loop
- provider-neutral tool calls
- file/search/edit/web/command tools
- `glob` and `grep` compatibility tools
- prompt and `AGENTS.md` loading
- file-level content hashes
- parallel tool scheduling rules
- DCG-inspired destructive-command guard
- rollback and `/undo`
- memory and compaction
- tests and verification

This section does not cover the full code-index implementation in detail. The index remains part of the larger plan, but the agent should treat it as a fast hint source, not as ground truth.

## 2. Agent principles

1. Keep It Simple Stupid.
2. Prefer C++17 standard library and existing dependencies.
3. Do not add Tree-sitter, libgit2, MCP, plugins, subagents, or heavyweight frameworks in the first serious version.
4. Optimize for fewer model turns, not just faster local function calls.
5. Prefer batched reads, compact skeletons, indexed symbol lookup, and bounded tool output.
6. Use common tool names that most LLMs already understand.
7. Use file-level content hashes as the primary freshness and edit-safety mechanism.
8. Keep line-level/hashline anchors as possible future work.
9. Serialize mutating tools that touch the same file or workspace state.
10. Let safe read-only tools run in parallel when possible.
11. Guard destructive commands cheaply with regex/pattern matching before execution.
12. Do not prompt constantly. Ask only for clearly destructive or indeterminate high-risk actions.

## 3. Suggested implementation files

Adapt names to the existing pkchat layout if equivalent files already exist. Do not duplicate functionality.

```text
src/agent/agent_loop.h
src/agent/agent_loop.cpp
src/agent/tool_call.h
src/agent/tool_registry.h
src/agent/tool_registry.cpp
src/agent/tool_scheduler.h
src/agent/tool_scheduler.cpp
src/agent/tools_files.h
src/agent/tools_files.cpp
src/agent/tools_search.h
src/agent/tools_search.cpp
src/agent/tools_edit.h
src/agent/tools_edit.cpp
src/agent/tools_command.h
src/agent/tools_command.cpp
src/agent/tools_web.h
src/agent/tools_web.cpp
src/agent/git_tools.h
src/agent/git_tools.cpp
src/agent/command_guard.h
src/agent/command_guard.cpp
src/agent/agent_prompts.h
src/agent/agent_prompts.cpp
src/agent/agents_md.h
src/agent/agents_md.cpp
src/agent/agent_memory.h
src/agent/agent_memory.cpp
src/agent/context_compact.h
src/agent/context_compact.cpp
src/agent/hash.h
src/agent/hash.cpp
```

Suggested tests:

```text
tests/agent/test_tool_registry.cpp
tests/agent/test_file_tools.cpp
tests/agent/test_edit_tools.cpp
tests/agent/test_str_replace_fuzzy.cpp
tests/agent/test_apply_patch.cpp
tests/agent/test_command_guard.cpp
tests/agent/test_tool_scheduler.cpp
tests/agent/test_agents_md.cpp
tests/agent/test_memory.cpp
tests/agent/test_web_tools.cpp
```

Project-local runtime data:

```text
.pkchat/
  settings.json
  session.jsonl
  memory.md
  history/
  tmp/
  plans/
```

## 4. Settings-driven limits

Do not hardcode practical limits inside tools. Store defaults in settings and allow override by user-global settings, project settings, session settings, and individual tool-call parameters.

Resolution order:

```text
compiled safe default
user-global settings
project .pkchat/settings.json
session /settings overrides
single tool-call parameter
```

Recommended settings keys:

```json
{
  "limits": {
    "read_file": {"max_lines": 500, "max_bytes": 50000},
    "read_many": {"max_total_bytes": 80000},
    "get_skeleton": {"max_items": 300, "max_bytes": 40000},
    "glob": {"max_results": 500},
    "search_text": {"max_hits": 80, "max_bytes": 60000},
    "search_symbol": {"max_results": 20},
    "read_symbol": {"max_bytes": 50000},
    "git_diff": {"max_bytes": 50000},
    "run_command": {"timeout_ms": 120000, "max_output_bytes": 12000},
    "fetch_url": {"timeout_ms": 30000, "max_bytes": 200000},
    "search_web": {"timeout_ms": 30000, "max_results": 10}
  },
  "hash": {
    "file_hash_algorithm": "fnv1a64",
    "include_size_in_fingerprint": true
  },
  "parallel": {
    "max_read_tools": 8,
    "max_network_tools": 3,
    "serialize_workspace_mutations": true
  },
  "editing": {
    "fuzzy_edit": true,
    "require_unique_match_by_default": true,
    "max_fuzzy_candidates": 20
  },
  "guard": {
    "enabled": true,
    "ask_on_destructive_git": true,
    "ask_on_recursive_force_delete": true,
    "ask_on_database_delete": true,
    "ask_on_destructive_sql": true,
    "forbid_workspace_escape": true
  },
  "web": {
    "enabled": true,
    "search_provider": "configured",
    "user_agent": "pkchat/agent"
  },
  "agents_md": {
    "enabled": true,
    "load_root": true,
    "load_nearest_for_files": true,
    "max_bytes_total": 20000
  }
}
```

The exact numeric values are suggested defaults only. The implementation should read them from settings.

## 5. Provider-neutral tool representation

Use one internal representation and convert it to OpenAI-compatible, Anthropic-style, Gemini-style, and plain text fallback tool formats at the adapter boundary.

```cpp
struct ToolCall {
    std::string id;
    std::string name;
    Json args;
};

struct ToolError {
    std::string code;
    std::string message;
    std::string rule_id;
};

struct ToolResult {
    std::string id;
    bool ok;
    Json data;
    std::vector<std::string> warnings;
    bool truncated = false;
    Json metadata;
    std::optional<ToolError> error;
};
```

Common compact JSON shape:

```json
{
  "ok": true,
  "error": null,
  "data": {},
  "warnings": [],
  "truncated": false,
  "metadata": {}
}
```

Failure shape:

```json
{
  "ok": false,
  "error": {
    "code": "stale_file",
    "message": "The file changed since it was read.",
    "rule_id": null
  },
  "data": {},
  "warnings": [],
  "truncated": false,
  "metadata": {}
}
```

For weaker models that do not reliably use native tool calls, support a strict text fallback:

```text
<tool_call name="read_file">
{"path":"src/main.cpp","start_line":1,"end_line":120}
</tool_call>
```

## 6. File hashes and freshness

Use file-level content hashes as the primary safety and freshness mechanism.

Recommended first version:

- store `size`, `mtime_ns`, and `file_hash`
- skip hashing if size and timestamp are unchanged
- hash only changed files
- reindex only touched files after pkchat edits
- include `file_hash` in read/edit results
- require or recommend `expected_file_hash` for overwrite-like operations

The hash is a fast content fingerprint for stale-context detection, not a security primitive. A simple in-tree implementation such as FNV-1a 64-bit is acceptable for v1. If collision risk becomes a practical concern, move to a stronger built-in implementation later without changing the tool API.

Line-level hashes are postponed. Range hashes may still be returned by `read_file`, `read_many`, and `read_symbol` because they are useful for `replace_range`, but the main freshness check remains the file hash.

Suggested file fingerprint output:

```json
{
  "path": "src/agent.cpp",
  "size": 38211,
  "mtime_ns": 1783170000123456789,
  "file_hash": "fnv1a64:8f72c1e3aa010c91"
}
```

## 7. Built-in tool list

Prefer short, familiar tool names. Some tools are aliases for model compatibility.

### 7.1 Context and discovery tools

```text
project_overview
inspect_code_task
list_directory
glob
search_text
grep
find
search_symbol
get_skeleton
read_symbol
read_file
read_many
find_callers
find_callees
find_tests
```

### 7.2 File and edit tools

```text
write_file
remove
edit_file
str_replace
apply_patch
```

Optional provider-facing alias:

```text
delete_file -> remove
```

### 7.3 Command, git, and web tools

```text
run_command
git_status
git_diff
fetch_url
search_web
```

### 7.4 Index tools

```text
index_status
index_update
index_rebuild
```

Normal operation should use automatic per-file updates and `index_update`. `index_rebuild` exists for recovery/debugging, not routine use.

## 8. Tool specifications

All output caps, timeouts, and maximum counts are settings-driven. Tool parameters override settings for that call.

### 8.1 `project_overview`

Purpose: return a compact project map.

Parameters:

```json
{
  "max_files": "integer|null",
  "max_symbols": "integer|null",
  "include_tests": "boolean"
}
```

Return data:

```json
{
  "root": "/path/to/project",
  "languages": [{"language":"cpp","files":120,"bytes":1800000}],
  "important_files": [{"path":"src/main.cpp","score":0.92}],
  "entry_points": [{"name":"main","path":"src/main.cpp","line":35}],
  "likely_test_commands": ["make test"],
  "index_fresh": true
}
```

### 8.2 `inspect_code_task`

Purpose: macro-tool that reduces turns by returning likely files, symbols, tests, and suggested reads for a task.

Parameters:

```json
{
  "query": "string",
  "max_symbols": "integer|null",
  "max_files": "integer|null",
  "include_skeletons": "boolean",
  "include_tests": "boolean",
  "max_bytes": "integer|null"
}
```

Return data:

```json
{
  "query": "add agent mode",
  "likely_symbols": [
    {"symbol_id":44,"qualified_name":"ChatSession::run","path":"src/chat.cpp","start_line":80,"end_line":210,"reason":"main chat loop"}
  ],
  "likely_files": ["src/chat.cpp","src/openai.cpp"],
  "suggested_reads": [{"path":"src/chat.cpp","start_line":80,"end_line":210}],
  "likely_tests": ["tests/test_chat.cpp"],
  "truncated": false
}
```

### 8.3 `list_directory`

Purpose: list files and directories.

Parameters:

```json
{
  "path": "string",
  "recursive": "boolean",
  "max_depth": "integer|null",
  "include_hidden": "boolean",
  "include_ignored": "boolean",
  "max_entries": "integer|null"
}
```

Return data:

```json
{
  "path": ".",
  "entries": [
    {"path":"src","type":"directory"},
    {"path":"src/main.cpp","type":"file","size":12345}
  ],
  "truncated": false
}
```

### 8.4 `glob`

Purpose: path discovery by pattern. This is for filenames and paths, not file content.

Parameters:

```json
{
  "pattern": "string",
  "root": "string|null",
  "include_hidden": "boolean",
  "include_ignored": "boolean",
  "max_results": "integer|null"
}
```

Examples:

```json
{"pattern":"**/*test*.cpp","root":"."}
{"pattern":"**/CMakeLists.txt","root":"."}
{"pattern":"src/**/*.{cpp,h}","root":"."}
```

Return data:

```json
{
  "root": ".",
  "pattern": "**/*test*.cpp",
  "matches": ["tests/test_agent.cpp", "tests/test_tools.cpp"],
  "match_count": 2,
  "truncated": false
}
```

Implementation notes:

- Use the C++ filesystem library and simple glob matching.
- Respect `.gitignore` where practical by using `git ls-files` inside git repositories.
- Fall back to filesystem walking outside git repositories.
- Keep this separate from `search_text` to avoid wasting tokens on content search when only paths are needed.

### 8.5 `search_text`

Purpose: search file contents by literal text or regex.

Parameters:

```json
{
  "query": "string",
  "path": "string|null",
  "glob": "string|null",
  "regex": "boolean",
  "case_sensitive": "boolean",
  "whole_word": "boolean",
  "context_lines": "integer",
  "max_hits": "integer|null",
  "max_bytes": "integer|null"
}
```

Return data:

```json
{
  "hits": [
    {"path":"src/main.cpp","line":42,"column":13,"match":"Agent::run","context":"40| ...\n41| ...\n42| ..."}
  ],
  "hit_count": 12,
  "truncated": false
}
```

Implementation notes:

- Use fast internal literal search for non-regex queries.
- Use `std::regex` for regex mode.
- Optionally use `rg` as fallback when available.
- Consider SQLite FTS5 when fast case-insensitive partial DB search is needed.

### 8.6 `grep`

Purpose: compatibility alias for content search. Many LLMs understand `grep`.

Parameters:

```json
{
  "pattern": "string",
  "path": "string|null",
  "glob": "string|null",
  "regex": "boolean",
  "case_sensitive": "boolean",
  "context_lines": "integer",
  "max_hits": "integer|null"
}
```

Return data:

Same shape as `search_text`.

Implementation:

- Dispatch internally to `search_text`.
- Do not call shell `grep` unless explicitly falling back through `run_command`.

### 8.7 `find`

Purpose: simple compatibility alias for literal text search.

Parameters:

```json
{
  "path": "string",
  "search_string": "string",
  "max_hits": "integer|null"
}
```

Return data:

Same shape as `search_text`.

### 8.8 `search_symbol`

Purpose: search the SQLite symbol/code index.

Parameters:

```json
{
  "query": "string",
  "kind": "string|null",
  "language": "string|null",
  "path_glob": "string|null",
  "max_results": "integer|null",
  "include_docs": "boolean",
  "include_call_summary": "boolean"
}
```

Return data:

```json
{
  "results": [
    {
      "symbol_id": 184,
      "score": 0.92,
      "pagerank": 0.81,
      "kind": "method",
      "qualified_name": "Agent::run",
      "parameters": "const AgentTask& task",
      "return_type": "bool",
      "path": "src/agent.cpp",
      "start_line": 42,
      "end_line": 118,
      "doc": "Runs the agent loop.",
      "calls": ["read_file", "edit_file", "run_command"]
    }
  ],
  "index_fresh": true,
  "truncated": false
}
```

Important rule: the scanner/index is a hint, not truth. The agent must verify with `read_symbol`, `get_skeleton`, `read_many`, `read_file`, `search_text`, `glob`, `grep`, or compiler/tests when needed.

### 8.9 `get_skeleton`

Purpose: return signatures, declarations, and doc comments from a file. This is one of the main token-saving tools.

Parameters:

```json
{
  "path": "string",
  "include_private": "boolean",
  "include_line_numbers": "boolean",
  "include_doc_comments": "boolean",
  "include_fields": "boolean",
  "max_items": "integer|null",
  "max_bytes": "integer|null"
}
```

Return data:

```json
{
  "path": "src/agent.cpp",
  "language": "cpp",
  "file_hash": "fnv1a64:...",
  "symbols": [
    {
      "symbol_id": 184,
      "kind": "method",
      "qualified_name": "Agent::run",
      "signature": "bool Agent::run(const AgentTask& task)",
      "line": 42,
      "end_line": 118,
      "doc": "Runs the agent loop until completion or interruption.",
      "doc_source": "comment"
    }
  ],
  "truncated": false
}
```

Fallback rule:

- If the skeleton is missing expected symbols, looks stale, or contradicts the source, immediately fall back to `read_symbol`, `read_file`, `read_many`, `search_text`, `glob`, `grep`, or `run_command`.
- The LLM must not blindly trust skeleton data.

### 8.10 `read_symbol`

Purpose: read an indexed symbol body and nearby context.

Parameters:

```json
{
  "symbol_id": "integer",
  "include_doc": "boolean",
  "include_callers": "boolean",
  "include_callees": "boolean",
  "include_siblings": "boolean",
  "context_lines": "integer",
  "max_bytes": "integer|null"
}
```

Return data:

```json
{
  "symbol": {
    "symbol_id": 184,
    "qualified_name": "Agent::run",
    "kind": "method",
    "path": "src/agent.cpp",
    "start_line": 42,
    "end_line": 118,
    "file_hash": "fnv1a64:...",
    "range_hash": "fnv1a64:...",
    "content": "37| ...\n42| bool Agent::run(...) {\n..."
  },
  "callers": [],
  "callees": [],
  "truncated": false
}
```

If symbol location is stale, reindex the file once and retry. If still inconsistent, return an error and suggest `search_text` or `read_file`.

### 8.11 `read_file`

Purpose: read a whole file or line range.

Parameters:

```json
{
  "path": "string",
  "start_line": "integer|null",
  "end_line": "integer|null",
  "max_bytes": "integer|null",
  "max_lines": "integer|null",
  "include_line_numbers": "boolean",
  "include_hashes": "boolean"
}
```

Return data:

```json
{
  "path": "src/example.cpp",
  "language": "cpp",
  "file_hash": "fnv1a64:...",
  "range": {"start_line": 120, "end_line": 145},
  "range_hash": "fnv1a64:...",
  "content": "120| int f() {\n121| ...\n",
  "line_count": 300,
  "truncated": false
}
```

### 8.12 `read_many`

Purpose: batch several reads into one tool call.

Parameters:

```json
{
  "items": [
    {"path": "string", "start_line": "integer|null", "end_line": "integer|null"}
  ],
  "max_total_bytes": "integer|null",
  "include_line_numbers": "boolean",
  "include_hashes": "boolean"
}
```

Return data:

```json
{
  "items": [
    {
      "path": "src/a.cpp",
      "ok": true,
      "file_hash": "fnv1a64:...",
      "range_hash": "fnv1a64:...",
      "range": {"start_line": 10, "end_line": 80},
      "content": "10| ...",
      "truncated": false
    }
  ],
  "total_bytes": 42000,
  "truncated": false
}
```

Prefer this over repeated `read_file` calls.

### 8.13 `write_file`

Purpose: create or overwrite a file.

Parameters:

```json
{
  "path": "string",
  "content": "string",
  "create_dirs": "boolean",
  "expected_file_hash": "string|null",
  "mode": "overwrite|create_new"
}
```

Return data:

```json
{
  "path": "src/new_file.cpp",
  "bytes_written": 1234,
  "new_file_hash": "fnv1a64:...",
  "created": true,
  "guard": {"decision":"allow","rule_id":null},
  "indexed": true
}
```

Rules:

- Canonicalize path.
- Refuse writes outside workspace root or trusted temp directories.
- If overwriting, record rollback data first.
- Reindex the affected file immediately.

### 8.14 `remove`

Purpose: remove a file or empty directory.

Parameters:

```json
{
  "path": "string",
  "recursive": "boolean",
  "expected_file_hash": "string|null"
}
```

Return data:

```json
{
  "path": "src/old.cpp",
  "removed": true,
  "was_directory": false,
  "guard": {"decision":"allow","rule_id":null},
  "index_updated": true
}
```

Rules:

- Refuse recursive deletion by default unless explicitly requested and guard allows it.
- Ask before deleting database files such as `*.sqlite`, `*.sqlite3`, `*.db`, `*.db3`, `*.duckdb`.
- Refuse deletion outside workspace root or trusted temp directories.
- Record rollback data where practical.

### 8.15 `edit_file`

Purpose: primary structured editing tool.

Parameters:

```json
{
  "path": "string",
  "expected_file_hash": "string|null",
  "ops": [
    {
      "type": "replace_range|insert_at|delete_range|replace_text|replace_symbol|create_file",
      "start_line": "integer|null",
      "end_line": "integer|null",
      "line": "integer|null",
      "symbol_id": "integer|null",
      "expected_hash": "string|null",
      "old_text": "string|null",
      "new_text": "string|null",
      "replacement": "string|null",
      "replace_all": "boolean|null",
      "line_range_hint": {"start_line":"integer", "end_line":"integer"}
    }
  ],
  "atomic": "boolean",
  "create_dirs": "boolean"
}
```

Return data:

```json
{
  "path": "src/agent.cpp",
  "applied": true,
  "operations_applied": 2,
  "old_file_hash": "fnv1a64:...",
  "new_file_hash": "fnv1a64:...",
  "reverse_patch_path": ".pkchat/history/20260719-120000-001.patch",
  "index_updated": true,
  "summary": ["replaced lines 42-118", "inserted before line 7"],
  "warnings": []
}
```

Operations:

- `replace_range`: replace inclusive line range. Preferred normal edit mode.
- `insert_at`: insert before a line.
- `delete_range`: delete inclusive line range.
- `replace_text`: use exact/fuzzy text matching.
- `replace_symbol`: replace indexed symbol body.
- `create_file`: create new file, fail if it exists.

Rules:

- Apply multiple line operations bottom-to-top.
- Use `expected_file_hash` or operation `expected_hash` when available.
- If stale, return current hash and a short current preview.
- For `atomic = true`, leave no partial edits after failure.
- Record rollback data before mutation.
- Reindex only affected files after success.

### 8.16 `str_replace`

Purpose: compatibility editing tool used by many coding agents.

Parameters:

```json
{
  "path": "string",
  "old_text": "string",
  "new_text": "string",
  "expected_file_hash": "string|null",
  "replace_all": "boolean",
  "line_range_hint": {"start_line":"integer", "end_line":"integer"},
  "fuzzy": "boolean"
}
```

Return data:

```json
{
  "path": "src/example.cpp",
  "matches_found": 1,
  "replacements_made": 1,
  "match_mode": "exact|normalized_whitespace|indent_stripped",
  "old_file_hash": "fnv1a64:...",
  "new_file_hash": "fnv1a64:...",
  "reverse_patch_path": ".pkchat/history/20260719-120000-002.patch",
  "index_updated": true
}
```

Fallback order:

1. exact byte-for-byte match
2. normalized whitespace match
3. leading-indent-stripped match
4. fail with useful diagnostics

Multiple-match behavior:

- if `replace_all = true`, replace all matches
- else use `line_range_hint` if supplied
- else fail with `ambiguous_match` and return candidate line numbers

This should use the same engine as `edit_file.replace_text`.

### 8.17 `apply_patch`

Purpose: compatibility tool for OpenAI-style patch edits.

Parameters:

```json
{
  "patch": "string",
  "atomic": "boolean",
  "fuzzy": "boolean"
}
```

Return data:

```json
{
  "applied": true,
  "files_changed": ["src/a.cpp", "src/b.cpp"],
  "operations_applied": 4,
  "new_hashes": {"src/a.cpp":"fnv1a64:...", "src/b.cpp":"fnv1a64:..."},
  "reverse_patch_path": ".pkchat/history/20260719-120000-003.patch",
  "index_updated": true,
  "warnings": []
}
```

Rules:

- Parse add/update/delete patch operations.
- Validate all paths before applying.
- Apply atomically by default.
- Run destructive guard before deleting files.
- Reindex changed files after success.

### 8.18 `run_command`

Purpose: execute build/test/git/shell commands.

Parameters:

```json
{
  "command": "string",
  "cwd": "string|null",
  "timeout_ms": "integer|null",
  "stdin": "string|null",
  "env": "object|null",
  "max_output_bytes": "integer|null"
}
```

Return data:

```json
{
  "command": "make test",
  "cwd": "/path/to/project",
  "exit_code": 2,
  "duration_ms": 1842,
  "stdout": "...",
  "stderr": "...",
  "stdout_truncated": true,
  "stderr_truncated": false,
  "output_summary": "2 compiler errors in src/agent.cpp",
  "guard": {"decision":"allow","rule_id":null}
}
```

Rules:

- Do not use `system()`.
- Use `posix_spawn`/`fork+exec` on Unix-like systems and `CreateProcess` on Windows.
- Kill process groups on timeout.
- Cap stdout/stderr.
- Run the destructive-command guard before execution.
- After commands that may modify files, mark index as possibly stale and cheaply update changed files.

### 8.19 `git_status`

Purpose: compact git status through the git CLI.

Parameters:

```json
{
  "short": "boolean",
  "include_branch": "boolean"
}
```

Return data:

```json
{
  "is_repo": true,
  "branch": "main",
  "dirty": true,
  "files": [{"path":"src/agent.cpp","status":"M"}]
}
```

Typical command:

```text
git status --short --branch
```

### 8.20 `git_diff`

Purpose: bounded git diff through the git CLI.

Parameters:

```json
{
  "path": "string|null",
  "cached": "boolean",
  "stat": "boolean",
  "max_bytes": "integer|null"
}
```

Return data:

```json
{
  "diff": "diff --git ...",
  "stat": "src/agent.cpp | 42 +++++---",
  "truncated": false
}
```

Typical commands:

```text
git diff --stat
git diff -- PATH
git diff --cached
```

Do not add libgit2 for v1.

### 8.21 `fetch_url`

Purpose: fetch a URL using libcurl.

Parameters:

```json
{
  "url": "string",
  "method": "GET|HEAD",
  "headers": {"Header-Name":"value"},
  "max_bytes": "integer|null",
  "timeout_ms": "integer|null",
  "follow_redirects": "boolean",
  "extract_text": "boolean",
  "include_headers": "boolean"
}
```

Return data:

```json
{
  "url": "https://example.com/page",
  "final_url": "https://example.com/page",
  "status": 200,
  "content_type": "text/html; charset=utf-8",
  "title": "Example page",
  "text": "Extracted readable text...",
  "body": null,
  "headers": {},
  "bytes_read": 12345,
  "truncated": false
}
```

Rules:

- Allow `http` and `https` by default.
- Disable `file`, `ftp`, and unusual schemes unless explicitly enabled.
- Bound bytes and timeouts.
- Use simple dependency-free HTML text extraction.

### 8.22 `search_web`

Purpose: web search through a configured provider.

Parameters:

```json
{
  "term": "string",
  "max_results": "integer|null",
  "timeout_ms": "integer|null",
  "site": "string|null",
  "freshness_days": "integer|null",
  "fetch_top_results": "boolean"
}
```

Return data:

```json
{
  "term": "sqlite fts5 trigram tokenizer",
  "results": [
    {"title":"Result title","url":"https://example.com/result","snippet":"Short snippet","rank":1,"fetched_text":null}
  ],
  "provider": "configured",
  "truncated": false
}
```

Rules:

- Do not hardcode one provider.
- Support a configurable provider such as SearXNG, Brave, Bing, Google Programmable Search, or a generic JSON endpoint.
- If no provider is configured, return `web_search_unavailable`.

### 8.23 `index_status`

Purpose: report index state.

Parameters:

```json
{
  "check_filesystem": "boolean",
  "max_changed_files": "integer|null"
}
```

Return data:

```json
{
  "index_exists": true,
  "fresh": true,
  "files_indexed": 220,
  "symbols_indexed": 6400,
  "refs_indexed": 18000,
  "changed_files": [],
  "last_updated": 1783170000
}
```

### 8.24 `index_update`

Purpose: update changed files only.

Parameters:

```json
{
  "paths": ["string"],
  "force": "boolean"
}
```

Return data:

```json
{
  "files_checked": 220,
  "files_updated": 3,
  "files_removed": 1,
  "duration_ms": 87,
  "errors": []
}
```

### 8.25 `index_rebuild`

Purpose: full rebuild for recovery/debugging.

Parameters:

```json
{
  "confirm": "boolean"
}
```

Return data:

```json
{
  "rebuilt": true,
  "files_indexed": 220,
  "symbols_indexed": 6400,
  "refs_indexed": 18000,
  "duration_ms": 940
}
```

Normal users should rarely need this.

## 9. FTS5 use

Use SQLite FTS5 for symbol/document search when available. For fast case-insensitive partial search inside the DB, consider an FTS5 trigram table for paths, symbol names, and compact text fragments.

Example:

```sql
CREATE VIRTUAL TABLE IF NOT EXISTS code_search_fts USING fts5(
    path,
    symbol_name,
    text,
    tokenize='trigram'
);
```

If FTS5 or the trigram tokenizer is unavailable on the linked SQLite build, fall back to indexed `LIKE`, path/name filters, internal C++ search, and `run_command` with `rg`/`grep` where available.

## 10. Parallel call handling

The scheduler should accept multiple tool calls from the model and execute safe independent calls concurrently.

### 10.1 Tool safety classes

```text
read_only_parallel_safe
network_parallel_limited
index_read_parallel_safe
index_write_serialized
file_mutation_serialized_by_path
workspace_mutation_serialized
command_guarded_maybe_mutating
always_serial
```

Recommended classification:

```text
read_only_parallel_safe:
  read_file, read_many, get_skeleton, list_directory, glob, search_text, grep, find,
  search_symbol, read_symbol, find_callers, find_callees, find_tests, project_overview,
  git_status, git_diff

network_parallel_limited:
  fetch_url, search_web

file_mutation_serialized_by_path:
  write_file, remove, edit_file, str_replace

workspace_mutation_serialized:
  apply_patch

index_write_serialized:
  index_update, index_rebuild

command_guarded_maybe_mutating:
  run_command
```

### 10.2 Lock rules

Use simple lock scopes:

```text
workspace read lock
workspace mutation lock
file path mutation lock
index write lock
network concurrency token
```

Rules:

- Multiple reads may run in parallel.
- Multiple searches may run in parallel.
- Multiple network tools may run in parallel up to a configured cap.
- Mutating tools for different files may run in parallel only if they do not share index/rollback conflicts. For v1, it is simpler to serialize all file mutations.
- Never mutate the same file in parallel.
- Serialize `apply_patch`.
- Serialize destructive or maybe-mutating `run_command` with file edits.
- If two model-emitted tool calls conflict, run them in safe order rather than failing.

### 10.3 `run_command` parallel policy

Classify commands before scheduling:

```text
safe_read_command:
  git status, git diff, git log, git show, pwd, ls, rg, grep, find without -delete

maybe_mutating_command:
  make, ninja, cmake --build, cargo test, go test, npm test, pytest, compiler commands

high_risk_command:
  anything caught by the destructive-command guard
```

Safe read commands can run in parallel with other reads. Maybe-mutating commands should not run in parallel with edits. High-risk commands must wait for user approval or be blocked.

## 11. Destructive-command guard

Add a small built-in guard inspired by destructive-command protection tools, but keep it simple and cheap. It should use command normalization, path canonicalization, and regex/pattern rules.

This is not a full sandbox. It is a fast layer against common irreversible mistakes.

### 11.1 Guard applies to

```text
run_command
write_file
remove
edit_file
str_replace
apply_patch
```

It also applies to any future database execution tool.

### 11.2 Guard decision shape

```json
{
  "decision": "allow|ask|deny",
  "rule_id": "PKCHAT_GUARD_RM_RF",
  "severity": "low|medium|high|critical",
  "reason": "Recursive force delete requires approval.",
  "safe_alternative": "Use trash, move to .pkchat/tmp, or run git clean -n first."
}
```

### 11.3 Guard control flow

```text
1. canonicalize cwd and paths
2. normalize command spelling and wrappers
3. apply explicit user/project blocks
4. apply explicit safe allow rules
5. run keyword gate for cheap high-risk detection
6. apply destructive-pattern rules
7. if dangerous: ask or deny
8. if indeterminate and high-risk: ask
9. otherwise allow
```

### 11.4 Path rules

- File writes/deletes must stay inside the workspace root or trusted temp directories.
- Canonicalize paths before decision.
- Handle symlinks and `..` conservatively.
- Treat mixed Windows/POSIX separators carefully.
- Reject or ask on path analysis failure for mutating operations.

Trusted temp directories:

```text
POSIX:
  $TMPDIR when set
  /tmp
  /var/tmp
  workspace/.pkchat/tmp

Windows:
  %TEMP%
  %TMP%
  GetTempPath result
  workspace\.pkchat\tmp
```

### 11.5 Commands requiring approval

Recursive force delete:

```text
rm -rf PATH
rm -fr PATH
rm -r -f PATH
rm --recursive --force PATH
find PATH -delete
xargs rm -rf
shred PATH
dd ... of=PATH
truncate -s 0 PATH
```

Destructive git:

```text
git reset --hard
git reset --merge
git clean -f
git clean -fd
git clean -fdx
git checkout -- PATH
git restore PATH          # except safe staged-only forms
git push --force
git push -f
git stash drop
git stash clear
git branch -D NAME
```

Destructive SQL/database operations:

```text
DROP DATABASE
DROP SCHEMA
DROP TABLE
TRUNCATE
DELETE FROM table          # without WHERE
sqlite3 db.sqlite "DROP TABLE ..."
psql -c "DROP TABLE ..."
mysql -e "DROP TABLE ..."
```

Database file deletion:

```text
*.sqlite
*.sqlite3
*.db
*.db3
*.duckdb
```

Workspace escape:

```text
writes outside workspace root
edits outside workspace root
deletes outside workspace root
redirection to outside path
cp/mv/install/touch/mkdir outside allowed roots
```

### 11.6 Safe commands that should not be blocked

```text
git status
git diff
git log
git show
git add
git commit
git fetch
git pull
git push without force
git clean -n
git restore --staged PATH
rg PATTERN
grep PATTERN
find PATH -name PATTERN
ls
pwd
```

### 11.7 Shell normalization

The guard should recognize common wrappers and variants:

```text
/usr/bin/git status
env git status
command git status
bash -c 'rm -rf build'
sh -c "git reset --hard"
python -c "... destructive code ..."
node -e "... destructive code ..."
```

For `bash -c`, `sh -c`, `python -c`, `node -e`, heredocs, and piped scripts, either analyze the embedded text or conservatively ask if high-risk keywords are present.

### 11.8 User approval

Humans own exceptions.

Rules:

- The agent may request approval.
- The agent must not approve its own request.
- The agent must not disable guard rules.
- Approvals should be one-shot by default.
- When blocked, the agent must replan instead of retrying the same blocked command.

Good alternatives to suggest:

```text
git diff
git stash push
git clean -n
git push --force-with-lease
move files to .pkchat/tmp
make a backup
run SELECT before DELETE
run schema dump before DROP
```

## 12. Prompt and `AGENTS.md` handling

### 12.1 Built-in prompts

Keep internal prompts short and task-specific.

Suggested files:

```text
resources/agents/AGENTS.base.md
resources/agents/AGENTS.coding.md
resources/agents/AGENTS.debug.md
resources/agents/AGENTS.review.md
resources/agents/AGENTS.refactor.md
resources/agents/AGENTS.tests.md
```

Load only the base prompt plus at most one task-specific prompt. Avoid huge system prompts.

### 12.2 Project `AGENTS.md`

Load project rules from `AGENTS.md` files.

Simple v1 rule:

1. Load `AGENTS.md` from the workspace root if it exists.
2. When editing or reading a specific file, also load the nearest `AGENTS.md` between the workspace root and that file's directory.
3. More specific `AGENTS.md` rules override broader project rules when they conflict.
4. Cache loaded files by `file_hash`.
5. Re-read if hash changes.

Example:

```text
workspace/AGENTS.md
workspace/src/AGENTS.md
workspace/src/ui/AGENTS.md
```

For `src/ui/button.cpp`, applicable order is:

```text
workspace/AGENTS.md
workspace/src/AGENTS.md
workspace/src/ui/AGENTS.md
```

### 12.3 Instruction precedence

Recommended precedence:

```text
system/developer safety rules
pkchat built-in agent rules
user current request
project AGENTS.md rules
local nearest AGENTS.md rules
model-generated plan
```

Important: `AGENTS.md` must not be allowed to disable the destructive-command guard, change the workspace root, exfiltrate secrets, or override the user's direct request.

### 12.4 Minimum built-in instruction

The built-in agent prompt should include this behavior:

```text
Use the local index first because it is cheap, but treat it as a hint, not truth. The scanner can miss symbols, embedded code, macro-generated code, overloaded functions, dynamic calls, and unusual syntax. If get_skeleton or search_symbol looks incomplete, stale, or contradictory, fall back to read_symbol, read_many, read_file, search_text, glob, grep, git grep, compiler output, or tests.
```

## 13. Preferred agent tool order

For context gathering:

```text
1. inspect_code_task(query)
2. search_symbol(query)
3. get_skeleton(path)
4. read_symbol(symbol_id)
5. read_many([...])
6. targeted read_file(path, start_line, end_line)
7. glob(pattern)
8. search_text / grep / find
9. run_command with rg/git grep/compiler/test fallback
10. fetch_url/search_web only when external current information is needed
```

For editing:

```text
1. edit_file.replace_range with expected_hash
2. edit_file.replace_symbol with symbol_id and expected_hash
3. edit_file.replace_text / str_replace with fuzzy fallback
4. apply_patch for patch-style multi-file edits
5. write_file only for new files or intentional full rewrites
```

For verification:

```text
1. narrow unit test for changed area
2. compiler/build command for touched component
3. relevant integration test
4. formatter/linter only if already part of project workflow
5. git diff/status summary
```

## 14. Slash commands

Ship these in the first serious agent version:

```text
/help
/quit
/exit
/settings
/settings show
/settings set KEY VALUE
/settings unset KEY
/model
/tools
/new
/read PATH
/plan
/compact
/compact auto
/compact off
/status
/diff
/undo
/memory
/index status
/index update
/index rebuild
/index explain PATH
/guard status
/guard explain COMMAND
/resume
/clear
```

`/guard explain COMMAND` should show whether a command would be allowed, asked, or denied and why, without running it.

`/tools` should display available tools, aliases, and whether web/search tools are configured.

`/status` should show:

```text
workspace root
git branch
dirty files
current model
context usage estimate
index freshness
last test result
guard enabled/disabled status
loaded AGENTS.md files
```

Ctrl-C behavior:

```text
First Ctrl-C: interrupt current model stream or running command.
Second Ctrl-C: abort current agent task and return to pkchat prompt.
```

## 15. Memory, compaction, and rollback

### 15.1 Session log

Use append-only `.pkchat/session.jsonl` as the source of truth.

Example events:

```json
{"type":"user_goal","text":"Add agent mode"}
{"type":"tool_call","name":"search_symbol","args":{"query":"agent loop"}}
{"type":"edit","files":["src/agent.cpp"],"reverse_patch":".pkchat/history/001.patch"}
{"type":"test","command":"make test","exit_code":0}
{"type":"decision","text":"Use replace_range as primary edit primitive"}
```

### 15.2 Generated memory

Generate `.pkchat/memory.md` from the event log.

Suggested shape:

```markdown
# Project Memory

## Current goal
Implement agent mode for pkchat.

## Decisions
- Use file-level hashes as the primary edit-safety mechanism.
- Use range replacement as the primary edit primitive.
- Use git CLI instead of libgit2.

## Modified files
- src/agent/agent_loop.cpp: added first agent loop.
- src/agent/tools_edit.cpp: added range edit engine.

## Last verification
- `make test` failed with 2 errors in src/agent/tools_edit.cpp.

## Next likely actions
- Fix edit tool tests.
- Re-run narrow tests.
```

Update memory after meaningful milestones, not after every tiny tool call.

### 15.3 Compaction

Compaction must preserve:

```text
original user goal
current plan
completed steps
files edited
file hashes for active files
exact test commands and results
important errors
design decisions
open TODOs
loaded AGENTS.md files
current git diff summary
```

Compaction should discard:

```text
old tool chatter
large file dumps no longer needed
repeated compiler output
superseded plans
failed search paths that do not matter
```

### 15.4 Rollback

Before each edit batch, store rollback data:

```text
.pkchat/history/YYYYMMDD-HHMMSS-NNN.patch
```

Successful edit results should include:

```text
files changed
operation count
old and new file hashes
reverse patch path
index update status
```

`/undo` should apply the last pkchat-created reverse patch or restore saved file snapshots.

## 16. Implementation milestones

### Milestone 1: minimal agent loop and tool registry

Files:

```text
src/agent/agent_loop.*
src/agent/tool_call.*
src/agent/tool_registry.*
src/agent/tool_scheduler.*
```

Tasks:

- Add provider-neutral `ToolCall` and `ToolResult`.
- Add tool registry with name, schema, handler, and safety class.
- Add agent loop that can receive model tool calls, execute tools, and return results.
- Add text fallback parser for weak tool-calling models.

Verification:

```text
./pkchat --agent --tools-selftest
```

Expected output:

```text
agent tools self-test: OK
```

### Milestone 2: file, path, and search tools

Files:

```text
src/agent/tools_files.*
src/agent/tools_search.*
src/agent/hash.*
```

Tasks:

- Implement `read_file`, `read_many`, `write_file`, `remove`.
- Implement `list_directory`, `glob`, `search_text`, `grep`, `find`.
- Add file-level hashing.
- Add path canonicalization and workspace-root checks.

Tests:

```text
tests/agent/test_file_tools.cpp
tests/agent/test_tool_search.cpp
```

Verification:

```text
ctest -R "agent_file|agent_search" --output-on-failure
```

Expected output:

```text
100% tests passed
```

### Milestone 3: edit engine

Files:

```text
src/agent/tools_edit.*
```

Tasks:

- Implement `edit_file.replace_range`.
- Implement `edit_file.insert_at` and `delete_range`.
- Implement `edit_file.replace_text`.
- Implement `str_replace` compatibility wrapper.
- Implement fuzzy fallback: exact, normalized whitespace, indent stripped.
- Implement `apply_patch` compatibility parser.
- Add rollback patch creation.
- Reindex touched files after successful edits.

Tests:

```text
tests/agent/test_edit_tools.cpp
tests/agent/test_str_replace_fuzzy.cpp
tests/agent/test_apply_patch.cpp
```

Verification:

```text
ctest -R "agent_edit|agent_patch|agent_fuzzy" --output-on-failure
```

Expected output:

```text
100% tests passed
```

### Milestone 4: command execution and guard

Files:

```text
src/agent/tools_command.*
src/agent/git_tools.*
src/agent/command_guard.*
```

Tasks:

- Implement `run_command` without `system()`.
- Implement timeout and process-group kill.
- Implement bounded stdout/stderr.
- Implement `git_status` and `git_diff` through git CLI.
- Implement destructive-command guard.
- Add `/guard status` and `/guard explain COMMAND`.

Tests:

```text
tests/agent/test_command_guard.cpp
tests/agent/test_tool_scheduler.cpp
```

Guard test cases must include:

```text
rm -rf build
rm -fr build
rm -r -f build
git reset --hard
git clean -fdx
git push --force
sqlite3 app.sqlite "DROP TABLE users;"
sqlite3 app.sqlite "DELETE FROM users;"
find . -delete
rm app.sqlite
write outside workspace
write to /tmp/pkchat-test-file
```

Verification:

```text
ctest -R "agent_guard|agent_command" --output-on-failure
```

Expected output:

```text
100% tests passed
```

### Milestone 5: web tools

Files:

```text
src/agent/tools_web.*
```

Tasks:

- Implement `fetch_url` with libcurl.
- Implement simple HTML text extraction.
- Implement `search_web` through configured provider.
- Return `web_search_unavailable` if not configured.

Tests:

```text
tests/agent/test_web_tools.cpp
```

Test failure cases:

```text
invalid URL
unsupported scheme
network timeout
HTTP 404
large response truncation
provider not configured
```

Verification:

```text
ctest -R "agent_web" --output-on-failure
```

Expected output:

```text
100% tests passed
```

### Milestone 6: prompts, AGENTS.md, memory, and compaction

Files:

```text
src/agent/agent_prompts.*
src/agent/agents_md.*
src/agent/agent_memory.*
src/agent/context_compact.*
resources/agents/AGENTS.base.md
resources/agents/AGENTS.coding.md
resources/agents/AGENTS.debug.md
resources/agents/AGENTS.review.md
resources/agents/AGENTS.refactor.md
resources/agents/AGENTS.tests.md
```

Tasks:

- Load short built-in prompts.
- Load root and nearest `AGENTS.md` files.
- Enforce instruction precedence.
- Generate `.pkchat/session.jsonl`.
- Generate `.pkchat/memory.md`.
- Implement `/compact`, `/compact auto`, and `/compact off`.

Tests:

```text
tests/agent/test_agents_md.cpp
tests/agent/test_memory.cpp
```

Verification:

```text
ctest -R "agent_agents|agent_memory|agent_compact" --output-on-failure
```

Expected output:

```text
100% tests passed
```

## 17. Test requirements

Add normal and failure cases. At minimum:

```text
Unicode: ÄÖ, Chinese, Arabic, Russian, emoji
empty strings
very long strings
zero and small positive/negative numbers
very large numbers
invalid numeric input
invalid/misspelled URL
corrupted file contents
binary file detection
permission denied on read/write
network timeout/unavailable
ambiguous str_replace matches
stale file hash
stale range hash
recursive delete blocked/asked
destructive git blocked/asked
destructive SQL blocked/asked
workspace escape blocked
parallel read calls succeed
parallel same-file edits serialize
```

Do not keep rerunning the same failing tool blindly. If a tool fails for a non-permission and non-network reason, use a fallback route.

## 18. Agent benchmark metrics

Extend benchmark mode to track:

```text
model_turns
tool_calls
parallel_tool_groups
input_tokens
output_tokens
wall_time_ms
local_tool_time_ms
model_time_ms
edit_attempts
edit_failures
fuzzy_edit_successes
test_runs
first_test_pass_rate
final_diff_lines
guard_blocks
guard_approval_requests
index_updates_after_edits
```

Most important metrics:

```text
model turns per completed task
edit failure rate
tokens to first correct patch
first-test-pass rate
search/read token volume
number of blocked destructive actions
```

## 19. Summary of changes to merge

Merge these agent-focused updates into the current plan:

- Add `glob` as a first-class path-discovery tool.
- Add `grep` as a compatibility alias for `search_text`.
- Keep file-level hashes as primary freshness/edit-safety mechanism.
- Postpone line-based hashing/hashline anchors.
- Keep SQLite code index as a pkchat differentiator, but treat index results as hints.
- Prefer `get_skeleton` before full-file reads, but require fallback when skeleton/index data is wrong.
- Keep `edit_file.replace_range` as the primary edit operation.
- Keep `str_replace` and `apply_patch` for compatibility.
- Add fuzzy fallback for text replacement.
- Add parallel scheduling for independent reads/searches and serialization for mutations.
- Use git through command-line tools, not libgit2.
- Add `fetch_url` and `search_web` with configurable provider and bounded output.
- Add DCG-inspired destructive-command guard using simple normalization and regex/pattern rules.
- Forbid or ask before recursive force delete, destructive git, destructive SQL, database-file deletion, and workspace-escape writes/deletes.
- Load concise built-in agent prompts plus project `AGENTS.md` files.
- Keep memory as append-only `session.jsonl` plus generated `memory.md`.
- Add tests for guard rules, fuzzy edits, hashes, parallel scheduling, and AGENTS.md precedence.
