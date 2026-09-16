#!/usr/bin/env python3
"""Generate deterministic dependency-free PPTX unit fixtures.

The script deliberately uses only the Python standard library.  It produces one
small feature package and malformed variants that exercise the shared OOXML ZIP
and XML limits.  The three large interoperability decks in this directory are
supplied separately and are never changed by this script.
"""

from __future__ import annotations

import argparse
import base64
import pathlib
import struct
import warnings
import zipfile


ROOT = pathlib.Path(__file__).resolve().parent
STAMP = (1980, 1, 1, 0, 0, 0)
P = "http://schemas.openxmlformats.org/presentationml/2006/main"
A = "http://schemas.openxmlformats.org/drawingml/2006/main"
R = "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
PKG_R = "http://schemas.openxmlformats.org/package/2006/relationships"


def relationship_xml(items: list[tuple[str, str, str, bool]]) -> str:
    rows = []
    for rid, kind, target, external in items:
        mode = ' TargetMode="External"' if external else ""
        rows.append(f'<Relationship Id="{rid}" Type="{kind}" Target="{target}"{mode}/>')
    return f'<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="{PKG_R}">' + "".join(rows) + "</Relationships>"


def shape(shape_id: int, name: str, paragraphs: str, placeholder: str = "") -> str:
    ph = f'<p:ph type="{placeholder}"/>' if placeholder else ""
    return (
        f'<p:sp><p:nvSpPr><p:cNvPr id="{shape_id}" name="{name}"/><p:cNvSpPr/>'
        f'<p:nvPr>{ph}</p:nvPr></p:nvSpPr><p:spPr/><p:txBody><a:bodyPr/>'
        f'<a:lstStyle/>{paragraphs}</p:txBody></p:sp>'
    )


def paragraph(text: str, props: str = "", run_props: str = "") -> str:
    return f'<a:p>{props}<a:r><a:rPr lang="en-US"{run_props}/><a:t>{text}</a:t></a:r><a:endParaRPr/></a:p>'


def slide_xml(body: str) -> str:
    return (
        f'<?xml version="1.0" encoding="UTF-8"?><p:sld xmlns:a="{A}" xmlns:r="{R}" xmlns:p="{P}">'
        f'<p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/>'
        f'</p:nvGrpSpPr><p:grpSpPr/>{body}</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/>'
        f'</p:clrMapOvr></p:sld>'
    )


def picture(shape_id: int, name: str, description: str, relationship_id: str) -> str:
    return (
        f'<p:pic><p:nvPicPr><p:cNvPr id="{shape_id}" name="{name}" descr="{description}"/>'
        '<p:cNvPicPr/><p:nvPr/></p:nvPicPr><p:blipFill>'
        f'<a:blip r:embed="{relationship_id}"/>'
        '<a:stretch><a:fillRect/></a:stretch></p:blipFill><p:spPr/></p:pic>'
    )


def table_xml() -> str:
    def cell(text: str, continuation: bool = False) -> str:
        merge = ' hMerge="1"' if continuation else ""
        return f'<a:tc{merge}><a:txBody><a:bodyPr/><a:lstStyle/>{paragraph(text)}</a:txBody><a:tcPr/></a:tc>'
    return (
        '<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id="8" name="Table"/>'
        '<p:cNvGraphicFramePr/><p:nvPr/></p:nvGraphicFramePr><p:xfrm/>'
        f'<a:graphic><a:graphicData uri="{A}/table"><a:tbl><a:tblPr firstRow="1"/>'
        '<a:tblGrid><a:gridCol w="3000000"/><a:gridCol w="3000000"/></a:tblGrid>'
        f'<a:tr h="400000">{cell("Name")}{cell("Value")}</a:tr>'
        f'<a:tr h="400000">{cell("café")}{cell("", True)}</a:tr>'
        '</a:tbl></a:graphicData></a:graphic></p:graphicFrame>'
    )


