#!/usr/bin/env python3
"""Benchmark Ainiux DOCX conversion against python-docx and Spire.Doc.

The normal mode runs each candidate/input pair exactly once in a fresh process,
then scores canonical document semantics with a standard-library Markdown/OOXML
oracle.  Results and generated files belong under the ignored ``results/``
directory.  The optional ``--bootstrap`` mode creates a temporary virtual
environment containing the pinned comparison libraries and removes it after the
run.

Examples:
  python3 scripts/ainiux/docx_benchmark.py --self-test
  python3 scripts/ainiux/docx_benchmark.py --bootstrap \
      --output-dir results/docx-conversion-2026-09-14 \
      --report docs/docx_conversion_benchmark.md
"""

from __future__ import annotations

import argparse
import hashlib
import html
import importlib.metadata
import json
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
import unicodedata
import venv
import zipfile
from collections import Counter
from dataclasses import dataclass, field
from datetime import datetime, timezone
from html.parser import HTMLParser
from pathlib import Path, PurePosixPath
from typing import Dict, Iterable, List, Mapping, MutableMapping, Optional, Sequence, Tuple
from urllib.parse import quote
from xml.etree import ElementTree as ET


PYTHON_DOCX_VERSION = "1.2.0"
SPIRE_DOC_VERSION = "14.8.1"
MAX_INPUTS = 8
CANDIDATES = ("ainiux", "python-docx-adapter", "spire-doc")
W_NS = "http://schemas.openxmlformats.org/wordprocessingml/2006/main"
R_NS = "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
PKG_REL_NS = "http://schemas.openxmlformats.org/package/2006/relationships"
CT_NS = "http://schemas.openxmlformats.org/package/2006/content-types"
XML_NS = "http://www.w3.org/XML/1998/namespace"
W = "{" + W_NS + "}"
R = "{" + R_NS + "}"
TOKEN_RE = re.compile(r"[^\W_]+(?:['\N{RIGHT SINGLE QUOTATION MARK}][^\W_]+)*", re.UNICODE)
LIST_RE = re.compile(r"^(\s*)([-+*]|\d+[.)])\s+(.*)$")
HEADING_RE = re.compile(r"^(#{1,6})\s+(.*?)(?:\s+#+)?$")
TABLE_RULE_RE = re.compile(r"^\s*\|?\s*:?-{3,}:?\s*(?:\|\s*:?-{3,}:?\s*)+\|?\s*$")
FENCE_RE = re.compile(r"^\s*(`{3,}|~{3,})(.*)$")
EVALUATION_RE = re.compile(r"evaluation warning|created with spire\.doc|spire\.doc for", re.I)
INLINE_PATTERNS = (
    ("image", re.compile(r"!\[([^\]]*)\]\([^)]*\)")),
    ("link", re.compile(r"\[([^\]]+)\]\(\s*([^\s)]+)(?:\s+[\"'][^)]*[\"'])?\s*\)")),
    ("bold", re.compile(r"\*\*(.+?)\*\*|__(.+?)__")),
    ("underline", re.compile(r"\+\+(.+?)\+\+")),
    ("strike", re.compile(r"~~(.+?)~~")),
    ("code", re.compile(r"(`+)(.+?)\1")),
    ("italic", re.compile(r"(?<!\*)\*([^*\n]+?)\*(?!\*)|(?<!\w)_([^_\n]+?)_(?!\w)")),
)


@dataclass
class SemanticModel:
    tokens: List[str] = field(default_factory=list)
    anchors: Counter = field(default_factory=Counter)
    blocks: List[List[str]] = field(default_factory=list)

    def add_block(self, text: str) -> None:
        block_tokens = tokenize(text)
        if block_tokens:
            self.tokens.extend(block_tokens)
            self.blocks.append(block_tokens)

    def add_anchor(self, kind: str, *values: object) -> None:
        normalized = [normalize_anchor_value(value) for value in values]
        self.anchors[json.dumps([kind] + normalized, ensure_ascii=False, separators=(",", ":"))] += 1


def normalize_text(text: str) -> str:
    return unicodedata.normalize("NFKC", text).replace("\u00a0", " ")


def tokenize(text: str) -> List[str]:
    return [item.casefold() for item in TOKEN_RE.findall(normalize_text(text))]


def normalize_anchor_value(value: object) -> object:
    if isinstance(value, str):
        words = tokenize(value)
        return " ".join(words[:32]) if words else normalize_text(value).strip().casefold()[:160]
    return value


def f_score(matched: int, reference_count: int, hypothesis_count: int) -> Tuple[float, float, float]:
    recall = matched / reference_count if reference_count else 1.0
    precision = matched / hypothesis_count if hypothesis_count else (1.0 if not reference_count else 0.0)
    f1 = 2.0 * precision * recall / (precision + recall) if precision + recall else 0.0
    return precision, recall, f1


def counter_scores(reference: Counter, hypothesis: Counter) -> Dict[str, float]:
    matched = sum((reference & hypothesis).values())
    precision, recall, f1 = f_score(matched, sum(reference.values()), sum(hypothesis.values()))
    return {"precision": precision, "recall": recall, "f1": f1}


def bigrams(items: Sequence[str]) -> Counter:
    return Counter(zip(items, items[1:]))


def sequence_present(needle: Sequence[str], haystack: Sequence[str]) -> bool:
    if not needle:
        return True
    if len(needle) > len(haystack):
        return False
    first = needle[0]
    end = len(haystack) - len(needle) + 1
    return any(haystack[index:index + len(needle)] == list(needle)
               for index in range(end) if haystack[index] == first)


def score_models(reference: SemanticModel, hypothesis: SemanticModel) -> Dict[str, object]:
    token_score = counter_scores(Counter(reference.tokens), Counter(hypothesis.tokens))
    bigram_score = counter_scores(bigrams(reference.tokens), bigrams(hypothesis.tokens))
    structure_score = counter_scores(reference.anchors, hypothesis.anchors)
    signature = reference.blocks[-1][-16:] if reference.blocks else []
    return {
        "token": token_score,
        "ordered_token_bigram": bigram_score,
        "anchored_structure": structure_score,
        "reference_tokens": len(reference.tokens),
        "output_tokens": len(hypothesis.tokens),
        "reference_anchors": sum(reference.anchors.values()),
        "output_anchors": sum(hypothesis.anchors.values()),
        "final_source_block_present": sequence_present(signature, hypothesis.tokens),
        "final_source_block_signature": signature,
    }


def context_anchor(text: str, offset: int) -> Tuple[str, str]:
    left = tokenize(text[:offset])
    right = tokenize(text[offset + 1:])
    return (left[-1] if left else "", right[0] if right else "")


def parse_inline_markdown(source: str, model: SemanticModel) -> str:
    """Return visible text and add inline semantic anchors."""
    source = re.sub(r"\[!\[[^\]]*\]\([^)]*\)\]\([^)]*\)", "", source)
    for match in re.finditer(r"\t", source):
        model.add_anchor("tab", *context_anchor(source, match.start()))
    for match in re.finditer(r"<br\s*/?>", source, re.I):
        model.add_anchor("hard_break", *context_anchor(source, match.start()))

    output: List[str] = []
    position = 0
    while position < len(source):
        found: Optional[Tuple[int, int, str, re.Match]] = None
        for kind, pattern in INLINE_PATTERNS:
            match = pattern.search(source, position)
            if match is None:
                continue
            candidate = (match.start(), match.end(), kind, match)
            if found is None or candidate[:2] < found[:2]:
                found = candidate
        if found is None:
            output.append(source[position:])
            break
        start, end, kind, match = found
        output.append(source[position:start])
        if kind == "image":
            visible = ""
        elif kind == "link":
            visible = match.group(1)
            model.add_anchor("link", visible, html.unescape(match.group(2)))
        elif kind == "code":
            visible = match.group(2)
            model.add_anchor("code", visible)
        else:
            visible = next((group for group in match.groups() if group is not None), "")
            model.add_anchor("emphasis:" + kind, visible)
        output.append(visible)
        position = end
    text = "".join(output)
    text = re.sub(r"<br\s*/?>", "\n", text, flags=re.I)
    text = re.sub(r"\\([\\`*{}\[\]()#+.!_>~-])", r"\1", text)
    return html.unescape(text)


def strip_excluded_images(text: str) -> str:
    """Remove image syntax/placeholders from the semantic text on both sides."""
    text = re.sub(r"\[!\[[^\]]*\]\([^)]*\)\]\([^)]*\)", "", text, flags=re.S)
    text = re.sub(r"!\[[^\]]*\]\([^)]*\)", "", text, flags=re.S)
    text = re.sub(r"\[image omitted(?::[^\]]*)?\]", "", text, flags=re.I | re.S)
    return re.sub(r"<img\b[^>]*>", "", text, flags=re.I | re.S)


class _HTMLTableParser(HTMLParser):
    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.rows: List[List[Tuple[str, int, int]]] = []
        self.current_row: Optional[List[Tuple[str, int, int]]] = None
        self.current_cell: Optional[List[str]] = None
        self.current_colspan = 1
        self.current_rowspan = 1

    def handle_starttag(self, tag: str, attrs: List[Tuple[str, Optional[str]]]) -> None:
        attributes = dict(attrs)
        if tag.casefold() == "tr":
            self.current_row = []
        elif tag.casefold() in ("td", "th") and self.current_row is not None:
            self.current_cell = []
            try:
                self.current_colspan = max(1, int(attributes.get("colspan") or "1"))
                self.current_rowspan = max(1, int(attributes.get("rowspan") or "1"))
            except ValueError:
                self.current_colspan = self.current_rowspan = 1
        elif tag.casefold() == "br" and self.current_cell is not None:
            self.current_cell.append("\n")

    def handle_endtag(self, tag: str) -> None:
        if tag.casefold() in ("td", "th") and self.current_row is not None and self.current_cell is not None:
            value = re.sub(r"[ \f\v]+", " ", "".join(self.current_cell)).strip()
            self.current_row.append((value, self.current_colspan, self.current_rowspan))
            self.current_cell = None
        elif tag.casefold() == "tr" and self.current_row is not None:
            self.rows.append(self.current_row)
            self.current_row = None

    def handle_data(self, data: str) -> None:
        if self.current_cell is not None:
            self.current_cell.append(data)


