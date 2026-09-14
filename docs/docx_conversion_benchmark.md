# DOCX conversion benchmark (2026-09-14)

This point-in-time benchmark compares canonical semantic conversion, not lossless Word layout. Each of the 24 candidate/file combinations ran exactly once in a fresh process with no warmup or retry. Wall time includes process and library startup, conversion, and output writes.

## Method

All conversions were pinned to logical CPU 5 with `LC_ALL=C.UTF-8`, `PYTHONHASHSEED=0`, and isolated output directories. Candidate order rotates by input to reduce fixed cache-order bias. Ainiux was invoked by explicit repository path; its revision and binary digest are recorded below.

The independent standard-library oracle uses the adjacent Markdown goldens for the two small DOCX fixtures, extracts supported main-story OOXML semantics for the two large DOCX files, and treats each Markdown source as the expected model for Markdown-to-DOCX. Counted tokens, ordered token bigrams, and text-anchored structures are scored independently. Structures cover headings, ordered/unordered lists, tables, links, bold/italic/underline/strike, code, quotes, tabs, and hard breaks. Images, headers/footers, notes, comments, fonts, colors, and page layout are excluded.

A result is incomplete when conversion or format validation fails, counted-token recall is below 99%, or the final source block is absent. Incomplete results remain visible but cannot contribute to speed rankings. Unlicensed Spire.Doc evaluation text is retained and counted as extra content.

## Per-file results

