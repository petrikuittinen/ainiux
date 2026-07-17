#!/usr/bin/env python3
"""Generate compact editor word-property tables from the Unicode UCD.

Usage:
    generate_editor_unicode_data.py UnicodeData.txt CaseFolding.txt > unicode_word_data.hpp
"""

from __future__ import annotations

import sys


def merge_ranges(values: list[int]) -> list[tuple[int, int]]:
    if not values:
        return []
    ranges: list[tuple[int, int]] = []
    first = previous = values[0]
    for value in values[1:]:
        if value == previous + 1:
            previous = value
            continue
        ranges.append((first, previous))
        first = previous = value
    ranges.append((first, previous))
    return ranges


def read_categories(path: str) -> tuple[list[tuple[int, int]], list[tuple[int, int]]]:
    words: list[int] = []
    uppercase: list[int] = []
    pending: tuple[int, str] | None = None
    with open(path, encoding="utf-8") as source:
        for line in source:
            fields = line.rstrip("\n").split(";")
            codepoint = int(fields[0], 16)
            name = fields[1]
            category = fields[2]
            if name.endswith(", First>"):
                pending = (codepoint, category)
                continue
            if name.endswith(", Last>"):
                if pending is None or pending[1] != category:
                    raise ValueError("malformed UnicodeData First/Last range")
                values = range(pending[0], codepoint + 1)
                pending = None
            else:
                values = (codepoint,)
            for value in values:
                if category[0] in "LNM":
                    words.append(value)
                if category in ("Lu", "Lt"):
                    uppercase.append(value)
    if pending is not None:
        raise ValueError("unterminated UnicodeData range")
    return merge_ranges(words), merge_ranges(uppercase)


def read_casefold(path: str) -> list[tuple[int, tuple[int, ...]]]:
    mappings: dict[int, tuple[int, ...]] = {}
    with open(path, encoding="utf-8") as source:
        for raw in source:
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            fields = [field.strip() for field in line.split(";")]
            codepoint = int(fields[0], 16)
            status = fields[1]
            if status not in ("C", "F"):
                continue
            mapping = tuple(int(value, 16) for value in fields[2].split())
            if len(mapping) > 3:
                raise ValueError(f"case fold for U+{codepoint:04X} exceeds table width")
            mappings[codepoint] = mapping
    return sorted(mappings.items())


def emit_ranges(name: str, ranges: list[tuple[int, int]]) -> None:
    print(f"inline constexpr UnicodeRange {name}[] = {{")
    for first, last in ranges:
        print(f"    {{0x{first:X}U, 0x{last:X}U}},")
    print("};")


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    word_ranges, uppercase_ranges = read_categories(sys.argv[1])
    folds = read_casefold(sys.argv[2])
    print("#pragma once")
    print()
    print("// Generated from Unicode 15.1.0 UnicodeData.txt and CaseFolding.txt.")
    print("// Source: https://www.unicode.org/Public/15.1.0/ucd/")
    print("// Unicode data license: docs/unicode-license.txt")
    print("// Regenerate with tools/generate_editor_unicode_data.py; do not edit by hand.")
    print("#include <cstdint>")
    print()
    print("namespace ainiux::editor::unicode_data {")
    print("struct UnicodeRange { std::uint32_t first; std::uint32_t last; };")
    print("struct FoldMapping { std::uint32_t source; std::uint32_t values[3]; std::uint8_t length; };")
    emit_ranges("kWordRanges", word_ranges)
    emit_ranges("kUppercaseRanges", uppercase_ranges)
    print("inline constexpr FoldMapping kCaseFolds[] = {")
    for codepoint, mapping in folds:
        padded = mapping + (0,) * (3 - len(mapping))
        print(
            f"    {{0x{codepoint:X}U, {{0x{padded[0]:X}U, 0x{padded[1]:X}U, "
            f"0x{padded[2]:X}U}}, {len(mapping)}}},"
        )
    print("};")
    print("}  // namespace ainiux::editor::unicode_data")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