def feature_parts() -> dict[str, bytes | str]:
    root_rel_type = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument"
    slide_rel_type = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide"
    image_rel_type = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/image"
    notes_rel_type = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/notesSlide"
    hyperlink_type = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink"
    types = (
        '<?xml version="1.0" encoding="UTF-8"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
        '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
        '<Default Extension="xml" ContentType="application/xml"/>'
        '<Default Extension="png" ContentType="image/png"/>'
        '<Default Extension="jpg" ContentType="image/jpeg"/>'
        '<Default Extension="gif" ContentType="image/gif"/>'
        '<Override PartName="/ppt/presentation.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml"/>'
        '<Override PartName="/ppt/slides/slide1.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.slide+xml"/>'
        '<Override PartName="/ppt/slides/slide2.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.slide+xml"/>'
        '<Override PartName="/ppt/notesSlides/notesSlide2.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.notesSlide+xml"/>'
        '</Types>'
    )
    title = shape(2, "Title", paragraph("简体中文 繁體中文 العربية עברית Русский é 😀"), "title")
    rich = (
        '<a:p><a:r><a:rPr b="1"/><a:t>Bold</a:t></a:r><a:r><a:rPr i="1"/>'
        '<a:t> italic</a:t></a:r><a:r><a:rPr u="sng" strike="sngStrike"/>'
        '<a:t> underline strike</a:t></a:r><a:r><a:rPr><a:hlinkClick r:id="rIdLink"/>'
        '</a:rPr><a:t> link</a:t></a:r><a:br/><a:r><a:t>hard break</a:t></a:r></a:p>'
        '<a:p><a:pPr lvl="0"><a:buChar char="•"/></a:pPr><a:r><a:t>bullet</a:t></a:r></a:p>'
        '<a:p><a:pPr lvl="1"><a:buAutoNum type="arabicPeriod" startAt="3"/></a:pPr>'
        '<a:r><a:t>nested ordered</a:t></a:r></a:p>'
    )
    body = shape(3, "Content", rich)
    pictures = (
        picture(4, "PNG pixel", "fixture PNG", "rIdPng")
        + picture(5, "JPEG pixel", "fixture JPEG", "rIdJpeg")
        + picture(6, "GIF pixel", "fixture GIF", "rIdGif")
    )
    slide1 = slide_xml(title + body + pictures)
    slide2 = slide_xml(shape(2, "Title", paragraph("Second slide"), "title") + table_xml())
    presentation = (
        f'<?xml version="1.0" encoding="UTF-8"?><p:presentation xmlns:p="{P}" xmlns:r="{R}">'
        '<p:sldIdLst><p:sldId id="300" r:id="rId2"/><p:sldId id="256" r:id="rId1"/></p:sldIdLst>'
        '<p:sldSz cx="12192000" cy="6858000"/></p:presentation>'
    )
    notes = slide_xml(shape(2, "Notes", paragraph("Remember the demonstration."), "body"))
    png = base64.b64decode(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="
    )
    jpeg = base64.b64decode(
        "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAP//////////////////////////////////////////"
        "////////////////////////////////////////////2wBDAf//////////////////////////////"
        "////////////////////////////////////////////////////wAARCAABAAEDASIAAhEBAxEB/8QA"
        "FQABAQAAAAAAAAAAAAAAAAAAAAX/xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oADAMBAAIQAxAAAAF/"
        "/8QAFBABAAAAAAAAAAAAAAAAAAAAAP/aAAgBAQABBQJ//8QAFBEBAAAAAAAAAAAAAAAAAAAAAP/a"
        "AAgBAwEBPwF//8QAFBEBAAAAAAAAAAAAAAAAAAAAAP/aAAgBAgEBPwF//8QAFBABAAAAAAAAAAAA"
        "AAAAAAAAAP/aAAgBAQAGPwJ//8QAFBABAAAAAAAAAAAAAAAAAAAAAP/aAAgBAQABPyF//9oADAMB"
        "AAIAAwAAABD/xAAUEQEAAAAAAAAAAAAAAAAAAAAA/9oACAEDAQE/EH//xAAUEQEAAAAAAAAAAAAA"
        "AAAAAAAA/9oACAECAQE/EH//xAAUEAEAAAAAAAAAAAAAAAAAAAAA/9oACAEBAAE/EH//2Q=="
    )
    gif = base64.b64decode("R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw==")
    return {
        "[Content_Types].xml": types,
        "_rels/.rels": relationship_xml([("rId1", root_rel_type, "ppt/presentation.xml", False)]),
        "ppt/presentation.xml": presentation,
        "ppt/_rels/presentation.xml.rels": relationship_xml([
            ("rId1", slide_rel_type, "slides/slide1.xml", False),
            ("rId2", slide_rel_type, "slides/slide2.xml", False),
        ]),
        "ppt/slides/slide1.xml": slide1,
        "ppt/slides/_rels/slide1.xml.rels": relationship_xml([
            ("rIdPng", image_rel_type, "../media/pixel.png", False),
            ("rIdJpeg", image_rel_type, "../media/pixel.jpg", False),
            ("rIdGif", image_rel_type, "../media/pixel.gif", False),
            ("rIdLink", hyperlink_type, "https://example.com/pptx", True),
        ]),
        "ppt/slides/slide2.xml": slide2,
        "ppt/slides/_rels/slide2.xml.rels": relationship_xml([
            ("rIdNotes", notes_rel_type, "../notesSlides/notesSlide2.xml", False),
        ]),
        "ppt/notesSlides/notesSlide2.xml": notes,
        "ppt/notesSlides/_rels/notesSlide2.xml.rels": relationship_xml([]),
        "ppt/media/pixel.png": png,
        "ppt/media/pixel.jpg": jpeg,
        "ppt/media/pixel.gif": gif,
    }


