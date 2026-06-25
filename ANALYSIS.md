# pkchat — Critical Code Review

**Date:** 2026-06-25  
**Scope:** Full repository read-through with emphasis on `--editor` and the planned v0.8 AI-assisted editor  
**Constraint:** Read-only review; no code was modified.

---

## Executive Summary

`pkchat` is a well-architected C++17 CLI chat client at **v0.74**. The non-editor portions—CLI, provider adapters, HTTP/SSE transport, URL-fetch safety, configuration, chat persistence, runtime jobs, TUI foundation, and benchmark mode—are implemented to a **professional standard** suitable for continued development.

The `--editor` flag today is **not** an AI-powered text editor. It is a **standalone multiline text-editor foundation** (piece table + rectangular renderer + basic terminal harness). The v0.8 milestone in `PLANS.md` ("AI-assisted editor") is **entirely unimplemented**: there is no `--assist`, no `/assist` commands, no selection model, no preview/apply workflow, and no provider/runtime integration inside the editor loop.

If you expected an AI writing assistant, you are roughly at **15–20% of the v0.8 vision**—the editing *substrate* exists, but none of the AI product surface does.

---

## What `--editor` Actually Is Today

### Implemented

| Capability | Status | Notes |
|---|---|---|
| Piece-table buffer | Done | `PieceTable` in `src/editor/editor.cpp` — appropriate for large-file editing |
| Rectangular rendering | Done | `EditorState::render(Rect)` decouples layout from I/O — correct for future split panels |
| Soft word wrap | Done | Whitespace-aware breaks, hard-wrap fallback |
| Logical vs visual line movement | Done | `VerticalMovementMode` — TUI chat input uses visual rows |
| Terminal raw mode + restore | Done | `TerminalSession` RAII restores `termios` and alternate screen |
| Minibuffer prompts | Done | Save (Ctrl+S), load (Ctrl+O), quit-with-dirty-buffer (Ctrl+Q) |
| Basic navigation/editing | Done | Home/End, arrows, backspace, delete (via escape seq), kill-to-EOL |
| File load/save | Partial | Works, but see security gaps below |
| Resize detection | Partial | Polls `TIOCGWINSZ` on idle; no dedicated resize tests |
| TUI embedding | Done | Chat input reuses `EditorState` + `ContextualCompleter` |

### Not Implemented (required for v0.8)

Everything in `PLANS.md` § v0.8 remains unchecked:

- `--assist`, `--assist-prompt`, and in-editor `/assist …` commands
- Text **selection** and range-based operations
- **Preview / apply / reject / regenerate** workflow
- **Streaming assist panel** and multi-panel layout
- Cancellable **runtime jobs** inside the editor event loop
- Provider prompt construction, context limits, secret scanning before send
- **Undo/redo**
- Search, goto-line, horizontal scroll (field exists but is unused)
- Clipboard (acknowledged as deferred)

There are **zero** references to `assist`, `selection`, or `undo` under `src/editor/`.

---

## Editor: What Was Done Well

These parts are genuinely professional and worth building on.

### 1. Piece table as the text model

The `PieceTable` design (`original_` + `add_` buffers, piece vector, lazy line cache) is the right choice for an editor that will see frequent inserts/deletes and eventually AI-driven range replacements. Insert/erase split pieces correctly; `write_to()` streams to disk without materializing the full string.

```40:84:src/editor/editor.hpp
class PieceTable {
   public:
    static PieceTable from_string(std::string original);
    // ...
    Error insert(size_t pos, const std::string& text);
    Error erase(size_t pos, size_t count);
    // line/column helpers ...
};
```

Unit tests in `test_editor_piece_table_edits()` cover insert, erase, append, and line metadata. This is solid groundwork for `/assist rewrite` applying a replacement range.

### 2. Renderer decoupled from terminal I/O

`render_panel()` takes a `Rect`, scroll position, and cursor offset and returns `RenderedPanel` lines plus cursor coordinates. The TUI reuses the same path for its multiline input panel. This is exactly what `PLANS.md` describes for a future main-editor + preview-panel layout.

### 3. Documented design intent

`docs/decisions.md` § "Standalone Editor Foundation" accurately describes the architecture, the intentional TAB-completion split (disabled in standalone editor, enabled in chat TUI), and the POSIX/ANSI terminal choice. The codebase matches the documentation—a sign of disciplined agent work.

