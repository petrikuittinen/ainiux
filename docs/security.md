# Security

- API keys are read from environment variables, key files, stdin, or explicit headers.
- `-k`/`--key` is supported for testing but warns because command-line arguments may be visible to other local users.
- Authorization-like headers and configured key values are redacted from transport errors.
- LM Studio authentication is optional by default.
- Local web mode is not implemented. Two headless tool-using workflows exist: read-only `--security-review`, and one-shot `run` / `--run` (interactive `agent` / `--agent`) with ordinary workspace mutation tools. Agent **tools** are shell-free direct argv execution. Security-review `run_command` stays on a strict read-only allowlist; agent `run_command` applies structural checks and denylists shells, elevation, package installs, disk destroyers, and destructive Windows/PowerShell forms. Unrestricted shell is only available as an explicit **user** UI command (below).

## User-initiated interactive shell (`/shell` and `!`)

Chat TUI, agent TUI, and REPL accept user-typed shell commands:

- `/shell COMMAND` or `!COMMAND` (for example `!ls -laFg`) — **display-only notice** in history (stdout+stderr+metadata). Never sent to the model; not used as an agent goal. Agent mode may persist the notice in `.ainiux-pr/agent.sqlite`; chat SQLite does not store notice roles.
- `/shell-stdout COMMAND` or `!!COMMAND` — **pure stdout** into the TUI **input draft** (replaces the draft). On success there is no history notice. On failure, a display-only diagnostic notice (command, exit, stderr) is shown and the status line states what went wrong; the draft still holds pure stdout only. The user may edit or discard; only Enter/Ctrl+S sends the draft as a normal user message (then it *can* reach the model). REPL has no draft buffer and prints pure stdout plus a failure diagnostic on `stderr`.

On POSIX both forms run **`/bin/sh -c`** (or `/usr/bin/sh`). On Windows they run built-in Windows PowerShell 5.1 with `-NoLogo -NoProfile -NonInteractive` and an encoded UTF-16 command; PowerShell and native-command output are set to UTF-8 and the final command status is propagated. They are **not** agent tools: the model cannot invoke them. Stdin is closed. Output is byte-capped, timed (default 60s, or CLI/config `--timeout`), Esc-cancellable in the TUI, and known configured credentials are redacted before display or draft fill.

This is full local shell power for the person at the keyboard. Do not confuse it with agent `run_command`, which never invokes a shell and uses denylist + structural safety rather than listing every harmless binary.

## Headless Security Review

`--security-review` explicitly authorizes sending every eligible file in the current workspace's refreshed index snapshot to the configured model provider. The command prints the file/byte scope and destination provider/model on `stderr` before model work. Its deterministic Markdown report is written to `stdout`; findings do not alter the exit status, while incomplete coverage and operational failures return nonzero after rendering the available report.

Only trusted installed prompts (`share/ainiux/prompts/agent_prompt.md`, `master_prompt.md`, and `security_prompt.md`) or their embedded build copies define system/task instructions. Agent sessions use the stable agent prompt plus a native/XML protocol appendix and append Ainiux-generated Act/Plan controls. `security_prompt.md` adds the security-review task contract and adversarial posture and is concatenated after `master_prompt.md` with its historical byte sequence. The explicit `--trusted-prompt-dir` override is for controlled tests/installations and must supply all three non-empty files; missing-file errors name the exact required path. Prompts are never discovered from the reviewed workspace. All workspace bytes—including `AGENTS.md`, `SKILL.md`, comments, web/MCP fixtures, transcripts, images represented in source, and tool output—are untrusted review data for security-review workers.

Native function calls are supported through Chat Completions/OpenRouter and Responses. Calls are bounded to 20 rounds and 64 calls per review step. A worker still reading after round 12 receives an explicit finalization reminder; from round 16 onward only the final-submission definition is exposed, and any hallucinated read call is denied with a structured result. Unknown tools, invalid arguments, truncation, and policy denials produce structured results. Transient failures receive two cancellation-aware retries; an invalid final submission receives one repair turn with exact missing/unexpected coverage details. Each batch prompt ends with its authoritative `EXPECTED_COVERAGE` array, and workers normally finalize through the schema-defined `submit_security_review` function. A worker result is valid only when its coverage array names every supplied source path exactly once and excludes paths opened only through tools. Finding source paths/ranges and at least one non-empty title or impact remain strict; omitted or empty presentation and assessment metadata receives explicit conservative defaults before the coordinator rather than invalidating the whole batch. Bare assistant JSON remains a compatibility fallback: the validator can extract one complete valid object from a preamble or Markdown fence, rejects multiple valid objects as ambiguous, and never heuristically repairs malformed JSON. Opaque reasoning details and Responses output items are replayed through the provider protocol rather than converted into instructions or display text. Model-controlled finding fields are escaped before local Markdown rendering; freshly verified source evidence remains inside a dynamically sized fenced block.

