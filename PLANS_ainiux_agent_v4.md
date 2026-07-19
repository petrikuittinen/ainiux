# PLANS_ainiux_agent_v4.md — ainiux agent mode and project-local code index

This document is the single merged plan for ainiux local agent mode. It supersedes the draft files:

- `PLANS_ainiux_agent_v2.md` (index/scanner/ranking depth)
- `PLANS_agent_part_update_v3.md` (agent runtime, guard, parallel tools, AGENTS.md)

**Basis:** v3 structure and agent runtime.  
**Restored from v2:** project-local SQLite code index, scanners, PageRank, full tool specs for graph tools, fuzzy-edit depth, preferred strategies.  
**Product name:** ainiux throughout (not pkchat).  
**Intended merge target:** later fold into main `PLANS.md` under v1.0 agent mode when implementation starts.

---

## 1. Scope

Implement a fast, dependency-light coding agent in ainiux using the existing C++17 codebase, libcurl, and SQLite3.

Entry point (separate from ordinary chat):

```sh
ainiux agent [options]
```

Agent mode must never silently gain filesystem, shell, or network powers inside normal chat, REPL, TUI, or editor AI assist.

This plan covers:

- agent loop and provider-neutral tool calls
- project-local `.ainiux/` store including **`index.sqlite`**
- SQLite code index, incremental updates, C++17 scanners, ranking
- file/search/edit/web/command/git tools (`glob`, `grep`, and other compatibility aliases)
- file-level content hashes as primary freshness/edit safety
- parallel tool scheduling rules
- DCG-inspired destructive-command guard
- prompt loading and project `AGENTS.md` rules
- rollback, `/undo`, memory, and compaction
- tests, verification, and agent benchmark metrics

### 1.1 Architecture fit (reuse, do not reimplement)

Route long-running work through existing layers:

| Concern | Existing home | Agent rule |
| --- | --- | --- |
| HTTP / SSE | `src/http/` | No second HTTP stack |
| Provider adapters | `src/provider/` | Tool call formats convert at adapter boundary |
| Jobs / cancel | `src/runtime/` | UI/agent loop must not block on network or long I/O |
| URL fetch safety | `src/fetch/` | `fetch_url` reuses private/loopback/metadata policy |
| Web search | `src/search/` | `search_web` reuses configured providers and fallbacks |
| Credential redaction | `src/security/` | Never log API keys or secrets in tool results or session logs |
| Mode dispatch | `src/main.cpp` → `src/app/` | New agent runner under `src/app/` + `src/agent/` |

Central chat library:

```text
~/.ainiux/ainiux.db
```

This database is **only** for the TUI chat library (and related app state). It must **not** hold project code indexes, agent session logs, or per-project edit history.

### 1.2 Default safety posture (accepted choice A)

Practical coding-agent defaults for the first serious version:

- Ordinary **reads, searches, skeletons, and index lookups** inside the workspace: allowed without prompting.
- Ordinary **writes and structured edits** inside the workspace: allowed without per-file prompting; always log, hash-check when possible, and record rollback data.
- **Destructive / high-risk** operations: ask or deny via the guard (recursive force delete, destructive git, destructive SQL, database file deletion, workspace escape, indeterminate high-risk wrappers).
- **Web tools** (`fetch_url`, `search_web`): allowed when enabled in settings; still bound by existing fetch/search safety (size, timeout, scheme, private URL policy).
- **Commands** (`run_command`): allowed when not caught as high-risk; maybe-mutating commands (build/test) run without a constant ask, but stay serialized with file mutations.
- Do **not** prompt constantly. Ask only for clearly destructive or indeterminate high-risk actions.
- Fuller sandbox levels (`--sandbox none|basic|strict`, default read-only product posture from older roadmap notes) remain a later hardening track; document them, but do not block the first useful agent on perfect sandboxing.

The agent may request approval. The agent must never approve its own request, disable guard rules, or override the user's direct safety intent.

---

## 2. Principles

1. Keep It Simple Stupid.
2. Prefer C++17 standard library and existing dependencies (libcurl, SQLite3).
3. Do not add Tree-sitter, libgit2, MCP, plugins, subagents, or heavyweight frameworks in the first serious version.
4. Optimize for fewer model turns, not only faster local function calls.
5. Prefer batched reads, compact skeletons, indexed symbol lookup, and bounded tool output.
6. Prefer deterministic local tools over model guessing whenever possible.
7. Use common tool names that most LLMs already understand.
8. Use file-level content hashes as the primary freshness and edit-safety mechanism.
9. Keep line-level/hashline anchors as possible future work.
10. Serialize mutating tools that touch the same file or workspace state.
11. Let safe read-only tools run in parallel when possible.
12. Guard destructive commands cheaply with normalization and regex/pattern matching before execution.
13. Treat the SQLite code index as a **fast hint source**, not ground truth.
14. Store tool limits in settings, not as hardcoded constants.
15. Rare or difficult edge cases fall back to slower shell tools (`rg`, `find`, compiler output, `git`).
16. No memory leaks: every tool path, cancel path, and index transaction must release resources.

### 2.1 Non-goals for the first implementation

Deferred:

- Tree-sitter integration
- libgit2 integration
- Full sandboxing / container isolation
- Perfect static analysis
- Perfect C++ parsing
- Full semantic call graph resolution
- MCP support
- SKILLS.md plugin ecosystem
- Heavy permission prompting before every tool call
- Native Anthropic Messages-only tool path as a separate product (provider adapters still translate formats)
- Autonomous multi-agent orchestration

The first version should be pragmatic, fast, and useful on ordinary projects.

---

## 3. Project-local storage layout

Use a hidden **project-local** directory under the workspace root:

```text
.ainiux/
  index.sqlite
  settings.json
  session.jsonl
  memory.md
  history/
  tmp/
  plans/
```

Purposes:

| Path | Purpose |
| --- | --- |
| `index.sqlite` | **Project code index** (WAL). Portable with the project tree when copied to another machine. |
| `settings.json` | Project-local agent limits and agent preferences. |
| `session.jsonl` | Append-only event log of the agent session (source of truth for history). |
| `memory.md` | Generated summary of important project/session facts (not sole source of truth). |
| `history/` | Reverse patches, edit snapshots, rollback data. |
| `tmp/` | Trusted workspace temp (guard-allowed temp moves). |
| `plans/` | Saved plans for long-running tasks. |

