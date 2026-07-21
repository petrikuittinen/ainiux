# Security

- API keys are read from environment variables, key files, stdin, or explicit headers.
- `-k`/`--key` is supported for testing but warns because command-line arguments may be visible to other local users.
- Authorization-like headers and configured key values are redacted from transport errors.
- LM Studio authentication is optional by default.
- Local web mode and interactive multi-surface agent UI are not implemented. Two headless tool-using workflows exist: read-only `--security-review`, and one-shot `agent` / `--agent` which adds ordinary workspace writes (`write_file`, exact `str_replace`) but not deletes, approvals, or unrestricted shell.

## Headless Security Review

`--security-review` explicitly authorizes sending every eligible file in the current workspace's refreshed index snapshot to the configured model provider. The command prints the file/byte scope and destination provider/model on `stderr` before model work. Its deterministic Markdown report is written to `stdout`; findings do not alter the exit status, while incomplete coverage and operational failures return nonzero after rendering the available report.

Only trusted installed prompts (`share/ainiux/prompts/master_prompt.md` and `security_prompt.md`) or their embedded build copies define instructions. `master_prompt.md` is the shared tool-using foundation (trust boundary, native tool channel, error recovery). `security_prompt.md` is the security-review task layer only and is concatenated after master for `--security-review`. One-shot agent mode uses master plus a static native/XML protocol appendix without the security task layer. The explicit `--trusted-prompt-dir` override is for controlled tests/installations. Prompts are never discovered from the reviewed workspace. All workspace bytes—including `AGENTS.md`, `SKILL.md`, comments, web/MCP fixtures, transcripts, images represented in source, and tool output—are untrusted review data.

Native function calls are supported through Chat Completions/OpenRouter and Responses. Calls are bounded to 20 rounds and 64 calls per review step. A worker still reading after round 12 receives an explicit finalization reminder; from round 16 onward only the final-submission definition is exposed, and any hallucinated read call is denied with a structured result. Unknown tools, invalid arguments, truncation, and policy denials produce structured results. Transient failures receive two cancellation-aware retries; an invalid final submission receives one repair turn with exact missing/unexpected coverage details. Each batch prompt ends with its authoritative `EXPECTED_COVERAGE` array, and workers normally finalize through the schema-defined `submit_security_review` function. A worker result is valid only when its coverage array names every supplied source path exactly once and excludes paths opened only through tools. Finding source paths/ranges and at least one non-empty title or impact remain strict; omitted or empty presentation and assessment metadata receives explicit conservative defaults before the coordinator rather than invalidating the whole batch. Bare assistant JSON remains a compatibility fallback: the validator can extract one complete valid object from a preamble or Markdown fence, rejects multiple valid objects as ambiguous, and never heuristically repairs malformed JSON. Opaque reasoning details and Responses output items are replayed through the provider protocol rather than converted into instructions or display text. Model-controlled finding fields are escaped before local Markdown rendering; freshly verified source evidence remains inside a dynamically sized fenced block.

The read registry exposes only the completed index snapshot. Actual reads verify the indexed content hash and reject traversal, symlinks, ignored/unindexed paths, `.ainiux`, and VCS metadata. Outputs are bounded UTF-8 JSON envelopes. The command runner uses `fork`, pre-resolved `execve`, pipes, polling, process groups, cancellation/timeouts, and guaranteed reaping; it never invokes a shell, and its child performs no allocations between `fork` and `execve`. Its allowlist is limited to inspection commands and snapshot-safe Git status/file/workspace metadata; Git object/history/diff reads, pagers, external helpers and text conversions, config overrides, recursive ignore bypasses, writes, builds, tests, and interpreters are denied.

Exact configured credentials (including their JSON-escaped forms) are redacted from source batches, tool/command streams, diagnostics, reports, and the per-run diagnostic log. Authorization, API-key, and cookie header values are never logged. No agent session database or interactive agent transcript is created.

