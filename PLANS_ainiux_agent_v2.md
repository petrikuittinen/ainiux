# PLANS.md - ainiux agent mode and code index plan

This document is intended to be merged into the existing `PLANS.md`. The goal is to add a fast, token-efficient, dependency-light agentic coding mode to ainiux while preserving the current C++17-first design philosophy.

## 1. Core direction

Build a state-of-the-art coding agent around these principles:

1. Keep external dependencies minimal.
2. Use C++17 for fast local tools, indexing, editing, searching, and command execution.
3. Use SQLite3 with WAL mode as the project-local code index.
4. Prefer fast 80/20 parsing using C++17 pattern matching and `std::regex`.
5. Leave Tree-sitter integration for the future.
6. Leave libgit2 out of the initial implementation; use the `git` command-line tool.
7. Optimize for fewer LLM turns, smaller tool outputs, and high edit success rate.
8. Prefer deterministic local tools over model guessing whenever possible.
9. Keep tool outputs bounded and token-efficient by default.
10. Store tool limits in settings, not as hardcoded constants.
11. Add web access through simple model-familiar tools: `fetch_url(url)` and `search_web(term)`.
12. Prioritize indexing for C, C++, C#, Python, JavaScript, TypeScript, Java, PHP, Perl, HTML5, CSS, Rust, Go, and GNU-style assembler.
13. Make rare or difficult edge cases fall back to slower shell tools such as `rg`, `find`, compiler output, and `git`.

The main performance target is not only raw local execution speed. The bigger win is reducing expensive model round trips by giving the model better context, better edit primitives, and better error recovery.

## 2. Non-goals for the first implementation

The first implementation should not attempt to solve everything.

Deferred:

- Tree-sitter integration.
- libgit2 integration.
- Full sandboxing.
- Perfect static analysis.
- Perfect C++ parsing.
- Full semantic call graph resolution.
- MCP support.
- SKILLS.md support.
- Heavy permission prompting before every tool call.

The first version should be pragmatic, fast, and useful on ordinary projects.

## 3. Project-local storage layout

Use a hidden project-local directory:

```text
.ainiux/
  index.sqlite
  session.jsonl
  memory.md
  settings.json
  history/
  plans/
```

Purposes:

- `index.sqlite`: SQLite code index.
- `session.jsonl`: append-only event log of the agent session.
- `memory.md`: generated summary of important project/session facts.
- `settings.json`: project-local ainiux settings.
- `history/`: reverse patches, edit snapshots, or rollback data.
- `plans/`: saved plans for long-running tasks.

`memory.md` should be generated from the append-only session log. It should not be the only source of truth.


### 3.1 Settings and configurable limits

Do not hardcode practical limits such as maximum read size, maximum search hits, command timeout, network timeout, fuzzy edit thresholds, or output byte caps inside tool implementations.

Use layered settings:

```text
compiled defaults
user-global settings
project-local .ainiux/settings.json
session settings from /settings
single tool-call parameters
```

Recommended global settings path:

```text
$XDG_CONFIG_HOME/ainiux/settings.json
fallback: ~/.config/ainiux/settings.json
```

Project-local settings path:

```text
.ainiux/settings.json
```

A tool-call parameter should override settings for that single call. A project-local setting should override user-global settings. User-global settings should override compiled defaults. Compiled defaults should exist only as a safe last resort.

Example settings shape:

```json
{
  "limits": {
    "read_file": {"max_lines": 500, "max_bytes": 50000},
    "read_many": {"max_total_bytes": 80000},
    "get_skeleton": {"max_items": 300, "max_bytes": 40000},
    "search_text": {"max_hits": 80, "max_bytes": 60000},
    "search_symbol": {"max_results": 20},
    "run_command": {"timeout_ms": 120000, "max_output_bytes": 12000},
    "fetch_url": {"timeout_ms": 30000, "max_bytes": 200000},
    "search_web": {"max_results": 10, "timeout_ms": 30000}
  },
  "index": {
    "sqlite_cache_size_kb": 200000,
    "sqlite_mmap_size": 268435456,
    "use_fts5": true,
    "use_fts5_trigram_for_partial_search": true
  },
  "editing": {
    "fuzzy_edit": true,
    "max_fuzzy_candidates": 20,
    "require_unique_match_by_default": true
  },
  "network": {
    "enabled": true,
    "user_agent": "ainiux/agent",
    "search_provider": "configured"
  }
}
```

## 4. SQLite index design

Use SQLite3 with WAL mode enabled.

Recommended pragmas:

```sql
PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
PRAGMA temp_store=MEMORY;
PRAGMA cache_size=-200000;
PRAGMA mmap_size=268435456;
```

The cache and mmap sizes should be configurable, but these are reasonable defaults for normal developer machines.

### 4.1 Files table

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

### 4.2 Symbols table

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

The first implementation should not run LLM summarization during full indexing. It should use existing comments and cheap heuristics only. Optional model-generated summaries can be created lazily and cached by `body_hash`.

### 4.3 References table

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

The call graph should be treated as probabilistic. Store confidence values rather than pretending every reference is known perfectly.

Example confidence levels:

```text
1.00 exact resolved symbol
0.80 same file unique matching name
0.70 imported or included name
0.50 likely class or object method call
0.30 lexical call-like token
```

### 4.4 FTS tables

Use SQLite FTS5 when available for fast symbol and documentation search:

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

When fast case-insensitive partial search is needed inside the DB, consider an additional FTS5 trigram table for file paths, symbol names, and short text fragments:

