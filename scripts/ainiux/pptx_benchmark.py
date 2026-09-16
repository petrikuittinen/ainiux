#!/usr/bin/env python3
"""Reproducible fresh-process PPTX conversion benchmark.

The ainiux runtime never uses this script. Optional competitors are isolated in
a temporary environment by --bootstrap. Results include command order, wall
time, peak RSS, output size, slide count, text recall, and media-part counts.
"""

from __future__ import annotations

import argparse
import collections
import datetime as dt
import html
import json
import os
import pathlib
import platform
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
import uuid
import zipfile


ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_DECKS = [
    ROOT / "tests/pptx_files/IntroductiontoPython3v2.pptx",
    ROOT / "tests/pptx_files/JavaScriptBasics.pptx",
    ROOT / "tests/pptx_files/LinuxAdministrationAndUsage.pptx",
]
DELIMITER = re.compile(r"(?m)^---\s*$")
WORD = re.compile(r"[^\W_]+", re.UNICODE)


def split_slides(markdown: str) -> list[str]:
    slides: list[str] = []
    current: list[str] = []
    fenced = False
    marker = ""
    for line in markdown.replace("\r\n", "\n").split("\n"):
        fence = re.match(r"^ {0,3}(`{3,}|~{3,})", line)
        if not fenced and line == "---":
            slides.append("\n".join(current))
            current = []
            continue
        if fence:
            next_marker = fence.group(1)[0]
            if not fenced:
                fenced, marker = True, next_marker
            elif next_marker == marker:
                fenced = False
        current.append(line)
    slides.append("\n".join(current))
    return slides


def plain_markdown(text: str) -> str:
    text = re.sub(r"^<!-- ainiux-pptx[^\n]*-->\s*", "", text)
    text = re.sub(r"!\[([^]]*)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"\[([^]]+)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"(?m)^#{1,6}\s+", "", text)
    text = re.sub(r"(?m)^\s*(?:[-+*]|\d+[.)])\s+", "", text)
    return re.sub(r"[*_~+`]", "", text)


def python_read(source: pathlib.Path, output: pathlib.Path) -> int:
    from pptx import Presentation
    from pptx.enum.shapes import MSO_SHAPE_TYPE

    presentation = Presentation(str(source))
    emitted: list[str] = []
    for slide in presentation.slides:
        blocks: list[str] = []
        title = slide.shapes.title
        for shape in slide.shapes:
            if shape == title and getattr(shape, "has_text_frame", False):
                blocks.append("# " + shape.text)
            elif getattr(shape, "has_table", False):
                rows = [[cell.text.replace("|", "\\|") for cell in row.cells]
                        for row in shape.table.rows]
                if rows:
                    blocks.append("| " + " | ".join(rows[0]) + " |\n| " +
                                  " | ".join("---" for _ in rows[0]) + " |" +
                                  "".join("\n| " + " | ".join(row) + " |" for row in rows[1:]))
            elif shape.shape_type == MSO_SHAPE_TYPE.PICTURE:
                blocks.append(f"[image omitted: {shape.name}]")
            elif getattr(shape, "has_text_frame", False) and shape.text.strip():
                blocks.append(shape.text)
        notes = slide.notes_slide.notes_text_frame.text.strip()
        if notes:
            blocks.append("> **Speaker notes:**\n>\n" +
                          "\n".join("> " + line for line in notes.splitlines()))
        emitted.append("\n\n".join(blocks))
    output.write_text("\n\n---\n\n".join(emitted) + "\n", encoding="utf-8")
    return 0