The read registry exposes only the completed index snapshot. Actual reads verify the indexed content hash and reject traversal, symlinks/reparse escapes, ignored or unindexed paths, `.ainiux`, and VCS metadata. Outputs are bounded UTF-8 JSON envelopes. The shared command runner uses bounded pipes, cancellation/timeouts, and guaranteed cleanup without invoking a shell: POSIX uses process groups and `execve`; Windows uses `CreateProcessW`, an explicit handle list, and a kill-on-close Job Object assigned before the child resumes. Its allowlist is limited to inspection commands and snapshot-safe Git status/file/workspace metadata; Git object/history/diff reads, pagers, external helpers and text conversions, config overrides, recursive ignore bypasses, writes, builds, tests, and interpreters are denied.

Exact configured credentials (including their JSON-escaped forms) are redacted from source batches, tool/command streams, diagnostics, reports, and the per-run diagnostic log. Authorization, API-key, and cookie header values are never logged. No agent session database or interactive agent transcript is created.

Security reviews create `.ainiux-pr/logs/security-review/security-review-*.jsonl` in addition to the index. One-shot agent runs create `.ainiux-pr/logs/agent/agent-*.jsonl` with the same permissions and live-flush behavior. POSIX uses mode `0600` files below mode `0700` directories; Windows uses protected DACLs granting the current user and SYSTEM. Symlinked/reparse or non-directory log paths are refused. While a run is active it appends each event to a live `*.jsonl.partial` path and flushes every record; graceful completion durably renames it to the final `.jsonl`. Crash partials and unrelated files are preserved. The latest three completed logs per kind are retained by default (`security_review_log_keep_runs`). Security-review logging can be disabled with `[agent] security_review_log_enabled = off` or `--no-security-review-log`; agent logging defaults on and can be disabled with `--no-agent-log`.

The diagnostic log intentionally preserves source and model payloads without truncation. Configured credentials are redacted, but unknown secrets embedded in project files or generated by the model can still appear. Logs stay local and are never included in model requests; users should protect or remove them according to their own retention requirements. A logging failure emits one prominent redacted `stderr` warning, even under `--quiet`, then disables logging without changing review/report/agent exit semantics.

## Headless one-shot agent

`ainiux run` / `--run` / `-r` / `--run-file` is a non-interactive coding agent for a single user goal. Interactive `ainiux agent` / `--agent` / `-a` uses the same tools from a chat-like TUI. It refreshes `.ainiux-pr/index.sqlite`, loads the stable trusted agent prompt plus a native or XML protocol appendix, optionally injects workspace-root `AGENTS.md` as separate framed project context, appends the active task-mode control, and runs the shared agent loop. Index/search/symbol tools retain the security-review snapshot rules, while exact-path native reads and Act mutations use validated live filesystem paths and therefore do not require project files to be indexed:

The code index stores definitions and static importance only. Indexed paths and ranges are navigation metadata, never an authorization source, and remain subject to ordinary fingerprint, containment, permission, and live-source checks before reads or edits.

`--disable-indexing` is a strict, session-scoped Agent/Run/Plan control. Ainiux does not probe or open the project index, run in-memory indexed-symbol rescans after writes, or schedule mutation/final refreshes. `glob` and bounded text search remain available through database-free live discovery that applies the same source eligibility, ignore, hidden-directory, symlink, size, UTF-8, containment, result-limit, and cancellation rules. Other index-backed tools and `replace_symbol` are not exposed; Guard approvals, exact-path live reads, ordinary edits, Git, and configured network policy remain unchanged. The option never deletes or rewrites an existing index database.

Interactive reasoning previews contain only readable text explicitly supplied by the provider. Before display/persistence they are whitespace-normalized, redacted with the configured credential set, and bounded by grapheme count and terminal width; only the final clipped preview is stored. Encrypted or opaque reasoning is never converted to display text. Persisted `thinking` rows and every display-only `notice` row are excluded from provider resume context, compaction projections/summaries, and transcript token estimates. Deliberate retry, loop, and protocol feedback uses explicit model-conversation messages instead of relying on display notices. One-shot `run` and `plan` do not emit or persist these previews.