```sql
CREATE VIRTUAL TABLE IF NOT EXISTS code_search_fts USING fts5(
    path,
    symbol_name,
    text,
    tokenize='trigram'
);
```

Use this only if the linked SQLite build supports FTS5 and the tokenizer needed for the target platform. If FTS5 is not available, fall back to indexed `LIKE` searches, path/name filters, and the internal C++ scanner.

### 4.5 Useful indexes

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

## 5. Index update strategy

The index should be updated automatically for affected files after ainiux edits files.

Manual full rebuild should exist, but normal users should rarely need it. The normal path is automatic per-file updates that complete in a fraction of a second on ordinary projects.

### 5.1 Fast change detection

For each file, track:

- path
- size
- modification timestamp with highest available precision
- checksum/hash

Normal update path:

1. Compare stored size and timestamp.
2. If unchanged, skip hashing.
3. If changed, compute hash.
4. If hash is unchanged, update metadata only if needed.
5. If hash changed, rescan only that file.
6. Delete and replace symbols/refs for that file in one transaction.
7. Refresh FTS rows for changed symbols.

For files edited through ainiux, update the index immediately for those files only.

For files changed externally, detect changes during:

- agent startup
- `/status`
- `/index status`
- before `search_symbol`
- before `read_symbol`
- after `run_command` if the command may have generated or modified source files

The check should be cheap. Very large repositories may need more sophisticated scheduling, but ordinary projects should get sub-second updates.

### 5.2 Rebuild command

Keep manual rebuild commands for recovery and debugging:

```text
/index status
/index update
/index rebuild
/index explain PATH
```

Expected behavior:

- `/index status`: show freshness, file count, symbol count, changed files.
- `/index update`: update changed files only.
- `/index rebuild`: delete and rebuild the entire project index.
- `/index explain PATH`: show why a file is indexed, ignored, or failed.

## 6. C++17 pattern matching scanner

The first scanner should use fast C++17 code with regular expressions and lightweight lexical scanning.

The goal is not perfect parsing. The goal is to identify the common 80%+ of symbols and references quickly.

Priority supported first-pass languages and file types:

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

HTML5 indexing should detect embedded JavaScript inside `<script>` blocks and, when practical, embedded CSS inside `<style>` blocks. GNU assembler means the first assembler scanner should assume GAS syntax and common extensions such as `.s`, `.S`, and `.asm` where the project convention is clear.

### 6.1 General scanner rules

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

Use `std::regex` for common signatures and lightweight brace/indent/tag tracking for symbol ranges.

The scanner should avoid expensive full parsing. It should never block ordinary editing for long.

### 6.2 Comment extraction

Extract nearby comments directly above a symbol.

Examples:

- C/C++/Java/C#/JavaScript/TypeScript/PHP/Rust/Go/CSS: `//`, `/* ... */`, `/** ... */` where supported by the language
- Python: triple-quoted docstrings and preceding `#` comments
- Perl: preceding `#` comments
- HTML5: `<!-- ... -->` comments, plus comments inside embedded script/style blocks
- GNU assembler: preceding `#`, `//`, or `/* ... */` comments depending on source convention

Doc comment extraction should be conservative. Avoid attaching unrelated comments separated by blank lines or unrelated code.

### 6.3 Cheap heuristic docs

If no doc comment exists, store `doc_source = none` or a cheap heuristic description.

Example heuristic:

```text
C++ function returning bool. Calls parse_args, load_config, run_chat_loop.
```

Do not run LLM summarization during initial indexing.

### 6.4 Function end detection

Use simple language-specific approaches:

- Brace languages: count braces while ignoring strings and comments as much as practical.
- Python: indentation-based range detection.
- Perl/PHP: brace counting with package/function awareness.
- Rust/Go: brace counting plus regexes for `fn`, `impl`, `type`, `struct`, `enum`, `func`, `type`, and `interface`.
- HTML5: tag/block tracking, with embedded JavaScript and CSS delegated to their scanners.
- CSS: selector/rule block tracking.
- GNU assembler: labels and directives, with ranges ending at the next label or section directive.

It is acceptable to miss rare cases. Fallback tools can recover. The scanner is a fast hint generator, not an authority.

## 7. PageRank and ranking

Calculate PageRank over symbols and files using the approximate reference graph.

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

Approximate formula:

```text
score = text_match
      + 0.25 * normalized_pagerank
      + 0.20 * path_match
      + 0.15 * recently_touched
      + 0.10 * same_language_as_active_file
```

The exact formula can be tuned by benchmark mode.

## 8. Built-in agent tools

All tool calls should use a provider-neutral internal representation.

```cpp
struct ToolCall {
    std::string id;
    std::string name;
    Json args;
};

struct ToolResult {
    std::string id;
    bool ok;
    std::string content;
    Json metadata;
};
```

Every tool result should be bounded, deterministic, and easy for the model to use.

All `max_*`, timeout, candidate-count, and truncation defaults shown below are suggested settings values, not hardcoded implementation constants. If a tool parameter is `null` or omitted, resolve it through the settings stack described in section 3.1.

### 8.1 Common result envelope

All tools should return a structured result with these fields conceptually available:

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

On failure:

```json
{
  "ok": false,
  "error": {
    "code": "stale_range",
    "message": "The file changed since the range was read."
  },
  "data": {},
  "warnings": [],
  "truncated": false,
  "metadata": {}
}
```

The user-visible text format can be compact, but internally keep enough structure for provider adapters.

### 8.2 `read_file`

Purpose: read a whole file or a line range.

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

Suggested initial settings defaults:

```text
start_line: null
end_line: null
max_bytes: 50000
max_lines: 500
include_line_numbers: true
include_hashes: true
```

