# ainiux Refactor And Risk Analysis

Date: 2026-07-09

Scope: full tracked repository review with emphasis on reducing lines of code, removing old baggage, consolidating repetition, and flagging security, memory, and unchecked-return risks.

This replaces the old `ANALYSIS.md`, which was stale and described an older editor state.

## Inputs Used

Read-only commands and tools used:

- `rg --files`
- `git status --short`
- `find . -maxdepth 3 -type d`
- `find src include tests -type f ... | xargs wc -l`
- custom read-only duplicate literal/block scans
- `cppcheck --enable=warning,style,performance,portability --inline-suppr --suppress=missingIncludeSystem --std=c++17 -Iinclude -Isrc -Itests/unit src include tests/unit`
- `flawfinder --quiet --minlevel=2 src include tests`
- `clang-tidy` with explicit GCC 13 include paths, no fix mode, and `bugprone-*`, `clang-analyzer-*`, `performance-*`, `portability-*`

Analyzer caveat: `clang-tidy` emitted huge internal warning counts because of header/system traversal. I only used concrete diagnostics pointing at project files. `flawfinder` is lexical and produced many false positives for identifiers containing words such as `system`; those are not treated as vulnerabilities unless the surrounding code supports the finding.

## Executive Summary

The biggest line-count wins are not micro-optimizations. They are:

1. Delete or move tracked benchmark/scratch artifacts that do not belong in the runtime source tree.
2. Centralize small utility functions copied across modules.
3. Replace repeated selector/picker rendering with one generic text selector.
4. Collapse repetitive SQLite binding/error boilerplate.
5. Split large orchestration functions only where it removes repeated state handling or makes UI jobs/pickers reusable.
6. Consolidate provider/profile/settings strings and validation lists into single sources of truth.

I did not find an obvious raw owning-pointer leak pattern in the production code. The code is generally RAII-heavy. The main memory-risk finding is retained streaming HTTP body data, not a classic leak. The most actionable security risks are unbounded provider-side file reads and editor path TOCTOU/truncating-save behavior.

## Size Hotspots

Tracked C/C++ code under `src`, `include`, and `tests` is about 35,531 lines by `wc -l`.

Largest production files:

- `src/provider/provider.cpp`: 2,898 lines
- `src/tui/run.cpp`: 1,680 lines
- `src/editor/run_editor.cpp`: 1,505 lines
- `src/config/config.cpp`: 1,426 lines
- `src/search/search.cpp`: 1,071 lines
- `src/markdown/markdown.cpp`: 1,010 lines
- `src/html/html.cpp`: 967 lines
- `src/editor/editor_assist.cpp`: 967 lines
- `src/chat/sqlite_store.cpp`: 879 lines
- `src/benchmark/run.cpp`: 799 lines
- `src/cli/args.cpp`: 707 lines

These files are not all bad. `provider.cpp`, `tui/run.cpp`, and `editor/run_editor.cpp` are the highest-value refactor targets because they combine large size with repeated local patterns.

## Delete Or Move Old Baggage

### Tracked benchmark result dumps

The repository tracks generated result files under `results/`, for example:

- `results/benchmark-1782071070.jsonl`
- `results/benchmark-1782125370.md`
- `results/benchmark-1782383495.jsonl`
- `results/judgement.md`

These look like generated outputs, not source fixtures. They inflate review noise and make it unclear which files are canonical test data. Move curated examples under `docs/` or `tests/fixtures/benchmark/` if they are needed; otherwise delete them and ignore generated `results/`.

Suggested `.gitignore` additions:

```gitignore
/results/
*.tmp
*~
__pycache__/
*.pyc
```

### Top-level scratch/conversion files

Likely baggage at repo root:

- `test.html`
- `test.md`
- `llm_knowledge_cutoff_test_dataset_v2.md`

If these are fixtures, move them under `tests/fixtures/` with descriptive names. If they are manual experiment inputs, delete them.

### Cutoff helper placement

`find_cutoff` is a symlink to `find_cutoff.sh`, and the script shells out to the local `ainiux` binary. That may be useful, but it does not belong at repo root long term.

Suggested actions:

- Move to `tools/find_cutoff.sh` or `scripts/find_cutoff.sh`.
- Add `tools/README.md` or document it under benchmark docs.
- Keep the root clean of one-off workflow helpers unless they are part of the public interface.

