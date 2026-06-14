# TODO

- Replace the current curl-executable transport fallback with a libcurl RAII transport once libcurl development headers are available.
- Expand JSON handling behind the existing facade or vendor a reviewed JSON library.
- Add broader error-path and credential-redaction tests.
- Add true incremental stream delivery from the HTTP layer.
- Expand REPL persistence into XDG chat IDs, `/chat` listing, `/new`, and schema migrations.