Interactive command permissions are layered after structural parsing, the destructive-command Guard, and command-aware canonical path validation. Confirm asks for every otherwise executable `run_command`. Smart automatically approves only complete argv forms proven to be in the built-in read-only inspection/passive-snapshot set, and only when cwd plus every recognized path input remain canonically inside the active project; unknown flags, non-vetted commands, and external paths ask. Yolo skips the ordinary permission prompt after validation. Hard Guard denials, protected metadata, traversal, unsafe symlinks, malformed structure, and Plan-policy violations cannot be approved in any mode.

The vetted set includes project display/search/checksum commands (`pwd`, `ls`, `cat`, `head`, non-following `tail`, `stat`, `file`, `wc`, `du`, `grep`, `rg`, print-only `find`, `diff`, `cmp`, `readlink`, checksum tools), passive process/system snapshots, strict non-mutating system-display forms, and emulated executable lookup. It rejects recursive link following, process/file-output helpers, checksum verification inputs, follow mode, command execution from search tools, and unrecognized option shapes. Plan uses the same conservative classifier as a non-elevatable command allowlist; security-review retains its narrower index-snapshot policy. POSIX executable lookup uses a fixed trusted PATH. Windows deliberately reads inherited PATH but ignores empty/relative entries, never searches the current directory implicitly, limits PATHEXT to `.com`, `.exe`, `.bat`, and `.cmd`, logs the resolved absolute executable, and sanitizes the child environment. Batch files run through resolved `cmd.exe` only when every argument has a non-expanding representation. Redirects and unquoted shell substitutions are rejected rather than parsed. Privilege escalation remains hard-denied for model commands; `/shell` is the separate explicit user-typed surface.

Interactive agent startup stores and restores non-secret provider, model, API/base
URL, reasoning, and generation/display preferences in project-local `agent.sqlite`.
API keys, custom authorization headers, and resolved credentials are excluded.

Interactive Agent may use the selected OpenRouter, OpenAI, or DeepSeek API key for a bounded, cancellable GET to that provider's configured credit endpoint. This is enabled only when the active inference base URL matches the built-in official base; custom gateways never have their credential forwarded to a provider billing endpoint. The key remains in the authorization header and existing HTTP redaction path; neither the key nor the raw balance response is logged or persisted. Lookup failure, including an OpenAI dashboard endpoint rejecting a project-scoped key, is non-fatal and removes the optional border label.

- `apply_patch`, index/symbol/search tools, and dedicated Git tools remain project-scoped. Validated exact-path native filesystem tools may access external paths according to the active interactive permission mode; external changes create no project history or index entry.
- `git_status` / `git_diff` use the git CLI with pager/external-diff disabled; only read-only options are allowed (no `--output`, no force push, etc.).
- `index_status` / `index_update` refresh project `.ainiux-pr/index.sqlite`; `index_rebuild` requires `confirm=true` and is agent-only.
- `fetch_url` and `search_web` reuse `src/fetch/` and `src/search/` safety (timeouts, size caps, private/loopback blocking unless explicitly allowed).
- Agent `fetch_url` **always** returns UTF-8 **Markdown or plain text** (HTML→MD via `src/html/`, scripts/styles stripped). It never returns raw HTML/CSS/JS to the model: full pages are a prompt-injection and token-cost hazard. CLI `--fetch-url` may still export HTML for local use.
- Path escape, absolute paths, `~/…` / `~user/…` / `$ENV` components, `.ainiux-pr` / `.ainiux` / `.git` components, and symlink components are refused for ordinary workspace writes. On POSIX `~` is a relative path component: without this check `~/code/x` would incorrectly create `$workspace/~/code/x`. After resolve, every ordinary write path is re-checked to stay under the project workspace root. The narrow exception is interactive Act-mode `write_file`: Ainiux resolves and displays the exact external target, blocks the worker for a one-shot Yes/No decision, and writes only after Yes. No/Esc, cancellation, headless operation, and Plan mode deny it. Approved external writes do not create `.ainiux-pr/history` backups or update the project index.
- `read_file` follows the same one-shot approval rule for an exact outside-project regular file. The usual UTF-8, size, line-range, output cap, and configured-secret redaction checks still apply. Other read/search/index tools remain workspace-contained.
- Inside the active project, `read_file` and `read_many` accept any safe regular file on the live filesystem, including ignored, unsupported, generated, or newly created files absent from the code index. Protected metadata, traversal, symlink paths, non-UTF-8 files, and configured size limits remain denied.
- `create_dirs=true` never silently `mkdir -p`: creating missing parent directories requires interactive Guard **y/n** approval (`ask_on_create_dirs`). Headless `run` denies directory creation (create parents first, or use interactive agent). `create_directories` never deletes existing trees; if a parent path exists as a non-directory, the write fails.
- Optional `expected_file_hash` / per-op `expected_hash` rejects stale concurrent edits.
- Pre-overwrite copies are stored under `.ainiux-pr/history/` (project-local; mode depends on umask/filesystem defaults).
- The live touched-file index view is updated after a successful native write so later reads in the same run stay consistent. Exact touched paths are coalesced into a cancellable definitions refresh; readers consume only a completed database generation, and cancellation preserves the prior SQLite transaction.
- Project `AGENTS.md` cannot disable safety rules, change the workspace root, or override the user's direct request.