### Empty future directories

Tracked placeholders exist in:

- `src/web/.gitkeep`
- `src/tools/.gitkeep`
- `src/unicode/.gitkeep`

These match the roadmap, so this is low priority. If the aim is fewer tracked files now, delete empty future directories and recreate them when implementation starts.

## Repetition: Shared Utility Candidates

### ASCII trim/lower helpers are copied repeatedly

Copies of `trim_ascii`, `trim_ascii_copy`, `lower_ascii`, or equivalent exist in many modules:

- `src/app/detail.cpp:5`
- `src/fetch/fetch.cpp:13`
- `src/http/http.cpp:82`
- `src/provider/provider.cpp:25`
- `src/chat/sqlite_store.cpp:217`
- `src/config/config.cpp:630`
- `src/editor/detail/editor_common.cpp:9`
- `src/editor/editor_assist.cpp:121`
- `src/search/search.cpp:19`

Suggested action: put small, allocation-aware helpers in `src/common.hpp` / `src/common.cpp`, for example:

- `ascii_trim_copy(std::string)`
- `ascii_lower_copy(std::string)`
- `ascii_iequals_normalized_provider_key(...)` or similar if dash-to-underscore is needed

Do not over-generalize into Unicode trimming. These are ASCII protocol/config helpers.

### Repeated line splitting

The same `split by '\n', drop trailing '\r'` pattern exists in:

- `src/html/html.cpp`
- `src/markdown/markdown.cpp`

Suggested action: add a common `split_lines_crlf()` helper. This is small but safe and reduces parser boilerplate.

### Repeated positive integer env parsing

`parse_positive_int_env` appears in both:

- `src/editor/ai_continue.cpp`
- `src/search/search.cpp`

Suggested action: move to a small config/env utility. It should keep the current behavior of ignoring missing, empty, malformed, and non-positive values.

### Repeated fetch option mapping

The same `cli::Options` to `fetch::Options` mapping exists in:

- `src/app/document_mode.cpp:224`
- `src/benchmark/detail.cpp:14`

Suggested action: one shared helper, probably in an app/config boundary module, to avoid future divergence in timeout/proxy/private-address defaults.

### JSON value serialization duplicated

`json_value_to_string`-style serialization exists in:

- `src/chat/session.cpp`
- `src/provider/provider.cpp`

Suggested action: move JSON value serialization into the JSON facade, for example `json::stringify(const Value&)`. This reduces provider/chat coupling and avoids future inconsistent JSON output.

### MIME/content-type and repeated literals

Repeated literals include:

- `text/plain`
- `text/html`
- `image/png`
- `image/jpeg`
- `image/gif`
- `application/xhtml+xml`

Suggested action: define common MIME constants in the relevant module (`src/input/` or a small `src/common/mime.hpp`) only if touched during input/fetch refactors. Do not create a huge constants file.

## Repetition: Provider And Settings Strings

Provider names, aliases, settings names, and policy names are repeated across CLI parsing, config parsing, provider building, chat settings, docs, and tests.

Examples:

- `lm_studio`, `lmstudio`, `custom_openai_chat`
- `thinking_budget`, `enable_thinking`, `reasoning_effort`
- context policies: `error`, `truncate-oldest`, `summarize-oldest`, `summarize-middle`, `provider-auto`
- sampling settings: `temperature`, `top_k`, `top_p`, `min_p`, `repeat_penalty`, `presence_penalty`
- command strings: `/insert`, `/attach`, `/search`, `/provider`, `/model`

Suggested action:

- Keep provider registry data in `src/provider/`, but expose normalized constants or lookup helpers for names/aliases.
- Define option-setting metadata once for config, CLI, and `/setting` validation where feasible.
- Keep tests using some literal strings for behavior readability, but avoid duplicating provider/profile canonical lists in tests.

High-value target: `src/chat/settings.cpp`, `src/config/config.cpp`, and `src/cli/args.cpp` all know about overlapping generation settings. A small metadata table could reduce repetitive parse/clear/serialize branches.

## UI Repetition

### Generic text selector

TUI provider/model/thread picker rendering:

- `src/tui/input_handlers.cpp:100`
- `src/tui/input_handlers.cpp:115`
- `src/tui/input_handlers.cpp:124`
- `src/tui/input_handlers.cpp:128`

Editor buffer picker rendering:

- `src/editor/buffers.cpp:15`
- `src/editor/buffers.cpp:35`

These share the same shape:

- header/hint line
- selected prefix
- up/down/page/home/end movement
- item label formatting
- cancel/select/new affordances

Suggested action: add a generic selector type with:

- `selected`
- `items`
- movement helper
- configurable selected prefix
- header/hint string
- label callback

Keep domain-specific label functions, for example thread metadata and buffer dirty/line/column details. Do not force provider/model/thread/buffer into one data type.

Expected result: less code in TUI/editor and less drift in key behavior.

### Repeated confirmation prompts

Editor and TUI confirmation flows repeatedly implement yes/no parsing, prompt reset, and cancel messages:

- editor overwrite/load/autosave/quit confirmations in `src/editor/terminal_ui.cpp`
- TUI model/thread/remove confirmations in `src/tui/session_load.cpp` and `src/tui/run.cpp`

Suggested action: centralize confirmation state handling around:

- accepted keys: `y`, `Y`
- rejected keys: `n`, `N`, empty Enter where current behavior allows it
- retry prompt: `Type y or n: `
- cancel/accept callback supplied by caller

This reduces UI line count while preserving each surface's wording.

### Repeated status/help strings

Examples:

- `No models returned`
- `Enter saves - Esc cancels`
- provider/model picker hints
- command labels in help, completions, and status messages

Suggested action: do not create a global strings dump. Put local constants near the shared UI helper or command metadata table.

## Large Function Refactor Targets

### `src/tui/run.cpp`

`tui::run` is about 1,645 lines. It holds thread state, job state, command handling, picker handling, rendering decisions, and persistence scheduling.

Suggested split:

- TUI thread/session actions
- picker actions
- file/fetch/search job launch helpers
- model/provider switching helpers
- command dispatch table

Goal: reduce accidental coupling and repeated status/event plumbing. Avoid a broad rewrite; extract stable chunks around existing behavior.

### `src/editor/run_editor.cpp`

`run_editor` is about 1,406 lines. It includes local file setup, help view, AI assist session, model listing, commands, terminal loop, and duplicated `ensure_empty_file`.

Suggested split:

- remove duplicate `ensure_empty_file` by using the public one in `src/editor/file_io.cpp`
- isolate assist job/session handling
- isolate model/provider selector handling
- isolate help/buffer view handling

### `src/provider/provider.cpp`

`provider.cpp` is 2,898 lines and contains:

- profile registry
- URL/key context building
- reasoning compatibility mapping
- request JSON building
- model table formatting
- chat and Responses parsers
- SSE parsers

Suggested split:

- `profiles.cpp`
- `request_json.cpp`
- `reasoning.cpp`
- `models.cpp`
- `sse.cpp`
- `responses.cpp` / `chat_completions.cpp` if needed

Keep the public `provider.hpp` interface stable. This is a maintainability split more than a line-count trick.

### `src/config/config.cpp` and `src/cli/args.cpp`

Both are long because parsing is explicit. Do not replace them with clever macro-heavy code. Instead, extract only repeated value parsers and setting metadata.

Good candidates:

- enum-string validation tables
- non-negative numeric parsing wrappers
- common generation setting metadata
- shared render/output format parsing

## Security, Correctness, And Memory Findings

### High: unbounded prompt/system/key file reads

`src/provider/provider.cpp:187` reads a path or stdin into memory with no byte cap:

- `std::cin.rdbuf()` for `-`
- `std::ifstream` into `std::ostringstream`

It is used by `build_context` for:

- `--prompt-file` at `src/provider/provider.cpp:2325`
- `--system-file` at `src/provider/provider.cpp:2331`
- `--key-stdin` at `src/provider/provider.cpp:2336`
- `--key-file` at `src/provider/provider.cpp:2342`

Other input paths already enforce byte limits. This path should also enforce clear caps:

- prompt/system file: use `max_input_bytes` or a dedicated prompt-file cap
- key file/stdin: small dedicated max, for example 64 KiB or less
- reject directories/special files where practical

This is resource-exhaustion risk, not a credential leak finding.

### Medium: streaming HTTP accumulates full body