### 3.1 Hard boundary: no central DB for project index

```text
~/.ainiux/ainiux.db     # TUI chat library ONLY
.ainiux/index.sqlite    # THIS project's code index
```

Reasons:

- The index must travel with the project when the tree is copied or shared.
- Different projects must not share or pollute one another’s symbol graphs.
- Central DB lifecycle (chat threads, media, app state) must stay independent of indexing.

If `index.sqlite` is missing, create it on first agent/index use inside the workspace. If the workspace is not writable, fail with a clear error and do not fall back to `~/.ainiux/`.

### 3.2 Settings and configurable limits

Do not hardcode practical limits (max read size, search hits, command timeout, network timeout, fuzzy thresholds, output caps) inside tool implementations.

Resolution order (most specific wins):

```text
compiled safe defaults
user-global agent settings
project-local .ainiux/settings.json
session overrides from /settings
single tool-call parameter
```

User-global agent settings path (accepted: JSON for agent limits, separate from main TOML-alike `config.conf` for now):

```text
$XDG_CONFIG_HOME/ainiux/agent-settings.json
fallback: ~/.config/ainiux/agent-settings.json
```

Project-local:

```text
.ainiux/settings.json
```

Main ainiux `config.conf` continues to own ordinary product settings (providers, themes, benchmarks). Agent tool limits live in the JSON stack above unless later explicitly mirrored into `config.conf`.

Example settings shape:

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
    "project_overview": {"max_files": 30, "max_symbols": 30},
    "list_directory": {"max_entries": 300},
    "git_diff": {"max_bytes": 50000},
    "run_command": {"timeout_ms": 120000, "max_output_bytes": 12000},
    "fetch_url": {"timeout_ms": 30000, "max_bytes": 200000},
    "search_web": {"max_results": 10, "timeout_ms": 30000}
  },
  "hash": {
    "file_hash_algorithm": "fnv1a64",
    "include_size_in_fingerprint": true
  },
  "index": {
    "sqlite_cache_size_kb": 200000,
    "sqlite_mmap_size": 268435456,
    "use_fts5": true,
    "use_fts5_trigram_for_partial_search": true
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
    "user_agent": "ainiux/agent"
  },
  "network": {
    "enabled": true
  },
  "agents_md": {
    "enabled": true,
    "load_root": true,
    "load_nearest_for_files": true,
    "max_bytes_total": 20000
  }
}
```

Numeric values are suggested defaults only. Implementation must read them from settings.

When output is truncated, return:

- `truncated = true`
- which setting or tool parameter caused truncation
- how much was omitted if known
- the most useful head/tail portions where appropriate

For command output, prefer compiler/test error extraction over dumping massive logs.

---

## 4. File hashes and freshness

Use file-level content hashes as the primary safety and freshness mechanism.

Recommended first version:

- store `size`, `mtime_ns`, and `file_hash`
- skip hashing if size and timestamp are unchanged
- hash only changed files
- reindex only touched files after ainiux edits
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

---

## 5. SQLite index design

Store the index at:

```text
.ainiux/index.sqlite
```

Use SQLite3 with WAL mode enabled.

Recommended pragmas (cache/mmap sizes configurable via settings):

```sql
PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
PRAGMA temp_store=MEMORY;
PRAGMA cache_size=-200000;
PRAGMA mmap_size=268435456;
```

### 5.1 Files table

```sql
CREATE TABLE IF NOT EXISTS files (
    id INTEGER PRIMARY KEY,
    path TEXT NOT NULL UNIQUE,
    language TEXT NOT NULL,
    size INTEGER NOT NULL,
    mtime_ns INTEGER NOT NULL,
    hash TEXT NOT NULL,
    indexed_at INTEGER NOT NULL,
    ignored INTEGER NOT NULL DEFAULT 0,
    last_error TEXT
);
```

### 5.2 Symbols table

```sql
CREATE TABLE IF NOT EXISTS symbols (
    id INTEGER PRIMARY KEY,
    file_id INTEGER NOT NULL REFERENCES files(id),
    language TEXT NOT NULL,
    kind TEXT NOT NULL,
    name TEXT NOT NULL,
    qualified_name TEXT NOT NULL,
    parameters TEXT,
    return_type TEXT,
    start_line INTEGER NOT NULL,
    end_line INTEGER NOT NULL,
    signature_hash TEXT NOT NULL,
    body_hash TEXT,
    doc TEXT,
    doc_source TEXT NOT NULL,
    pagerank REAL NOT NULL DEFAULT 0.0
);
```

`kind` examples:

```text
function
method
class
struct
enum
interface
module
namespace
constructor
destructor
macro
constant
variable
```

`doc_source` examples:

```text
comment
docstring
generated
heuristic
none
```

The first implementation must not run LLM summarization during full indexing. Use existing comments and cheap heuristics only. Optional model-generated summaries can be created lazily later and cached by `body_hash`.

### 5.3 References table

```sql
CREATE TABLE IF NOT EXISTS refs (
    id INTEGER PRIMARY KEY,
    file_id INTEGER NOT NULL REFERENCES files(id),
    from_symbol_id INTEGER REFERENCES symbols(id),
    target_symbol_id INTEGER REFERENCES symbols(id),
    target_name TEXT NOT NULL,
    kind TEXT NOT NULL,
    line INTEGER NOT NULL,
    confidence REAL NOT NULL
);
```

`kind` examples:

```text
call
import
include
inherit
instantiate
macro
use
```

Treat the call graph as probabilistic. Store confidence values rather than pretending every reference is perfect.

Example confidence levels:

```text
1.00 exact resolved symbol
0.80 same file unique matching name
0.70 imported or included name
0.50 likely class or object method call
0.30 lexical call-like token
```

### 5.4 FTS tables

Use SQLite FTS5 when available:

```sql
CREATE VIRTUAL TABLE IF NOT EXISTS symbols_fts USING fts5(
    qualified_name,
    name,
    parameters,
    doc,
    content='symbols',
    content_rowid='id'
);
```

When fast case-insensitive partial search is needed inside the DB, consider an additional FTS5 trigram table:

```sql
CREATE VIRTUAL TABLE IF NOT EXISTS code_search_fts USING fts5(
    path,
    symbol_name,
    text,
    tokenize='trigram'
);
```

Use FTS5 only if the linked SQLite build supports it and the required tokenizer. If unavailable, fall back to indexed `LIKE`, path/name filters, and the internal C++ scanner / `search_text`.

### 5.5 Useful indexes

```sql
CREATE INDEX IF NOT EXISTS idx_files_path ON files(path);
CREATE INDEX IF NOT EXISTS idx_files_hash ON files(hash);
CREATE INDEX IF NOT EXISTS idx_symbols_file ON symbols(file_id);
CREATE INDEX IF NOT EXISTS idx_symbols_name ON symbols(name);
CREATE INDEX IF NOT EXISTS idx_symbols_qname ON symbols(qualified_name);
CREATE INDEX IF NOT EXISTS idx_symbols_kind ON symbols(kind);
CREATE INDEX IF NOT EXISTS idx_refs_from ON refs(from_symbol_id);
CREATE INDEX IF NOT EXISTS idx_refs_target ON refs(target_symbol_id);
CREATE INDEX IF NOT EXISTS idx_refs_name ON refs(target_name);
```

---

## 6. Index update strategy

Update the index automatically for files ainiux edits. Manual full rebuild exists for recovery; normal users should rarely need it.

### 6.1 Fast change detection

For each file, track path, size, mtime (highest available precision), and content hash.

Normal update path:

1. Compare stored size and timestamp.
2. If unchanged, skip hashing.
3. If changed, compute hash.
4. If hash is unchanged, update metadata only if needed.
5. If hash changed, rescan only that file.
6. Delete and replace symbols/refs for that file in one transaction.
7. Refresh FTS rows for changed symbols.

For files edited through ainiux, reindex those files immediately.

For files changed externally, detect changes during:

- agent startup
- `/status`
- `/index status`
- before `search_symbol`
- before `read_symbol`
- after `run_command` if the command may have generated or modified source files

The check should be cheap. Ordinary projects should get sub-second incremental updates.

### 6.2 Rebuild and status commands

```text
/index status
/index update
/index rebuild
/index explain PATH
```

Expected behavior:

- `/index status`: freshness, file count, symbol count, changed files
- `/index update`: update changed files only
- `/index rebuild`: delete and rebuild the entire project index
- `/index explain PATH`: why a file is indexed, ignored, or failed

Corresponding tools: `index_status`, `index_update`, `index_rebuild`.

---

## 7. C++17 pattern matching scanner

The first scanner uses fast C++17 code with regular expressions and lightweight lexical scanning. The goal is the common 80%+ of symbols and references, not perfect parsing.

Priority languages and file types:

```text
C
C++
C#
Python
JavaScript
TypeScript
Java
PHP
Perl
HTML5
CSS
Rust
Go
GNU-style assembler
```

HTML5 indexing should detect embedded JavaScript inside `<script>` blocks and, when practical, embedded CSS inside `<style>` blocks. GNU assembler assumes GAS-style syntax and common extensions (`.s`, `.S`, `.asm`) where project convention is clear.

### 7.1 General scanner rules

Each language scanner should extract:

- module/package/namespace names
- class/struct/interface names
- function/method names
- constructor names
- parameters
- return type when easy
- start and end line
- nearby doc comments/docstrings
- imports/includes/uses
- likely function calls
- HTML elements with ids/classes when useful for web tasks
- CSS selectors and rule blocks
- assembler labels, directives, global symbols, and call/jump references

Use `std::regex` for common signatures and lightweight brace/indent/tag tracking for symbol ranges. Never block ordinary editing for long.

### 7.2 Comment extraction

Extract nearby comments directly above a symbol.

Examples:

- C/C++/Java/C#/JavaScript/TypeScript/PHP/Rust/Go/CSS: `//`, `/* ... */`, `/** ... */` where supported
- Python: triple-quoted docstrings and preceding `#` comments
- Perl: preceding `#` comments
- HTML5: `<!-- ... -->`, plus comments inside embedded script/style blocks
- GNU assembler: preceding `#`, `//`, or `/* ... */` depending on source convention

