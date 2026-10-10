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
| `grep` | `query` | Live UTF-8 content search of any file type; `path` + `glob` combine; use `offset` with returned `next_offset` for pagination. |
| `symbol` | `query` | Ranked indexed definitions. Hidden without an index. |
| `outline` | `path` | Declarations in one file. Hidden without an index. |
| `read` | `path` **or** `items` | One file, or batch 1–100 ranges (`items`); defaults to 128 KiB and returns `next_start_line` when truncated. Images: `attach`. |
| `run` | `command` | Shell-free argv exec. Smart auto-allows classified in-project `mkdir`/`rmdir`/`rm`/`mv`, bounded `git` inspection (`status`/`diff`/`log`/`show`/`ls-files`/`rev-parse`/`rev-list`/`remote -v`, including copied `git -c pager` prefixes), workspace `python`/`node`/`pytest` forms, `node --test`, and everyday C/C++/Java/C# toolchains (`make`/`g++`/`javac`/`dotnet`); asks for non-empty `rm -r`. |
| `fetch` | `url` | HTTP(S) → Markdown/text. Network sessions only. |
| `web_search` | `term` | At most 3 search hits. Network sessions only. |
| `ask` | `questions` | Pause an interactive Agent turn for 1–6 required single-choice questions. Hidden from headless and non-agent surfaces. |
| `goal_met` | `evidence` | Complete an active `/goal`. Advertised only while a session goal is Active (chrome shows `goal`). Hidden in ordinary Act/Lead turns. |
| `attach` | `path` | Queue one local PNG/JPEG/GIF for this turn. |
| `edit` | `path`, `ops` | Preferred in-file edit. Line ops apply bottom-to-top on the original file, before `replace_text`. |
| `write` | `path`, `content` | Create/overwrite a file. |
| `mkdir` | `path` | Directory create. |
| `mv` | `source`, `destination` | Rename; destination must not exist. |
| `rm` | `path` | **Regular file** delete. Directories: `run rmdir` or `run rm -r`. |

`apply_patch` is implemented and still executes under the exact name in Act/Lead
(policy-denied when mutations are off). It is not advertised in
`definitions()`, so models are steered to `edit`. Exact-name calls from older
transcripts or Codex-trained muscle memory still run.

## Removed (not advertised, not executable)

`str_replace`, `git_status`, `git_diff`, `read_symbol`, `read_many` (batching lives on `read.items`), `search_text`, `find`, `list_directory`, `list_dir`, `project_overview`, `get_skeleton`, `search_web`, `read_file`, `run_command`, `edit_file`, `write_file`, `create_directory`, `rename_path`, `remove`, `fetch_url`, `attach_image`, `index_overview`, `file_outline`, `search_symbol`.

## Smart `run` filesystem commands

Classified by `assess_workspace_fs_command`:

- Auto-allow in-project: `mkdir`/`mkdir -p`, `rmdir`, `rm` (files), `mv`, and `rm -r` of an **empty** directory.
- Ask once: `rm -r` / `rm -rf` when any operand is a **non-empty** directory.
- Confirm still asks for every `run`. Yolo asks nothing.
- Headless Ask → Deny (non-empty tree delete stays blocked in `ainiux run`).
- Windows `cmd` is still denied. POSIX `rmdir` is not treated as Windows `rd`.

## Smart `run` git inspection and workspace interpreters

- Auto-allow bounded in-project Git inspection: `status`, `diff`, `log`, `show`, `blame`, `ls-files`, `rev-parse`, `rev-list`, `remote -v`/`get-url`, `check-ignore`, listing `branch`/`tag`, `stash list`/`show`, and similar read-only queries. `git -c core.pager=cat …` copied from a prior hardened argv still classifies as the porcelain subcommand. Reject `--output`/`--ext-diff`/`--textconv`, `git add`/`commit`/`push`, `git stash` without `list`/`show`, and `git -c alias.*`.
- Auto-allow workspace-scoped interpreters classified by `assess_workspace_interpreter_command`: `python`/`python3` file, `-m`, and `-c`; `node` file, `--test`, and `-e`/`--eval`; `pytest`. Cwd and recognized path operands must stay in-project. Inline `-c`/`-e` payloads with absolute, home, protected, or `..` path literals stay prompts, as do `python -m http.server`, `node --inspect`/`--watch`/`--loader`, and unknown python/node flags.
- Auto-allow everyday C/C++/Java/C# toolchains in the same classifier: `gcc`/`g++`/`clang`/`make`/`cmake`/`ninja` (and `cc`/`ar`/`ld`/`clang-format`), `javac`/`java`/`jar`/`mvn`/`gradle`, and `dotnet`/`csc`/`msbuild`. Compiler plugin/wrapper flags, `make --eval`/`-f -`, `make install`, `cmake --install`, `jshell`, Maven `deploy`/`release:perform`, and Gradle remote `publish` stay prompts. `dotnet nuget push|delete` and `dotnet tool install --global` are hard Guard denials. Unknown compiler flags fail open so typical `-O2`/`-Wall`/`-j` builds do not prompt.
- `python -m pip` / `ensurepip` is a hard Guard denial, matching `pip install`. Wrapping `scripts/ainiux` in `python -c` and `python -` stdin programs stay denied. `bash -c` stays denied.
- Confirm still asks for every `run`. Restricted read-only policy accepts the Git forms and still rejects interpreter and toolchain execution.