### 4. Path/command completion for TUI chat input

`ContextualCompleter` distinguishes `/help`-style commands from `/insert PATH` path tokens, supports prefix cycling, and runs directory scans on a **worker thread** with cancellation (`tests/unit/test_runner.cpp`: `test_editor_path_completion`, `test_editor_contextual_completion_modes`). That is appropriate architecture for a responsive TUI.

### 5. Unit test coverage for the editor *core*

The following have automated tests (no terminal required):

- Piece-table edits
- Rectangular rendering and vertical scroll
- Word wrap (hard break and space break)
- Kill-to-line-end edge cases (empty lines, final line)
- Visual vs logical vertical navigation
- File round-trip
- Path and contextual completion

This is a good base. What is missing is everything that touches the terminal, Unicode, or future AI workflows.

---

## Editor: Critical Gaps and Weaknesses

### 1. UTF-8 input is broken in interactive modes

Both `run_editor()` and the TUI input loop read **one byte at a time** and, for printable input, insert a single-byte `std::string(1, ch)`:

```1226:1231:src/editor/editor.cpp
        } else if (ch >= 0x20U) {
            const std::string text(1, static_cast<char>(ch));
            Error insert_error = state.insert(text);
```

Multi-byte UTF-8 sequences are inserted as separate corrupted bytes, not as characters. The project’s own `AGENTS.md` mandates Unicode tests including Chinese, Arabic, emoji, and combining marks—but **no editor tests** cover these, and the input path cannot pass them interactively.

`display_width_at()` also treats every non-control byte as width **1**, so CJK and emoji will mis-wrap and misplace the cursor even if input were fixed.

**Impact:** The editor is effectively **ASCII-oriented** despite UTF-8-aware *movement* helpers (`utf8_len`, `previous_char_offset`). This blocks serious multilingual editing and will break AI-assist previews for non-English drafts.

### 2. No file size bound on editor load

```1103:1114:src/editor/editor.cpp
Error load_file(const std::string& path, PieceTable& out) {
    std::ifstream in(path, std::ios::binary);
    // ...
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = PieceTable::from_string(buffer.str());
```

The entire file is read into a single `std::string` with **no size cap**. A multi-gigabyte file or `/dev/zero` pipe can exhaust memory. Contrast with chat/config/input modules that enforce explicit byte limits.

### 3. Non-atomic, non-restrictive save

`save_file()` truncates and writes directly. Chat persistence (`src/chat/session.cpp`) uses temp file → fsync → rename → `0600` permissions. The editor does neither atomic replace nor restrictive permissions. A crash mid-write can corrupt the target file; world-readable saves are possible depending on umask.

### 4. Performance characteristics that will hurt AI assist

Several hot paths materialize the full buffer:

- `PieceTable::str()` — called by path completion and `replace_token()`
- `replace_token()` clones the entire `PieceTable` before mutating
- Line cache rebuild scans all content on invalidation

For small chat prompts this is fine. For whole-file proofing or large drafts (v0.8 explicitly mentions whole-file operations), this will become noticeable. Not a security flaw, but a design debt before AI features land.

### 5. `scroll_column` is dead code

`EditorState` carries `scroll_column` and `render_panel()` accepts it, but the parameter is explicitly discarded `(void)scroll_column`. Long logical lines clip without horizontal navigation—another v0.8 prerequisite for code editing.

### 6. Standalone editor feature gaps vs TUI

| Feature | Standalone `--editor` | TUI chat input |
|---|---|---|
| TAB completion | Disabled (by design) | Enabled (async) |
| Visual-row movement | Not exposed (defaults to logical) | Enabled |
| Provider/runtime integration | None | Full chat jobs |
| Multi-panel | Single panel | History + input |

The standalone editor is a minimal harness. That is acceptable for a foundation, but it means **most polish lives only in TUI**, not in `--editor`.

### 7. No undo, no selection, no search

`PLANS.md` and `TODO.md` already list these. They are not optional niceties for AI assist—they are **hard prerequisites** for preview/apply and scoped model requests.

---

## Security Analysis

### Editor-specific