Be conservative: do not attach unrelated comments separated by blank lines or unrelated code.

### 7.3 Cheap heuristic docs

If no doc comment exists, store `doc_source = none` or a cheap heuristic description, for example:

```text
C++ function returning bool. Calls parse_args, load_config, run_chat_loop.
```

Do not run LLM summarization during initial indexing.

### 7.4 Function end detection

Language-specific approaches:

- Brace languages: count braces while ignoring strings and comments as much as practical
- Python: indentation-based range detection
- Perl/PHP: brace counting with package/function awareness
- Rust/Go: brace counting plus regexes for common declarations
- HTML5: tag/block tracking; embedded JS/CSS delegated to their scanners
- CSS: selector/rule block tracking
- GNU assembler: labels and directives; ranges end at the next label or section directive

Missing rare cases is acceptable. Fallback tools recover. The scanner is a fast hint generator, not an authority.

---

## 8. PageRank and ranking

Calculate approximate PageRank over symbols and files using the reference graph.

Useful edges:

- symbol calls symbol
- file includes/imports file
- class owns method
- test calls production symbol
- entry point calls function

Search ranking should combine:

```text
text match score
symbol name match
path match
language match
PageRank
recently edited/touched boost
active task boost
```

Approximate formula (tunable via benchmark mode):

```text
score = text_match
      + 0.25 * normalized_pagerank
      + 0.20 * path_match
      + 0.15 * recently_touched
      + 0.10 * same_language_as_active_file
```

---

## 9. Suggested implementation files

Adapt names if equivalent modules already exist. Do not duplicate functionality.

