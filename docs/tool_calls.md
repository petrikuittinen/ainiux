# Agent-mode native tool inventory (v1.18)

Current advertised names from `ReadToolRegistry::definitions()` in
`src/agent/tools.cpp`. Silent execute aliases are gone: old names such as
`read_file`, `run_command`, `list_dir`, `search_text`, and `str_replace` are
**unknown_tool**.

Act and Lead send the same logical definition list and have the same execution authority.
Index tools (`index`, `outline`, `symbol`) are omitted
when no completed code index is present.

Token estimates use Ainiux `estimate_tokens_from_text` (`ceil(bytes/4)` plus 8
wrapper tokens per tool). See the previous revision of this file for the
methodology.

## Advertised set (Act/Lead, index on, network on)

| Tool | Required | Role |
| --- | --- | --- |
| `index` | — | Index summary (languages, counts, freshness). Hidden without an index. |
| `ls` | — | Real directory listing. Prefer before `rm`. |
| `glob` | `pattern` | Live workspace path match for any file type. |
| `grep` | `query` | Live UTF-8 content search of any file type; `path` + `glob` combine; use `offset` with returned `next_offset` for pagination. `context` above 25 is clamped. Invalid inferred `\|` patterns search literal branches. |
| `symbol` | `query` | Ranked indexed definitions. Hidden without an index. |
| `outline` | `path` | Declarations in one file. Hidden without an index. |
| `read` | `path` **or** `items` | One file, or batch 1–100 ranges (`items`); defaults to 128 KiB and returns `next_start_line` when truncated. Inverted `start_line`/`end_line` pairs are swapped with a warning. Images: `attach`. |
| `skill` | `name` | Read an active skill's `SKILL.md`, or a supporting UTF-8 file using optional relative `path`. Hidden when no skills are active; confined to selected skill directories. |
| `run` | `command` | Shell-free argv exec. Foreground default and advertised `timeout_ms` maximum are 600 seconds (`[agent] run_timeout_seconds`, CLI `--run-timeout`; `0` disables the timeout). Smart allows validated project commands by default, including Git inspection/staging/ordinary commits/fetch and unfamiliar tools/options; explicit risks and external filesystem access ask. |
| `fetch` | `url` | HTTP(S) → Markdown/text. Network sessions only. |
| `web_search` | `term` | At most 3 search hits. Network sessions only. |
| `ask` | `questions` | Pause an interactive Agent turn for 1–6 required single-choice questions. Hidden from headless and non-agent surfaces. |
| `goal_met` | `evidence` | Complete an active `/goal`. Advertised only while a session goal is Active (chrome shows `goal`). Hidden in ordinary Act/Lead turns. |
| `attach` | `path` | Queue one local PNG/JPEG/GIF for this turn. |
| `edit` | `path`, `ops` | Preferred in-file edit. Line ops apply bottom-to-top on the original file, before `replace_text`. |
| `write` | `path`, `content` | Create/overwrite a file. Smart/Yolo create missing project parents. |
| `mkdir` | `path` | Directory create. |
| `mv` | `source`, `destination` | Rename; destination must not exist. |
| `rm` | `path` | **Regular file** delete. Directories: `run rmdir` or `run rm -r`. |

`apply_patch` is implemented and still executes under the exact name in Act/Lead
(policy-denied when mutations are off). It is not advertised in
`definitions()`, so models are steered to `edit`. Exact-name calls from older
transcripts or Codex-trained muscle memory still run.

Independent tools in one model round share a worker pool of 4 threads by default
(`[agent] max_parallel_agents`, range 1 through 32). Consecutive
`read`/`skill`/`ls`/`glob`/`grep`/`fetch`/`web_search` calls overlap with each other;
consecutive `run` calls overlap with each other. Mutations, `index`/`symbol`/`outline`,
`ask`/`attach`/`goal_met`, and MCP stay exclusive so a later read sees a prior
edit. Results stay in the model's call order.

## Removed (not advertised, not executable)

`str_replace`, `git_status`, `git_diff`, `read_symbol`, `read_many` (batching lives on `read.items`), `search_text`, `find`, `list_directory`, `list_dir`, `project_overview`, `get_skeleton`, `search_web`, `read_file`, `run_command`, `edit_file`, `write_file`, `create_directory`, `rename_path`, `remove`, `fetch_url`, `attach_image`, `index_overview`, `file_outline`, `search_symbol`.

## Smart `run` filesystem commands

Smart permits validated in-project filesystem commands without a command/option allowlist:

- Auto-allow in-project: `mkdir`/`mkdir -p`, `rmdir`, `rm` (files), `mv`, and `rm -r` of an **empty** directory.
- Ask once: `rm -r` / `rm -rf` when any operand is a **non-empty** directory.
- Confirm still asks for every `run`. Yolo asks nothing.
- Headless Ask → Deny (non-empty tree delete stays blocked in `ainiux run`).
- Windows `cmd` is still denied. POSIX `rmdir` is not treated as Windows `rd`.

## Smart `run` Git and project commands

- Project commands run after Guard and path validation. Unknown tools and options alone do not ask. This covers broad Git inspection flags, `git add`, path-only unstaging, ordinary commits, fetch, local branch/tag creation, interpreters, builds and tests.
- Git push, pull/merge/rebase/cherry-pick/revert, amendment, history resets, branch/tag deletion or replacement, and working-file replacement ask. Global Git prefixes, option assignments and combined short flags retain the same risk checks. Dangerous configuration/path overrides stay denied; pager and external-diff hardening remains.
- All contained project folders, including `.venv`, `.tools`, and caches, permit direct Agent access. Native mutations refuse `.ainiux-pr` and `.git`; reads of `.git` and `run git` remain allowed. Discovery/index ignores do not define permissions. Git queries and search exclusion patterns may mention protected names; real cwd, file inputs, outputs and auxiliary options are validated separately.
- `.venv/bin/python` may symlink to the same trusted PATH interpreter and retains the venv prefix. Unknown Python/Node flags alone do not ask. Explicit outside/protected inline literals, listeners/debug loaders, compiler plugins/wrappers and install/deploy targets ask. Inline path scanning is not an OS sandbox.
- Privilege escalation, free-form shells, destructive search execution, `python -m pip`/`ensurepip`, project-helper wrappers in `python -c`, and existing hard Guard denials remain blocked in Confirm/Smart.
- Confirm asks for executable commands; unchanged `scripts/ainiux/` content trust remains the exception. Script content and external access combine into one approval after path validation. Headless Ask decisions deny. Restricted sessions and security-review retain their independent allowlists.