| Issue | Severity | Detail |
|---|---|---|
| Unbounded file read on load | **Medium** | Memory exhaustion via large path |
| Non-atomic save | **Low–Medium** | Crash → truncated/corrupt file |
| No save permission hardening | **Low** | Unlike chat JSON (`0600`) |
| Arbitrary path read/write via minibuffer | **Low (expected)** | User-directed; no sandbox. Symlink overwrite behavior depends on OS |
| Path completion directory enumeration | **Low** | Lists cwd-relative paths user can already `ls`; runs off UI thread in TUI |

No evidence of credential leakage through the editor path today because the editor never contacts a model.

### Project-wide (relevant when AI assist is added)

These are implemented **well** today and should be reused for v0.8:

- **SSRF protections** in `src/fetch/fetch.cpp` and `src/http/http.cpp` (literal host block, resolved-address socket callback, proxy restriction)
- **Credential redaction** in `src/security/redact.cpp` with unit-tested sensitive header detection
- **Config schema** rejects inline API keys; files capped at 1 MiB
- **Chat save** atomic write + restrictive permissions
- **URL fetch** explicit opt-in, size/timeout limits, documented in `docs/security.md`

Risks to address **before** AI assist sends buffer text to a provider:

| Future risk | Mitigation needed |
|---|---|
| Sending secrets in selected text | Pre-send scanner (keys, `Authorization`, PEM blocks) — planned in PLANS.md but not implemented |
| Sending whole unsaved buffer silently | Range contract + explicit user confirmation — not implemented |
| Assist metadata persisted to disk | Sidecar policy — not implemented |
| HTML output XSS | Documented as unsanitized (`docs/security.md`); relevant if assist generates HTML |
| `redact_secrets()` substring-only | Partial matches, encoding tricks, or chunked secrets may evade naive replace |

### URL fetch / benchmark

Security here is **above average** for a young CLI tool: integration tests verify private-address refusal (`test_http_private_address_socket_block`, `test_safe_fetch_rejects_private_literal`, `test_mock_server.sh`). Benchmark datasets are validated for size and UTF-8; adversarial prompt content is documented as intentional.

---

## Test Coverage Assessment

### Strengths

- **~60+ unit test functions** in `tests/unit/test_runner.cpp` (~1,980 lines)
- **Broad integration suite** in `tests/integration/test_mock_server.sh` (streaming, auth errors, attachments, URL fetch, config, benchmark, REPL, TUI smoke)
- Build targets: `make test`, `sanitize`, `leak-check` (per `Makefile`)
- Editor **core logic** has meaningful unit coverage (see list above)
- Mock OpenAI server (`tests/mock_server/openai_mock.py`) supports deterministic CI

All unit tests pass (`make test-unit` → `unit tests passed`).

### Editor-specific gaps (aligned with `TODO.md`)

| Missing test area | Risk |
|---|---|
| UTF-8 insert/display/cursor for editor | Silent data corruption for non-ASCII users |
| Terminal resize with wrapped content | Layout bugs |
| Minibuffer save/load/quit flows (automated) | Only partial coverage via `tui_insert_driver.py` |
| `run_editor()` interactive harness | No pty-based editor navigation tests |
| Multi-panel / preview panel | v0.8 UI untested |
| Leak-check paths for editor load/save/edit | AGENTS.md requirement not evidenced for editor |
| AI assist cancel/apply/reject | Feature absent |
| `scroll_column` / horizontal navigation | Feature absent |
| Wide-character / emoji cell width | Feature incomplete |

### AI editor (v0.8) — planned tests all absent

`PLANS.md` lists eleven test categories for v0.8 (selection ranges, prompt construction, apply/reject state machine, mock-provider assist, streaming preview, cancel-during-assist, resize with preview, UTF-8 selection, leak-check assist paths). **None exist** because the features do not exist.

---

## Broader Codebase: Professional Highlights

These areas are suitable foundations and demonstrate competent agent engineering:

### Architecture and modularity

Clean separation: `cli/`, `provider/`, `http/`, `runtime/`, `editor/`, `tui/`, `fetch/`, `config/`, `chat/`, `benchmark/`. UI layers do not embed provider JSON details. `main.cpp` orchestrates modes with explicit mutual-exclusion checks (e.g. `--editor` cannot combine with `--repl`, `--chat`, prompts, or `--format`).

### Provider layer

Data-driven registry with aliases, LM Studio as first-class local profile, `none` offline profile for editor/conversion workflows, Chat Completions + Responses adapters, capability flags, credential resolution from env/key-file.