```text
src/agent/agent_loop.hpp
src/agent/agent_loop.cpp
src/agent/tool_call.hpp
src/agent/tool_registry.hpp
src/agent/tool_registry.cpp
src/agent/tool_scheduler.hpp
src/agent/tool_scheduler.cpp
src/agent/tools_files.hpp
src/agent/tools_files.cpp
src/agent/tools_search.hpp
src/agent/tools_search.cpp
src/agent/tools_edit.hpp
src/agent/tools_edit.cpp
src/agent/tools_command.hpp
src/agent/tools_command.cpp
src/agent/tools_web.hpp
src/agent/tools_web.cpp
src/agent/git_tools.hpp
src/agent/git_tools.cpp
src/agent/command_guard.hpp
src/agent/command_guard.cpp
src/agent/agent_prompts.hpp
src/agent/agent_prompts.cpp
src/agent/agents_md.hpp
src/agent/agents_md.cpp
src/agent/agent_memory.hpp
src/agent/agent_memory.cpp
src/agent/context_compact.hpp
src/agent/context_compact.cpp
src/agent/hash.hpp
src/agent/hash.cpp
src/agent/index/index_db.hpp
src/agent/index/index_db.cpp
src/agent/index/index_update.hpp
src/agent/index/index_update.cpp
src/agent/index/scanner.hpp
src/agent/index/scanner.cpp
src/agent/index/scanners/          # per-language lightweight scanners
src/agent/index/rank.hpp
src/agent/index/rank.cpp
```

Suggested tests:

```text
tests/unit/agent/test_tool_registry.cpp
tests/unit/agent/test_file_tools.cpp
tests/unit/agent/test_edit_tools.cpp
tests/unit/agent/test_str_replace_fuzzy.cpp
tests/unit/agent/test_apply_patch.cpp
tests/unit/agent/test_command_guard.cpp
tests/unit/agent/test_tool_scheduler.cpp
tests/unit/agent/test_agents_md.cpp
tests/unit/agent/test_memory.cpp
tests/unit/agent/test_web_tools.cpp
tests/unit/agent/test_index_db.cpp
tests/unit/agent/test_scanner.cpp
tests/unit/agent/test_hash.cpp
```

Built-in prompt resources (short, task-specific):

```text
resources/agents/AGENTS.base.md
resources/agents/AGENTS.coding.md
resources/agents/AGENTS.debug.md
resources/agents/AGENTS.review.md
resources/agents/AGENTS.refactor.md
resources/agents/AGENTS.tests.md
```

(If the repo prefers `config/` or `docs/` install paths, place installable copies consistently with other bundled templates; keep source of truth next to the agent module or under `resources/agents/`.)

---

## 10. Provider-neutral tool representation

Use one internal representation; convert to OpenAI-compatible, Anthropic-style, Gemini-style, and plain text fallback formats at the adapter boundary.

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

Text fallback for weaker models:

```text
<tool_call name="read_file">
{"path":"src/main.cpp","start_line":1,"end_line":120}
</tool_call>
```

All `max_*`, timeout, candidate-count, and truncation defaults are settings values, not hardcoded constants. If a tool parameter is `null` or omitted, resolve through the settings stack.

---

## 11. Built-in tool list

Prefer short, familiar tool names. Some tools are aliases for model compatibility.

### 11.1 Context and discovery

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

### 11.2 File and edit

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

### 11.3 Command, git, and web

```text
run_command
git_status
git_diff
fetch_url
search_web
```

### 11.4 Index

```text
index_status
index_update
index_rebuild
```

Normal operation uses automatic per-file updates and `index_update`. `index_rebuild` is for recovery/debugging.

---

## 12. Tool specifications

### 12.1 `project_overview`

Purpose: compact project map.

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

### 12.2 `inspect_code_task`

Purpose: macro-tool that reduces turns by returning likely files, symbols, tests, and suggested reads for a task. Internally may combine symbol search, text search, PageRank, recent files, and path heuristics.

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
  "skeletons": [],
  "likely_tests": ["tests/test_chat.cpp"],
  "truncated": false
}
```

### 12.3 `list_directory`

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

Suggested defaults: `recursive: false`, `max_depth: 1`, `include_hidden: false`, `include_ignored: false`, `max_entries: 300`.

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

Notes: respect `.gitignore` via `git ls-files` when inside a git repository; fall back to filesystem traversal otherwise.

### 12.4 `glob`

Purpose: path discovery by pattern (filenames/paths, not file content).

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

Implementation: C++ filesystem + simple glob matching; keep separate from `search_text` so path discovery does not waste tokens on content search.

### 12.5 `search_text`

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

Implementation: fast internal literal search; `std::regex` for regex mode; optional `rg` fallback when available; optionally SQLite FTS5 for DB-backed partial search.

### 12.6 `grep`

Purpose: compatibility alias for content search (many models expect `grep`).

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

Return: same shape as `search_text`. Dispatch internally to `search_text`. Do not call shell `grep` unless falling back through `run_command`.

### 12.7 `find`

Purpose: simple compatibility alias for literal text search.

Parameters:

```json
{
  "path": "string",
  "search_string": "string",
  "max_hits": "integer|null"
}
```

Return: same shape as `search_text`. Internally dispatch with `regex = false`.

### 12.8 `search_symbol`

Purpose: search the SQLite symbol/code index in `.ainiux/index.sqlite`.

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

**Important:** the scanner/index is a hint, not truth. Verify with `read_symbol`, `get_skeleton`, `read_many`, `read_file`, `search_text`, `glob`, `grep`, or compiler/tests when needed. Cheaply update changed files before searching.

### 12.9 `get_skeleton`

Purpose: signatures, declarations, and doc comments for a file — primary token-saving tool.

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

Use the SQLite index when fresh; if missing or stale, quickly rescan only that file. If the skeleton looks incomplete, stale, or contradictory, fall back to reads/searches — the model must not blindly trust skeleton data.

### 12.10 `read_symbol`

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

### 12.11 `read_file`

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

Suggested defaults: `max_bytes: 50000`, `max_lines: 500`, `include_line_numbers: true`, `include_hashes: true`.

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

Refuse binary files by default. Include `range_hash` when line ranges are returned.

### 12.12 `read_many`

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

Prefer this over repeated `read_file` calls. If the output cap is reached, include complete earlier items and mark later items omitted or truncated.

### 12.13 `write_file`

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

- Canonicalize path; refuse writes outside workspace root or trusted temp directories.
- If `mode = create_new`, fail if the file exists.
- If `expected_file_hash` is supplied and does not match, fail with `stale_file`.
- Record rollback data before overwriting.
- Reindex the affected file immediately.

### 12.14 `remove`

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
- Ask before deleting database files such as `*.sqlite`, `*.sqlite3`, `*.db`, `*.db3`, `*.duckdb` (includes project `.ainiux/index.sqlite`).
- Refuse deletion outside workspace root or trusted temp directories.
- Record rollback data where practical; update the index.

### 12.15 `edit_file`

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

Suggested defaults: `atomic: true`, `create_dirs: false`.

#### Operations

**`replace_range`** — replace inclusive lines `start_line`–`end_line` with `replacement`. Preferred normal edit mode. If `expected_hash` is supplied and mismatches, fail with `stale_range` and return current range preview.

**`insert_at`** — insert `new_text` before line `line`.

**`delete_range`** — delete inclusive lines `start_line`–`end_line`.

**`replace_text`** — find `old_text`, replace with `new_text`. Exact match first; fuzzy fallback (section 13) if enabled; disambiguate with `line_range_hint` or fail `ambiguous_match`.

**`replace_symbol`** — replace indexed symbol body by `symbol_id` + `replacement`. Check `expected_hash` against body/range hash when supplied. Reindex after success.

**`create_file`** — create new file with `new_text`; fail if exists (use `write_file` internally).

Return data:

```json
{
  "path": "src/agent.cpp",
  "applied": true,
  "operations_applied": 2,
  "old_file_hash": "fnv1a64:...",
  "new_file_hash": "fnv1a64:...",
  "reverse_patch_path": ".ainiux/history/20260719-120000-001.patch",
  "index_updated": true,
  "summary": ["replaced lines 42-118", "inserted before line 7"],
  "warnings": []
}
```

Rules:

- Apply multiple line operations bottom-to-top.
- Use `expected_file_hash` or operation `expected_hash` when available.
- If stale, return current hash and a short current preview.
- For `atomic = true`, leave no partial edits after failure.
- Record rollback data before mutation.
- Reindex only affected files after success.

### 12.16 `str_replace`

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
  "reverse_patch_path": ".ainiux/history/20260719-120000-002.patch",
  "index_updated": true
}
```