Security-review never enables mutation or network tools and never injects project `AGENTS.md` as instructions. Agent `run_command` is still **argv-only** (no real shell, no unquoted `|`/`&`/`;` chaining). Workspace scripts are first-class: `./server.sh start`, bare `server.sh` under the project cwd/root, and `bash server.sh …` / `sh ./script.sh` (script-file form) are allowed in Act. Free-form shell code (`bash -c` / `sh -c`) remains denied in Confirm/Smart. Interactive user `/shell` / `!` is a separate UI feature (see above). Final assistant text is written to `stdout`; status, notices, and errors go to `stderr`. Turn/loop limits and transport retries follow the agent-loop reliability rules (identical-call soft/hard caps, consecutive-failure abort, 50-turn scripted cap, no automatic tool re-execution).

### Interactive permission modes and Guard approvals

Each project persists one interactive Agent permission mode in `settings_json`: `confirm`, `smart` (the legacy/fresh-project default), or `yolo`. Confirm asks for native writes, all exact-path native access outside the project, and every otherwise executable model-issued command. Smart allows project writes and native access beneath the canonical system temporary directory, asks for other external native access, automatically runs vetted project-contained read-only commands, and asks for other model-issued commands. Yolo skips elevatable prompts after complete validation **and** skips hard Guard denials (shells, sudo, package managers, host control, …) at the user's risk; it still never runs a real shell for unquoted control operators. Recursive/database/destructive actions ask in Smart and are allowed in Yolo. One tool call produces at most one consolidated prompt.

Confirm/Smart cannot elevate hard Guard denials, Plan-policy denials, protected metadata access, or unsafe symlink/path races. Yolo intentionally does elevate Guard denials (user risk). User-entered `/shell` and `!` remain explicit user actions and do not receive a second permission prompt.

High-risk tool actions can return Guard decision **Ask** (for example `git reset --hard`, force push, recursive `rm`, deleting `*.sqlite`/`*.db` via `remove`). Behavior:

- **Headless** `ainiux run` / `--run`: Ask is always **Deny**. The tool result explains that interactive agent is required for approval. The model must replan; it cannot self-approve.
- **Interactive** `ainiux agent` / `--agent`: the tool worker blocks and the TUI shows a Guard approval panel. The user presses **y** (allow once) or **n**/Esc (deny). Job cancel also cancels a pending Ask. Approvals are **one-shot** (not sticky across later tools).
- Every interactive resolution is stored in project-local `.ainiux-pr/agent.sqlite` table `approvals` (tool name, command preview, rule id, decision, source, message) and mirrored as a short `notice` line in the agent transcript. Hard **Deny** rules (free-form `sh -c`/`bash -c`, sudo, `find -delete`, disk destroyers) are never elevatable by y/n in Confirm/Smart; Yolo skips them.

The agent never approves its own request, never disables guard rules, and never treats user `/shell` as a substitute for Guard Ask.

## Editor Advisory Locks

Writable editor buffers coordinate ainiux processes with an atomic, user-only `FILE.LOCK` directory beside the canonical target. Owner metadata is bounded and contains no API credentials: schema version, hostname, PID, start time, canonical target, and a unique ownership token. Cleanup rereads and matches the token, unlinks only the known metadata file, and removes only the now-empty directory. It never recursively deletes lock contents.

This is advisory coordination, not an operating-system write prohibition: unrelated programs can still alter the target. Device/inode, size, existence, and high-resolution modification-time fingerprints make those changes visible before editing a formerly read-only buffer or saving. Only a PID proven dead on the same hostname is recovered automatically. Remote, live, malformed, missing, token-mismatched, or nonempty locks require the user to verify ownership before manual removal.

