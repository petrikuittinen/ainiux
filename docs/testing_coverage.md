# Testing coverage analysis — ainiux

> **Point-in-time coverage snapshot:** test counts and gaps reflect the audited revision. Use [TESTING.md](../TESTING.md) for the current test policy and commands.

Status: analysis complete, evidence-backed from the live workspace (Makefile, TESTING.md,
`.github/workflows/ci.yml`, `tests/`, `tools/`, AGENTS.md slow-test policy).

---

## 1. Executive summary

The project has a genuinely broad, layered test suite for a C++17 codebase of this size
(~118 production `.cpp` files across 23 modules):

- **Unit** — in-process `build/test_runner` with per-module suites for every production module
  except `version/` (trivial constants). **2,500+ `check()` assertions** across ~39 unit files;
  the agent, provider, editor, TUI, config, and CLI suites are the deepest (several exceed 500
  assertions each; exact totals need a scripted count).
- **Fault injection** — separate `build/test_io_faults` binary: read-only permission failures,
  ENOSPC via `LD_PRELOAD` (`posix_io_mock.so`), real local slow-response/chunked-body timeouts
  (`slow_http_mock.py`), and connect timeouts.
- **Mock servers** — one shared `openai_mock.py` with strict request-shape validation (per-provider
  reasoning fields, image parts, HTML→Markdown conversion, security-review detection, streaming,
  request-local delays), plus `slow_http_mock.py`.
- **Integration** — shell scripts + Python PTY drivers covering CLI, REPL, benchmark, grade,
  fetch, conversion, attachments, images, editor, TUI, SQLite persistence, code index, and
  native-tool security review end-to-end.
- **Sanitizers** — `make test-sanitize` (ASan+UBSan clean rebuild + full test path), local/opt-in.
- **Valgrind** — `make test-leak` on `test_runner`, `test_io_faults`, and `ainiux --version`.
- **CI** — `.github/workflows/ci.yml` is **manual-only** (`workflow_dispatch`), Ubuntu,
  `make test-full` + `make test-leak`.

### Biggest gaps (ranked)

1. **CI is manual-only and never runs the sanitizer** — nothing runs on push/PR; `make test-full`
   has no sanitizer leg (CI relies on Valgrind for leak checks). The ASan/UBSan path is exercised
   only when a developer opts in locally.
2. **No coverage measurement** — there is no gcov/lcov target, so line/function coverage
   percentages are unknown; assertions-per-file is the only available proxy.
3. **UBSan runs in recover mode** — the sanitize build omits `-fno-sanitize-recover`, so undefined
   behavior prints to stderr but does not fail `test-sanitize` unless it also trips ASan.
4. **Valgrind coverage is narrow** — `test_runner`, `test_io_faults`, `ainiux --version` only.
   The ENOSPC `LD_PRELOAD` path, all integration scripts, all PTY drivers, and the mock-server
   process trees are never Valgrind-checked (documented in TESTING.md).
5. **Four PTY drivers are not wired into any target** — `editor_save_driver.py`,
   `editor_prose_continue_driver.py`, `editor_text_modes_driver.py`, `clipboard_driver.py` exist
   under `tests/integration/` but no Makefile target or script invokes them.
6. **`test_llama_server.sh` silently skips in CI** — it exits 0 when no llama-server is listening
   on `127.0.0.1:30000`, so llama-server-specific coverage (context-window metadata, verbose
   streaming) never runs in CI.
7. **No real-provider tests, no fuzzing, no TSan/MSan, no stress/resize TUI tests** (the first and
   last are documented; fuzzing and TSan/MSan are absent entirely).

---

## 2. Test infrastructure inventory (Makefile + CI)

