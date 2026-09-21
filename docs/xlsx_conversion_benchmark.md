# XLSX conversion benchmark (2026-09-15)

This point-in-time benchmark compares **speed** of XLSX↔Markdown conversion, not lossless workbook layout. Each of the 48 candidate/file combinations ran exactly once in a fresh process with no warmup or retry. Wall time includes process and library startup, conversion, and output writes.

## Method

Ainiux was a `make -j16 optimized` binary (`-O3 -DNDEBUG`). All conversions were pinned to logical CPU 5 with `LC_ALL=C.UTF-8`, `PYTHONHASHSEED=0`, and isolated output directories. Candidate order rotates by input to reduce fixed cache-order bias. Ainiux was invoked by explicit repository path; its revision and binary digest are recorded below. The host `php` 8.3.6 lacked `ext-zip` / `ext-xml`; PhpSpreadsheet therefore ran under PHP 8.3.33 extracted from a `php:8.3-cli` image with `zip` enabled, invoked as a normal host process (not `docker run` per conversion).

Neither openpyxl nor PhpSpreadsheet converts Markdown natively. This harness maps worksheet cell values to GitHub-flavored tables (sheet name as a heading) and Markdown tables back to a new workbook, matching Ainiux's documented XLSX subset. Formula cells use cached values; the Excel calculation engine is not run. Drawings, charts, styles, and merged-cell layout are out of scope.

XLSX→Markdown uses the eight in-tree workbooks listed below. Markdown→XLSX uses the adjacent `countries-population.md` and `hello-world.md` goldens, and Ainiux-extracted Markdown (untimed) for the other six workbooks so every writer sees the same source.

A result is incomplete when conversion fails, the output is missing/empty, Markdown is not valid UTF-8, or a generated XLSX package has no workbook part. Incomplete results remain visible but cannot contribute to speed rankings.

## Per-file results