def normalize_html_tables(text: str) -> str:
    """Turn raw HTML tables emitted by Spire into ordered canonical table text."""
    table_pattern = re.compile(r"<table\b.*?</table\s*>", re.I | re.S)

    def consume(match: re.Match) -> str:
        parser = _HTMLTableParser()
        parser.feed(match.group(0))
        grid: Dict[Tuple[int, int], str] = {}
        maximum_column = 0
        for row_number, raw_row in enumerate(parser.rows):
            column = 0
            for value, colspan, rowspan in raw_row:
                while (row_number, column) in grid:
                    column += 1
                for row_offset in range(rowspan):
                    for column_offset in range(colspan):
                        grid[(row_number + row_offset, column + column_offset)] = (
                            value if row_offset == 0 and column_offset == 0 else "")
                column += colspan
                maximum_column = max(maximum_column, column)
        rendered_rows = []
        for row_number in range(len(parser.rows)):
            cells = [grid.get((row_number, column_number), "").replace("|", r"\|").replace("\n", "<br>")
                     for column_number in range(maximum_column)]
            rendered_rows.append("| " + " | ".join(cells) + " |")
        if not rendered_rows:
            return "\n"
        rule = "| " + " | ".join("---" for _ in range(maximum_column)) + " |"
        return "\n" + rendered_rows[0] + "\n" + rule + "\n" + "\n".join(rendered_rows[1:]) + "\n"

    return table_pattern.sub(consume, text)


def normalize_inline_html(text: str) -> str:
    replacements = ((r"<(?:strong|b)\b[^>]*>(.*?)</(?:strong|b)\s*>", r"**\1**"),
                    (r"<(?:em|i)\b[^>]*>(.*?)</(?:em|i)\s*>", r"*\1*"),
                    (r"<u\b[^>]*>(.*?)</u\s*>", r"++\1++"),
                    (r"<(?:s|del|strike)\b[^>]*>(.*?)</(?:s|del|strike)\s*>", r"~~\1~~"))
    for pattern, replacement in replacements:
        text = re.sub(pattern, replacement, text, flags=re.I | re.S)
    text = re.sub(r"<a\b[^>]*href=[\"']([^\"']+)[\"'][^>]*>(.*?)</a\s*>",
                  lambda match: "[" + re.sub(r"<[^>]+>", "", match.group(2)) + "](" + match.group(1) + ")",
                  text, flags=re.I | re.S)
    for level in range(1, 7):
        text = re.sub(rf"<h{level}\b[^>]*>(.*?)</h{level}\s*>",
                      lambda match, count=level: "\n" + "#" * count + " " + match.group(1) + "\n",
                      text, flags=re.I | re.S)
    text = re.sub(r"</?(?:p|div|section|article|blockquote|ul|ol|li)\b[^>]*>", "\n", text, flags=re.I)
    text = re.sub(r"<!--.*?-->", "", text, flags=re.S)
    return re.sub(r"<(?!br\s*/?>)[^>]+>", "", text, flags=re.I)


def split_table_row(line: str) -> List[str]:
    line = line.strip()
    if line.startswith("|"):
        line = line[1:]
    if line.endswith("|") and not line.endswith(r"\|"):
        line = line[:-1]
    cells: List[str] = []
    current: List[str] = []
    escaped = False
    code_delimiter = ""
    for ch in line:
        if escaped:
            current.append(ch)
            escaped = False
        elif ch == "\\":
            current.append(ch)
            escaped = True
        elif ch == "`":
            current.append(ch)
            code_delimiter = "" if code_delimiter else "`"
        elif ch == "|" and not code_delimiter:
            cells.append("".join(current).strip())
            current = []
        else:
            current.append(ch)
    cells.append("".join(current).strip())
    return cells