Security reviews create `.ainiux/logs/security-review/security-review-*.jsonl` in addition to the index. One-shot agent runs create `.ainiux/logs/agent/agent-*.jsonl` with the same permissions and live-flush behavior. These mode-`0600` JSONL files contain full serialized prompts, raw JSON/SSE responses, tool arguments/results, validation failures, retries, and correlation fields. Their parent directories are mode `0700`; symlinked/non-directory log paths are refused. While a run is active it appends each event to a live `*.jsonl.partial` path, flushes after every record so `tail -f` can follow progress, and only at graceful completion fsyncs once more and atomically renames to the final `*.jsonl` name. The live path is printed on `stderr` at start (unless `--quiet`); crash partials and unrelated files are preserved. The latest three completed logs per kind are retained by default (`security_review_log_keep_runs`). Security-review logging can be disabled with `[agent] security_review_log_enabled = false` or `--no-security-review-log`; agent logging defaults on and can be disabled with `--no-agent-log`.

The diagnostic log intentionally preserves source and model payloads without truncation. Configured credentials are redacted, but unknown secrets embedded in project files or generated by the model can still appear. Logs stay local and are never included in model requests; users should protect or remove them according to their own retention requirements. A logging failure emits one prominent redacted `stderr` warning, even under `--quiet`, then disables logging without changing review/report/agent exit semantics.

## Headless one-shot agent

`ainiux agent` / `--agent` is a non-interactive coding agent for a single user goal (`-p` / `--prompt-file`). It refreshes `.ainiux/index.sqlite`, loads the trusted master prompt plus a static native or XML protocol appendix, optionally injects workspace-root `AGENTS.md` as a separate untrusted user-context message (capped; never system prompt), and runs the shared agent loop with the same snapshot-backed read tools and inspection command allowlist as security review, plus ordinary workspace mutations when the agent registry is created with writes enabled:

- `edit_file` (preferred), `write_file`, and exact `str_replace` may create/overwrite workspace-relative UTF-8 files only.
- Path escape, `.ainiux` / `.git` components, and symlink components are refused.
- Optional `expected_file_hash` / per-op `expected_hash` rejects stale concurrent edits.
- Pre-overwrite copies are stored under `.ainiux/history/` (project-local; mode depends on umask/filesystem defaults).
- In-memory index snapshot hashes are updated after a successful write so later reads in the same run stay consistent. Full on-disk reindex of symbols still happens on the next agent/index refresh.
- Project `AGENTS.md` cannot disable safety rules, change the workspace root, or override the user's direct request.

Security-review never enables these mutation tools and never injects project `AGENTS.md` as instructions. Agent mode still does not enable `remove`/recursive delete, approval UI, `agent.sqlite`, interactive TUI agent mode, or shell beyond the inspection allowlist. Final assistant text is written to `stdout`; status, notices, and errors go to `stderr`. Turn/loop limits and transport retries follow the agent-loop reliability rules (identical-call soft/hard caps, consecutive-failure abort, 50-turn scripted cap, no automatic tool re-execution).

## Editor Advisory Locks

Writable editor buffers coordinate ainiux processes with an atomic, user-only `FILE.LOCK` directory beside the canonical target. Owner metadata is bounded and contains no API credentials: schema version, hostname, PID, start time, canonical target, and a unique ownership token. Cleanup rereads and matches the token, unlinks only the known metadata file, and removes only the now-empty directory. It never recursively deletes lock contents.

This is advisory coordination, not an operating-system write prohibition: unrelated programs can still alter the target. Device/inode, size, existence, and high-resolution modification-time fingerprints make those changes visible before editing a formerly read-only buffer or saving. Only a PID proven dead on the same hostname is recovered automatically. Remote, live, malformed, missing, token-mismatched, or nonempty locks require the user to verify ownership before manual removal.

## Configuration Files

Automatic system and user configuration files may select a credential environment variable or key-file path, but API key values and arbitrary authorization headers are not accepted by the schema. Files are capped at 1 MiB and must be regular files. Unknown settings and invalid types fail closed before any part of that file is applied.