| Target | What it runs | In CI? |
| --- | --- | --- |
| `make test` (fast dev gate) | `test-unit` + `test-integration-smoke` | — |
| `make test-full` | `test-unit` + `test-unit-faults` + `test-integration` | yes (manual) |
| `make test-unit` | `build/test_runner` (in-process, ~26 suites) | via full |
| `make test-unit-faults` | `build/test_io_faults` (read-only + network faults) then `tools/run_enospc_test.sh` (LD_PRELOAD ENOSPC) | via full |
| `make test-integration-smoke` | `test_mock_smoke.sh` (Chat stream, Responses, one-shot agent round) | — |
| `make test-integration` | `test_code_index.sh`, `test_mock_server.sh` (which itself invokes `test_sqlite_persistence.sh`), `test_llama_server.sh` (conditional) | via full |
| `make test-integration-sqlite` | `test_sqlite_persistence.sh` → `tui_sqlite_driver.py` | via `test_mock_server.sh` (line 968) |
| `make test-sanitize` | clean rebuild `-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer` + full `test-full` path | **no** |
| `make leak-check` / `test-leak` | Valgrind `--error-exitcode=1 --leak-check=full --show-leak-kinds=definite,indirect --quiet` on `test_runner`, `test_io_faults`, `ainiux --version`; falls back to `test-sanitize` if valgrind missing | yes (manual) |

Notes:

- `test_mock_server.sh` reuses one shared `openai_mock.py` for all its scenarios (including
  `AINIUX_MOCK_BASE="$BASE" test_sqlite_persistence.sh` at line 968), so SQLite TUI persistence
  **is** part of `make test-full`.
- Sanitizer + ENOSPC interplay is handled: `tools/run_enospc_test.sh` detects an ASan binary and
  resolves the compiler's runtime to place first in `LD_PRELOAD`; an unresolved runtime is an
  explicit unsupported-toolchain failure.
- No gcov/lcov/`--coverage` target exists anywhere in the Makefile.
- No TSan, MSan, or fuzz harnesses exist.

CI (`.github/workflows/ci.yml`): `workflow_dispatch` only (explicitly disabled on push to avoid
failure emails), `ubuntu-latest`, installs `build-essential pkg-config libcurl4-openssl-dev
libsqlite3-dev python3 valgrind`, then `make`, `make test-full`, `make test-leak`. No macOS/BSD
jobs despite the portability goal; no sanitizer job.

---

## 3. Unit coverage by module

`tests/unit/<module>/` mirrors `src/<module>/`. The dispatcher (`tests/unit/test_runner.cpp`)
wires 26 `run_all()` suites; `test_io_faults.cpp` wires the fault suites
(`io::run_readonly_all`, `io::run_enospc_all`, `http::run_network_faults`).

Assertion counts (`check(` occurrences; several files exceed the 500-hit display window, so the
largest counts are lower bounds):

| Module | Files | Approx. assertions | Notes |
| --- | --- | --- | --- |
| agent | 11 | **>1,000** (display truncated at 500) | loop, session runtime, command guard, file tools (edit_file/remove/glob/search matrix), apply_patch, project root, compaction, review, session store, agents_md, adversarial, index |
| provider | 1 | **>500** (truncated at ~500) | SSE edge matrix (CR-only, batched, concatenated, leaked prefixes, DONE), Responses parser, per-provider reasoning serialization, registry/aliases, credit parsing, tool definitions, image validation |
| editor | 1 | **>500** (truncated at ~500) | piece table, indent/outdent, word/path completion, clipboard helper, OSC 52 decode, AI assist commands/config/stream filters, reformat, file I/O, movement keys, terminal input |
| tui | 1 | **>500** (truncated at ~500) | frame diffing, layout, themes + WCAG 4.5:1 contrast, agent chrome/status/activity, thinking display, pickers, history scroll, slash commands |
| cli | 1 | ~200 | option matrix, provider shortcuts, validation functions, removed-option rejection |
| config | 1 | ~250 | TOML-alike parser edge cases, layering, themes/editor-commands/benchmarks/models config, schema validation, reasoning selector |
| chat | 1 | ~120 | JSON save/load, SQLite store round-trip, media store (images/Markdown, aging, expiry), migrations, app-state |
| benchmark | 1 | ~90 | dataset validation (133 builtin cases), scorers, judge grading, CLI |
| highlight | 1 | ~90 | language detection over `tests/highlight/` corpus, token roles, incremental cache |
| markdown | 1 | ~70 | HTML conversion, plaintext, table pretty-printing, output formats |
| ui | 2 | ~60 | scrollbar metrics, text selector, provider/model labels |
| app | 1 | ~40 | user shell parse/execute/timeout/redaction, chat-role filters |
| context | 1 | ~35 | policy compaction (error/truncate/summarize, oldest/middle), token estimation |
| input | 1 | ~35 | image classification/loading, extension matrix, text context, /insert |
| html | 1 | ~25 | conversion fixtures (broken/malformed), UTF-8 validation |
| search | 1 | ~20 | DDG JSON/HTML fixtures, Tavily, truncation |
| json | 1 | 18 | parse, escapes, Unicode, numbers, trailing data |
| http | 2 | 19 | URL validation, SSE transport (in fault binary: timeouts/connect) |
| encoding | 1 | maps, aliases, UTF-16 detect, web charset, iconv allowlist |
| fetch | 1 | 13 | URL/SSRF validation, charset conversion |
| security | 1 | 11 | redaction helpers, sensitive header names |
| runtime | 1 | 8 | queue, cancellation, job lifecycle |
| io (faults) | 1 | 8 | read-only chat/editor load+save, ENOSPC chat/editor save |
| output | 1 | ~10 | thinking splitter (streaming, Unicode, unfinished tags) |
| support | 1 | — | harness (`check`, fixture reader) |

