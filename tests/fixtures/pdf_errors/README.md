# Malformed PDF regression fixtures

These are deliberately small, synthetic documents for PDF-to-Markdown failure
tests. Keep them out of `tests/pdf_files/`, which is the successful extraction
and quality-comparison corpus. No downloads or third-party PDF library is needed.

Run `make test-pdf` for only the PDF units. Hostile cases run in separate child
processes with a 1.5-second timeout each (scaled by `AINIUX_TEST_TIME_SCALE`) and
8 KiB captured-output limits. Crashes, timeouts, exceptions and unexpected
successes fail the tests; they are never counted as expected rejections. On
POSIX the child disables core dumps; on Windows it disables crash dialogs.

For a single case after building, use, for example:

```sh
build/test_runner --pdf-case error-self-referencing-length
```

The single-case command runs directly, without the parent timeout. Use
`make test-pdf` when testing a potentially hanging/crashing parser.

The parser may recover xref damage. It also reports page-level decoding failures
as `[page 1: ...]` in successful Markdown output. Tests accept that documented
page-error behavior where appropriate, but reject silent success or a generic
scanned-page placeholder for malformed streams.

| Fixture (`.pdf`) | Damage / expected behavior |
| --- | --- |
| `error-unsupported-version-3.0` | Synthetic unsupported major-version header; `Bad PDF header`. Changing only the header to 1.4 provides a valid control. |
| `error-missing-root` | Trailer has no `/Root`; file-open failure. |
| `error-dangling-pages-reference` | Catalog points at a nonexistent `/Pages` object; file-open failure. |
| `error-self-referencing-length` | Content stream's `/Length` refers to itself; reject without recursion/stack overflow. |
| `error-cyclic-stream-lengths` | Two stream `/Length` references form a cycle; reject without recursion/stack overflow. |
| `error-stream-length-past-eof` | Very large direct stream length; bounded file/page error. |
| `error-indirect-length-overflow` | Indirect stream length exceeds the integer/address range; bounded file/page error. |
| `error-page-tree-cycle` | `/Kids` refers back to its `/Pages` object; explicit cycle error. |
| `error-cid-width-range-overflow` | CID width range ends at `UINT32_MAX`; reject without a wrapping loop or huge allocation. |
| `error-repeated-page-tree` | Repeated child references create exponential traversal; reject promptly or honor `max_pages=1` promptly. |
| `error-unterminated-cmap-array` | ToUnicode `bfrange` destination array ends at EOF; explicit page error. |
| `error-oversized-cmap-range` | ToUnicode range would expand to 2^32 mappings; explicit page error before allocation. |
| `error-corrupt-flate-stream` | Content stream claims Flate but contains invalid compressed data; explicit page error. |
| `error-unsupported-content-filter` | Unknown content-stream filter; explicit page error. |
| `error-unterminated-content-string` | Content-stream string ends before `)`; explicit page error. |
| `error-invalid-content-hex` | Non-hex characters in a content operand; explicit page error. |
| `error-unterminated-pages-array` | Truncated `/Kids` array/dictionary; explicit file error. |

Additional in-memory cases test numeric reference/integer bounds, excessive token
length/nesting, truncated/high-ratio Flate and ASCII85 expansion limits, PNG
predictor arithmetic overflow, input-byte limits, compressed-object header/offset
bounds and storage growth, repeated failure cleanup, deep acyclic dependencies,
lazy-load cancellation, simple/CID font bounds, non-finite geometry, strict text
ordering, and deterministic truncations and byte mutations of the small valid
control. Valid controls and parsing after
failure help distinguish actual rejection behavior from a broken test setup.

The PDFs use uncompressed ASCII bodies and classic xref tables with byte-accurate
offsets. When editing, recalculate `/Length`, xref offsets, and `startxref` unless
that field is intentionally malformed. The local Git attributes preserve those
byte counts across Windows checkouts.