Uses the same engine as `edit_file.replace_text`. Prefer `edit_file.replace_range` when possible.

### 12.17 `apply_patch`

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
  "reverse_patch_path": ".ainiux/history/20260719-120000-003.patch",
  "index_updated": true,
  "warnings": []
}
```

Rules: parse add/update/delete; validate paths; apply atomically by default; run destructive guard before deletes; reindex changed files.

### 12.18 `find_callers`

Purpose: find symbols that call or reference a symbol.

Parameters:

```json
{
  "symbol_id": "integer|null",
  "name": "string|null",
  "max_results": "integer|null",
  "min_confidence": "number"
}
```

Suggested defaults: `max_results: 30`, `min_confidence: 0.3`.

Return data:

```json
{
  "callers": [
    {
      "symbol_id": 220,
      "qualified_name": "main",
      "path": "src/main.cpp",
      "line": 87,
      "confidence": 0.8
    }
  ],
  "truncated": false
}
```

### 12.19 `find_callees`

Purpose: find symbols called by a symbol.

Parameters:

```json
{
  "symbol_id": "integer",
  "max_results": "integer|null",
  "min_confidence": "number"
}
```

Suggested defaults: `max_results: 30`, `min_confidence: 0.3`.

Return data:

```json
{
  "callees": [
    {
      "symbol_id": 184,
      "qualified_name": "Agent::run",
      "path": "src/agent.cpp",
      "line": 42,
      "confidence": 0.8
    }
  ],
  "truncated": false
}
```

### 12.20 `find_tests`

Purpose: find likely tests for a file or symbol.

Parameters:

```json
{
  "path": "string|null",
  "symbol_id": "integer|null",
  "max_results": "integer|null"
}
```

Suggested default: `max_results: 20`.

Return data:

```json
{
  "tests": [
    {
      "path": "tests/test_agent.cpp",
      "symbol_id": 900,
      "qualified_name": "test_agent_run_handles_interrupt",
      "confidence": 0.7
    }
  ],
  "commands": [
    "make test",
    "ctest --output-on-failure"
  ],
  "truncated": false
}
```

Notes: use naming conventions, paths, and references. Approximate is fine.

### 12.21 `run_command`

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
- Use `posix_spawn` / `fork+exec` on Unix-like systems (and a documented Windows path only if Windows support is in scope).
- Kill process groups on timeout.
- Cap stdout/stderr.
- Run the destructive-command guard before execution.
- After commands that may modify files, mark index as possibly stale and cheaply update changed files.

### 12.22 `git_status`

Purpose: compact git status through the git CLI (not libgit2).

Parameters:

```json
{
  "short": "boolean",
  "include_branch": "boolean"
}
```

Typical command: `git status --short --branch`.

### 12.23 `git_diff`

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

Typical commands: `git diff --stat`, `git diff -- PATH`, `git diff --cached`.

### 12.24 `fetch_url`

Purpose: fetch a URL using libcurl / existing fetch safety layer.

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

Return data includes `url`, `final_url`, `status`, `content_type`, `title`, `text`/`body`, optional headers, `bytes_read`, `truncated`.

Rules:

- Allow `http` and `https` by default; disable `file`, `ftp`, and unusual schemes unless explicitly enabled.
- Bound bytes and timeouts.
- Reuse private/loopback/metadata blocking from `src/fetch/` unless the user explicitly allows private URL fetch.
- Simple dependency-free HTML text extraction.

### 12.25 `search_web`

Purpose: web search through configured provider (`src/search/`).

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

If no provider is configured, return `web_search_unavailable`. Do not hardcode one commercial provider.

### 12.26 `index_status`

Purpose: report index state for `.ainiux/index.sqlite`.

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
  "path": ".ainiux/index.sqlite",
  "fresh": true,
  "files_indexed": 220,
  "symbols_indexed": 6400,
  "refs_indexed": 18000,
  "changed_files": [],
  "last_updated": 1783170000
}
```