Return data:

```json
{
  "path": "src/example.cpp",
  "language": "cpp",
  "file_hash": "91a7d2...",
  "range": {"start_line": 120, "end_line": 145},
  "range_hash": "6f01c9...",
  "content": "120| int f() {\n121| ...\n",
  "line_count": 300,
  "truncated": false
}
```

Notes:

- Refuse binary files by default.
- Include `range_hash` when line ranges are returned.
- The `range_hash` is used by range-based edits.

### 8.3 `read_many`

Purpose: batch multiple file/range reads into one tool call.

Parameters:

```json
{
  "items": [
    {
      "path": "string",
      "start_line": "integer|null",
      "end_line": "integer|null"
    }
  ],
  "max_total_bytes": "integer|null",
  "include_line_numbers": "boolean",
  "include_hashes": "boolean"
}
```

Suggested initial settings defaults:

```text
max_total_bytes: 80000
include_line_numbers: true
include_hashes: true
```

Return data:

```json
{
  "items": [
    {
      "path": "src/a.cpp",
      "ok": true,
      "file_hash": "...",
      "range_hash": "...",
      "range": {"start_line": 10, "end_line": 80},
      "content": "10| ...",
      "truncated": false
    }
  ],
  "total_bytes": 42000,
  "truncated": false
}
```

Notes:

- Prefer this over repeated `read_file` calls.
- If the output cap is reached, include complete earlier items and mark later items as omitted or truncated.

### 8.4 `get_skeleton`

Purpose: return just signatures and doc comments from a file. This is much more token-efficient than reading the whole file.

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

Suggested initial settings defaults:

```text
include_private: true
include_line_numbers: true
include_doc_comments: true
include_fields: true
max_items: 300
max_bytes: 40000
```

Return data:

```json
{
  "path": "src/agent.cpp",
  "language": "cpp",
  "file_hash": "...",
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

Notes:

- This should use the SQLite index when fresh.
- If the file is not indexed or stale, quickly rescan only that file.
- This is one of the most important token-saving tools.

### 8.5 `write_file`

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

Suggested initial settings defaults:

```text
create_dirs: false
expected_file_hash: null
mode: overwrite
```

Return data:

```json
{
  "path": "src/new_file.cpp",
  "bytes_written": 1234,
  "new_file_hash": "...",
  "created": true,
  "indexed": true
}
```

Notes:

- If `mode = create_new`, fail if the file exists.
- If `expected_file_hash` is supplied and does not match, fail with `stale_file`.
- Record rollback data before overwriting an existing file.
- Reindex the affected file immediately.

### 8.6 `remove`

Purpose: remove a file or empty directory.

Parameters:

```json
{
  "path": "string",
  "recursive": "boolean",
  "expected_file_hash": "string|null"
}
```

Suggested initial settings defaults:

```text
recursive: false
expected_file_hash: null
```

Return data:

```json
{
  "path": "src/old.cpp",
  "removed": true,
  "was_directory": false,
  "index_updated": true
}
```

Notes:

- For safety, refuse recursive deletion by default.
- Always canonicalize paths and refuse deletion outside the workspace root.
- Record rollback information where practical.

### 8.7 `list_directory`

Purpose: list files/directories.

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

Suggested initial settings defaults:

```text
recursive: false
max_depth: 1
include_hidden: false
include_ignored: false
max_entries: 300
```

Return data:

```json
{
  "path": ".",
  "entries": [
    {"path": "src", "type": "directory"},
    {"path": "src/main.cpp", "type": "file", "size": 12345}
  ],
  "truncated": false
}
```

Notes:

- Respect `.gitignore` where practical by using `git ls-files` when inside a git repository.
- Fallback to filesystem traversal when not in git.

### 8.8 `search_text`

Purpose: find text or regex matches in project files.

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

Suggested initial settings defaults:

```text
path: null
regex: false
case_sensitive: true
whole_word: false
context_lines: 2
max_hits: 80
max_bytes: 60000
```

Return data:

```json
{
  "hits": [
    {
      "path": "src/main.cpp",
      "line": 42,
      "column": 13,
      "match": "Agent::run",
      "context": "40| ...\n41| ...\n42| ..."
    }
  ],
  "hit_count": 12,
  "truncated": false
}
```

Notes:

- This can use internal C++ regex/string search.
- It may optionally use `rg` as fallback when available.
- Keep output bounded.

### 8.9 `find`

Purpose: compatibility alias for simple text search.

Parameters:

```json
{
  "path": "string",
  "search_string": "string",
  "max_hits": "integer|null"
}
```

Suggested initial settings defaults:

```text
max_hits: 80
```

Return data:

Same as `search_text`, but simpler.

Notes:

- Internally dispatch to `search_text` with `regex = false`.
- This exists because many models expect a simple `find` tool.

### 8.10 `search_symbol`

Purpose: search the SQLite symbol index.

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

Suggested initial settings defaults:

```text
kind: null
language: null
path_glob: null
max_results: 20
include_docs: true
include_call_summary: true
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
      "doc": "Runs the agent loop until completion or interruption.",
      "calls": ["read_file", "edit_file", "run_command"]
    }
  ],
  "truncated": false,
  "index_fresh": true
}
```

Notes:

- This should update changed files cheaply before searching.
- This should be preferred before reading entire files.

### 8.11 `read_symbol`

Purpose: read a symbol body and optionally nearby context.

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

Suggested initial settings defaults:

```text
include_doc: true
include_callers: false
include_callees: true
include_siblings: false
context_lines: 5
max_bytes: 50000
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
    "file_hash": "...",
    "range_hash": "...",
    "doc": "...",
    "content": "37| ...\n42| bool Agent::run(...) {\n..."
  },
  "callers": [],
  "callees": [],
  "truncated": false
}
```

Notes:

- Return `range_hash` so the model can use `edit_file.replace_range` safely.
- If symbol location is stale, reindex the file and retry once.

### 8.12 `find_callers`

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

Suggested initial settings defaults:

```text
max_results: 30
min_confidence: 0.3
```

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

### 8.13 `find_callees`

Purpose: find symbols called by a symbol.

Parameters:

```json
{
  "symbol_id": "integer",
  "max_results": "integer|null",
  "min_confidence": "number"
}
```

Suggested initial settings defaults:

```text
max_results: 30
min_confidence: 0.3
```

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

### 8.14 `find_tests`

Purpose: find likely tests for a file or symbol.

Parameters:

```json
{
  "path": "string|null",
  "symbol_id": "integer|null",
  "max_results": "integer|null"
}
```

Suggested initial settings defaults:

```text
max_results: 20
```

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

Notes:

- Use naming conventions, paths, and references.
- It is fine if this is approximate.

### 8.15 `project_overview`

Purpose: return a compact map of the project.

Parameters:

```json
{
  "max_files": "integer|null",
  "max_symbols": "integer|null",
  "include_tests": "boolean"
}
```

Suggested initial settings defaults:

```text
max_files: 30
max_symbols: 30
include_tests: true
```

Return data:

```json
{
  "root": "/path/to/project",
  "languages": [
    {"language": "cpp", "files": 120, "bytes": 1800000}
  ],
  "important_files": [
    {"path": "src/main.cpp", "pagerank": 0.92},
    {"path": "src/agent.cpp", "pagerank": 0.86}
  ],
  "entry_points": [
    {"symbol_id": 1, "qualified_name": "main", "path": "src/main.cpp", "line": 35}
  ],
  "likely_test_commands": ["make test"],
  "index_fresh": true
}
```

### 8.16 `inspect_code_task`

Purpose: macro-tool that gives the model a compact, ranked context bundle for a coding task.

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

Suggested initial settings defaults:

```text
max_symbols: 20
max_files: 12
include_skeletons: true
include_tests: true
max_bytes: 80000
```

Return data:

```json
{
  "query": "add agentic mode",
  "likely_symbols": [
    {
      "symbol_id": 44,
      "qualified_name": "ChatSession::run",
      "path": "src/chat.cpp",
      "start_line": 80,
      "end_line": 210,
      "reason": "chat loop likely related to agent loop"
    }
  ],
  "likely_files": ["src/chat.cpp", "src/openai.cpp", "src/editor.cpp"],
  "suggested_reads": [
    {"path": "src/chat.cpp", "start_line": 80, "end_line": 210}
  ],
  "skeletons": [],
  "likely_tests": [],
  "truncated": false
}
```

Notes:

- This tool is intended to reduce repeated `list_directory`, `search_text`, and `read_file` calls.
- It can internally combine symbol search, text search, PageRank, recent files, and path heuristics.

### 8.17 `edit_file`

Purpose: primary structured editing tool.

Parameters:

```json
{
  "path": "string",
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
      "line_range_hint": {
        "start_line": "integer",
        "end_line": "integer"
      }
    }
  ],
  "atomic": "boolean",
  "create_dirs": "boolean"
}
```

Suggested initial settings defaults:

```text
atomic: true
create_dirs: false
```

Supported operations:

#### `replace_range`

Replaces lines `start_line` through `end_line` inclusive.

Required:

```text
start_line
end_line
replacement
```

Recommended:

```text
expected_hash
```

Behavior:

- If `expected_hash` is supplied, compare it to the current hash of the target range.
- If it does not match, fail with `stale_range` and return the current range preview.
- This is the preferred edit mode for most modifications.

#### `insert_at`

Inserts text before line `line`.

Required:

```text
line
new_text
```

Optional:

```text
expected_hash
```

#### `delete_range`

Deletes lines `start_line` through `end_line` inclusive.

Required:

```text
start_line
end_line
```

Recommended:

```text
expected_hash
```

#### `replace_text`

Finds `old_text` and replaces it with `new_text`.

Required:

```text
old_text
new_text
```

Optional:

```text
replace_all
line_range_hint
```

Behavior:

- Exact match first.
- If exact match fails, try fuzzy fallback as described in section 9.
- If multiple matches exist and `replace_all` is false, use `line_range_hint` if supplied.
- If still ambiguous, fail with `ambiguous_match` and return candidate locations.

#### `replace_symbol`

Replaces an entire indexed symbol body.

Required:

```text
symbol_id
replacement
```

Recommended:

```text
expected_hash
```

Behavior:

- Resolve symbol to file and line range.
- Check `expected_hash` against `body_hash` or current range hash if supplied.
- Replace the symbol range.
- Reindex the affected file.

#### `create_file`

Creates a new file.

Required:

```text
new_text
```

Behavior:

- Fail if the file already exists.
- Use `write_file` internally.

Return data:

```json
{
  "path": "src/agent.cpp",
  "applied": true,
  "operations_applied": 2,
  "new_file_hash": "...",
  "reverse_patch_path": ".ainiux/history/2026-07-04T120000.patch",
  "index_updated": true,
  "summary": [
    "replaced lines 42-118",
    "inserted before line 7"
  ],
  "warnings": []
}
```

Failure examples:

```json
{
  "ok": false,
  "error": {
    "code": "stale_range",
    "message": "Expected range hash did not match current file."
  },
  "data": {
    "current_range_hash": "...",
    "current_preview": "120| ..."
  }
}
```

Notes:

- Apply multiple line operations from bottom to top to avoid line number shifts.
- For `atomic = true`, no partial edits should remain after failure.
- Record rollback data before applying changes.
- Reindex the affected file immediately after a successful edit.

### 8.18 `str_replace`

Purpose: compatibility editing tool used by many coding agents.

Parameters:

```json
{
  "path": "string",
  "old_text": "string",
  "new_text": "string",
  "replace_all": "boolean",
  "line_range_hint": {
    "start_line": "integer",
    "end_line": "integer"
  },
  "fuzzy": "boolean"
}
```

Suggested initial settings defaults:

```text
replace_all: false
line_range_hint: null
fuzzy: true
```

Return data:

```json
{
  "path": "src/example.cpp",
  "matches_found": 1,
  "replacements_made": 1,
  "match_mode": "exact|normalized_whitespace|indent_stripped",
  "new_file_hash": "...",
  "reverse_patch_path": ".ainiux/history/...patch",
  "index_updated": true
}
```

Behavior:

1. Try exact match.
2. If exact match fails and `fuzzy = true`, try normalized whitespace matching.
3. If that fails, try leading-indent-stripped matching.
4. If multiple matches exist and `replace_all = false`, use `line_range_hint` if supplied.
5. If still ambiguous, fail with `ambiguous_match` and return candidate locations.
6. If no match exists, fail with `not_found` and return nearby possible matches if cheap.

Notes:

- `str_replace` should internally use the same engine as `edit_file.replace_text`.
- This exists for compatibility, but `edit_file.replace_range` should be preferred when possible.

### 8.19 `apply_patch`

Purpose: compatibility tool for patch-style editing.

Parameters:

```json
{
  "patch": "string",
  "atomic": "boolean",
  "fuzzy": "boolean"
}
```

Suggested initial settings defaults:

```text
atomic: true
fuzzy: false
```

Return data:

```json
{
  "applied": true,
  "files_changed": ["src/a.cpp", "src/b.cpp"],
  "operations_applied": 4,
  "new_hashes": {
    "src/a.cpp": "...",
    "src/b.cpp": "..."
  },
  "reverse_patch_path": ".ainiux/history/...patch",
  "index_updated": true,
  "warnings": []
}
```

Behavior:

- Parse OpenAI-style apply-patch input.
- Support add, update, and delete file operations.
- Validate paths before applying.
- Apply atomically by default.
- Reindex changed files.

Notes:

- Keep this for compatibility with models trained on patch tools.
- Prefer structured `edit_file` internally where possible.


### 8.20 `fetch_url`

Purpose: fetch a web page or raw URL content using libcurl.

Parameters:

```json
{
  "url": "string",
  "method": "GET|HEAD",
  "headers": {"Header-Name": "value"},
  "max_bytes": "integer|null",
  "timeout_ms": "integer|null",
  "follow_redirects": "boolean",
  "extract_text": "boolean",
  "include_headers": "boolean"
}
```

Suggested initial settings defaults:

```text
method: GET
headers: {}
max_bytes: settings.limits.fetch_url.max_bytes
timeout_ms: settings.limits.fetch_url.timeout_ms
follow_redirects: true
extract_text: true
include_headers: false
```

Return data:

```json
{
  "url": "https://example.com/page",
  "final_url": "https://example.com/page",
  "status": 200,
  "content_type": "text/html; charset=utf-8",
  "title": "Example page",
  "text": "Extracted readable text when extract_text is true...",
  "body": "Raw body when extract_text is false or content is not HTML...",
  "headers": {},
  "bytes_read": 12345,
  "truncated": false
}
```

Notes:

- Use libcurl, which ainiux already depends on.
- Refuse unsupported schemes by default. Allow `http` and `https`; keep `file`, `ftp`, and other schemes disabled unless explicitly enabled in settings.
- Keep output bounded. HTML extraction can be simple and dependency-free: strip scripts/styles, decode the most common entities, collapse whitespace, and return title/headings/paragraph-ish text first.
- This tool is for fetching known URLs. Use `search_web(term)` for discovery.

### 8.21 `search_web`

Purpose: search the web using a configured search provider and return compact results.

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

Suggested initial settings defaults:

```text
max_results: settings.limits.search_web.max_results
timeout_ms: settings.limits.search_web.timeout_ms
site: null
freshness_days: null
fetch_top_results: false
```

Return data:

```json
{
  "term": "sqlite fts5 trigram tokenizer",
  "results": [
    {
      "title": "Result title",
      "url": "https://example.com/result",
      "snippet": "Short provider snippet...",
      "rank": 1,
      "fetched_text": null
    }
  ],
  "provider": "configured",
  "truncated": false
}
```

Notes:

- The name `search_web` is intentionally plain because many LLMs already understand this style of tool.
- Do not hardcode one commercial search provider. Support a configurable provider, for example SearXNG, Brave, Bing, Google Programmable Search, or a generic JSON endpoint.
- If no provider is configured, return a clear `web_search_unavailable` error instead of pretending to search.
- If `site` is set, transform the query into a site-restricted search if the configured provider supports it.
- If `fetch_top_results` is true, internally call the same bounded fetch logic as `fetch_url` for the first few results, controlled by settings.

### 8.22 `run_command`

Purpose: execute shell commands such as compilers, tests, Python scripts, make, and git.

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

Suggested initial settings defaults:

```text
cwd: workspace root
timeout_ms: 120000
stdin: null
env: null
max_output_bytes: 12000
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
  "output_summary": "2 compiler errors in src/agent.cpp"
}
```

Notes:

- Do not use `system()`.
- Use `posix_spawn`/`fork+exec` on Unix-like systems and `CreateProcess` on Windows.
- Kill the process group on timeout.
- Cap output aggressively.
- After commands that may modify files, mark the index as possibly stale and cheaply check changed files.

### 8.23 `git_status`

Purpose: return compact git status.

Parameters:

```json
{
  "short": "boolean",
  "include_branch": "boolean"
}
```

Suggested initial settings defaults:

```text
short: true
include_branch: true
```

Return data:

```json
{
  "is_repo": true,
  "branch": "main",
  "dirty": true,
  "files": [
    {"path": "src/agent.cpp", "status": "M"},
    {"path": "src/tools.cpp", "status": "??"}
  ]
}
```

Implementation:

- Use git command line.
- Typical command: `git status --short --branch`.

### 8.24 `git_diff`

Purpose: return bounded git diff.

Parameters:

```json
{
  "path": "string|null",
  "cached": "boolean",
  "stat": "boolean",
  "max_bytes": "integer|null"
}
```

Suggested initial settings defaults:

```text
path: null
cached: false
stat: false
max_bytes: 50000
```

Return data:

```json
{
  "diff": "diff --git ...",
  "stat": "src/agent.cpp | 42 +++++---",
  "truncated": false
}
```

Implementation:

- Use git command line.
- Typical commands:
  - `git diff -- PATH`
  - `git diff --stat`
  - `git diff --cached`

### 8.25 `index_status`

Purpose: return index freshness and statistics.

Parameters:

```json
{
  "check_filesystem": "boolean",
  "max_changed_files": "integer|null"
}
```

Suggested initial settings defaults:

```text
check_filesystem: true
max_changed_files: 50
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

