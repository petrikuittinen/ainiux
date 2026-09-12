#!/usr/bin/env python3
"""Regenerate hand-authored DOCX fixtures using only the Python standard library."""

from __future__ import annotations

import io
import pathlib
import struct
import warnings
import zipfile


ROOT = pathlib.Path(__file__).resolve().parent
FIXED_TIME = (2000, 1, 1, 0, 0, 0)
CONTENT_TYPES = (
    '<?xml version="1.0"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
    '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
    '<Default Extension="xml" ContentType="application/xml"/>'
    '<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>'
    '</Types>'
).encode()
ROOT_RELS = (
    '<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
    '<Relationship Id="rId1" Type="http://purl.oclc.org/ooxml/officeDocument/relationships/officeDocument" Target="word/document.xml"/>'
    '</Relationships>'
).encode()


class Unseekable(io.RawIOBase):
    def __init__(self) -> None:
        self.buffer = bytearray()

    def writable(self) -> bool:
        return True

    def seekable(self) -> bool:
        return False

    def write(self, data: bytes) -> int:
        self.buffer.extend(data)
        return len(data)

    def tell(self) -> int:
        return len(self.buffer)


def archive(entries: list[tuple[str, bytes, int]], descriptors: bool = False) -> bytes:
    target: io.BytesIO | Unseekable = Unseekable() if descriptors else io.BytesIO()
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", UserWarning)
        with zipfile.ZipFile(target, "w", allowZip64=True) as output:
            for name, data, method in entries:
                info = zipfile.ZipInfo(name, FIXED_TIME)
                info.compress_type = method
                info.external_attr = 0o100600 << 16
                output.writestr(info, data, compress_type=method, compresslevel=6)
    return bytes(target.buffer) if isinstance(target, Unseekable) else target.getvalue()


def package(document: bytes, extras: list[tuple[str, bytes, int]] | None = None,
            descriptors: bool = False) -> bytes:
    entries = [
        ("[Content_Types].xml", CONTENT_TYPES, zipfile.ZIP_STORED),
        ("_rels/.rels", ROOT_RELS, zipfile.ZIP_DEFLATED),
        ("word/document.xml", document, zipfile.ZIP_DEFLATED),
    ]
    entries.extend(extras or [])
    return archive(entries, descriptors)


def patch_u16(data: bytearray, offset: int, value: int) -> None:
    data[offset:offset + 2] = struct.pack("<H", value)


def strict_fixture() -> bytes:
    document = (
        '<?xml version="1.0" encoding="UTF-8"?>'
        '<x:document xmlns:x="http://purl.oclc.org/ooxml/wordprocessingml/main" '
        'xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006">'
        '<x:body><x:p><x:r><x:t>Strict arbitrary prefix</x:t></x:r></x:p>'
        '<mc:AlternateContent><mc:Choice Requires="x"><x:p><x:r><x:t>Choice text</x:t></x:r></x:p>'
        '</mc:Choice><mc:Fallback><x:p><x:r><x:t>Fallback text</x:t></x:r></x:p></mc:Fallback>'
        '</mc:AlternateContent></x:body></x:document>'
    ).encode()
    return package(document, [("word/unused.xml", b"stored", zipfile.ZIP_STORED)], descriptors=True)


def image_fixture() -> bytes:
    document = (
        '<?xml version="1.0" encoding="UTF-8"?>'
        '<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main" '
        'xmlns:wp="http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing">'
        '<w:body><w:p><w:r><w:t>Inline </w:t><w:drawing><wp:inline><wp:docPr id="1" '
        'name="Picture" descr="fixture image"/></wp:inline></w:drawing><w:t> after</w:t></w:r></w:p>'
        '<w:p><w:r><w:pict><wp:anchor><wp:docPr id="2" name="Anchor"/></wp:anchor></w:pict></w:r></w:p>'
        '</w:body></w:document>'
    ).encode()
    media = bytes(range(256)) * 1024
    return package(document, [("word/media/image1.png", media, zipfile.ZIP_STORED)])


def error_fixtures() -> dict[str, bytes]:
    minimal = b'<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body/></w:document>'
    valid = package(minimal)
    encrypted = bytearray(valid)
    local = encrypted.find(b"PK\x03\x04")
    central = encrypted.find(b"PK\x01\x02")
    patch_u16(encrypted, local + 6, struct.unpack_from("<H", encrypted, local + 6)[0] | 1)
    patch_u16(encrypted, central + 8, struct.unpack_from("<H", encrypted, central + 8)[0] | 1)
    deep = (b'<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">' +
            b"<w:sdt>" * 257 + b"</w:sdt>" * 257 + b"</w:document>")
    stored = bytearray(archive([
        ("[Content_Types].xml", CONTENT_TYPES, zipfile.ZIP_STORED),
        ("_rels/.rels", ROOT_RELS, zipfile.ZIP_STORED),
        ("word/document.xml", minimal, zipfile.ZIP_STORED),
    ]))
    main_local = stored.find(b"PK\x03\x04", stored.find(b"PK\x03\x04", stored.find(b"PK\x03\x04") + 1) + 1)
    name_length, extra_length = struct.unpack_from("<HH", stored, main_local + 26)
    stored[main_local + 30 + name_length + extra_length] ^= 1
    bad_zip64 = bytearray(valid)
    eocd = bad_zip64.rfind(b"PK\x05\x06")
    patch_u16(bad_zip64, eocd + 10, 0xFFFF)
    bomb = bytearray(valid)
    bomb_central = bomb.find(b"PK\x01\x02")
    bomb_compressed = struct.unpack_from("<I", bomb, bomb_central + 20)[0]
    struct.pack_into("<I", bomb, bomb_central + 24, bomb_compressed * 1000 + 1)
    return {
        "missing-content-types.docx": archive([("_rels/.rels", ROOT_RELS, zipfile.ZIP_DEFLATED)]),
        "missing-main-part.docx": archive([
            ("[Content_Types].xml", CONTENT_TYPES, zipfile.ZIP_DEFLATED),
            ("_rels/.rels", ROOT_RELS, zipfile.ZIP_DEFLATED),
        ]),
        "traversal.docx": archive([("../escape.xml", b"x", zipfile.ZIP_STORED)]),
        "duplicate-names.docx": archive([
            ("same.xml", b"a", zipfile.ZIP_STORED),
            ("same.xml", b"b", zipfile.ZIP_STORED),
        ]),
        "unsupported-compression.docx": archive([("part.xml", b"x", zipfile.ZIP_BZIP2)]),
        "encrypted-flag.docx": bytes(encrypted),
        "malformed-xml.docx": package(b'<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"><w:body></w:document>'),
        "deep-xml.docx": package(deep),
        "bad-crc.docx": bytes(stored),
        "bad-zip64-metadata.docx": bytes(bad_zip64),
        "zip-bomb-metadata.docx": bytes(bomb),
        "truncated.docx": valid[:-11],
    }


def main() -> None:
    (ROOT / "strict-prefixes.docx").write_bytes(strict_fixture())
    (ROOT / "image-placeholder.docx").write_bytes(image_fixture())
    errors = ROOT / "errors"
    errors.mkdir(exist_ok=True)
    for name, data in error_fixtures().items():
        (errors / name).write_bytes(data)


if __name__ == "__main__":
    main()