### 12.27 `index_update`

Purpose: update changed files only.

Parameters:

```json
{
  "paths": ["string"],
  "force": "boolean"
}
```

If `paths` is empty, detect changed files cheaply. If `force = true`, rescan even if timestamp/hash appears unchanged.

### 12.28 `index_rebuild`

Purpose: full rebuild for recovery/debugging.

Parameters:

```json
{
  "confirm": "boolean"
}
```

Normal users should rarely need this.

---

## 13. Fuzzy edit fallback

Failed edits are expensive because each failure often requires another model round trip. Therefore `str_replace` and `edit_file.replace_text` support a Gemini-style fuzzy fallback.

### 13.1 Fallback order

1. Exact byte-for-byte match.
2. Normalized whitespace match.
3. Leading-indent-stripped match.
4. Fail with useful diagnostics.

### 13.2 Normalized whitespace matching

Conceptually collapse runs of whitespace for comparison while mapping back to original source offsets for the actual replace:

```text
"foo(  a,\n b )" -> "foo( a, b )"
```

### 13.3 Leading-indent-stripped matching

For multi-line snippets, compare after stripping common leading indentation from both old text and candidate text. Handles extra indent level, tabs vs spaces at the margin, and snippets copied without surrounding indentation.

### 13.4 Multiple matches

- If `replace_all = true`, replace all matches.
- Else if `line_range_hint` is supplied, choose the match inside or closest to that range.
- Else fail with `ambiguous_match` and return candidate line numbers.

### 13.5 Safety rules

- Prefer exact match over fuzzy match.
- Report `match_mode` in the result.
- If the fuzzy match is too weak, fail.
- If multiple candidates remain, fail unless `replace_all` or `line_range_hint` disambiguates.
- Return candidate locations on ambiguity.

### 13.6 Why this matters

Slight whitespace/indent errors from the model become successful edits and save extra LLM turns, tool calls, file reads, and user frustration.

---

## 14. Parallel call handling

The scheduler accepts multiple tool calls from the model and executes safe independent calls concurrently.

### 14.1 Tool safety classes

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
  inspect_code_task, git_status, git_diff

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

### 14.2 Lock rules

Lock scopes:

```text
workspace read lock
workspace mutation lock
file path mutation lock
index write lock
network concurrency token
```

Rules:

- Multiple reads/searches may run in parallel.
- Multiple network tools may run in parallel up to a configured cap.
- For v1, it is simpler to serialize all file mutations (even different files) if index/rollback conflicts are hard.
- Never mutate the same file in parallel.
- Serialize `apply_patch`.
- Serialize maybe-mutating `run_command` with file edits.
- If two model-emitted tool calls conflict, run them in safe order rather than failing.

### 14.3 `run_command` parallel policy

```text
safe_read_command:
  git status, git diff, git log, git show, pwd, ls, rg, grep, find without -delete

maybe_mutating_command:
  make, ninja, cmake --build, cargo test, go test, npm test, pytest, compiler commands

high_risk_command:
  anything caught by the destructive-command guard
```

Safe read commands can run in parallel with other reads. Maybe-mutating commands should not run in parallel with edits. High-risk commands must wait for user approval or be blocked.

---

## 15. Destructive-command guard

A small built-in guard inspired by destructive-command protection tools: command normalization, path canonicalization, and regex/pattern rules. This is not a full sandbox.

### 15.1 Guard applies to

```text
run_command
write_file
remove
edit_file
str_replace
apply_patch
```

Also any future database execution tool.

### 15.2 Guard decision shape

```json
{
  "decision": "allow|ask|deny",
  "rule_id": "AINIUX_GUARD_RM_RF",
  "severity": "low|medium|high|critical",
  "reason": "Recursive force delete requires approval.",
  "safe_alternative": "Use trash, move to .ainiux/tmp, or run git clean -n first."
}
```

### 15.3 Guard control flow

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

### 15.4 Path rules

- File writes/deletes must stay inside the workspace root or trusted temp directories.
- Canonicalize paths before decision; handle symlinks and `..` conservatively.
- Reject or ask on path analysis failure for mutating operations.

Trusted temp directories:

```text
$TMPDIR when set
/tmp
/var/tmp
workspace/.ainiux/tmp
```

### 15.5 Commands requiring approval

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

Workspace escape: writes/edits/deletes outside workspace root; redirection to outside path; `cp`/`mv`/`install`/`touch`/`mkdir` outside allowed roots.

### 15.6 Safe commands that should not be blocked

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

### 15.7 Shell normalization

Recognize common wrappers:

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

### 15.8 User approval

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
move files to .ainiux/tmp
make a backup
run SELECT before DELETE
run schema dump before DROP
```

### 15.9 Basic command hygiene (even without full sandbox)

- Canonicalize paths.
- Refuse file edits outside workspace root.
- Detect binary files before text edits.
- Cap command output; use timeouts; kill process groups on timeout.
- Keep edit history; provide `/undo`.

---

## 16. Prompt and `AGENTS.md` handling

### 16.1 Built-in prompts

Keep internal prompts short and task-specific. Load only the base prompt plus at most one task-specific prompt.

```text
resources/agents/AGENTS.base.md
resources/agents/AGENTS.coding.md
resources/agents/AGENTS.debug.md
resources/agents/AGENTS.review.md
resources/agents/AGENTS.refactor.md
resources/agents/AGENTS.tests.md
```

### 16.2 Project `AGENTS.md`

1. Load `AGENTS.md` from the workspace root if it exists.
2. When editing or reading a specific file, also load the nearest `AGENTS.md` between the workspace root and that file's directory.
3. More specific `AGENTS.md` rules override broader project rules when they conflict.
4. Cache loaded files by `file_hash`; re-read if hash changes.
5. Cap total injected bytes via `agents_md.max_bytes_total`.

Example for `src/ui/button.cpp`:

```text
workspace/AGENTS.md
workspace/src/AGENTS.md
workspace/src/ui/AGENTS.md
```

Also recognize related project instruction filenames when present (`PLAN.md`, `PLANS.md`) only if product policy later expands discovery; v1 must at least support `AGENTS.md` with clear precedence.

### 16.3 Instruction precedence

```text
system/developer safety rules
ainiux built-in agent rules
user current request
project AGENTS.md rules
local nearest AGENTS.md rules
model-generated plan
```

`AGENTS.md` must not disable the destructive-command guard, change the workspace root, exfiltrate secrets, or override the user's direct request.

### 16.4 Minimum built-in instruction (condensed from v2 draft + v3)

The base built-in agent prompt must include:

```text
Use the local project index (.ainiux/index.sqlite) first because it is cheap, but treat it
as a hint, not truth. The scanner can miss symbols, embedded code, macro-generated code,
overloaded functions, dynamic calls, and unusual syntax. Never blindly trust get_skeleton,
search_symbol, PageRank, or call graph data. If the indexed view looks incomplete, stale,
or contradictory, fall back to read_symbol, read_many, read_file, search_text, glob, grep,
git grep, compiler output, or tests.