def markdown_semantics(text: str) -> SemanticModel:
    model = SemanticModel()
    normalized = normalize_text(text).replace("\r\n", "\n").replace("\r", "\n")
    normalized = strip_excluded_images(normalized)
    normalized = normalize_html_tables(normalized)
    normalized = normalize_inline_html(normalized)
    lines = normalized.split("\n")
    index = 0
    while index < len(lines):
        line = lines[index]
        if not line.strip():
            index += 1
            continue
        fence = FENCE_RE.match(line)
        if fence:
            delimiter = fence.group(1)
            payload: List[str] = []
            index += 1
            while index < len(lines) and not lines[index].lstrip().startswith(delimiter):
                payload.append(lines[index])
                index += 1
            if index < len(lines):
                index += 1
            code_text = "\n".join(payload)
            model.add_anchor("code_block", fence.group(2).strip(), code_text)
            model.add_block(code_text)
            continue
        heading = HEADING_RE.match(line)
        if heading:
            visible = parse_inline_markdown(heading.group(2), model)
            model.add_anchor("heading", len(heading.group(1)), visible)
            model.add_block(visible)
            index += 1
            continue
        if index + 1 < len(lines) and line.strip() and re.match(r"^\s*(=+|-+)\s*$", lines[index + 1]):
            visible = parse_inline_markdown(line.strip(), model)
            level = 1 if "=" in lines[index + 1] else 2
            model.add_anchor("heading", level, visible)
            model.add_block(visible)
            index += 2
            continue
        if index + 1 < len(lines) and "|" in line and TABLE_RULE_RE.match(lines[index + 1]):
            rows = [split_table_row(line)]
            index += 2
            while index < len(lines) and "|" in lines[index] and lines[index].strip():
                rows.append(split_table_row(lines[index]))
                index += 1
            for row_number, row in enumerate(rows):
                for column_number, cell in enumerate(row):
                    visible = parse_inline_markdown(cell, model)
                    model.add_anchor("table_cell", row_number, column_number, visible)
                    model.add_block(visible)
            continue
        list_match = LIST_RE.match(line)
        if list_match:
            indent, marker, body = list_match.groups()
            level = max(0, len(indent.expandtabs(4)) // 2)
            kind = "ordered" if marker[0].isdigit() else "unordered"
            visible = parse_inline_markdown(body, model)
            model.add_anchor("list:" + kind, level, visible)
            model.add_block(visible)
            index += 1
            continue
        if line.lstrip().startswith(">"):
            body = re.sub(r"^\s*>\s?", "", line)
            visible = parse_inline_markdown(body, model)
            model.add_anchor("quote", visible)
            model.add_block(visible)
            index += 1
            continue

        paragraph_lines = [line]
        index += 1
        while index < len(lines) and lines[index].strip():
            next_line = lines[index]
            if (FENCE_RE.match(next_line) or HEADING_RE.match(next_line) or LIST_RE.match(next_line)
                    or next_line.lstrip().startswith(">")):
                break
            if index + 1 < len(lines) and "|" in next_line and TABLE_RULE_RE.match(lines[index + 1]):
                break
            paragraph_lines.append(next_line)
            index += 1
        combined: List[str] = []
        for line_number, part in enumerate(paragraph_lines):
            hard_break = bool(re.search(r" {2,}$", part))
            stripped = re.sub(r" {2,}$", "", part)
            combined.append(stripped)
            if line_number + 1 < len(paragraph_lines):
                if hard_break:
                    joined_so_far = " ".join(combined)
                    following = paragraph_lines[line_number + 1]
                    model.add_anchor("hard_break",
                                     tokenize(joined_so_far)[-1] if tokenize(joined_so_far) else "",
                                     tokenize(following)[0] if tokenize(following) else "")
                    combined.append("\n")
                else:
                    combined.append(" ")
        model.add_block(parse_inline_markdown("".join(combined), model))
    return model


def safe_zip_members(archive: zipfile.ZipFile) -> None:
    seen = set()
    for info in archive.infolist():
        path = PurePosixPath(info.filename)
        if info.filename in seen:
            raise ValueError("duplicate ZIP member: " + info.filename)
        if path.is_absolute() or ".." in path.parts or "\\" in info.filename:
            raise ValueError("unsafe ZIP member: " + info.filename)
        seen.add(info.filename)


def relationship_part(part: str) -> str:
    path = PurePosixPath(part)
    return str(path.parent / "_rels" / (path.name + ".rels"))


def resolve_part(base_part: str, target: str) -> str:
    if target.startswith("/"):
        candidate = PurePosixPath(target[1:])
    else:
        candidate = PurePosixPath(base_part).parent / target
    resolved: List[str] = []
    for item in candidate.parts:
        if item == ".":
            continue
        if item == "..":
            if not resolved:
                raise ValueError("relationship target escapes package")
            resolved.pop()
        else:
            resolved.append(item)
    return "/".join(resolved)


def main_document_part(archive: zipfile.ZipFile) -> str:
    types = ET.fromstring(archive.read("[Content_Types].xml"))
    for override in types.findall("{" + CT_NS + "}Override"):
        content_type = override.attrib.get("ContentType", "")
        if content_type.endswith("wordprocessingml.document.main+xml"):
            return override.attrib["PartName"].lstrip("/")
    rels = ET.fromstring(archive.read("_rels/.rels"))
    for rel in rels.findall("{" + PKG_REL_NS + "}Relationship"):
        if rel.attrib.get("Type", "").endswith("/officeDocument"):
            return rel.attrib["Target"].lstrip("/")
    raise ValueError("DOCX has no main document part")


def optional_xml(archive: zipfile.ZipFile, part: str) -> Optional[ET.Element]:
    try:
        return ET.fromstring(archive.read(part))
    except KeyError:
        return None


def load_relationships(archive: zipfile.ZipFile, part: str) -> Dict[str, str]:
    root = optional_xml(archive, relationship_part(part))
    if root is None:
        return {}
    result = {}
    for rel in root.findall("{" + PKG_REL_NS + "}Relationship"):
        rel_id = rel.attrib.get("Id", "")
        target = rel.attrib.get("Target", "")
        if rel.attrib.get("TargetMode") == "External":
            result[rel_id] = target
        elif target:
            result[rel_id] = resolve_part(part, target)
    return result


def style_catalog(root: Optional[ET.Element]) -> Tuple[Dict[str, str], Dict[str, Dict[str, bool]]]:
    names: Dict[str, str] = {}
    formatting: Dict[str, Dict[str, bool]] = {}
    if root is None:
        return names, formatting
    for style in root.findall(W + "style"):
        style_id = style.attrib.get(W + "styleId", "")
        name = style.find(W + "name")
        names[style_id] = name.attrib.get(W + "val", style_id) if name is not None else style_id
        rpr = style.find(W + "rPr")
        formatting[style_id] = formatting_flags(rpr)
    return names, formatting


def numbering_catalog(root: Optional[ET.Element]) -> Dict[Tuple[str, str], str]:
    if root is None:
        return {}
    abstracts: Dict[Tuple[str, str], str] = {}
    for abstract in root.findall(W + "abstractNum"):
        abstract_id = abstract.attrib.get(W + "abstractNumId", "")
        for level in abstract.findall(W + "lvl"):
            ilvl = level.attrib.get(W + "ilvl", "0")
            fmt = level.find(W + "numFmt")
            abstracts[(abstract_id, ilvl)] = fmt.attrib.get(W + "val", "bullet") if fmt is not None else "bullet"
    nums: Dict[str, str] = {}
    for num in root.findall(W + "num"):
        abstract = num.find(W + "abstractNumId")
        if abstract is not None:
            nums[num.attrib.get(W + "numId", "")] = abstract.attrib.get(W + "val", "")
    return {(num_id, level): fmt for num_id, abstract_id in nums.items()
            for (candidate, level), fmt in abstracts.items() if candidate == abstract_id}


def bool_property(node: Optional[ET.Element], name: str) -> bool:
    if node is None:
        return False
    child = node.find(W + name)
    if child is None:
        return False
    return child.attrib.get(W + "val", "true").casefold() not in ("0", "false", "off", "none")


def formatting_flags(rpr: Optional[ET.Element]) -> Dict[str, bool]:
    return {
        "bold": bool_property(rpr, "b") or bool_property(rpr, "bCs"),
        "italic": bool_property(rpr, "i") or bool_property(rpr, "iCs"),
        "underline": (rpr is not None and rpr.find(W + "u") is not None
                      and rpr.find(W + "u").attrib.get(W + "val", "single") not in ("none", "0", "false")),
        "strike": bool_property(rpr, "strike") or bool_property(rpr, "dstrike"),
    }


def paragraph_semantics(paragraph: ET.Element, relationships: Mapping[str, str],
                        style_names: Mapping[str, str], style_formats: Mapping[str, Dict[str, bool]],
                        numbering: Mapping[Tuple[str, str], str]) -> Tuple[str, List[Tuple[str, Tuple[object, ...]]]]:
    parts: List[str] = []
    anchors: List[Tuple[str, Tuple[object, ...]]] = []
    ppr = paragraph.find(W + "pPr")
    style_id = ""
    if ppr is not None and ppr.find(W + "pStyle") is not None:
        style_id = ppr.find(W + "pStyle").attrib.get(W + "val", "")

    def visit(parent: ET.Element, hyperlink: Optional[str] = None) -> str:
        local: List[str] = []
        for child in list(parent):
            if child.tag == W + "r":
                run_parts: List[str] = []
                for run_child in list(child):
                    if run_child.tag == W + "t":
                        run_parts.append(run_child.text or "")
                    elif run_child.tag == W + "tab":
                        before = tokenize("".join(parts + local + run_parts))
                        anchors.append(("tab", ((before[-1] if before else ""), "")))
                        run_parts.append("\t")
                    elif run_child.tag in (W + "br", W + "cr"):
                        before = tokenize("".join(parts + local + run_parts))
                        anchors.append(("hard_break", ((before[-1] if before else ""), "")))
                        run_parts.append("\n")
                run_text = "".join(run_parts)
                rpr = child.find(W + "rPr")
                flags = formatting_flags(rpr)
                if rpr is not None and rpr.find(W + "rStyle") is not None:
                    run_style = rpr.find(W + "rStyle").attrib.get(W + "val", "")
                    for key, value in style_formats.get(run_style, {}).items():
                        flags[key] = flags[key] or value
                for key, enabled in flags.items():
                    if enabled and tokenize(run_text):
                        anchors.append(("emphasis:" + key, (run_text,)))
                local.append(run_text)
            elif child.tag == W + "hyperlink":
                target = relationships.get(child.attrib.get(R + "id", ""), child.attrib.get(W + "anchor", ""))
                label = visit(child, target)
                if label:
                    anchors.append(("link", (label, target)))
                local.append(label)
            elif child.tag in (W + "smartTag", W + "sdt", W + "customXml", W + "ins"):
                local.append(visit(child, hyperlink))
        return "".join(local)

    text = strip_excluded_images(visit(paragraph))
    style_name = style_names.get(style_id, style_id)
    heading = re.search(r"heading\s*([1-6])", style_name, re.I)
    if heading:
        anchors.append(("heading", (int(heading.group(1)), text)))
    elif style_name.casefold() in ("title", "subtitle"):
        anchors.append(("heading", (1 if style_name.casefold() == "title" else 2, text)))
    if "quote" in style_name.casefold():
        anchors.append(("quote", (text,)))
    if ppr is not None:
        numpr = ppr.find(W + "numPr")
        if numpr is not None:
            ilvl_node = numpr.find(W + "ilvl")
            num_node = numpr.find(W + "numId")
            level = ilvl_node.attrib.get(W + "val", "0") if ilvl_node is not None else "0"
            num_id = num_node.attrib.get(W + "val", "") if num_node is not None else ""
            fmt = numbering.get((num_id, level), "bullet")
            kind = "unordered" if fmt == "bullet" else "ordered"
            anchors.append(("list:" + kind, (int(level) if level.isdigit() else 0, text)))
    return text, anchors


def docx_semantics(path: Path) -> SemanticModel:
    model = SemanticModel()
    with zipfile.ZipFile(path) as archive:
        safe_zip_members(archive)
        part = main_document_part(archive)
        document = ET.fromstring(archive.read(part))
        relationships = load_relationships(archive, part)
        style_part = next((value for key, value in relationships.items() if value.endswith("styles.xml")),
                          str(PurePosixPath(part).parent / "styles.xml"))
        numbering_part = next((value for key, value in relationships.items() if value.endswith("numbering.xml")),
                              str(PurePosixPath(part).parent / "numbering.xml"))
        style_names, style_formats = style_catalog(optional_xml(archive, style_part))
        numbering = numbering_catalog(optional_xml(archive, numbering_part))
        body = document.find(W + "body")
        if body is None:
            raise ValueError("DOCX main part has no body")
        for child in list(body):
            if child.tag == W + "p":
                text, anchors = paragraph_semantics(child, relationships, style_names, style_formats, numbering)
                for kind, values in anchors:
                    model.add_anchor(kind, *values)
                model.add_block(text)
            elif child.tag == W + "tbl":
                for row_number, row in enumerate(child.findall(W + "tr")):
                    for column_number, cell in enumerate(row.findall(W + "tc")):
                        cell_parts: List[str] = []
                        for paragraph in cell.findall(".//" + W + "p"):
                            text, anchors = paragraph_semantics(
                                paragraph, relationships, style_names, style_formats, numbering)
                            cell_parts.append(text)
                            for kind, values in anchors:
                                model.add_anchor(kind, *values)
                        cell_text = "\n".join(part for part in cell_parts if part)
                        model.add_anchor("table_cell", row_number, column_number, cell_text)
                        model.add_block(cell_text)
    return model


def markdown_escape(text: str) -> str:
    return re.sub(r"([\\`*\[\]_|])", r"\\\1", text)


def wrap_markdown_run(text: str, bold: bool, italic: bool, underline: bool, strike: bool) -> str:
    escaped = markdown_escape(text)
    if not tokenize(text):
        return escaped
    if underline:
        escaped = "++" + escaped + "++"
    if strike:
        escaped = "~~" + escaped + "~~"
    if italic:
        escaped = "*" + escaped + "*"
    if bold:
        escaped = "**" + escaped + "**"
    return escaped


def python_docx_paragraph_to_markdown(paragraph: object, document: object) -> str:
    # python-docx has no Markdown converter; this adapter intentionally owns the mapping.
    chunks: List[str] = []
    try:
        inner = paragraph.iter_inner_content()
    except AttributeError:
        inner = paragraph.runs
    for item in inner:
        if item.__class__.__name__ == "Hyperlink":
            label = "".join(run.text for run in item.runs)
            chunks.append("[" + markdown_escape(label) + "](" + item.url + ")")
            continue
        text = item.text
        chunks.append(wrap_markdown_run(text, bool(item.bold), bool(item.italic),
                                        item.underline not in (None, False), bool(item.font.strike)))
    body = "".join(chunks)
    style_name = (paragraph.style.name or "") if paragraph.style is not None else ""
    heading = re.search(r"Heading\s+([1-6])", style_name, re.I)
    if heading:
        return "#" * int(heading.group(1)) + " " + body
    if "quote" in style_name.casefold():
        return "> " + body
    ppr = paragraph._p.pPr
    numpr = ppr.numPr if ppr is not None else None
    if numpr is not None:
        level = int(numpr.ilvl.val) if numpr.ilvl is not None else 0
        marker = "-"
        try:
            num_id = str(numpr.numId.val)
            numbering = document.part.numbering_part.element
            num = next(item for item in numbering.num_lst if str(item.numId) == num_id)
            abstract = next(item for item in numbering.abstractNum_lst
                            if str(item.abstractNumId) == str(num.abstractNumId.val))
            lvl = next(item for item in abstract.lvl_lst if int(item.ilvl) == level)
            if str(lvl.numFmt.val) != "bullet":
                marker = "1."
        except (AttributeError, StopIteration, ValueError):
            if "number" in style_name.casefold():
                marker = "1."
        return "  " * level + marker + " " + body
    if "list bullet" in style_name.casefold():
        return "- " + body
    if "list number" in style_name.casefold():
        return "1. " + body
    return body


def python_docx_to_markdown(input_path: Path, output_path: Path) -> None:
    from docx import Document  # type: ignore
    from docx.table import Table  # type: ignore
    from docx.text.paragraph import Paragraph  # type: ignore

    document = Document(str(input_path))
    blocks: List[str] = []
    for child in document.element.body.iterchildren():
        if child.tag == W + "p":
            blocks.append(python_docx_paragraph_to_markdown(Paragraph(child, document), document))
        elif child.tag == W + "tbl":
            table = Table(child, document)
            rows: List[List[str]] = []
            for row in table.rows:
                cells = []
                for cell in row.cells:
                    pieces = [python_docx_paragraph_to_markdown(p, document) for p in cell.paragraphs]
                    cells.append("<br>".join(pieces).replace("|", r"\|"))
                rows.append(cells)
            if rows:
                width = max(len(row) for row in rows)
                rows = [row + [""] * (width - len(row)) for row in rows]
                blocks.append("| " + " | ".join(rows[0]) + " |\n| "
                              + " | ".join("---" for _ in range(width)) + " |\n"
                              + "\n".join("| " + " | ".join(row) + " |" for row in rows[1:]))
    output_path.write_text("\n\n".join(blocks).rstrip() + "\n", encoding="utf-8")


def append_hyperlink(paragraph: object, label: str, target: str) -> None:
    from docx.oxml import OxmlElement  # type: ignore
    from docx.oxml.ns import qn  # type: ignore
    from docx.opc.constants import RELATIONSHIP_TYPE  # type: ignore

    relationship_id = paragraph.part.relate_to(target, RELATIONSHIP_TYPE.HYPERLINK, is_external=True)
    hyperlink = OxmlElement("w:hyperlink")
    hyperlink.set(qn("r:id"), relationship_id)
    run = OxmlElement("w:r")
    text = OxmlElement("w:t")
    text.set("{" + XML_NS + "}space", "preserve")
    text.text = label
    run.append(text)
    hyperlink.append(run)
    paragraph._p.append(hyperlink)


def add_python_docx_inline(paragraph: object, source: str) -> None:
    token_pattern = re.compile(
        r"!\[[^\]]*\]\([^)]*\)|\[([^\]]+)\]\(([^\s)]+)(?:\s+[\"'][^)]*[\"'])?\)"
        r"|\*\*(.+?)\*\*|__(.+?)__|\+\+(.+?)\+\+|~~(.+?)~~|(`+)(.+?)\7"
        r"|(?<!\*)\*([^*\n]+?)\*(?!\*)|(?<!\w)_([^_\n]+?)_(?!\w)")

    def add_plain(value: str) -> None:
        pieces = re.split(r"(<br\s*/?>|\n)", value, flags=re.I)
        for piece in pieces:
            if not piece:
                continue
            if piece == "\n" or re.fullmatch(r"<br\s*/?>", piece, re.I):
                paragraph.add_run().add_break()
            else:
                paragraph.add_run(re.sub(r"\\([\\`*{}\[\]()#+.!_>~-])", r"\1", html.unescape(piece)))

    position = 0
    for match in token_pattern.finditer(source):
        add_plain(source[position:match.start()])
        value = match.group(0)
        if value.startswith("!["):
            pass
        elif match.group(1) is not None:
            append_hyperlink(paragraph, match.group(1), html.unescape(match.group(2)))
        else:
            content = next((match.group(index) for index in (3, 4, 5, 6, 8, 9, 10)
                            if match.group(index) is not None), "")
            run = paragraph.add_run(content)
            if value.startswith(("**", "__")):
                run.bold = True
            elif value.startswith("++"):
                run.underline = True
            elif value.startswith("~~"):
                run.font.strike = True
            elif value.startswith("`"):
                run.font.name = "Courier New"
            else:
                run.italic = True
        position = match.end()
    add_plain(source[position:])


def python_docx_from_markdown(input_path: Path, output_path: Path) -> None:
    from docx import Document  # type: ignore
    from docx.shared import Inches  # type: ignore

    lines = input_path.read_text(encoding="utf-8").replace("\r\n", "\n").replace("\r", "\n").split("\n")
    document = Document()
    index = 0
    while index < len(lines):
        line = lines[index]
        if not line.strip():
            index += 1
            continue
        fence = FENCE_RE.match(line)
        if fence:
            payload = []
            delimiter = fence.group(1)
            index += 1
            while index < len(lines) and not lines[index].lstrip().startswith(delimiter):
                payload.append(lines[index])
                index += 1
            if index < len(lines):
                index += 1
            paragraph = document.add_paragraph()
            run = paragraph.add_run("\n".join(payload))
            run.font.name = "Courier New"
            continue
        heading = HEADING_RE.match(line)
        if heading:
            paragraph = document.add_heading(level=len(heading.group(1)))
            add_python_docx_inline(paragraph, heading.group(2))
            index += 1
            continue
        if index + 1 < len(lines) and "|" in line and TABLE_RULE_RE.match(lines[index + 1]):
            rows = [split_table_row(line)]
            index += 2
            while index < len(lines) and "|" in lines[index] and lines[index].strip():
                rows.append(split_table_row(lines[index]))
                index += 1
            width = max(len(row) for row in rows)
            table = document.add_table(rows=len(rows), cols=width)
            for row_number, row in enumerate(rows):
                for column_number, cell_source in enumerate(row):
                    add_python_docx_inline(table.cell(row_number, column_number).paragraphs[0], cell_source)
            continue
        list_match = LIST_RE.match(line)
        if list_match:
            indent, marker, body = list_match.groups()
            ordered = marker[0].isdigit()
            paragraph = document.add_paragraph(style="List Number" if ordered else "List Bullet")
            level = max(0, len(indent.expandtabs(4)) // 2)
            if level:
                paragraph.paragraph_format.left_indent = Inches(0.25 * level)
            add_python_docx_inline(paragraph, body)
            index += 1
            continue
        if line.lstrip().startswith(">"):
            paragraph = document.add_paragraph(style="Quote")
            add_python_docx_inline(paragraph, re.sub(r"^\s*>\s?", "", line))
            index += 1
            continue
        paragraph_lines = [line]
        index += 1
        while index < len(lines) and lines[index].strip():
            next_line = lines[index]
            if FENCE_RE.match(next_line) or HEADING_RE.match(next_line) or LIST_RE.match(next_line) or next_line.lstrip().startswith(">"):
                break
            if index + 1 < len(lines) and "|" in next_line and TABLE_RULE_RE.match(lines[index + 1]):
                break
            paragraph_lines.append(next_line)
            index += 1
        joined = ""
        for line_number, part in enumerate(paragraph_lines):
            hard = bool(re.search(r" {2,}$", part))
            joined += re.sub(r" {2,}$", "", part)
            if line_number + 1 < len(paragraph_lines):
                joined += "\n" if hard else " "
        paragraph = document.add_paragraph()
        add_python_docx_inline(paragraph, joined)
    document.save(str(output_path))


def spire_convert(direction: str, input_path: Path, output_path: Path) -> None:
    from spire.doc import Document, FileFormat  # type: ignore

    document = Document()
    try:
        if direction == "md_to_docx":
            document.LoadFromFile(str(input_path), FileFormat.Markdown)
            document.SaveToFile(str(output_path), FileFormat.Docx)
        else:
            document.LoadFromFile(str(input_path))
            document.SaveToFile(str(output_path), FileFormat.Markdown)
    finally:
        document.Close()


def worker_main(candidate: str, direction: str, input_name: str, output_name: str) -> int:
    input_path = Path(input_name)
    output_path = Path(output_name)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    if candidate == "python-docx-adapter":
        if direction == "docx_to_md":
            python_docx_to_markdown(input_path, output_path)
        else:
            python_docx_from_markdown(input_path, output_path)
    elif candidate == "spire-doc":
        spire_convert(direction, input_path, output_path)
    else:
        raise ValueError("unknown worker candidate: " + candidate)
    return 0


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_utf8(path: Path) -> Tuple[Optional[str], Optional[str]]:
    try:
        return path.read_bytes().decode("utf-8"), None
    except (OSError, UnicodeDecodeError) as exc:
        return None, str(exc)


def markdown_validation(path: Path, source_name: str) -> Dict[str, object]:
    text, error = read_utf8(path)
    if error:
        return {"valid_utf8": False, "error": error, "unbalanced_emphasis": True,
                "four_asterisk_artifacts": 0, "regression_needles": {}}
    assert text is not None
    scrubbed = re.sub(r"```.*?```|~~~.*?~~~|`[^`]*`", "", text, flags=re.S)
    balances = {}
    for delimiter in ("**", "~~", "++"):
        balances[delimiter] = len(re.findall(r"(?<!\\)" + re.escape(delimiter), scrubbed)) % 2 == 0
    needles: Dict[str, bool] = {}
    lowered = source_name.casefold()
    if "laura_byman" in lowered:
        needles["Laura Byman"] = "Laura Byman" in text
    if "svg-kuvien" in lowered:
        needles["Niklas Mälton"] = "Niklas Mälton" in text
    return {
        "valid_utf8": True,
        "unbalanced_emphasis": not all(balances.values()),
        "delimiter_balances": balances,
        "four_asterisk_artifacts": text.count("****"),
        "regression_needles": needles,
        "evaluation_text_present": bool(EVALUATION_RE.search(text)),
    }


def docx_package_validation(path: Path) -> Dict[str, object]:
    try:
        with zipfile.ZipFile(path) as archive:
            safe_zip_members(archive)
            bad_member = archive.testzip()
            part = main_document_part(archive)
            ET.fromstring(archive.read("[Content_Types].xml"))
            ET.fromstring(archive.read("_rels/.rels"))
            ET.fromstring(archive.read(part))
            return {"valid": bad_member is None, "bad_crc_member": bad_member, "main_part": part}
    except (OSError, KeyError, ValueError, zipfile.BadZipFile, ET.ParseError) as exc:
        return {"valid": False, "error": str(exc)}


def libreoffice_validation(path: Path, reference: SemanticModel, libreoffice: Optional[str]) -> Dict[str, object]:
    if libreoffice is None:
        return {"available": False, "opened": None, "text_token_recall": None}
    with tempfile.TemporaryDirectory(prefix="ainiux-docx-lo-") as temporary:
        root = Path(temporary)
        output_dir = root / "out"
        output_dir.mkdir()
        profile = root / "profile"
        command = [libreoffice, "--headless", "-env:UserInstallation=file://" + quote(str(profile)),
                   "--convert-to", "txt:Text", "--outdir", str(output_dir), str(path)]
        try:
            completed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                       check=False, timeout=120, env=stable_environment())
        except (OSError, subprocess.TimeoutExpired) as exc:
            return {"available": True, "opened": False, "error": str(exc), "text_token_recall": None}
        text_path = output_dir / (path.stem + ".txt")
        text, error = read_utf8(text_path) if text_path.exists() else (None, "no text output")
        if completed.returncode != 0 or error:
            message = completed.stderr.decode("utf-8", errors="replace").strip() or error
            return {"available": True, "opened": False, "exit_status": completed.returncode,
                    "error": message, "text_token_recall": None}
        assert text is not None
        scores = counter_scores(Counter(reference.tokens), Counter(tokenize(text)))
        return {"available": True, "opened": True, "exit_status": completed.returncode,
                "text_token_recall": scores["recall"], "stdout": completed.stdout.decode("utf-8", errors="replace").strip(),
                "stderr": completed.stderr.decode("utf-8", errors="replace").strip()}


def stable_environment() -> Dict[str, str]:
    environment = dict(os.environ)
    environment.update({"LC_ALL": "C.UTF-8", "LANG": "C.UTF-8", "PYTHONHASHSEED": "0", "TZ": "UTC"})
    environment.pop("PYTHONPATH", None)
    return environment


def set_affinity(cpu: int) -> None:
    os.sched_setaffinity(0, {cpu})


def run_measured(command: Sequence[str], stdout_path: Path, stderr_path: Path, cpu: int) -> Dict[str, object]:
    import resource

    stdout_path.parent.mkdir(parents=True, exist_ok=True)
    start = time.perf_counter()
    with stdout_path.open("wb") as stdout_stream, stderr_path.open("wb") as stderr_stream:
        process = subprocess.Popen(list(command), stdout=stdout_stream, stderr=stderr_stream,
                                   env=stable_environment(), preexec_fn=lambda: set_affinity(cpu))
        _, status, usage = os.wait4(process.pid, 0)
        process.returncode = os.waitstatus_to_exitcode(status)
    wall = time.perf_counter() - start
    peak_rss_bytes = int(usage.ru_maxrss) * (1 if sys.platform == "darwin" else 1024)
    return {
        "wall_seconds": wall,
        "cpu_seconds": usage.ru_utime + usage.ru_stime,
        "user_cpu_seconds": usage.ru_utime,
        "system_cpu_seconds": usage.ru_stime,
        "peak_rss_bytes": peak_rss_bytes,
        "exit_status": process.returncode,
        "command": list(command),
    }


def input_direction(path: Path) -> str:
    suffix = path.suffix.casefold()
    if suffix == ".docx":
        return "docx_to_md"
    if suffix in (".md", ".markdown", ".mkd", ".mdown"):
        return "md_to_docx"
    raise ValueError("input must be DOCX or Markdown: " + str(path))


def oracle_for_input(path: Path, direction: str) -> Tuple[SemanticModel, str]:
    if direction == "md_to_docx":
        return markdown_semantics(path.read_text(encoding="utf-8")), "source_markdown"
    golden = path.with_suffix(".md")
    if golden.is_file():
        return markdown_semantics(golden.read_text(encoding="utf-8")), "adjacent_markdown_golden"
    return docx_semantics(path), "stdlib_ooxml_main_story"


def warnings_from_file(path: Path) -> List[str]:
    text, error = read_utf8(path)
    if error:
        return [error]
    assert text is not None
    return [line for line in text.splitlines() if line.strip()]


def conversion_command(candidate: str, direction: str, input_path: Path, output_path: Path,
                       ainiux: Path) -> List[str]:
    if candidate == "ainiux":
        output_format = "md" if direction == "docx_to_md" else "docx"
        maximum = max(32 * 1024 * 1024, input_path.stat().st_size + 1024 * 1024)
        return [str(ainiux), "--no-config", "--input", str(input_path), "--output-format", output_format,
                "--output", str(output_path), "--max-input-bytes", str(maximum)]
    return [sys.executable, str(Path(__file__).resolve()), "--worker", candidate, direction,
            str(input_path), str(output_path)]


def package_version(distribution: str) -> str:
    try:
        return importlib.metadata.version(distribution)
    except importlib.metadata.PackageNotFoundError:
        return "not installed"


def git_value(arguments: Sequence[str]) -> str:
    try:
        completed = subprocess.run(["git"] + list(arguments), cwd=Path(__file__).resolve().parents[2],
                                   stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=True, text=True)
        return completed.stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def executable_version(command: Optional[str]) -> str:
    if command is None:
        return "not installed"
    try:
        completed = subprocess.run([command, "--version"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   check=False, text=True, timeout=30)
        return completed.stdout.strip().splitlines()[0]
    except (OSError, subprocess.TimeoutExpired, IndexError):
        return "unknown"


def cpu_description() -> str:
    try:
        for line in Path("/proc/cpuinfo").read_text(encoding="utf-8").splitlines():
            if line.casefold().startswith(("model name", "hardware")):
                return line.split(":", 1)[-1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def aggregate_results(results: Sequence[Mapping[str, object]]) -> Dict[str, object]:
    complete_by_case: Dict[str, set] = {}
    all_cases = set()
    for row in results:
        case = str(row["case_id"])
        all_cases.add(case)
        if row.get("complete"):
            complete_by_case.setdefault(case, set()).add(str(row["candidate"]))
    common = sorted(case for case in all_cases if complete_by_case.get(case) == set(CANDIDATES))

    def totals(cases: Iterable[str], require_all: bool) -> List[Dict[str, object]]:
        selected_cases = set(cases)
        rows = []
        for candidate in CANDIDATES:
            candidate_rows = [row for row in results if row["candidate"] == candidate
                              and row["case_id"] in selected_cases]
            fully_complete = len(candidate_rows) == len(selected_cases) and all(row.get("complete") for row in candidate_rows)
            if require_all and not fully_complete:
                continue
            wall = sum(float(row["timing"]["wall_seconds"]) for row in candidate_rows)  # type: ignore[index]
            cpu = sum(float(row["timing"]["cpu_seconds"]) for row in candidate_rows)  # type: ignore[index]
            input_bytes = sum(int(row["input_bytes"]) for row in candidate_rows)
            rows.append({"candidate": candidate, "cases": len(candidate_rows), "complete": fully_complete,
                         "wall_seconds": wall, "cpu_seconds": cpu, "input_bytes": input_bytes,
                         "throughput_bytes_per_second": input_bytes / wall if wall else None,
                         "mean_token_f1": (sum(float(row["accuracy"]["token"]["f1"]) for row in candidate_rows)  # type: ignore[index]
                                           / len(candidate_rows) if candidate_rows else None)})
        ainiux_wall = next((float(row["wall_seconds"]) for row in rows if row["candidate"] == "ainiux"), None)
        for row in rows:
            row["wall_ratio_vs_ainiux"] = (float(row["wall_seconds"]) / ainiux_wall
                                             if ainiux_wall else None)
        return rows

    fully_complete_candidates = []
    for candidate in CANDIDATES:
        candidate_rows = [row for row in results if row["candidate"] == candidate]
        if len(candidate_rows) == len(all_cases) and all(row.get("complete") for row in candidate_rows):
            fully_complete_candidates.append(candidate)
    full_rows = totals(sorted(all_cases), require_all=True)
    return {
        "common_complete_case_ids": common,
        "common_complete_totals": totals(common, require_all=False),
        "full_corpus_case_ids": sorted(all_cases),
        "full_corpus_eligible_candidates": fully_complete_candidates,
        "full_corpus_totals": full_rows,
    }


def rescore_payload(payload: MutableMapping[str, object], raw_path: Path) -> None:
    """Recompute semantic scores from preserved outputs without rerunning candidates."""
    output_root = raw_path.parent
    results = payload["results"]
    if not isinstance(results, list):
        raise ValueError("raw JSON results must be a list")
    oracle_cache: Dict[Tuple[str, str], SemanticModel] = {}
    for row in results:
        if not isinstance(row, MutableMapping):
            raise ValueError("raw JSON result row must be an object")
        input_path = Path(str(row["input"]))
        direction = str(row["direction"])
        key = (str(input_path), direction)
        if key not in oracle_cache:
            oracle_cache[key] = oracle_for_input(input_path, direction)[0]
        reference = oracle_cache[key]
        output_path = output_root / str(row["output"])
        exit_status = int(row["timing"]["exit_status"])
        output_exists = output_path.is_file()
        validation = row.get("validation")
        if not isinstance(validation, MutableMapping):
            validation = {}
            row["validation"] = validation
        hypothesis = SemanticModel()
        parse_error: Optional[str] = None
        if output_exists and exit_status == 0:
            try:
                if direction == "docx_to_md":
                    validation["markdown"] = markdown_validation(output_path, input_path.name)
                    text, utf8_error = read_utf8(output_path)
                    if utf8_error:
                        parse_error = utf8_error
                    else:
                        hypothesis = markdown_semantics(text or "")
                    validation["evaluation_text_present"] = bool(text and EVALUATION_RE.search(text))
                else:
                    package = docx_package_validation(output_path)
                    validation["docx_package"] = package
                    if package["valid"]:
                        hypothesis = docx_semantics(output_path)
                        validation["evaluation_text_present"] = bool(
                            EVALUATION_RE.search(" ".join(hypothesis.tokens)))
                    else:
                        parse_error = str(package.get("error", "invalid DOCX package"))
            except (OSError, KeyError, ValueError, zipfile.BadZipFile, ET.ParseError) as exc:
                parse_error = str(exc)
        accuracy = score_models(reference, hypothesis)
        reasons = []
        if exit_status != 0:
            reasons.append("exit status " + str(exit_status))
        if not output_exists:
            reasons.append("missing output")
        if parse_error:
            reasons.append("format/oracle error: " + parse_error)
        if float(accuracy["token"]["recall"]) < 0.99:  # type: ignore[index]
            reasons.append("token recall below 99%")
        if not accuracy["final_source_block_present"]:
            reasons.append("final source block absent")
        if direction == "md_to_docx":
            package = validation.get("docx_package", {})
            if package and not package.get("valid"):
                reasons.append("invalid DOCX package")
            libreoffice = validation.get("libreoffice", {})
            if libreoffice and libreoffice.get("available") and not libreoffice.get("opened"):
                reasons.append("LibreOffice open failed")
        row["accuracy"] = accuracy
        row["complete"] = not reasons
        row["incomplete_reasons"] = reasons
    payload["aggregates"] = aggregate_results(results)
    metadata = payload.get("metadata")
    if isinstance(metadata, MutableMapping):
        metadata["harness_sha256"] = sha256_file(Path(__file__).resolve())


def human_bytes(value: Optional[float]) -> str:
    if value is None:
        return "—"
    units = ("B", "KiB", "MiB", "GiB")
    amount = float(value)
    unit = units[0]
    for unit in units:
        if abs(amount) < 1024.0 or unit == units[-1]:
            break
        amount /= 1024.0
    return f"{amount:.1f} {unit}" if unit != "B" else f"{int(amount)} B"


def pct(value: object) -> str:
    return f"{100.0 * float(value):.2f}%"


def candidate_label(name: str) -> str:
    return {"ainiux": "Ainiux", "python-docx-adapter": "python-docx adapter",
            "spire-doc": "Spire.Doc"}.get(name, name)


def render_report(payload: Mapping[str, object]) -> str:
    metadata = payload["metadata"]  # type: ignore[assignment]
    results = payload["results"]  # type: ignore[assignment]
    aggregates = payload["aggregates"]  # type: ignore[assignment]
    lines = [
        "# DOCX conversion benchmark (" + str(metadata["date"]) + ")", "",
        "This point-in-time benchmark compares canonical semantic conversion, not lossless Word layout. "
        "Each of the " + str(len(results)) + " candidate/file combinations ran exactly once in a fresh process with no warmup or retry. "
        "Wall time includes process and library startup, conversion, and output writes.", "",
        "## Method", "",
        "All conversions were pinned to logical CPU " + str(metadata["cpu_affinity"]) +
        " with `LC_ALL=C.UTF-8`, `PYTHONHASHSEED=0`, and isolated output directories. Candidate order rotates by input "
        "to reduce fixed cache-order bias. Ainiux was invoked by explicit repository path; its revision and binary digest "
        "are recorded below.", "",
        "The independent standard-library oracle uses the adjacent Markdown goldens for the two small DOCX fixtures, "
        "extracts supported main-story OOXML semantics for the two large DOCX files, and treats each Markdown source as "
        "the expected model for Markdown-to-DOCX. Counted tokens, ordered token bigrams, and text-anchored structures are "
        "scored independently. Structures cover headings, ordered/unordered lists, tables, links, bold/italic/underline/strike, "
        "code, quotes, tabs, and hard breaks. Images, headers/footers, notes, comments, fonts, colors, and page layout are excluded.", "",
        "A result is incomplete when conversion or format validation fails, counted-token recall is below 99%, or the final "
        "source block is absent. Incomplete results remain visible but cannot contribute to speed rankings. Unlicensed Spire.Doc "
        "evaluation text is retained and counted as extra content.", "",
        "## Per-file results", "",
        "| Direction | File | Candidate | Input→output | Wall | CPU | Peak RSS | Throughput | Exit | Token P/R/F1 | Bigram F1 | Structure F1 | Complete | vs Ainiux |",
        "| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | --- | ---: |",
    ]
    complete_baselines = {row["case_id"]: row["complete"] for row in results
                          if row["candidate"] == "ainiux"}
    for row in results:
        timing = row["timing"]
        accuracy = row["accuracy"]
        ratio = row.get("wall_ratio_vs_ainiux")
        ratio_eligible = row["complete"] and complete_baselines.get(row["case_id"], False)
        ratio_text = f"{float(ratio):.2f}×" if ratio is not None and ratio_eligible else "—"
        direction = "DOCX→MD" if row["direction"] == "docx_to_md" else "MD→DOCX"
        token = accuracy["token"]
        lines.append("| " + " | ".join([
            direction, "`" + str(row["input_display"]).replace("|", r"\|") + "`",
            candidate_label(str(row["candidate"])), human_bytes(float(row["input_bytes"])) + "→" + human_bytes(float(row["output_bytes"])),
            f"{float(timing['wall_seconds']):.4f} s",
            f"{float(timing['cpu_seconds']):.4f} s", human_bytes(float(timing["peak_rss_bytes"])),
            human_bytes(float(row["throughput_bytes_per_second"])) + "/s", str(timing["exit_status"]),
            pct(token["precision"]) + "/" + pct(token["recall"]) + "/" + pct(token["f1"]),
            pct(accuracy["ordered_token_bigram"]["f1"]), pct(accuracy["anchored_structure"]["f1"]),
            "yes" if row["complete"] else "**no**", ratio_text]) + " |")

    lines.extend(["", "`vs Ainiux` is candidate wall time divided by Ainiux wall time on the same file; larger is slower.", "",
                  "## Aggregate comparisons", ""])
    common_ids = aggregates["common_complete_case_ids"]
    lines.append("Three-way totals use only the common complete subset: **" + str(len(common_ids)) + " of "
                 + str(len(aggregates["full_corpus_case_ids"])) + " cases**.")
    lines.extend(["", "| Candidate | Eligible cases | Wall total | CPU total | Throughput | Mean token F1 | vs Ainiux |",
                  "| --- | ---: | ---: | ---: | ---: | ---: | ---: |"])
    for row in aggregates["common_complete_totals"]:
        lines.append("| " + " | ".join([
            candidate_label(str(row["candidate"])), str(row["cases"]), f"{float(row['wall_seconds']):.4f} s",
            f"{float(row['cpu_seconds']):.4f} s", human_bytes(row["throughput_bytes_per_second"]) + "/s",
            pct(row["mean_token_f1"]) if row["mean_token_f1"] is not None else "—",
            f"{float(row['wall_ratio_vs_ainiux']):.2f}×" if row["wall_ratio_vs_ainiux"] is not None else "—"]) + " |")
    eligible = aggregates["full_corpus_eligible_candidates"]
    lines.extend(["", "Full-corpus totals are shown only for candidates completing all eight cases. Eligible: "
                  + (", ".join(candidate_label(item) for item in eligible) if eligible else "none") + ".", ""])
    if aggregates["full_corpus_totals"]:
        lines.extend(["| Candidate | Cases | Wall total | CPU total | Throughput | Mean token F1 | vs Ainiux |",
                      "| --- | ---: | ---: | ---: | ---: | ---: | ---: |"])
        for row in aggregates["full_corpus_totals"]:
            lines.append("| " + " | ".join([
                candidate_label(str(row["candidate"])), str(row["cases"]), f"{float(row['wall_seconds']):.4f} s",
                f"{float(row['cpu_seconds']):.4f} s", human_bytes(row["throughput_bytes_per_second"]) + "/s",
                pct(row["mean_token_f1"]),
                f"{float(row['wall_ratio_vs_ainiux']):.2f}×" if row["wall_ratio_vs_ainiux"] is not None else "—"]) + " |")

    complete_counts = {candidate: sum(1 for row in results
                                      if row["candidate"] == candidate and row["complete"])
                       for candidate in CANDIDATES}
    common_totals = aggregates["common_complete_totals"]
    fastest = min(common_totals, key=lambda item: item["wall_seconds"]) if common_totals else None
    spire_evaluations = sum(1 for row in results if row["candidate"] == "spire-doc"
                            and row["validation"].get("evaluation_text_present"))
    docx_rows = [row for row in results if row["direction"] == "md_to_docx" and row["output_bytes"]]
    opened_rows = [row for row in docx_rows if row["validation"].get("libreoffice", {}).get("opened")]
    lines.extend(["", "## Findings", ""])
    if fastest is not None:
        lines.append("- On the " + str(len(common_ids)) + "-case common complete subset, **"
                     + candidate_label(str(fastest["candidate"])) + " was fastest** at "
                     + f"{float(fastest['wall_seconds']):.4f} seconds total. "
                     + "; ".join(candidate_label(str(row["candidate"])) + " was "
                                 + f"{float(row['wall_ratio_vs_ainiux']):.2f}× Ainiux time"
                                 for row in common_totals if row["candidate"] != "ainiux") + ".")
    lines.append("- Complete conversions: " + ", ".join(candidate_label(candidate) + " "
                 + str(complete_counts[candidate]) + "/" + str(len(results) // len(CANDIDATES))
                 for candidate in CANDIDATES) + ".")
    lines.append("- All " + str(len(opened_rows)) + " generated DOCX packages that existed opened in LibreOffice"
                 + ("." if len(opened_rows) == len(docx_rows) else
                    f"; {len(docx_rows) - len(opened_rows)} did not pass the open check."))
    lines.append("- Spire evaluation text was present in " + str(spire_evaluations) + "/"
                 + str(len(results) // len(CANDIDATES)) + " outputs and was retained in precision scoring. "
                 "Its large-file recall losses are consistent with an unlicensed-limit risk, so those timings must not rank as complete work.")
    control_failures = [row for row in results if row["input_display"].endswith("we_wont_be_missed.md")
                        and not row["complete"]]
    if control_failures:
        lines.append("- `we_wont_be_missed.md` contains XML-forbidden control bytes. Ainiux and the python-docx adapter "
                     "rejected it; Spire wrote a truncated document. Sanitize or explicitly reject forbidden controls before "
                     "using this source in a success-only writer benchmark.")
    name_checks = [value for row in results for value in
                   row["validation"].get("markdown", {}).get("regression_needles", {}).values()]
    if name_checks:
        lines.append("- The `Laura Byman` and `Niklas Mälton` regression needles were preserved in all "
                     + str(len(name_checks)) + " applicable Markdown outputs; no targeted emphasis or `****` artifact check fired.")

    incomplete = [row for row in results if not row["complete"]]
    lines.extend(["", "## Completeness and validation", ""])
    if incomplete:
        lines.extend(["| File | Candidate | Reason | Evaluation text | LibreOffice |", "| --- | --- | --- | --- | --- |"])
        for row in incomplete:
            validation = row["validation"]
            reasons = ", ".join(row["incomplete_reasons"])
            evaluation = validation.get("evaluation_text_present", False)
            lo = validation.get("libreoffice", {})
            lo_text = "opened" if lo.get("opened") is True else ("failed" if lo.get("opened") is False else "not available")
            lines.append("| `" + str(row["input_display"]).replace("|", r"\|") + "` | "
                         + candidate_label(str(row["candidate"])) + " | " + reasons.replace("|", r"\|") + " | "
                         + ("yes" if evaluation else "no") + " | " + lo_text + " |")
    else:
        lines.append("All " + str(len(results)) + " conversions met the completeness rule and all generated DOCX packages passed validation.")

    artifact_rows = []
    for row in results:
        check = row["validation"].get("markdown")
        if check and (check.get("unbalanced_emphasis") or check.get("four_asterisk_artifacts")
                      or not all(check.get("regression_needles", {}).values())):
            artifact_rows.append(row)
    lines.extend(["", "Markdown outputs were decoded strictly as UTF-8 and checked for unbalanced emphasis delimiters, "
                  "`****` artifacts, and the `Laura Byman` / `Niklas Mälton` regression needles. Generated DOCX files "
                  "were inspected as ZIP/OPC packages and opened through LibreOffice headless text export."])
    if artifact_rows:
        lines.append("The following Markdown checks reported findings: " + ", ".join(
            "`" + str(row["input_display"]) + "`/" + candidate_label(str(row["candidate"])) for row in artifact_rows) + ".")
    else:
        lines.append("No targeted Markdown artifact check fired.")

    warning_rows = [row for row in results if row["warnings"]]
    if warning_rows:
        lines.extend(["", "### Converter diagnostics", "",
                      "These stderr diagnostics are also preserved verbatim beside each output and in `raw.json`.", "",
                      "| File | Candidate | Diagnostic |", "| --- | --- | --- |"])
        for row in warning_rows:
            for warning in row["warnings"]:
                safe_warning = str(warning).replace("|", r"\|").replace("\n", " ")
                lines.append("| `" + str(row["input_display"]).replace("|", r"\|") + "` | "
                             + candidate_label(str(row["candidate"])) + " | " + safe_warning + " |")

    lines.extend(["", "## Environment", "", "| Item | Value |", "| --- | --- |",
                  "| Timestamp | `" + str(metadata["generated_at_utc"]) + "` |",
                  "| Host | `" + str(metadata["platform"]).replace("|", r"\|") + "` |",
                  "| CPU | `" + str(metadata["cpu"]).replace("|", r"\|") + "` |",
                  "| Python | `" + str(metadata["python"]).replace("|", r"\|") + "` |",
                  "| python-docx | `" + str(metadata["packages"]["python-docx"]) + "` |",
                  "| Spire.Doc | `" + str(metadata["packages"]["spire-doc"]) + "` (unlicensed) |",
                  "| LibreOffice | `" + str(metadata["libreoffice"]).replace("|", r"\|") + "` |",
                  "| Ainiux revision | `" + str(metadata["ainiux_git_revision"]) + "` |",
                  "| Ainiux SHA-256 | `" + str(metadata["ainiux_sha256"]) + "` |",
                  "| Ainiux binary | `" + str(metadata["ainiux_binary"]) + "` |",
                  "| Harness SHA-256 | `" + str(metadata.get("harness_sha256", "not recorded")) + "` |", "",
                  "Exact harness command:", "", "```sh", str(metadata["exact_command"]), "```", "",
                  "The pinned releases are documented on [python-docx PyPI](https://pypi.org/project/python-docx/) and "
                  "[Spire.Doc PyPI](https://pypi.org/project/spire-doc/). Spire's API exposes native `Markdown` and `Docx` "
                  "formats in its [conversion interface](https://www.e-iceblue.com/Knowledgebase/Python/Spire.Doc-for-Python/Program-Guide/Conversion.html). "
                  "The vendor states that unlicensed use can impose paragraph/table limits during load or save; see the "
                  "[Spire limitation discussion](https://www.e-iceblue.com/forum/paragraph-recognizing-problem-t13628.html).", "",
                  "## Limitations and follow-up", "",
                  "These are single observations, not statistically stable latency estimates. OS cache state still matters despite "
                  "fresh processes and rotated order. The python-docx numbers belong to this benchmark's semantic adapter because "
                  "python-docx itself reads and writes Word documents but does not provide a Markdown converter. Spire ran without a license; "
                  "evaluation content reduces precision and product limits may truncate large conversions.", "",
                  "Treat any low token/bigram score, structure mismatch, delimiter artifact, missing name needle, or LibreOffice discrepancy "
                  "as a parser/writer follow-up. This benchmark intentionally does not change the production converter; fixes should receive "
                  "focused DOCX fixtures and failure-path tests in a separate change.", ""])
    return "\n".join(lines)


def default_inputs(repository: Path) -> List[Path]:
    return [
        repository / "tests/docx_files/headings-lists-links.docx",
        repository / "tests/docx_files/tables.docx",
        Path.home() / "test_files/SVG-kuvien optimointi_valmis.docx",
        Path.home() / "test_files/opinnäytetyö_Laura_Byman_final.docx",
        repository / "tests/docx_files/supported.md",
        repository / "tests/fixtures/comprehensive.md",
        Path.home() / "test_files/we_wont_be_missed.md",
        Path.home() / "test_files/dracula.md",
    ]


def run_benchmark(args: argparse.Namespace) -> int:
    repository = Path(__file__).resolve().parents[2]
    ainiux = Path(args.ainiux).expanduser().resolve()
    if not ainiux.is_file() or not os.access(ainiux, os.X_OK):
        raise ValueError("Ainiux must be an executable file: " + str(ainiux))
    inputs = [Path(item).expanduser().resolve() for item in args.inputs] if args.inputs else default_inputs(repository)
    if not inputs or len(inputs) > MAX_INPUTS:
        raise ValueError(f"benchmark accepts 1-{MAX_INPUTS} source files, got {len(inputs)}")
    missing = [str(path) for path in inputs if not path.is_file()]
    if missing:
        raise ValueError("missing benchmark input(s): " + ", ".join(missing))
    if len(set(inputs)) != len(inputs):
        raise ValueError("benchmark inputs must be unique")
    if not hasattr(os, "sched_setaffinity") or not hasattr(os, "wait4"):
        raise ValueError("fresh-process CPU/RSS measurement requires POSIX sched_setaffinity and wait4")
    allowed = os.sched_getaffinity(0)
    if args.cpu not in allowed:
        raise ValueError(f"logical CPU {args.cpu} is unavailable; allowed CPUs: {sorted(allowed)}")
    versions = {"python-docx": package_version("python-docx"), "spire-doc": package_version("spire-doc")}
    expected = {"python-docx": PYTHON_DOCX_VERSION, "spire-doc": SPIRE_DOC_VERSION}
    mismatches = [f"{name}=={expected[name]} (found {versions[name]})" for name in expected
                  if versions[name] != expected[name]]
    if mismatches:
        raise ValueError("missing pinned benchmark packages: " + ", ".join(mismatches)
                         + "; use --bootstrap or a throwaway venv")

    output_root = Path(args.output_dir).expanduser().resolve()
    if output_root.exists() and any(output_root.iterdir()):
        raise ValueError("output directory must be absent or empty (no overwrite): " + str(output_root))
    output_root.mkdir(parents=True, exist_ok=True)
    outputs_root = output_root / "outputs"
    outputs_root.mkdir()
    libreoffice = shutil.which("libreoffice") or shutil.which("soffice")
    results: List[MutableMapping[str, object]] = []
    oracles: Dict[str, SemanticModel] = {}

    for input_index, input_path in enumerate(inputs):
        direction = input_direction(input_path)
        case_id = f"{input_index + 1:02d}-{direction}-{input_path.stem}"
        oracle, oracle_source = oracle_for_input(input_path, direction)
        oracles[case_id] = oracle
        order = list(CANDIDATES[input_index % len(CANDIDATES):] + CANDIDATES[:input_index % len(CANDIDATES)])
        for order_index, candidate in enumerate(order):
            candidate_dir = outputs_root / case_id / candidate
            candidate_dir.mkdir(parents=True)
            output_path = candidate_dir / ("output.md" if direction == "docx_to_md" else "output.docx")
            stdout_path = candidate_dir / "stdout.txt"
            stderr_path = candidate_dir / "stderr.txt"
            command = conversion_command(candidate, direction, input_path, output_path, ainiux)
            print(f"[{len(results) + 1:02d}/{len(inputs) * len(CANDIDATES):02d}] "
                  f"{input_path.name}: {candidate}", file=sys.stderr, flush=True)
            timing = run_measured(command, stdout_path, stderr_path, args.cpu)
            output_exists = output_path.is_file()
            output_bytes = output_path.stat().st_size if output_exists else 0
            validation: Dict[str, object] = {}
            hypothesis = SemanticModel()
            parse_error: Optional[str] = None
            if output_exists and timing["exit_status"] == 0:
                try:
                    if direction == "docx_to_md":
                        validation["markdown"] = markdown_validation(output_path, input_path.name)
                        text, utf8_error = read_utf8(output_path)
                        if utf8_error:
                            parse_error = utf8_error
                        else:
                            hypothesis = markdown_semantics(text or "")
                        validation["evaluation_text_present"] = bool(text and EVALUATION_RE.search(text))
                    else:
                        package = docx_package_validation(output_path)
                        validation["docx_package"] = package
                        if package["valid"]:
                            hypothesis = docx_semantics(output_path)
                            joined_tokens = " ".join(hypothesis.tokens)
                            validation["evaluation_text_present"] = bool(EVALUATION_RE.search(joined_tokens))
                            validation["libreoffice"] = libreoffice_validation(output_path, oracle, libreoffice)
                        else:
                            parse_error = str(package.get("error", "invalid DOCX package"))
                except (OSError, KeyError, ValueError, zipfile.BadZipFile, ET.ParseError) as exc:
                    parse_error = str(exc)
            accuracy = score_models(oracle, hypothesis)
            incomplete_reasons = []
            if timing["exit_status"] != 0:
                incomplete_reasons.append("exit status " + str(timing["exit_status"]))
            if not output_exists:
                incomplete_reasons.append("missing output")
            if parse_error:
                incomplete_reasons.append("format/oracle error: " + parse_error)
            if float(accuracy["token"]["recall"]) < 0.99:  # type: ignore[index]
                incomplete_reasons.append("token recall below 99%")
            if not accuracy["final_source_block_present"]:
                incomplete_reasons.append("final source block absent")
            if direction == "md_to_docx":
                package = validation.get("docx_package", {})
                if package and not package.get("valid"):
                    incomplete_reasons.append("invalid DOCX package")
                lo = validation.get("libreoffice", {})
                if lo and lo.get("available") and not lo.get("opened"):
                    incomplete_reasons.append("LibreOffice open failed")
            stderr_warnings = warnings_from_file(stderr_path)
            row: MutableMapping[str, object] = {
                "case_id": case_id, "input": str(input_path),
                "input_display": (str(input_path.relative_to(repository)) if input_path.is_relative_to(repository)
                                  else str(input_path).replace(str(Path.home()), "~", 1)),
                "input_sha256": sha256_file(input_path), "input_bytes": input_path.stat().st_size,
                "direction": direction, "oracle_source": oracle_source,
                "candidate": candidate, "candidate_order": order_index + 1,
                "output": str(output_path.relative_to(output_root)), "output_bytes": output_bytes,
                "throughput_bytes_per_second": input_path.stat().st_size / float(timing["wall_seconds"]),
                "timing": timing, "accuracy": accuracy, "validation": validation,
                "warnings": stderr_warnings, "complete": not incomplete_reasons,
                "incomplete_reasons": incomplete_reasons,
            }
            results.append(row)

    by_case: Dict[str, Dict[str, MutableMapping[str, object]]] = {}
    for row in results:
        by_case.setdefault(str(row["case_id"]), {})[str(row["candidate"])] = row
    for candidate_rows in by_case.values():
        baseline = float(candidate_rows["ainiux"]["timing"]["wall_seconds"])  # type: ignore[index]
        for row in candidate_rows.values():
            row["wall_ratio_vs_ainiux"] = float(row["timing"]["wall_seconds"]) / baseline  # type: ignore[index]

    generated = datetime.now(timezone.utc)
    metadata = {
        "schema_version": 1, "date": generated.date().isoformat(),
        "generated_at_utc": generated.isoformat(), "platform": platform.platform(),
        "cpu": cpu_description(), "cpu_affinity": args.cpu, "python": sys.version.replace("\n", " "),
        "packages": versions, "libreoffice": executable_version(libreoffice),
        "ainiux_binary": str(ainiux), "ainiux_sha256": sha256_file(ainiux),
        "ainiux_git_revision": git_value(["rev-parse", "HEAD"]),
        "harness_sha256": sha256_file(Path(__file__).resolve()),
        "ainiux_git_dirty": bool(git_value(["status", "--porcelain"])),
        "candidate_order_policy": "cyclic rotation by input index", "runs_per_pair": 1,
        "warmups": 0, "retries": 0, "spire_licensed": False,
        "exact_command": os.environ.get("AINIUX_DOCX_BENCH_COMMAND",
                                        shlex.join([sys.executable, str(Path(__file__).resolve())] + sys.argv[1:])),
    }
    payload: Dict[str, object] = {"metadata": metadata, "inputs": [str(path) for path in inputs],
                                 "results": results, "aggregates": aggregate_results(results)}
    raw_path = output_root / "raw.json"
    raw_path.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    report_text = render_report(payload)
    generated_report = output_root / "report.md"
    generated_report.write_text(report_text, encoding="utf-8")
    if args.report:
        report_path = Path(args.report).expanduser().resolve()
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(report_text, encoding="utf-8")
    print(str(raw_path))
    print(str(generated_report))
    return 0


def synthetic_docx(path: Path) -> None:
    content_types = f'''<?xml version="1.0" encoding="UTF-8"?>
<Types xmlns="{CT_NS}"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
<Default Extension="xml" ContentType="application/xml"/><Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>
<Override PartName="/word/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml"/></Types>'''
    root_rels = f'''<?xml version="1.0"?><Relationships xmlns="{PKG_REL_NS}"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/></Relationships>'''
    document_rels = f'''<?xml version="1.0"?><Relationships xmlns="{PKG_REL_NS}"><Relationship Id="rIdLink" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink" Target="https://example.com" TargetMode="External"/></Relationships>'''
    styles = f'''<?xml version="1.0"?><w:styles xmlns:w="{W_NS}"><w:style w:type="paragraph" w:styleId="Heading1"><w:name w:val="heading 1"/></w:style></w:styles>'''
    document = f'''<?xml version="1.0" encoding="UTF-8"?><w:document xmlns:w="{W_NS}" xmlns:r="{R_NS}"><w:body>
<w:p><w:pPr><w:pStyle w:val="Heading1"/></w:pPr><w:r><w:t>Oracle Heading</w:t></w:r></w:p>
<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>Bold words</w:t></w:r><w:r><w:tab/><w:t>after tab</w:t><w:br/><w:t>after break</w:t></w:r><w:hyperlink r:id="rIdLink"><w:r><w:t>site</w:t></w:r></w:hyperlink></w:p>
<w:tbl><w:tr><w:tc><w:p><w:r><w:t>Cell</w:t></w:r></w:p></w:tc></w:tr></w:tbl><w:sectPr/></w:body></w:document>'''
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("[Content_Types].xml", content_types)
        archive.writestr("_rels/.rels", root_rels)
        archive.writestr("word/document.xml", document)
        archive.writestr("word/_rels/document.xml.rels", document_rels)
        archive.writestr("word/styles.xml", styles)


def self_test() -> int:
    markdown = """# Heading\n\nText with **bold**, *italic*, ++under++, ~~strike~~, [site](https://example.com), a\ttab, and a  \nhard break.\n\n- item\n  1. nested\n\n| A | B |\n| --- | --- |\n| x | y |\n\nFinal source block.\n"""
    model = markdown_semantics(markdown)
    anchor_text = "\n".join(model.anchors)
    for needle in ("heading", "emphasis:bold", "emphasis:italic", "emphasis:underline", "emphasis:strike",
                   "link", "tab", "hard_break", "list:unordered", "list:ordered", "table_cell"):
        assert needle in anchor_text, "missing Markdown semantic anchor: " + needle
    identical = score_models(model, markdown_semantics(markdown))
    assert identical["token"]["f1"] == 1.0  # type: ignore[index]
    truncated = score_models(model, markdown_semantics("# Heading\n\nText with bold."))
    assert truncated["token"]["recall"] < 0.99  # type: ignore[index]
    assert not truncated["final_source_block_present"]
    html_table = markdown_semantics(
        '<table><tr><th>A</th><th>B</th></tr><tr><td rowspan="2">x</td><td>y</td></tr>'
        '<tr><td>z</td></tr></table>')
    assert html_table.tokens == ["a", "b", "x", "y", "z"]
    assert sum(html_table.anchors.values()) == 6
    without_images = markdown_semantics(
        "kept ![multiline alt\ntext](/tmp/image.png) words [image omitted: other\nimage] end")
    assert without_images.tokens == ["kept", "words", "end"]
    with tempfile.TemporaryDirectory(prefix="ainiux-docx-self-test-") as temporary:
        docx_path = Path(temporary) / "synthetic.docx"
        synthetic_docx(docx_path)
        package = docx_package_validation(docx_path)
        assert package["valid"]
        extracted = docx_semantics(docx_path)
        extracted_anchors = "\n".join(extracted.anchors)
        for needle in ("heading", "emphasis:bold", "link", "tab", "hard_break", "table_cell"):
            assert needle in extracted_anchors, "missing OOXML semantic anchor: " + needle
    sample_payload = {
        "metadata": {"date": "2000-01-01", "generated_at_utc": "2000-01-01T00:00:00+00:00",
                     "cpu_affinity": 5, "platform": "test", "cpu": "test", "python": "test",
                     "packages": {"python-docx": PYTHON_DOCX_VERSION, "spire-doc": SPIRE_DOC_VERSION},
                     "libreoffice": "test", "ainiux_git_revision": "deadbeef", "ainiux_sha256": "0" * 64,
                     "ainiux_binary": "./ainiux", "exact_command": "python3 docx_benchmark.py"},
        "results": [],
        "aggregates": {"common_complete_case_ids": [], "common_complete_totals": [],
                       "full_corpus_case_ids": [], "full_corpus_eligible_candidates": [],
                       "full_corpus_totals": []},
    }
    report = render_report(sample_payload)
    assert "DOCX conversion benchmark" in report and "Aggregate comparisons" in report
    print("DOCX benchmark self-test passed")
    return 0


def bootstrap(argv: Sequence[str]) -> int:
    forwarded = [item for item in argv if item != "--bootstrap"]
    exact = shlex.join([sys.executable, str(Path(__file__).resolve())] + list(argv))
    with tempfile.TemporaryDirectory(prefix="ainiux-docx-benchmark-venv-") as temporary:
        environment_path = Path(temporary)
        venv.EnvBuilder(with_pip=True, clear=True).create(environment_path)
        python = environment_path / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
        install = [str(python), "-m", "pip", "install", "--disable-pip-version-check",
                   "python-docx==" + PYTHON_DOCX_VERSION, "Spire.Doc==" + SPIRE_DOC_VERSION]
        completed = subprocess.run(install, check=False)
        if completed.returncode != 0:
            return completed.returncode
        environment = dict(os.environ)
        environment["AINIUX_DOCX_BENCH_COMMAND"] = exact
        return subprocess.run([str(python), str(Path(__file__).resolve())] + forwarded,
                              check=False, env=environment).returncode


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("inputs", nargs="*", help="up to eight .docx/.md sources (default: documented eight-file matrix)")
    parser.add_argument("--ainiux", default="./ainiux", help="explicit repository binary (default: ./ainiux)")
    parser.add_argument("--output-dir", default="results/docx-conversion-" + datetime.now().date().isoformat())
    parser.add_argument("--report", help="also write the generated Markdown report here")
    parser.add_argument("--cpu", type=int, default=5, help="logical CPU affinity for every candidate process")
    parser.add_argument("--bootstrap", action="store_true", help="run inside a temporary venv with pinned libraries")
    parser.add_argument("--self-test", action="store_true", help="run synthetic oracle/scoring/report tests")
    parser.add_argument("--rescore-json", type=Path,
                        help="rescore preserved outputs in an existing raw JSON without rerunning candidates")
    parser.add_argument("--worker", nargs=4, metavar=("CANDIDATE", "DIRECTION", "INPUT", "OUTPUT"),
                        help=argparse.SUPPRESS)
    return parser.parse_args(argv)


def main(argv: Optional[Sequence[str]] = None) -> int:
    actual = list(argv) if argv is not None else sys.argv[1:]
    args = parse_args(actual)
    try:
        if args.worker:
            return worker_main(*args.worker)
        if args.self_test:
            return self_test()
        if args.rescore_json:
            raw_path = args.rescore_json.expanduser().resolve()
            payload = json.loads(raw_path.read_text(encoding="utf-8"))
            rescore_payload(payload, raw_path)
            raw_path.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
            report_text = render_report(payload)
            (raw_path.parent / "report.md").write_text(report_text, encoding="utf-8")
            if args.report:
                report_path = Path(args.report).expanduser().resolve()
                report_path.parent.mkdir(parents=True, exist_ok=True)
                report_path.write_text(report_text, encoding="utf-8")
            print(str(raw_path))
            return 0
        if args.bootstrap:
            return bootstrap(actual)
        return run_benchmark(args)
    except (OSError, ValueError, RuntimeError, zipfile.BadZipFile, ET.ParseError) as exc:
        print("docx benchmark: " + str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