## Configuration Files

Automatic installed-default and user configuration files may select a credential environment variable or key-file path, but API key values and arbitrary authorization headers are not accepted by the schema. Files are capped at 1 MiB and must be regular files. Unknown settings and invalid types fail closed before any part of that file is applied.

`--no-config` skips the automatic user files while installed defaults remain effective. `--debug` prints configuration paths and load states to `stderr`, but not parsed values, credential contents, or authorization headers. Paths can still reveal local account or directory names, so avoid debug logs when that metadata is sensitive.

`url_fetch.allow_private_addresses = on` relaxes SSRF protections for explicit CLI/TUI fetches and should only be enabled when local-network access is intended. `network.insecure_tls = on` prints a warning whenever effective. User configuration normally lives at `~/.config/ainiux/config.conf`; protect it appropriately if it contains a sensitive key-file path or private endpoint URL.


## Chat Files

`--save-chat PATH` writes the transcript, provider name, base URL, model, settings, messages, usage, and compaction metadata. API keys and authorization headers are not saved. New chat files use a private temporary sibling, durable flush, and atomic replacement: mode `0600` plus `fsync`/`rename` on POSIX, and protected DACL plus `FlushFileBuffers`/write-through replacement on Windows.

The TUI local chat library stores threads in `~/.ainiux/ainiux.db` using SQLite. The directory is mode `0700` on POSIX and protected by a current-user/SYSTEM DACL on Windows; the database is user-private where supported. It stores prompts, responses, provider/base URL/model metadata, attachments, usage JSON, and compaction events, but not API keys, authorization headers, cookies, or configured key-file contents.


## Rendered HTML Output

`--output-format html` renders assistant Markdown to HTML, and preserves raw HTML blocks/fragments emitted by the model. It is meant for local rendering and file export, not sanitizing untrusted model output. Do not serve generated HTML to other users or open it in privileged browser contexts unless the content is trusted or sanitized by a separate tool.


## URL Fetching

The first v0.5 input/URL-fetching slice is explicit: `--input PATH` reads supported local `.txt`, `.md`, and `.html` files, `--fetch-url URL` fetches an HTML page, and interactive `/fetch URL` inserts a fetched page into context. Used alone, CLI options print converted content according to `--output-format`; used with `-p`/`--prompt` or `--prompt-file` in non-interactive CLI mode, they insert the converted content as a visible user-context message before the final prompt. URL fetching is never triggered implicitly from text inside a prompt.

Defaults:

- response body cap: 1 MiB unless `--max-fetch-bytes N` is set
- connect timeout: existing `--connect-timeout` default
- total timeout for fetch mode: 30 seconds unless `--timeout N` is set
- redirects: followed by default (max 5); each hop still blocks private/loopback/metadata addresses via the socket-open check
- request headers: sends browser-style `User-Agent`, `Accept`, `Accept-Language`, `Sec-Fetch-*`, and `Upgrade-Insecure-Requests` headers
- content type: accepts empty content type, `text/html`, `application/xhtml+xml`, and (for text fetch) `text/plain`
- body encoding: non-UTF-8 bodies are converted when possible (ISO-8859-1 / Windows-1252 via Content-Type or meta); tool results are always valid UTF-8
- private/loopback/link-local/multicast/common metadata literal hosts and resolved socket addresses are refused unless `--allow-private-url-fetch` is set
- agent `fetch_url` `max_bytes` limits the **returned Markdown/text** size; raw HTML may download under a larger safety ceiling before conversion

Resolved IPv4 and IPv6 addresses are checked in libcurl's socket-open callback before a connection is created, so a public-looking hostname cannot connect to a private result. URL fetching through `--proxy` is refused without `--allow-private-url-fetch`, because the client cannot verify target DNS performed by a proxy. The override deliberately disables both literal and resolved-address blocking.

## Web Search

Web search is explicit through `--search QUERY`, REPL `/search QUERY`, TUI `/search QUERY`, and editor `Esc /search QUERY`. It is never triggered from URLs or search terms found inside prompt text alone.

Defaults:

- result cap: 3 unless `web_search.max_results`, `--max-web-search-results`, or `MAXIMUM_WEB_SEARCH_RESULTS` overrides it
- provider order: configured API providers when keys/base URLs exist, then free DuckDuckGo HTML SERP (Instant Answer secondary)
- credentials: API keys come from environment variables or config `*_key_env` names; do not store secrets in config files
- network: uses the same libcurl transport, timeouts, and proxy settings as other HTTP features
- local installs: Searxng/Exa on loopback require `--allow-private-url-fetch`, matching URL-fetch private-address policy
- agent `search_web` returns at most 3 results so models do not fan out into many fetches; overlong result URLs are truncated
- URL fetch uses a desktop Firefox-like User-Agent and browser-style Accept / Sec-Fetch headers (still not a full browser)
- Fetched HTML/text is normalized to **UTF-8** (ISO-8859-1 / Windows-1252 via Content-Type or meta charset). Raw legacy bytes are never put into tool-result JSON (that broke local llama.cpp with “ill-formed UTF-8”)

Search results are untrusted third-party text. They are inserted as user-context messages and should be treated as external input by both users and models. DuckDuckGo HTML markup may change over time; parser failures surface a clear error rather than inventing results.

## Local Image Input

Image input is explicit through `--input IMAGE` or repeated `--attach IMAGE` combined with a prompt. Supported endings are matched case-insensitively and file signatures must match PNG, JPEG, or GIF before data is sent. The default 20 MiB input cap limits both binary reads and subsequent base64 growth; use `--max-image-bytes N` to lower it for constrained environments. WebP input is disabled because tested vision endpoints did not handle it reliably.

Images are embedded in provider requests as data URLs, which sends the complete selected file to the configured model endpoint. Non-interactive images remain request-local. Full-screen chat copies validated image bytes into `~/.ainiux/media/sha256/` with mode `0600` under mode-`0700` directories; SQLite stores the digest, size, MIME type, display name, and original local source reference, never base64. Request workers resolve only lowercase SHA-256 references beneath that managed root, verify size and digest, and release encoded request data afterward. Users backing up `ainiux.db` should also back up the sibling `media` directory.

`/cleanup` and automatic media expiration delete managed bytes but retain database tombstones and lock affected transcripts read-only, so missing media is never silently omitted from a later request. The same lock is applied when a managed file is manually removed or cannot be validated.

The default `--image-capability auto` mode requires both a provider profile whose Chat Completions adapter can carry image parts and a recognized vision model name. `--image-capability allow` is an explicit trust decision for compatible unknown/custom models; it does not make an incompatible provider understand images.

## Text Attachments

`--attach PATH` and interactive `/attach PATH` may send selected local contents to the configured model endpoint. Full-screen chat converts text-like attachments once to canonical Markdown. Up to `[media] max_size_to_store_to_db` UTF-8 bytes are stored directly in SQLite; larger Markdown is copied into the private content-addressed media directory as a `.md` object. Moving or changing the original source does not alter saved replay content. Inline Markdown does not expire automatically; file-backed Markdown follows the same cleanup, tombstone, and read-only-thread protections as managed images. Interactive `/insert FILE_OR_URL` instead places bounded UTF-8 text in the current editor buffer or chat draft; local insertion ignores the file ending, rejects NUL and invalid UTF-8, and is not sent anywhere until the user later sends that chat draft. URL insertion is an explicit network operation: only HTTP(S) is accepted, the normal private-address/DNS/proxy/TLS/timeout/size protections apply, and the response must be UTF-8 HTML. HTML becomes Markdown by default; disabling `input.auto-convert-html-to-md` preserves untrusted raw HTML in the draft. Inserted chat text and attachment context can appear in saved transcripts.

PDF and DOCX are not read as text or uploaded in this slice. Their future input and output converters require explicit dependency, safety, and fidelity decisions.

## Benchmark Datasets

Benchmark prompts and any fetched reference text are sent to the selected model provider. The built-in 50-case corpus performs no URL fetches. A custom JSONL case may specify `fetch_url`; this is an explicit network operation using the same response-size, timeout, proxy, TLS, private-address, and resolved-socket restrictions as other URL fetching. Benchmark text fetching accepts UTF-8 `text/plain`, `text/html`, or `application/xhtml+xml`; HTML is converted to Markdown before it enters context. The supplied `benchmarks/long-context.jsonl` contacts Project Gutenberg and must be selected explicitly.

Treat third-party datasets as untrusted input. Loading is capped at 16 MiB total and 1 MiB per line, requires unique IDs and a known schema, and validates UTF-8 before any model request. Dataset content can still contain adversarial instructions by design, so do not run benchmarks against providers or tools with privileges beyond ordinary chat.