Prefer tools in this order for context:
1. inspect_code_task
2. search_symbol
3. get_skeleton
4. read_symbol / read_many
5. targeted read_file ranges
6. glob / search_text / grep / find
7. run_command fallbacks
8. fetch_url / search_web only when external current information is needed

Prefer edits in this order:
1. edit_file.replace_range with expected_hash
2. edit_file.replace_symbol
3. edit_file.replace_text / str_replace with fuzzy fallback
4. apply_patch for multi-file patches
5. write_file only for new files or intentional full rewrites

Keep context small. Prefer batched reads and edits. Do not rewrite unrelated code.
After edits, run the narrowest useful test or build. If a tool fails for a non-permission
and non-network reason, do not retry blindly; use a different route.

Planning: concrete, bite-sized plans with exact files, commands, expected outputs, and
verification. Follow DRY, YAGNI, and KISS. Avoid new dependencies without clear need.

Coding: clear names, explicit errors, simple control flow. Preserve existing style unless
changing it is part of the task.

Testing: prefer TDD where practical. Cover Unicode, empty/long strings, edge numbers,
invalid input, permission and network failures where relevant. Rerun focused tests after
behavior changes.

Refactoring: remove duplication, improve names, extract helpers only when they simplify,
keep behavior unchanged, run tests after refactoring.
```

---

## 17. Preferred agent tool order

### 17.1 Context gathering

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

### 17.2 Editing

```text
1. edit_file.replace_range with expected_hash
2. edit_file.replace_symbol with symbol_id and expected_hash
3. edit_file.replace_text / str_replace with fuzzy fallback
4. apply_patch for patch-style multi-file edits
5. write_file only for new files or intentional full rewrites
```

### 17.3 Verification

```text
1. narrow unit test for changed area
2. compiler/build command for touched component
3. relevant integration test
4. formatter/linter only if already part of project workflow
5. git diff/status summary
```

---

## 18. Slash commands

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

`/guard explain COMMAND` shows whether a command would be allowed, asked, or denied and why, without running it.

`/tools` displays available tools, aliases, and whether web/search tools are configured.

`/status` shows:

```text
workspace root
git branch
dirty files
current model
context usage estimate
index path (.ainiux/index.sqlite) and freshness
last test result
guard enabled/disabled status
loaded AGENTS.md files
```

Ctrl-C behavior:

```text
First Ctrl-C: interrupt current model stream or running command.
Second Ctrl-C: abort current agent task and return to ainiux prompt.
```

(Align with terminal/editor copy conventions where agent shares editor-backed surfaces; document any surface-specific differences.)

---

## 19. Memory, compaction, and rollback

### 19.1 Session log

Use append-only `.ainiux/session.jsonl` as the source of truth.

Example events:

```json
{"type":"user_goal","text":"Add agent mode"}
{"type":"tool_call","name":"search_symbol","args":{"query":"agent loop"}}
{"type":"edit","files":["src/agent.cpp"],"reverse_patch":".ainiux/history/001.patch"}
{"type":"test","command":"make test","exit_code":0}
{"type":"decision","text":"Use replace_range as primary edit primitive"}
```

### 19.2 Generated memory

Generate `.ainiux/memory.md` from the event log.

Suggested shape:

```markdown
# Project Memory

## Current goal
Implement agent mode for ainiux.

## Decisions
- Use file-level hashes as the primary edit-safety mechanism.
- Use range replacement as the primary edit primitive.
- Store code index in project-local .ainiux/index.sqlite.
- Use git CLI instead of libgit2.

## Modified files
- src/agent/agent_loop.cpp: added first agent loop.
- src/agent/tools_edit.cpp: added range edit engine.

## Last verification
- `make test` failed with 2 errors in src/agent/tools_edit.cpp.

## Known issues
- No full sandbox yet.
- C++ scanner misses complex macro-generated methods.

## Next likely actions
- Fix edit tool tests.
- Re-run narrow tests.
```

Update memory after meaningful milestones, not after every tiny tool call.

### 19.3 Compaction

Preserve:

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
active file/symbol IDs
current git diff summary
```

Discard:

```text
old tool chatter
large file dumps no longer needed
repeated compiler output
superseded plans
failed search paths that do not matter
```

### 19.4 Rollback

Before each edit batch, store rollback data:

```text
.ainiux/history/YYYYMMDD-HHMMSS-NNN.patch
```

Successful edit results should include: files changed, operation count, old and new file hashes, reverse patch path, index update status.

`/undo` applies the last ainiux-created reverse patch or restores saved file snapshots.

---

## 20. Git integration

Use git through the command line. Do not add libgit2 in the first implementation.

Useful commands:

```text
git status --short --branch
git diff --stat
git diff -- PATH
git diff --cached
git ls-files
git grep
git rev-parse --show-toplevel
```

Reasons: available in most environments, respects user config, no new dependency, process startup is not the main bottleneck.

---

## 21. Implementation milestones

Accepted merge: v3 runtime milestones plus explicit index/scanner/ranking milestones from v2.

### Milestone 1: minimal agent loop and tool registry

Files: `agent_loop.*`, `tool_call.*`, `tool_registry.*`, `tool_scheduler.*`

Tasks:

