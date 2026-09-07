# Code-pruning baseline

This baseline precedes the safe-pruning tranches and uses commit
`caebb0f3c839e7438cebdf0f6687da5e1b1d5f73` (v1.32). It was captured on
Linux/aarch64 with Linux 6.17 and GCC 13.3.0. Generated files, embedded browser
sources, tests, and documentation are deliberately reported separately.

## Source inventory

| Class | Files | Lines |
| --- | ---: | ---: |
| Native production C/C++ under `src/` and `include/`, excluding the generated Unicode table | 348 | 115,959 |
| Embedded WebUI HTML/CSS/JavaScript under `src/web/` | 9 | 7,339 |
| Generated `src/editor/detail/unicode_word_data.hpp` | 1 | 2,992 |
| Test code | 126 | 42,428 |
| Markdown and text documentation | 41 | 8,480 |
| Configuration, scripts, tools, and resource code | 21 | 4,603 |

The checkout contained 695 tracked paths and approximately 228,406 tracked
lines. Two tracked fixture paths were absent from the working tree, so the
repository-wide total is informational; the scoped source counts above are the
authoritative pruning measurements.

The pre-existing documentation edits were left untouched. The untracked root
`secret.txt` is excluded by `.gitignore` and was neither inspected nor added to
any measurement input.

## Build and test measurements

| Measurement | Result |
| --- | --- |
| Clean default build, `make -j10 all` | 11.05 s wall, 542,580 KiB peak RSS |
| Executable size | 15,409,696 bytes |
| GNU `size` | 9,171,225 text; 34,752 data; 9,256 bss; 9,215,233 total |
| Cold `make -j10 test-unit` | passed; 23.23 s wall, 413,812 KiB peak RSS |
| `make -j10 test-full` | reached editor integration in 37.08 s, then failed because the bare-editor test inherited a persisted online model selection from the shared test HOME |
| `make -j10 test-sanitize` | sanitized unit, WebUI, fault, and code-index tests passed; the same editor-test state leak stopped the suite after 305.48 s; no ASan/UBSan finding preceded it |

The compiler emitted existing libstdc++ `std::regex` maybe-uninitialized
warnings and test-only ignored-result warnings during the sanitizer build.
These are not sanitizer findings.

## Code-index measurements

The fixed corpus is an archive of the baseline commit containing 608 eligible
files. Commands used `--no-config --index-code`.

| Measurement | Result |
| --- | --- |
| Cold index | 317 ms internal, 0.34 s wall, 33,916 KiB peak RSS |
| Indexed output | 608 files, 17,547 symbols, 16 workers |
| Refresh | 7 ms internal, 0.01 s wall, 15,692 KiB peak RSS |
| SQLite database | 5,451,776 bytes |

The sorted symbol fingerprint is SHA-256
`b4ffb7c66da742a98047773df0125094d3e487aa52ff65683586d464b441b50b`.
It covers path, name, kind, qualified name, line range, signature, parameters,
return type, documentation, and importance, sorted deterministically by path,
range, kind, qualified name, name, and signature.

## Compatibility fingerprints

- `ainiux --help` SHA-256:
  `6d0769995d35014e8e7e41b1d8cf1c5293d718f434287e21fd13d4c42680655d`.
- CLI acceptance and diagnostic behavior is characterized by the table-driven
  CLI unit tests plus the CLI integration smoke tests.
- Native agent tool names, schemas, and handlers are characterized by the
  file-tool unit tests. The pruning tranche adds a one-to-one descriptor/handler
  invariant.
- Scanner fixture symbols and deterministic ordering are characterized by the
  index unit suite and `tests/integration/test_code_index.sh`.
- Editor width, completion, layout, clipboard, and reformat behavior is covered
  by the editor and TUI unit suites and their PTY integration drivers.

The indentation detector already allocated `kMaxTabWidth + 1` buckets at this
baseline, so the reported width-32 out-of-bounds defect was not present in the
v1.32 source. A focused width-32 boundary test was added before pruning to keep
that invariant explicit.

## First pruning tranche results

This tranche removed statically unreferenced wrappers and ranking remnants,
deleted the unused credit-balance job and redundant generic subprocess
translation unit, replaced the agent tool definition/dispatch duplication with
one descriptor registry, and removed obsolete tool-name special cases. It also
consolidated request-secret collection, agent session option construction,
editor longest-common-prefix handling, and shared code-index scanner mechanics.
TOML and INI scanning now use a small line-rule table after lexical masking;
languages that require multiline or scope-aware logic retain their dedicated
scanners.

| Measurement | Baseline | After tranche | Change |
| --- | ---: | ---: | ---: |
| Native production files | 348 | 346 | -2 |
| Native production lines | 115,959 | 114,663 | -1,296 (-1.12%) |
| Embedded WebUI lines | 7,339 | 7,339 | unchanged |
| Executable size | 15,409,696 bytes | 15,353,816 bytes | -55,880 (-0.36%) |
| GNU `size` total | 9,215,233 bytes | 9,175,913 bytes | -39,320 (-0.43%) |
| Clean optimized build | 11.05 s / 542,580 KiB | 11.21 s / 543,352 KiB | +1.45% / +0.14% |
| Fixed-corpus cold index | 317 ms / 33,916 KiB | 293 ms / 34,276 KiB | -7.57% / +1.06% |
| Fixed-corpus refresh | 7 ms / 15,692 KiB | 6 ms median / 15,940 KiB first run | -14.29% / +1.58% |

The post-tranche fixed corpus still contains 608 files and 17,547 symbols, and
the SQLite database remains 5,451,776 bytes. Bidirectional SQL `EXCEPT` checks
between the baseline and post-tranche symbol tables returned zero rows, covering
all fingerprinted symbol fields. The CLI help SHA-256 is also unchanged.

Verification completed on Linux/aarch64:

- repeated `make -j10 test-unit` runs passed;
- `make -j10 test` passed;
- `make test-integration` passed in 221.25 seconds;
- `make -j10 test-sanitize` passed its sanitized unit, WebUI, fault, complete
  integration, and local llama-server gates in 792.31 seconds, with no ASan or
  UBSan finding (peak RSS 1,463,376 KiB);
- a whole-production `cppcheck --enable=unusedFunction` pass found no additional
  deletion candidate without a test or platform consumer.

The integration drivers now isolate HOME, XDG directories, and project working
directories where required. This fixes the baseline suite's order-dependent
leak of persisted editor/model state without weakening the scenarios.

Native Windows CI and Valgrind were not available in this environment. The
platform-sensitive file-lock consolidation is therefore deferred. The
declarative CLI parser and any option removals are separate future tranches;
accepted spellings and diagnostics remain unchanged, and no removal list has
been proposed without the required manual approval.