`src/http/http.cpp:272` appends every chunk to `response.body` before invoking `on_body` at `src/http/http.cpp:273`.

For streaming chat, provider code consumes chunks via callback (`src/provider/provider.cpp:2821`) and usually does not need the full raw SSE body after success. Long streams can therefore retain unnecessary memory.

Suggested action:

- Add a request flag such as `discard_body_after_callback` or `streaming_body_mode`.
- Preserve body on non-2xx/error responses so diagnostics still work.
- Keep `max_body_bytes` behavior clear for non-streaming fetches and model listing.

### Medium: editor saves truncate directly

`src/editor/file_io.cpp:94` opens editor saves with `std::ios::trunc` and writes directly to the target.

Chat persistence uses a safer temp-write/fsync/rename flow. Editor saves are user-directed, so this is not the same confidentiality issue as chat persistence, but a crash or disk-full event can corrupt the target file.

Suggested action:

- Introduce atomic editor save for regular files.
- Preserve explicit overwrite confirmation behavior.
- Use restrictive permissions for newly created scratch files where reasonable.

### Medium: editor `access()` TOCTOU and duplicate helper

`flawfinder` correctly flagged `access()` race candidates:

- `src/editor/file_io.cpp:113`
- `src/editor/run_editor.cpp:31`
- `src/editor/run_editor.cpp:103`

There is also duplicate `ensure_empty_file` logic in `src/editor/file_io.cpp` and private `src/editor/run_editor.cpp`.

Suggested action:

- Remove the private duplicate.
- Prefer open/create semantics that perform existence checks and creation atomically where possible.
- Keep behavior for existing user paths stable.

### Medium: JSON parser mutates parse position inside compound condition

`clang-tidy` flagged `src/json/json.cpp:218`:

```cpp
if (pos_ + 2 > input_.size() || input_[pos_++] != '\\' || input_[pos_++] != 'u')
```

This is compact but hard to audit because it mixes bounds checking and side effects. It is especially sensitive inside surrogate-pair parsing.

Suggested action: split into explicit reads:

- verify two bytes are available
- read first byte
- read second byte
- validate values

### Medium: optional access warning in editor AI setup

`clang-tidy` flagged unchecked optional access in:

- `src/editor/editor_ai_setup.cpp:12`
- `src/editor/editor_ai_setup.cpp:60`
- `src/editor/editor_ai_setup.cpp:66`
- `src/editor/editor_ai_setup.cpp:74`

Manual reading shows most accesses follow helper checks or `ensure_editor_ai_context`, but the proof is indirect. This is a readability and future-maintenance risk.

Suggested action:

- Convert helper checks into explicit local references after validation.
- Return early on missing context, then use `AiContinueContext& ctx = *context;`.
- Keep tests around `/provider`, `/model`, and offline editor mode.

### Low-Medium: dead stores in HTML parser cleanup

`cppcheck` and `clang-tidy` flagged dead stores:

- `src/html/html.cpp:945`: `pre_depth = 0`
- `src/html/html.cpp:953`: `blockquote_depth = 0`

The cleanup still appends captured content, but the assignments themselves are unused. Remove them if no side effects are intended.

### Low-Medium: redundant cancellation checks

`cppcheck` flagged conditions that appear redundant:

- `src/benchmark/run.cpp:449`
- `src/input/input.cpp:181`
- `src/input/input.cpp:259`
- `src/fetch/fetch.cpp:271`

These may be deliberate checkpoints before/after expensive work, but some are currently adjacent enough to look like dead checks. Keep cancellation checks where they guard expensive CPU work such as base64 encoding or HTML conversion, but remove duplicate no-op checks.

### Low: selection parser branch appears impossible

`cppcheck` flagged `src/editor/selection.cpp:136`: `strip_tilde_modifier_suffix(...)` always returns true.

Suggested action: either make the helper return `void` or restore meaningful failure behavior. This reduces branch noise.

### Low: unchecked/ignored cleanup return values

Candidates:

- `src/http/http.cpp:179`: `inet_ntop` return value is ignored
- `src/chat/sqlite_store.cpp:446`: `sqlite3_close` return is ignored
- `src/chat/session.cpp:342`: parent directory `fsync` result is ignored

