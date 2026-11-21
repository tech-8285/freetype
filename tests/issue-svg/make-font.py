#!/usr/bin/env python3
"""Construct a synthetic font with an 'SVG ' table for the OT-SVG tests.

No font files or third-party Python packages are used: the font is built
from scratch with the `struct` module of the standard library, and the
compressed document uses `gzip`, which is part of the standard library as
well.

The font has two glyphs with SVG documents: glyph 1 points to an
uncompressed document, glyph 2 to a GZIP-compressed one.  The two
documents have a different length and content so that a mix-up between
them is detected by the test program.

This file is distributed under the FreeType project license, LICENSE.TXT.
"""
import gzip
import struct
import sys
from pathlib import Path


DOC_GLYPH1 = (
    '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1000 1000">'
    '<rect width="1000" height="1000" fill="#0f0"/></svg>'
)

DOC_GLYPH2 = (
    '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1000 1000">'
    '<circle cx="500" cy="500" r="400" fill="#00f"/></svg>'
)


def checksum(data):
    data += b"\0" * (-len(data) % 4)
    return sum(struct.unpack(">" + "I" * (len(data) // 4), data)) & 0xFFFFFFFF


def make_svg_table():
    docs = [
        (DOC_GLYPH1.encode(), 1, 1, False),
        (DOC_GLYPH2.encode(), 2, 2, True),
    ]

    records = b""
    payload = b""
    offset = 2 + 12 * len(docs)  # relative to the start of the document list
    for text, start, end, compressed in docs:
        data = text
        if compressed:
            # The SVG specification requires the gzip format; FreeType
            # recognizes it by its magic bytes.
            buf = gzip.compress(text, mtime=0)
            # The last four bytes hold the uncompressed size, little-endian.
            data = buf[:-4] + struct.pack("<I", len(text))

        records += struct.pack(">HHII", start, end, offset, len(data))
        payload += data
        offset += len(data)

    doc_list = struct.pack(">H", len(docs)) + records + payload

    # Version 0 followed by the offset of the document list from the start
    # of the table.  The list itself starts at offset 10, so four padding
    # bytes follow the six header bytes.
    return struct.pack(">HI", 0, 10) + bytes(4) + doc_list


def make_font():
    head = struct.pack(">IIIIHHQQhhhhHHhhh", 0x10000, 0x10000, 0, 0x5F0F3CF5,
                       0, 1000, 0, 0, 0, 0, 0, 0, 0, 8, 2, 1, 0)
    hhea = bytearray(36)
    struct.pack_into(">IhhhH", hhea, 0, 0x10000, 800, -200, 0, 700)
    struct.pack_into(">h", hhea, 18, 1)  # caretSlopeRise
    struct.pack_into(">H", hhea, 34, 3)  # numberOfHMetrics

    # Three empty glyphs: .notdef, 1 and 2.
    glyf = struct.pack(">5h", 0, 0, 0, 0, 0)
    loca = struct.pack(">4I", 0, 0, 0, len(glyf))
    maxp = struct.pack(">I14H", 0x10000, 3, 0, 0, 0, 0, 0, 0, 0, 0,
                       0, 0, 0, 0, 1)

    # Format 4 cmap mapping 'A' -> glyph 1 and 'B' -> glyph 2.
    cmap4 = struct.pack(">7H", 4, 32, 0, 4, 4, 1, 0)
    cmap4 += struct.pack(">5H2h2H", 66, 65535, 0, 65, 65535, -65, 1, 0, 0)

    names = [(1, "SVG Test"), (2, "Regular"), (4, "SVG Test Regular"),
             (6, "SvgTest-Regular")]
    records, strings = bytearray(), bytearray()
    for name_id, value in names:
        encoded = value.encode("utf-16-be")
        records += struct.pack(">6H", 3, 1, 0x409, name_id,
                                len(encoded), len(strings))
        strings += encoded
    name = struct.pack(">3H", 0, len(names), 6 + len(records))
    name += records + strings

    tables = {
        b"SVG ": make_svg_table(),
        b"cmap": struct.pack(">HHHHI", 0, 1, 3, 1, 12) + cmap4,
        b"glyf": glyf, b"head": head, b"hhea": bytes(hhea),
        b"hmtx": struct.pack(">HhHhHh", 700, 0, 700, 0, 700, 0),
        b"loca": loca, b"maxp": maxp, b"name": name,
        b"post": struct.pack(">I", 0x30000) + bytes(28),
    }

    count = len(tables)
    power = 1 << (count.bit_length() - 1)
    font = bytearray(struct.pack(">I4H", 0x10000, count, power * 16,
                                 power.bit_length() - 1,
                                 count * 16 - power * 16))
    font += bytes(16 * count)

    head_offset = None
    for index, (tag, data) in enumerate(sorted(tables.items())):
        offset = len(font)
        struct.pack_into(">4sIII", font, 12 + 16 * index,
                         tag, checksum(data), offset, len(data))
        font += data + bytes(-len(data) % 4)
        if tag == b"head":
            head_offset = offset

    struct.pack_into(">I", font, head_offset + 8,
                     (0xB1B0AFBA - checksum(font)) & 0xFFFFFFFF)
    assert checksum(font) == 0xB1B0AFBA

    return bytes(font)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: make-font.py output")

    Path(sys.argv[1]).write_bytes(make_font())