| Direction | File | Candidate | Input→output | Wall | CPU | Peak RSS | Throughput | Exit | Token P/R/F1 | Bigram F1 | Structure F1 | Complete | vs Ainiux |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | --- | ---: |
| DOCX→MD | `tests/docx_files/headings-lists-links.docx` | Ainiux | 7.0 KiB→260 B | 0.0142 s | 0.0104 s | 19.5 MiB | 490.8 KiB/s | 0 | 100.00%/100.00%/100.00% | 100.00% | 100.00% | yes | 1.00× |
| DOCX→MD | `tests/docx_files/headings-lists-links.docx` | python-docx adapter | 7.0 KiB→251 B | 0.0730 s | 0.0723 s | 33.0 MiB | 95.4 KiB/s | 0 | 100.00%/100.00%/100.00% | 100.00% | 69.57% | yes | 5.14× |
| DOCX→MD | `tests/docx_files/headings-lists-links.docx` | Spire.Doc | 7.0 KiB→355 B | 0.1949 s | 0.1918 s | 88.3 MiB | 35.7 KiB/s | 0 | 70.27%/100.00%/82.54% | 81.97% | 95.65% | yes | 13.73× |
| DOCX→MD | `tests/docx_files/tables.docx` | python-docx adapter | 5.9 KiB→182 B | 0.0725 s | 0.0711 s | 32.9 MiB | 81.5 KiB/s | 0 | 77.78%/100.00%/87.50% | 80.00% | 0.00% | yes | 6.61× |
| DOCX→MD | `tests/docx_files/tables.docx` | Spire.Doc | 5.9 KiB→1.8 KiB | 0.2500 s | 0.2288 s | 96.1 MiB | 23.6 KiB/s | 0 | 56.00%/100.00%/71.79% | 70.27% | 91.67% | yes | 22.80× |
| DOCX→MD | `tests/docx_files/tables.docx` | Ainiux | 5.9 KiB→162 B | 0.0110 s | 0.0081 s | 19.5 MiB | 538.4 KiB/s | 0 | 100.00%/100.00%/100.00% | 100.00% | 100.00% | yes | 1.00× |
| DOCX→MD | `~/test_files/SVG-kuvien optimointi_valmis.docx` | Spire.Doc | 4.6 MiB→45.2 KiB | 1.2772 s | 1.2758 s | 293.2 MiB | 3.6 MiB/s | 0 | 75.67%/97.70%/85.28% | 80.34% | 15.30% | **no** | — |
| DOCX→MD | `~/test_files/SVG-kuvien optimointi_valmis.docx` | Ainiux | 4.6 MiB→42.2 KiB | 0.0382 s | 0.0316 s | 23.5 MiB | 121.5 MiB/s | 0 | 78.18%/99.86%/87.70% | 85.49% | 14.41% | yes | 1.00× |
| DOCX→MD | `~/test_files/SVG-kuvien optimointi_valmis.docx` | python-docx adapter | 4.6 MiB→31.2 KiB | 0.1306 s | 0.1296 s | 45.4 MiB | 35.6 MiB/s | 0 | 100.00%/99.37%/99.68% | 99.05% | 31.96% | yes | 3.42× |
| DOCX→MD | `~/test_files/opinnäytetyö_Laura_Byman_final.docx` | Ainiux | 6.7 MiB→54.3 KiB | 0.0522 s | 0.0451 s | 28.3 MiB | 127.8 MiB/s | 0 | 97.50%/99.84%/98.66% | 98.50% | 33.76% | yes | 1.00× |
| DOCX→MD | `~/test_files/opinnäytetyö_Laura_Byman_final.docx` | python-docx adapter | 6.7 MiB→50.6 KiB | 0.1879 s | 0.1865 s | 48.7 MiB | 35.5 MiB/s | 0 | 100.00%/99.84%/99.92% | 99.78% | 70.68% | yes | 3.60× |
| DOCX→MD | `~/test_files/opinnäytetyö_Laura_Byman_final.docx` | Spire.Doc | 6.7 MiB→56.3 KiB | 1.2320 s | 1.2299 s | 235.3 MiB | 5.4 MiB/s | 0 | 97.00%/99.77%/98.37% | 98.02% | 41.21% | yes | 23.62× |
| MD→DOCX | `tests/docx_files/supported.md` | python-docx adapter | 281 B→36.2 KiB | 0.0878 s | 0.0866 s | 38.6 MiB | 3.1 KiB/s | 0 | 100.00%/100.00%/100.00% | 100.00% | 70.97% | yes | 7.67× |
| MD→DOCX | `tests/docx_files/supported.md` | Spire.Doc | 281 B→8.6 KiB | 0.3457 s | 0.3413 s | 128.9 MiB | 812 B/s | 0 | 67.50%/93.10%/78.26% | 71.64% | 54.55% | **no** | — |
| MD→DOCX | `tests/docx_files/supported.md` | Ainiux | 281 B→4.2 KiB | 0.0115 s | 0.0083 s | 28.3 MiB | 23.9 KiB/s | 0 | 96.67%/100.00%/98.31% | 94.74% | 70.59% | yes | 1.00× |
| MD→DOCX | `tests/fixtures/comprehensive.md` | Spire.Doc | 1.7 KiB→11.0 KiB | 0.5006 s | 0.3851 s | 143.1 MiB | 3.4 KiB/s | 0 | 92.18%/100.00%/95.93% | 94.15% | 78.10% | yes | 37.10× |
| MD→DOCX | `tests/fixtures/comprehensive.md` | Ainiux | 1.7 KiB→5.1 KiB | 0.0135 s | 0.0086 s | 28.3 MiB | 127.7 KiB/s | 0 | 100.00%/100.00%/100.00% | 100.00% | 83.81% | yes | 1.00× |
| MD→DOCX | `tests/fixtures/comprehensive.md` | python-docx adapter | 1.7 KiB→37.1 KiB | 0.0948 s | 0.0934 s | 38.7 MiB | 18.2 KiB/s | 0 | 100.00%/100.00%/100.00% | 100.00% | 82.61% | yes | 7.02× |
| MD→DOCX | `~/test_files/we_wont_be_missed.md` | Ainiux | 78.0 KiB→0 B | 0.0093 s | 0.0080 s | 28.3 MiB | 8.2 MiB/s | 6 | 0.00%/0.00%/0.00% | 0.00% | 0.00% | **no** | — |
| MD→DOCX | `~/test_files/we_wont_be_missed.md` | python-docx adapter | 78.0 KiB→0 B | 0.0855 s | 0.0849 s | 38.3 MiB | 911.9 KiB/s | 2 | 0.00%/0.00%/0.00% | 0.00% | 0.00% | **no** | — |
| MD→DOCX | `~/test_files/we_wont_be_missed.md` | Spire.Doc | 78.0 KiB→23.2 KiB | 0.2243 s | 0.2227 s | 98.3 MiB | 347.8 KiB/s | 0 | 94.18%/66.65%/78.06% | 69.00% | 2.17% | **no** | — |
| MD→DOCX | `~/test_files/dracula.md` | python-docx adapter | 810.9 KiB→344.7 KiB | 0.2682 s | 0.2667 s | 43.4 MiB | 3.0 MiB/s | 0 | 100.00%/100.00%/100.00% | 100.00% | 0.00% | yes | 4.30× |
| MD→DOCX | `~/test_files/dracula.md` | Spire.Doc | 810.9 KiB→83.0 KiB | 3.3076 s | 3.2810 s | 146.3 MiB | 245.2 KiB/s | 0 | 99.99%/21.34%/35.17% | 35.17% | 0.00% | **no** | — |
| MD→DOCX | `~/test_files/dracula.md` | Ainiux | 810.9 KiB→354.0 KiB | 0.0625 s | 0.0548 s | 50.7 MiB | 12.7 MiB/s | 0 | 100.00%/100.00%/100.00% | 100.00% | 100.00% | yes | 1.00× |

