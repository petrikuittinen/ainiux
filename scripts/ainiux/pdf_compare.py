#!/usr/bin/env python3
"""Compare Ainiux PDF-to-Markdown quality against pdftotext and optional Python extractors.

This is a tuning tool, not a product dependency and not part of `make test`.
It scores content coverage (is the text there?) and spacing artifacts
(split words, double spaces, space-before-punctuation).

Usage:
  python3 scripts/ainiux/pdf_compare.py tests/pdf_files/*.pdf
  python3 scripts/ainiux/pdf_compare.py /path/to/file.pdf --json
  AINIUX=./ainiux python3 scripts/ainiux/pdf_compare.py tests/pdf_files/DeepSeek2501.12948v1.pdf
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unicodedata
from collections import Counter
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple


TOKEN_RE = re.compile(r"\w+", re.UNICODE)
DOUBLE_SPACE_RE = re.compile(r"  +")
SPACE_BEFORE_PUNCT_RE = re.compile(r" +[.,;:!?)]")
CJK_RE = re.compile(r"[\u4e00-\u9fff]")
ARABIC_RE = re.compile(r"[\u0600-\u06ff]")
CYRILLIC_RE = re.compile(r"[\u0400-\u04ff]")


def find_ainiux() -> Optional[str]:
    env = os.environ.get("AINIUX")
    if env:
        path = Path(env).expanduser()
        if path.exists():
            return str(path.resolve())
    here = Path.cwd()
    for name in ("ainiux", "ainiux.exe"):
        candidate = here / name
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate.resolve())
    return shutil.which("ainiux")


def nfkc(text: str) -> str:
    text = unicodedata.normalize("NFKC", text)
    text = text.replace("\u00a0", " ").replace("\u2423", " ")
    return text


def letters(text: str) -> Counter:
    return Counter(ch.casefold() for ch in nfkc(text) if ch.isalnum())


def letter_recall(reference: str, hypothesis: str) -> float:
    ref = letters(reference)
    hyp = letters(hypothesis)
    total = sum(ref.values())
    if total == 0:
        return 1.0
    matched = sum(min(count, hyp[ch]) for ch, count in ref.items())
    return matched / total


def tokens(text: str) -> List[str]:
    return TOKEN_RE.findall(nfkc(text))


def token_scores(reference: str, hypothesis: str) -> Tuple[float, float]:
    ref = set(tokens(reference))
    hyp = set(tokens(hypothesis))
    if not ref:
        return 1.0, 1.0 if not hyp else 0.0
    recall = len(ref & hyp) / len(ref)
    precision = (len(ref & hyp) / len(hyp)) if hyp else 0.0
    return recall, precision


def split_word_count(reference: str, hypothesis: str) -> int:
    ref_tokens = set(tokens(reference))
    hyp_tokens = tokens(hypothesis)
    count = 0
    for left, right in zip(hyp_tokens, hyp_tokens[1:]):
        joined = left + right
        if joined in ref_tokens and left not in ref_tokens:
            count += 1
    return count


def blank_line_ratio(text: str) -> float:
    lines = text.splitlines()
    if not lines:
        return 0.0
    blanks = sum(1 for line in lines if not line.strip())
    return blanks / len(lines)


def script_counts(text: str) -> Dict[str, int]:
    folded = nfkc(text)
    return {
        "cjk": len(CJK_RE.findall(folded)),
        "arabic": len(ARABIC_RE.findall(folded)),
        "cyrillic": len(CYRILLIC_RE.findall(folded)),
    }


def raw_space_metrics(text: str) -> Dict[str, int]:
    return {
        "double_spaces": len(DOUBLE_SPACE_RE.findall(text)),
        "space_before_punct": len(SPACE_BEFORE_PUNCT_RE.findall(text)),
    }


def run_ainiux(binary: str, pdf: Path, output_format: str) -> Tuple[Optional[str], Optional[str]]:
    max_bytes = max(10 * 1024 * 1024, pdf.stat().st_size + 4096)
    with tempfile.TemporaryDirectory(prefix="ainiux-pdf-compare-") as tmp:
        out = Path(tmp) / ("out.md" if output_format == "md" else "out.txt")
        cmd = [
            binary,
            "--input",
            str(pdf),
            "--output-format",
            output_format,
            "--output",
            str(out),
            "--max-input-bytes",
            str(max_bytes),
        ]
        try:
            proc = subprocess.run(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
        except OSError as exc:
            return None, f"failed to launch ainiux: {exc}"
        if proc.returncode != 0:
            err = proc.stderr.decode("utf-8", errors="replace").strip()
            if not err:
                err = proc.stdout.decode("utf-8", errors="replace").strip()
            return None, f"ainiux exit {proc.returncode}: {err or 'no output'}"
        try:
            return out.read_text(encoding="utf-8", errors="replace"), None
        except OSError as exc:
            return None, f"ainiux wrote no output: {exc}"


def run_pdftotext(pdf: Path) -> Tuple[Optional[str], Optional[str]]:
    binary = shutil.which("pdftotext")
    if binary is None:
        return None, "pdftotext not found"
    with tempfile.TemporaryDirectory(prefix="ainiux-pdftotext-") as tmp:
        out = Path(tmp) / "out.txt"
        cmd = [binary, "-enc", "UTF-8", "-nopgbrk", str(pdf), str(out)]
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        if proc.returncode != 0:
            err = proc.stderr.decode("utf-8", errors="replace").strip()
            return None, f"pdftotext exit {proc.returncode}: {err or 'no output'}"
        try:
            return out.read_text(encoding="utf-8", errors="replace"), None
        except OSError as exc:
            return None, f"pdftotext wrote no output: {exc}"


def run_pdfplumber(pdf: Path) -> Tuple[Optional[str], Optional[str]]:
    try:
        import pdfplumber  # type: ignore
    except ImportError:
        return None, "pdfplumber not installed"
    try:
        with pdfplumber.open(str(pdf)) as doc:
            pages = [page.extract_text() or "" for page in doc.pages]
        return "\n".join(pages), None
    except Exception as exc:  # noqa: BLE001 - comparison tool, report any backend error
        return None, f"pdfplumber failed: {exc}"


def run_pypdf(pdf: Path) -> Tuple[Optional[str], Optional[str]]:
    try:
        from pypdf import PdfReader  # type: ignore
    except ImportError:
        return None, "pypdf not installed"
    try:
        reader = PdfReader(str(pdf))
        pages = [page.extract_text() or "" for page in reader.pages]
        return "\n".join(pages), None
    except Exception as exc:  # noqa: BLE001 - comparison tool, report any backend error
        return None, f"pypdf failed: {exc}"


def compare_against(name: str, hypothesis: str, reference: str) -> Dict[str, object]:
    token_recall, token_precision = token_scores(reference, hypothesis)
    scripts_h = script_counts(hypothesis)
    scripts_r = script_counts(reference)
    spaces = raw_space_metrics(hypothesis)
    return {
        "backend": name,
        "letter_recall": round(letter_recall(reference, hypothesis), 4),
        "token_recall": round(token_recall, 4),
        "token_precision": round(token_precision, 4),
        "split_words": split_word_count(reference, hypothesis),
        "double_spaces": spaces["double_spaces"],
        "space_before_punct": spaces["space_before_punct"],
        "blank_line_ratio": round(blank_line_ratio(hypothesis), 4),
        "cjk": scripts_h["cjk"],
        "cjk_ref": scripts_r["cjk"],
        "arabic": scripts_h["arabic"],
        "arabic_ref": scripts_r["arabic"],
        "cyrillic": scripts_h["cyrillic"],
        "cyrillic_ref": scripts_r["cyrillic"],
        "chars": len(hypothesis),
        "ref_chars": len(reference),
    }


def compare_file(pdf: Path, ainiux: str, include_python: bool) -> Dict[str, object]:
    result: Dict[str, object] = {"file": str(pdf), "errors": []}
    md, err = run_ainiux(ainiux, pdf, "md")
    if err:
        result["errors"].append(err)  # type: ignore[union-attr]
        return result
    plain, plain_err = run_ainiux(ainiux, pdf, "plaintext")
    if plain_err:
        result["errors"].append(plain_err)  # type: ignore[union-attr]
        plain = md
    result["ainiux_md_chars"] = len(md or "")
    result["ainiux_text_chars"] = len(plain or "")

    backends: List[Tuple[str, Optional[str], Optional[str]]] = [
        ("pdftotext", *run_pdftotext(pdf)),
    ]
    if include_python:
        backends.append(("pdfplumber", *run_pdfplumber(pdf)))
        backends.append(("pypdf", *run_pypdf(pdf)))

    comparisons = []
    for name, text, backend_err in backends:
        if backend_err:
            if not backend_err.endswith("not installed") and not backend_err.endswith("not found"):
                result["errors"].append(f"{name}: {backend_err}")  # type: ignore[union-attr]
            else:
                result.setdefault("skipped", []).append(backend_err)  # type: ignore[arg-type]
            continue
        comparisons.append(compare_against(name, md or "", text or ""))
    result["comparisons"] = comparisons
    result["ainiux_spaces"] = raw_space_metrics(md or "")
    return result


def print_table(rows: Sequence[Dict[str, object]]) -> None:
    headers = [
        "file",
        "vs",
        "letter_recall",
        "token_recall",
        "split_words",
        "double_spaces",
        "space_punct",
        "blank_ratio",
        "cjk",
        "arabic",
        "cyrillic",
    ]
    printed = [headers]
    for row in rows:
        pdf = Path(str(row.get("file", ""))).name
        errors = row.get("errors") or []
        if errors and not row.get("comparisons"):
            printed.append([pdf, "ERROR", str(errors[0])[:60], "", "", "", "", "", "", "", ""])
            continue
        for cmp in row.get("comparisons") or []:
            printed.append(
                [
                    pdf,
                    str(cmp["backend"]),
                    f"{cmp['letter_recall']:.3f}",
                    f"{cmp['token_recall']:.3f}",
                    str(cmp["split_words"]),
                    str(cmp["double_spaces"]),
                    str(cmp["space_before_punct"]),
                    f"{cmp['blank_line_ratio']:.2f}",
                    f"{cmp['cjk']}/{cmp['cjk_ref']}",
                    f"{cmp['arabic']}/{cmp['arabic_ref']}",
                    f"{cmp['cyrillic']}/{cmp['cyrillic_ref']}",
                ]
            )
        if not row.get("comparisons"):
            spaces = row.get("ainiux_spaces") or {}
            printed.append(
                [
                    pdf,
                    "(no ref)",
                    "",
                    "",
                    "",
                    str(spaces.get("double_spaces", "")),
                    str(spaces.get("space_before_punct", "")),
                    "",
                    "",
                    "",
                    "",
                ]
            )
    widths = [max(len(str(item[i])) for item in printed) for i in range(len(headers))]
    for i, line in enumerate(printed):
        cells = [str(line[j]).ljust(widths[j]) for j in range(len(headers))]
        print("  ".join(cells))
        if i == 0:
            print("  ".join("-" * widths[j] for j in range(len(headers))))
    skipped = []
    for row in rows:
        skipped.extend(row.get("skipped") or [])  # type: ignore[arg-type]
    unique_skips = sorted(set(str(item) for item in skipped))
    if unique_skips:
        print()
        for item in unique_skips:
            print(f"skipped: {item}")
    errors = []
    for row in rows:
        for item in row.get("errors") or []:  # type: ignore[union-attr]
            errors.append(f"{Path(str(row.get('file', ''))).name}: {item}")
    if errors:
        print()
        for item in errors:
            print(f"error: {item}", file=sys.stderr)


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pdfs", nargs="+", type=Path, help="PDF files to compare")
    parser.add_argument("--json", action="store_true", help="print JSON instead of a table")
    parser.add_argument(
        "--python-backends",
        action="store_true",
        help="also compare against pdfplumber and pypdf when they are importable",
    )
    args = parser.parse_args(argv)

    ainiux = find_ainiux()
    if ainiux is None:
        print("ainiux binary not found (set AINIUX or run from the build directory)", file=sys.stderr)
        return 2

    rows = []
    for pdf in args.pdfs:
        if not pdf.is_file():
            rows.append({"file": str(pdf), "errors": [f"not a file: {pdf}"], "comparisons": []})
            continue
        rows.append(compare_file(pdf, ainiux, args.python_backends))

    if args.json:
        json.dump(rows, sys.stdout, indent=2, ensure_ascii=False)
        sys.stdout.write("\n")
    else:
        print_table(rows)
    return 0


if __name__ == "__main__":
    sys.exit(main())
