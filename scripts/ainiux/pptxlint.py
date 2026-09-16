#!/usr/bin/env python3
"""Small dependency-free structural validator for generated PPTX packages."""

from __future__ import annotations

import argparse
import io
import posixpath
import sys
import zipfile
import xml.etree.ElementTree as ET

PKG_REL = "http://schemas.openxmlformats.org/package/2006/relationships"
OFFICE_REL_SUFFIX = "/officeDocument"
CONTENT_TYPES = "http://schemas.openxmlformats.org/package/2006/content-types"


def qname(uri: str, local: str) -> str:
    return "{%s}%s" % (uri, local)


def relationship_part(source: str) -> str:
    directory, name = posixpath.split(source)
    prefix = directory + "/" if directory else ""
    return prefix + "_rels/" + name + ".rels"


def resolve_target(source: str, target: str) -> str | None:
    if "\\" in target or "\x00" in target:
        return None
    if target.startswith("/"):
        candidate = target[1:]
    else:
        candidate = posixpath.join(posixpath.dirname(source), target)
    normalized = posixpath.normpath(candidate)
    if normalized in ("", ".", "..") or normalized.startswith("../") or normalized.startswith("/"):
        return None
    return normalized


def parse_xml(blob: bytes, name: str, errors: list[str]) -> ET.Element | None:
    try:
        return ET.fromstring(blob)
    except (ET.ParseError, ValueError) as exc:
        errors.append("malformed XML in %s: %s" % (name, exc))
        return None


def lint_bytes(blob: bytes, label: str = "<memory>") -> list[str]:
    errors: list[str] = []
    try:
        archive = zipfile.ZipFile(io.BytesIO(blob))
    except (zipfile.BadZipFile, ValueError) as exc:
        return ["invalid ZIP: %s" % exc]
    with archive:
        infos = archive.infolist()
        names = [item.filename for item in infos]
        members = set(names)
        if len(names) != len(members):
            errors.append("duplicate ZIP member name")
        for info in infos:
            name = info.filename
            checked_name = name[:-1] if name.endswith("/") else name
            if (not checked_name or checked_name.startswith(("/", "\\")) or "\\" in checked_name or
                    any(part in ("", ".", "..") for part in checked_name.split("/"))):
                errors.append("unsafe ZIP member: %s" % name)
            if info.flag_bits & (1 | 0x40 | 0x2000):
                errors.append("encrypted ZIP member: %s" % name)
            if info.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
                errors.append("unsupported compression for %s" % name)
        required = {"[Content_Types].xml", "_rels/.rels"}
        for name in sorted(required - members):
            errors.append("missing required part: %s" % name)
        try:
            bad = archive.testzip()
            if bad:
                errors.append("CRC/inflate failure: %s" % bad)
        except (zipfile.BadZipFile, RuntimeError, NotImplementedError) as exc:
            errors.append("ZIP integrity failure: %s" % exc)

        roots: dict[str, ET.Element] = {}
        for name in names:
            if name.endswith((".xml", ".rels")) or name == "[Content_Types].xml":
                try:
                    root = parse_xml(archive.read(name), name, errors)
                except (KeyError, OSError, RuntimeError, zipfile.BadZipFile) as exc:
                    errors.append("could not read %s: %s" % (name, exc))
                    continue
                if root is not None:
                    roots[name] = root

        defaults: dict[str, str] = {}
        overrides: dict[str, str] = {}
        types = roots.get("[Content_Types].xml")
        if types is not None:
            for child in types:
                local = child.tag.rsplit("}", 1)[-1]
                if local == "Default":
                    defaults[child.attrib.get("Extension", "").lower()] = child.attrib.get("ContentType", "")
                elif local == "Override":
                    part = child.attrib.get("PartName", "")
                    overrides[part] = child.attrib.get("ContentType", "")
                    if not part.startswith("/") or part[1:] not in members:
                        errors.append("content-type override targets a missing part: %s" % part)
            for name in names:
                if name == "[Content_Types].xml" or name.endswith("/"):
                    continue
                extension = name.rsplit(".", 1)[-1].lower() if "." in name else ""
                if "/" + name not in overrides and extension not in defaults:
                    errors.append("part has no content type: %s" % name)

        main_parts: list[str] = []
        for rel_name, root in roots.items():
            if not rel_name.endswith(".rels"):
                continue
            source = ""
            if rel_name != "_rels/.rels":
                directory, filename = posixpath.split(rel_name)
                if not directory.endswith("/_rels") or not filename.endswith(".rels"):
                    errors.append("misplaced relationship part: %s" % rel_name)
                    continue
                source = posixpath.join(directory[:-6], filename[:-5]).lstrip("/")
            seen_ids: set[str] = set()
            for rel in root:
                if rel.tag.rsplit("}", 1)[-1] != "Relationship":
                    continue
                rid = rel.attrib.get("Id", "")
                target = rel.attrib.get("Target", "")
                rel_type = rel.attrib.get("Type", "")
                if not rid or rid in seen_ids:
                    errors.append("missing or duplicate relationship Id in %s" % rel_name)
                seen_ids.add(rid)
                if rel.attrib.get("TargetMode", "").lower() == "external":
                    continue
                resolved = resolve_target(source, target)
                if resolved is None:
                    errors.append("unsafe relationship target in %s: %s" % (rel_name, target))
                elif resolved not in members:
                    errors.append("relationship target is missing: %s -> %s" % (rel_name, resolved))
                elif rel_name == "_rels/.rels" and rel_type.endswith(OFFICE_REL_SUFFIX):
                    main_parts.append(resolved)
        if len(main_parts) != 1:
            errors.append("package must have exactly one officeDocument relationship")
        elif main_parts[0] != "ppt/presentation.xml":
            errors.append("officeDocument is not a PPTX presentation: %s" % main_parts[0])

        for name, root in roots.items():
            if not name.startswith("ppt/slides/slide") or not name.endswith(".xml"):
                continue
            shape_ids: set[str] = set()
            for element in root.iter():
                if element.tag.rsplit("}", 1)[-1] != "cNvPr":
                    continue
                shape_id = element.attrib.get("id", "")
                if not shape_id or shape_id in shape_ids:
                    errors.append("missing or duplicate shape id in %s" % name)
                shape_ids.add(shape_id)
        presentation = roots.get("ppt/presentation.xml")
        if presentation is not None:
            slide_ids: set[str] = set()
            for element in presentation.iter():
                if element.tag.rsplit("}", 1)[-1] != "sldId":
                    continue
                value = element.attrib.get("id", "")
                if not value or value in slide_ids:
                    errors.append("missing or duplicate presentation slide id")
                slide_ids.add(value)
            if not slide_ids:
                errors.append("presentation contains no slides")
    return errors