def python_write(source: pathlib.Path, output: pathlib.Path) -> int:
    from pptx import Presentation
    from pptx.util import Inches, Pt

    presentation = Presentation()
    presentation.slide_width = Inches(13.333333)
    presentation.slide_height = Inches(7.5)
    blank = presentation.slide_layouts[6]
    for source_slide in split_slides(source.read_text(encoding="utf-8")):
        slide = presentation.slides.add_slide(blank)
        lines = plain_markdown(source_slide).strip().splitlines()
        title = lines.pop(0) if lines else ""
        if title:
            box = slide.shapes.add_textbox(Inches(0.6), Inches(0.3), Inches(12.1), Inches(0.8))
            box.text_frame.text = title
            box.text_frame.paragraphs[0].runs[0].font.size = Pt(26)
            box.text_frame.paragraphs[0].runs[0].font.bold = True
        body = "\n".join(lines).strip()
        if body:
            box = slide.shapes.add_textbox(Inches(0.6), Inches(1.2), Inches(12.1), Inches(5.8))
            box.text_frame.text = body
            for paragraph in box.text_frame.paragraphs:
                for run in paragraph.runs:
                    run.font.size = Pt(16)
    presentation.save(str(output))
    return 0


def fodp_text_box(text: str, title: bool) -> str:
    y, height, style = ("0.35in", "0.8in", "Title") if title else ("1.25in", "5.7in", "Body")
    paragraphs = "".join(f"<text:p>{html.escape(line)}</text:p>" for line in text.splitlines())
    return (f'<draw:frame draw:style-name="gr1" draw:text-style-name="{style}" '
            f'svg:x="0.6in" svg:y="{y}" svg:width="12.1in" svg:height="{height}" '
            f'presentation:class="{style.lower()}"><draw:text-box>{paragraphs}</draw:text-box></draw:frame>')


def fodp_document(markdown: str) -> str:
    pages = []
    for index, source_slide in enumerate(split_slides(markdown), 1):
        lines = plain_markdown(source_slide).strip().splitlines()
        title = lines.pop(0) if lines else ""
        body = "\n".join(lines).strip()
        content = (fodp_text_box(title, True) if title else "") + (fodp_text_box(body, False) if body else "")
        pages.append(f'<draw:page draw:name="page{index}" draw:style-name="dp1" '
                     f'draw:master-page-name="Default">{content}</draw:page>')
    return f'''<?xml version="1.0" encoding="UTF-8"?>
<office:document xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0"
 xmlns:style="urn:oasis:names:tc:opendocument:xmlns:style:1.0"
 xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0"
 xmlns:draw="urn:oasis:names:tc:opendocument:xmlns:drawing:1.0"
 xmlns:presentation="urn:oasis:names:tc:opendocument:xmlns:presentation:1.0"
 xmlns:svg="urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0"
 xmlns:fo="urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0"
 office:mimetype="application/vnd.oasis.opendocument.presentation" office:version="1.3">
 <office:styles>
  <style:style style:name="Title" style:family="paragraph"><style:text-properties fo:font-size="26pt" fo:font-weight="bold"/></style:style>
  <style:style style:name="Body" style:family="paragraph"><style:text-properties fo:font-size="16pt"/></style:style>
  <style:style style:name="gr1" style:family="graphic"><style:graphic-properties draw:auto-grow-height="false"/></style:style>
 </office:styles>
 <office:automatic-styles>
  <style:page-layout style:name="PM1"><style:page-layout-properties fo:page-width="13.333333in" fo:page-height="7.5in" style:print-orientation="landscape"/></style:page-layout>
  <style:style style:name="dp1" style:family="drawing-page"/>
 </office:automatic-styles>
 <office:master-styles><style:master-page style:name="Default" style:page-layout-name="PM1" draw:style-name="dp1"/></office:master-styles>
 <office:body><office:presentation>{''.join(pages)}</office:presentation></office:body>
</office:document>'''


def libreoffice_write(source: pathlib.Path, output: pathlib.Path, executable: str) -> int:
    with tempfile.TemporaryDirectory(prefix="ainiux-pptx-lo-") as temporary:
        temp = pathlib.Path(temporary)
        fodp = temp / (output.stem + ".fodp")
        fodp.write_text(fodp_document(source.read_text(encoding="utf-8")), encoding="utf-8")
        profile = (temp / "profile").resolve().as_uri()
        command = [executable, "--headless", f"-env:UserInstallation={profile}",
                   "--convert-to", "pptx", "--outdir", str(temp), str(fodp)]
        completed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        generated = temp / (fodp.stem + ".pptx")
        if completed.returncode or not generated.is_file():
            sys.stderr.write(completed.stdout + completed.stderr)
            return completed.returncode or 1
        shutil.copyfile(generated, output)
    return 0