`--no-config` skips only the automatic user file; system configuration remains effective. `--debug` prints configuration paths and load states to `stderr`, but not parsed values, credential contents, or authorization headers. Paths can still reveal local account or directory names, so avoid debug logs when that metadata is sensitive.

`url_fetch.allow_private_addresses = true` relaxes SSRF protections for explicit CLI/TUI fetches and should only be enabled when local-network access is intended. `network.insecure_tls = true` prints a warning whenever effective. User configuration normally lives at `~/.config/ainiux/config.conf`; protect it appropriately if it contains a sensitive key-file path or private endpoint URL.


## Chat Files

`--save-chat PATH` writes the transcript, provider name, base URL, model, settings, messages, usage, and compaction metadata. API keys and authorization headers are not saved. New chat files are written through a temporary file, fsynced where supported, renamed over the target, and created with mode `0600`.

The TUI local chat library stores threads in `~/.ainiux/ainiux.db` using SQLite. The directory is created with mode `0700` and the database file with user-only permissions where supported. It stores prompts, responses, provider/base URL/model metadata, attachments, usage JSON, and compaction events, but not API keys, authorization headers, cookies, or configured key-file contents.


## Rendered HTML Output

`--output-format html` renders assistant Markdown to HTML, and preserves raw HTML blocks/fragments emitted by the model. It is meant for local rendering and file export, not sanitizing untrusted model output. Do not serve generated HTML to other users or open it in privileged browser contexts unless the content is trusted or sanitized by a separate tool.


## URL Fetching

The first v0.5 input/URL-fetching slice is explicit: `--input PATH` reads supported local `.txt`, `.md`, and `.html` files, `--fetch-url URL` fetches an HTML page, and interactive `/fetch URL` inserts a fetched page into context. Used alone, CLI options print converted content according to `--output-format`; used with `-p`/`--prompt` or `--prompt-file` in non-interactive CLI mode, they insert the converted content as a visible user-context message before the final prompt. URL fetching is never triggered implicitly from text inside a prompt.

Defaults:

- response body cap: 1 MiB unless `--max-fetch-bytes N` is set
- connect timeout: existing `--connect-timeout` default
- total timeout for fetch mode: 30 seconds unless `--timeout N` is set
- redirects: not followed in this slice
- request headers: sends browser-style `User-Agent`, `Accept`, `Accept-Language`, and `Upgrade-Insecure-Requests` headers
- content type: accepts empty content type, `text/html`, and `application/xhtml+xml`
- body encoding: validates UTF-8 and rejects invalid legacy-charset bytes with a clear unsupported-feature error
- private/loopback/link-local/multicast/common metadata literal hosts and resolved socket addresses are refused unless `--allow-private-url-fetch` is set

Resolved IPv4 and IPv6 addresses are checked in libcurl's socket-open callback before a connection is created, so a public-looking hostname cannot connect to a private result. URL fetching through `--proxy` is refused without `--allow-private-url-fetch`, because the client cannot verify target DNS performed by a proxy. The override deliberately disables both literal and resolved-address blocking.

## Web Search

Web search is explicit through `--search QUERY`, REPL `/search QUERY`, TUI `/search QUERY`, and editor `Esc /search QUERY`. It is never triggered from URLs or search terms found inside prompt text alone.

Defaults:

- result cap: 3 unless `web_search.max_results`, `--max-web-search-results`, or `MAXIMUM_WEB_SEARCH_RESULTS` overrides it
- provider order: configured API providers when keys/base URLs exist, then DuckDuckGo Instant Answer, then Google HTML parsing
- credentials: API keys come from environment variables or config `*_key_env` names; do not store secrets in config files
- network: uses the same libcurl transport, timeouts, and proxy settings as other HTTP features
- local installs: Searxng/Exa on loopback require `--allow-private-url-fetch`, matching URL-fetch private-address policy

Search results are untrusted third-party text. They are inserted as user-context messages and should be treated as external input by both users and models. Google HTML fallback parsing may break when result markup changes.

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