### HTTP and streaming

libcurl behind RAII wrappers (`CurlHandle`, `CurlHeaders`), incremental SSE parsing, cancellation through xferinfo callback, structured transport error classification.

### Runtime / concurrency

Small but correct: `CancellationToken`, `JobHandle` with cancel-on-destruct, worker threads delivering events to the TUI loop without mutating shared state from workers. Path completion and file jobs follow this pattern.

### Persistence and configuration

Chat JSON: atomic save, schema fields, permission hardening. Config: transactional parse-and-apply, UTF-8 validation, XDG paths, `--no-config` semantics.

### Documentation discipline

`AGENTS.md`, `PLANS.md`, `TODO.md`, `docs/decisions.md`, `docs/security.md`, and `README.md` are unusually thorough for an agent-built repo. Milestone status in `PLANS.md` is honest (v0.8 entirely open).

### Benchmark mode

Substantial, tested, concurrent JSONL runner with validation, scoring hooks, and integration coverage—beyond typical "v0.x" scope.

---

## Broader Codebase: Weaknesses and Technical Debt

| Area | Concern |
|---|---|
| JSON facade (`src/json/`) | Hand-rolled; acknowledged as interim. Risk for complex provider schemas and structured assist responses |
| Unicode | Validated at extraction boundaries (HTML, config, attachments) but not in editor/TUI input |
| Credential redaction tests | `TODO.md` calls for broader coverage; only basic header-name tests found |
| Runtime cancellation tests | `TODO.md` notes gaps for interrupted streaming HTTP |
| TUI | ncurses-free ANSI implementation works but lacks the interactive test depth the editor needs |
| `main.cpp` size | ~1,570 lines—mode orchestration could eventually split, but acceptable now |

---

## Milestone Reality Check

| Milestone | Claimed status | Assessment |
|---|---|---|
| v0.0–v0.3 foundations | Done | Credible; tests and docs support it |
| v0.4 providers | Largely done | Registry, LM Studio, Responses slice present |
| v0.5 attachments/URL fetch | Largely done | Strong SSRF story |
| v0.6 config | Done | Well tested |
| v0.7 benchmark | Done | Impressive for project age |
| **v0.8 AI-assisted editor** | **Not started** | Only non-AI editor foundation exists |
| v0.9 web / v1.0 agent | Not started | Correctly deferred |

`README.md` describes `--editor` as a "standalone multiline editor"—accurate. It does **not** claim AI assistance. The gap is between user expectation ("AI-powered text editor") and implemented scope ("text editor substrate for future AI and current TUI input").

---

## Recommendations (Prioritized)

If the goal is a credible AI-assisted editor, suggested order:

1. **Fix UTF-8 input and display width** in editor + TUI (grapheme/cell-width or integrate `utf8proc`/`libgrapheme` per `AGENTS.md`).
2. **Add selection + undo** to `EditorState` before any model integration.
3. **Bound `load_file()`** (and consider mmap/streaming for large files).
4. **Adopt chat-style atomic save** with restrictive permissions.
5. **Wire runtime jobs + event queue** into `run_editor()` mirroring `tui.cpp` (non-blocking assist, Esc to cancel).
6. **Implement v0.8 minimal slice**: one action (e.g. `/assist rewrite`), preview panel using second `Rect`, plain-text replacement (defer JSON structured output).
7. **Add pre-send secret scan** on text ranges before provider calls.
8. **Expand tests**: UTF-8 editor tests, pty-based minibuffer test, leak-check on editor paths, mock-provider assist integration test.

---

## Conclusion

`pkchat` is a **seriously structured** CLI chat project with several modules implemented at production-minded quality. The agent that built it made good architectural bets—piece table, rectangular rendering, provider registry, SSRF-aware fetch, atomic chat save, cancellable runtime jobs—and backed many of them with tests and documentation.

The `--editor` / AI editor story is **not** far along relative to the v0.8 plan. What exists is a **capable local text editor core** embedded in the TUI and exposed as a standalone mode, but without AI, without selection/undo, with broken multilingual input, and with save/load hardening below the standard the rest of the project sets.

Treat the current editor as **infrastructure worth keeping**, not as a product-ready AI writing tool. The path from here to v0.8 is well documented in `PLANS.md`; almost all of that work remains ahead.