### 8.26 `index_update`

Purpose: update changed files only.

Parameters:

```json
{
  "paths": ["string"],
  "force": "boolean"
}
```

Suggested initial settings defaults:

```text
paths: []
force: false
```

Behavior:

- If `paths` is empty, detect changed files cheaply.
- If `paths` is non-empty, update only those paths.
- If `force = true`, rescan even if timestamp/hash appears unchanged.

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

### 8.27 `index_rebuild`

Purpose: full rebuild of the project index.

Parameters:

```json
{
  "confirm": "boolean"
}
```

Suggested initial settings defaults:

```text
confirm: false
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

Notes:

- This is mainly a recovery/debug command.
- Normal operation should use incremental updates.

## 9. Fuzzy edit fallback

Failed edits are expensive because each failure often requires another inference round trip. Therefore `str_replace` and `edit_file.replace_text` should support a Gemini-style fuzzy fallback.

### 9.1 Fallback order

When replacing text:

1. Exact byte-for-byte match.
2. Normalized whitespace match.
3. Leading-indent-stripped match.
4. Fail with useful diagnostics.

### 9.2 Normalized whitespace matching

Normalized whitespace matching should compare strings after normalizing runs of whitespace.

Conceptually:

```text
"foo(  a,\n b )" -> "foo( a, b )"
```

This should preserve enough mapping back to original offsets so the exact source range can be replaced.

### 9.3 Leading-indent-stripped matching

For multi-line snippets, compare after stripping common leading indentation from both old text and candidate text.

This handles common model failures such as:

- one extra indentation level
- tabs vs spaces near the left margin
- snippets copied without surrounding indentation

### 9.4 Multiple matches

If multiple matches are found:

- If `replace_all = true`, replace all matches.
- Else, if `line_range_hint` is supplied, choose the match inside or closest to that range.
- Else, fail with `ambiguous_match` and return candidate line numbers.

### 9.5 Safety rules

Fuzzy matching should not silently make risky edits.

Rules:

- Prefer exact match over fuzzy match.
- Report `match_mode` in the result.
- If the fuzzy match is too weak, fail.
- If multiple candidates remain, fail unless `replace_all = true` or `line_range_hint` disambiguates.
- Return candidate locations on ambiguity.

### 9.6 Why this matters

The model often gets whitespace or indentation slightly wrong. Turning those failures into successful edits saves:

- one or more extra LLM turns
- extra tool calls
- extra file reads
- user frustration

This is likely a large practical latency and token-efficiency win.

## 10. Configurable output limits

Every tool should have output limits, but those limits must come from settings rather than hidden hardcoded constants. The values below are suggested initial settings, not fixed behavior. Users should be able to override them globally, per project, per session, and per tool call.

Suggested initial settings:

```text
limits.read_file.max_lines:              500
limits.read_file.max_bytes:              50000
limits.read_many.max_total_bytes:        80000
limits.get_skeleton.max_items:           300
limits.get_skeleton.max_bytes:           40000
limits.search_text.max_hits:             80
limits.search_text.max_bytes:            60000
limits.search_symbol.max_results:        20
limits.read_symbol.max_bytes:            50000
limits.run_command.timeout_ms:           120000
limits.run_command.max_output_bytes:     12000
limits.project_overview.max_files:       30
limits.project_overview.max_symbols:     30
limits.git_diff.max_bytes:               50000
limits.list_directory.max_entries:       300
limits.fetch_url.timeout_ms:             30000
limits.fetch_url.max_bytes:              200000
limits.search_web.max_results:           10
limits.search_web.timeout_ms:            30000
```

When output is truncated, return:

- `truncated = true`
- which setting or tool parameter caused truncation
- how much was omitted if known
- the most useful head/tail portions where appropriate

For command output, prefer compiler/test error extraction over dumping massive logs.

## 11. Slash commands

Ship these commands in the first serious agent version:

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
/status
/diff
/undo
/memory
/index status
/index update
/index rebuild
/index explain PATH
/resume
/clear
```

