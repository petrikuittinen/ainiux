# TODO

## Native Windows parity release gate

- Run the manually triggered UCRT64 build/unit/fault/mock/SQLite/ConPTY/sanitizer/package workflow and retain the packaged artifact/checksum.
- Complete native hands-on acceptance in Windows Terminal and modern conhost for every implemented mode, provider streaming, cancellation, Act/Plan permissions, editor locks/dired, indexing, security review, SQLite, PowerShell, and clipboard save/restore.
- Keep the Windows ZIP unpublished until that parity pass is recorded. Windows ARM64, MSVC/CMake, MSI/installers, old Windows, and mintty full-screen operation remain out of scope.

## Smarter local agent and code index (v1.1)

- Tune static symbol importance and lexical task ranking on larger multilingual projects.
- Enrich `grep` with the enclosing indexed symbol where cheap.
- Add command-generated/rename/removal stress coverage and tune coalescing/shutdown behavior on larger repositories.
- Benchmark index startup, incremental refresh, memory use, model rounds, tool calls, full-file reads, time to first useful edit, and final correctness.
- Benchmark and design lazy SQLite-backed Agent symbol queries before replacing the eager in-memory snapshot; preserve security-review snapshot authorization and atomic refresh publication.
- Keep `glob`, `grep`, targeted reads, compiler output, and tests as verification/fallback paths. The index remains a hint.
- Do not rewrite the built-in agent prompt in this milestone; the user will specify a separate prompt-optimization pass for small local models.
- `/goal` (session completion condition + `goal_met`) is implemented for interactive agent. Still reserve `/loop` and sub-agents until separately specified.
- Continue agent cleanup: load the agent transcript as the sole TUI source of truth, reduce chat-session coupling, strengthen editor↔agent handoff, and retain security-review as strictly read-only.
- Measure representative agent turns before adding read-only tool parallelism. Add a bounded pool only if local tool execution reaches at least 5% of turn wall time; treat network-tool parallelism separately.
- Add a future Agent-specific custom-command and skill system only after its own design; do not reuse `editor-commands.conf`, whose commands remain Chat/editor-only.
- Reusable agent helpers live under `scripts/ainiux/`. `.ainiux-pr/scripts/` is gone. Watch whether models still invent `python3 -c` after the Guard and prompt change.

## Deferred roadmap

- Local OpenAI-compatible server mode remains planned but follows v1.1.
- Image generation is v1.2, after the smarter-indexing milestone.
- Browser web UI remains postponed behind the local server/runtime foundation.

## Web search / fetch

- Keyless search uses DuckDuckGo HTML (Instant Answer secondary). DDG may rate-limit or show bot challenges after rapid queries; Google HTML scrape was removed (JS-only shells).
- **Later:** evaluate a more reliable free/casual web search provider (or optional lightweight local proxy) without requiring paid APIs for everyday use. Keep optional Tavily/Exa/Firecrawl/Searxng for power users.
- Agent client `web_search` returns at most **3** results; hosted provider `web_search` (catalog `web_search=on`) takes precedence when the model/API can emit it. Native Gemini generateContent/Interactions and native Anthropic Messages search remain future work.
- Fetch converts declared charsets to UTF-8 (built-in UTF-16 / 125x / Latin / KOI8, plus allowlisted `iconv` CJK) so tool results stay valid JSON for local model servers; JSON string escape also refuses raw ill-formed UTF-8 bytes.
- Agent `fetch` is Markdown/plain-text only (no raw HTML to the model); HTML→MD strips scripts/styles.

## Syntax Highlighting

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

- Keep explicit JSON save/load as import/export compatibility, not the primary local chat library.
- Add focused leak and permission-failure coverage when persistence ownership changes.

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

- Continue hardening charset conversion: remaining CJK coverage depends on `iconv` / Windows code pages; unlabeled 8-bit files still require `--encoding` or the editor picker.
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
