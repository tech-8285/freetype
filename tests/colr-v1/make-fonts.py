#!/usr/bin/env python3
"""Generate synthetic 'COLR' v1 fixtures for the paint-parsing tests.

No external font or third-party Python package is used.  Four small TrueType
fonts are created, each containing a 'CPAL' table and a 'COLR' version 1
table.  The 'COLR' table is always the last table in the file and is written
without trailing padding, so its last byte is the last byte of the font.
This lets the C test place the font buffer directly before an inaccessible
guard page and prove that parsing never reads past the end of the table.

  colr-v1-valid.ttf
      A well-formed, deeply nested paint graph: four root layers, a
      'PaintComposite' whose source and backdrop are different subtrees, a
      transform, a gradient with two color stops, and one paint table shared
      by two layers.

  colr-v1-invalid.ttf
      One base glyph per malformation: paint and color-line offsets that
      leave the 'COLR' table, a layer index outside the layer list, an
      unsupported paint format, an invalid composite mode, and a color line
      with an overflowing stop count.

  colr-v1-oob-layer.ttf
      The layer list precedes the base glyph list, and the only layer
      offset points below the start of the paint region.

  colr-v1-cycle.ttf
      Paint graphs that revisit a paint table: a self-reference through
      'PaintColrGlyph', a two-glyph cycle, and a 'PaintColrLayers' whose
      layer points back at the same paint.

The layout follows the OpenType 'COLR' table with version 1 extensions, see
<https://learn.microsoft.com/typography/opentype/spec/colr>.

Distributed under the FreeType project license, LICENSE.TXT.
"""

import struct
import sys
from pathlib import Path


def u16(*values):
    return struct.pack(">" + "H" * len(values), *values)


def s16(*values):
    return struct.pack(">" + "h" * len(values), *values)


def u32(*values):
    return struct.pack(">" + "I" * len(values), *values)


def s32(*values):
    return struct.pack(">" + "i" * len(values), *values)