### 11.1 `/compact`

Supported forms:

```text
/compact
/compact auto
/compact off
```

Compaction should preserve:

- original user goal
- current plan
- completed steps
- files edited
- exact test commands and results
- important errors
- design decisions
- open TODOs
- active file/symbol IDs
- current git diff summary

Compaction should discard:

- old tool chatter
- full file dumps already no longer needed
- repeated compiler output
- failed search paths
- superseded plans

### 11.2 `/status`

Show:

- workspace root
- git branch
- dirty files
- current model
- approximate context usage
- index freshness
- last test result

### 11.3 `/undo`

Undo the last ainiux-applied edit batch using reverse patch or saved file snapshots.

### 11.4 Ctrl-C behavior

Use two-stage interrupt behavior:

```text
First Ctrl-C: interrupt current model stream or running command.
Second Ctrl-C: abort current agent task and return to ainiux prompt.
```

## 12. Session memory

Use append-only `session.jsonl` as the durable source of session history.

Example events:

```json
{"type":"user_goal","text":"Add agent mode"}
{"type":"tool_call","name":"search_symbol","args":{"query":"agent loop"}}
{"type":"edit","files":["src/agent.cpp"],"reverse_patch":".ainiux/history/001.patch"}
{"type":"test","command":"make test","exit_code":0}
{"type":"decision","text":"Use replace_range as primary edit primitive"}
```

