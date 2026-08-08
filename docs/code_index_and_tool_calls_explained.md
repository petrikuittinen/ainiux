# Code index database and tool calls

Internal reference for what Ainiux stores in the project code index and how agent tools use it (or fall back without it). Based on `src/agent/index/` and `src/agent/tools.cpp` as of the v1.1 definitions-only index (schema version **4**, scanner version **5**).

This is a navigation aid for humans and agents optimizing the index. User-facing behavior is in [Agent workflows](agent.md); architecture constraints live in [AGENTS.md](../AGENTS.md) and [Decisions](decisions.md).

---

## 1. Purpose and scope

- **Location:** `<workspace>/.ainiux-pr/index.sqlite` (WAL mode). Separate from the user chat library `~/.ainiux/ainiux.db`.
- **Role:** Fast, optional **definitions-only** hint source. Never ground truth. Models and tools must still verify against live source before edits.
- **What it is not:**
  - No references / call edges / caller counts
  - No graph scores or PageRank
  - No full source text of files
  - No SQLite FTS / inverted index over free-form file contents
  - No automatic request-context hint cache

Older schemas that stored `refs` and `symbol_scores` are **dropped** on migration to the current lightweight schema.

CLI entry points: `--index-code`, `--print-index`, `--clear-index`, Index lifecycle is CLI/mutation-driven (no agent `index_*` tools). Agent can run with `--disable-indexing` so the index is never probed or touched.

---

## 2. Database schema

Three tables only: `metadata`, `files`, `symbols`.

### 2.1 `metadata` (key → value)

| Key | Purpose |
| --- | --- |
| `schema_version` | Currently `4` |
| `scanner_version` | Currently `5` |
| `ignore_fingerprint` | Detect root ignore-rule changes |
| `max_source_code_file_size` | Size bound used when indexed |
| `workspace` | Absolute workspace root |
| `updated_at` | Last completed index time |
| `complete` | Snapshot finished successfully (`1`) |

### 2.2 `files`

Richer than a minimal id / path / hash / timestamp table:

| Column | Notes |
| --- | --- |
| `id` | Primary key |
| `path` | Workspace-relative, **UNIQUE** |
| `language` | Scanner language name |
| `size` | File size in bytes |
| `mtime_ns` | mtime for cheap “unchanged” skips |
| `content_hash` | FNV-based hex of file content |
| `line_count` | Line total when scanned |
| `scan_status` | e.g. `indexed` vs skip states |
| `scan_error` | Reason when not fully indexed |
| `indexed_at` | When this file row was written |

Skipped / binary / unreadable / oversized sources can still appear as rows with non-`indexed` status and an error string.

### 2.3 `symbols` (definitions)

| Column | Notes |
| --- | --- |
| `id` | Primary key |
| `file_id` | FK → `files(id)` **ON DELETE CASCADE** (not a path string) |
| `kind` | class, function, method, type, … |
| `name` | Simple name |
| `qualified_name` | Nested / scoped name |
| `signature` | Collapsed signature text |
| `parameters` | Extracted params when available |
| `return_type` | Extracted return when available |
| `line_start`, `line_end` | 1-based range in the file |
| `documentation` | Docstring / preceding comment when found |
| `signature_hash` | Hex hash of signature |
| `body_hash` | Hex hash of body range |
| `importance` | **0–100 static score** from declaration kind, visibility, and scope — **not** PageRank or a graph score |

In-memory `Symbol` / `IndexedSymbol` types in `index.hpp` match this payload.

### 2.4 SQLite indexes

| Index | Columns | Helps |
| --- | --- | --- |
| `files.path` | UNIQUE on `path` | File by path; `path IN (...)` |
| `symbols_file_source` | `(file_id, line_start, id)` | All symbols in a file, source order |
| `symbols_name` | `(name)` | Exact simple-name equality |
| `symbols_qualified_name` | `(qualified_name)` | Exact qualified-name equality |
| `symbols_kind` | `(kind)` | Filter by kind |
| PK / FK | `symbols.id`, `file_id` | By id; cascade deletes |

### 2.5 Lookup reality (important for optimization)