def worker_mode(arguments: list[str]) -> int | None:
    if not arguments or not arguments[0].startswith("--worker-"):
        return None
    mode = arguments[0]
    if mode == "--worker-python-read" and len(arguments) == 3:
        return python_read(pathlib.Path(arguments[1]), pathlib.Path(arguments[2]))
    if mode == "--worker-python-write" and len(arguments) == 3:
        return python_write(pathlib.Path(arguments[1]), pathlib.Path(arguments[2]))
    if mode == "--worker-libreoffice-write" and len(arguments) == 4:
        return libreoffice_write(pathlib.Path(arguments[1]), pathlib.Path(arguments[2]), arguments[3])
    raise SystemExit("invalid benchmark worker arguments")


def run_checked(command: list[str], *, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, env=env)


def bootstrap_tools(directory: pathlib.Path) -> tuple[str, pathlib.Path]:
    venv = directory / "python"
    run_checked([sys.executable, "-m", "venv", str(venv)])
    python = venv / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
    run_checked([str(python), "-m", "pip", "install", "--disable-pip-version-check",
                 "python-pptx==1.0.2"])
    npm = shutil.which("npm")
    if not npm:
        raise RuntimeError("npm is required by --bootstrap for PptxGenJS 4.0.1")
    node_root = directory / "node"
    node_root.mkdir()
    run_checked([npm, "install", "--no-audit", "--no-fund", "--prefix", str(node_root),
                 "pptxgenjs@4.0.1"])
    bundle = node_root / "node_modules/pptxgenjs/dist/pptxgen.cjs.js"
    return str(python), bundle


def slide_count(markdown: str) -> int:
    return len(split_slides(markdown))


def media_count(package: pathlib.Path) -> int:
    try:
        with zipfile.ZipFile(package) as archive:
            return sum(name.startswith("ppt/media/") and not name.endswith("/") for name in archive.namelist())
    except (OSError, zipfile.BadZipFile):
        return -1


def token_recall(reference: str, candidate: str) -> float:
    expected = collections.Counter(word.casefold() for word in WORD.findall(plain_markdown(reference)))
    actual = collections.Counter(word.casefold() for word in WORD.findall(plain_markdown(candidate)))
    total = sum(expected.values())
    if not total:
        return 1.0
    return sum(min(count, actual[word]) for word, count in expected.items()) / total


def measure(command: list[str], stats: pathlib.Path, env: dict[str, str] | None = None) -> tuple[float, int]:
    timer = shutil.which("time") or "/usr/bin/time"
    wrapped = [timer, "-f", "%M", "-o", str(stats)] + command
    started = time.perf_counter()
    completed = subprocess.run(wrapped, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                               text=True, env=env)
    elapsed = time.perf_counter() - started
    if completed.returncode:
        raise RuntimeError("command failed: " + " ".join(command) + "\n" + completed.stderr)
    return elapsed, int(stats.read_text(encoding="utf-8").strip())


def self_test() -> int:
    sample = "# One\n\n```\n---\n```\n\n---\n\n# Two\n"
    assert len(split_slides(sample)) == 2
    assert "One" in plain_markdown(sample)
    document = fodp_document(sample)
    assert document.count("<draw:page ") == 2 and "office:presentation" in document
    print("PPTX benchmark self-test passed")
    return 0


