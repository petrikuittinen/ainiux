# PPTX conversion benchmark

Measured 2026-09-16 on `Linux-6.17.0-1021-nvidia-aarch64-with-glibc2.39` with 5 fresh processes per tool and corpus item. Run order rotated by iteration; raw commands, order, timing, RSS, size, and fidelity data are in the companion `measurements.json`.

| Direction | Tool | Median ms | Median slides/s | Median peak RSS MiB | Median output KiB | Mean text recall | Slide counts | pptxlint |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- | --- |
| markdown-to-pptx | LibreOffice LibreOffice 24.2.7.2 420(Build:2) | 1429.43 | 100.0 | 370.5 | 226.9 | 100.00% | pass | pass |
| markdown-to-pptx | PptxGenJS 4.0.1 | 154.89 | 923.2 | 82.4 | 1374.4 | 100.00% | pass | fail |
| markdown-to-pptx | ainiux Ainiux v1.36 | 31.82 | 4563.1 | 20.3 | 331.3 | 100.00% | pass | pass |
| markdown-to-pptx | python-pptx 1.0.2 | 182.84 | 787.5 | 42.4 | 195.3 | 100.00% | pass | pass |
| pptx-to-markdown | ainiux Ainiux v1.36 | 56.56 | 2544.1 | 22.2 | 84.6 | 100.00% | pass | pass |
| pptx-to-markdown | python-pptx 1.0.2 | 324.23 | 442.2 | 70.3 | 74.7 | 97.30% | pass | pass |

## Scope and interpretation

The corpus is the three supplied 143-, 220-, and 124-slide decks. PPTX→Markdown compares Ainiux with python-pptx 1.0.2. Markdown→PPTX compares Ainiux with python-pptx 1.0.2, PptxGenJS 4.0.1, and LibreOffice when listed above. Competitor adapters intentionally target canonical slide text and do not claim Ainiux's formatting, notes, relationship, or media fidelity. LibreOffice is measured with a dependency-free Markdown→FODP adapter plus headless FODP→PPTX conversion.

Text recall is a case-folded word multiset score against Ainiux's canonical extraction of each source deck; it is a semantic smoke metric, not a rendering score. Media counts are recorded in raw results. Every generated package is read back by Ainiux and checked by `pptxlint.py`.

PptxGenJS 4.0.1 packages retained the expected slide text, but its structural check failed because it emitted content-type overrides for missing extra slide-master parts.

Within this documented corpus and these exact adapters, Ainiux had the lowest measured median fresh-process time in both directions. This is not an unrestricted fastest-in-the-world claim.

## Interoperability

LibreOffice 24.2.7.2 opened and exported all three representative Ainiux-generated corpus packages (143, 220, and 124 slides) to PDF in headless mode. Microsoft PowerPoint interoperability was not tested.