- **Group-by-file** is in good shape: `file_id` + compound source-order index. `query_symbols(..., paths)` uses `WHERE f.path IN (...)` then joins symbols.
- **Ranked name / task search is not index-pushdown today.** Agent `search_symbol` goes through `query_ranked_symbols`, which selects (or walks) symbols and scores them in C++ with lexical tiers (full name → exact identifier component → component-prefix; multi-token coverage; importance only as a tie-breaker). That is effectively **O(n) over definitions**, not log-time fuzzy search.
- B-tree indexes on `name` / `qualified_name` exist for equality-style access but are **not** what the ranked search path uses for filtering.
- There is **no FTS** for substrings inside signatures, docs, or source bodies.

### 2.6 Corrected mental model

| Common expectation | Actual |
| --- | --- |
| files: id, path, hash, timestamp | + language, size, mtime_ns, line_count, scan_status/error, indexed_at |
| symbols: id, file path FK, name, lines, “fuzzy pagerank” | FK is `file_id`; + kind, qualified_name, signature, params, return, docs, hashes; **static importance 0–100**, not PageRank |
| Only two tables | + **`metadata`** key/value |
| Extreme-fast name lookup | Indexes exist; **ranked search still scans definitions** |
| Group by files | Yes — well supported |

---

## 3. Access modes (how tools see the index)

| Mode | Index behavior |
| --- | --- |
| **Agent / Run / Plan, indexing on** (`IndexAccessMode::LazyHints`) | Short-lived read-only SQLite queries per tool; mutation overlay merges recent writes until a refresh generation completes. |
| **Agent with `--disable-indexing` / create_without_index** | Index-only tools are **hidden** (or return `indexing_disabled`). `glob` uses live discovery; `grep` prefers `rg` then live built-in scan. Project reads use the live filesystem. |
| **Security review** (`MutationPolicy::Disabled`, snapshot authorization) | Eager completed snapshot; many path tools are **authorization-bound** to indexed files. Mutations off. `grep` still prefers `rg` but post-filters to indexed paths. |
| **Lazy query failure for `glob` / `grep` only** | Falls back to `index::discover_source_files()` for the eligible set (same rules as indexing, no SQLite). `grep` may still use `rg` against that live set. |

Security-review and Agent share tool *names* but not always the same path authorization model for reads and `run_command`.

---

## 4. Shared tool result envelope

All registry tools return a JSON string shaped like:

```json
{
  "ok": true,
  "data": {},
  "error": null,
  "warnings": [],
  "truncated": false,
  "metadata": {}
}
```

On failure, `ok` is false and `error` is `{ "code": "...", "message": "..." }`. Below, **parameters** are tool arguments and **returns** describe the successful `data` payload.

---

## 5. Tool catalog

### 5.A Index-required tools

Hidden when indexing is disabled. **No live fallback** for the symbol/meta features themselves.

#### `index_overview` (silent execute alias: `project_overview`)

| | |
| --- | --- |
| **Params** | *(none)* |
| **Uses index** | Totals, files, up to 4096 symbols; freshness check |
| **Fallback** | None → `indexing_disabled` |
| **Returns** | `workspace`, `updated_at`, `languages[]` (`language`, `files`, `lines`, `bytes`), aggregate `files` / `lines` / `bytes`, `important_files[]`, `entry_points[]` (`path`, `symbol`, `line`), `important_symbols[]` (`path`, `symbol_id`, `symbol`, `line`, `importance`), `likely_test_commands[]`, `index_fresh` |

#### `search_symbol`

| | |
| --- | --- |
| **Params** | `query` (required), `max_results` (default 50, max 200) |
| **Uses index** | Ranked definition scan (`query_ranked_symbols` / `rank_task_symbols`) |
| **Fallback** | None |
| **Returns** | Array of `{ id, path, kind, name (qualified), signature, line_start, line_end, importance }` |

#### `file_outline` (silent execute alias: `get_skeleton`)

| | |
| --- | --- |
| **Params** | `path` (required) |
| **Uses index** | File must be `status=indexed`; symbols for that path |
| **Fallback** | None (`not_found` if not indexed) |
| **Returns** | Array of `{ id, kind, name, signature, line_start, line_end, documentation, importance }` |

#### `read_symbol`

| | |
| --- | --- |
| **Params** | `symbol_id` (required, ≥ 1) |
| **Uses index** | Symbol row → line range; then **reads live source** for that range |
| **Fallback** | None for lookup; content always from disk after id resolve |
| **Returns** | `{ symbol_id, path, line_start, line_end, content, file_hash, range_hash, importance }` |