def selftest() -> bool:
    parts = {
        "[Content_Types].xml": (
            '<Types xmlns="%s"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
            '<Default Extension="xml" ContentType="application/xml"/></Types>' % CONTENT_TYPES
        ),
        "_rels/.rels": (
            '<Relationships xmlns="%s"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/'
            'officeDocument/2006/relationships/officeDocument" Target="ppt/presentation.xml"/></Relationships>' % PKG_REL
        ),
        "ppt/presentation.xml": (
            '<p:presentation xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main" '
            'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">'
            '<p:sldIdLst><p:sldId id="256" r:id="rId1"/></p:sldIdLst></p:presentation>'
        ),
        "ppt/_rels/presentation.xml.rels": (
            '<Relationships xmlns="%s"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/'
            'officeDocument/2006/relationships/slide" Target="slides/slide1.xml"/></Relationships>' % PKG_REL
        ),
        "ppt/slides/slide1.xml": (
            '<p:sld xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">'
            '<p:cNvPr id="1" name="root"/></p:sld>'
        ),
    }
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, value in parts.items():
            archive.writestr(name, value)
    if lint_bytes(output.getvalue(), "selftest-valid"):
        return False
    broken = io.BytesIO()
    with zipfile.ZipFile(broken, "w") as archive:
        archive.writestr("[Content_Types].xml", parts["[Content_Types].xml"])
    return bool(lint_bytes(broken.getvalue(), "selftest-broken"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="*", help="PPTX packages to validate")
    parser.add_argument("--selftest", action="store_true", help="run validator self-tests")
    args = parser.parse_args()
    failed = False
    if args.selftest:
        if not selftest():
            print("pptxlint self-test failed", file=sys.stderr)
            failed = True
        else:
            print("pptxlint self-test passed")
    if not args.files and not args.selftest:
        parser.error("provide at least one PPTX file or --selftest")
    for path in args.files:
        try:
            with open(path, "rb") as source:
                errors = lint_bytes(source.read(), path)
        except OSError as exc:
            errors = ["could not read file: %s" % exc]
        if errors:
            failed = True
            print("%s:" % path, file=sys.stderr)
            for error in errors:
                print("  error: %s" % error, file=sys.stderr)
        else:
            print("%s: ok" % path)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