| Direction | File | Candidate | Input→output | Wall | CPU | Peak RSS | Throughput | Exit | Complete | vs Ainiux |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- | ---: |
| XLSX→MD | `tests/xlsx_files/1mb.xlsx` | Ainiux | 1.1 MiB→3.0 MiB | 0.1175 s | 0.1126 s | 37.3 MiB | 9.8 MiB/s | 0 | yes | 1.00× |
| XLSX→MD | `tests/xlsx_files/1mb.xlsx` | openpyxl adapter | 1.1 MiB→3.0 MiB | 0.2658 s | 0.2648 s | 45.1 MiB | 4.3 MiB/s | 0 | yes | 2.26× |
| XLSX→MD | `tests/xlsx_files/1mb.xlsx` | PhpSpreadsheet adapter | 1.1 MiB→3.0 MiB | 0.3197 s | 0.3185 s | 129.6 MiB | 3.6 MiB/s | 0 | yes | 2.72× |
| XLSX→MD | `tests/xlsx_files/countries-population.xlsx` | openpyxl adapter | 12.1 KiB→16.8 KiB | 0.0805 s | 0.0795 s | 29.0 MiB | 150.9 KiB/s | 0 | yes | 5.37× |
| XLSX→MD | `tests/xlsx_files/countries-population.xlsx` | PhpSpreadsheet adapter | 12.1 KiB→16.8 KiB | 0.0328 s | 0.0319 s | 31.0 MiB | 370.1 KiB/s | 0 | yes | 2.19× |
| XLSX→MD | `tests/xlsx_files/countries-population.xlsx` | Ainiux | 12.1 KiB→16.8 KiB | 0.0150 s | 0.0099 s | 17.1 MiB | 810.1 KiB/s | 0 | yes | 1.00× |
| XLSX→MD | `tests/xlsx_files/hello-world.xlsx` | PhpSpreadsheet adapter | 4.2 KiB→1.6 KiB | 0.0241 s | 0.0236 s | 27.7 MiB | 174.1 KiB/s | 0 | yes | 2.23× |
| XLSX→MD | `tests/xlsx_files/hello-world.xlsx` | Ainiux | 4.2 KiB→1.6 KiB | 0.0108 s | 0.0079 s | 17.2 MiB | 387.7 KiB/s | 0 | yes | 1.00× |
| XLSX→MD | `tests/xlsx_files/hello-world.xlsx` | openpyxl adapter | 4.2 KiB→1.6 KiB | 0.0758 s | 0.0752 s | 28.2 MiB | 55.3 KiB/s | 0 | yes | 7.01× |
| XLSX→MD | `tests/xlsx_files/threesheets.xlsx` | Ainiux | 11.8 KiB→114 B | 0.0133 s | 0.0088 s | 17.2 MiB | 888.6 KiB/s | 0 | yes | 1.00× |
| XLSX→MD | `tests/xlsx_files/threesheets.xlsx` | openpyxl adapter | 11.8 KiB→115 B | 0.0754 s | 0.0748 s | 28.2 MiB | 156.9 KiB/s | 0 | yes | 5.66× |
| XLSX→MD | `tests/xlsx_files/threesheets.xlsx` | PhpSpreadsheet adapter | 11.8 KiB→115 B | 0.0252 s | 0.0243 s | 27.7 MiB | 470.1 KiB/s | 0 | yes | 1.89× |
| XLSX→MD | `tests/xlsx_files/1900_Calendar.xlsx` | openpyxl adapter | 9.9 KiB→81 B | 0.0750 s | 0.0743 s | 28.2 MiB | 132.0 KiB/s | 0 | yes | 5.73× |
| XLSX→MD | `tests/xlsx_files/1900_Calendar.xlsx` | PhpSpreadsheet adapter | 9.9 KiB→61 B | 0.0247 s | 0.0239 s | 27.7 MiB | 400.6 KiB/s | 0 | yes | 1.89× |
| XLSX→MD | `tests/xlsx_files/1900_Calendar.xlsx` | Ainiux | 9.9 KiB→80 B | 0.0131 s | 0.0080 s | 17.2 MiB | 756.5 KiB/s | 0 | yes | 1.00× |
| XLSX→MD | `tests/xlsx_files/sharedformulae.xlsx` | PhpSpreadsheet adapter | 8.9 KiB→150 B | 0.0232 s | 0.0228 s | 28.0 MiB | 381.8 KiB/s | 0 | yes | 2.21× |
| XLSX→MD | `tests/xlsx_files/sharedformulae.xlsx` | Ainiux | 8.9 KiB→149 B | 0.0105 s | 0.0082 s | 17.2 MiB | 842.7 KiB/s | 0 | yes | 1.00× |
| XLSX→MD | `tests/xlsx_files/sharedformulae.xlsx` | openpyxl adapter | 8.9 KiB→150 B | 0.0752 s | 0.0746 s | 28.2 MiB | 118.0 KiB/s | 0 | yes | 7.14× |
| XLSX→MD | `tests/xlsx_files/utf16be.bom.xlsx` | Ainiux | 7.9 KiB→33 B | 0.0122 s | 0.0076 s | 17.2 MiB | 648.2 KiB/s | 0 | yes | 1.00× |
| XLSX→MD | `tests/xlsx_files/utf16be.bom.xlsx` | openpyxl adapter | 7.9 KiB→34 B | 0.0744 s | 0.0738 s | 28.1 MiB | 106.7 KiB/s | 0 | yes | 6.07× |
| XLSX→MD | `tests/xlsx_files/utf16be.bom.xlsx` | PhpSpreadsheet adapter | 7.9 KiB→34 B | 0.0250 s | 0.0241 s | 27.9 MiB | 317.2 KiB/s | 0 | yes | 2.04× |
| XLSX→MD | `tests/xlsx_files/without_cell_reference.xlsx` | openpyxl adapter | 6.2 KiB→507 B | 0.0787 s | 0.0781 s | 28.6 MiB | 79.2 KiB/s | 0 | yes | 7.35× |
| XLSX→MD | `tests/xlsx_files/without_cell_reference.xlsx` | PhpSpreadsheet adapter | 6.2 KiB→497 B | 0.0248 s | 0.0239 s | 28.2 MiB | 251.7 KiB/s | 0 | yes | 2.31× |
| XLSX→MD | `tests/xlsx_files/without_cell_reference.xlsx` | Ainiux | 6.2 KiB→506 B | 0.0107 s | 0.0077 s | 17.2 MiB | 582.2 KiB/s | 0 | yes | 1.00× |
| MD→XLSX | `results/xlsx-conversion-2026-09-15/prepared-md/1mb.md` | PhpSpreadsheet adapter | 3.0 MiB→1.1 MiB | 0.7052 s | 0.7043 s | 74.5 MiB | 4.3 MiB/s | 0 | yes | 4.00× |
| MD→XLSX | `results/xlsx-conversion-2026-09-15/prepared-md/1mb.md` | Ainiux | 3.0 MiB→1.1 MiB | 0.1761 s | 0.1715 s | 41.8 MiB | 17.1 MiB/s | 0 | yes | 1.00× |
| MD→XLSX | `results/xlsx-conversion-2026-09-15/prepared-md/1mb.md` | openpyxl adapter | 3.0 MiB→1.2 MiB | 0.4273 s | 0.4261 s | 45.6 MiB | 7.0 MiB/s | 0 | yes | 2.43× |
| MD→XLSX | `tests/xlsx_files/countries-population.md` | Ainiux | 17.0 KiB→12.1 KiB | 0.0148 s | 0.0097 s | 21.5 MiB | 1.1 MiB/s | 0 | yes | 1.00× |
| MD→XLSX | `tests/xlsx_files/countries-population.md` | openpyxl adapter | 17.0 KiB→13.5 KiB | 0.0834 s | 0.0828 s | 29.0 MiB | 203.3 KiB/s | 0 | yes | 5.63× |
| MD→XLSX | `tests/xlsx_files/countries-population.md` | PhpSpreadsheet adapter | 17.0 KiB→15.4 KiB | 0.0405 s | 0.0395 s | 29.7 MiB | 418.7 KiB/s | 0 | yes | 2.74× |
| MD→XLSX | `tests/xlsx_files/hello-world.md` | openpyxl adapter | 1.7 KiB→6.0 KiB | 0.0771 s | 0.0751 s | 28.5 MiB | 21.6 KiB/s | 0 | yes | 5.89× |
| MD→XLSX | `tests/xlsx_files/hello-world.md` | PhpSpreadsheet adapter | 1.7 KiB→7.4 KiB | 0.0278 s | 0.0268 s | 28.6 MiB | 60.0 KiB/s | 0 | yes | 2.12× |
| MD→XLSX | `tests/xlsx_files/hello-world.md` | Ainiux | 1.7 KiB→4.2 KiB | 0.0131 s | 0.0078 s | 21.5 MiB | 127.1 KiB/s | 0 | yes | 1.00× |
| MD→XLSX | `results/xlsx-conversion-2026-09-15/prepared-md/threesheets.md` | PhpSpreadsheet adapter | 114 B→7.9 KiB | 0.0249 s | 0.0244 s | 28.5 MiB | 4.5 KiB/s | 0 | yes | 2.43× |
| MD→XLSX | `results/xlsx-conversion-2026-09-15/prepared-md/threesheets.md` | Ainiux | 114 B→3.9 KiB | 0.0102 s | 0.0078 s | 21.5 MiB | 10.9 KiB/s | 0 | yes | 1.00× |
| MD→XLSX | `results/xlsx-conversion-2026-09-15/prepared-md/threesheets.md` | openpyxl adapter | 114 B→5.6 KiB | 0.0759 s | 0.0751 s | 28.5 MiB | 1.5 KiB/s | 0 | yes | 7.41× |
| MD→XLSX | `results/xlsx-conversion-2026-09-15/prepared-md/1900_Calendar.md` | Ainiux | 80 B→2.8 KiB | 0.0127 s | 0.0085 s | 21.5 MiB | 6.1 KiB/s | 0 | yes | 1.00× |
| MD→XLSX | `results/xlsx-conversion-2026-09-15/prepared-md/1900_Calendar.md` | openpyxl adapter | 80 B→4.7 KiB | 0.0763 s | 0.0755 s | 28.3 MiB | 1.0 KiB/s | 0 | yes | 5.99× |
| MD→XLSX | `results/xlsx-conversion-2026-09-15/prepared-md/1900_Calendar.md` | PhpSpreadsheet adapter | 80 B→6.0 KiB | 0.0274 s | 0.0264 s | 28.5 MiB | 2.9 KiB/s | 0 | yes | 2.15× |
| MD→XLSX | `results/xlsx-conversion-2026-09-15/prepared-md/sharedformulae.md` | openpyxl adapter | 149 B→4.8 KiB | 0.0778 s | 0.0759 s | 28.2 MiB | 1.9 KiB/s | 0 | yes | 6.20× |
| MD→XLSX | `results/xlsx-conversion-2026-09-15/prepared-md/sharedformulae.md` | PhpSpreadsheet adapter | 149 B→6.1 KiB | 0.0275 s | 0.0265 s | 28.5 MiB | 5.3 KiB/s | 0 | yes | 2.19× |
| MD→XLSX | `results/xlsx-conversion-2026-09-15/prepared-md/sharedformulae.md` | Ainiux | 149 B→2.9 KiB | 0.0126 s | 0.0086 s | 21.6 MiB | 11.6 KiB/s | 0 | yes | 1.00× |
| MD→XLSX | `results/xlsx-conversion-2026-09-15/prepared-md/utf16be.bom.md` | PhpSpreadsheet adapter | 33 B→5.9 KiB | 0.0257 s | 0.0251 s | 28.5 MiB | 1.3 KiB/s | 0 | yes | 2.24× |
| MD→XLSX | `results/xlsx-conversion-2026-09-15/prepared-md/utf16be.bom.md` | Ainiux | 33 B→2.8 KiB | 0.0115 s | 0.0079 s | 21.6 MiB | 2.8 KiB/s | 0 | yes | 1.00× |
| MD→XLSX | `results/xlsx-conversion-2026-09-15/prepared-md/utf16be.bom.md` | openpyxl adapter | 33 B→4.7 KiB | 0.0757 s | 0.0749 s | 28.4 MiB | 435 B/s | 0 | yes | 6.59× |
| MD→XLSX | `results/xlsx-conversion-2026-09-15/prepared-md/without_cell_reference.md` | Ainiux | 506 B→3.1 KiB | 0.0131 s | 0.0087 s | 21.6 MiB | 37.8 KiB/s | 0 | yes | 1.00× |
| MD→XLSX | `results/xlsx-conversion-2026-09-15/prepared-md/without_cell_reference.md` | openpyxl adapter | 506 B→5.1 KiB | 0.0773 s | 0.0764 s | 28.5 MiB | 6.4 KiB/s | 0 | yes | 5.91× |
| MD→XLSX | `results/xlsx-conversion-2026-09-15/prepared-md/without_cell_reference.md` | PhpSpreadsheet adapter | 506 B→6.3 KiB | 0.0272 s | 0.0262 s | 28.6 MiB | 18.2 KiB/s | 0 | yes | 2.08× |

