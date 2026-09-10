# PDF-to-Markdown extract speed (2026-09-09)

One wall-clock pass over `tests/pdf_files/`. No rebuild: timings used the existing
stripped `./ainiux` binary. Ainiux wrote Markdown (`--output-format md`). **pypdf
6.18.0** and **pdfplumber 0.11.10** extracted plaintext in-process after a single
import, so they did not pay a new process start per file; Ainiux did.

Each converter ran **once** per file. Times are `time.perf_counter()` seconds.
Speedup is `other / ainiux` (how many times faster Ainiux was). Ainiux needed
`--max-input-bytes` above the default 10 MiB for `State-of-AI.pdf` (10.7 MiB).

Python libraries lived in a throwaway venv and are not product dependencies.

## Results

| File | Size | Pages | Ainiux | pypdf | pdfplumber | Ainiux vs pypdf | Ainiux vs pdfplumber | Ainiux pages/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `ru-llm-eval-short-note.pdf` | 58 KiB | 1 | 0.008 s | 0.004 s | 0.019 s | **0.5×** (slower) | **2.4×** | 129 |
| `ru-transformers-short-note.pdf` | 62 KiB | 2 | 0.007 s | 0.009 s | 0.045 s | **1.3×** | **6.8×** | 302 |
| `arabic-prose-sample.pdf` | 81 KiB | 8 | 0.009 s | 0.040 s | 0.291 s | **4.4×** | **31.9×** | 875 |
| `Nvidia-Quarterly-Presentation-final-1.pdf` | 1.7 MiB | 17 | 0.012 s | 0.128 s | 0.245 s | **10.5×** | **20.1×** | 1,396 |
| `DeepSeek2501.12948v1.pdf` | 1.3 MiB | 22 | 0.024 s | 0.176 s | 0.669 s | **7.4×** | **27.9×** | 919 |
| `State-of-AI.pdf` | 10.7 MiB | 36 | 0.042 s | 0.281 s | 1.058 s | **6.7×** | **25.3×** | 861 |
| `chinese-proverbs-collection.pdf` | 365 KiB | 46 | 0.015 s | 0.184 s | 0.578 s | **12.3×** | **38.6×** | 3,073 |
| `fortum-tammi-kesakuun-2026-puolivuosikatsaus.pdf` | 946 KiB | 61 | 0.057 s | 1.830 s | 3.137 s | **32.2×** | **55.2×** | 1,074 |
| `dgx-spark.pdf` | 2.2 MiB | 81 | 0.029 s | 0.719 s | 2.072 s | **24.5×** | **70.5×** | 2,758 |
| `dracula.pdf` | 974 KiB | 353 | 0.077 s | 1.283 s | 10.864 s | **16.7×** | **141.8×** | 4,606 |
| **TOTAL** | **18.3 MiB** | **627** | **0.279 s** | **4.655 s** | **18.977 s** | **16.7×** | **68.0×** | **2,245** |

On this corpus Ainiux finished in **0.28 s**. pypdf took **4.7 s** (about **17×**
slower). pdfplumber took **19 s** (about **68×** slower). The 353-page
`dracula.pdf` is the clearest case: Ainiux **77 ms** vs pypdf **1.3 s** vs
pdfplumber **10.9 s**.

The 1-page Russian note is the exception: Ainiux’s process start (~8 ms) is larger
than pypdf’s in-process extract (~4 ms). After a few pages that overhead
disappears.

Ainiux extraction in this snapshot is single-threaded. Parallel page extract was
not used.

## How it was run

```sh
./ainiux --input FILE --output-format md --output /tmp/out.md --max-input-bytes N
```

pypdf: `PdfReader(path)` then `page.extract_text()` for every page.

pdfplumber: `pdfplumber.open` then `page.extract_text()` for every page.

This is a speed snapshot, not a quality comparison. Markdown vs plaintext and
layout fidelity are out of scope here. Re-run with `scripts/ainiux/pdf_compare.py`
for extract quality vs `pdftotext`.
