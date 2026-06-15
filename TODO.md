# TODO

- Replace the chat TUI input box with the standalone editor panel once editor resize/cursor behavior is hardened.
- Harden the standalone editor with resize tests, multi-panel tests, scroll commands, search, and full Unicode grapheme/cell-width handling.
- Harden the TUI foundation with resize tests, scrollback, better Unicode cell-width handling, and broader command coverage.
- Expand runtime cancellation tests to cover interrupted streaming HTTP and slow file jobs.
- Expand JSON handling behind the existing facade or vendor a reviewed JSON library.
- Add broader error-path and credential-redaction tests.
- Expand REPL persistence into XDG chat IDs, `/chat` listing, `/new`, and schema migrations.
