# TODO
- Add deterministic refusal checks and rubric/judge scoring for benchmark `reference_answer`, `assessment_criteria`, and `safety` metadata; keep descriptive metadata unscored until matching and judge semantics are specified and tested.
- Add Parquet benchmark input compatible with Hugging Face Datasets after the JSONL path is mature; keep the dependency isolated behind `src/benchmark/`.
- Continue v0.5 follow-up work: charset conversion, Responses API image input, provider-reported context limits/token estimation, and cancellable URL fetching for future web callers.

## Deferred Document Conversion

1. PDF input (PDF-to-Markdown) and PDF output (Markdown-to-PDF).
2. MS Word input (`.docx`-to-Markdown) and MS Word output (Markdown-to-`.docx`).

These formats are intentionally deferred. Do not treat PDF or DOCX binary data as prompt text.

- Continue v0.4 provider work: capability probing, provider-specific error normalization, broader Responses API schema coverage, and adapter docs/tests for real providers.
- Harden the standalone editor with resize tests, multi-panel tests, scroll commands, search, and full Unicode grapheme/cell-width handling.
- Harden the TUI foundation with interactive resize tests, model-output rendering tests, scrollback polish, better Unicode cell-width handling, and broader command coverage.
- Expand runtime cancellation tests to cover interrupted streaming HTTP and slow file jobs.
- Expand JSON handling behind the existing facade or vendor a reviewed JSON library.
- Add broader error-path and credential-redaction tests.
- Expand REPL persistence into XDG chat IDs, `/chat` listing, `/new`, and schema migrations.