Coverage strengths visible in the evidence:

- **Transport/SSE edge cases are unit-level**: CR-only boundaries, batched/concatenated JSON
  payloads, leaked `data:` prefixes, JSON immediately followed by `[DONE]`, braces inside content,
  UTF-8 splits — all exercised without a network.
- **Security-sensitive logic is unit-tested**: command Guard allow/deny/trap matrix (option
  classifier traps like `date --set`, `find -exec`, `rg --pre`, `tail --follow`, `diff
  --output=`), path containment (escape/absolute/tilde/symlink/protected), Plan read-only policy,
  redaction, SSRF/private-URL rejection, read-only registries.
- **Adversarial/Unicode/malformed-input cases are consistently present** (invalid UTF-8 rendering,
  huge inputs, cancellation, corrupt DBs/files).
- Every production module except `version/` has a matching suite; `src/agent/index/` is covered by
  `tests/unit/agent/test_index.cpp`.

---

## 4. Fault injection coverage

Separate `build/test_io_faults` binary (kept out of the fast runner on purpose):

- **Read-only failures**: chmod-based permission denial for chat session load/save and editor
  file load/save (with `PermissionGuard` restore).
- **ENOSPC**: `tests/mock/posix_io_mock.c` `LD_PRELOAD` shim fails writes to paths containing
  `mock-enospc` (`AINIUX_MOCK_ENOSPC=1`); covers atomic chat save and editor save, including the
  three distinct failure points (open/write/close). Optional `AINIUX_MOCK_EACCES` for EACCES.
  Launcher: `tools/run_enospc_test.sh` (ASan-aware preload resolution).
- **Network**: `slow_http_mock.py` real local server — `/delay/N` (response held open) and
  `/slow-body` (slow chunked body) → timeout error codes; connect timeout to an unreachable
  TEST-NET address. `tests/unit/mock/slow_server.cpp` is an RAII C++ wrapper (fork/exec python,
  ready-pipe handshake, SIGTERM→SIGKILL reaping) used by `http::run_network_faults`.

Note: read-only tests run in `test_io_faults` (not `test_runner`), and ENOSPC requires the
preload env — i.e., they need `make test-unit-faults`, which **is** part of `test-full`/CI.

---

## 5. Mock servers

### `tests/mock_server/openai_mock.py` (one shared server)

- Endpoints: `/v1/models`, `/v1/models-multiple`, `/v1/models-empty`, `/v1/chat/completions`,
  `/v1/responses`.
