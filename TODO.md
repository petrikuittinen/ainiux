# TODO

## Editor AI Commands

- Add `/regenerate` to repeat the previous AI command with the same command options where practical.
- Ensure AI text mutations are one undoable editor operation so a separate preview panel is not required.
- Nice-to-have: keep standalone editor input/navigation fully responsive while an AI assist request is active; cancellation remains the required behavior.
- Harden the standalone editor with resize tests, multi-panel tests, scroll commands, search, and full Unicode grapheme/cell-width handling.

## Chat Persistence

- Add focused integration coverage for SQLite-backed TUI chat storage in `~/.pkchat/pkchat.db`, including automatic save/load, `/new`, `/remove`, `/provider`, and `/list` picker behavior.
- Harden SQLite autosave scheduling so an explicit JSON `/save` or slow file job can be followed by a deferred SQLite save instead of skipping that autosave.
- Add recovery behavior when `app_state.last_thread_id` points at a deleted or missing thread.
- Keep explicit JSON save/load as import/export compatibility, not the primary local chat library.
- Add SQLite schema migrations, corruption handling, permission-denied tests, and leak checks for open/save/load/list/remove paths.

## Provider And API Hardening

- Continue v0.4 provider work: provider capability probing, provider-specific error normalization, broader Responses API schema coverage, and adapter docs/tests for real providers.
- Add provider-reported context limits and improve token estimation.
- Add Responses API image input support.
- Expand JSON handling behind the existing facade or vendor a reviewed JSON library.
- Add more credential-redaction and provider error-path tests.

## Benchmark Mode

- Add deterministic refusal checks and rubric/judge scoring for benchmark `reference_answer`, `assessment_criteria`, and `safety` metadata; keep descriptive metadata unscored until matching and judge semantics are specified and tested.
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

1. PDF input (PDF-to-Markdown) and PDF output (Markdown-to-PDF).
2. MS Word input (`.docx`-to-Markdown) and MS Word output (Markdown-to-`.docx`).

These formats are intentionally deferred. Do not treat PDF or DOCX binary data as prompt text.

## Postponed Browser Web UI

- Browser local web UI is postponed. Prefer local OpenAI-compatible server mode first, with rudimentary authentication and local conversion pseudo-models such as `html-to-md` and `md-to-html`.