Generate `.ainiux/memory.md` from the event log.

Suggested memory format:

```markdown
# Project Memory

## Current goal
Implement agent mode for ainiux.

## Decisions
- Use range replacement with expected hashes as the primary edit primitive.
- Use apply_patch and str_replace for compatibility.
- Use git command line instead of libgit2.

## Modified files
- src/agent.cpp: added initial agent loop.
- src/tools.cpp: added file editing tools.

## Known issues
- No sandbox yet.
- C++ scanner misses complex macro-generated methods.

## Next likely actions
- Run narrow tests.
- Update command help.
```

Update memory after meaningful milestones, not after every tiny tool call.

## 13. Rollback and edit history

Even without heavy sandboxing, ainiux should keep cheap rollback data.

Before each edit batch, store at least one of:

- reverse patch
- copy of touched files
- pre-edit hash and text ranges

Recommended path:

```text
.ainiux/history/YYYYMMDD-HHMMSS-NNN.patch
```

Every successful edit result should include:

- files changed
- operation count
- new hashes
- reverse patch path
- whether index update succeeded

This enables `/undo`, easier debugging, and better trust.

## 14. Git integration

Use git through the command line.

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

Do not add libgit2 in the first implementation.

Reasons:

- Git CLI is already available in most development environments.
- It respects existing user config and repository behavior.
- It avoids another dependency.
- Process startup is not expected to be the main bottleneck.

## 15. Command execution safety basics

Even without a full sandbox, basic hygiene is required.

Rules:

- Canonicalize paths.
- Refuse file edits outside workspace root.
- Detect binary files before text edits.
- Cap command output.
- Use timeouts.
- Kill process groups on timeout.
- Keep edit history.
- Provide `/undo`.

This avoids obvious footguns without constant permission prompts.

## 16. Provider-neutral tool adapter

Internally, tools should be represented independently of any specific LLM provider.

Adapters can translate this into:

- OpenAI-compatible tool calls
- Anthropic-style tool calls
- Gemini-style function calls
- text fallback for weak/local models

For weaker models or OpenAI-compatible endpoints that do not reliably support native tool calls, support a text fallback format such as:

```text
<tool_call name="read_file">
{"path":"src/main.cpp","start_line":1,"end_line":120}
</tool_call>
```

Then parse it locally and return tool results in a similarly clear wrapper.

## 17. Minimal AGENTS.md draft

This draft merges the short ainiux-specific tool guidance with the user-supplied planning, coding, testing, design, and refactoring rules. Keep the real file short; the goal is a concise agent system prompt, not a huge framework prompt.