`vs Ainiux` is candidate wall time divided by Ainiux wall time on the same file; larger is slower.

## Aggregate comparisons

Three-way totals use only the common complete subset: **16 of 16 cases**.

| Candidate | Eligible cases | Wall total | CPU total | Throughput | vs Ainiux |
| --- | ---: | ---: | ---: | ---: | ---: |
| Ainiux | 16 | 0.4673 s | 0.4011 s | 9.1 MiB/s | 1.00× |
| openpyxl adapter | 16 | 1.7715 s | 1.7569 s | 2.4 MiB/s | 3.79× |
| PhpSpreadsheet adapter | 16 | 1.4057 s | 1.3924 s | 3.0 MiB/s | 3.01× |

Full-corpus totals are shown only for candidates completing every case. Eligible: Ainiux, openpyxl adapter, PhpSpreadsheet adapter.

| Candidate | Cases | Wall total | CPU total | Throughput | vs Ainiux |
| --- | ---: | ---: | ---: | ---: | ---: |
| Ainiux | 16 | 0.4673 s | 0.4011 s | 9.1 MiB/s | 1.00× |
| openpyxl adapter | 16 | 1.7715 s | 1.7569 s | 2.4 MiB/s | 3.79× |
| PhpSpreadsheet adapter | 16 | 1.4057 s | 1.3924 s | 3.0 MiB/s | 3.01× |

