# Testing ainiux

This document describes how to run the automated test suite, what it covers, and which mocks are used.

## Quick start

```sh
make test
```

This runs unit tests, fault-injection tests, and both integration scripts.

Useful targets:

| Target | What it runs |
|--------|----------------|
| `make test-unit` | `test_runner` plus `test_io_faults` (network/read-only/ENOSPC) |
| `make test-unit-faults` | Fault tests only |
| `make test-integration` | Mock-server end-to-end script plus SQLite TUI persistence script |
| `make test-integration-sqlite` | SQLite TUI persistence only |
| `make test-sanitize` | AddressSanitizer/UBSan build of the full `make test` path |
| `make test-leak` | Valgrind on `test_runner`, `test_io_faults`, and `ainiux --version` |

CI (`.github/workflows/ci.yml`) runs `make test` and `make test-leak` on Ubuntu with libcurl, libsqlite3, Python 3, and Valgrind installed.

## Layout

- `tests/unit/` — module-oriented C++ unit tests. `test_runner` dispatches `run_all()` from each module directory.
- `build/test_io_faults` — separate binary for slower or environment-dependent checks.
- `tests/integration/test_mock_server.sh` — CLI, REPL, benchmark/grade, fetch, config, attachments, and TUI insert coverage against a local mock API.
- `tests/integration/test_sqlite_persistence.sh` — SQLite-backed TUI persistence via `tui_sqlite_driver.py`.
- `tests/mock_server/` — Python HTTP mocks for OpenAI-compatible APIs and slow responses.
- `tests/mock/` — POSIX `LD_PRELOAD` shim for disk-full simulation.

Approximate size today: **900+** unit assertions, **170+** integration checks in the main mock-server script, plus SQLite integration scenarios and fault tests.

## Coverage overview

**Strong unit coverage**

- CLI parsing and options
- Provider registry, request shaping, and response parsing
- Config loading and validation
- HTML/Markdown/input/output conversion
- Editor piece table, panels, selection, and file I/O
- Chat JSON save/load
- SQLite store round-trip, listing, soft delete, corrupt DB, and missing-thread handling
- Runtime cancellation and event delivery
- Security redaction helpers
- Unicode, numeric, and malformed-input edge cases

**Integration coverage**

- Chat Completions and Responses API against `openai_mock.py`
- Streaming, JSON/NDJSON output, thinking-trace redaction
- REPL, benchmark modes, action-balanced safety ratings, configurable judge grading, URL fetch safety, attachments, images
- Editor/chat shared provider/model selectors, editor colored selector panels, provider-to-model discovery, and one-model auto-selection
- TUI insert/attach/fetch driver
- SQLite TUI workflows: `/new`, autosave/reload, `/list`, `/provider`, `/remove`, stale `last_thread_id`, corrupt database, image persistence across restart, `/cleanup`, and read-only expired-media threads

**Fault injection**

- Slow HTTP response and chunked-body timeouts (`slow_http_mock.py`)
- Connect timeout to an unreachable TEST-NET address
- Read-only file and directory permission failures (`chmod`)
- Simulated `ENOSPC` on tagged paths via `posix_io_mock.so`

## Mocks

### `tests/mock_server/openai_mock.py`

Local OpenAI-compatible server used by `test_mock_server.sh`. Supports model listing, chat completions, responses API, streaming, reasoning fields, attachments, images, HTML fetch fixtures, and strict configurable benchmark-grading requests.

### `tests/mock_server/slow_http_mock.py`

Used by `build/test_io_faults` for real local timeout tests:

- `/delay/N` — hold the response open
- `/slow-body` — slow chunked body
- `/health` — readiness probe

### `tests/mock/posix_io_mock.so`

Built by the Makefile and preloaded for ENOSPC tests:

```sh
AINIUX_MOCK_ENOSPC=1 LD_PRELOAD=build/posix_io_mock.so build/test_io_faults --enospc
```

Write attempts to paths containing `mock-enospc` fail with `ENOSPC`. Optional `AINIUX_MOCK_EACCES=1` blocks writes to `mock-eacces` paths.

### `tests/integration/tui_sqlite_driver.py`

PTY driver for TUI commands. Uses an isolated `HOME` so the database is created at `$HOME/.ainiux/ainiux.db` and verified with Python `sqlite3`.
It covers managed-image restart/expiration and canonical Markdown attachment replay: small inline Markdown, one-time HTML conversion to file-backed `.md`, same-process follow-ups, source changes after import, and restored-thread requests.

## Known gaps

- No automated tests against real OpenAI, LM Studio, or other production providers (mock-only CI).
- Valgrind does not cover the ENOSPC `LD_PRELOAD` path or the full integration shell scripts.
- Interactive TUI resize, long-running stress, and browser/web mode are not implemented yet and are not covered.
- Sanitizer builds are local/optional; CI currently uses Valgrind for leak checking on the unit/fault binaries.