- **Validates request shape, not just replies**: exact per-provider reasoning fields
  (OpenAI Chat `reasoning_effort=high`, OpenAI Responses `reasoning.effort=4096`, Gemini
  numeric effort, Kimi `"off"`, DeepSeek `"xhigh"`, GLM `"xhigh"`), forbidden-field checks,
  image part presence, restored historical images, HTML attachment converted to Markdown,
  security-review detection via the task-layer prompt/tool schema.
- Features: streaming deltas, reasoning streams, request-local delayed streams, one slow request,
  strict benchmark grading, one-shot agent tools, native-tool coordinator loop.
- **Not covered by the mock**: credit/balance endpoints (OpenRouter/DeepSeek credit parsing is
  unit-only in `test_provider.cpp`), real error-body matrices for every provider, and proxy/redirect
  scenarios.

### `tests/mock_server/slow_http_mock.py`

Used by the fault binary: `/delay/N`, `/slow-body`, `/health` (readiness handshake).

---

## 6. Integration coverage (scripts + PTY drivers)

| Script/driver | Coverage |
| --- | --- |
| `test_mock_smoke.sh` | Chat Completions stream (`Hello`), Responses no-stream, one-shot agent `run` round (metrics regex, `agent.sqlite` created, stdout/stderr separation, second run must not inject prior transcript). |
| `test_code_index.sh` | `--index-code`/`--print-index`/`--clear-index` in a temp workspace; ignore rules, binary skips, stale-snapshot warnings, option rejection, full-language report verification (Markdown→INI, 21 languages), HTML-only embedded-script exclusion. |
| `test_mock_server.sh` (~970 lines) | benchmark (validate/JSONL/verbose/CSV/speed/cancel-exit-130), grade (whole/interleaved/continued transcripts, streaming, cancellation), fetch-url (private vs `--allow-private-url-fetch`), document conversion (html/md/txt, json/jsond, stdin, `--max-input-bytes`), URL/input context, attachments (multi, stdin, conflict, size/binary/UTF-8/missing/deferred), images (PNG/JPEG/GIF, WebM reject, `--image-capability`, REPL), editor PTY drivers, startup-selection PTY driver, DeepSeek profile shape, security review (Chat + Responses + opaque reasoning + untrusted AGENTS.md), context-policy error, REPL save-chat, lmstudio shortcut, empty-models failure, TUI insert/attach/fetch driver, SQLite persistence (line 968). |
| `test_llama_server.sh` | **Conditional**: skips (exit 0) unless `/v1/models` answers on `127.0.0.1:30000`. When live: `--list-models` context window (131k), auto-model chat, verbose stream `context: N (p%)`, unknown-model behavior. Effectively a no-op in CI. |
| `test_sqlite_persistence.sh` + `tui_sqlite_driver.py` | `/new`, autosave/reload, `/list`, `/provider`, `/remove`, stale `last_thread_id`, corrupt DB, image persistence across restart, `/cleanup`, read-only expired-media threads, corrupted model-metadata repair flow. Uses isolated `$HOME` and Python `sqlite3` verification. |
| `tui_startup_selection_driver.py` | Bare-offline chat startup, single-model auto-selection, multi-model selector. |
| `tui_insert_driver.py` | TUI `/insert` file/image/URL with persisted chat-JSON assertions. |
| `editor_continue_driver.py`, `editor_buffers_driver.py`, `editor_locking_driver.py` | Editor AI continue, multi-buffer, and file-lock/ownership PTY flows. |
| `clipboard_driver.py`, `editor_save_driver.py`, `editor_prose_continue_driver.py`, `editor_text_modes_driver.py` | **Present but not invoked by any Makefile target or script** (verified by search; only self-references and TESTING.md mention them). |

---

## 7. Sanitizer and Valgrind detail

### Sanitizer (`make test-sanitize`)

- Clean rebuild with `-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer` and the full
  `test-full` path (unit + faults + integration), including the ENOSPC preload leg (ASan runtime
  resolved and prepended by `run_enospc_test.sh`).
- **Gap**: no `-fno-sanitize-recover`. ASan errors abort; UBSan violations print in recover mode
  and do **not** fail the run by themselves. Consider adding `-fno-sanitize-recover=undefined`
  (and possibly `-fsanitize-recover=address` stays off by default) so UB fails CI/local runs.