## Findings

- On the 16-case common complete subset, **Ainiux was fastest** at 0.4673 seconds total. openpyxl adapter was 3.79× Ainiux time; PhpSpreadsheet adapter was 3.01× Ainiux time.
- Complete conversions: Ainiux 16/16, openpyxl adapter 16/16, PhpSpreadsheet adapter 16/16.

## Completeness and validation

All 48 conversions met the completeness rule. Generated XLSX packages were checked as ZIP archives with a workbook relationship.

## Environment

| Item | Value |
| --- | --- |
| Timestamp | `2026-09-15T08:47:15.029370+00:00` |
| Host | `Linux-6.17.0-1021-nvidia-aarch64-with-glibc2.39` |
| CPU | `aarch64` |
| Python | `3.12.3 (main, Mar 23 2026, 19:04:32) [GCC 13.3.0]` |
| PHP | `PHP 8.3.33 (cli) (built: Aug 25 2026 00:39:16) (NTS)` |
| openpyxl | `3.1.5` |
| PhpSpreadsheet | `/home/eye/agent_analysis/PhpSpreadsheet @9e6263ebe` |
| Ainiux revision | `b2e965e3a7751129a88029833d3af9bc0a9288d2` |
| Ainiux SHA-256 | `c4382b3dfdd0d159ce7cfe308e25aaa05a7fc0f24043e475093c406ac1cfb014` |
| Ainiux binary | `/home/eye/ainiux/ainiux` |
| Harness SHA-256 | `d4a9117320c64efecaad44fa12a82d9e1b49f27f2f49f334e337b2f9450ead0b` |

Exact harness command:

```sh
make -j16 optimized
python3 scripts/ainiux/xlsx_benchmark.py --bootstrap \
  --ainiux ./ainiux \
  --php ./build/php-xlsx/php \
  --phpspreadsheet ~/agent_analysis/PhpSpreadsheet \
  --cpu 5 \
  --output-dir results/xlsx-conversion-2026-09-15 \
  --report docs/xlsx_conversion_benchmark.md
```

## Limitations and follow-up

These are single observations, not statistically stable latency estimates. OS cache state still matters despite fresh processes and rotated order. Competitor numbers belong to this benchmark's Markdown adapters because the libraries read and write workbooks but do not provide Markdown converters. Adapter mapping of dates, empty cells, and cached formulas can differ from Ainiux without failing the speed completeness rule.

This benchmark does not change the production converter. Parser/writer follow-ups should receive focused XLSX fixtures and failure-path tests in a separate change.