def checksum(data):
    data = bytes(data) + b"\0" * (-len(data) % 4)
    return sum(struct.unpack(">" + "I" * (len(data) // 4), data)) & 0xFFFFFFFF


# Paint formats, matching `FT_PaintFormat` and the OpenType specification.
FORMAT_COLR_LAYERS = 1
FORMAT_SOLID = 2
FORMAT_LINEAR_GRADIENT = 4
FORMAT_GLYPH = 10
FORMAT_COLR_GLYPH = 11
FORMAT_TRANSFORM = 12
FORMAT_COMPOSITE = 32

EXTEND_PAD = 0
COMPOSITE_SRC_OVER = 3

# All offsets inside a paint table are relative to the start of that paint
# table, which is why children must follow their parent in the byte stream.
PAINT_OFFSET_MASK = (1 << 24) - 1


class PaintPool:
    """Lay out paint tables and record 24-bit child offsets."""

    def __init__(self, absolute_base):
        self.buf = bytearray()
        self.absolute_base = absolute_base

    def absolute(self, position):
        return self.absolute_base + position

    def allocate(self, size):
        position = len(self.buf)
        self.buf.extend(b"\0" * size)
        return position

    def patch_child(self, position, base_position, target_absolute):
        offset = target_absolute - self.absolute(base_position)
        assert 0 <= offset <= PAINT_OFFSET_MASK, \
            "child table must follow its parent"
        self.buf[position:position + 3] = offset.to_bytes(3, "big")

    # -- paint tables -----------------------------------------------------

    def colr_layers(self, num_layers, first_layer_index):
        position = self.allocate(6)
        self.buf[position] = FORMAT_COLR_LAYERS
        self.buf[position + 1] = num_layers
        self.buf[position + 2:position + 6] = u32(first_layer_index)
        return self.absolute(position)

    def solid(self, palette_index, alpha):
        position = self.allocate(5)
        self.buf[position] = FORMAT_SOLID
        self.buf[position + 1:position + 3] = u16(palette_index)
        self.buf[position + 3:position + 5] = s16(alpha)
        return self.absolute(position)

    def glyph(self, glyph_id, make_fill):
        position = self.allocate(6)
        self.buf[position] = FORMAT_GLYPH
        self.buf[position + 4:position + 6] = u16(glyph_id)
        fill = make_fill()
        self.patch_child(position + 1, position, fill)
        return self.absolute(position)

    def colr_glyph(self, glyph_id):
        position = self.allocate(3)
        self.buf[position] = FORMAT_COLR_GLYPH
        self.buf[position + 1:position + 3] = u16(glyph_id)
        return self.absolute(position)

    def composite(self, mode, make_source, make_backdrop):
        position = self.allocate(8)
        self.buf[position] = FORMAT_COMPOSITE
        self.buf[position + 4] = mode
        source = make_source()
        backdrop = make_backdrop()
        self.patch_child(position + 1, position, source)
        self.patch_child(position + 5, position, backdrop)
        return self.absolute(position)

    def transform(self, make_paint, xx, yx, xy, yy, dx, dy):
        position = self.allocate(7)
        self.buf[position] = FORMAT_TRANSFORM
        affine = self.allocate(24)
        # On-disk order is Affine2x3: xx, yx, xy, yy, dx, dy.
        self.buf[affine:affine + 24] = s32(xx, yx, xy, yy, dx, dy)
        paint = make_paint()
        self.patch_child(position + 1, position, paint)
        self.patch_child(position + 4, position, self.absolute(affine))
        return self.absolute(position)

    def linear_gradient(self, make_color_line, p0, p1, p2):
        position = self.allocate(16)
        self.buf[position] = FORMAT_LINEAR_GRADIENT
        self.buf[position + 4:position + 16] = s16(
            p0[0], p0[1], p1[0], p1[1], p2[0], p2[1])
        color_line = make_color_line()
        self.patch_child(position + 1, position, color_line)
        return self.absolute(position)

    def color_line(self, extend, stops, stop_count=None):
        count = len(stops) if stop_count is None else stop_count
        position = self.allocate(3 + 6 * len(stops))
        self.buf[position] = extend
        self.buf[position + 1:position + 3] = u16(count)
        for index, (offset, palette_index, alpha) in enumerate(stops):
            start = position + 3 + 6 * index
            self.buf[start:start + 6] = s16(offset) + u16(palette_index) + \
                s16(alpha)
        return self.absolute(position)


def build_colr(records, layers, base_glyph_list_first=True,
               raw_record_offsets=None, raw_layer_offsets=None,
               patch_after=None):
    """Assemble a version 1 'COLR' table.

    `records` is a list of `(glyph_id, emit)`, and `layers` is a list of
    `emit` callables.  Each `emit` receives a `PaintPool` and returns the
    absolute offset of the paint table it created.  Entries listed in
    `raw_record_offsets` / `raw_layer_offsets` suppress emission and write
    the given literal offset instead.
    """
    raw_record_offsets = raw_record_offsets or {}
    raw_layer_offsets = raw_layer_offsets or {}

    base_glyph_list_size = 4 + 6 * len(records)
    layer_list_size = 4 + 4 * len(layers)

    if base_glyph_list_first:
        base_glyph_list_abs = 34
        layer_list_abs = base_glyph_list_abs + base_glyph_list_size
    else:
        layer_list_abs = 34
        base_glyph_list_abs = layer_list_abs + layer_list_size

    pool = PaintPool(34 + base_glyph_list_size + layer_list_size)

    record_paints = []
    for index, (_, emit) in enumerate(records):
        record_paints.append(
            None if index in raw_record_offsets else emit(pool))

    layer_paints = []
    for index, emit in enumerate(layers):
        layer_paints.append(
            None if index in raw_layer_offsets else emit(pool))

    base_glyph_list = bytearray(u32(len(records)))
    for index, (glyph_id, _) in enumerate(records):
        base_glyph_list += u16(glyph_id)
        if index in raw_record_offsets:
            base_glyph_list += u32(raw_record_offsets[index])
        else:
            base_glyph_list += u32(
                record_paints[index] - base_glyph_list_abs)

    layer_list = bytearray(u32(len(layers)))
    for index in range(len(layers)):
        if index in raw_layer_offsets:
            layer_list += u32(raw_layer_offsets[index])
        else:
            layer_list += u32(layer_paints[index] - layer_list_abs)

    # version, numBaseGlyphRecords, baseGlyphRecordsOffset,
    # layerRecordsOffset, numLayerRecords, then the five v1 offsets.
    header = bytearray(u16(1, 0) + u32(0, 0) + u16(0) +
                       u32(base_glyph_list_abs, layer_list_abs, 0, 0, 0))
    assert len(header) == 34

    if base_glyph_list_first:
        body = base_glyph_list + layer_list
    else:
        body = layer_list + base_glyph_list

    table = header + body + pool.buf

    if patch_after:
        patch_after(table, base_glyph_list_abs, layer_list_abs)

    return bytes(table)


def make_colr_valid():
    """Four root layers with a composite, a transform, and a shared paint."""
    shared = []

    def layer0(p):
        return p.glyph(5, lambda: p.solid(3, 0x4000))

    def layer1(p):
        def source():
            def child():
                glyph = p.glyph(6, lambda: p.solid(4, 0x2000))
                shared.append(glyph)
                return glyph

            return p.transform(child,
                               0x10000, 0x0800, -0x0400, 0x10000,
                               0x8000, -0x4000)

        return p.composite(COMPOSITE_SRC_OVER,
                           source,
                           lambda: p.glyph(7, lambda: p.solid(5, 0x1000)))

    def layer2(p):
        return p.linear_gradient(
            lambda: p.color_line(EXTEND_PAD,
                                 [(0x0000, 1, 0x4000),
                                  (0x4000, 2, 0x2000)]),
            (10, 20), (30, 40), (50, 60))

    def root(p):
        return p.colr_layers(4, 0)

    # Layer 3 reuses the exact paint table emitted below the transform that
    # the composite in layer 1 installs.
    def layer3(p):
        assert shared, "layer 1 must be emitted before layer 3"
        return shared[0]

    return build_colr([(1, root)], [layer0, layer1, layer2, layer3])


def make_colr_invalid():
    """One base glyph per malformed paint reference."""

    def truncated_record(p):
        # The record offset is patched to point at the last table byte.
        return p.solid(0, 0x4000)

    def interior_record(p):
        # The record offset is patched to point into the base glyph list.
        return p.solid(0, 0x4000)

    def bad_layer_offset(p):
        # The layer list's only entry is patched past the table end.
        return p.colr_layers(1, 0)

    def bad_child_offset(p):
        position = p.allocate(6)
        p.buf[position] = FORMAT_GLYPH
        p.buf[position + 1:position + 4] = (0xFFFFF).to_bytes(3, "big")
        p.buf[position + 4:position + 6] = u16(5)
        return p.absolute(position)

    def bad_color_line_offset(p):
        position = p.allocate(16)
        p.buf[position] = FORMAT_LINEAR_GRADIENT
        p.buf[position + 1:position + 4] = (0xFFFFF).to_bytes(3, "big")
        return p.absolute(position)

    def bad_backdrop_offset(p):
        position = p.allocate(8)
        p.buf[position] = FORMAT_COMPOSITE
        p.buf[position + 4] = COMPOSITE_SRC_OVER
        p.buf[position + 5:position + 8] = (0xFFFFF).to_bytes(3, "big")
        source = p.solid(0, 0x4000)
        p.patch_child(position + 1, position, source)
        return p.absolute(position)

    def zero_child_offset(p):
        # A 'PaintGlyph' whose child offset is zero.  Taken literally it
        # would make the paint point at itself, so the offset must be
        # rejected rather than followed.
        position = p.allocate(6)
        p.buf[position] = FORMAT_GLYPH
        p.buf[position + 1:position + 4] = (0).to_bytes(3, "big")
        p.buf[position + 4:position + 6] = u16(5)
        return p.absolute(position)

    def layer_range_overflow(p):
        # One layer is requested (which the layer list does contain), but
        # the index is the list's size, so the range ends past its end.
        return p.colr_layers(1, 1)

    def stop_count_overflow(p):
        return p.linear_gradient(
            lambda: p.color_line(EXTEND_PAD, [], stop_count=0xFFFF),
            (0, 0), (1, 1), (2, 2))

    def unsupported_format(p):
        position = p.allocate(2)
        p.buf[position] = 0x9F
        return p.absolute(position)

    def invalid_composite_mode(p):
        position = p.allocate(8)
        p.buf[position] = FORMAT_COMPOSITE
        p.buf[position + 4] = 28  # FT_COLR_COMPOSITE_MAX
        p.buf[position + 5:position + 8] = (0x000001).to_bytes(3, "big")
        source = p.solid(0, 0x4000)
        p.patch_child(position + 1, position, source)
        return p.absolute(position)

    def invalid_extend_mode(p):
        return p.linear_gradient(
            lambda: p.color_line(3, []),
            (0, 0), (1, 1), (2, 2))

    records = [
        (1, truncated_record),        # offset == end of table
        (2, interior_record),         # offset inside the base glyph list
        (3, bad_layer_offset),        # layer offset past the table
        (4, bad_child_offset),        # child paint offset past the table
        (5, bad_color_line_offset),   # color line offset past the table
        (6, bad_backdrop_offset),     # backdrop paint offset past the table
        (7, layer_range_overflow),    # layer index range exceeds the list
        (8, stop_count_overflow),     # color stop count overflows
        (9, unsupported_format),      # unknown paint format
        (10, invalid_composite_mode),  # composite mode out of range
        (11, invalid_extend_mode),    # extend mode out of range
        (12, zero_child_offset),      # child offset of zero
    ]

    def patch_records(table, base_glyph_list_abs, layer_list_abs):
        def set_offset(index, value):
            position = base_glyph_list_abs + 4 + 6 * index + 2
            table[position:position + 4] = u32(value)

        set_offset(0, len(table) - base_glyph_list_abs)  # first byte past end
        set_offset(1, 4)                                 # inside the list

    return build_colr(
        records,
        [lambda p: p.solid(0, 0x4000)],
        raw_layer_offsets={0: 0x100000},
        patch_after=patch_records)


def make_colr_oob_layer():
    """The layer list precedes the base glyph list; its entry points below
    the paint region."""
    def root(p):
        return p.colr_layers(1, 0)

    return build_colr(
        [(1, root)],
        [lambda p: p.solid(0, 0x4000)],
        base_glyph_list_first=False,
        raw_layer_offsets={0: 0})


def make_colr_cycle():
    """Paint graphs that revisit a paint table."""
    self_cycle = []

    def self_reference(p):
        return p.colr_glyph(1)  # glyph 1's own root paint

    def forward(p):
        return p.colr_glyph(3)  # glyph 3 points back at glyph 2

    def backward(p):
        return p.colr_glyph(2)

    def root(p):
        paint = p.colr_layers(1, 0)
        self_cycle.append(paint)
        return paint

    def patch_layers(table, base_glyph_list_abs, layer_list_abs):
        # The only layer points back at glyph 4's root, i.e. the very
        # 'PaintColrLayers' table that owns it.
        table[layer_list_abs + 4:layer_list_abs + 8] = u32(
            self_cycle[0] - layer_list_abs)

    return build_colr(
        [(1, self_reference), (2, forward), (3, backward), (4, root)],
        [lambda p: p.solid(0, 0x4000)],
        patch_after=patch_layers)


def make_colr_truncated():
    """A 'PaintSolid' truncated to four bytes at the very end of the table.

    The format byte is followed by only three bytes, so reading the paint
    fields would run one byte past the end of the 'COLR' table.
    """
    header = u16(1, 0) + u32(0, 0) + u16(0) + u32(34, 0, 0, 0, 0)
    base_glyph_list = u32(1) + u16(1) + u32(48 - 34)

    table = bytearray(header + base_glyph_list)   # 44 bytes
    table += b"\0" * (48 - len(table))
    table += bytes([FORMAT_SOLID, 0x00, 0x01, 0x00])  # 4 of the 5 needed
    assert len(table) == 52
    return bytes(table)


def make_font(colr):
    """Wrap the 'COLR' table in a minimal TrueType font.

    The 'COLR' table is emitted last and without padding so that the table's
    end coincides with the end of the font.
    """
    num_glyphs = 16

    # `indexToLocFormat` is 0, i.e. `loca` holds 16-bit offsets.
    head = struct.pack(">IIIIHHQQhhhhHHhhh",
                       0x00010000, 0x00010000, 0, 0x5F0F3CF5,
                       0, 1000, 0, 0, 0, 0, 0, 0, 0, 8, 2, 0, 0)
    hhea = bytearray(36)
    struct.pack_into(">IhhhH", hhea, 0, 0x00010000, 800, -200, 0, 500)
    struct.pack_into(">h", hhea, 18, 1)               # caretSlopeRise
    struct.pack_into(">H", hhea, 34, num_glyphs)      # numberOfHMetrics
    maxp = struct.pack(">I14H", 0x00010000, num_glyphs, 0, 0, 0, 0, 1, 0, 0,
                       0, 0, 0, 0, 0, 0)
    glyf = b"\0\0\0\0"
    loca = u16(*([0] * (num_glyphs + 1)))
    hmtx = u16(*([500, 0] * num_glyphs))
    cmap4 = struct.pack(">7H", 4, 32, 0, 4, 4, 1, 0)
    cmap4 += struct.pack(">5H2h2H", 65, 65535, 0, 65, 65535, -64, 1, 0, 0)
    cmap = struct.pack(">HHHHI", 0, 1, 3, 1, 12) + cmap4
    name = struct.pack(">3H", 0, 0, 6)
    post = struct.pack(">I", 0x00030000) + bytes(28)

    palette_size = 8
    colors = bytes([0x20, 0x30, 0x40, 0xFF] * palette_size)
    cpal = u16(0, palette_size, 1, palette_size) + u32(12) + u16(0) + colors

    tables = {
        b"COLR": colr,
        b"CPAL": cpal,
        b"cmap": cmap,
        b"glyf": glyf,
        b"head": head,
        b"hhea": bytes(hhea),
        b"hmtx": hmtx,
        b"loca": loca,
        b"maxp": maxp,
        b"name": name,
        b"post": post,
    }

    count = len(tables)
    power = 1 << (count.bit_length() - 1)
    font = bytearray(struct.pack(">I4H", 0x00010000, count, power * 16,
                                 power.bit_length() - 1,
                                 count * 16 - power * 16))
    font += bytes(16 * count)

    # Keep 'COLR' out of the sorted emission order so that it lands last.
    entries = sorted(tables.items())
    entries = [(tag, data) for tag, data in entries if tag != b"COLR"]
    entries.append((b"COLR", colr))

    head_offset = None
    for index, (tag, data) in enumerate(entries):
        offset = len(font)
        struct.pack_into(">4sIII", font, 12 + 16 * index,
                         tag, checksum(data), offset, len(data))
        font += data
        if tag != b"COLR":
            font += bytes(-len(data) % 4)
        if tag == b"head":
            head_offset = offset

    assert font[-len(colr):] == colr, "'COLR' must be the last table"
    struct.pack_into(">I", font, head_offset + 8,
                     (0xB1B0AFBA - checksum(font)) & 0xFFFFFFFF)
    assert checksum(font) == 0xB1B0AFBA
    return bytes(font)


OUTPUTS = [
    ("colr-v1-valid.ttf", make_colr_valid),
    ("colr-v1-invalid.ttf", make_colr_invalid),
    ("colr-v1-oob-layer.ttf", make_colr_oob_layer),
    ("colr-v1-cycle.ttf", make_colr_cycle),
    ("colr-v1-truncated.ttf", make_colr_truncated),
]


if __name__ == "__main__":
    if len(sys.argv) != 6:
        raise SystemExit("usage: make-fonts.py valid.ttf invalid.ttf "
                         "oob-layer.ttf cycle.ttf truncated.ttf")
    for (name, make), output in zip(OUTPUTS, sys.argv[1:]):
        data = make_font(make())
        Path(output).write_bytes(data)
        print(f"{name} {len(data)} {output}")
