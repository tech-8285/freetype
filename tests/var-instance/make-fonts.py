#!/usr/bin/env python3
"""Construct a two-axis variable font for the instance-semantics regression.

The font has a `wght` and a `wdth` axis, named instances covering both
default and non-default coordinates, plus `gvar` and `HVAR` tables so that
glyph outlines and horizontal advances really vary with the instance.

The fixture is consumed by `main.c`, which checks that the design
coordinates reported through the Multiple Masters API, the variation flags,
and the loaded glyph advances always describe the same instance when
switching between named instances, design coordinates, and blend
coordinates.

This file is distributed under the FreeType project license, LICENSE.TXT.
"""
import sys

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.varLib import build
from fontTools.designspaceLib import (DesignSpaceDocument, AxisDescriptor,
                                      SourceDescriptor, InstanceDescriptor)

UPM = 1000

# (wght, wdth, advance, lsb, width, height)
MASTERS = [
    (100,  75, 450, 10,  60, 600),
    (100, 100, 500, 10,  80, 600),
    (400,  75, 540, 20,  90, 700),
    (400, 100, 600, 20, 120, 700),
    (900,  75, 810, 40, 150, 800),
    (900, 100, 900, 40, 200, 800),
]


def make_glyph(advance, lsb, width, height):
    pen = TTGlyphPen(None)
    x0, x1 = lsb, lsb + width
    pen.moveTo((x0, 0))
    pen.lineTo((x0, height))
    pen.lineTo((x1, height))
    pen.lineTo((x1, 0))
    pen.closePath()
    return pen.glyph()


def build_master(path, master):
    _, _, advance, lsb, width, height = master
    fb = FontBuilder(UPM, isTTF=True)
    fb.setupGlyphOrder([".notdef", "A"])
    fb.setupCharacterMap({0x41: "A"})
    fb.setupGlyf({
        ".notdef": make_glyph(300, 0, 100, 500),
        "A": make_glyph(advance, lsb, width, height),
    })
    fb.setupHorizontalMetrics({
        ".notdef": (300, 0),
        "A": (advance, lsb),
    })
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    fb.setupNameTable({"familyName": "VarInstanceTest",
                       "styleName": "Regular"})
    fb.setupOS2()
    fb.setupPost()
    fb.setupMaxp()
    fb.font.save(path)


def main(output):
    paths = []
    for index, master in enumerate(MASTERS):
        path = f"{output}.master{index}.ttf"
        build_master(path, master)
        paths.append(path)

    doc = DesignSpaceDocument()
    for tag, name, minimum, default, maximum in [
        ("wght", "Weight", 100, 400, 900),
        ("wdth", "Width", 75, 100, 125),
    ]:
        axis = AxisDescriptor()
        axis.name = name
        axis.tag = tag
        axis.minimum = minimum
        axis.default = default
        axis.maximum = maximum
        doc.addAxis(axis)

    for path, master in zip(paths, MASTERS):
        source = SourceDescriptor()
        source.path = path
        source.name = f"{master[0]}-{master[1]}"
        source.location = {"Weight": master[0], "Width": master[1]}
        source.copyInfo = (master[0] == 400 and master[1] == 100)
        source.copyGroups = True
        source.copyLib = True
        doc.addSource(source)

    for wght, wdth, style in [
        (100, 100, "Thin"),
        (400, 100, "Regular"),
        (900, 100, "Black"),
        (400,  75, "Condensed"),
        (900,  75, "BlackCondensed"),
    ]:
        instance = InstanceDescriptor()
        instance.location = {"Weight": wght, "Width": wdth}
        instance.familyName = "VarInstanceTest"
        instance.styleName = style
        doc.addInstance(instance)

    designspace = f"{output}.designspace"
    doc.write(designspace)

    font, _, _ = build(designspace)
    font.save(output)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: make-fonts.py output.ttf")
    main(sys.argv[1])