- No TSan (data races) or MSan (uninitialized reads) builds.

### Valgrind (`make test-leak` / `leak-check`)

- Flags: `--error-exitcode=1 --leak-check=full --show-leak-kinds=definite,indirect --quiet`.
  Definite and indirect leaks are errors; "possible"/"reachable" are reported only in full output
  and do not fail.
- Coverage: `test_runner`, `test_io_faults`, `ainiux --version`. **Not** covered: ENOSPC preload
  path, integration scripts, PTY drivers, mock-server process trees (documented in TESTING.md).
- CI runs it; if valgrind is missing, the target falls back to `test-sanitize`.

### Policy context (AGENTS.md)

- `make test-full`, `test-integration*`, `test_mock_server.sh`, `test-sanitize`, `leak-check`,
  `test-leak` are slow suites — not part of the routine minor-change loop; run on explicit request
  or release/CI work.
- "No memory leaks" rule requires focused coverage for allocation/cleanup/cancellation paths on
  every such change; full sanitizer/Valgrind only on request.

---

## 8. Known gaps and recommendations

### Coverage gaps (evidence-backed)

1. **CI**: manual-only, single OS, no sanitizer job, no macOS/BSD despite the portability goal.
2. **No coverage metric**: add a `coverage` target (gcov/lcov or llvm-cov) and a CI job so
   regressions in coverage are visible; until then use assertion counts only as a proxy.
3. **UBSan recover mode** — see §7.
4. **Valgrind scope** — at minimum add `test_mock_smoke.sh`/`test_code_index.sh` to the leak leg;
   note the ENOSPC preload path can't run under Valgrind as-is (documented).
5. **Unwired PTY drivers** — wire `clipboard_driver.py`, `editor_save_driver.py`,
   `editor_prose_continue_driver.py`, `editor_text_modes_driver.py` into `test_mock_server.sh` or a
   dedicated target, or delete them if superseded (TESTING.md documents clipboard coverage that no
   target runs).
6. **`test_llama_server.sh` silent skip** — make the skip loud in CI (e.g., echo a skip notice) or
   provide a scripted llama-server fixture so the context-window/verbose-stream path is exercised
   automatically.
7. **No real-provider tests** (documented), no fuzz harnesses, no TSan/MSan.
8. **JSON depth limit** (security finding F5 from the prior audit): the parser has no nesting-depth
   cap, and there is no unit test for deeply nested provider/tool payloads. Add
   `deep nesting rejected/capped` cases to `tests/unit/json/`.
9. **Interactive TUI**: resize, long-running stress, and full-screen interactive regression tests
   are missing (documented in TESTING.md); the PTY drivers cover a narrow slice (startup, insert,
   SQLite, editor).
10. **Mock server**: no credit/balance endpoints (unit-only), no proxy/redirect/error-body matrix
    for every profile.

### What is solid and should not regress

- Broad unit matrix for SSE parsing, provider serialization, Guard/path containment, redaction,
  editor core, config parser, and TUI chrome.
- Fault injection for permission/disk/timeout paths with real subprocess servers and LD_PRELOAD.
- One shared mock server with strict request-shape validation keeps integration honest without
  process sprawl (per ANALYSIS.md, the earlier multi-mock sprawl was consolidated).
- SQLite TUI persistence runs as part of `test-full` via `test_mock_server.sh` line 968.
- ASan-aware ENOSPC launcher; `test_io_faults` kept out of the fast runner.

### Suggested follow-ups (small, test-backed)

1. Add `-fno-sanitize-recover=undefined` to the sanitize target and a CI job for `make
   test-sanitize` (or a sanitizer smoke job) so UB/ASan run automatically.
2. Add a `make coverage` (gcov/lcov) target and record a baseline percentage per module.
3. Wire or remove the four orphan PTY drivers; make the llama-server skip explicit in CI output.
4. Add JSON deep-nesting and oversized-depth unit tests (ties to security finding F5).
5. Extend Valgrind to the smoke integration script.