- Provider-neutral `ToolCall` / `ToolResult`
- Tool registry with name, schema, handler, safety class
- Agent loop: receive model tool calls, execute tools, return results
- Text fallback parser for weak tool-calling models
- Wire `ainiux agent` mode dispatch without enabling tools in normal chat

Verification:

```text
./ainiux --agent --tools-selftest
# expected: agent tools self-test: OK
```

### Milestone 2: file, path, and search tools

Files: `tools_files.*`, `tools_search.*`, `hash.*`

Tasks:

- `read_file`, `read_many`, `write_file`, `remove`
- `list_directory`, `glob`, `search_text`, `grep`, `find`
- File-level hashing
- Path canonicalization and workspace-root checks

Tests: `test_file_tools`, `test_hash`, search tool tests.

### Milestone 3: edit engine

Files: `tools_edit.*`

Tasks:

- `edit_file` ops: replace_range, insert_at, delete_range, replace_text, replace_symbol, create_file
- `str_replace` compatibility wrapper
- Fuzzy fallback: exact, normalized whitespace, indent stripped
- `apply_patch` compatibility parser
- Rollback patch creation under `.ainiux/history/`
- Hook for reindex of touched files (may no-op until Milestone 4)

Tests: `test_edit_tools`, `test_str_replace_fuzzy`, `test_apply_patch`.

### Milestone 4: project-local SQLite code index and scanners

Files: `index/*`, scanners

Tasks:

- Create/open `.ainiux/index.sqlite` (never central chat DB)
- Schema: files, symbols, refs, FTS when available
- Incremental scan using size/mtime/hash
- Immediate reindex after ainiux edits
- Language scanners for priority languages
- Tools: `get_skeleton`, `search_symbol`, `read_symbol`, `index_status`, `index_update`, `index_rebuild`
- Slash: `/index status|update|rebuild|explain`

Success criteria:

- Changed ainiux-edited files reindex immediately
- External changes detected by timestamp plus hash
- Skeletons and symbol search fast enough for interactive use
- Index file lives at `.ainiux/index.sqlite` and is portable with the project

### Milestone 5: ranking and task inspection

Tasks:

- PageRank over symbols/files
- Ranking formula with recent-file / language boosts
- `inspect_code_task`, `find_callers`, `find_callees`, `find_tests`, `project_overview`

Success criteria:

- Agent usually finds relevant files without repeated directory listing
- Fewer full-file reads; faster first useful edit

### Milestone 6: command execution and guard

Files: `tools_command.*`, `git_tools.*`, `command_guard.*`

Tasks:

- `run_command` without `system()`
- Timeout and process-group kill
- Bounded stdout/stderr
- `git_status`, `git_diff`
- Destructive-command guard
- `/guard status`, `/guard explain COMMAND`

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
rm .ainiux/index.sqlite
write outside workspace
write to /tmp/ainiux-test-file
```

### Milestone 7: web tools

Files: `tools_web.*`

Tasks:

- `fetch_url` via existing curl/fetch safety
- Simple HTML text extraction
- `search_web` via configured `src/search/` providers
- Return `web_search_unavailable` if not configured

Failure cases: invalid URL, unsupported scheme, timeout, HTTP 404, large response truncation, provider not configured, private URL blocked.

### Milestone 8: prompts, AGENTS.md, memory, and compaction

Files: `agent_prompts.*`, `agents_md.*`, `agent_memory.*`, `context_compact.*`, resource prompt files

Tasks:

- Load short built-in prompts (base + one task-specific)
- Load root and nearest `AGENTS.md` with precedence
- Generate `.ainiux/session.jsonl` and `.ainiux/memory.md`
- `/compact`, `/compact auto`, `/compact off`
- `/plan`, `/resume`, `/memory`

### Milestone 9: benchmark and tuning

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

Use benchmarks to tune tool output caps, symbol ranking, fuzzy edit thresholds, context compaction, and scanner regexes.

---

## 22. Test requirements

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
index created at .ainiux/index.sqlite not ~/.ainiux/ainiux.db
incremental reindex after edit
scanner extracts common C++/Python symbols
FTS fallback when FTS5 unavailable (if practical to simulate)
```

Do not keep rerunning the same failing tool blindly. If a tool fails for a non-permission and non-network reason, use a fallback route.

Leak checks: success, error, cancel, interrupted stream/command paths where tooling is available (`make test-sanitize`, `make test-leak`).

---

## 23. Summary

The first serious ainiux agent mode should be built around:

- separate entry `ainiux agent` (no silent tools in normal chat)
- reuse of provider, runtime, fetch, search, and security layers
- project-local store under `.ainiux/` with **`index.sqlite`** (never the central TUI chat DB)
- fast C++17 `std::regex`/pattern scanning for priority languages
- SQLite3 WAL index with optional FTS5 / trigram
- incremental per-file updates and settings-driven SQLite pragmas
- settings-driven tool limits (project JSON + optional user-global agent settings)
- file-level hashes as primary edit safety; range hashes for range edits
- token-efficient skeleton and symbol tools; index is a hint not truth
- `glob` / `grep` / `find` compatibility tools
- `fetch_url` and `search_web` with bounded output and existing safety policies
- range edits with expected hashes; fuzzy fallback for text replacement
- `apply_patch` and `str_replace` compatibility
- parallel reads; serialized mutations; DCG-style destructive guard
- git command-line integration
- append-only session log, generated memory, cheap rollback and `/undo`
- short built-in prompts plus project `AGENTS.md` precedence
- practical default safety (workspace edits allowed; destructive actions ask/deny)

This is the 80/20 approach: extremely fast on common cases, simple to reason about, dependency-light, portable per project via `.ainiux/index.sqlite`, and able to fall back to slower generic tools when the index is wrong.

---

## 24. Document history

| Version | Notes |
| --- | --- |
| v2 | Deep index/scanner/ranking plan; project `.ainiux/index.sqlite` |
| v3 | Agent runtime focus; guard; parallel tools; AGENTS.md; used pkchat naming |
| **v4** | Merge: v3 basis + all accepted v2 features; ainiux branding; explicit central-DB boundary; architecture fit; merged milestones; safety choice A |

Superseded drafts may remain in the repo for history; **implement against this file**.
