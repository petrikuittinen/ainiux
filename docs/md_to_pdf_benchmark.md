# Markdown-to-PDF write speed (2026-09-10)

One wall-clock pass over Markdown extracted from `tests/pdf_files/`. No rebuild:
timings used the existing stripped `./ainiux` binary. Setup (untimed) wrote each
PDF to Markdown with Ainiux. The timed work is **Markdown → PDF**. All three
writers received the same `.md` files.

**WeasyPrint 70.0** (Markdown → HTML → PDF) and **reportlab 5.0.1** (Markdown →
Platypus) ran in-process after a single import, so they did not pay a new
process start per file; Ainiux did. `python-markdown` 3.10.3 produced the HTML
fed to both Python writers.

Each converter ran **once** per file. Times are `time.perf_counter()` seconds.
`pages/s` uses that writer’s own output page count (`pdfinfo`). Layout engines
paginate differently, so page counts are not comparable across columns.
Speedup is `other / ainiux` (how many times faster Ainiux was).

Python libraries lived in a throwaway venv and are not product dependencies.
The first WeasyPrint file (`chinese-tang-poems-traditional.pdf`) includes
library and fontconfig init (~1.5 s); later 1-page files were ~45 ms.

## Results

| File | MD size | Ainiux | Ainiux p/s | WeasyPrint | WeasyPrint p/s | reportlab | reportlab p/s | vs WeasyPrint | vs reportlab |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `chinese-tang-poems-traditional.pdf` | 392 B | 0.016 s (1 p) | 64 | 1.455 s (1 p) | 0.7 | 0.008 s (1 p) | 128 | **92.6×** | **0.5×** (slower) |
| `hebrew-prose-sample.pdf` | 934 B | 0.007 s (1 p) | 140 | 0.045 s (1 p) | 22 | 0.003 s (1 p) | 356 | **6.3×** | **0.4×** (slower) |
| `ru-llm-eval-short-note.pdf` | 2.1 KiB | 0.007 s (1 p) | 147 | 0.046 s (1 p) | 22 | 0.004 s (1 p) | 249 | **6.8×** | **0.6×** (slower) |
| `ru-transformers-short-note.pdf` | 5.9 KiB | 0.007 s (2 p) | 273 | 0.055 s (2 p) | 36 | 0.009 s (2 p) | 230 | **7.6×** | **1.2×** |
| `Nvidia-Quarterly-Presentation-final-1.pdf` | 9.4 KiB | 0.008 s (5 p) | 654 | 0.159 s (6 p) | 38 | 0.013 s (5 p) | 386 | **20.8×** | **1.7×** |
| `arabic-prose-sample.pdf` | 33 KiB | 0.026 s (10 p) | 384 | 0.176 s (11 p) | 62 | 0.058 s (11 p) | 190 | **6.8×** | **2.2×** |
| `chinese-proverbs-collection.pdf` | 51 KiB | 0.027 s (25 p) | 922 | 2.064 s (29 p) | 14 | 0.089 s (24 p) | 269 | **76.1×** | **3.3×** |
| `DeepSeek2501.12948v1.pdf` | 56 KiB | 0.013 s (20 p) | 1,483 | 0.336 s (21 p) | 63 | 0.453 s (13 p) | 29 | **24.9×** | **33.6×** |
| `State-of-AI.pdf` | 89 KiB | 0.014 s (30 p) | 2,111 | 0.544 s (31 p) | 57 | 0.072 s (25 p) | 349 | **38.3×** | **5.0×** |
| `dgx-spark.pdf` | 150 KiB | 0.035 s (68 p) | 1,968 | 1.009 s (79 p) | 78 | 3.513 s (34 p) | 9.7 | **29.2×** | **101.7×** |
| `fortum-tammi-kesakuun-2026-puolivuosikatsaus.pdf` | 178 KiB | 0.021 s (63 p) | 2,945 | 0.807 s (64 p) | 79 | 0.125 s (53 p) | 423 | **37.7×** | **5.9×** |
| `dracula.pdf` | 811 KiB | 0.078 s (266 p) | 3,420 | 3.833 s (279 p) | 73 | 1.197 s (231 p) | 193 | **49.3×** | **15.4×** |
| **TOTAL** | **1.4 MiB** | **0.259 s (492 p)** | **1,898** | **10.529 s (525 p)** | **50** | **5.543 s (401 p)** | **72** | **40.6×** | **21.4×** |

On this corpus Ainiux finished in **0.26 s** (492 output pages, about **1,900
pages/s**). WeasyPrint took **10.5 s** (about **41×** slower, **50 pages/s**).
reportlab took **5.5 s** (about **21×** slower, **72 pages/s**). The 266-page
Ainiux `dracula.pdf` rewrite is the clearest case: Ainiux **78 ms** vs
WeasyPrint **3.8 s** vs reportlab **1.2 s**.

The 1-page notes are the exception versus reportlab: Ainiux’s process start
(~7–16 ms) is larger than in-process Platypus (~3–8 ms). After a few pages that
overhead disappears. WeasyPrint is slower on every file.

Ainiux writing in this snapshot is single-threaded.

## How it was run

Extract (untimed):

```sh
./ainiux --no-config --input FILE.pdf --output-format md --output FILE.md --max-input-bytes 33554432
```

`--max-input-bytes` is only required for extract of `State-of-AI.pdf` (10.7 MiB).
The Markdown bodies are all under 1 MiB.

Ainiux write:

```sh
./ainiux --no-config --input FILE.md --output-format pdf --output OUT.pdf --max-input-bytes 33554432
```

WeasyPrint: `markdown.markdown(..., extensions=["extra", "sane_lists"])` then
`weasyprint.HTML(string=...).write_pdf(...)` with A4 and 1 inch CSS margins.

reportlab: the same Markdown-to-HTML step, then Platypus
`SimpleDocTemplate` / `Paragraph` / `Preformatted` / `ListFlowable` on A4 with
1 inch margins.

This is a speed snapshot, not a quality comparison. Ainiux embeds subsetted
TrueType for CJK/Hebrew/Arabic; WeasyPrint uses system fonts; reportlab Core
fonts substitute those scripts. Output page counts differ because wrapping and
heading styles differ.
