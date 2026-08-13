# Agent-mode native tool inventory (v1.18)

Current advertised names from `ReadToolRegistry::definitions()` in
`src/agent/tools.cpp`. Silent execute aliases are gone: old names such as
`read_file`, `run_command`, `list_dir`, `search_text`, and `str_replace` are
**unknown_tool**.

Act and Plan still send the same definition list. Plan denies `mkdir`, `mv`,
and `rm` at execute time. Index tools (`index`, `outline`, `symbol`) are omitted
when no completed code index is present.

Token estimates use Ainiux `estimate_tokens_from_text` (`ceil(bytes/4)` plus 8
wrapper tokens per tool). See the previous revision of this file for the
methodology.

## Advertised set (Act/Plan, index on, network on)

| Tool | Required | Role |
| --- | --- | --- |
| `index` | — | Index summary (languages, counts, freshness). Hidden without an index. |
| `ls` | — | Real directory listing. Prefer before `rm`. |
| `glob` | `pattern` | Eligible source path match. |
| `grep` | `query` | Content search; `path` + `glob` combine. |
| `symbol` | `query` | Ranked indexed definitions. Hidden without an index. |
| `outline` | `path` | Declarations in one file. Hidden without an index. |
| `read` | `path` **or** `items` | One file, or batch 1–100 ranges (`items`). Images: `attach`. |
| `run` | `command` | Shell-free argv exec. Smart auto-allows classified in-project `mkdir`/`rmdir`/`rm`/`mv`; asks for non-empty `rm -r`. |
| `fetch` | `url` | HTTP(S) → Markdown/text. Network sessions only. |
| `web_search` | `term` | At most 3 search hits. Network sessions only. |
| `goal_met` | `evidence` | Complete an active `/goal`. |
| `attach` | `path` | Queue one local PNG/JPEG/GIF for this turn. |
| `edit` | `path`, `ops` | Preferred in-file edit. |
| `write` | `path`, `content` | Create/overwrite a file. |
| `mkdir` | `path` | Act-only directory create. |
| `mv` | `source`, `destination` | Act-only rename; dest must not exist. |
| `rm` | `path` | Act-only **regular file** delete. Directories: `run rmdir` or `run rm -r`. |
| `apply_patch` | — | Codex/OpenAI multi-file patch. |

## Removed (not advertised, not executable)

`str_replace`, `git_status`, `git_diff`, `read_symbol`, `read_many` (batching lives on `read.items`), `search_text`, `find`, `list_directory`, `list_dir`, `project_overview`, `get_skeleton`, `search_web`, `read_file`, `run_command`, `edit_file`, `write_file`, `create_directory`, `rename_path`, `remove`, `fetch_url`, `attach_image`, `index_overview`, `file_outline`, `search_symbol`.

## Smart `run` filesystem commands

Classified by `assess_workspace_fs_command`:

- Auto-allow in-project: `mkdir`/`mkdir -p`, `rmdir`, `rm` (files), `mv`, and `rm -r` of an **empty** directory.
- Ask once: `rm -r` / `rm -rf` when any operand is a **non-empty** directory.
- Confirm still asks for every `run`. Yolo asks nothing.
- Headless Ask → Deny (non-empty tree delete stays blocked in `ainiux run`).
- Windows `cmd` is still denied. POSIX `rmdir` is not treated as Windows `rd`.