def package(parts: dict[str, bytes | str], duplicate: str = "") -> bytes:
    from io import BytesIO
    output = BytesIO()
    with zipfile.ZipFile(output, "w", allowZip64=True) as archive:
        for name in sorted(parts):
            info = zipfile.ZipInfo(name, STAMP)
            info.compress_type = zipfile.ZIP_STORED if name.endswith((".png", ".jpg", ".gif")) else zipfile.ZIP_DEFLATED
            info.external_attr = 0o100600 << 16
            payload = parts[name].encode("utf-8") if isinstance(parts[name], str) else parts[name]
            archive.writestr(info, payload, compresslevel=6)
        if duplicate:
            info = zipfile.ZipInfo(duplicate, STAMP)
            info.compress_type = zipfile.ZIP_STORED
            with warnings.catch_warnings():
                warnings.simplefilter("ignore", UserWarning)
                archive.writestr(info, b"duplicate")
    return output.getvalue()


def patch_first_headers(data: bytes, *, flags: int | None = None,
                        method: int | None = None, crc: int | None = None,
                        expanded: int | None = None) -> bytes:
    out = bytearray(data)
    local = out.find(b"PK\x03\x04")
    central = out.find(b"PK\x01\x02")
    if local < 0 or central < 0:
        raise RuntimeError("generated ZIP has no headers")
    if flags is not None:
        struct.pack_into("<H", out, local + 6, flags)
        struct.pack_into("<H", out, central + 8, flags)
    if method is not None:
        struct.pack_into("<H", out, local + 8, method)
        struct.pack_into("<H", out, central + 10, method)
    if crc is not None:
        struct.pack_into("<I", out, local + 14, crc)
        struct.pack_into("<I", out, central + 16, crc)
    if expanded is not None:
        struct.pack_into("<I", out, local + 22, expanded)
        struct.pack_into("<I", out, central + 24, expanded)
    return bytes(out)


def fixtures() -> dict[str, bytes]:
    parts = feature_parts()
    valid = package(parts)
    missing_root = dict(parts)
    del missing_root["_rels/.rels"]
    missing_main = dict(parts)
    missing_main["_rels/.rels"] = relationship_xml([])
    missing_slide = dict(parts)
    del missing_slide["ppt/slides/slide2.xml"]
    missing_image = dict(parts)
    del missing_image["ppt/media/pixel.png"]
    malformed = dict(parts)
    malformed["ppt/slides/slide1.xml"] = "<p:sld><broken></p:sld>"
    deep = dict(parts)
    deep["ppt/slides/slide1.xml"] = slide_xml("<a:x>" * 300 + "deep" + "</a:x>" * 300)
    unsafe = dict(parts)
    unsafe["../escape.xml"] = "escape"
    return {
        "features.pptx": valid,
        "truncated.pptx": valid[:-20],
        "missing-root.pptx": package(missing_root),
        "missing-main-relationship.pptx": package(missing_main),
        "missing-slide-target.pptx": package(missing_slide),
        "missing-image-target.pptx": package(missing_image),
        "malformed-xml.pptx": package(malformed),
        "deep-xml.pptx": package(deep),
        "unsafe-traversal.pptx": package(unsafe),
        "duplicate-members.pptx": package(parts, "ppt/slides/slide1.xml"),
        "bad-crc.pptx": patch_first_headers(valid, crc=0),
        "encrypted-flag.pptx": patch_first_headers(valid, flags=1),
        "unsupported-compression.pptx": patch_first_headers(valid, method=99),
        "expansion-bomb-metadata.pptx": patch_first_headers(valid, expanded=0x7FFFFFFF),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="validate existing fixture names and ZIP readability")
    parser.add_argument("--output-dir", type=pathlib.Path, default=ROOT)
    args = parser.parse_args()
    generated = fixtures()
    if args.check:
        missing = [name for name in generated if not (args.output_dir / name).is_file()]
        if missing:
            raise SystemExit("missing generated PPTX fixtures: " + ", ".join(missing))
        stale = [name for name, data in generated.items()
                 if (args.output_dir / name).read_bytes() != data]
        if stale:
            raise SystemExit("stale generated PPTX fixtures: " + ", ".join(stale))
        with zipfile.ZipFile(args.output_dir / "features.pptx") as archive:
            required = {"_rels/.rels", "ppt/presentation.xml", "ppt/slides/slide1.xml",
                        "ppt/media/pixel.png", "ppt/media/pixel.jpg", "ppt/media/pixel.gif"}
            if not required.issubset(archive.namelist()):
                raise SystemExit("features.pptx is missing required OPC parts")
        print("PPTX fixture check passed")
        return 0
    args.output_dir.mkdir(parents=True, exist_ok=True)
    for name, data in generated.items():
        (args.output_dir / name).write_bytes(data)
    print(f"wrote {len(generated)} PPTX fixtures to {args.output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