`vs Ainiux` is candidate wall time divided by Ainiux wall time on the same file; larger is slower.

## Aggregate comparisons

Three-way totals use only the common complete subset: **4 of 8 cases**.

| Candidate | Eligible cases | Wall total | CPU total | Throughput | Mean token F1 | vs Ainiux |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Ainiux | 4 | 0.0908 s | 0.0723 s | 73.6 MiB/s | 99.66% | 1.00× |
| python-docx adapter | 4 | 0.4281 s | 0.4233 s | 15.6 MiB/s | 96.85% | 4.71× |
| Spire.Doc | 4 | 2.1775 s | 2.0355 s | 3.1 MiB/s | 87.16% | 23.98× |

Full-corpus totals are shown only for candidates completing all eight cases. Eligible: none.


## Findings

- On the 4-case common complete subset, **Ainiux was fastest** at 0.0908 seconds total. python-docx adapter was 4.71× Ainiux time; Spire.Doc was 23.98× Ainiux time.
- Complete conversions: Ainiux 7/8, python-docx adapter 7/8, Spire.Doc 4/8.
- All 10 generated DOCX packages that existed opened in LibreOffice.
- Spire evaluation text was present in 8/8 outputs and was retained in precision scoring. Its large-file recall losses are consistent with an unlicensed-limit risk, so those timings must not rank as complete work.
- `we_wont_be_missed.md` contains XML-forbidden control bytes. Ainiux and the python-docx adapter rejected it; Spire wrote a truncated document. Sanitize or explicitly reject forbidden controls before using this source in a success-only writer benchmark.
- The `Laura Byman` and `Niklas Mälton` regression needles were preserved in all 6 applicable Markdown outputs; no targeted emphasis or `****` artifact check fired.

## Completeness and validation

| File | Candidate | Reason | Evaluation text | LibreOffice |
| --- | --- | --- | --- | --- |
| `~/test_files/SVG-kuvien optimointi_valmis.docx` | Spire.Doc | token recall below 99% | yes | not available |
| `tests/docx_files/supported.md` | Spire.Doc | token recall below 99% | yes | opened |
| `~/test_files/we_wont_be_missed.md` | Ainiux | exit status 6, missing output, token recall below 99%, final source block absent | no | not available |
| `~/test_files/we_wont_be_missed.md` | python-docx adapter | exit status 2, missing output, token recall below 99%, final source block absent | no | not available |
| `~/test_files/we_wont_be_missed.md` | Spire.Doc | token recall below 99%, final source block absent | yes | opened |
| `~/test_files/dracula.md` | Spire.Doc | token recall below 99% | yes | opened |