Some cleanup failures may intentionally be non-fatal. Add comments where ignored by design, or surface errors where data durability depends on them.

### Low: test environment restoration uses invalidated `getenv` pointers

`clang-tidy` flagged:

- `tests/unit/editor/test_editor.cpp:135`
- `tests/unit/editor/test_editor.cpp:140`

The test stores `getenv` pointers, calls `setenv`/`unsetenv`, then reuses the old pointers. Copy the old values into `std::string` before mutating the environment.

## Analyzer Findings To Treat As Refactor Hints

### SQLite assignment-in-condition boilerplate

`clang-tidy` flagged many lines in `src/chat/sqlite_store.cpp` such as:

- `src/chat/sqlite_store.cpp:384`
- `src/chat/sqlite_store.cpp:575`
- `src/chat/sqlite_store.cpp:593`

The pattern is intentional:

```cpp
if (!(err = stmt.bind_text(...)).ok()) return err;
```

Still, it is repetitive and noisy. A helper would reduce lines and avoid analyzer churn:

```cpp
Error bind_all(Statement& stmt, ...);
```

or simply split assignment from condition in local helper routines.

### Branch clones

`clang-tidy` found repeated branch bodies in:

- `src/provider/provider.cpp`
- `src/markdown/markdown.cpp`
- `src/html/html.cpp`
- `src/tui/activity.cpp`
- `src/tui/theme.cpp`

Not all are bugs. Many are explicit mapping tables. Use these as refactor hints only when the result is clearer.

### `std::endl` and pass-by-value performance warnings

Examples:

- `src/app/chat_session.cpp:148`
- many `runtime::CancellationToken` pass-by-value warnings

These are not priority findings. Fix opportunistically when touching those files.

## Memory Ownership Review

No obvious raw owning pointer pattern was found in production code. Positive signs:

- libcurl handles and header lists are RAII-wrapped in `src/http/http.cpp`
- SQLite statements and transactions use RAII wrappers in `src/chat/sqlite_store.cpp`
- runtime jobs are joined/cancelled through wrapper types
- file descriptor ownership is wrapped in chat JSON persistence

Main memory concerns:

- full streamed HTTP body retention (`src/http/http.cpp:272`)
- unbounded provider prompt/system/key reads (`src/provider/provider.cpp:187`)
- large parser modules can allocate heavily, but input/fetch/config paths mostly have caps

## Suggested Refactor Order

1. Repo hygiene: delete/move generated results and scratch files; expand `.gitignore`.
2. Common utilities: ASCII trim/lower, line splitting, env parsing, JSON stringify.
3. UI selector helper: provider/model/thread/buffer pickers plus shared movement behavior.
4. Provider file-read caps: prompt/system/key file and stdin limits.
5. HTTP streaming body retention flag.
6. Editor file path cleanup: remove duplicate `ensure_empty_file`, fix `access()` TOCTOU where practical, consider atomic saves.
7. SQLite bind boilerplate helper.
8. Provider split: profiles, request JSON, reasoning, models, SSE.
9. Long function extraction in TUI/editor only after helper boundaries exist.

## Commands Worth Adding To CI Or Developer Workflow

Suggested non-blocking targets:

```sh
make cppcheck
make flawfinder
make clang-tidy
```

Recommended behavior:

- `cppcheck`: fail on warning/performance/portability only after suppressions are reviewed.
- `flawfinder`: report-only by default because of false positives.
- `clang-tidy`: report-only until a compile database or explicit compiler wrapper is stable.

Do not make all analyzer warnings fatal immediately. The current output includes legitimate style/refactor hints and false positives.

## Quick Win List

Highest confidence, low blast radius:

- Move duplicate fetch option mapping into one helper.
- Move duplicate `trim_ascii`/`lower_ascii` helpers into common utilities.
- Remove dead stores in `src/html/html.cpp`.
- Remove private duplicate `ensure_empty_file` in `src/editor/run_editor.cpp`.
- Copy old env values in `tests/unit/editor/test_editor.cpp` before `setenv`.
- Split `src/json/json.cpp:218` into explicit operations.
- Add caps to provider `read_file`.

Highest line-count impact:

- Generic selector helper for TUI/editor pickers.
- SQLite bind/step helper.
- Provider module split.
- Generation setting metadata table shared by CLI/config/chat settings.
