# Decisions

## C++17 and Makefile

The initial implementation uses C++17 and a plain Makefile to keep the binary portable across POSIX-like systems.

## HTTP Transport Fallback

`src/http/` owns HTTP transport. The desired implementation is libcurl with RAII wrappers, but this environment does not currently provide libcurl development headers or `curl-config`. To keep v0.1 testable, the first transport executes the installed `curl` binary via `fork`/`execvp` with pipes, avoiding shell interpolation. The module boundary is intentionally small so it can be replaced by libcurl without changing provider or CLI code.

## JSON Facade

`src/json/` is a small internal JSON facade used for request escaping and provider response parsing. The project should replace or expand it with a reviewed JSON library when dependency installation is available.