#### `edit_file` op `replace_symbol` (only when indexing on)

| | |
| --- | --- |
| **Params** (inside op) | `symbol_id`, `replacement` / `new_text` / `text`, optional expected hash fields |
| **Uses index** | Resolve id → path/line range → rewrite live file as a range replace |
| **Fallback** | Op absent from schema when indexing disabled; external (outside-project) paths denied for this op |
| **Returns** | Via normal `edit_file` success shape (hashes, summary, `indexed_snapshot_updated`, etc.) |

---

### 5.B Path search: `glob` and `grep`

Same discovery eligibility as the indexer (editor language set, ignore rules, size caps). These tools do **not** use the symbol table for matching.

#### `glob`

| | |
| --- | --- |
| **Params** | `pattern` (required), `max_results` (def 200, max 1000) |
| **With index** | Match against indexed file paths in the snapshot |
| **Without / lazy fail** | `index::discover_source_files()` then same glob match |
| **Returns** | Array of path strings |

#### `grep` (silent execute aliases: `search_text`, `find`)

| | |
| --- | --- |
| **Params** | `query` (required; `pattern` is a compatibility alias), `regex`, `case_sensitive`, `word`, `path` (exact file), `glob` (file set; not with `path`), `context` (0–10), `max_results` (def 50, max 500) |
| **Backend order** | **1)** system **`rg`** if present; **2)** built-in scan over **indexed** candidates when indexing is on; **3)** built-in scan over **live discovery** when indexing is off or the index is unavailable |
| **Eligible set** | Index on → `status=indexed` paths from the snapshot/lazy query. Index off → live `discover_source_files()`. Exact `path` / tool `glob` further restrict. |
| **`rg` details** | Soft dependency resolved only on the fixed process PATH: `/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin` (not the caller’s `PATH`). Invoked via `run_argv` / `fork`+`execve` (shell-free), with `--no-config`, line-oriented flags, optional `-F`/`-i`/`-w`/`-C`, and a per-file `--max-count`. Hits are **post-filtered** to the eligible set so security-review and agent semantics match the built-in path. Cancel/timeout apply to the child. |
| **Fallback** | If `rg` is missing, fails, times out, or exits with status &gt; 1 → portable built-in scanner (open eligible files, `std::string::find` or ECMAScript `std::regex`). May add a short warning; never hard-fails solely because `rg` is absent. |
| **Portability** | No new library deps (still libcurl + sqlite3). Most Windows machines lack `rg`; they use builtin only. Invalid ECMAScript patterns still return `invalid_regex` before either backend runs. |
| **Returns** | Array of `{ path, line, text, context?: [{ line, text }] }` |
| **Metadata** | `search_backend`: `rg` \| `builtin_index` \| `builtin_live` |

---

### 5.C Live filesystem first (index optional / secondary)

#### `list_dir` (silent execute alias: `list_directory`)

| | |
| --- | --- |
| **Params** | `path` (optional, workspace-relative or approved external), `max_entries` (def 200, max 500) |
| **Uses index?** | Only sets `indexed: true/false` when a snapshot is present |
| **Fallback** | Always real `readdir` (empty dirs, non-source names, unusual filenames) |
| **Returns** | Array of `{ name, type, size, indexed, empty? }` |

#### `read_file`

| | |
| --- | --- |
| **Params** | `path` (required), `start_line` (def 1), `end_line` (def 0 = EOF), `max_bytes` (def 65536, max 262144) |
| **Security-review** | `read_source` → path must be an eligible **indexed** file |
| **Agent Act/Plan** | `read_workspace_source` → any safe project regular file; **need not be indexed** |
| **External** | Outside project with Guard approval |
| **Returns** | `{ path, line_start, line_end, content (line-numbered), file_hash, range_hash, bytes }` |

#### `read_many`

| | |
| --- | --- |
| **Params** | `items[]` (1–100 of the same shape as `read_file`), `max_bytes` aggregate (def/max 262144) |
| **Same dual path as `read_file`** | Index-bound in security-review; live FS in agent |
| **Returns** | Array of per-item range objects + metadata: `requested_items`, `returned_items`, `byte_cap`, `bytes_remaining` |

#### Mutations that **update** the index when present

These operate on the live filesystem in Act/Plan. If indexing is enabled they rescan/enqueue touched paths into the overlay and persistent refresh:

- `write_file`, `edit_file` (non-symbol ops), `str_replace`, `apply_patch`, `create_directory`, `rename_path`, `remove`
- After non-read-only `run_command`, a full-tree freshness pass may be queued

`replace_symbol` is the only edit op that **requires** symbol rows.

---

### 5.D Not primarily index tools

| Tool | Role |
| --- | --- |
| `run_command` | Argv exec (no shell). Security-review may **filter stdout paths** to the indexed set; agent uses validated live project paths. |
| `git_status`, `git_diff` | Git CLI only |
| `fetch_url`, `web_search` | Network (when allowed); unrelated to the code index |

---

## 6. Quick matrix

| Tool | When index exists | If index missing / disabled |
| --- | --- | --- |
| `index_overview` | SQLite totals + symbols | **Unavailable** |
| `search_symbol` | Ranked definitions | **Unavailable** |
| `file_outline` | Definitions for file | **Unavailable** |
| `read_symbol` | Id → range; content from disk | **Unavailable** |
| `edit_file.replace_symbol` | Id → lines | **Op removed / error** |
| `glob` | Indexed path list | **Live `discover_source_files`** |
| `grep` (+ silent `search_text`/`find`) | `rg` if present → else builtin over indexed candidates | `rg` if present → else builtin over live discovery |
| `list_dir` | Annotates `indexed` only | **Same readdir** |
| `read_file` / `read_many` | Security-review: index auth; Agent: live | **Agent: live FS** |
| Writes / non-RO command | Refresh / overlay side effects | No index side effects |

---

## 7. Optimization notes

1. **Symbol search is not SQL-indexed ranking** — `search_symbol` scores definitions in process (lexical tiers + importance tie-break), not via FTS or pushed-down `WHERE name = ?` ranking.
2. **`grep` never searches the symbol table** — eligible paths only; string matching is `rg` (preferred) or a built-in full-file scan of those candidates.
3. **`rg` is the big win for common agent loops** — multi-file rare needles drop from hundreds of ms (builtin open/scan) to tens of ms when `rg` is installed. Keep the builtin path first-class for hosts without `rg`.
4. **Lazy agent path** loads only what each tool needs (e.g. `file_outline` symbols for one path; `search_symbol` a ranked slice), but ranking still walks many rows.
5. **Dual discovery code paths** — indexed snapshot vs `discover_source_files()` share eligibility rules; changing one should keep the other consistent (including `grep` post-filters).
6. **Security-review vs agent** is the sharp edge for `read_*` and `run_command` path scope — same tool name, different authorization model.
7. **Static `importance` is cheap declaration metadata**, not a graph measure; graph tables were intentionally removed in the v1.1 lightweight schema.

---

## 8. Source pointers

| Area | Path |
| --- | --- |
| Schema constants, public types | `src/agent/index/index.hpp` |
| CREATE TABLE, refresh, queries, ranking | `src/agent/index/index.cpp` |
| Language scanners | `src/agent/index/scanner.cpp`, `scanner_extra.cpp` |
| Tool registry, execute paths, envelopes | `src/agent/tools.cpp`, `src/agent/tools.hpp` |
| Process spawn, fixed PATH, `run_argv`, `ripgrep_available` | `src/agent/process.cpp`, `src/agent/process.hpp` |
| Product / agent constraints | `AGENTS.md` |
| Design notes (project-local index) | `docs/decisions.md` |
| Roadmap v1.1 index notes | `PLANS.md` |
| User-facing index CLI | [Agent workflows](agent.md#code-index) (`--index-code`, `--print-index`, `--clear-index`) |

---

## 9. Related CLI (non-tool)

| Flag / command | Behavior |
| --- | --- |
| `--index-code` | Create or incrementally refresh `.ainiux-pr/index.sqlite` |
| `--print-index` | Markdown dump of the stored snapshot |
| `--clear-index` | Remove index DB + WAL/SHM sidecars only |
| `--disable-indexing` | Agent/Run/Plan without probing or mutating the index |
| `--security-review` | Refreshes index, then uses snapshot as authorization boundary for reads |

Multi-file discovery/scanning uses `floor(online_cores × 0.80)` workers (bounded by work; single-file scan is inline). Unchanged size/mtime pairs are not reopened. Snapshot replace is transactional so cancellation keeps the previous completed database.
