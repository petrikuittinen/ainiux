# TODO

## Local agent mode (v1.0)

- Project-centric agent rework landed: singleton `.ainiux-pr/agent.sqlite` v2, one-backup history policy, compact tool lines, window-% auto-compact, Chat/Agent chrome, `/cmd-out`. Agent tools now include git_status/git_diff, index_status/update/rebuild, fetch_url/search_web, find_tests, inspect_code_task.
- Agent TUI chrome: permanent input-label line `Ainiux vX [provider/model reasoning] N tok (P.P%)` (window from `--context` → `/v1/models` → default 256k); agent-mode errors go to history notices, not status-bar-only.
- Interactive Guard `Ask` approvals landed: agent TUI y/n panel, one-shot decisions, persist to `approvals` in `.ainiux-pr/agent.sqlite`, headless `run` still maps Ask→Deny.
- Next: find_callers/find_callees (call-graph refs), load agent transcript as sole TUI source of truth (less chat-session coupling), plan/security agent modes, stronger editor↔agent handoff.
- Keep security-review strictly read-only when expanding agent tools (`run_command` stays index-scoped there; network tools stay agent-only).

## Web search / fetch

- Keyless search uses DuckDuckGo HTML (Instant Answer secondary). DDG may rate-limit or show bot challenges after rapid queries; Google HTML scrape was removed (JS-only shells).
- **Later:** evaluate a more reliable free/casual web search provider (or optional lightweight local proxy) without requiring paid APIs for everyday use. Keep optional Tavily/Exa/Firecrawl/Searxng for power users.
- Agent `search_web` returns at most **3** results; tool text steers the model to fetch only the top few URLs. Search result URLs are truncated when extremely long.
- Fetch converts ISO-8859-1 / Windows-1252 pages to UTF-8 so tool results stay valid JSON for local model servers; JSON string escape also refuses raw ill-formed UTF-8 bytes.
- Agent `fetch_url` is Markdown/plain-text only (no raw HTML to the model); HTML→MD strips scripts/styles.

## Syntax Highlighting

- Let the Markdown-only editor/chat preview settle before adding Python, C/C++, C#, Java, JavaScript/TypeScript, HTML/CSS/XML, JSON, and Bash modes.
- Add startup and `/theme` warnings for explicit low-contrast user syntax colors while preserving those overrides.

## Editor AI Commands

- Ensure AI text mutations are one undoable editor operation so a separate preview panel is not required.
- Nice-to-have: keep standalone editor input/navigation fully responsive while an AI assist request is active; cancellation remains the required behavior.
- Harden the standalone editor with resize tests, multi-panel tests, scroll commands, search, and full Unicode grapheme/cell-width handling.

## Editor Text Handling

- Add PTY and large-selection stress coverage for editor indentation and mixed line-ending warnings.
- Add more real-world indentation-detection fixtures for continuation alignment and mixed tab/space conventions.
- Expand PTY coverage for `/reformat`, `/reformat-all`, cancellation, and stale-result messages.

## Chat Persistence

- Add focused integration coverage for SQLite-backed TUI chat storage in `~/.ainiux/ainiux.db`, including automatic save/load, `/new`, `/remove`, `/provider`, and `/list` picker behavior.
- Harden SQLite autosave scheduling so an explicit JSON `/save` or slow file job can be followed by a deferred SQLite save instead of skipping that autosave.
- Add recovery behavior when `app_state.last_thread_id` points at a deleted or missing thread.
- Keep explicit JSON save/load as import/export compatibility, not the primary local chat library.
- Add SQLite schema migrations, corruption handling, permission-denied tests, and leak checks for open/save/load/list/remove paths.

## Provider And API Hardening

- Continue v0.4 provider work: provider capability probing, provider-specific error normalization, broader Responses API schema coverage, and adapter docs/tests for real providers.
- Add optional live reasoning capability probing where providers expose reliable metadata; keep `models.conf` as the portable offline catalog and direct values forward-compatible.
- Periodically verify shipped `models.conf` entries against primary vendor documentation as model IDs, defaults, and accepted values evolve.
- Add a native Anthropic Messages adapter for full Claude extended/adaptive thinking behavior, signatures, output configuration validation, and preserved reasoning state; the OpenAI-compatible Anthropic profile only maps request-side thinking controls.
- Expand native-tool compatibility testing against real providers, especially DeepSeek reasoning content and future native Anthropic Messages thinking signatures; the Chat/OpenRouter and Responses review paths now preserve their opaque continuation items.
- Add provider-reported context limits and improve token estimation.
- Add Responses API image input support.
- Expand JSON handling behind the existing facade or vendor a reviewed JSON library.
- Add more credential-redaction and provider error-path tests.

## Benchmark Mode

- Calibrate configurable judge prompts across representative real providers and document model-to-model variance; retain the raw transcript, evaluation basis, and findings for audit rather than presenting judge scores as ground truth.
- Expand and calibrate policy-sensitive safety boundary cases across providers without collapsing nuanced sexual, hate, harassment, and public-figure requests into one expected action.
- Add more built-in benchmark questions, including additional safety cases.
- Add prompts that help identify or estimate model knowledge cutoff dates, and report those separately from speed/quality aggregates.
- Add Parquet benchmark input compatible with Hugging Face Datasets after the JSONL path is mature; keep the dependency isolated behind `src/benchmark/`.

## Runtime, Cancellation, And Leak Checks

- Continue hardening charset conversion and text encoding paths.
- Expand runtime cancellation tests to cover interrupted streaming HTTP and slow file jobs.
- Add interrupted-stream cancellation tests for streaming parser/provider paths.
- Add leak checks for more success, error, failure, interrupted-stream, and cancellation paths.
- Continue safe URL-fetching hardening for future server/web callers.
- Harden the TUI foundation with interactive resize tests, model-output rendering tests, scrollback polish, better Unicode cell-width handling, and broader command coverage.

## Deferred Document Conversion

- Extend interactive `/attach` to queue multiple heterogeneous attachments with clear per-item status; keep it separate from `/insert` text editing.

1. PDF input (PDF-to-Markdown) and PDF output (Markdown-to-PDF).
2. MS Word input (`.docx`-to-Markdown) and MS Word output (Markdown-to-`.docx`).

These formats are intentionally deferred. Do not treat PDF or DOCX binary data as prompt text.

## Postponed Browser Web UI

- Browser local web UI is postponed. Prefer local OpenAI-compatible server mode first, with rudimentary authentication and local conversion pseudo-models such as `html-to-md` and `md-to-html`.