```markdown
# AGENTS.md

Use ainiux's project index first, but treat it as fast guidance, not truth. The scanner is deliberately 80/20 and can miss symbols, confuse overloads, ignore macro-generated code, or return stale line ranges. Never blindly trust `get_skeleton`, `search_symbol`, PageRank, or call graph data. When the indexed view looks incomplete, surprising, or inconsistent with the task, verify with `read_symbol`, `read_many`, `search_text`, `find`, `git grep`, compiler output, tests, or targeted shell commands.

Prefer ainiux-specific tools in this order:

1. `inspect_code_task(query)` for a compact ranked map of likely files, symbols, and tests.
2. `search_symbol(query)` for functions, classes, methods, files, and likely call links.
3. `get_skeleton(path)` for signatures and doc comments before reading a large file.
4. `read_symbol(symbol_id)` or `read_many(...)` for exact source context.
5. `edit_file.replace_range` with `expected_hash` for most edits.
6. `edit_file.replace_symbol` when replacing a known indexed function or method.
7. `str_replace` for small compatibility edits; use fuzzy fallback if exact text fails.
8. `apply_patch` for patch-style multi-file edits.
9. `run_command` for narrow tests, builds, formatters, git, grep, and fallback inspection.
10. `fetch_url(url)` and `search_web(term)` only when current external information is needed.

Keep context small. Do not read whole files when a skeleton, symbol body, line range, or search result is enough. Prefer batched reads and batched edits over many small tool calls. Do not rewrite unrelated code. After edits, run the narrowest useful test or build command. If the first tool choice fails for a non-permission and non-network reason, do not keep retrying blindly; use a different route.

Planning: make concrete, bite-sized plans with exact files, commands, expected outputs, and verification steps. If the user gave enough information, proceed instead of asking. Follow DRY, YAGNI, and KISS. Use the standard library or native platform feature unless there is a clear reason not to. Avoid new dependencies, boilerplate, and abstractions that do not simplify the implementation.

Coding: use clear names, constants instead of magic values, explicit error handling, and simple readable control flow. Short readable code is better than clever framework code. If one readable line is enough, use one line. Preserve existing style unless changing it is part of the task.

Testing: prefer TDD. Write or identify a failing test, run it, implement the smallest fix, and rerun. Use unit tests, fixtures, mockups, and integration tests where they fit. Include normal and failure cases: Unicode such as ÄÖ, Chinese, Arabic, Russian and emoji; empty strings; very long strings; zero and small negative/positive numbers; very large numbers; invalid numeric and URL input; corrupted files; missing permissions; and network failures. After feature work, bug fixes, refactoring, or behavioral changes, rerun relevant tests.

Web/UI work: default to responsive, mobile-friendly HTML5 with UTF-8. Support light and dark themes where appropriate. Maintain WCAG 2.1 contrast for text, links, and visited links. Use standard controls, keyboard shortcuts, and icons unless asked otherwise. Make designs look good, but never at the expense of usability or contrast.

Refactoring: remove duplication, improve names, extract helpers only when they simplify the code, simplify expressions, shorten code where it stays readable, and keep behavior unchanged. Run tests after refactoring. If tests fail, roll back or proceed in smaller steps.
```

## 18. Implementation milestones

### Milestone 1: minimal agent loop

Implement:

```text
read_file
read_many
write_file
remove
list_directory
search_text
find
fetch_url
search_web
edit_file.replace_range
edit_file.replace_text
str_replace
apply_patch
run_command
git_status
git_diff
/status
/diff
/undo
/compact
```

Success criteria:

- Agent can inspect files.
- Agent can edit files safely with rollback.
- Agent can run build/test commands.
- Agent can compact context.
- Agent can show diff/status.

### Milestone 2: fast SQLite code index

Implement:

```text
files
symbols
refs
symbols_fts
incremental scan
index_status
index_update
index_rebuild
get_skeleton
search_symbol
read_symbol
project_overview
HTML5/CSS/Rust/Go/GNU-assembler scanners
embedded JavaScript extraction from HTML5
```

Success criteria:

- Changed ainiux-edited files are reindexed immediately.
- External changes are detected by timestamp plus hash.
- Skeletons and symbol search are fast enough for normal interactive use.

### Milestone 3: task inspection and ranking

Implement:

```text
inspect_code_task
find_callers
find_callees
find_tests
PageRank ranking
recent-file boosts
```

Success criteria:

- The agent usually finds relevant files without repeated directory listing.
- The agent uses fewer full-file reads.
- The model reaches the first useful edit faster.

### Milestone 4: memory and long-horizon sessions

Implement:

```text
session.jsonl
memory.md generation
/plan
/resume
/memory
better /compact
```

Success criteria:

- Long tasks can be resumed.
- Compaction preserves important facts.
- Project decisions are retained.

### Milestone 5: benchmark and tuning

Track:

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
test_runs
first_test_pass_rate
final_diff_lines
```

Most important metrics:

```text
model turns per task
edit failure rate
tokens to first correct patch
first-test-pass rate
search/read token volume
```

Use benchmark mode to tune:

- configurable tool output caps
- symbol ranking
- fuzzy edit thresholds
- context compaction behavior
- scanner regexes

## 19. Preferred edit strategy

Use this order:

1. `edit_file.replace_range` with `expected_hash`.
2. `edit_file.replace_symbol` with `symbol_id` and `expected_hash`.
3. `apply_patch` for multi-file compatibility patches.
4. `str_replace` for small exact or fuzzy text replacements.
5. `write_file` only for new files or complete rewrites.

The main reason is reliability. Each failed edit can cost another model round trip.

## 20. Preferred context strategy

Use this order:

1. `inspect_code_task` for initial orientation.
2. `search_symbol` for targeted symbol discovery.
3. `get_skeleton` for token-cheap file overview.
4. `read_symbol` for exact implementation details.
5. `read_many` for batched ranges.
6. `read_file` for full files only when needed.
7. `search_text`/`find` as fallback.
8. `fetch_url`/`search_web` when current external information is required.
9. Shell tools through `run_command` as final fallback.

This should make ainiux faster and more token-efficient than agents that primarily list directories, grep blindly, and read whole files.

## 21. Summary

The first serious ainiux agent mode should be built around:

- fast C++17 `std::regex`/pattern scanning
- priority scanners for C/C++/C#/Python/JavaScript/TypeScript/Java/PHP/Perl/HTML5/CSS/Rust/Go/GNU assembler
- embedded JavaScript extraction from HTML5
- SQLite3 WAL index
- optional FTS5 and FTS5 trigram use for fast DB-backed search
- incremental per-file updates
- settings-driven limits instead of hardcoded caps
- token-efficient skeleton and symbol tools
- `fetch_url` and `search_web` web tools
- range edits with expected hashes
- fuzzy fallback for text replacement
- apply_patch and str_replace compatibility
- git command-line integration
- bounded command output
- cheap rollback and `/undo`
- append-only session log plus generated memory

This fits the desired 80/20 approach: extremely fast on common cases, simple to reason about, dependency-light, and still able to fall back to slower generic tools when needed.