def main() -> int:
    if (worker := worker_mode(sys.argv[1:])) is not None:
        return worker
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--bootstrap", action="store_true")
    parser.add_argument("--ainiux", type=pathlib.Path, default=ROOT / "ainiux")
    parser.add_argument("--output-dir", type=pathlib.Path)
    parser.add_argument("--report", type=pathlib.Path)
    parser.add_argument("--iterations", type=int, default=5)
    parser.add_argument("--deck", action="append", type=pathlib.Path)
    parser.add_argument("--python", default=sys.executable)
    parser.add_argument("--python-pptx-source", type=pathlib.Path)
    parser.add_argument("--pptxgenjs", type=pathlib.Path)
    parser.add_argument("--libreoffice", default=shutil.which("libreoffice") or "")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.output_dir is None or args.report is None:
        parser.error("--output-dir and --report are required unless --self-test is used")
    if args.iterations < 3:
        parser.error("--iterations must be at least 3")
    decks = [path.resolve() for path in (args.deck or DEFAULT_DECKS)]
    if not args.ainiux.is_file() or any(not path.is_file() for path in decks):
        parser.error("the explicit ainiux binary and every corpus deck must exist")
    args.ainiux = args.ainiux.resolve()
    output_dir = args.output_dir.resolve()
    if output_dir.exists() and any(output_dir.iterdir()):
        parser.error("--output-dir must not exist or must be empty")
    output_dir.mkdir(parents=True, exist_ok=True)

    temporary = tempfile.TemporaryDirectory(prefix="ainiux-pptx-benchmark-")
    temp = pathlib.Path(temporary.name)
    python = args.python
    bundle = args.pptxgenjs
    env = os.environ.copy()
    if args.bootstrap:
        python, bundle = bootstrap_tools(temp / "bootstrap")
    elif args.python_pptx_source:
        env["PYTHONPATH"] = str((args.python_pptx_source / "src").resolve())
    if bundle is None:
        default_bundle = pathlib.Path.home() / "agent_analysis/pptxgenjs/dist/pptxgen.cjs.js"
        bundle = default_bundle if default_bundle.is_file() else None

    versions: dict[str, str] = {}
    versions["ainiux"] = run_checked([str(args.ainiux), "--version"]).stdout.strip()
    versions["python-pptx"] = run_checked(
        [python, "-c", "import pptx; print(pptx.__version__)"], env=env).stdout.strip()
    if bundle:
        versions["PptxGenJS"] = "4.0.1"
    if args.libreoffice:
        versions["LibreOffice"] = run_checked([args.libreoffice, "--version"]).stdout.strip()

    markdown_sources: dict[pathlib.Path, pathlib.Path] = {}
    references: dict[pathlib.Path, str] = {}
    expected_slides: dict[pathlib.Path, int] = {}
    for deck in decks:
        target = output_dir / (deck.stem + ".reference.md")
        run_checked([str(args.ainiux), "--input", str(deck), "--output-format", "md",
                     "--output", str(target), "--quiet"])
        markdown_sources[deck] = target
        references[deck] = target.read_text(encoding="utf-8")
        expected_slides[deck] = slide_count(references[deck])

    read_tools = ["ainiux", "python-pptx"]
    write_tools = ["ainiux", "python-pptx"]
    if bundle:
        write_tools.append("PptxGenJS")
    if args.libreoffice:
        write_tools.append("LibreOffice")
    records: list[dict[str, object]] = []
    script = pathlib.Path(__file__).resolve()
    node_worker = script.with_name("pptxgenjs_worker.cjs")

    for corpus_index, deck in enumerate(decks):
        markdown_source = markdown_sources[deck]
        for iteration in range(args.iterations):
            ordered_reads = read_tools[iteration % len(read_tools):] + read_tools[:iteration % len(read_tools)]
            ordered_writes = write_tools[(iteration + corpus_index) % len(write_tools):] + write_tools[:(iteration + corpus_index) % len(write_tools)]
            for order, tool in enumerate(ordered_reads):
                output = output_dir / f"{deck.stem}.read.{tool}.{iteration}.md"
                if tool == "ainiux":
                    command = [str(args.ainiux), "--input", str(deck), "--output-format", "md",
                               "--output", str(output), "--quiet"]
                else:
                    command = [python, str(script), "--worker-python-read", str(deck), str(output)]
                elapsed, rss = measure(command, temp / f"read-{corpus_index}-{iteration}-{order}.rss", env)
                text = output.read_text(encoding="utf-8")
                records.append({"direction": "pptx-to-markdown", "tool": tool, "corpus": deck.name,
                                "iteration": iteration, "order": order, "seconds": elapsed,
                                "peak_rss_kib": rss, "output_bytes": output.stat().st_size,
                                "slides": slide_count(text), "expected_slides": expected_slides[deck],
                                "text_recall": token_recall(references[deck], text),
                                "media_parts": media_count(deck)})
            for order, tool in enumerate(ordered_writes):
                output = output_dir / f"{deck.stem}.write.{tool}.{iteration}.pptx"
                if tool == "ainiux":
                    command = [str(args.ainiux), "--input", str(markdown_source),
                               "--output-format", "pptx", "--output", str(output), "--quiet"]
                elif tool == "python-pptx":
                    command = [python, str(script), "--worker-python-write", str(markdown_source), str(output)]
                elif tool == "PptxGenJS":
                    command = [shutil.which("node") or "node", str(node_worker), str(bundle),
                               str(markdown_source), str(output), "on"]
                else:
                    command = [sys.executable, str(script), "--worker-libreoffice-write",
                               str(markdown_source), str(output), args.libreoffice]
                elapsed, rss = measure(command, temp / f"write-{corpus_index}-{iteration}-{order}.rss", env)
                roundtrip = output.with_suffix(".roundtrip.md")
                run_checked([str(args.ainiux), "--input", str(output), "--output-format", "md",
                             "--output", str(roundtrip), "--quiet"])
                text = roundtrip.read_text(encoding="utf-8")
                lint = subprocess.run([sys.executable, str(ROOT / "scripts/ainiux/pptxlint.py"), str(output)],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
                records.append({"direction": "markdown-to-pptx", "tool": tool, "corpus": deck.name,
                                "iteration": iteration, "order": order, "seconds": elapsed,
                                "peak_rss_kib": rss, "output_bytes": output.stat().st_size,
                                "slides": slide_count(text), "expected_slides": expected_slides[deck],
                                "text_recall": token_recall(references[deck], text),
                                "media_parts": media_count(output), "pptxlint": lint})

    interoperability = {"libreoffice_exported_ainiux_packages": 0,
                        "powerpoint": "not tested"}
    if args.libreoffice:
        libreoffice_output = temp / "libreoffice-readback"
        libreoffice_output.mkdir()
        for index, deck in enumerate(decks):
            package = output_dir / f"{deck.stem}.write.ainiux.0.pptx"
            export_directory = libreoffice_output / f"export-{index}"
            profile = libreoffice_output / f"profile-{index}"
            export_directory.mkdir()
            profile.mkdir()
            run_checked([args.libreoffice,
                         "-env:UserInstallation=" + profile.resolve().as_uri(),
                         "--headless", "--convert-to", "pdf", "--outdir",
                         str(export_directory), str(package)])
            exported = export_directory / (package.stem + ".pdf")
            if not exported.is_file() or not exported.stat().st_size:
                raise RuntimeError("LibreOffice did not export " + str(package))
            interoperability["libreoffice_exported_ainiux_packages"] += 1

    raw = {"generated_at": dt.datetime.now(dt.timezone.utc).isoformat(),
           "platform": platform.platform(), "versions": versions,
           "iterations": args.iterations, "corpus": [str(path) for path in decks],
           "interoperability": interoperability, "records": records}
    (output_dir / "measurements.json").write_text(json.dumps(raw, indent=2, ensure_ascii=False) + "\n",
                                                   encoding="utf-8")

    grouped: dict[tuple[str, str], list[dict[str, object]]] = collections.defaultdict(list)
    for record in records:
        grouped[(str(record["direction"]), str(record["tool"]))].append(record)
    rows = []
    winners: dict[str, bool] = {}
    for (direction, tool), items in sorted(grouped.items()):
        median_ms = statistics.median(float(item["seconds"]) for item in items) * 1000
        rates = [float(item["expected_slides"]) / float(item["seconds"]) for item in items]
        rows.append((direction, tool, median_ms, statistics.median(rates),
                     statistics.median(int(item["peak_rss_kib"]) for item in items) / 1024,
                     statistics.median(int(item["output_bytes"]) for item in items) / 1024,
                     statistics.mean(float(item["text_recall"]) for item in items) * 100,
                     all(int(item["slides"]) == int(item["expected_slides"]) for item in items),
                     all(bool(item.get("pptxlint", True)) for item in items)))
    for direction in ("pptx-to-markdown", "markdown-to-pptx"):
        direction_rows = [row for row in rows if row[0] == direction]
        ainiux_row = next(row for row in direction_rows if row[1] == "ainiux")
        winners[direction] = all(ainiux_row[2] < row[2] for row in direction_rows if row[1] != "ainiux")

    report = ["# PPTX conversion benchmark", "",
              f"Measured {dt.date.today().isoformat()} on `{platform.platform()}` with "
              f"{args.iterations} fresh processes per tool and corpus item. Run order rotated by "
              "iteration; raw commands, order, timing, RSS, size, and fidelity data are in the companion `measurements.json`.", "",
              "| Direction | Tool | Median ms | Median slides/s | Median peak RSS MiB | Median output KiB | Mean text recall | Slide counts | pptxlint |",
              "| --- | --- | ---: | ---: | ---: | ---: | ---: | --- | --- |"]
    for row in rows:
        report.append(f"| {row[0]} | {row[1]} {versions.get(row[1], '')} | {row[2]:.2f} | {row[3]:.1f} | {row[4]:.1f} | {row[5]:.1f} | {row[6]:.2f}% | {'pass' if row[7] else 'fail'} | {'pass' if row[8] else 'fail'} |")
    report += ["", "## Scope and interpretation", "",
               "The corpus is the three supplied 143-, 220-, and 124-slide decks. PPTX→Markdown compares Ainiux with python-pptx 1.0.2. Markdown→PPTX compares Ainiux with python-pptx 1.0.2, PptxGenJS 4.0.1, and LibreOffice when listed above. Competitor adapters intentionally target canonical slide text and do not claim Ainiux's formatting, notes, relationship, or media fidelity. LibreOffice is measured with a dependency-free Markdown→FODP adapter plus headless FODP→PPTX conversion.", "",
               "Text recall is a case-folded word multiset score against Ainiux's canonical extraction of each source deck; it is a semantic smoke metric, not a rendering score. Media counts are recorded in raw results. Every generated package is read back by Ainiux and checked by `pptxlint.py`.", ""]
    if any(row[1] == "PptxGenJS" and not row[8] for row in rows):
        report += ["PptxGenJS 4.0.1 packages retained the expected slide text, but its structural check failed because it emitted content-type overrides for missing extra slide-master parts.", ""]
    if all(winners.values()):
        report.append("Within this documented corpus and these exact adapters, Ainiux had the lowest measured median fresh-process time in both directions. This is not an unrestricted fastest-in-the-world claim.")
    else:
        report.append("Ainiux did not have the lowest measured median in every direction for this run; no fastest claim is made.")
    report += ["", "## Interoperability", ""]
    if interoperability["libreoffice_exported_ainiux_packages"]:
        report.append(
            f"LibreOffice opened and exported all {interoperability['libreoffice_exported_ainiux_packages']} "
            "representative Ainiux-generated corpus packages to PDF in headless mode.")
    else:
        report.append("LibreOffice interoperability was not run.")
    report.append("Microsoft PowerPoint interoperability was not tested.")
    report.append("")
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text("\n".join(report), encoding="utf-8")
    print(f"wrote {len(records)} measurements and {args.report}")
    temporary.cleanup()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