Markdown outputs were decoded strictly as UTF-8 and checked for unbalanced emphasis delimiters, `****` artifacts, and the `Laura Byman` / `Niklas Mälton` regression needles. Generated DOCX files were inspected as ZIP/OPC packages and opened through LibreOffice headless text export.
No targeted Markdown artifact check fired.

### Converter diagnostics

These stderr diagnostics are also preserved verbatim beside each output and in `raw.json`.

| File | Candidate | Diagnostic |
| --- | --- | --- |
| `tests/docx_files/tables.docx` | Ainiux | warning: merged table cells were flattened into a rectangular Markdown table |
| `~/test_files/SVG-kuvien optimointi_valmis.docx` | Ainiux | warning: headers, footers, notes, or comments were omitted from DOCX conversion |
| `~/test_files/SVG-kuvien optimointi_valmis.docx` | Ainiux | warning: embedded images were replaced with visible Markdown placeholders |
| `~/test_files/opinnäytetyö_Laura_Byman_final.docx` | Ainiux | warning: headers, footers, notes, or comments were omitted from DOCX conversion |
| `~/test_files/opinnäytetyö_Laura_Byman_final.docx` | Ainiux | warning: embedded images were replaced with visible Markdown placeholders |
| `~/test_files/we_wont_be_missed.md` | Ainiux | AINIUX_ERR_UNSUPPORTED_FEATURE: cannot write DOCX: Markdown contains an XML-forbidden control byte at byte 18451 |
| `~/test_files/we_wont_be_missed.md` | python-docx adapter | docx benchmark: All strings must be XML compatible: Unicode or ASCII, no NULL bytes or control characters |

## Environment

| Item | Value |
| --- | --- |
| Timestamp | `2026-09-14T18:57:28.522512+00:00` |
| Host | `Linux-6.17.0-1021-nvidia-aarch64-with-glibc2.39` |
| CPU | `aarch64` |
| Python | `3.12.3 (main, Mar 23 2026, 19:04:32) [GCC 13.3.0]` |
| python-docx | `1.2.0` |
| Spire.Doc | `14.8.1` (unlicensed) |
| LibreOffice | `LibreOffice 24.2.7.2 420(Build:2)` |
| Ainiux revision | `aa89788141b850dc75b78ac282172d1c67350d26` |
| Ainiux SHA-256 | `0e0823558fa31d7bfae98f2cd797eb41ca20274d5faf12301f607471e2c09b4e` |
| Ainiux binary | `/home/eye/ainiux/ainiux` |
| Harness SHA-256 | `383313e676961821c89bd76b0eda775e19c309ed2df23dd3eacc664686f9a6f7` |

Exact harness command:

```sh
/usr/bin/python3 /home/eye/ainiux/scripts/ainiux/docx_benchmark.py --bootstrap --ainiux ./ainiux --cpu 5 --output-dir results/docx-conversion-2026-09-14 --report docs/docx_conversion_benchmark.md
```

The pinned releases are documented on [python-docx PyPI](https://pypi.org/project/python-docx/) and [Spire.Doc PyPI](https://pypi.org/project/spire-doc/). Spire's API exposes native `Markdown` and `Docx` formats in its [conversion interface](https://www.e-iceblue.com/Knowledgebase/Python/Spire.Doc-for-Python/Program-Guide/Conversion.html). The vendor states that unlicensed use can impose paragraph/table limits during load or save; see the [Spire limitation discussion](https://www.e-iceblue.com/forum/paragraph-recognizing-problem-t13628.html).

## Limitations and follow-up

These are single observations, not statistically stable latency estimates. OS cache state still matters despite fresh processes and rotated order. The python-docx numbers belong to this benchmark's semantic adapter because python-docx itself reads and writes Word documents but does not provide a Markdown converter. Spire ran without a license; evaluation content reduces precision and product limits may truncate large conversions.

Treat any low token/bigram score, structure mismatch, delimiter artifact, missing name needle, or LibreOffice discrepancy as a parser/writer follow-up. This benchmark intentionally does not change the production converter; fixes should receive focused DOCX fixtures and failure-path tests in a separate change.
